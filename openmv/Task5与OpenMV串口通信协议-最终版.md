# Task5 STM32 ↔ OpenMV 串口通信协议（最终实现版）

本文档描述本工程已经实现的 Task5 通信行为。OpenMV 端应以本文档为
联调依据，不要假设一次 UART 读取恰好得到一帧，也不要直接用内存结构体
映射字节流。

## 1. 硬件与串口参数

### 1.1 UART5 接线

| STM32F407 引脚 | 功能 | 连接 OpenMV |
|---|---|---|
| `PC12` | `UART5_TX` | OpenMV 的 UART RX |
| `PD2` | `UART5_RX` | OpenMV 的 UART TX |
| `GND` | 信号地 | OpenMV GND |

双方均使用 3.3 V TTL 电平，必须共地。TX 与 RX 交叉连接，不能把
STM32 TX 接到 OpenMV TX。

| 参数 | 值 |
|---|---|
| 波特率 | 115200 bit/s |
| 数据位 | 8 bit |
| 停止位 | 1 bit |
| 校验位 | 无 |
| 流控 | 无 |
| 字节序 | 多字节整数均为小端序 |

USART6 已被 TJC 串口屏占用；OpenMV 仍只使用 UART5。

### 1.2 锯齿波输出

| 项目 | 实现 |
|---|---|
| 输出引脚 | `PA4 / DAC_OUT1` |
| 波形 | 单调递增锯齿波，随后直接回零 |
| DAC 精度 | 12 bit，码值 0～4095 |
| 每周期采样点 | 100 点 |
| 1 kHz 更新率 | 100 ksample/s |
| 10 kHz 更新率 | 1 Msample/s |
| 触发源 | TIM6 TRGO |
| 数据搬运 | DMA1 Stream5，循环模式 |

第 `i` 个点的 DAC 码值为：

```text
code[i] = floor(i * 4095 / 99), i = 0..99
```

因此一个周期内没有下降台阶，只有周期边界从 4095 回到 0。

## 2. 实体按键的 Task5 选择规则

三个按键的身份决定锁相模式，同一个按键的按压时长决定锯齿波频率：

| 按键 | 模式 | 协议 `lock_mode` |
|---|---|---:|
| PA0 | 对角直线，同频 0°模式 | `0x00` |
| PB9 | 圆，同频正交模式 | `0x01` |
| PB8 | “∞”，二倍频 0°模式 | `0x02` |

| 同一按键的操作 | 锯齿波频率 |
|---|---:|
| 短按，按住时间小于 400 ms | 1000 Hz |
| 长按，按住时间大于等于 400 ms | 10000 Hz |

具体判定规则：

1. 按下事件与释放确认的去抖时间均为 50 ms。
2. 第一次有效按压后，在主循环中记录按下时刻并持续读取对应 GPIO。
3. 按键持续按住达到 400 ms 时，立即确认为长按并按 10 kHz 启动。
4. 按键在 400 ms 前释放且释放电平稳定 50 ms，确认为短按并按
   1 kHz 启动。
5. 一个按键正在进行时长判定时，其他按键事件被忽略。
6. 一个 Task5 Session 已经启动后仍可再次选择；STM32 先终止旧
   Session，再用新的 Session ID 启动所选模式。
7. 三个 EXTI 在 Task5 全流程持续有效，仅在 Task1–4 状态下关闭；非
   Task5 状态不会使用这些事件启动 Session。
8. 按压时长和 DAC 锯齿波档位只在 MCU 内部使用。Task5 串口屏的运行、
   锁相和错误画面不得显示 1000 Hz、10000 Hz 或任何其他 DAC 锯齿波
   频率信息。

## 3. 统一数据帧

```text
AA 55 | VER | TYPE | SEQ | LEN_L LEN_H | PAYLOAD | CRC_L CRC_H
```

