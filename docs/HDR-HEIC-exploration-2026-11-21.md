# HDR HEIC 探索记录（2026-11-21）

分支：`fix/review-1300182-aa26097`。本文记录 2026-11-21 一天内围绕 iPhone HDR HEIC（gain map）做的探索：一次失败的真机测试、一次速度优化、一场标定、一个已定论的单位问题、一条用户定的规则、一次仍未解决的过亮问题，以及最终决定——先全部按 SDR 显示，调查暂停。

本文是记录，不是总结陈词：失败和未解决的问题都如实写在这里，数字都来自实测或真实文件。

## 1. 背景：N1 问题——Batch 7 的推导规则真机测试失败

HDR 修复轮的 Batch 7（`1653ceb`）给没有 headroom 元数据的 Apple gain map 加了一条推导规则：

```
headroom = 3.0 × (p99.9 / 255)，钳制在 [0, 3.0] 档
```

其中 p99.9 是 gain map 字节的 99.9 百分位，3.0 档是物理上限（log2(1600/203) ≈ 2.98）。两张真实 iPhone HEIC 上的推导值：2026-06-07 那张 3.000 档，2026-04-05 那张 1.329 档。

**2026-11-21 用户真机测试失败**，两点：

1. **高光太亮**：两张图的高光都明显过亮。
2. **打开变慢很多**：打开时间从 1 秒不到变成 8–10 秒。

诊断（代码层面已确认）：

- **推导规则高估了**。gain map 只编码相对增益——p99.9 = 255 的意思是“这张照片用满了自己的增益范围”，不是“峰值是 1600 尼特”。3.0 档是物理上限，不是这张照片的实际值；一张傍晚拍的照片（06-07，19:13）峰值几乎不可能到上限。连 1.329 档那张也过亮，说明错的是规则的尺度，不只是上限。
- **慢，是因为 gain map 在 Batch 7 之前其实从未真正生效过**。`DecodeGainMapGray` 在 Batch 7 之前就跑了，但 XMP 解析失败就返回 NULL，应用路径从没走过。Batch 7 补上推导后，4032×3024（1219 万像素）第一次走了完整应用路径：36MB RGB 临时拷贝 + 12MB 双线性上采样后的 gain + 146MB 线性浮点缓冲 + 48MB BGRA 输出（约 244MB 临时内存），`ApplyGainMap` 逐像素 pow + 全分辨率色调映射，全部单线程。gain 本身的 HEVC 解码开销不是新增的。

用户补充的标定信息（2026-11-21 ~14:25、14:33）：两张图修之前（即 gain 未生效时）颜色“基本正常”；Apple Photos 里这两张与修之前显示基本一致，肉眼无区别；但在 Photos 里看上去确实是 HDR 在显示。合起来读：**Photos 是在应用 gain map 的，只是真正的 headroom 很小**——小到正常观看下几乎与 SDR 无异，远低于 3.000 / 1.329 档。

## 2. 速度：LUT 优化（`b11451a`），只省了约 1.2 秒

跟进工作分两条线。速度线（`b11451a`，`perf: LUT-ize ApplyGainMap (N1-A)`）已落地：

- `ApplyGainMap` 查表化：两张 256 项的栈上表（gain 字节 → boost；sRGB 字节 → 线性值）。**精确查表，不是近似**（输入都是字节）。4032×3024 上 `ApplyGainMap` 从 1.915 秒降到 0.256 秒（7.5 倍），pow 调用从 60,963,840 次降到 757 次；输出字节完全一致（全分辨率线性校验和与 BGRA 校验和不变），新测试 `TestApplyGainMapLutExact` 用直接 pow 的参考实现逐项钉住 LUT 输出（float 精确相等）。
- **但只省了用户 7–9 秒里的约 1.2 秒**。实测的 pow 总量是 32 次/像素（约 3.9 亿次），不是之前代码阅读估计的约 20 次/像素（约 2.44 亿次）：色调映射 `ConvertLinearImageToBgra` 一项就是 27 次/像素——其中 12 次来自 EETF 内部的反归一化往返（`Bt2390EetfPqWithParams` 结尾的 `PqNitsToSignal(PqSignalToNits(...))`，ks < 1 时每通道 4 次 pow），这是估计时漏掉的部分。按本机约 37ns/次 pow 算约 14.4 秒，与用户机器上 7–9 秒的量级一致。色调映射那一遍（12.303 秒）没动过，总耗时约 14.2 秒 → 约 12.7 秒。
- **色调映射快速路径被证明不安全，没有采用**。思路是峰值 ≤ 1.0 时跳过 EETF——密集扫描 50,560 例，**24,138 例结果不同（47.7%）**：峰值 ≤ 1.001 时 0 例不同，峰值 1.1 时 5.9%、1.5 时 31%、2.5 时 57%、8 时 82%。机制已确认：EETF 膝点 ks = 1.5·maxLum − 0.5 依赖图像峰值，峰值 > 1 时低于 1.0 的通道仍会被滚降，峰值条件是承重的，不能拿掉。`HdrToneMap.cpp` 未改动。
- 后续候选（**未开始**，需用户点头）：把色调映射里的 PQ 往返查表化——输入是连续 double 不是字节，需要先定量化误差界，不像字节 LUT 那样精确。

