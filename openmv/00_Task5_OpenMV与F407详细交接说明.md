# Task5 OpenMV 与 STM32F407 详细交接说明

更新时间：2026-07-31  
交接对象：负责野火 STM32F407、DAC 锯齿波、DDS 和串口状态机的同学  
OpenMV 平台：OpenMV N6（STM32N657X0）  
OpenMV 固件：OpenMV v5.0.0 / MicroPython v1.28.0-49

---

## 1. 先说结论

当前 OpenMV 端已经完成以下内容：

1. 1 kHz 锯齿扫描图像的频率倍数粗识别；
2. 10 kHz 锯齿扫描图像的频率倍数粗识别；
3. 两套模型合并为一个 OpenMV 单文件程序；
4. 根据 F407 在 `START_TASK` 中给出的扫描频率选择模型；
5. UART7 二进制通信协议、CRC、ACK/NACK、Session 和状态机；
6. 拆包、粘包、前导噪声、CRC 错误恢复和重复命令处理；
7. 中文串口提示、IDE 瞄准画面和 OLED 低帧率预览。

F407 必须明确告诉 OpenMV 当前锯齿波是 1 kHz 还是 10 kHz。
OpenMV 不会根据画面自行猜测扫描档位。

目前真正经过图像数据训练和验证的是“锯齿扫描粗识别”。第二阶段 DDS
李萨如图的视觉高低方向判断还没有训练完成。现有 `DDS_TEST_RESULT`
是为了先把 F407 搜索状态机和串口链路联通而提供的数值后备逻辑，
不能当成最终视觉精调结果。

---

## 2. 交接包文件

| 文件 | 用途 |
|---|---|
| `OpenMV_main_task5_uart.py` | 最终 OpenMV 单文件，包含两套模型和 UART7 状态机 |
| `Task5与OpenMV串口通信协议-最终版.md` | 双方必须共同遵守的完整协议 |
| `OpenMV程序运行说明.md` | OpenMV 运行、接线和重新生成说明 |
| `00_Task5_OpenMV与F407详细交接说明.md` | 当前这份面向 F407 同学的交接说明 |

最终 OpenMV 文件 SHA256：

```text
7AD225414B0CECFCB3D68E3C47C1F9A5DCCBB915660AFA6268CA777607BD98AC
```

如果之后有人修改了程序，哈希改变是正常的，但应重新进行协议测试和
OpenMV 实机测试。

---

## 3. 整体工作流程

```text
F407 输出指定频率锯齿波
        |
        | START_TASK(session, mode, saw_freq)
        v
OpenMV 立即 ACK
        |
        | 根据 saw_freq 选择 1 kHz 或 10 kHz 模型
        | 拍摄示波器 XY 图并进行多帧投票
        v
OpenMV 发送 COARSE_RESULT
        |
        v
F407 立即 ACK
        |
        | 停止 DAC 锯齿波
        | 切换继电器到 DDS 路径
        | 根据粗估值设置 DDS
        | 本地等待 200 ms
        v
F407 发送 DDS_TEST
        |
        v
OpenMV 立即 ACK，再等待 capture_delay_ms
        |
        v
OpenMV 发送 DDS_TEST_RESULT
        |
        v
F407 立即 ACK，并根据结果调整 DDS
        |
        | 重复 DDS_TEST，找到目标后
        v
F407 发送 STOP_TASK，OpenMV ACK 后回到 IDLE
```

扫描档位的预期用途：

| 输入信号大致范围 | F407 锯齿波 | START_TASK 中的 `saw_freq_hz` | OpenMV 模型 |
|---|---:|---:|---|
| 1～10 kHz | 1 kHz | 1000 | 1 kHz 模型 |
| 10～100 kHz | 10 kHz | 10000 | 10 kHz 模型 |

10 kHz 边界可以由 F407 的上层策略决定使用哪个档位，但发送给 OpenMV 的
频率必须与示波器上实际输出的锯齿波一致。

---

## 4. 图像识别部分做了什么

### 4.1 数据集

两档扫描频率使用各自独立的数据集：

- 1 kHz 档：1820 张，每个待测频率 20 张；正弦波 1～10 kHz，
  100 Hz 步进；数据集 SHA256 为
  `b0da32f07e5434607ac573d19ec1d5207cbc0f0300b35312dcfd22761368e8d1`。
