# obs-colorwheel 架构与实现说明

> 独立插件，骨架对齐 `obs-curves`，为后续合并预留接口。
> 目标环境：OBS 32.2.2 x64 / Windows 10+ / MinGW-w64 (w64devkit, GCC 16.2.0)

---

## 0. 当前状态（一句话）

**已构建、已安装、已在 OBS 里加载成功，12/12 数学自测通过，编辑器窗口经离线测试台实测可打开并渲染。**
尚未完成的是视觉打磨与几处交互细节（见 §7）。

| 产物 | 大小 | 说明 |
|---|---|---|
| `obs-colorwheel.dll` | **129,085 B** | 滤镜 + 数学引擎 + D2D 编辑器，一个 DLL |
| `colorwheel.effect` | 3,917 B | 着色器，纯 uniform 数学 |
| `zh-CN.ini` / `en-US.ini` | ~40 B | 模块名本地化 |

安装位置：

```
C:\ProgramData\obs-studio\plugins\obs-colorwheel\
├─ bin\64bit\obs-colorwheel.dll
└─ data\{effects\colorwheel.effect, locale\*.ini}
```

---

## 1. 三个关键设计决定

### 1.1 不用 LUT —— 这是和 obs-curves 最重要的一处分歧

obs-curves 的核心数据通路是「10 条曲线 → 烘焙 1024×16 RGBA32F LUT → shader 查表」。色轮**不沿用**这条路：

- Lift / Gamma / Gain / Offset / 对比度 全是逐通道一维函数，shader 里一次 `pow` 加几次乘加即可，比采样纹理更便宜；
- 更重要的是，它**彻底消灭了一整类 bug**。obs-curves 踩过的两次：§17 恒等斜坡写成 `x/255.0f`（1024 列时 v 到 4.0 → 整帧过曝）、§21 LUT 已是 1024 但 shader 仍按 256 采样（2 texel 偏移）。两者都是「LUT 尺寸与采样公式不同源」。
- 顺带：改参数**零烘焙延迟**，拖动即时生效；少一张 256 KB 的浮点纹理。

代价：色轮的输出不能像曲线那样被编辑器直接读回来画成曲线。用 `grade_eval_channel()` 在 CPU 上重算同一公式即可（同一份代码，见 §2）。

### 1.2 用 16F 中间缓冲，并因此必须关掉 direct rendering

`obs_source_process_filter_begin(ctx, format, allow_direct)` 的语义容易被误解。实测与 OBS 源码一致：

> 当这个滤镜是链上**唯一/最靠近源**的滤镜、父源是普通同步源、且允许直出时，OBS 走 **bypass 路径**——此时 `format` **被完全忽略**，shader 直接跑在父源纹理上。

所以「传 `GS_RGBA16F` 就有 16F」是错的。本插件显式：

```c
const enum gs_color_space space = GS_CS_SRGB_16F;
const enum gs_color_format format = gs_get_format_from_space(space); // GS_RGBA16F
obs_source_process_filter_begin_with_color_space(ctx, format, space, OBS_NO_DIRECT_RENDERING);
```

用 `_with_color_space` 而不是 `begin()`，因为 `begin()` 把色彩空间硬编码成 `GS_CS_SRGB`，会和 16F 格式自相矛盾。

**不加 `OBS_SOURCE_SRGB`**：调色必须在 sRGB 编码值上做；加了会把曲线作用到线性值上，整体偏亮。

### 1.3 编辑器沿用 obs-curves 的形态

```
WS_POPUP | WS_THICKFRAME + 自绘标题栏（WM_NCCALCSIZE→0、WM_NCHITTEST→HTCAPTION/缩放边）
└─ D3D11 device → IDXGISwapChain1(FLIP_SEQUENTIAL, 2 buffers)
   └─ ID2D1DeviceContext → DirectWrite
      └─ 四个色轮 + 数值，全部自绘；没有 COMBOBOX、没有子窗口、没有图片资源
```

色轮**不是每帧画渐变**：一次 CPU 逐像素烘焙成 BGRA 位图，只在尺寸/DPI 变化时重建，每帧只 `DrawBitmap` 一次。四个轮子共用同一张图。这既保证解析抗锯齿，又让每帧成本接近零。

---

## 2. 数学核心（`grade_engine.c`）

全部以「中性锚点」的 UI 值存储，和 Resolve 面板一致：

```
暗部 Lift   0.00    中灰 Gamma  0.00    亮部 Gain  1.00    偏移 Offset 25.00
对比度 1.000   轴心 0.435   饱和度 50.00   色相 50.00   亮度混合 100.00
```

处理链（顺序固定）：