| 字段 | 长度 | 说明 |
|---|---:|---|
| 帧头 | 2 | 固定 `0xAA 0x55` |
| `VER` | 1 | 当前固定 `0x01` |
| `TYPE` | 1 | 消息类型 |
| `SEQ` | 1 | 发送方的 8 bit 序列号 |
| `LEN` | 2 | Payload 长度，小端序 |
| `PAYLOAD` | 0～64 | 消息数据 |
| `CRC16` | 2 | CRC16/MODBUS，小端序 |

完整帧长度为：

```text
9 + LEN 字节
```

最长合法帧为 73 字节。

CRC 计算范围为：

```text
VER, TYPE, SEQ, LEN_L, LEN_H, PAYLOAD
```

CRC 不包含 `AA 55`，初值为 `0xFFFF`，多项式的反射形式为
`0xA001`，结果按低字节在前发送。

MicroPython 参考实现：

```python
def crc16_modbus(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF
```

## 4. SEQ、Session ID 与 Test ID

### 4.1 SEQ

- STM32 与 OpenMV 各自维护自己的发送 SEQ。
- 每发送一个新帧，自己的 SEQ 加 1，`255` 后回到 `0`。
- ACK/NACK 也是新帧，也消耗发送方自己的 SEQ。
- 发生重发时必须重发完全相同的帧，尤其必须保留原 TYPE、SEQ、
  Session ID 和 Test ID。
- 接收方不能要求对方的 SEQ 连续；只能用 TYPE+SEQ 识别确认和重复包。
- ACK/NACK 本身不再要求 ACK。

### 4.2 Session ID

`session_id` 为 `uint16`。STM32 每次按键选择启动一个新 Task5 任务时
加 1，跳过 0。OpenMV 返回的 Session ID 必须与当前 Session 一致。

旧 Session 的结果不能影响当前任务。STM32 收到旧 Session 结果时发送：

```text
NACK error_code = 0x07
```

### 4.3 Test ID

`test_id` 为 `uint16`。同一个 Session 中每发送一个新的 DDS 测试频率
加 1，跳过 0。同一 DDS_TEST 因超时而重发时，Test ID 和 SEQ 均保持不变。

## 5. 消息类型

| TYPE | 名称 | 方向 | 本版状态 |
|---:|---|---|---|
| `0x01` | ACK | 双向 | 必须 |
| `0x02` | NACK | 双向 | 必须 |
| `0x03` | HEARTBEAT | 双向 | 可选；STM32 收到后 ACK |
| `0x10` | START_TASK | STM32 → OpenMV | 必须 |
| `0x11` | COARSE_RESULT | OpenMV → STM32 | 必须 |
| `0x20` | DDS_TEST | STM32 → OpenMV | 必须 |
| `0x21` | DDS_TEST_RESULT | OpenMV → STM32 | 必须 |
| `0x22` | STOP_TASK | STM32 → OpenMV | 必须 |
| `0x7F` | OPENMV_ERROR | OpenMV → STM32 | 可选 |

`0x30` 之后的相位消息未启用。频率找到后，相位锁定由 STM32 本地
ADC+AD9910 环路完成，OpenMV 回到空闲态。

## 6. ACK

```text
TYPE = 0x01
LEN  = 3
```

| 偏移 | 类型 | 字段 | 含义 |
|---:|---|---|---|
| 0 | uint8 | `ack_type` | 被确认帧的 TYPE |
| 1 | uint8 | `ack_seq` | 被确认帧的 SEQ |
| 2 | uint8 | `status` | 接收状态 |

| status | 含义 |
|---:|---|
| `0x00` | 首次接收并接受 |
| `0x01` | 重复包，已处理 |
| `0x02` | 命令已接受，异步结果尚未完成 |

STM32 接受以上三个状态。OpenMV 初版正常使用 `0x00`，处理重复命令时
使用 `0x01` 即可。

## 7. NACK

```text
TYPE = 0x02
LEN  = 3
```

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint8 | `error_type` |
| 1 | uint8 | `error_seq` |
| 2 | uint8 | `error_code` |