- 10 kHz 档：910 张，每个待测频率 10 张；正弦波 10～100 kHz，
  1 kHz 步进。
- 1 kHz v2 数据集要求白框和中央黑色示波器屏幕完整入镜。

### 4.2 算法

OpenMV 对 QVGA RGB565 图像进行识别。1 kHz v2 模型先利用白框隔离中央
黑色示波器屏幕，再裁剪屏幕内部区域；短时定位失败时最多沿用最近 20 帧
的可靠屏幕矩形。10 kHz 模型继续使用原有绿色轨迹定位。两者都把识别
区域缩放为 32×32 特征，再用量化 PCA 和最近邻模型判断频率倍数。

两套模型参数：

| 模型 | PCA 维数 | 训练样本/参考样本 |
|---|---:|---:|
| 1 kHz 扫描（白框 v2） | 40 | 1820 |
| 10 kHz 扫描 | 38 | 910 |

程序不会仅使用第一帧。一次粗识别最多保留最近 12 个有效单帧结果，
至少取得 8 个结果后，在 ±0.3 倍范围内聚类并取中位数。这样可以抑制
刚切换信号时的错误帧和偶发别名。

### 4.3 离线验证结果

| 模型 | 验证方式 | 主要结果 |
|---|---|---|
| 1 kHz 白框 v2 | 滚动验证 | 误差≤100 Hz：98.85%；误差≤200 Hz：100% |
| 10 kHz | 固定三帧中位数 | 误差≤1 kHz：95.60%；误差≤2 kHz：100% |
| 10 kHz | 覆盖全部 910 张的滚动验证 | 误差≤1 kHz：93.41%；误差≤2 kHz：99.89% |

这说明粗识别适合给 F407 确定后续 DDS 搜索起点，不应把 10 kHz 档的
粗结果理解为已经达到 100 Hz 精调精度。

### 4.4 时间

当前 IDE 开图和 OLED 开启时，实际帧率约为 3 FPS。至少 8 个有效画面
通常约 2.7 秒。OpenMV 内部粗识别超时为 4 秒，协议中 F407 等待
`COARSE_RESULT` 的上限是 5 秒。

如果赛场已经确认相机位置，可以把 OpenMV 单文件顶部的
`IDE_STREAM_ENABLED = True` 改为 `False`，减少 USB 图像流开销。
OLED 已限制为低刷新率，只用于判断相机是否对准。

---

## 5. 硬件串口接线

串口参数：

```text
115200 baud
8 data bits
no parity
1 stop bit
无硬件流控
```

接线：

| STM32F407 | OpenMV N6 | 方向 |
|---|---|---|
| PC12 / UART5_TX | P13 / UART7_RX | F407 → OpenMV |
| PD2 / UART5_RX | P14 / UART7_TX | OpenMV → F407 |
| GND | GND | 共地 |

注意事项：

1. TX 与 RX 必须交叉；
2. 两边都使用 3.3 V TTL 电平；
3. 必须共地；
4. 不要接 RS-232 电平；
5. OpenMV v5.0.0 的 N6 固件不接受 `tx/rx/txbuf/rxbuf` 初始化关键字，
   当前程序使用已经实机通过构造阶段的兼容写法：

```python
UART(7, 115200)
```

UART7 的 P14/P13 映射由 OpenMV 固件根据外设编号固定选择。

---

## 6. 帧格式

每一帧为：

```text
AA 55 VER TYPE SEQ LEN_L LEN_H PAYLOAD... CRC_L CRC_H
```

| 字段 | 长度 | 说明 |
|---|---:|---|
| `AA 55` | 2 | 固定帧头 |
| `VER` | 1 | 当前固定为 `0x01` |
| `TYPE` | 1 | 消息类型 |
| `SEQ` | 1 | 发送方自己的发送序号 |
| `LEN_L/LEN_H` | 2 | Payload 长度，小端，最大 64 |
| `PAYLOAD` | LEN | 消息内容 |
| `CRC_L/CRC_H` | 2 | CRC16/MODBUS，小端 |

总帧长：

```text
9 + LEN
```

CRC 计算范围：

```text
从 VER 开始，到 Payload 最后一个字节结束
```

CRC 不包括 `AA 55`，也不包括 CRC 字段本身。

CRC 参数：

