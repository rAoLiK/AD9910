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

## 当前 DDS_TEST 的边界

目前已有数据只训练了“锯齿扫描粗识别”，还没有第二阶段 DDS 李萨如图的
高/低方向训练集。因此 `DDS_TEST_RESULT` 暂时以粗识别频率作为目标做
数值方向联调，用于验证 F407 搜索状态机和串口协议；它不是新的图像精判。

在第二阶段视觉模型完成前，不应把该后备结果当作最终 100 Hz 精调依据。
若希望联调时禁止后备判断，把单文件顶部的
`DDS_NUMERIC_FALLBACK = True` 改为 `False`，此时 DDS 测试统一返回
`LOW_CONFIDENCE`。

## 同步嵌入模型

更新两个 `ref/main_*_new_dataset.py` 的模型常量后，在仓库根目录运行：

```powershell
.\tools\sync_openmv_task5_models.ps1
```

脚本会把两份参考模型同步到 `openmv\OpenMV_main_task5_uart.py`，不会改写
该文件中的 UART 协议与应用状态机。