## 3. 标定：没有改规则（证据不够）

标定线用了 kennylevinsen 的 UltraHDR JPEG（ISO 21496-1 真值已知），完整分析在 `~/workspace/.scratch-n1-calib/calibration.md`：

- 标定集是退化的：两张图的真值 gainMapMax 都是 **0.100000**（gamma 1.0），无法拟合出有区分度的规则。
- 结构性结论：**gainMapMax 是编码器的 headroom 常量，不是最大像素处的增益**（实际最大 log 增益只有 gainMapMax 的 41% / 10%）——所以任何像素统计量都无法还原它。
- 拟合质量：没有统计量能给出一致的 k——p99.9 的 k ∈ [0.24, 1.02]（4.20 倍 spread）；p90 误差 0.115 档；均值拟合最好（误差 0.060 档）但概念上脆弱；p50 退化（HLG 那张 p50 = 0）。
- 当时领先的候选是重标定的 p99.9（k = 0.6314 → 0.631 / 0.280 档），但 k 只是 bracket 出来的中点，不是确立的值。**这条线没有改规则**——随后用户自己提出了下面的规则，标定候选作废。

## 4. P2-A2 定论：`HDRGainMapHeadroom` 是线性比值，不是档数

用户问能不能从网上找一张带 `HDRGainMapHeadroom` 的 iPhone HEIC。找到了：

- **文件**：`~/workspace/.scratch-headroom-heic/iphone_15_pro.heic`（886,825 字节，sha256 `c68e438f92341da271a731a059113db9395366f4c2f78ead5ded64deba7de463`），来源 https://github.com/photoprism/photoprism/blob/develop/assets/samples/iphone_15_pro.heic。iPhone 15 Pro，iOS 17.1，2023:10:31 11:44:43，主图 3024×4032，gain aux item 51，1512×2016。XMP：`HDRGainMapHeadroom` **3.847906**，`HDRGainMapVersion` 131072。gain 统计：min 110 / max 156 / mean 121.40 / p99.9 151——窄带分布，和我们那两张（mean 约 26–32，范围 0–255）完全不同。
- **决定性证据**：同一文件的 MakerNote 两个字段 0x21 = 1.010000、0x30 = 1.184557，按苹果文档公式算出 1.944079 档 → headroom = 2^1.944079 = **3.847921**，与 XMP 的 3.847906 差 0.000015（0.0004%）——两个独立的元数据只有在 XMP 按比值理解时才对得上。旁证：PIXL Engine（"headroom tag is a ratio, not stops"）、苹果文档的应用公式 `hdr = sdr·(1+(headroom−1)·gainmap)`（headroom 直接当乘数）、storytold/craft-libs、libavif 的 `hdrHeadroomLinear`。
- **正确读法**：`gainMapMax = log2(3.847906) = 1.9441` 档（最大 3.85 倍）。旧代码把该字段直接当档数，在这张图上会算成 3.847906 档（14.40 倍），**高估 3.74 倍**。
- **备份文件**：libultrahdr 的 `apple_gainmap_new.jpg`（iPhone 13 mini，Headroom 4.532783 → 2.1804 档；按档数理解会是 23.15 倍，高估 5.11 倍）；反例 `apple_gainmap_old.jpg`（Version 65536，**没有** Headroom——老格式，和我们那两张一个 pattern）。
- 本地排查均为阴性：两张 `.scratch-heic` 文件 0 次、kennylevinsen 全集 0 次、libheif 的 `example.heic` 0 次。