```text
CRC16/MODBUS
初值：0xFFFF
多项式：0xA001
结果按低字节在前发送
```

参考 C 实现：

```c
uint16_t Task5_CRC16_Modbus(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;

    while (length--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; ++i) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

禁止直接把未加 `packed` 的 C 结构体强制转换后发送。最安全的方法是逐字段
按小端写入字节数组，避免结构体填充和字节序问题。

---

## 7. 消息类型

| TYPE | 名称 | 方向 |
|---:|---|---|
| `0x01` | ACK | 双向 |
| `0x02` | NACK | 双向 |
| `0x03` | HEARTBEAT | 双向，可选 |
| `0x10` | START_TASK | F407 → OpenMV |
| `0x11` | COARSE_RESULT | OpenMV → F407 |
| `0x20` | DDS_TEST | F407 → OpenMV |
| `0x21` | DDS_TEST_RESULT | OpenMV → F407 |
| `0x22` | STOP_TASK | F407 → OpenMV |
| `0x7F` | OPENMV_ERROR | OpenMV → F407，可选 |

ACK 和 NACK 本身不能再被 ACK，否则双方会形成无限 ACK 循环。

---

## 8. SEQ、Session ID 和 Test ID

### 8.1 SEQ

F407 和 OpenMV 各自维护自己的 8 位发送 SEQ，发送新帧时递增，溢出后
自然回到 0。

ACK Payload 中的 `ack_type` 和 `ack_seq` 指向被确认的原帧。
ACK 帧外层自己的 SEQ 是 ACK 发送方的新序号，不能拿它与原命令 SEQ
直接比较。

协议文档中的 ACK 示例使用 `0x40` 只是示例。当前 OpenMV 每次上电后
自己的发送 SEQ 从 0 开始，F407 不应写死 OpenMV 的外层 SEQ。

### 8.2 Session ID

- `uint16_t`；
- 0 无效；
- 每次新的 Task5 流程使用新的 Session；
- 同一任务内所有命令和结果必须使用相同 Session；
- 收到旧 Session 时返回 `NACK SESSION(0x07)`。

建议 F407 每次开始任务时将 Session 加 1，并跳过 0。

### 8.3 Test ID

- `uint16_t`；
- 0 无效；
- 同一 Session 中每个新的 DDS 频率测试加 1；
- 超时重发同一个 `DDS_TEST` 时，Test ID 和 SEQ 都不能改变。

---

## 9. ACK/NACK

ACK Payload 固定 3 字节：

```text
ack_type, ack_seq, status
```

`status`：

| 值 | 含义 |
|---:|---|
| `0x00` | 第一次接收，已接受 |
| `0x01` | 重复包，已识别，不重复执行 |
| `0x02` | 已转入异步处理 |

NACK Payload 固定 3 字节：

```text
nack_type, nack_seq, error_code
```

错误码：

| 值 | 含义 |
|---:|---|
| `0x01` | 长度错误 |
| `0x02` | 协议版本错误 |
| `0x03` | lock_mode 错误 |
| `0x04` | 锯齿波频率非法 |
| `0x05` | 正忙 |
| `0x06` | 当前状态不允许该命令 |
| `0x07` | Session 错误 |
| `0x08` | 参数超范围 |
| `0x09` | 相机错误 |
| `0x0A` | 内部错误 |

---

## 10. START_TASK

```text
TYPE = 0x10
LEN  = 7
Payload = session_id:uint16
          lock_mode:uint8
          saw_freq_hz:uint32
```

字段均为小端。

合法值：

- `session_id != 0`；
- `lock_mode` 为 0、1、2；
- `saw_freq_hz` 只能是 1000 或 10000；
- OpenMV 必须处于 IDLE，或者该帧是当前 START 的完全重复包。

OpenMV 收到合法 START 后会先发送 ACK，再加载模型和识别。因此 F407 的
100 ms ACK 等待不会被 PCA 计算阻塞。

1 kHz 示例：

```text
session=1, mode=0, saw=1000, SEQ=1
AA 55 01 10 01 07 00 01 00 00 E8 03 00 00 6F 94
```

---

## 11. COARSE_RESULT

```text
TYPE = 0x11
LEN  = 10
Payload = session_id:uint16
          result:uint8
          ratio_x1000:uint32
          confidence:uint8
          left_peak_count:uint8
          right_peak_count:uint8
