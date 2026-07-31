# Task5 双档识别与 UART7 联调

## 直接使用

在 OpenMV IDE 中打开 `OpenMV_main_task5_uart.py` 并运行。它是完整
单文件，不依赖 OpenMV 文件系统中的其他 `.py` 文件。需要脱离 IDE 上电
自动运行时，将该文件复制到 OpenMV 的 `PYBFLASH` 根目录并命名为
`main.py`。

原来的 `main.py`（1 kHz）和 `main_10k.py`（10 kHz）没有被覆盖。

F407 在 `START_TASK` 中发送的 `saw_freq_hz` 决定使用哪个模型：

- `1000`：使用 1 kHz 扫描模型；
- `10000`：使用 10 kHz 扫描模型；
- 其他值：返回 `NACK 0x04`，OpenMV 不会自行猜测扫描频率。

1 kHz 与 10 kHz 均使用对应参考程序中的 1820 张新数据集模型、最近
20 帧中的至少 12 个有效结果，并以 1 秒为周期做聚类投票；1 kHz 的
聚类半径为 ±0.4 倍，10 kHz 为 ±0.3 倍。任一档连续 1.2 秒丢失波形时
都会清空快速共识历史，避免把移动相机前后的结果混合。粗识别现在没有
失败硬超时：稳定共识一旦
形成就立即上报；30 秒后若仍不稳定但曾识别到有效轨迹，则从最多 40 个
有效候选中返回最密集的一簇并标记低置信度；完全没有有效轨迹时持续采集，
不发送 `result=2/ratio=0`。模型载入和相机单帧采集失败也只会节流重试。

STM32 在等待粗识别期间每 5 秒重放同一条 `START_TASK`，用于让 OpenMV
重发可能丢失的缓存结果，但不会因等待次数进入错误态。若连接了仍会发送
失败结果的旧 OpenMV 程序，STM32 会 ACK、结束旧 Session 并自动开始新
Session，不再显示 `OpenMV bad result`。Task5 三个实体键始终可以取消
当前识别并选择新模式。

新 10 kHz 模型按实测固定偏差在投票前补偿：原始估计 20～50 kHz 加
1 kHz，原始估计不低于 51 kHz 加 2 kHz，最后限制为 10～100 kHz。
新 1 kHz 模型也按其参考程序补偿：原始估计从 1.9、3.8、5.6、7.4 kHz
起分别加 0.2、0.3、0.5、0.8 kHz，最后限制为 1～10 kHz。UART 上报的
`ratio_x1000` 已经是对应模型补偿后的值，STM32 端不要重复补偿。相机
启动预热已同步为参考程序的 1000 ms，随后冻结自动增益、自动曝光和
自动白平衡。

## UART7 接线

通信参数为 `115200, 8N1`。

| F407 | OpenMV N6 | 作用 |
|---|---|---|
| PC12 / UART5_TX | P13 / UART7_RX | F407 发给 OpenMV |
| PD2 / UART5_RX | P14 / UART7_TX | OpenMV 发给 F407 |
| GND | GND | 必须共地 |

TX 和 RX 必须交叉连接，双方均使用 3.3 V 电平。

## OpenMV N6 MicroPython 兼容性

OpenMV v5.0.0 / MicroPython v1.28.0-49 的 N6 固件不支持
`del bytearray[...]`。协议接收缓存消费已使用重新切片：

```python
self.buffer = self.buffer[consumed:]
```

不要改回 `del self.buffer[:consumed]`，否则首个完整 START_TASK 虽然已经
通过 CRC 校验，解析器仍会抛出 `TypeError` 并在发送 ACK 前退出。

## 已实现的协议行为

- 固定帧头、LEN、CRC16/MODBUS；
- UART 粘包、拆包、前导噪声和 CRC 错帧恢复；
- `START_TASK`、`COARSE_RESULT`、`DDS_TEST`、
  `DDS_TEST_RESULT`、`STOP_TASK`、`HEARTBEAT`；
- 耗时处理前立即 ACK；
- 重复命令不重复执行，结果帧按原 SEQ 原字节重发；
- Session、状态、范围、忙状态及长度检查；
- 串口信息采用简短中文提示，IDE 图像保留瞄准框，OLED 仍只作瞄准预览。

## 第二阶段 DDS_TEST 视觉判定

粗识别只负责给出大致频率。F407 将目标 DDS 频率按就近取整量化到
100 Hz 网格并限制在合法输出范围，以该值为 `origin`，随后固定按以下
顺序尝试：

```text
origin, origin + 100 Hz, origin - 100 Hz,
origin + 200 Hz, origin - 200 Hz, ...
```

步长始终为 100 Hz。候选顺序和 DDS 设置由 F407 负责；OpenMV 不根据
图像猜测“应升高”或“应降低”，也不以粗估数值代替第二阶段视觉判断。

每个新的 `DDS_TEST` 到来后，OpenMV 等待规定的稳定时间，然后在固定的
示波器屏幕 ROI 内提取绿色轨迹，并以一个多帧窗口检查轨迹稳定性和稀疏度。
图像可用时，第二阶段只发送两种业务分类：

- `TARGET_REACHED`（`0x00`，简称 `TARGET`）：多帧特征满足已标定的目标条件；
- `NOT_MATCHED`（`0x03`）：不满足目标条件，由 F407 尝试下一个候选频率。

OpenMV 不在第二阶段发送 `DDS_TOO_LOW`、`DDS_TOO_HIGH`、`UNSTABLE`、
`LOW_CONFIDENCE` 或 `TIMEOUT` 作为业务分类。若在整个观察期限内无法定位
屏幕或得到足够的有效轨迹帧，则返回 `IMAGE_ERROR`（`0x06`）；F407 会在
同一频率重试，而不是把相机故障误当成频率不匹配。重复的同一 `DDS_TEST`
仍按协议重发缓存结果，不创建一次新的判定。

收到 `TARGET_REACHED` 后，F407 ACK 结果并发送正常 `STOP_TASK`。STOP
清理握手无论成功还是重试耗尽，都保持当时的 DDS 频率，不启动本地 PLL；
对于支持蜂鸣命令的 TJC 屏，F407 发送 `beep 500`，产生 0.5 s 提示音。

### 视觉阈值标定

稳定性和稀疏度阈值不能只凭单张理想图片指定。应固定实际相机位置、镜头、
曝光、示波器亮度、ROI 和接线，至少采集以下多帧序列：

| DDS 相对正确频率 | 标定用途 | 期望分类 |
|---:|---|---|
| `0 Hz` | 正样本，覆盖正常抖动和亮度变化 | `TARGET_REACHED` |
| `+100 Hz`、`-100 Hz` | 最近邻负样本，约束误报 | `NOT_MATCHED` |
| `+200 Hz`、`-200 Hz` | 更远负样本，检查判据单调性和余量 | `NOT_MATCHED` |

阈值确定后还需使用未参与标定的数据和实际板卡复测。题目频率是 100 Hz 的
倍数，因此上述搜索能逐点覆盖候选网格；但视觉判定本身不能测出 1 Hz 误差。
在未用频率计或示波器验证 DDS 与信号源的真实误差前，不得把
`TARGET_REACHED` 绝对声称为误差小于 1 Hz。

## 同步嵌入模型

更新两个 `ref/main_*_new_dataset.py` 的模型常量后，在仓库根目录运行：

```powershell
.\tools\sync_openmv_task5_models.ps1
```

脚本会把两份参考模型同步到 `openmv\OpenMV_main_task5_uart.py`，不会改写
该文件中的 UART 协议与应用状态机。