| error_code | 含义 |
|---:|---|
| `0x01` | Payload 长度错误 |
| `0x02` | 协议版本不支持 |
| `0x03` | lock_mode 非法 |
| `0x04` | 锯齿波频率非法 |
| `0x05` | 接收方正忙 |
| `0x06` | 当前状态不允许该消息，或 Test ID 不是当前测试 |
| `0x07` | Session ID 不匹配 |
| `0x08` | 参数/结果超出范围 |
| `0x09` | 图像传感器错误 |
| `0x0A` | 内部程序错误 |

CRC 错误和 `LEN > 64` 时直接丢弃，不返回 NACK，因为此时 TYPE/SEQ
也不可信。

STM32 收到针对当前可靠命令的 `BUSY(0x05)` 后，延迟 200 ms，再用原
SEQ 重发。收到其他 NACK 时终止 Task5、关闭 DAC/DDS、回到安全输出。

## 8. START_TASK

```text
TYPE = 0x10
LEN  = 7
```

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | uint16 | `session_id` | 当前任务号 |
| 2 | uint8 | `lock_mode` | 0/1/2 |
| 3 | uint32 | `saw_freq_hz` | 只允许 1000 或 10000 |

OpenMV 收到后必须依次检查：

1. `VER == 1`；
2. `LEN == 7`；
3. `lock_mode` 为 0、1 或 2；
4. `saw_freq_hz` 为 1000 或 10000；
5. 当前为空闲，或该帧是当前 START_TASK 的重复包。

检查通过后立即 ACK，再开始耗时的图像识别。不要等识别结束后才 ACK。

## 9. COARSE_RESULT

```text
TYPE = 0x11
LEN  = 10
```

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | uint16 | `session_id` | 必须等于当前 Session |
| 2 | uint8 | `result` | 粗识别结果 |
| 3 | uint32 | `ratio_x1000` | 输入频率/锯齿波频率×1000 |
| 7 | uint8 | `confidence` | 0～100 |
| 8 | uint8 | `left_peak_count` | 调试计数 |
| 9 | uint8 | `right_peak_count` | 调试计数 |

当 `START_TASK.saw_freq_hz == 10000` 时，`ratio_x1000` 已包含 OpenMV
新模型的实测固定偏差补偿：原始估计 20～50 kHz 加 1 kHz，原始估计
不低于 51 kHz 加 2 kHz，然后限制到 10～100 kHz。补偿在多帧共识之前
完成，STM32 必须直接使用收到的 `ratio_x1000`，不得再次添加 1 kHz 或
2 kHz。`saw_freq_hz == 1000` 时不执行该补偿。该变化不改变帧格式、字段
长度、CRC 或 ACK 行为。

| result | 含义 |
|---:|---|
| `0x00` | 成功 |
| `0x01` | 成功但置信度较低 |
| `0x02` | 无有效曲线 |
| `0x03` | 左右结果差异过大 |
| `0x04` | 拖尾干扰严重 |
| `0x05` | 过曝/欠曝 |
| `0x06` | 识别超时 |
| `0x07` | 任务取消 |

当前 OpenMV 粗识别只发送 `0x00` 或 `0x01`。它不会因为识别耗时、临时
模型载入失败或单帧相机错误发送 `0x02/ratio=0`；稳定结果形成前持续采集，
30 秒后有有效候选时返回最佳候选并标记 `0x01`。若完全没有检测到轨迹，
则继续等待而不是编造倍数。

为兼容旧 OpenMV 程序，STM32 收到 `0x02`～`0x07` 或零倍数时仍会 ACK，
随后发送旧 Session 的 `STOP_TASK(reason=0x04)` 并以新 Session 自动重启
相同模式的粗识别，不进入 `TASK5_ERROR_BAD_RESULT`。越界倍数返回 NACK
后同样自动重启。重启期间保持本次选择的 DAC 锯齿波输出。

STM32 计算：

```text
estimated_input_hz =
    round(saw_freq_hz * ratio_x1000 / 1000)

mode 0/1:
    initial_dds_hz = estimated_input_hz

mode 2:
    initial_dds_hz = 2 * estimated_input_hz
```