```

当前 OpenMV 的主要结果：

| result | 含义 | F407 行为 |
|---:|---|---|
| `0x00` | 成功 | 继续 DDS |
| `0x01` | 有结果但置信度较低 | 可以继续 DDS，但扩大搜索范围 |
| `0x02` | 没找到有效曲线 | ACK 后安全结束 |
| `0x06` | 识别超时 | ACK 后安全结束 |
| 其他 | 见完整协议 | ACK 后按错误处理 |

频率计算：

```c
estimated_input_hz =
    round(saw_freq_hz * ratio_x1000 / 1000);
```

为了防止 32 位乘法溢出，F407 建议使用 `uint64_t` 中间变量：

```c
uint32_t estimated_input_hz =
    (uint32_t)(((uint64_t)saw_freq_hz * ratio_x1000 + 500) / 1000);
```

模式与 DDS 初始值：

```text
mode 0/1：initial_dds_hz = estimated_input_hz
mode 2：  initial_dds_hz = 2 * estimated_input_hz
```

OpenMV 当前 `left_peak_count/right_peak_count` 是调试辅助值，F407 的主流程
不要依赖这两个字段，只使用 `result`、`ratio_x1000` 和 `confidence`。

F407 收到合法 COARSE_RESULT 后的顺序：

1. 立即 ACK；
2. 停止 DAC 锯齿波；
3. 切换继电器到 DDS 路径；
4. 设置 DDS 初始频率；
5. 本地等待 200 ms；
6. 发送第一个 DDS_TEST。

---

## 12. DDS_TEST

```text
TYPE = 0x20
LEN  = 11
Payload = session_id:uint16
          test_id:uint16
          dds_freq_hz:uint32
          search_stage:uint8
          capture_delay_ms:uint16
```

约束：

- `test_id != 0`；
- `dds_freq_hz` 为 1～200000；
- `search_stage` 为 0～4；
- 当前协议推荐 `capture_delay_ms = 200`；
- 必须在 OpenMV 的 `WAIT_DDS_TEST` 状态发送。

F407 调 DDS 后已经本地等待 200 ms，OpenMV 收到命令并 ACK 后还会至少
等待一次 `capture_delay_ms`，再给出判断。这可以避免 DDS 或示波器画面
尚未稳定。

如果 OpenMV 正在处理当前 Test：

- 完全相同的 TYPE、SEQ、Test ID 和 Payload：ACK DUPLICATE，不重新处理；
- 不同 Test ID：NACK BUSY；
- 旧 Session：NACK SESSION。

---

## 13. DDS_TEST_RESULT

```text
TYPE = 0x21
LEN  = 8
Payload = session_id:uint16
          test_id:uint16
          result:uint8
          match_score:uint16
          confidence:uint8
```

| result | 含义 | F407 建议行为 |
|---:|---|---|
| `0x00` | TARGET_REACHED | 结束搜索，准备 STOP |
| `0x01` | DDS_TOO_LOW | 提高 DDS |
| `0x02` | DDS_TOO_HIGH | 降低 DDS |
| `0x03` | 方向未知、不匹配 | 围绕粗估值扩大搜索 |
| `0x04` | 不稳定 | 原频率重测，最多额外 2 次 |
| `0x05` | 低置信度 | 原频率重测，最多额外 2 次 |
| `0x06` | 图像错误 | 原频率重测，最多额外 2 次 |
| `0x07` | 超时 | 原频率重测，最多额外 2 次 |

F407 必须先 ACK 结果，再修改 DDS 或发送下一测试。OpenMV 会保留结果原帧，
如果 F407 因结果 ACK 丢失而重发同一 DDS_TEST，OpenMV 会重发完全相同的
DDS_TEST_RESULT，不会创建新判断。

---

## 14. 当前 DDS 后备逻辑，务必看清

当前程序中的：

```python
DDS_NUMERIC_FALLBACK = True
```

表示第二阶段暂时不使用新的李萨如图视觉模型，而是将 DDS 频率与
`COARSE_RESULT` 得到的粗估目标比较：

```text
mode 0/1：目标 = 粗估输入频率
mode 2：  目标 = 2 × 粗估输入频率
```

容差：

```text
1 kHz 扫描模型：100 Hz
10 kHz 扫描模型：1000 Hz
```

返回规则：

```text
误差在容差内：TARGET_REACHED
DDS 小于目标：DDS_TOO_LOW
DDS 大于目标：DDS_TOO_HIGH
```

这个逻辑的用途是：

- 检查 F407 UART 收发；
- 检查 Session/Test ID/SEQ；
- 检查二分或变步长搜索状态机；
- 检查 ACK、超时和重发；
- 检查继电器、DDS 设置与停止流程。

它不能证明 OpenMV 已经从 DDS 李萨如图判断出真实高低方向，也不能补偿
DDS 晶振与信号源晶振的微小频偏。正式完成第二阶段前，不要只凭
`TARGET_REACHED` 宣称视觉精调已完成。

如果只想严格测试通信而不允许后备逻辑返回成功，可以把程序顶部改为：

```python
DDS_NUMERIC_FALLBACK = False
```

此时 DDS_TEST 会返回 `LOW_CONFIDENCE`。

---

## 15. STOP_TASK

```text
TYPE = 0x22
LEN  = 3
Payload = session_id:uint16
          reason:uint8
