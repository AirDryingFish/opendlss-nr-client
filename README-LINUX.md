# OpenDLSS-NR 原生 Linux 客户端

在 Linux/X11 上复用 OpenDLSS-NR 的 Filament 渲染器和原生 FP8 Tensor Core 路径。
模型渲染、运动向量、NR 网络和时间合成留在同一个 Vulkan 设备中运行。
这不是 WebCuda 浏览器版，也不会将不支持 NR 的普通预览标为 NR 输出。

## 本机运行条件

本项目针对 NVIDIA Ada（计算能力 8.9）及更新 GPU。当前测试机器为 RTX 4080 SUPER。
启动前运行：

```sh
./scripts/check-gpu.sh
./scripts/check-gpu.sh --json
```

退出码 `0` 表示前置能力检查通过，`2` 表示能力不足，`1` 表示检查程序或 Vulkan 加载失败。
检查通过还不等于网络已运行或已达到目标帧率。

2026-11-11 本机驱动已升级至 **NVIDIA 595.99.02**；RTX 4080 SUPER 的实际检查结果为 `supported: true`，原生 NR 已运行并完成下述基准。
不要仅按驱动版本号判断；更换环境后重新运行检查。程序不会自动更换驱动或使用 CPU/浏览器替代后端。

检查器核实 `VK_EXT_shader_float8`，以及 cooperative matrix、CUDA kernel launch、shader clock、SM builtins 等扩展，以及原生客户端实际请求的 Vulkan 特性和带时间戳的图形/计算队列。
操作系统仍需提供 NVIDIA Vulkan 驱动、Vulkan loader、X11 及常规桌面运行库。
Linux 入口固定使用 SDL X11 后端；Wayland 原生窗口不属于此版本的支持范围。

## 启动和导入

以下命令从项目目录运行；启动脚本本身也可使用绝对路径从其他目录调用。

```sh
./scripts/run-client.sh
./scripts/run-client.sh "/完整路径/模型.glb"
./scripts/run-client.sh "/完整路径/模型.gltf" "/完整路径/环境.hdr"
./scripts/run-client.sh "/完整路径/模型.glb" --width 1024 --height 1024
```

默认打开 `assets/test-static.glb`，加载 `models/nr` 的本地权重，请求初始窗口为 1024×1024。
`--width` 和 `--height` 只指定初始窗口尺寸；窗口管理器确定的实际客户区尺寸决定场景和 NR 的分辨率。启动脚本接受 64–8192 像素，直接启动窗口程序时接受 64–16384 像素。
自定义权重目录可通过 `--model DIR` 或 `DLSS5VK_MODEL` 指定。
glTF 的外部缓冲区、图片需位于它引用的位置。
首版导入会实际校验 PNG/JPEG 贴图；KTX2 贴图暂不接受，请先转换为 PNG/JPEG。
不支持或损坏的资产会显示错误，并保留当前已加载的场景。
窗口内支持文件路径导入、拖放、轨道旋转、缩放、平移和重新取景，以及 NR/时间历史开关。
实际控制方式和快捷键以窗口中的操作提示为准。

在 **Scene → Background color** 中使用 sRGB 拾色器选择纯色背景；修改颜色会关闭 HDR 背景显示，不改变 IBL 环境照明。**HDR background** 复选框或 `B` 键可切换 HDR 背景。
直接导入或未显式指定 MSAA 的场景默认使用 **Scene MSAA 4×**；场景配置及界面仍可选择 1×（off）、2×、4×、8×。MSAA 仅增加场景采样，NR 分辨率仍跟随实际窗口。它可减少几何边缘的采样闪烁，透明混合的排序问题仍可能产生闪烁。

拖动窗口边缘或最大化后，场景、NR 和界面会自动跟随实际窗口尺寸，无需手动应用分辨率。窗口最小为 64×64；最小化时暂停渲染和尺寸资源重建，恢复时同步窗口尺寸并重置时间历史。相同尺寸不会重复重建资源。
已通过 6 次窗口尺寸请求和 12 次 NR 开关合成点击；包含横屏、竖屏、重复尺寸，以及 2048×2048 被桌面限制为 2048×1403 的情况。场景、NR、原生窗口、SDL 和 ImGui 尺寸一致；记录见 [窗口跟随验证](docs/verification/window-follow-2026-11-11.json)。此前 Apply 路径的记录保留为旧版验证。
已修复某些视角下头发被皮肤覆盖、后脑裸露的问题：普通 PBR/透明材质使用正常绘制通道，只有声明 transmission/volume 的材质才进入折射通道。截图角度的 NR 开/关对照和 24 种真实材质匹配组合均通过验证，见 [遮挡验证](docs/verification/head-occlusion-2026-11-11.json)。

**已打开的旧版客户端需关闭后重新启动**，才能使用更新后的程序。

## 测量实际 NR 性能

```sh
mkdir -p reports-local
./scripts/run-client.sh --width 1024 --height 1024 --nr 1 --temporal 1 \
  --benchmark-warmup 30 --benchmark-frames 120 \
  --benchmark-output "$PWD/reports-local/1024-benchmark.json"
```

