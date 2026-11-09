# JPEGView v1.3.46-20261109.1

基于 `fix/review-1300182-aa26097`（**不合 master**，先发 release），是 `v1.3.46-20261108.1` 之后的稳定版，自 `19897de` 起共 21 笔提交。版本号沿用 1.3.46，About 页显示 `1.3.46-20261109.1`。

头条：**JPEG 2000 的 CMYK 文件现在能看了**。此前 `JP2Reader` 遇到 `OPJ_CLRSPC_CMYK` 直接拒绝——`.jpx` / `.jpf` / `.j2k` 一律进空态，与扩展名无关。现在按 4 个墨量组件解码，优先用随包的 `cmyk.icm` 做色彩转换，profile 缺失时才回退朴素公式。

同批收尾：关窗期由加载线程投递的消息不再触到已释放的 provider；目录监视线程停不下来时整个对象泄漏而不是释放它仍在使用的锁和事件；参数库单例改在 UI 线程、任何 worker 之前构造；TGA 的 RLE 计数器改 `size_t`；菜单里被门禁拦下的项现在真的置灰，且不再连累「转换为 JXL」。

---

## 下载

| 构建 | 说明 |
| --- | --- |
| **JPEGView-x64-v1.3.46-20261109.1.zip** | x64 Release：`JPEGView.exe`、依赖 DLL、Config 文件 |
| **JPEGView-win32-v1.3.46-20261109.1.zip** | Win32 Release，内容同上 |

解压即用，无需安装。

## 本版验证

- **一键回归**（本版新增 `_test/regress/run_all.py`，真实样本来自 `D:\Downloads\samples`）：38 通过 / 0 失败 / 5 跳过。覆盖 CMYK PSD（RLE + 内嵌 ICC）、CMYK JP2 的三个扩展名、无 APP14 的 CMYK JPEG、MPF gain map、8000×14、20 余种格式冒烟、畸形 BMP/EXIF、JP2 三档保存、L-D04、单实例、ESC 窗口矩形。
- **17 场景全量 UI 回归**：快速翻页、加载失败、空目录、跨目录打开、加载中改窗口、幻灯片、最大化/全屏、失焦恢复、动画、剪贴板粘贴全部通过；0 次黑条、0 崩溃，`ONPAINT_DIBNULL=0` / `ONPAINT_CATCH=0`。
- **CMYK 色标实测**：`cmyk_chart_rswop.psd`（1200×600，RLE，218 KB SWOP profile）显示为正确的青/品红/黄/黑/紫/红/白/灰阶；同一内容的 `.jpx`、`.jpf`、`.j2k` 均能打开，色标一致（JPX 用 `cmyk.icm`、PSD 用内嵌 profile，个别色块有轻微差异）。
- **CI**：`36282cb`、`bc80cf4`、`c54504c` 三次推送共 9 个 job，x64 / Win32 / EOL guard 全部 success。
- **未覆盖**：`.jpx` 的 Part 2 多 codestream（本样本是单 codestream 的 JP2 容器）；CMYK JP2 的内嵌 ICC（`colr` 为枚举类型，没有 profile 可读）；Win32 真机仍只有 CI 编译。

## 更新内容

New features:

- JPEG 2000：CMYK 文件（`colr` enumCS 12）现在能解码显示，走随包的 `cmyk.icm`，profile 不可用时回退朴素墨量公式；组件数不是 4 的 CMYK 仍拒绝
- `.jpx` / `.jpf` 加入可浏览扩展名与打开对话框过滤器（加载器本来就按内容识别，是文件列表把它们过滤掉了）

Bugs fixed:

- 关窗：加载线程在 `SetEvent` 之后投递的完成消息，不会再落到已 delete 的 provider 上（处理器与 watcher 的文件变更处理器都以 provider 为空作为关机判据）
- 目录监听：线程等不到时不再 `TerminateThread` 后删掉它仍在用的临界区和事件，改为线程自报"已收工"，超时则整个对象泄漏
- 参数库：单例改在 `OnInitDialog`、任何 worker 创建之前构造，两个加载线程不会再各建一份
- 幻灯片列表里两个不同目录的同名文件，Reload / 排序后不再跳到第一个同名项；带路径且未命中时进缺失态而不是猜
- 内存中旋转后执行无损 JPEG 变换，不再静默丢掉未保存的旋转
- GDI+ 解码 JPEG 时补回 EXIF、文件哈希、保留的 APP/COM 段和真实色度采样，无损变换与裁剪的 MCU 判断不再按 4:2:0 猜
- 动画 WebP/APNG/JXL：把请求的帧号传给解码器，切回文件或动画回绕时从第 0 帧开始，而不是接着上次缓存的下一帧
- APNG 坏帧：`setjmp` 失败后立即清掉 libpng 的错误态缓存，并避免复制指向已销毁 `info` 的 EXIF 指针
- ICC 变换接受显式输出行距并拒绝小于一行的值，平面尺寸按 64 位校验
- 直方图：白点等于黑点的退化直方图不再除零把 NaN 送进 LUT
- 文件类型注册：DACL 重建的 `!AceType == ACCESS_DENIED_ACE_TYPE` 恒为假，导致整个键的权限被替换成单条 allow
- 文件列表：`CreateFile` 失败按 `INVALID_HANDLE_VALUE` 判断，不可读文件不再被当成可读
- 剪贴板：只接受 BI_RGB / BI_BITFIELDS，RLE、JPEG/PNG 压缩与 BI_ALPHABITFIELDS 在进入 GDI+ 之前拒绝
- EXIF：条带式（未压缩）缩略图在保存时丢弃，不再保留上一张图
- 批量改名/复制：正斜杠绝对路径能识别；回滚删除本次创建的整条目录链，且不会上溯到链外
- 无损裁剪：命令本身拒绝非 JPEG 与已破坏性编辑的图像，不只是菜单项
- 黑条：用户命令触发整体重载时的 `ClearAllRequests` 等待纳入"等待图像"守卫，第四个入口不再闪黑
- 窗口位置：ESC 或退出命令也会保存，不只有标题栏关闭
- 按钮绘制：不再每次变暗泄漏一个 GDI 位图
- 单实例：无图状态（标题带 dev 戳）的运行实例能被再次找到并转发文件
- INI：`UnsharpMaskParameters` / `ColorCorrection` / `LandscapeModeParameters` 里的 `nan` / `inf` 被拒绝
- 缩放导航器、EXIF 面板、帮助窗口、导航面板动画的 `GetDC` 全部与 `ReleaseDC` 配对，不再泄漏设备上下文
- 退出时释放第二组图像参数、打印对话框与打印参数

Engineering:

- 新增 `_test/samples/real/`（真实工具产物 + `manifest.json`）与 `_test/regress/run_all.py`、`pre_release.py`
- EOL 守门的"新增文本未配属性"断言现在真的会触发；`KeyMap.txt.default` 与 `symbols.km` 纳入文本策略
- Win32 配置加 `/LARGEADDRESSAWARE`；WICLoader 工程与主工程的 `/EHa` 对齐
- 发布 workflow 在 Create Release 前校验正文首行与两个附件名是否匹配 tag

Changed behavior:

- 无 ICC 的 CMYK PSD 与 no-APP14 的 CMYK JPEG 改用朴素墨量公式，不再把 K 当 alpha 叠黑
- README 的图像尺寸一节描述当前默认（像素上限默认关闭，字节与 65535 单边限制仍在）
