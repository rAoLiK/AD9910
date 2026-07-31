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

粗识别使用最近 12 帧中的至少 8 个有效结果做聚类投票，在当前约 3 FPS
条件下通常约 2.7 秒取得结果；最晚 4 秒返回，低于协议规定的 5 秒超时。

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

## 重新生成单文件

修改 `openmv_src` 中的源码后，在 PowerShell 中运行：

```powershell
.\tools\build_openmv_task5_uart.ps1
```

输出仍为 `openmv\main_task5_uart.py`。
