# 测试资产

## 默认人物

`test-static.glb` 是用户提供的人物测试模型的静态烘焙副本，作为客户端默认场景随仓库保存，使用普通 Git 文件，不依赖 Git LFS。它不包含 NR 权重。

| 项目 | 内容 |
| --- | --- |
| 文件大小 | 61,583,972 字节（约 61.6 MB / 58.7 MiB） |
| 格式 | glTF 2.0 二进制 GLB |
| 顶点 | 121,968 |
| 材质 / 嵌入贴图 | 9 / 23 |
| 动画 / 蒙皮 / morph | 无 |
| SHA-256 | `90ea40a9c98084312351cb4b3e37a84f7b34ee3aa0adbe7194af477cc531a389` |

原始测试模型包含超过 Filament 256 骨骼上限的蒙皮，且没有动画或 morph。本副本使用 [bake_static_glb.py](../scripts/bake_static_glb.py) 将当前姿态的位置和法线正确烘焙为静态几何；保留材质、UV、顶点颜色、索引及 23 张嵌入贴图。烘焙前后的顶点世界坐标最大分量误差约 `7.63e-8`。原始文件不随本仓库附带。

如需为自己的无动画、无 morph 模型生成静态副本，先在 Python 环境中安装 NumPy，再运行：

```sh
python3 scripts/bake_static_glb.py "/path/to/input.glb" "/path/to/new-static.glb"
```

模型来源为用户提供的测试资产，仓库未附其独立许可文件；源码的 MIT 许可证不用于声明该模型的授权。

## 公开测试模型

`cases/` 保存以下 [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) 的 GLB 版本，固定于提交 [`8f492390ddc7a2ae221a1369ef560589cf8c70fb`](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb)。模型文件仅在本地改名为 `Model.glb`，内容未改动。每个目录保留上游说明、元数据、许可文件和 `SOURCE.json`；[manifest.json](cases/manifest.json) 记录文件大小、SHA-256 与下载地址。完整许可文本见 [CC0 1.0](cases/LICENSES/CC0-1.0.txt) 和 [CC BY 4.0](cases/LICENSES/CC-BY-4.0.txt)。这些资产采用各自的许可证，不属于源码的 MIT 许可范围；上游元文档由 KhronosGroup/glTF-Sample-Assets 贡献者按 CC BY 4.0 提供。

| 模型 | 检查内容 | 本地文件 | 来源与许可 |
| --- | --- | --- | --- |
| Lantern | 法线贴图、金属与粗糙度 | [lantern/Model.glb](cases/lantern/Model.glb) | [上游](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/Lantern)；Microsoft / sbtron、Frank Galligan，CC0 |
| Fox | 蒙皮与骨骼动画 | [fox/Model.glb](cases/fox/Model.glb) | [上游](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/Fox)；PixelMannen 模型 CC0，tomkranis 绑定与动画、AsoboStudio / scurest 转换，CC BY 4.0 |
| AlphaBlendModeTest | OPAQUE、MASK 与 BLEND 透明模式 | [alpha-blend-mode-test/Model.glb](cases/alpha-blend-mode-test/Model.glb) | [上游](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/AlphaBlendModeTest)；Analytical Graphics, Inc. / Ed Mackey，CC BY 4.0 |
| TransmissionTest | `KHR_materials_transmission` 透射材质 | [transmission-test/Model.glb](cases/transmission-test/Model.glb) | [上游](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/TransmissionTest)；Adobe，CC0 |

在仓库根目录运行，模型使用与默认人物相同的本地 NR 权重：

```sh
./scripts/run-client.sh assets/cases/lantern/Model.glb --nr 1
./scripts/run-client.sh assets/cases/fox/Model.glb --nr 1 --animation 2
./scripts/run-client.sh assets/cases/alpha-blend-mode-test/Model.glb --nr 1
./scripts/run-client.sh assets/cases/transmission-test/Model.glb --nr 1
```

