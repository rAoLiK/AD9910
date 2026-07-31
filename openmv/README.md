# OpenMV 5.0 示波器 XY 图形识别

本程序识别示波器 XY 模式下的 Lissajous 图形，目标量是 **X、Y 两路信号的频率关系**。之所以这样解释“大小关系”，是因为后续的“近似一致或二倍关系”及图形缓慢变化，正是频率比接近 `1:1` 或 `2:1` 时的典型现象。程序也会附带输出由轨迹包围框得到的幅度比诊断值。

主程序为 [main.py](./main.py)，针对 OpenMV 固件 **5.0.0** 编写，使用新版 `csi.CSI()` 接口和元组坐标参数，没有沿用旧教程中的 `sensor.*` API。

## 判定原理

第一阶段在示波器绘图区内部的 20%～80% 位置分别放置 7 条水平和
7 条垂直探针，统计每条探针与亮轨迹形成的连续交点簇。完整
Lissajous 图中：

- 垂直探针上的平均交点数反映 X 方向频率因子；
- 水平探针上的平均交点数反映 Y 方向频率因子；
- 多帧一致后输出 `FX_GT_FY` 或 `FX_LT_FY`；
- 接触数相同、轨迹被遮挡或相位使接触点重合时，自动进入第二阶段。

内部探针不要求轨迹成为一个独立 blob。即使亮轨迹与示波器边框、
网格或菜单发生粘连，第一阶段仍然可以工作。

第二阶段利用图形随时间缓慢变化时出现的退化形状：

- 接近 `1:1` 时会周期性退化成一条直线，blob 的 `roundness` 很小；
- 若相机曝光、示波器余辉或采样时间叠加出多条交叉曲线，图形可能成为
  较密的中心对称网状区域。此时不要求单一 blob，而是连续检查 X/Y
  探针复杂度是否在 20% 容差内保持平衡、帧间变化是否缓慢；
- 接近 `2:1` 时会周期性退化成抛物线，轨迹质心会沿高频信号所在轴偏离包围框中心；
- 多帧滞回确认后输出 `FX_APPROX_FY`、`FX_APPROX_2FY` 或 `FY_APPROX_2FX`。

程序不会因一帧噪声立即改变结果。第一阶段默认需要 3 个有效帧；
清晰的直线/抛物线需要 4 个有效形状帧，密集网状的近 `1:1` 图形
需要额外连续观察，通常约 15 帧得到稳定结果。

## 启动时自动调节画面

程序启动后会依次完成：

1. 用 `find_rects()` 在整帧中寻找尺寸足够大且接近正方形的示波器
   绘图区，并向内缩进以排除亮边框；
2. 固定 0 dB 增益，扫描 700～7000 μs 的曝光候选值；
3. 在最佳曝光下扫描传感器对比度 `-2～3`，选中后再复扫一次曝光；
4. 对每个组合计算 ROI 灰度直方图，用 Otsu 分成背景和轨迹两类；
5. 最大化两类平均亮度差，同时惩罚轨迹饱和和背景过亮；
6. 应用最佳曝光/对比度，并自动生成轨迹阈值。

初始化完成后会输出一行：

```text
CAL,roi=68:40:170:166,exp=1900,gain=0,contrast=2,
threshold=176,dark=62,bright=221,balance=351,score=11842
```

实际输出在同一行。`dark`、`bright` 是背景和亮轨迹的平均灰度；
两者差值越大越好。自动调节只在启动时执行，不影响后续实时帧率。

## 部署

1. 将 `main.py` 复制到 OpenMV 文件系统根目录。
2. 固定相机和示波器，尽量使屏幕 X/Y 轴与图像水平/垂直方向一致。
3. 在 OpenMV IDE 中先运行并观察画面；调试时可设置：

   ```python
   DEBUG_DRAW = True
   ```

4. 正常情况下保留 `AUTO_DETECT_PLOT_ROI = True` 和
   `STARTUP_AUTO_CONTRAST = True`，无需手动设置 ROI、曝光或阈值。
5. 确认后关闭 `DEBUG_DRAW`，避免绘图占用帧时间。

如果自动矩形检测失败，程序使用以下 QVGA 备用 ROI：

```python
ANALYSIS_ROI = (68, 40, 170, 166)
```

若要强制使用手动参数：

```python
AUTO_DETECT_PLOT_ROI = False
STARTUP_AUTO_CONTRAST = False
MANUAL_EXPOSURE_US = 2000
MANUAL_GAIN_DB = 0.0
MANUAL_CONTRAST = 2
TRACE_THRESHOLD = 190
```

曝光应逐步调整，不要一次降低太多；目标是让轨迹保持明亮，同时让
屏幕背景、网格和边框明显变暗。

## 需要优先标定的参数

| 参数 | 作用 | 调整建议 |
|---|---|---|
| `AUTO_DETECT_PLOT_ROI` | 自动寻找绘图区 | 默认开启；OpenMV M4 不支持时使用备用 ROI |
| `STARTUP_AUTO_CONTRAST` | 启动曝光/对比度搜索 | 默认开启 |
| `ANALYSIS_ROI` | 自动检测失败时的备用 ROI | 排除边框、菜单和右侧光标 |
| `TRACE_THRESHOLD` | 自动标定失败时的备用阈值 | 高于网格，低于轨迹暗边 |
| `MANUAL_EXPOSURE_US` | 固定曝光时间 | 过曝时尝试 1500～3000 μs |
| `MANUAL_GAIN_DB` | 固定增益 | 拍摄亮屏时可从 0 dB 开始 |
| `MANUAL_CONTRAST` | 固定传感器对比度 | `None` 表示自动扫描 |
| `SWAP_XY_AXES` | 交换相机图像 X/Y | 相机旋转 90° 时设为 `True` |
| `MERGE_MARGIN` | 连接轨迹小断点 | LCD 扫描造成断线时从 3 增至 4~5 |
| `LINE_ROUNDNESS_MAX` | 1:1 直线阈值 | 误判二倍时减小 |
| `PARABOLA_CENTROID_SKEW_MIN` | 2:1 抛物线偏心阈值 | 漏判二倍时小幅减小 |