基准分别记录网络 GPU 耗时和整帧时间的中位数、P95，以及设备/驱动、实际窗口分辨率和 NR 状态。测量期间请保持窗口尺寸不变；调整窗口或最小化会使基准退出。
验收基准要求默认内核路由；改变 PTX、融合、链式执行等路由的 `DLSS5VK_*` 环境变量会使基准明确退出。普通查看仍可使用这些实验路由，窗口将显示自定义后端及相关变量。
正常帧保留流水执行；不会为取时间戳而逐帧强制等待 GPU。
过期或重复的 GPU 时间戳不计为新样本。导出/截图路径不属于正常逐帧渲染。
没有实际运行网络、样本不足或 NR 关闭时，不得据结果宣称已经达到 NR 的实时性能。
RTX 5080 的上游数据仅为参考，本机成绩以实际生成的报告为准。

以下性能报告均早于材质路由修复，保留为历史基准。

此前于 2026-11-11 在 RTX 4080 SUPER / NVIDIA 595.99.02 上以实际 1024×1024、NR 和 temporal 均开启运行，预热 30 帧并取得 120 个样本。网络 GPU 中位数为 **4.22961426 ms**，整帧 GPU 中位数为 **4.73266602 ms**，整帧 wall time 中位数为 **6.864666 ms**、P95 为 **7.796201 ms**。原始报告见 [1024 基准 JSON](docs/verification/1024-benchmark-2026-11-11.json)。
背景调色与默认 4× MSAA 更新后，1024×1024、NR/temporal 开启、30 帧预热及 120 个样本的实测中位数：网络 GPU **4.16 ms**，整帧 GPU **4.84 ms**，整帧 wall time **6.87 ms**（P95 **7.87 ms**）。报告见 [4× MSAA 基准](docs/verification/1024-msaa4-benchmark-2026-11-11.json)。

此前已实际完成准备后的静态资产加载、原生 NR 开/关截图和旧版分辨率切换。运行中重新加载资产及导入失败后保留原场景的恢复行为仍待单独进行运行验收。

## 测试人物为何另有一个 GLB

此前的 `test-fixed.glb` 已修复 Face/Body 的透明模式，但其 skin 声明 1,185 个骨骼，脸部使用 512 个，超过 Filament 的 256 骨骼上限。
该资产没有动画或 morph，因此本项目生成了保留当前姿态的静态副本 `assets/test-static.glb`。
它正确烘焙了蒙皮位置和法线，而不是直接删除蒙皮；121,968 个顶点的世界坐标最大分量误差约为 `7.63e-8`。
23 张嵌入贴图、材质、UV、顶点颜色和索引保持不变。原文件和此前的修复版未修改。

其他兼容的动画 GLB 使用上游骨骼动画功能。超过 256 骨骼的导入会明确报错，不会静默丢弃动画。
上游的透明头发、眼睛和睫毛没有独立的物体运动向量，运动时沿用其时间处理限制。

## 重新准备权重和资产

权重不属于开源许可证范围，不会随 Git 源码发布。当前本地目录已使用用户提供的兼容 DLL 静态提取。
提取器只读取 PE 资源，不执行 DLL。它要求新的输出目录，防止覆盖已有权重。

```sh
node scripts/model/scripts/extract-model.mjs "/路径/nvngx_dlssnr.dll" "/新的模型目录"
python3 scripts/bake_static_glb.py "/路径/无动画.glb" "/新的静态副本.glb"
```

静态烘焙工具需要 NumPy，且拒绝包含动画或 morph 的输入。
本机可使用已带 NumPy 的 Codex Python runtime；源码工具也可在单独的 Python 虚拟环境中安装 NumPy后运行。
原生加载器需要 71 个网络块、153 个张量、`manifest.json` 和 `model/stage-N.bin`；不同结构的新模型不自动兼容。
原生模型加载时验证各阶段的长度和 SHA-256。

## 从源码构建

```sh
NR_BUILD_JOBS=4 ./scripts/linux/build.sh
```

首次构建会下载固定版本的 Vulkan-Headers、volk、glslang、Filament 以及所需的用户目录编译器依赖。
依赖与缓存默认位于 `${XDG_CACHE_HOME:-$HOME/.cache}/opendlss-nr-client/` 下的 `native-build-deps` 和 `native-build`。
可设置 `NR_DEPS_DIR`、`NR_BUILD_DIR`、`NR_BUILD_JOBS` 和 `NR_PYTHON`；依赖和缓存不应放入源码交付目录。
脚本不使用 sudo，不全局安装工具链，也不更换显卡驱动。
自动依赖下载和本地头文件修复针对 Ubuntu 24.04 amd64，需要可用的 apt 包索引；其他发行版尚未验证，单独提供 `CC/CXX` 不保证构建成功。
本地 LLVM 只在固定包全部解压并通过编译、链接及运行检查后启用；中断后的重试会复用缓存并修复不完整安装。

输出包括 `build/demo/dlss5-demo`、`build/dlss5vk`、`build/nr-doctor`、SPIR-V/PTX 和材质资源。
本机的系统 `bits/syscall.h` 存在损坏；构建脚本使用用户目录内提取的干净头文件覆盖路径，未修改系统头文件。
启动时需要保留整个 `build` 目录、`assets` 和本地 `models`，不能只复制单个可执行文件。
LLVM C++ 运行库及其版权说明随本地构建放在 `build/lib` 和 `build/licenses/llvm`。

## 来源

- OpenDLSS-NR：`9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1`，保留仓库中的 MIT LICENSE 和 NOTICE。
- Filament：v1.77.0，使用上游随附补丁及本地材质路由修复补丁，许可证 Apache-2.0。
- 本地权重来自用户提供的 RTX Remix 1.6.1 DLL，不纳入开源源码许可。
- 模型提取器出处和许可证见 `scripts/model/README.md`。