本工程有效输入频率范围为 1～100 kHz，因此
`estimated_input_hz > 100000` 会返回 `NACK 0x08`；mode 2 的 DDS
上限相应为 200 kHz。

收到合法 COARSE_RESULT 后，STM32 立即 ACK、停止 DAC 锯齿波、切换
继电器到 DDS 路径，并设置第一个 DDS 频率。

## 10. DDS_TEST

```text
TYPE = 0x20
LEN  = 11
```

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | uint16 | `session_id` | 当前 Session |
| 2 | uint16 | `test_id` | 当前测试号 |
| 4 | uint32 | `dds_freq_hz` | STM32 已设置的 DDS 频率 |
| 8 | uint8 | `search_stage` | 搜索阶段 |
| 9 | uint16 | `capture_delay_ms` | 当前固定为 200 ms |

| search_stage | 含义 |
|---:|---|
| `0x00` | 粗搜索/尚未形成上下界 |
| `0x01` | 中等步长 |
| `0x02` | 精细搜索 |
| `0x03` | 最终确认，当前实现暂不主动发送 |
| `0x04` | 预留相位阶段 |

STM32 在修改 DDS 后先本地等待 200 ms，再发送 DDS_TEST。OpenMV 收到
合法命令后立即 ACK，然后至少再等待 `capture_delay_ms`，再采集和判断。

## 11. DDS_TEST_RESULT

```text
TYPE = 0x21
LEN  = 8
```

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | uint16 | `session_id` | 当前 Session |
| 2 | uint16 | `test_id` | 必须等于当前测试 |
| 4 | uint8 | `result` | 频率判断 |
| 5 | uint16 | `match_score` | 0～1000 |
| 7 | uint8 | `confidence` | 0～100 |

| result | OpenMV 含义 | STM32 行为 |
|---:|---|---|
| `0x00` | TARGET_REACHED | 结束相机搜索，进入本地相位锁定 |
| `0x01` | DDS_TOO_LOW | 提高 DDS 频率 |
| `0x02` | DDS_TOO_HIGH | 降低 DDS 频率 |
| `0x03` | NOT_MATCHED，方向未知 | 围绕粗估值正负交替扩大搜索 |
| `0x04` | UNSTABLE | 原频率重新测试，最多额外 2 次 |
| `0x05` | LOW_CONFIDENCE | 原频率重新测试，最多额外 2 次 |
| `0x06` | IMAGE_ERROR | 原频率重新测试，最多额外 2 次 |
| `0x07` | TIMEOUT | 原频率重新测试，最多额外 2 次 |

`DDS_TOO_LOW/HIGH` 的比较对象是当前模式所要求的目标 DDS 频率：

- mode 0/1 的目标是输入信号的 1 倍；
- mode 2 的目标是输入信号的 2 倍。

如果 OpenMV 能可靠判断方向，应优先返回 `0x01/0x02`，STM32 会先扩展
直到找到上下界，再取中点收敛。如果只能判断“相符/不相符”，可以只返回
`0x00/0x03`，但搜索次数和时间会更长。单个 Session 最多发送 40 个新的
DDS_TEST；超过即安全终止。

STM32 对每个合法 DDS_TEST_RESULT 立即 ACK。OpenMV 在收到这个 ACK
之前必须保留结果帧，以便超时后原样重发。

## 12. STOP_TASK

```text
TYPE = 0x22
LEN  = 3
```

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `session_id` |
| 2 | uint8 | `reason` |

| reason | 含义 |
|---:|---|
| `0x00` | 正常找到目标 |
| `0x01` | 用户退出/取消 |
| `0x02` | 搜索超时 |
| `0x03` | 搜索次数超限 |
| `0x04` | 重新开始 |
| `0x05` | STM32 系统或通信错误 |

OpenMV 收到后应停止当前识别、清除本 Session 的运行态、返回 ACK，
进入 IDLE。正常完成时，STM32 等到 STOP_TASK 的 ACK 后才启动本地相位
锁定。