```

常用 reason：

| 值 | 含义 |
|---:|---|
| `0x00` | 正常找到目标 |
| `0x01` | 用户退出 |
| `0x02` | 搜索超时 |
| `0x03` | 搜索次数超限 |
| `0x04` | 重新开始 |
| `0x05` | F407 系统或通信错误 |

OpenMV 收到当前 Session 的合法 STOP 后立即 ACK、释放当前模型并返回
IDLE。STOP 的重复包会收到 DUPLICATE ACK，不会再次改变状态。

---

## 16. 超时与重发

F407 建议严格使用协议中的值：

| 项目 | 数值 |
|---|---:|
| 等待命令 ACK | 100 ms |
| ACK 最大重发 | 3 次，即总发送最多 4 次 |
| 等待 COARSE_RESULT | 5 s |
| 等待 DDS_TEST_RESULT | 2 s |
| 结果等待最大重放 | 3 次 |
| DDS 本地稳定等待 | 200 ms |
| OpenMV capture_delay | 200 ms |
| 单 Session 新 DDS_TEST 上限 | 40 次 |

超时重发时，必须重发原始帧：

```text
TYPE 不变
SEQ 不变
Session ID 不变
Test ID 不变
Payload 不变
CRC 自然也不变
```

不能因为超时就分配新 SEQ 或新 Test ID，否则 OpenMV 会把它当成新命令。

CRC 错误帧不回复 ACK/NACK，直接静默丢弃，等待发送方超时重发。

---

## 17. F407 接收解析器要求

UART 是字节流，一次中断或 DMA 读取不一定对应一帧。F407 不能假设
“一次收到的字节就是一个完整包”。

推荐使用环形缓冲区或持久线性缓存，循环执行：

1. 搜索连续的 `0xAA 0x55`；
2. 不足 7 字节时继续等待；
3. 读取 LEN；
4. LEN 大于 64 时丢弃当前帧头并重新找头；
5. 缓存不足 `9 + LEN` 时继续等待；
6. 校验 CRC；
7. CRC 正确才分发；
8. CRC 错误时向后重新搜索 `AA 55`；
9. 处理完一帧后继续解析缓存中的下一帧。

必须通过以下情况：

- 半帧分两次到达；
- 两帧一次到达；
- 合法帧前有噪声；
- CRC 错误帧后紧跟合法帧；
- Payload 中出现 `AA 55`，不能因此提前切帧；
- ACK 丢失导致同一命令重复到达。

---

## 18. F407 建议状态机

```text
IDLE
  |
  | 用户开始
  v
OUTPUT_SAW
  |
  | 锯齿波稳定，发送 START_TASK
  v
WAIT_START_ACK
  |
  v
WAIT_COARSE_RESULT
  |
  | ACK COARSE_RESULT
  | 停 DAC，切 DDS
  v
SET_DDS_AND_WAIT
  |
  | 200 ms 后发送 DDS_TEST
  v
WAIT_DDS_ACK
  |
  v
WAIT_DDS_RESULT
  |
  | ACK 结果
  | 根据 LOW/HIGH/TARGET 调整
  +----------------------+
  |                      |
  | 未找到               | 找到或失败
  v                      v
