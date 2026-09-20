# Native NR Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox syntax for tracking.

**Goal:** Deliver a Linux native GLB viewer running the upstream Tensor Core NR path, with local weights, reproducible builds, diagnostics, and honest runtime validation.

**Architecture:** Reuse pinned OpenDLSS-NR/Filament, shared Vulkan command stream and GPU-resident temporal history. Adapt the Linux build and desktop interaction, prepare the supplied static test asset, and provide an absolute-location-independent launcher and GPU capability report.

**Tech Stack:** C++20, Vulkan, generated PTX sm_89, Filament 1.77.0, SDL2/X11, Python3, Node20+ for static DLL extraction.

**Spec:** ../specs/2026-11-11-native-client-design.txt (approved by user on 2026-11-11).

## Global Constraints

- Preserve upstream fast kernels and licenses at 9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1.
- No automatic driver changes, sudo, browser/CPU fallback, fabricated NR output or FPS.
- Source and launch entry live in outputs/opendlss-nr-client; large build dependencies/caches live under the parent work/ directory.
- Compatible model is 71 blocks / 153 tensors; existing source DLL is read statically, never executed.
- Preserve original GLBs; test asset has no animations and must be correctly baked, not merely stripped of skinning.
- As verified on 2026-11-11, NVIDIA 595.99.02 on RTX 4080 SUPER passes the actual capability probe (`supported: true`), and native NR rendering and benchmarking run successfully. Runtime asset reload/error recovery still requires acceptance testing.

## Review Focus

- Paths with spaces and launching outside the project directory must work without shell injection.
- Reloading a broken or incompatible model must not crash gltfio or corrupt the existing scene.
- Animated assets over the 256-joint limit must get an explicit error, not silent baking.
- GPU diagnostics must check the selected NVIDIA device and actual required features, not numeric driver versions.
- FPS/NR timing must not label NR-off frames as NR inference or imply successful benchmarking without real output.

### Task 1: Linux build and pinned native dependencies

