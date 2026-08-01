# Task5 与 OpenMV 串口通信协议 v2

本文是 STM32F407 与 OpenMV N6 的当前协议依据。两端必须同时使用
`VER=0x02`。v1 的“成功后发送 `reason=0` 并自动回到 IDLE”已经废止。

## 1. 必须满足的行为

1. 三种题型都先用单倍 DDS 候选确定绝对输入频率。
2. 绝对频率一经确认立即响铃，不等待相位锁定。
3. 随后直接设置题目要求的图像：
   - 对角线：单倍频、0°，方向为左下到右上；
   - 圆：单倍频、-90°；
   - “∞”：二倍频、广义相位 0°。
4. STM32 不启动本地 ADC PLL。OpenMV 持续测量李萨如图像运动，STM32
   根据 `VISUAL_LOCK_SAMPLE` 微调 DDS 频率。
5. 视觉纠偏始终限制在题目输出种子频率的 `±5 Hz`，不得越界。
6. 锁定后视觉反馈仍持续运行；漂移时原地重捕获，不退出 Session。
7. 图像暂时丢失、ACK 丢失、NACK、通信中断或迟到结果均不得自动退出，
   STM32 保持最后一个合法 DDS 命令。
8. 只有实体模式键、串口屏返回/切换、显式 `exit` 或复位可以退出。
9. `EXIT_TASK.reason=0` 非法，OpenMV 必须 NACK 并保持当前状态。

## 2. 物理层与帧格式

- 串口：115200、8N1、3.3 V、共地；
- F407 `PC12/UART5_TX` → OpenMV `P13/UART7_RX`；
- OpenMV `P14/UART7_TX` → F407 `PD2/UART5_RX`；
- 多字节整数为小端；
- CRC 为 CRC16/MODBUS，初值 `0xFFFF`。

```text
SOF0 SOF1 VER TYPE SEQ LEN_L LEN_H PAYLOAD... CRC_L CRC_H
 AA   55   02
```

`LEN` 最大为 64。CRC 覆盖 `VER` 到 Payload 末尾。

## 3. 消息类型

| TYPE | 名称 | 方向 | 说明 |
|---:|---|---|---|
| `0x01` | `ACK` | 双向 | 可靠命令确认 |
| `0x02` | `NACK` | 双向 | 拒绝及错误码 |
| `0x03` | `HEARTBEAT` | 双向 | 心跳 |
| `0x10` | `START_TASK` | STM32 → OpenMV | 开始 Session |
| `0x11` | `COARSE_RESULT` | OpenMV → STM32 | 粗略单倍频率 |
| `0x20` | `DDS_TEST` | STM32 → OpenMV | 单倍候选测试 |
| `0x21` | `DDS_TEST_RESULT` | OpenMV → STM32 | 候选图像结果 |
| `0x22` | `EXIT_TASK` | STM32 → OpenMV | 仅人工退出 |
| `0x30` | `VISUAL_LOCK_START` | STM32 → OpenMV | 兼容旧实验，不用于主路径 |
| `0x31` | `VISUAL_LOCK_SAMPLE` | OpenMV → STM32 | 永久视觉反馈样本 |
| `0x32` | `LOCK_HOLD` | STM32 → OpenMV | 设置题型并启动永久视觉保持 |
| `0x7F` | `OPENMV_ERROR` | OpenMV → STM32 | 诊断消息 |

ACK Payload：

```text
acked_type:u8, acked_seq:u8, status:u8
```

NACK Payload：

```text
rejected_type:u8, rejected_seq:u8, error:u8
```

主要 NACK：`0x01` 长度错误、`0x02` 版本错误、`0x03` 题型错误、
`0x05` 忙、`0x06` 状态错误、`0x07` Session 错误、`0x08` 越界、
`0x0B` 已处于锁相保持。

普通可靠命令 ACK 超时为 100 ms，最多快速重试 3 次。`LOCK_HOLD` 快速
重试耗尽后不结束任务，而是每 1 s 继续用相同 TYPE/SEQ/Payload 重发，直到
收到 ACK 或首个视觉样本。这样短时断线恢复后会自动恢复视觉闭环。

## 4. 业务载荷

### 4.1 START_TASK `0x10`

```text
session_id:u16, lock_mode:u8, saw_frequency_hz:u32
```

`lock_mode`：0 对角线、1 圆、2 “∞”；锯齿波只允许 1000 或 10000 Hz。
`LOCK_HOLD` 中收到不同 Session 的新 START，视为实体按键人工重选，可原子
切换到新 Session。

### 4.2 COARSE_RESULT `0x11`