若需要把包围框宽高换算为真实电压幅度，应分别测量示波器 X/Y 每格在图像中的像素数，并设置：

```python
X_PIXELS_PER_UNIT = 24.5
Y_PIXELS_PER_UNIT = 23.8
```

输出中的 `amplitude_ratio_x1000` 表示：

```text
(X包围宽度 / X_PIXELS_PER_UNIT) /
(Y包围高度 / Y_PIXELS_PER_UNIT) * 1000
```

## 输出协议

默认每 200 ms 通过 IDE/USB 标准输出一帧 ASCII 数据。启用 UART 时，将 `ENABLE_UART` 改为 `True`，并按实际开发板修改 `UART_ID`。

```text
$XY,seq,stage,result,confidence,nx,ny,roundness_x1000,
skew_x_x1000,skew_y_x1000,amplitude_relation,
amplitude_ratio_x1000,fps_x10*CS
```

实际数据在同一行，例如：

```text
$XY,37,2,5,87,1,1,901,103,8,3,1006,426*3B
```

`CS` 是从 `XY` 开始到最后一个字段的逐字节 XOR 校验值。结果代码如下：

| 代码 | 含义 |
|---:|---|
| 0 | 未检测到有效轨迹 |
| 1 | 正在判断/置信度不足 |
| 2 | `fX > fY` |
| 3 | `fX < fY` |
| 4 | `fX ≈ fY` |
| 5 | `fX ≈ 2*fY` |
| 6 | `fY ≈ 2*fX` |

幅度关系字段中，`0` 表示未知，`1` 表示 X 幅度大于 Y，`2`
表示 X 幅度小于 Y，`3` 表示两者在设定容差内近似相等。

## 性能设置

热路径扫描绘图区中的 14 条窄探针。第一阶段的频率方向已经明确时，
直接跳过 `find_blobs()`；只有关系模糊、需要第二阶段形状判断时才
调用一次原生 blob 检测。程序没有全帧 Python 像素循环、图像复制、
全帧二值化、形态学或 Hough 变换。

需要进一步提高帧率时，按以下顺序调整：

1. 缩小 `ANALYSIS_ROI`；
2. 保持 `DEBUG_DRAW = False`；
3. 增大 `BLOB_X_STRIDE/BLOB_Y_STRIDE`，但细轨迹可能漏检；
4. 将 `FRAME_SIZE` 改成 `csi.QQVGA`，随后重新标定 ROI、跨度和阈值；
5. 降低输出频率，增大 `REPORT_INTERVAL_MS`。

`STARTUP_AUTO_CONTRAST = True` 时，搜索结束后会锁定曝光、增益和
对比度，避免亮轨迹面积变化导致阈值和形状参数随自动曝光抖动。

## 适用边界

- 第一阶段允许 ROI 略微裁掉轨迹的最外侧极值，但中央 20%～80%
  探针范围必须覆盖有效轨迹；第二阶段仍应尽量保留完整的单线形状。
- 示波器网格和文字必须比轨迹暗。如果同样明亮，灰度阈值无法可靠分离，应改用示波器颜色设置、遮罩/更小 ROI，或进一步做 RGB565 颜色阈值版本。
- 二阶段优先等待直线或抛物线诊断形状；若形成多线叠加的密集网状
  图形，则根据多帧 X/Y 复杂度平衡判定近 `1:1`，不再无限等待
  单一轨迹。
- 摄像头透视畸变、严重反光、轨迹余辉过强或多条通道同时显示都会降低可靠性。

## 两张实拍图的预期结果

第一张高频差图像中，旧版默认 ROI 和阈值 170 会把过曝区域、屏幕边框和轨迹
合并成约 44240 像素的大块，因此旧逻辑无法找到合格轨迹。

在对应绘图区内，7 条中央垂直探针的交点
总数约为 12，水平探针约为 61，平均显示约为 `nx=2、ny=9`。
因此稳定三帧后应输出：

```text
stage=1
result=3        # FX_LT_FY，即 fX < fY
```

第二张频率相近的密集交叉图，在备用 ROI `(68, 40, 170, 166)`、
阈值 170 下，垂直探针总数约为 80，水平探针约为 87；提高阈值后
仍保持接近平衡。因此程序会先进入第二阶段，连续确认后输出：

```text
stage=2
result=4        # FX_APPROX_FY，即 fX ≈ fY
```

相关官方资料：

- [OpenMV 固件 5.0.0 破坏性 API 变更](https://docs.openmv.io/v5.0.0/changelog/firmware/v5.0.0.html)
- [OpenMV `csi` 相机接口](https://docs.openmv.io/v5.0.0/library/omv.csi.html)
- [OpenMV 图像 Blob API](https://docs.openmv.io/v5.0.0/library/omv.image.Blob.html)
- [OpenMV 矩形检测 API](https://docs.openmv.io/v5.0.0/library/omv.image.Rect.html)