**Files:** scripts/linux/*, CMakeLists.txt, demo/CMakeLists.txt, minimal portability fixes in src/* and demo/crash_report.cpp or demo/vulkan_device.cpp if needed.
**Ownership:** Build implementer; do not edit demo/main.cpp, client/* or scripts/bake_static_glb.py.
**Interfaces:** `scripts/linux/build.sh` builds `build/demo/dlss5-demo`, `build/dlss5vk`, tools/shader/PTX resources. Use existing upstream build paths, build caches outside deliverables; honor NR_BUILD_JOBS (default 4), NR_DEPS_DIR, NR_BUILD_DIR. Modern headers and volk available under tools/ via user-local symlinks to dependency dir. Launcher will use these relative resource paths.

- [x] Audit exact upstream generation invocations before porting; record clean baseline where runnable.
- [x] Add Linux dependency setup pinned to required versions; no system mutation. Prefer installed tools if compatible and local Clang/libc++ otherwise.
- [x] Port shader/PTX build invocations faithfully and add Linux CMake host/demo build.
- [x] Compile patched Filament and native binaries; fix actual compiler/linker failures without changing numerical kernels.
- [x] Run PTX divider test and relevant generation checks; save configure/build logs outside deliverables.
- [x] Record actual build result and required runtime libraries; coordinate UI edits before final rebuild.

### Task 2: Native client interaction and measurable rendering

**Files:** demo/main.cpp, demo/client_controls.h/.cpp if useful, tests/client_controls*.
**Ownership:** UI implementer; do not edit build files or upstream numerical kernels.
**Interfaces:** Existing Scene and NrPass. Preserve CLI compatibility and scripted captures. Add GLB drop/path loading, orbit/zoom/pan/reset, automatic scene/NR sizing to the actual window client area, explicit NR state and network vs frame timing. Runtime reload must reset history.

- [x] Write meaningful native tests for any factored camera/framing/argument helpers; verify failure before implementation.
- [x] Implement input path loading and SDL_DROPFILE, reject oversized skin before entering gltfio.
- [x] Implement orbit/zoom/pan and reset-to-bounds while retaining existing keyboard navigation where practical.
- [x] Follow the latest window-sizing requirement: remove manual render sizing controls; use CLI width/height only for the initial window request, and size scene/NR/UI to the actual window client area on startup, drag and maximize. Keep a 64×64 minimum, suspend rendering/resource resizing while minimized, and synchronize dimensions and reset temporal history on restore.
- [x] Add NR status, network GPU time and frame ms/FPS; warm benchmark mode records median/P95 for at least120 frames with GPU/driver/resolution metadata.
- [x] Build against the Linux dependency output and run helper tests; verify GPU runtime after the driver upgrade.
- [x] Historical Apply fix (`86c9012`): removed `SDL_SetWindowSize` and returned early for unchanged render dimensions. The old 2048→512→2048→2048 scenario completed 12 process-local SDL synthetic clicks with consistent native/SDL/ImGui/UI viewport dimensions. It verifies the previous manual-resolution implementation, not the new window-follow behavior.
- [x] Verify actual window resize requests 800×600→1200×900→720×960→2048×2048 (actual2048×1403)→1024×768→1024×768, 12 NR off/on clicks and matching native/SDL/render/ImGui/UI dimensions. Initial oversized-window startup also renders at actual2048×1403. Evidence: `docs/verification/window-follow-2026-11-11.json`. Native maximize/minimize buttons are code reviewed, not separately exercised. Restart old clients to load the updated binary.

### Task 3: Compatible test asset and native model data

**Files:** scripts/bake_static_glb.py, tests/test_bake_static_glb.py, scripts/model/*, assets/test-static.glb, models/nr/* (large outputs git-ignored).
**Ownership:** Asset implementer; no build/UI files.
**Interfaces:** `python scripts/bake_static_glb.py INPUT OUTPUT` rejects animation/morph-bearing assets, bakes position/normal/tangent as present with normalized weights and proper parent transforms, keeps textures/materials and scale. Existing audited Node extractor is copied with licenses into scripts/model/ and invoked on the known DLL to produce manifest +11stage files.

- [x] Write a synthetic non-identity joint/mesh transform test and an animated-asset rejection test; observe expected failures.
- [x] Implement static skin baking with correct accessors/buffer alignment and bounds.
- [x] Bake outputs/test-fixed.glb to assets/test-static.glb and independently check all world positions <=1e-5 difference, unit normals, material/texture preservation and no skin references.
- [x] Run glTF validator (zero errors, report preexisting warnings).
- [x] Extract and validate known DLL stage hashes/layout; do not execute DLL or publish weights.
- [x] Record asset/model verification results and CLI usage for packaging.

### Task 4: Startup diagnostics, launcher and integration (controller)

**Files:** client/doctor.cpp, client/requirements.h, tests/test_requirements.cpp, scripts/run-client.sh, scripts/check-gpu.sh, README-LINUX.md, .gitignore, tests/test_launcher.py.
**Interfaces:** Probe built to build/nr-doctor using tools/Vulkan-Headers and Vulkan loader; text+JSON reports supported adapter/extensions/features. Launcher resolves its own directory, honors explicit `--model`, uses prepared defaults, starts the actual native binary after diagnostics and resource validation. No implicit fallback on missing extensions.

- [x] Pin fake-device tests for missing extension, wrong GPU, and successful capability sets; verify expected failure.
- [x] Implement extension/feature probe and structured report, compile and run against actual GPU.
- [x] Write launcher tests covering path spaces, absent model/resources and propagation of native exit codes; then implement launchers.
- [x] Integrate build, model and UI results; add documented one-command build/start and driver capability recheck.
- [x] Review each task diff for scope and quality, fix real findings, run full available checks.
- [x] Verified actual startup gate from a different directory before upgrade: explicit missing-driver capability error, exit2; no fallback. After upgrade, actual NVIDIA 595.99.02 / RTX 4080 SUPER capability report is `supported: true`.
- [x] Verify prepared static asset loading, native NR-on/off captures and the previous manual resolution switching on the actual GPU. Window-follow acceptance is tracked separately above.
- [x] Historical benchmark at actual 1024×1024 with NR/temporal on, 30 warmup frames and 120 samples: network GPU median 4.22961426 ms; GPU frame median 4.73266602 ms; wall frame median 6.864666 ms and P95 7.796201 ms. Report: `docs/verification/1024-benchmark-2026-11-11.json`.
- [ ] Runtime acceptance: reload an asset while the client is running and verify recovery from a broken/incompatible import while retaining the existing scene. These cases have not yet been tested in the native GUI.
- [x] Publish files and verification results, explicitly distinguish build success, NR execution and measured timings. Keep the remaining runtime reload/error-recovery acceptance open.