Task5 运行中重新选择模式时，STM32 会依次发送旧 Session 的
`STOP_TASK(reason=0x04)` 和新 Session 的 `START_TASK`，但不等待旧
STOP 的 ACK。OpenMV 必须按接收顺序先清理旧 Session、回到 IDLE，再
接受新的 Session；旧 Session 的迟到结果不得影响新 Session。

## 13. 正常时序

```text
STM32                                      OpenMV
  |                                          |
  |-- START_TASK(session,mode,saw) ---------->|
  |<---------------- ACK ---------------------|
  |                                          | 粗识别
  |<-- COARSE_RESULT(session,ratio) ---------|
  |---------------- ACK --------------------->|
  | 停 DAC 锯齿波，设置 DDS，等待 200 ms      |
  |-- DDS_TEST(session,test,freq,delay) ------>|
  |<---------------- ACK ---------------------|
  |                                          | 等待 delay，识别
  |<-- DDS_TEST_RESULT(session,test,result) --|
  |---------------- ACK --------------------->|
  |                                          |
  |  根据结果调整 DDS，重复 DDS_TEST           |
  |                                          |
  |<-- DDS_TEST_RESULT(TARGET_REACHED) -------|
  |---------------- ACK --------------------->|
  |-- STOP_TASK(reason=normal) -------------->|
  |<---------------- ACK ---------------------|
  |                                          | 回到 IDLE
  | 启动 STM32 本地 ADC/AD9910 相位锁定         |
```

## 14. 超时与重发

| 项目 | STM32 实现值 |
|---|---:|
| 命令 ACK 超时 | 100 ms |
| ACK 最大重发次数 | 3 次；总发送次数最多 4 次 |
| COARSE_RESULT 等待 | 无失败硬超时；每 5 s 重放同一 START_TASK |
| DDS_TEST_RESULT 超时 | 2 s |
| COARSE_RESULT 等待重放 | 不限次数，用户可按模式键取消/重选 |
| DDS_TEST_RESULT 等待最大重放次数 | 3 次 |
| DDS 本地稳定等待 | 200 ms |
| OpenMV 拍摄延迟字段 | 200 ms |

等待粗识别结果期间，STM32 每 5 秒重放原 START_TASK 且不设重放上限；
等待 DDS 结果超时时则按上表的有限次数重放原 DDS_TEST。两者都保持
TYPE、SEQ、Session ID、Test ID 和 Payload 不变。

OpenMV 对重复命令的处理：

- 仍在识别：重发 ACK，不得重新启动第二次识别；
- 结果已经生成：重发 ACK，并重发缓存的原结果帧；
- 结果帧的 TYPE、SEQ、Session ID、Test ID 和 Payload 均保持不变。

## 15. OpenMV 必须缓存的状态

至少保存：

```text
state
current_session_id
last_rx_type
last_rx_seq
last_test_id
last_result_frame
last_result_waiting_ack
tx_seq
```

建议状态机：

```text
IDLE
  └─ START_TASK -> COARSE_RECOGNIZING
                       └─ result sent -> WAIT_DDS_TEST
                                             └─ DDS_TEST -> DDS_RECOGNIZING
                                                               ├─ result sent -> WAIT_DDS_TEST
                                                               └─ STOP_TASK -> IDLE
```

`DDS_RECOGNIZING` 收到一个不同 Test ID 的 DDS_TEST 时返回
`NACK BUSY(0x05)`；收到当前 Test ID、当前 SEQ 的重复 DDS_TEST 时重发
ACK，不启动新识别。

## 16. UART 流解析要求

UART 可能出现半包、粘包、多包和噪声。推荐解析过程：

1. 在字节流中寻找 `AA 55`。
2. 收齐 5 字节元数据：VER、TYPE、SEQ、LEN_L、LEN_H。
3. 若 `LEN > 64`，丢弃当前候选帧，重新寻找帧头。
4. 等待 `LEN + 2` 个剩余字节。
5. 校验 CRC。
6. CRC 正确才发布完整消息。
7. CRC 错误直接丢弃，继续寻找后续 `AA 55`。
8. Payload 中允许出现 `AA 55`，不能把它误当成新帧头。