```text
session_id:u16, result:u8, ratio_x1000:u32,
confidence:u8, reserved0:u8, reserved1:u8
```

只给出单倍输入频率初值。“∞”在此阶段也禁止提前乘二。

### 4.3 DDS_TEST `0x20`

```text
session_id:u16, test_id:u16, dds_frequency_hz:u32,
search_stage:u8, capture_delay_ms:u16
```

全部候选都是单倍频率。重复命令必须重发缓存结果，不重新观察。

### 4.4 DDS_TEST_RESULT `0x21`

```text
session_id:u16, test_id:u16, result:u8,
match_score:u16, confidence:u8
```

- `0x00 TARGET_REACHED`：确认绝对频率、响铃、设置题型 DDS、发送 LOCK_HOLD；
- `0x03 NOT_MATCHED`：继续下一个 100 Hz 候选；
- `0x06 IMAGE_ERROR`：DDS 不变，在同一候选上重试。

### 4.5 LOCK_HOLD `0x32`

```text
session_id:u16, lock_mode:u8,
input_frequency_hz:u32, output_frequency_hz:u32
```

长度固定 11 字节。输入范围 100–100000 Hz，输出不超过 200000 Hz。
模式 0/1 要求 `output=input`；模式 2 要求 `output=2*input`。

OpenMV 接受后：

1. 立即 ACK 并进入 `STATE_LOCK_HOLD`；
2. 保留 DDS 搜索阶段取得的固定示波器 ROI；
3. 根据题型选择 1:1 相位代理或 2:1 广义相位代理；
4. 每当得到有效图像测量就发送 `VISUAL_LOCK_SAMPLE`；
5. 不设置任何自动回 IDLE 的计时器；
6. 只允许合法人工 EXIT 或人工新 START 离开。

### 4.6 VISUAL_LOCK_SAMPLE `0x31`

```text
session_id:u16, sample_id:u16, camera_timestamp_ms:u32,
phase_mdeg:u32, speed_millihz:u16, quality:u8, flags:u8
```

长度固定 16 字节。该消息不要求 ACK；新样本自然覆盖丢失的旧样本。

- `phase_mdeg`：折叠图像相位，范围 0–180000；
- `speed_millihz`：图像相位运动对应的频差绝对值；
- `quality`：0–100；
- `flags.bit0`：相位有效；`bit1`：题型/轨迹有效。

STM32 使用本地接收时间执行稳定等待，不把两块板的启动时钟当成同一时钟。
有效样本驱动 DDS 频率，题目相位固定不变。锁定后仍处理样本；连续漂移时在
`seed ± 5 Hz` 内重新探测方向并收敛。若样本停止，则保持最后 DDS 命令并等待
后续样本，绝不自动退出。

### 4.7 EXIT_TASK `0x22`

```text
session_id:u16, reason:u8
```

- `0x01`：返回、页面切换或显式 `exit`；
- `0x04`：实体模式键人工重选；
- `0x02/0x03/0x05`：预留的显式退出；
- `0x00`：非法，必须 NACK。

正常锁定流程不得打印或发送“任务结束、原因0、已回到空闲”。合法人工退出
可以打印 `【人工退出】`。

## 5. 双端状态机

STM32：

```text
WAIT_SELECTION
  -> WAIT_START_ACK -> WAIT_COARSE_RESULT
  -> DDS_SETTLING <-> WAIT_DDS_ACK/WAIT_DDS_RESULT
  -> VISUAL_LOCKING (响铃、设置目标图像、发送 LOCK_HOLD)
  -> LOCKED (显示锁存，视觉反馈仍持续运行)
```

OpenMV：

```text
IDLE -> COARSE -> WAIT_DDS <-> DDS_RECOGNIZING
                           -> LOCK_HOLD -> 持续发送视觉样本
LOCK_HOLD -- explicit EXIT --> IDLE
LOCK_HOLD -- manual new START --> COARSE(new session)
```

## 6. 验收要点

- 三种模式找频阶段全部使用单倍频率；
- 绝对频率确认时只响铃一次；
- 双环只在确认后设置一次二倍种子，不发生重复倍频或超高频输出；
- 对角线方向为左下到右上；圆和双环相位不会被直线对齐逻辑修改；
- 所有视觉 DDS 命令严格位于目标种子 `±5 Hz`；
- LOCKED 后仍能看到连续 `VISUAL_LOCK_SAMPLE`；
- 源频率漂移时原地重捕获，Task5 不离开 LOCKED；
- 拔掉 OpenMV TX 时 DDS 保持，恢复通信后视觉闭环恢复；
- 无人工输入时双方永不自动退出；
- 返回、显式 `exit` 和实体模式键重选仍可退出或切换。