## 5. 用户的规则及实现（`3be9945`）

用户提议：“没有 headroom 就不应用 gain、直接显示 SDR；有 headroom 直接应用 headroom 值可不可以？”验证后确认可行，单位按比值取 log2。实现在 `3be9945`（CI 全绿）：

- **推导路径删除**：`DeriveAppleHeadroomFromGain`、`ScanAppleGainMapXmp`、`XmpHasAppleGainMapVersion` / `XmpHasAppleGainMapHeadroom`、`Metadata::derivedHeadroom` 全部删除，src/ 与 tests/ 中零残留；`appleHeadroomFallback` 保留（解析路径仍设置它）。
- **单位修正**：`ParseXmpMetadata` 的 Apple 分支现在 `gainMapMax = log2(ratio)`，比值校验为有限且 ∈ [1.0, 65536.0]（0–16 档）；比值 < 1.0 拒绝（HDR 比 SDR 暗无物理意义），== 1.0 接受为 identity（gainMapMax 0.0），> 65536 拒绝（16 档界与 Batch 3 的 `ApplyGainMap` clamp 一致，解析期拒绝 + clamp 留作纵深防御）。`HDRGainMapGamma` 处理不变（无 Gamma 时默认 1.0）。ISO `hdrgm` 路径未动（ISO 的 gainMapMax 按规范本来就是 log2 档数）。
- **三份真实 XMP 验证全部通过**：iPhone 15 Pro HEIC XMP（444 字节，逐字节取自文件）→ gainMapMax = 1.94407355612712（期望 log2(3.847906)）；Apple JPEG XMP（726 字节）→ 2.18039709607554（期望 log2(4.532783)）；两份 version-only XMP（06-07、04-05）→ 解析失败 → SDR。旧值探针：2.0 → 1.0、1.5 → 0.584962500721156；0.5 / 131072 / 1e6 / inf 拒绝，65536 → 16.0 接受。
- **测试**：1167 项，0 失败（`3be9945`；基线修正：`b11451a` 的 gainmap_test 是 182 项不是 168 项，LUT 汇报的数字是旧数）。CHANGELOG：Batch 7 的推导条目原地替换为新规则，不是另起一条自相矛盾的条目。
- **MakerNote 的 headroom 刻意没用**：见第 8 节第 (2) 条。
- 苹果线性应用公式与共享指数 `ApplyGainMap` 的中段差异**没有实现**单独应用路径（无强证据），已作为后续事项记在代码注释里。

## 6. 用户的两张 HEIC

用户上传的 `~/workspace/user/files/B7-1_iphone_heic.7z` 解出两张原片（`~/workspace/.scratch-b7-1-heic/`）。注意：用户先传的两张 `.jpg` 是普通 SDR JPEG（单 SOI、无 MPF、无 XMP、无 EXIF——上传时转码清掉了 gain map 和元数据），不能用；原片是这个 7z。

| 文件 | 机型 / 系统 / 拍摄 | Version | Headroom | gain aux | gain 统计 |
|---|---|---|---|---|---|
| iphone_13_pro_max.HEIC（2,182,707 字节，sha256 `e760c80eed310e4f27c092d5487693ca8e104e7cc01d25ba4828deb28f679676`） | iPhone 13 Pro Max / iOS 15.2.1 / 2022:02:16 | 65536 | 无 | item 51，1512×2016 | min 0 / max 255 / mean 37.33 / p99.9 249 |
| iphone_15_pro_ios18.HEIC（3,151,842 字节，sha256 `4b887e6bc345191cbb9836602373b3ec454558ccc62d3518041e085117238fe0`） | iPhone 15 Pro / iOS 18.0 / 2024:06:16 | 131072 | **2.769181** → gainMapMax 1.4695 档 | item 64，1512×2016 | min 93 / max 255 / mean 136.87 / p99.9 249 |

两张主图都是 3024×4032。

**规律（5 张真实文件全部符合）**：`HDRGainMapVersion` 65536（v1）→ 没有 Headroom；131072（v2）→ 有 Headroom。**iOS 版本不决定有没有**（iOS 18.0 这张有，iOS 18.6.2 的两张没有）。机型相关性目前也成立（iPhone 13 → v1，iPhone 15 Pro → v2），但只有两个机型，只能算提示，不能算证实。