```
1. offset     c += (O - 25) * 0.01
2. lift/gain  c = c*(K - L) + L         黑点→L，白点→K
3. gamma      c = sign(c)*|c|^(1/(1+G))
4. contrast   c = pivot*(c/pivot)^C     精确钉住轴心
5. 阴影/高光   亮度加权的近似（中性 0 = 恒等）
6. 饱和度/色相  Rec.709 Y'CbCr 平面旋转 + 保亮度色度增益
7. 亮度混合    c += (Yin - Yout)*(1-M)
```

两个细节是刻意的：

- **`spow` 带符号幂**：调色后数据会跑到负值，`pow` 遇负底数返回 NaN，整帧变黑。
- **饱和度走 `c = y + s*(c-y)`**，不用 HSV 的 `S += (y-0.5)*2`。后者正是 obs-curves §21 的 bug：灰像素 `S=0` 加饱和度会直接变红，减到负数反相成粉色。

色轮盘 → RGB 用**余弦瓣**：`Δᵢ = cos(θ - 2πi/3) · r · k`，三瓣之和恒为 0，所以推色相**不改变亮度**。

### 自测（`grade_test.c`，12/12 通过）

```
PASS  P1 neutral == identity          PASS  P1b grade_is_neutral
PASS  P2 lift: black -> 0.1           PASS  P2 lift: white stays 1
PASS  P3 gain 2: 0.5 -> 1.0           PASS  P3 gain 2: black stays 0
PASS  P4 gamma exp2: 0.5 -> 0.7071    PASS  P5 contrast pins pivot 0.435
PASS  P6 wheel deltas sum to 0        PASS  P7 sat 0 -> luma grey
PASS  P7b sat 50 -> identity          PASS  P8 hue round trip
```

`P1` 用 1e-6 容差卡死「中性 = 恒等」；`P8` 因为 float32 精度，往返残差约 5e-5（远小于 1 个 8-bit LSB），容差放到 2e-3 并注明原因。

---

## 3. 与 obs-curves 的接口对齐（为合并预留）

`colorwheel_editor.h` 是照 `curves_editor.h` 的形状写的：

| obs-curves | obs-colorwheel |
|---|---|
| `curves_editor_host { filter, curves, enabled, source_name, target_*, get_*, apply, mark_dirty, mark_all_dirty }` | `colorwheel_editor_host { filter, params, source_name, target_*, get_*, apply, mark_dirty }` |
| `obs_curves_editor_open()` | `obs_colorwheel_editor_open()` |
| `obs_curves_editor_on_filter_destroyed()` → bool | `obs_colorwheel_editor_on_filter_destroyed()` → bool |

`on_filter_destroyed` 返回 `false` 时滤镜**故意泄漏**——因为编辑器还持有指针，释放比让 UI 线程访问野指针安全。这条规则原样继承。

合并时只需：在现有编辑器里加一个页签，调用 `obs_colorwheel_editor_open` 的同构入口；LUT 不用合（色轮没有 LUT），只需要共享 D2D 设备和字体资源。

构建也复用同一套工具链：

```powershell
$shared = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-curves\native'   # w64devkit + OBS 32.2.2 头文件
native\build_dll.ps1
```

---

## 4. 设置键（扁平、可版本化）

```
pg_liftR/G/B   pg_gammaR/G/B   pg_gainR/G/B   pg_offR/G/B
pg_contrast  pg_pivot  pg_sat  pg_hue  pg_lummix  pg_boost  pg_shadows  pg_highlights
```

`pg_` 前缀与 obs-curves 的 `cN_*` 不冲突，两个插件可以同时挂在一个源上。

---

## 5. 实时预览是怎么保证的

链路（已按 OBS 源码核对）：

```
拖动 → 只在内存里改 params → mark_dirty 回调把 pending 拷进 render 结构
     → InvalidateRect → 下一帧 video_render 读到新值 → 立即上屏
松手 → apply 回调 → 写 obs_data → obs_source_update → 下一帧 tick 触发 update
```

三个刻意的选择：

1. **拖动中不写 settings**。每个鼠标事件都 `obs_source_update` 会跟渲染线程抢锁、重建滤镜状态——obs-curves §8.2 就是这么卡起来的。本插件鼠标移动只更新内存并合并到 16 ms 定时器，松手才提交。
2. **不读 `obs_source_get_settings()` 的返回值当拷贝**。它是同一个对象，改值不等于通知；必须显式 `obs_source_update`。
3. **`update` 里只做「settings → 结构体」，`video_render` 里每帧无条件重设 uniform**。参数不落地到渲染态是「滑杆动了画面不动」的头号原因。