随模型保存的 `view.json` 设置初始相机与环境图，以上命令会自动使用它们。Fox 可在窗口的动画控件中切换片段。[主 README](../README.md#效果) 提供全部公开模型与默认人物正面、侧面、背面的 OpenDLSS-NR 开关对比：每行左侧 NR OFF、右侧 NR ON（`temporal=1`），每对使用相同模型、材质、相机、照明与 960×720 分辨率，动画固定在同一帧。逐对配置见[对比记录](../docs/verification/nr-comparisons-2026-11-11.json)。

运行 `./scripts/run-client.sh --scenes assets/cases` 可在 **Model → Assets / scene** 下拉框中切换随附案例。

## 写实视觉测试场景

`realistic-scenes/` 增加三组写实资产，GLB 合计 **92,906,576 字节（约 92.9 MB）**，均采用 CC BY 4.0，使用普通 Git 文件保存。A Beautiful Game 和 Dish 保留官方原始 GLB 字节；**Gothic 仅新增绕 X 轴旋转 −90° 的场景父节点，将地面方向校正为 Y-up，原有节点和完整 BIN 数据块不变**。随附 `view.json` 是本项目新增的相机和显示配置。完整作者、来源、几何统计、原文件与派生文件的校验值见[场景说明](realistic-scenes/README.md)和 [manifest.json](realistic-scenes/manifest.json)。

| 场景 | 检查内容 | 来源与作者 |
| --- | --- | --- |
| [A Beautiful Game](realistic-scenes/a-beautiful-game/Model.glb) | 木质棋盘、密集棋子、PBR 与透射材质 | [Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ABeautifulGame)；Moeen Sayed、Mujtaba Sayed，ASWF / MaterialX，Chris Rydalch、Jonathan Stone、Ed Mackey |
| [Gothic Cloister Corner](realistic-scenes/gothic-cloister-corner/Model.glb) | 建筑摄影测量、拱廊、石材与植被 | [Babylon.js 官方资产](https://github.com/BabylonJS/Assets/tree/3798844710bb7e76199ee38b175176c0fe4e9e62/meshes/GothicCloisterCorner)；Zoilo |
| [Iridescent Dish with Olives](realistic-scenes/iridescent-dish-with-olives/Model.glb) | 真实产品参考、法线贴图、PBR、透射与盖子动画 | [Khronos](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/IridescentDishWithOlives)；Wayfair, LLC / Eric Chadwick |

```sh
./scripts/run-client.sh assets/realistic-scenes/a-beautiful-game/Model.glb --nr 1
./scripts/run-client.sh assets/realistic-scenes/gothic-cloister-corner/Model.glb --nr 1
./scripts/run-client.sh assets/realistic-scenes/iridescent-dish-with-olives/Model.glb --nr 1
```

Gothic 的照片纹理已烘焙光照并使用 `KHR_materials_unlit`，切换 HDR 不会重新照亮扫描表面。Dish 上游还包含 iridescence 扩展，其原生渲染表现尚待专项核验。许可文本见 [CC BY 4.0](realistic-scenes/LICENSES/CC-BY-4.0.txt)，各目录保留上游许可和来源记录。

三个场景的 OpenDLSS-NR 关闭与开启对比见[主 README](../README.md#效果)。

## 环境照明

| 预设 ID / 用途 | 文件 | 大小 | 来源与作者 |
| --- | --- | --- | --- |
| `studio-small-09` / 摄影棚 | [studio_small_09_1k.hdr](environments/studio_small_09_1k.hdr) | 1,615,248 字节 | [Studio Small 09](https://polyhaven.com/a/studio_small_09)，Sergej Majboroda |
| `kloofendal-daylight` / 白天户外 | [kloofendal_48d_partly_cloudy_1k.hdr](environments/kloofendal_48d_partly_cloudy_1k.hdr) | 1,637,206 字节 | [Kloofendal 48d Partly Cloudy](https://polyhaven.com/a/kloofendal_48d_partly_cloudy)，Greg Zaal |
| `chalk-quarry-sunset` / 日落 | [sunset_in_the_chalk_quarry_1k.hdr](environments/sunset_in_the_chalk_quarry_1k.hdr) | 1,580,299 字节 | [Sunset in the Chalk Quarry](https://polyhaven.com/a/sunset_in_the_chalk_quarry)，Sergej Majboroda |
| `entrance-hall` / 室内 | [entrance_hall_1k.hdr](environments/entrance_hall_1k.hdr) | 1,621,287 字节 | [Entrance Hall](https://polyhaven.com/a/entrance_hall)，Sergej Majboroda |

四张 HDRI 均来自 Poly Haven，采用 [CC0 1.0](https://polyhaven.com/license)，以原始 1k HDR 文件保存。[presets.json](environments/presets.json) 记录预设名称、文件、下载来源、作者、大小与 SHA-256；完整许可文本见 [LICENSE.txt](environments/LICENSE.txt)。Studio Small 09 用于上面的 PBR、透射材质和人物截图。每张图的下载记录与来源证据分别保存在 [Studio](environments/SOURCE.json)、[Daylight](environments/kloofendal-daylight.source.json)、[Sunset](environments/chalk-quarry-sunset.source.json) 和 [Interior](environments/entrance-hall.source.json)。

运行时可在 **Scene → Environment preset** 中选择预设，切换时保留模型和相机，并更新环境照明、反射与 HDR 背景。

四种预设各自的 OpenDLSS-NR 关闭与开启对比见[主 README](../README.md#效果)，使用相同人物、相机、IBL 强度与环境旋转。

也可在启动时指定预设：

```sh
./scripts/run-client.sh --environment-preset chalk-quarry-sunset
```
