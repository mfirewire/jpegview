# JPEGView v1.3.46-20261108.1

基于 `fix/review-1300182-aa26097`（**不合 master**，先发 release），是 `v1.3.46-20261108` 之后的稳定版，自 `5ee5374` 起共 6 笔提交。版本号沿用 1.3.46，About 页显示 `1.3.46-20261108.1`。

头条：这一轮把**颜色开关、剪贴板输入、文件改名**三条容易出错的路径补齐了。PNG 的 gAMA/sRGB 现在跟随 `UseEmbeddedColorProfiles`，关掉开关时文件 gamma 不再偷偷生效；粘贴进来的 CF_DIB 会先按 `GlobalSize` 核对头、调色板和像素区，畸形块直接拒绝而不是越界读；批量改名改为"先整批预检、再执行、失败可回滚"，单文件重命名也会先校验非法字符、保留设备名和空标题——旧版空标题会把图片改名成 `.jpg`（实测改名成了 `.png`）。

同批收尾：JP2 有损压缩比可配、alpha 白底合成、参考网格回绕拦截；HDR 负/NaN 样本不再进未定义的浮点转整数；>50 MB 的 JPEG 也能无损旋转；EXIF 缩略图重建失败时丢弃旧缩略图；OCR 退化词框与选区绘制、资源释放、INI 默认值对齐一并处理。

---

## 下载

| 构建 | 说明 |
| --- | --- |
| **JPEGView-x64-v1.3.46-20261108.1.zip** | x64 Release：`JPEGView.exe`、依赖 DLL、Config 文件 |
| **JPEGView-win32-v1.3.46-20261108.1.zip** | Win32 Release，内容同上 |

解压即用，无需安装。

## 本版验证

- **真机 x64**（`ccd2e3d` 构建 + 本版 INI）：`JP2LossyRate` 1/10/100 输出 1,001,634 / 91,874 / 9,163 B 单调递减；`MaxImagePixelsMP=10` 拒 108 MP、默认注释掉则正常打开；WIC 64 MP 可开、280 MP 仍被 DLL 的 256 MP 硬上限拒；84 MB JPEG 无损旋转成功；批量改名冲突时 0 文件改动、中途失败选 No 后 `1 file operation(s) rolled back.`；`a:b.jpg`、`CON.jpg`、空标题三种重命名全部被拒；注入 `inf`/`nan`/溢出的 INI 值不崩。
- **A/B 反证**（修复前构建）：84 MB 旋转报 `Reading the JPEG file from disk failed`；批量改名失败后残留 `p1_new.jpg`；重命名空标题把源图改名成 `.png`。三处旧缺陷均在新版消失。
- **17 场景全量 UI 回归**：快速翻页、加载失败、空目录、跨目录打开、加载中改窗口、幻灯片、最大化/全屏、失焦恢复、动画、剪贴板粘贴全部通过；0 次黑条、0 崩溃，`ONPAINT_DIBNULL=0` / `ONPAINT_CATCH=0`。
- **CI**：`c65109a`、`ccd2e3d`、`d34dd5a` 三个收尾提交的 x64 与 Win32 构建均双绿。
- **未覆盖**：HDR 负 float 与 EXIF 缩略图重建失败路径（黑盒构造不出触发条件，代码已逐行审）、OCR 词框交互、Win32 真机（仅 CI 编译通过，沿用上一版这一限制）。

## 更新内容

New features:

- 有损 JP2 压缩比可用 INI 键 `JP2LossyRate` 配置（1..100，默认 10，即历史 10:1），无损 JP2 不受影响
- 像素上限 `MaxImagePixelsMP` 默认关闭；硬字节上限 `MAX_IMAGE_BYTES` 与单边 65535 限制仍在，设 1..4096 可恢复显式上限。注意 WIC-only 格式（`*.wdp/*.jxr/*.dds`）仍受 WICLoader.dll 固定 256 MP 限制，该设置到不了那个 DLL
- 四条 UI 消息补齐英文、简体中文、繁体中文字符串表

Bugs fixed:

- PNG 的 gAMA/sRGB 现在跟随 `UseEmbeddedColorProfiles`；关闭开关时按原始值显示，带 iCCP 的文件也不会同时丢 profile 和 gamma
- 粘贴 CF_DIB 前校验 `biSize`、`GlobalSize`、调色板和像素区，负宽度或过短的数据块被拒绝
- JP2 / PAM 的 alpha 改为白底合成；JP2 组件数上限 8；参考网格 `x1<=x0` / `y1<=y0` 在减法回绕之前拒绝
- HDR 解码的负值/NaN 不再进入未定义的 float→int 转换（与 EXR 一致，钳到 0）
- 无损 JPEG 变换改用加载器的文件上限（x64 300 MB / Win32 50 MB），不再被私有的 50 MB 卡住
- 全尺寸 JPEG 另存时 EXIF 缩略图重建失败会丢弃旧缩略图，避免 Explorer 显示上一张图
- 单文件重命名校验非法字符、保留设备名、尾点尾空格和空标题；批量改名整批预检（非法名/重复目标/目标已存在），冲突时 0 改动，中途失败选 No 逆序回滚
- OCR：退化词框不再处处命中；选区改为一次 GDI+ 填充 + 文字按框裁剪带省略号；跨行选择保留中间空行
- 资源与线程：壁纸注册表句柄关闭；共享 ICC profile 用 `call_once` 只建一次；帮助/打印对象退出时释放
- 目录监听：裸盘符 `C:` 归一化到根目录；超过 MAX_PATH 的路径打日志而不是静默截断
- 无损保存开关按格式分别记忆，存无损 JP2 不会再让下一次 WebP/JXL 保存对话框默认无损

Changed behavior:

- 像素上限默认关闭（见上），INI 与代码默认值对齐：`SlideShowEffectTime=250`、`LandscapeModeParams` 补第 12 个值、`FileNameFormat` 双空格
- 参数库命令的文案从 "delete from parameter DB" 更正为 "save"；PBM 黑白注释更正

Engineering:

- 本版先做全量代码审核（P3 决策逐条对照），再做真机重点回归与修复前构建 A/B 反证；报告见仓库内的 `JPEGView-ccd2e3d-P3落地核对.md` 与 `JPEGView-ccd2e3d-真机回归报告.md`
- 两份 INI、两份 `MaxImageDef.h` 注释与 CHANGELOG 与 B2/A1 的实际行为对齐（仅文档，不改逻辑）