另有：**中性时直接 `obs_source_skip_video_filter()`**，一次全屏 pass 都不跑，同时保证「中性 == 逐像素恒等」可被验证。

---

## 6. 实测记录（以及它抓出来的 bug）

### 6.1 已通过

| 项目 | 证据 |
|---|---|
| 编译链接 | `obs-colorwheel.dll` 129,085 B，SHA-256 见构建输出 |
| 数学 | `grade_test.exe` 12/12 |
| OBS 加载 | 日志 `Loaded Modules: obs-colorwheel.dll`，无 `Failed to initialize module` |
| 编辑器窗口 | 离线测试台 `test_editor.exe` 实测 `window created`，截图见 `editor_shot4.png` |

### 6.2 测试过程抓出的 5 个真 bug

这些都是「编译通过」掩盖不了的，值得记住：

1. **模态 MessageBox 冻结 UI 线程**
   最初 D3D 初始化失败时弹 `MessageBoxW`。这在 OBS 主线程上等于冻结整个应用。离线测试台因此连续两次挂死。改为 `OutputDebugStringW` + 静默返回。
   **教训：插件里任何路径都不能弹模态框。**

2. **`FEATURE_LEVEL_11_1` 让 `D3D11CreateDevice` 返回 `E_INVALIDARG`**
   在不支持 D3D11.1 运行时的机器上，把 11_1 放进特性等级数组会让整个调用失败。这就是「窗口根本不出现」的真因。修法是失败后用不含 11_1 的数组重试。

3. **`hsv_to_rgb` 不处理负色相 → 色轮变成 6 个硬边楔形**
   `atan2` 一半圆返回负角，`fmodf` 后索引为负，落到 `switch` 的 default 分支。截图一眼就能看出来，但代码看着「没错」。
   修法：`h = h - floor(h)` 归一化到 `[0,1)` 再取索引。

4. **`swprintf` 的 `%s` 在这个工具链上按窄字符串解释**
   宽格式里传宽字符串，只输出第一个字符（`"Test Source..."` → `"T"`）。必须用 `%ls`。

5. **`FLIP_DISCARD` + 两缓冲 → 未重绘的缓冲被显示**
   表现为窗口上残留碎片。换 `FLIP_SEQUENTIAL` 后消失（这也是色轮 UI 设计文档给出的建议）。

### 6.3 编辑器当前长相

![编辑器实测截图](editor_shot4.png)

900×380，四个色轮 + 名称 + 三通道读数 + 底部操作提示 + 右上关闭按钮。

---

## 7. 已知问题 / 下一步

### 7.1 必须修

| # | 问题 | 说明 |
|---|---|---|
| 1 | 每个轮子标签右端有 4 处蓝色虚线碎片 | 绘制残留，定位在标签矩形右缘，怀疑退化文本矩形或笔刷状态未复位 |
| 2 | 缺 Master（第 4 个）读数 | Resolve 每个轮子是 R/G/B + Master 四个数值，现在只有三个 |
| 3 | 外环的 Master 指示点没有画 | 现在只能拖环，看不到当前 master 位置 |
| 4 | 没有 GDI 回退 | obs-curves 有；本插件 D3D 失败就直接没有编辑器（滤镜仍可用） |

### 7.2 视觉打磨（对照设计稿）

现在的界面是「能看」而不是「好看」。按设计稿还差：

- 每轮加圆角卡片底 + 1px 描边，把轮子「嵌」进去
- 外环倒角（内外缘压暗 28%）、外投影
- 冷调灰配色（`#1D1F26 / #272A33 / #3C404D`，注意 B 通道比 R 高 9–17；中性灰在 OBS 里一眼假）
- 数值改等宽字体 + 右对齐，避免拖动时抖
- 右上角补最小化按钮，标题栏可双击最大化

### 7.3 功能

- 撤销/重做、预设管理
- 滚轮微调（`WM_MOUSEWHEEL` 的坐标是**屏幕坐标**，必须先 `ScreenToClient`）
- Shift 精调（要记锚点做增量，不能直接赋值，否则一按 Shift 就跳）
- 125% / 150% DPI 与 HDR 真机验收

---

## 8. 建议的实施顺序

1. **先修 7.1 的 4 项**，尤其是碎片和 Master 读数——这是正确性问题，不是审美问题。
2. **再按 7.2 打磨一轮**，对照设计稿逐项对齐配色与层次。
3. 然后补撤销/重做与预设。
4. 最后才考虑与 obs-curves 合并成双页签。

合并本身很轻（ABI 已经对齐），但它会把两个插件的发布节奏绑在一起；建议等色轮自己稳定一两个版本再合。
