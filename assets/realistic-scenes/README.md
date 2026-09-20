# 写实视觉测试场景

三组资产用于建筑摄影测量、复杂桌面和真实产品的视觉检查，GLB 合计 **92,906,576 字节（约 92.9 MB）**。A Beautiful Game 和 Dish 保留官方固定版本的原始 GLB 字节，仅本地文件名不同。

**Gothic 的本地修改：** 在原场景外新增绕 X 轴旋转 −90° 的父节点，将扫描场景的地面方向从 Z-up 校正为 Y-up。原有节点、几何、材质、贴图以及完整 BIN 数据块均保持不变；GLB 的 JSON 场景层级、填充与文件长度随之更新。原文件与派生文件的大小、SHA-256、Git blob 标识及 BIN 不变证明分别记录在 [SOURCE.json](gothic-cloister-corner/SOURCE.json)，其中包含重现步骤。

三组 `view.json` 均为本项目新增的相机、环境和显示配置。

在仓库根目录运行，使用已有的本地 NR 权重：

```sh
./scripts/run-client.sh assets/realistic-scenes/a-beautiful-game/Model.glb --nr 1
./scripts/run-client.sh assets/realistic-scenes/gothic-cloister-corner/Model.glb --nr 1
./scripts/run-client.sh assets/realistic-scenes/iridescent-dish-with-olives/Model.glb --nr 1
```

也可在窗口的 **Model → Assets / scene** 中选择场景。启动命令会读取各目录的 `view.json`。

三个场景的 OpenDLSS-NR 开关对比见[主 README](../../README.md#效果)。每行左侧为 NR OFF，右侧为 NR ON（`temporal=1`）；每对保持模型、材质、相机、照明与 960×720 分辨率一致，动画固定在同一帧。逐对配置见[对比记录](../../docs/verification/nr-comparisons-2026-11-11.json)。

| 场景 | 文件大小（字节） | 默认场景图元 / 三角形 | 嵌入贴图 |
| --- | --- | --- | --- |
| [A Beautiful Game](a-beautiful-game/Model.glb) | 42,977,928 | 49 / 1,499,072 | 33 张 JPEG，2048×2048 |
| [Gothic Cloister Corner](gothic-cloister-corner/Model.glb) | 44,185,820 | 16 / 1,013,385 | 1 张 JPEG，1024×1024 |
| [Iridescent Dish with Olives](iridescent-dish-with-olives/Model.glb) | 5,742,828 | 4 / 24,448 | 11 张 PNG，512×512 或 2048×1024 |

几何统计包含默认场景的节点实例，按静态姿态计算。全部图片和缓冲区均嵌入 GLB；三组模型的 `extensionsRequired` 均为空，未使用 KTX2、Draco 或 Meshopt 压缩。[manifest.json](manifest.json) 和各目录的 `inspection.json` 记录世界坐标包围盒、嵌入相机、贴图尺寸与扩展；`SOURCE.json` 记录下载地址、Git blob 标识和 SHA-256。

## 作者、来源与许可

- **A Beautiful Game**：Moeen Sayed 与 Mujtaba Sayed 为 [SideFX Karma 教程](https://www.sidefx.com/tutorials/karma-a-beautiful-game/)创作原模型；ASWF / MaterialX 项目公开发布，Chris Rydalch 与 Jonathan Stone 完成最初的 GLB 转换，Ed Mackey 完成此版本的 glTF PBR 转换。采用 CC BY 4.0。保留的[上游说明](a-beautiful-game/README.md)、[许可](a-beautiful-game/LICENSE.md)及[来源记录](a-beautiful-game/SOURCE.json)列出完整信息。
- **Gothic Cloister Corner**：[Zoilo](https://sketchfab.com/iliedom) 的[建筑摄影测量模型](https://sketchfab.com/3d-models/gothic-cloister-corner-69f4351215e84d3ca61feea9fe9890db)，基于 Babylon.js 官方资产库的 mobile GLB，采用 CC BY 4.0。本项目仅增加上述场景根节点方向校正；原始[署名与许可文件](gothic-cloister-corner/license.txt)保留，原文件与派生文件的记录见 [SOURCE.json](gothic-cloister-corner/SOURCE.json)。
- **Iridescent Dish with Olives**：Wayfair, LLC / Eric Chadwick，根据真实餐具的照片参考制作，采用 CC BY 4.0。保留的[上游说明](iridescent-dish-with-olives/README.md)、[许可](iridescent-dish-with-olives/LICENSE.md)及[来源记录](iridescent-dish-with-olives/SOURCE.json)包含建模、材质与动画说明。

Khronos 两组模型固定于 [`8f492390ddc7a2ae221a1369ef560589cf8c70fb`](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb)，Gothic 固定于 BabylonJS/Assets [`3798844710bb7e76199ee38b175176c0fe4e9e62`](https://github.com/BabylonJS/Assets/tree/3798844710bb7e76199ee38b175176c0fe4e9e62/meshes/GothicCloisterCorner)。完整许可文本见 [CC BY 4.0](LICENSES/CC-BY-4.0.txt)。Khronos 上游元文档由该项目贡献者按 CC BY 4.0 提供。

## 材质检查说明

Gothic 使用 `KHR_materials_unlit`，照片纹理已包含烘焙光照；HDR 环境切换不会重新照亮扫描表面。

A Beautiful Game 可检查木质棋盘、棋子、PBR 与透射材质。Dish 可检查法线、PBR、透射和玻璃盖动画；上游还使用 IOR、volume 与 iridescence 扩展，iridescence 在当前 Filament 客户端中的原生渲染表现尚待专项核验。