OpenMV 端应使用持续存在的 `bytearray` 接收缓存。禁止写成：

```python
packet = uart.read()
if len(packet) == expected_length:
    ...
```

因为一次 `uart.read()` 与一帧没有一一对应关系。

多字节整数使用：

```python
import struct

session_id = struct.unpack_from("<H", payload, 0)[0]
dds_freq_hz = struct.unpack_from("<I", payload, 4)[0]

payload = struct.pack("<HHIBH", session_id, test_id,
                      dds_freq_hz, search_stage,
                      capture_delay_ms)
```

不要使用本机默认字节序，也不要假设结构体没有填充字节。

## 17. 完整测试向量

以下帧均已用工程中的实际 CRC 编码器验证。

### 17.1 START_TASK

```text
VER=1, TYPE=0x10, SEQ=0x01
session=1, mode=0, saw=1000 Hz

AA 55 01 10 01 07 00 01 00 00 E8 03 00 00 6F 94
CRC = 0x946F
```

### 17.2 OpenMV ACK START_TASK

假定 OpenMV 自己的发送 SEQ 为 `0x40`：

```text
AA 55 01 01 40 03 00 10 01 00 DB 92
CRC = 0x92DB
```

### 17.3 COARSE_RESULT

```text
SEQ=0x41, session=1, result=0
ratio_x1000=5200, confidence=95, peaks=5/5

AA 55 01 11 41 0A 00 01 00 00 50 14 00 00 5F 05 05 85 62
CRC = 0x6285
```

### 17.4 DDS_TEST

```text
SEQ=0x02, session=1, test=1
dds=5200 Hz, stage=0, capture_delay=200 ms

AA 55 01 20 02 0B 00 01 00 01 00 50 14 00 00 00 C8 00 4B 8C
CRC = 0x8C4B
```

### 17.5 DDS_TEST_RESULT

```text
SEQ=0x42, session=1, test=1
TARGET_REACHED, score=900, confidence=95

AA 55 01 21 42 08 00 01 00 01 00 00 84 03 5F 62 79
CRC = 0x7962
```

### 17.6 STOP_TASK

```text
SEQ=0x03, session=1, reason=normal

AA 55 01 22 03 03 00 01 00 00 96 36
CRC = 0x3696
```

## 18. 联调验收清单

1. 只发送半个 START_TASK，OpenMV 不执行命令；补齐后只执行一次。
2. 一次写入两个完整帧，OpenMV 能连续解析两个。
3. 在合法帧前加入噪声，解析器能重新找到 `AA 55`。
4. 修改一个 Payload 字节但不改 CRC，接收方静默丢弃。
5. START_TASK 的 ACK 丢失，STM32 重发相同 SEQ，OpenMV 不重复启动。
6. COARSE_RESULT 的 ACK 丢失，OpenMV 重发相同结果，STM32只重发 ACK，
   不重复切换输出。
7. DDS_TEST_RESULT 使用旧 Session 或旧 Test ID，不改变 DDS。
8. 短按任一模式键得到 1 kHz DAC 锯齿波；长按至少 400 ms 得到
   10 kHz。
9. 示波器确认 PA4 波形在每周期内单调递增。
10. TARGET_REACHED 后 OpenMV 收到 STOP_TASK 并回到 IDLE；STM32 随后
    切入本地相位锁定。
11. 拔掉 UART 或让 OpenMV 不应答，STM32 在超时后仍停留在 Task5，
    显示错误原因，并保持本次选择的满码域 DAC 锯齿波。
12. 在 Task5 已运行或已锁定时再次选择模式，OpenMV 依次收到旧 Session
    的 `STOP_TASK(reason=0x04)` 和新 Session 的 `START_TASK`，并开始
    新模式。