SET_DDS_AND_WAIT       SEND_STOP
                         |
                         v
                    WAIT_STOP_ACK
                         |
                         v
                       IDLE
```

任何无法恢复的通信错误都应：

1. 关闭 DAC/DDS 或切到安全输出；
2. 尽量发送 `STOP_TASK(reason=0x05)`；
3. 清空本地 Task5 状态；
4. 给 OLED/上位机明确错误提示；
5. 返回 IDLE，等待用户重新开始。

---

## 19. OpenMV 运行方法

联调时：

1. 用 OpenMV IDE 打开 `OpenMV_main_task5_uart.py`；
2. 确认 IDE 连接的是 OpenMV N6；
3. 点击运行；
4. 串口终端应出现：

```text
【启动】Task5 双档识别 + UART7
【就绪】...；UART7 115200，P14发送/P13接收
【等待】请由F407发送 START_TASK
```

5. F407 发送 START_TASK 后应出现：

```text
【开始粗识别】任务1，扫描1000 Hz，模式0
```

6. 成功时出现：

```text
【粗识别结果】约4300 Hz（4.3倍），可信度xx%，已发送
```

若需要脱离 IDE 上电自启，应把该单文件作为 OpenMV 文件系统中的
`main.py` 保存。交接包中故意保留独立文件名，避免覆盖之前的单档程序。

---

## 20. 已完成的检查

PC 端已经完成：

1. Python 语法编译检查；
2. 协议文档 6 组完整测试向量逐字节核对；
3. CRC16/MODBUS 核对；
4. 半包、粘包、前导噪声测试；
5. CRC 错帧静默丢弃测试；
6. START → COARSE → DDS → STOP 状态机模拟；
7. 重复 START、重复 DDS_TEST、重复 STOP 测试；
8. 两套嵌入模型与原始模型逐字节比较；
9. 确认旧的 1 kHz、10 kHz 单档 OpenMV 文件未被覆盖。

最新一次 OpenMV 实机运行还发现并修复了 UART 初始化兼容问题。N6 固件
不接受通用 MicroPython 的额外 UART 关键字，最终程序已经改为：

```python
UART(7, 115200)
```

---

## 21. 双方联调必须完成的实机检查

建议按顺序逐项打勾：

- [ ] F407 PC12 接 OpenMV P13；
- [ ] F407 PD2 接 OpenMV P14；
- [ ] 双方共地；
- [ ] 示波器确认 F407 锯齿波实际为 1 kHz；
- [ ] 示波器确认 F407 锯齿波实际为 10 kHz；
- [ ] F407 发送 1 kHz START 测试向量，收到合法 ACK；
- [ ] OpenMV 选择 1 kHz 模型并返回 COARSE_RESULT；
- [ ] F407 发送 10 kHz START，OpenMV 选择 10 kHz 模型；
- [ ] F407 对每个 OpenMV 结果立即 ACK；
- [ ] 故意丢弃一次 ACK，确认重复命令不重复执行；
- [ ] 一次发送两个帧，确认双方都能解析；
- [ ] 分两次发送半帧，确认补齐后只执行一次；
- [ ] 修改一个 Payload 字节但不改 CRC，确认被静默丢弃；
- [ ] 切换 DDS 后至少等待 200 ms；
- [ ] 同一 DDS 测试超时重发时 SEQ/Test ID 不变；
- [ ] 不同 Test ID 在 OpenMV 忙时收到 NACK BUSY；
- [ ] STOP 后 OpenMV 回到 IDLE；
- [ ] 拔掉 UART 后 F407 能超时关闭输出并回到安全状态；
- [ ] 10 秒总时间内完成粗识别和预定的 DDS 搜索次数评估。

---

## 22. 当前最重要的三条注意事项

1. **F407 发出的 `saw_freq_hz` 必须与实际锯齿波一致。**  
   实际是 1 kHz 就发送 1000，实际是 10 kHz 就发送 10000。OpenMV 不猜。

2. **收到 OpenMV 结果必须先 ACK，再切换硬件或发送下一条命令。**  
   超时重发必须原帧重发，SEQ 和 Test ID 均不能变化。

3. **DDS 阶段目前是通信联调后备逻辑，不是最终视觉精调。**  
   粗识别模型已经有数据验证；DDS 李萨如图精判仍需要后续数据和模型。
