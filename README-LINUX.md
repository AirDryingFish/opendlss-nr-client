# Linux 配置、故障排查与性能测量

首次安装请从 [README 的构建步骤](README.md#1-安装并构建客户端) 开始。本页列出可选的 CUDA 工具链、诊断命令和 Debug 操作。

## 安装 NVIDIA 硬件光流工具链

Ubuntu 24.04 x86_64 可使用 NVIDIA 的 apt 仓库安装 CUDA Toolkit 12.8。机器上已有兼容的 `nvcc` 时，确认 `nvcc --version` 能成功执行，再重新构建和检查。显卡驱动单独安装；运行 `nvidia-smi` 可查看当前驱动和设备。[NVIDIA Linux 安装指南](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html) · [NVIDIA 光流引擎](https://developer.nvidia.com/optical-flow-sdk)

```sh
cuda_keyring="${TMPDIR:-/tmp}/cuda-keyring_1.1-1_all.deb"
curl -fL --retry 3 -o "$cuda_keyring" \
  https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i "$cuda_keyring"
sudo apt update
sudo apt install -y cuda-toolkit-12-8
export PATH="/usr/local/cuda-12.8/bin:$PATH"
nvcc --version
./scripts/linux/build.sh
./scripts/check-features.py --only flow
```

光流后端从原始采集帧估算运动，使用驱动的光流硬件和本地构建的 CUDA 插值内核。要与 NR 组合，先分别运行 `--only flow nr` 的检查，再在 Live Settings 中同时启用两项。RIFE 和 MAKO 使用各自的 Vulkan 实现，可单独安装。

## 定位启动和运行问题

| 现象 | 检查方法和处理方式 |
| --- | --- |
| `--only build` 报缺少 pkg-config 模块 | 按 README 安装开发包，Filament 构建要求 CMake ≥ 3.22.1。Live 的 Dear ImGui 面板需要系统 SDL2 ≥ 2.0.19，使用 `pkg-config --modversion sdl2` 检查。 |
| 选择页打不开，窗口或选区黑屏 | 从要捕获的桌面终端运行 `echo "$XDG_SESSION_TYPE $DISPLAY"` 和 `./scripts/check-features.py --only chooser window region`。确认登录 Xorg、具有当前 X server 的访问权限，窗口采集启用了 XComposite。Wayland 原生屏幕和应用窗口不在此捕获路径的范围内。 |
| 选择 NR 或 Debug 后提示 GPU 不支持 | 运行 `./scripts/check-gpu.sh --json` 查看缺失的扩展、特性或队列，必要时更新驱动并重启。该命令退出码 `0` 表示 GPU 前置条件通过，`2` 为能力不足，`1` 为检查程序或 Vulkan 加载失败。实际推理用 `./scripts/check-features.py --only nr` 验证。 |
| 缺少 `manifest.json`，或阶段长度/校验值不匹配 | 从 README 指定版本的 DLL 提取到新目录（默认 `models/nr`）。自定义位置可设置 `DLSS5VK_MODEL`，或传 `--model /完整路径`。Live 原画与独立插帧可用 `--nr 0` 启动。 |
| 后端能列出，开始插帧却失败 | 运行 `./scripts/check-features.py --only flow rife mako --multiplier 4`，只保留所用后端。列表反映安装情况，检查会实际创建会话并处理测试帧。旧 RIFE/MAKO 插件能运行 2×，3×/4× 需要重新运行对应的 `prepare-*.sh`。 |
| MAKO 在某台 Vulkan GPU 上失败 | 检查驱动的外部内存/信号量 FD 和时间线信号量支持。v1.24 在 RTX 4080 SUPER 上可生成画面，但验证层曾报告其着色器使用 52,188 字节共享内存，超过设备报告的 49,152 字节；应在目标驱动实测。 |
| 显示倍率正确，但观看时未达到目标 FPS | 在 Settings 中分别看 Capture、Present 和 VSync。窗口模式在源最小化时暂停，区域模式与预览重叠时暂停。源帧重复、切镜、处理时间超出预算和显示器刷新率都影响最终结果。用相同输入关闭 NR 或降低倍率，可分别比较单项开销。 |

`--only region window` 在桌面左上角创建 32×32 临时测试窗口，读取两个颜色并比对。`--only debug` 打开 320×240 查看器，渲染内置模型三帧后退出。NR、光流、RIFE、MAKO 的单项 GPU 测试使用程序生成的输入，无需屏幕捕获。

## 构建缓存与文件布局

| 路径或环境变量 | 用途 |
| --- | --- |
| `build/demo/` | 构建得到的启动程序、Live 预览与 Debug 查看器 |
| `build/{data,lib,ptx,shaders}/` | 材质、着色器、运行库、安装的插帧插件及其模型 |
| `assets/` | 仓库中的 GLB、测试场景及四个 HDR 预设 |
| `models/nr/` | 用户提供的 NR 权重；含 71 个网络块、153 个张量、11 个阶段，加载时校验阶段长度和 SHA-256 |
| `NR_DEPS_DIR` / `NR_BUILD_DIR` | 下载依赖和中间编译目录。默认在 `${XDG_CACHE_HOME:-$HOME/.cache}/opendlss-nr-client/` 下的 `native-build-deps` 和 `native-build` |
| `NR_BUILD_JOBS` / `NR_PYTHON` | 编译并行任务数和生成工具使用的 Python 解释器 |

首次构建会获取固定版本的 Vulkan-Headers、volk、glslang、Filament 和补丁。使用系统 Clang ≥17 和 libc++，或在 Ubuntu 24.04 amd64 上由脚本下载用户目录工具链。编译中断可重跑相同命令。迁移构建产物时保留整个 `build/`、启动脚本、`assets/` 和本地 NR 权重的相对布局。

## Debug 模型和场景

可拖入 GLB/glTF、用 **Browse…** 选取外部模型，或从命令行启动。独立 glTF 应带齐引用的图片和缓冲区。支持 PNG/JPEG 贴图，KTX2 纹理需先转换；单个蒙皮的骨骼数量上限为 256。仓库人物使用已烘焙的静态姿态，含 121,968 个顶点和 23 张嵌入贴图。Fox 可用于检查骨骼动画。[资产与相机配置](assets/README.md) · [写实场景来源](assets/realistic-scenes/README.md)

```sh
./scripts/run-client.sh --debug assets/test-static.glb
./scripts/run-client.sh --debug assets/test-static.glb --width 1280 --height 720
./scripts/run-client.sh --debug --environment-preset entrance-hall
```

默认请求 1024×1024 窗口，场景与 NR 都随实际客户区尺寸变化。相邻的 `view.json` 为相应模型提供初始相机和照明。左键旋转、中键平移、滚轮缩放；右键自由观察，WASD/QE 移动，Shift 加速。**N** 切换 NR，**T** 切换其时间历史，**B** 切换 HDR 背景。**Scene → Background color** 设定纯色背景并保留环境照明。**Scene MSAA** 默认 4×；其采样数独立于 NR 分辨率。[窗口尺寸验证](docs/verification/window-follow-2026-11-11.json) · [HDR 环境](assets/README.md#环境照明)

## 测量目标机器的性能

```sh
mkdir -p reports-local
./scripts/run-client.sh --debug --width 1024 --height 1024 --nr 1 --temporal 1 \
  --benchmark-warmup 30 --benchmark-frames 120 \
  --benchmark-output "$PWD/reports-local/1024-benchmark.json"
```

测量期间保持窗口大小不变。报告含设备、驱动、实际分辨率、网络 GPU、整帧 GPU 和整帧 wall time 的中位数与 P95。之前的 RTX 4080 SUPER / 595.99.02 在 1024×1024、4× MSAA 下，网络 GPU 中位数为 4.16 ms，整帧 wall time 中位数为 6.87 ms。这是较早的 3D 场景记录，Live 的采集、插帧与显示使用独立路径。[原始基准 JSON](docs/verification/1024-msaa4-benchmark-2026-11-11.json)

早期 1080p、2× 的独立插帧测量中，主存 RGBA 输入到主存输出的耗时中位数为 MAKO 3.47 ms、NVIDIA 光流 3.63 ms、RIFE 13.59 ms（RTX 4080 SUPER，各 40 次）。要测完整 Live 流程，应使用相同源、分辨率和输入帧率，在 Settings 中分别观察各倍率的实际采集与提交率。

在已启用 NVIDIA 光流的机器上，可用以下命令复测 30 FPS 输入在不同刷新率下的表现。测试捕获 480×320 的移动窗口，开启 4×，分别模拟 60 Hz 和 120 Hz 的显示时钟，输出真实帧和生成帧各自的每秒提交数。

```sh
sudo apt install -y xvfb xauth python3-tk
/usr/bin/python3 tests/live_cadence.py
```