旧推导规则在这张 iOS 18 文件上会算出 3.0×249/255 = 2.929 档，真值 1.4695 档——高估一倍，进一步确认推导是两个方向都错。

## 7. 真机测试 2026-11-21 ~16:15：iOS 18 那张还是太亮，PhotoPrism 那张正常

用户在新规则 head（`3be9945`）上的结果：**iphone_15_pro_ios18.HEIC 高光还是太亮；iphone_15_pro.heic（PhotoPrism）显示正常**。

Muse 用 JPEGView 自己的代码在 Linux 上把两条管线完整复现了一遍（真实 `ApplyGainMap` + `ComputePeakLinear` + `ConvertLinearImageToBgra`；复现工具在 `~/workspace/.scratch-b7-1-heic/sim_tonemap.cpp`、`dump_gain.c`，输出 `ios18_tonemapped.ppm`、`pp_tonemapped.ppm`）：

- **iOS 18（栀子花）**：增益后峰值线性值 2.753，**3.28% 的像素顶到纯白 255**，花瓣是一片死白的色块（SDR 原图花瓣细节都在）；≥250 的像素 9.64%，≥240 的 16.22%。
- **PhotoPrism**：峰值线性值 1.402，顶到 255 的只有 **0.01%**，显示正常。

**原因是内容，不是 headroom，也不是公式**：

- iOS 18 这张有 2.43% 的像素本来就接近白色（底图 ≥240，白花瓣），提 2.45 倍后到 2.26 线性值；增益后 16.84% 的像素超过 SDR 白（>1.0），8.02% 超过 1.5，3.45% 超过 2.0。
- PhotoPrism 那张**没有任何**底图像素 ≥240，峰值才 1.479 线性值——它平均提得反而更多（1.90 倍对 1.74 倍），但没爆，所以看着正常。
- 苹果自己的线性公式会**更亮**，不是更暗（iOS 18：超 SDR 白的像素从 16.84% 涨到 19.74%；PhotoPrism：从 6.97% 涨到 46.41%），所以换公式不是修复方向。
- 没有竞争元数据：主图 XMP 里 0 个 GainMap / hdrgm 字段，代码读的是 gain 项的 XMP（Headroom 2.769181），路径正确；gain 解码也验证过（RGB 回退，R==G==B）。
- headroom 1.4695 档本身是对的（第 4 节的比值规则 + MakerNote 交叉验证），错的是“这么亮的底图 × 正确的增益”在 SDR 屏幕上呈现的效果。

## 8. 未解决的问题

(1) **Apple Photos 里 iOS 18 这张到底什么样？** 花瓣有细节还是也一片白？以及 Photos 是在什么屏幕上看的（iPhone/Mac 的 HDR 屏，还是 Windows 这台 SDR 屏）？如果 Photos 在 HDR 屏上花瓣有细节，那差别就是显示器——HDR 屏能直接显示 2.26 的亮度带细节，SDR 屏必须压缩，不是 JPEGView 的 bug。如果 Photos 在 SDR 屏上花瓣也有细节，JPEGView 的色调映射就得再查。**这个问题在写本文时没有答案。**

(2) **MakerNote 与 Photos 的矛盾现在更尖锐了，没解决**。MakerNote 公式本身已验证（在 PhotoPrism 文件上对到 0.0004%），但 04-05 那张按公式是 8.00 倍，Photos 却没这么亮。**不要拿 MakerNote 的值当 gain map 的尺度**。为什么 Photos 不体现这个尺度（可能 Photos 只用 XMP headroom、缺失时回落；或那些文件的 gain map 归一化方式不同），原因不明。

(3) **高峰值 gain map 图像的 SDR 色调映射**。BT.2390 是全局曲线：峰值 2.753 时，16.8% 超白的像素被挤进 8-bit 顶端，花瓣纹理被压进几个色阶里。是否需要更温和的高光处理（以及那会不会偏离 BT.2390），没有定论。

(4) **速度**。色调映射那一遍（27 pow/像素）仍是 gain map 打开慢的主因；PQ 往返查表化是候选方案，未开始（需要先定量化误差界）。

## 9. 决定（用户，2026-11-21 ~16:38）

**先不查了，全部 HEIC 暂时按 SDR 显示；调查暂停。** 本次提交执行该决定：

- **范围说明（协调人的决定，请用户纠正）**：只停 HEIF。用户讨论的是手上这些iPhone HDR HEIC；JPEG UltraHDR 与 AVIF 的 gain map 之前已真机验证通过（Batch 3 通过；kennylevinsen 样片与参考图一致），用户也没有抱怨它们——**不动**。如果用户的本意是全部格式都停，请纠正。
- 实现：`TryApplyHeifGainMap`（`src/JPEGView/HEIFWrapper.cpp`）顶部直接返回 NULL，所有 HEIC 显示 SDR；gain map 代码、解析器、测试全部保留（临时停用，不是删除）。恢复调查时删掉这个 early return 即可。
- 解析器里的 headroom 规则（无 headroom → SDR；有 headroom → 比值取 log2）保持不变，JPEG/AVIF 路径继续用它。
- 测试新增：第三份真实 XMP（iOS 18，Headroom 2.769181 → gainMapMax 1.469459354632348）已加进 `TestAppleHeadroomRatioRealXmp`（+5 项），常量与文件里的 444 字节逐字节一致；全套测试 1172 项，0 失败。

## 10. 文件清单

**样片**（都在本机 scratch 目录，不在仓库里）：

| 文件 | 位置 | 大小 / 校验 |
|---|---|---|
| iphone_15_pro.heic（PhotoPrism，Headroom 3.847906） | `~/workspace/.scratch-headroom-heic/iphone_15_pro.heic` | 886,825 字节，sha256 `c68e438f92341da271a731a059113db9395366f4c2f78ead5ded64deba7de463` |
| apple_gainmap_new.jpg（Headroom 4.532783） | `~/workspace/.scratch-headroom-heic/apple_gainmap_new.jpg` | 50,824 字节，sha256 `492a94bd0636bcf15b3f560c68142a7783936c1597e78b0d0461d8be7fddc078` |
| apple_gainmap_old.jpg（无 Headroom） | `~/workspace/.scratch-headroom-heic/apple_gainmap_old.jpg` | 50,742 字节 |
| iphone_13_pro_max.HEIC（无 Headroom） | `~/workspace/.scratch-b7-1-heic/iphone_13_pro_max.HEIC` | 2,182,707 字节，sha256 `e760c80eed310e4f27c092d5487693ca8e104e7cc01d25ba4828deb28f679676` |
| iphone_15_pro_ios18.HEIC（Headroom 2.769181） | `~/workspace/.scratch-b7-1-heic/iphone_15_pro_ios18.HEIC` | 3,151,842 字节，sha256 `4b887e6bc345191cbb9836602373b3ec454558ccc62d3518041e085117238fe0` |
| B7-1_iphone_heic.7z（用户发来的原片包） | `~/workspace/user/files/B7-1_iphone_heic.7z` | 5,323,753 字节 |
| 2026-06-07 19.13.11.HEIC（无 Headroom） | `~/workspace/.scratch-heic/2026-06-07 19.13.11.HEIC` | 4,378,613 字节，md5 `4d7a53a40a87530604834e0c7bea6a40` |
| 2026-04-05 16.21.46.HEIC（无 Headroom） | `~/workspace/.scratch-heic/2026-04-05 16.21.46.HEIC` | 2,586,757 字节，md5 `649d0714c8f9c8d3fa3444228837c2b2` |

**复现与分析工具**（本机 scratch，不在仓库里）：`~/workspace/.scratch-b7-1-heic/sim_tonemap.cpp`（JPEGView 真实 gain + 色调映射管线的 Linux 复现）、`dump_gain.c`（libheif 解码主图与 gain map）、`ios18_tonemapped.ppm` / `pp_tonemapped.ppm`（复现输出）、`~/workspace/.scratch-n1-calib/calibration.md`（标定分析）。

**相关 head**（分支 `fix/review-1300182-aa26097`）：`b11451a`（LUT 提速）、`3be9945`（headroom 规则：无 headroom → SDR，有 headroom → 比值取 log2）、本次提交（HEIF gain map 临时停用 + 第三份真实 XMP 测试 + 本文档）。
