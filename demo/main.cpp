// DLSS 5 NR demo: Filament (Vulkan backend, on the demo's own device) renders a glTF scene into an HDR color
// texture and, through the patched structure pass, per-object motion vectors; the NR pass runs inside Filament's
// command buffer between the scene and the present views; a present view draws the composited output and an
// ImGui overlay (filagui) the controls. SDL2 owns the window and the input.
//
//   dlss5-demo [scene.gltf|.glb [environment.hdr]] --model <nr model dir> [--scenes <dir>] [--width w --height h]
//              [--frames n] [--capture prefix] [--orbit deg/frame] [--view px,py,pz,tx,ty,tz[,fov]] [--nr 0|1]
//              [--temporal 0|1] [--style 0..3] [--style-knobs ...] [--intensity x] [--kernels <dir>]
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>
// SDL_syswm includes X11, whose Success macro collides with Filament enums.
#ifdef Success
#undef Success
#endif

#include <filagui/ImGuiHelper.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderTarget.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <gltfio/Animator.h>
#include <imgui.h>
#include <math/mat3.h>
#include <utils/EntityManager.h>
#include <utils/Path.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

#include "client_controls.h"
#include "crash_report.h"
#include "filament_vulkan.h"
#include "nr_pass.h"
#include "scene.h"
#include "vulkan_device.h"

using namespace filament;
using namespace filament::math;
namespace fs = std::filesystem;

namespace {

struct Options {
  std::string sceneArg, environmentArg, modelDir, kernelDir, scenesDir, capturePrefix, viewOverride;
  uint32_t width = 1280, height = 720;
  int frames = -1;
  int benchmarkFrames = 0, benchmarkWarmup = 30;
  std::string benchmarkOutput = "benchmark.json";
  float orbitDegreesPerFrame = 0.4f;
  int animation = 0;
  NrControls controls;
  int style = 0, networkStyle = 0;
};

struct FirstPersonCamera {
  float3 position{0, 1, 5};
  float yaw = 0.0f, pitch = 0.0f;   // radians; yaw 0 looks down -z
  float fov = 50.0f, nearPlane = 0.05f, farPlane = 1000.0f, moveSpeed = 1.0f;
  float3 orbitTarget{0, 0, 0};
  float3 forward() const { return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)}; }
  float3 right() const { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }
  void lookAt(float3 eye, float3 target) {
    position = eye;
    orbitTarget = target;
    float3 d = normalize(target - eye);
    pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    yaw = std::atan2(-d.x, -d.z);
  }
  void orbit(float dx, float dy) {
    const float distance = std::max(length(position - orbitTarget), 0.001f);
    yaw -= dx * 0.006f;
    pitch = std::clamp(pitch - dy * 0.006f, -1.55f, 1.55f);
    const auto p = client::orbitEye({orbitTarget.x, orbitTarget.y, orbitTarget.z}, yaw, pitch, distance);
    position = {float(p.x), float(p.y), float(p.z)};
  }
  void pan(float dx, float dy, float height) {
    const auto offset = client::panOffset(yaw, pitch, dx, dy, length(position-orbitTarget), height);
    const float3 delta{float(offset.x),float(offset.y),float(offset.z)};
    position += delta; orbitTarget += delta;
  }
  void zoom(float steps) {
    const float distance = std::max(length(position-orbitTarget),0.001f);
    const float next = float(client::zoomDistance(distance,steps,std::max(nearPlane*2.0f,0.0001f)));
    position = orbitTarget - forward() * next;
  }
  void apply(Camera& camera, float aspect) const {
    camera.setProjection(fov, aspect, nearPlane, farPlane, Camera::Fov::VERTICAL);
    float3 f = forward();
    camera.lookAt(double3(position), double3(position + f), double3(0, 1, 0));
  }
};

std::string exeDirectory() {
  char* base = SDL_GetBasePath();
  std::string dir = base ? base : "./";
  SDL_free(base);
  return dir;
}

void fail(const std::string& message) {
  fprintf(stderr, "%s\n", message.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "dlss5-demo", message.c_str(), nullptr);
  exit(2);
}

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) fail("cannot read " + path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

const char* kUsage = "usage: dlss5-demo [scene.gltf|.glb [environment.hdr]] --model <nr model dir> [--scenes <dir>] [--width w --height h] "
                     "[--frames n] [--capture prefix] [--orbit deg] [--view px,py,pz,tx,ty,tz[,fov]] [--nr 0|1] [--temporal 0|1] "
                     "[--style 0..3] [--style-knobs exposure,contrast,gamma,saturation,hue,vibrance,strength[,networkStyle]] [--intensity x] "
                     "[--animation n] [--benchmark-frames N>=120] [--benchmark-warmup N] [--benchmark-output path]";

Options parseOptions(int argc, char** argv) {
  Options o;
  std::vector<std::string> positional;
  auto integer = [](const std::string& flag, const std::string& value, int lo, int hi) {
    try { return client::parseInteger(value,lo,hi); }
    catch (const std::exception& e) { fail(flag + ": " + e.what()); }
    return lo;
  };
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](std::string& out) { if (i + 1 >= argc) fail("missing value for " + a + "\n\n" + kUsage); out = argv[++i]; };
    std::string v;
    if (a == "--model") next(o.modelDir);
    else if (a == "--kernels") next(o.kernelDir);
    else if (a == "--scenes") next(o.scenesDir);
    else if (a == "--capture") next(o.capturePrefix);
    else if (a == "--view") next(o.viewOverride);
    else if (a == "--width" || a == "-w") { next(v); o.width = integer(a,v,64,16384); }
    else if (a == "--height" || a == "-h") { next(v); o.height = integer(a,v,64,16384); }
    else if (a == "--frames") { next(v); o.frames = v == "-1" ? -1 : integer(a,v,0,10000000); }
    else if (a == "--benchmark-frames") { next(v); o.benchmarkFrames = integer(a,v,120,1000000); }
    else if (a == "--benchmark-warmup") { next(v); o.benchmarkWarmup = integer(a,v,0,1000000); }
    else if (a == "--benchmark-output") next(o.benchmarkOutput);
    else if (a == "--orbit") { next(v); o.orbitDegreesPerFrame = (float)atof(v.c_str()); }
    else if (a == "--animation") { next(v); o.animation = integer(a,v,0,1000000); }
    else if (a == "--temporal") { next(v); o.controls.temporal = atoi(v.c_str()) != 0; }
    else if (a == "--nr") { next(v); o.controls.enabled = atoi(v.c_str()) != 0; }
    else if (a == "--intensity") { next(v); o.controls.intensity = (float)atof(v.c_str()); }
    else if (a == "--style") { next(v); o.style = atoi(v.c_str()); o.controls.style = o.style < 3 ? o.style : 0; o.controls.styleCustom = o.style == 3; }
    else if (a == "--style-knobs") {   // exposure,contrast,gamma,saturation,hue,vibrance,strength[,networkStyle]
      next(v);
      float k[8] = {0, 0, 1, 0, 0, 1, 1, 0};
      sscanf(v.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f", &k[0], &k[1], &k[2], &k[3], &k[4], &k[5], &k[6], &k[7]);
      o.controls.styleExposure = k[0]; o.controls.styleContrast = k[1]; o.controls.styleGamma = k[2]; o.controls.styleSaturation = k[3];
      o.controls.styleHue = k[4]; o.controls.styleVibrance = k[5]; o.controls.styleStrength = k[6];
      o.networkStyle = (int)k[7]; o.controls.style = o.networkStyle; o.controls.styleCustom = true; o.style = 3;
    }
    else if (a.rfind("--", 0) == 0) fail("unknown option " + a + "\n\n" + kUsage);
    else positional.push_back(a);
  }
  if (!positional.empty()) o.sceneArg = positional[0];
  if (positional.size() > 1) o.environmentArg = positional[1];
  if (positional.size() > 2) fail("too many scene arguments\n" + std::string(kUsage));
  if (o.benchmarkFrames && (!o.controls.enabled || !o.capturePrefix.empty() ||
      !client::validBenchmark(o.benchmarkFrames,o.benchmarkWarmup,o.frames)))
    fail("benchmark requires NR on, no scripted capture, and at least warmup + samples + 2 total frames");
  return o;
}

// The presets are (exposure, contrast, saturation) offsets scaled by the local tone; copy them and use the tone
// as the strength so the custom knobs start from the preset's look
void loadStylePreset(NrControls& c, int& networkStyle, int preset) {
  c.styleExposure = preset == 1 ? -0.1f : 0.0f;
  c.styleContrast = preset == 1 ? -0.25f : 0.0f;
  c.styleSaturation = preset == 1 ? -0.1f : preset == 2 ? -0.15f : 0.0f;
  c.styleGamma = 1.0f; c.styleHue = 0.0f; c.styleVibrance = 1.0f;
  c.styleStrength = preset == 0 ? 1.0f : c.localTone;
  networkStyle = preset; c.style = preset;
}

// The present quad: a fullscreen pair of triangles in the device domain. The output texture's first row is the
// top of the picture and Filament samples with the OpenGL convention (v = 0 at the last row): v runs 1 -> 0 from
// the bottom vertices to the top ones.
struct PresentQuad {
  VertexBuffer* vertices = nullptr;
  IndexBuffer* indices = nullptr;
  Material* material = nullptr;
  MaterialInstance* instance = nullptr;
  utils::Entity entity;
  void create(Engine& engine, Scene& scene, const std::string& materialPath) {
    static const float data[] = {-1, -1, 0, 0,   1, -1, 1, 0,   -1, 1, 0, 1,   1, 1, 1, 1};
    static const uint16_t index[] = {0, 1, 2, 2, 1, 3};
    vertices = VertexBuffer::Builder().vertexCount(4).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT2, 0, 16)
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 8, 16)
        .build(engine);
    vertices->setBufferAt(engine, 0, VertexBuffer::BufferDescriptor(data, sizeof(data), nullptr));
    indices = IndexBuffer::Builder().indexCount(6).bufferType(IndexBuffer::IndexType::USHORT).build(engine);
    indices->setBuffer(engine, IndexBuffer::BufferDescriptor(index, sizeof(index), nullptr));
    std::string package = readFile(materialPath);
    material = Material::Builder().package(package.data(), package.size()).build(engine);
    instance = material->createInstance();
    entity = utils::EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{-1, -1, -1}, {1, 1, 1}})
        .material(0, instance)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vertices, indices, 0, 6)
        .culling(false).castShadows(false).receiveShadows(false)
        .build(engine, entity);
    scene.addEntity(entity);
  }
  void setTexture(Texture* texture) {
    TextureSampler sampler(TextureSampler::MinFilter::NEAREST, TextureSampler::MagFilter::NEAREST);
    instance->setParameter("image", texture, sampler);
  }
  void destroy(Engine& engine) {
    engine.destroy(entity);
    utils::EntityManager::get().destroy(entity);
    engine.destroy(instance);
    engine.destroy(material);
    engine.destroy(indices);
    engine.destroy(vertices);
  }
};

// The frame's textures: the scene target (color + depth), the velocity pair the structure pass writes, the NR output
struct FrameTextures {
  Texture* color = nullptr; Texture* depth = nullptr; Texture* velocity = nullptr; Texture* velocityDepth = nullptr; Texture* output = nullptr;
  RenderTarget* target = nullptr;
  uint32_t width = 0, height = 0;
  void create(Engine& engine, uint32_t w, uint32_t h) {
    width = w; height = h;
    color = Texture::Builder().width(w).height(h).format(Texture::InternalFormat::RGBA16F)
        .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE).build(engine);
    depth = Texture::Builder().width(w).height(h).format(Texture::InternalFormat::DEPTH32F)
        .usage(Texture::Usage::DEPTH_ATTACHMENT).build(engine);
    velocity = Texture::Builder().width(w).height(h).format(Texture::InternalFormat::RGBA32UI)
        .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE | Texture::Usage::BLIT_SRC).build(engine);
    velocityDepth = Texture::Builder().width(w).height(h).format(Texture::InternalFormat::DEPTH32F)
        .usage(Texture::Usage::DEPTH_ATTACHMENT | Texture::Usage::SAMPLEABLE).build(engine);
    output = Texture::Builder().width(w).height(h).format(Texture::InternalFormat::RGBA8)
        .usage(Texture::Usage::SAMPLEABLE | Texture::Usage::BLIT_DST).build(engine);
    target = RenderTarget::Builder().texture(RenderTarget::AttachmentPoint::COLOR, color).texture(RenderTarget::AttachmentPoint::DEPTH, depth).build(engine);
  }
  void destroy(Engine& engine) {
    engine.destroy(target);
    for (Texture* t : {color, depth, velocity, velocityDepth, output}) engine.destroy(t);
    *this = FrameTextures{};
  }
};

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  Options options = parseOptions(argc, argv);
  const std::string exeDir = exeDirectory();
  installCrashReport(exeDir);
  const std::string dataDir = exeDir + "../data";
  const std::string kernelDir = options.kernelDir.empty() ? exeDir + "../shaders" : options.kernelDir;
  const std::string scenesDir = options.scenesDir.empty() ? exeDir + "../scenes" : options.scenesDir;

  // the model directory: --model, DLSS5VK_MODEL, or models/nr next to the repository (build/demo/../../models/nr)
  std::string modelDir = options.modelDir;
  if (modelDir.empty() && getenv("DLSS5VK_MODEL")) modelDir = getenv("DLSS5VK_MODEL");
  if (modelDir.empty() && fs::exists(exeDir + "../../models/nr/manifest.json")) modelDir = exeDir + "../../models/nr";
  if (modelDir.empty()) fail(std::string("No model directory.\n\nPass --model <dir>, set DLSS5VK_MODEL, or put the model in models\\nr next to the repository.\n\n") + kUsage);
  if (!fs::exists(modelDir + "/manifest.json")) fail("No manifest.json in the model directory " + modelDir);

  // ---- the scenes
  std::vector<SceneView> scenes = scanScenes(scenesDir);
  int selectedScene = -1;
  if (!options.sceneArg.empty()) {
    scenes.insert(scenes.begin(), sceneFromArguments(options.sceneArg, options.environmentArg));
    selectedScene = 0;
  } else if (!scenes.empty()) {
    selectedScene = 0;
  } else {
    fail("No scene: pass a glTF (and an .hdr environment) or put scene directories with a view.json under " + scenesDir + "\n\n" + kUsage);
  }

  try { client::validateSceneFile(scenes[selectedScene].model); }
  catch (const std::exception& e) { fail(std::string("cannot load scene: ") + e.what()); }

  // ---- window
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fail(std::string("SDL_Init: ") + SDL_GetError());
  SDL_Window* window = SDL_CreateWindow("DLSS 5 NR - Filament / Vulkan", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        (int)options.width, (int)options.height, SDL_WINDOW_RESIZABLE);
  if (!window) fail(std::string("SDL_CreateWindow: ") + SDL_GetError());
  SDL_StartTextInput();
  SDL_SysWMinfo wmi;
  SDL_VERSION(&wmi.version);
  if (!SDL_GetWindowWMInfo(window, &wmi)) fail("SDL_GetWindowWMInfo failed");
#if defined(_WIN32)
  void* nativeWindow = (void*)wmi.info.win.window;
#elif defined(__APPLE__)
  void* prepareMetalView(void* nsWindow);   // native_window_cocoa.mm
  void* nativeWindow = prepareMetalView((void*)wmi.info.cocoa.window);
#else
  void* nativeWindow = (void*)wmi.info.x11.window;
#endif

  // ---- the device, shared with the renderer
  auto device = std::make_unique<VulkanDevice>();
  void* sharedContext = createSharedContext(device->handles());
  // (a scene like Bistro has over a thousand renderables and textures: more handle space than the default)
  Engine::Config config;
  config.driverHandleArenaSizeMB = 64;
  Engine* engine = Engine::Builder().backend(Engine::Backend::VULKAN).sharedContext(sharedContext).config(&config).build();
  if (!engine) fail("Filament could not start on the device");
  SwapChain* swapChain = engine->createSwapChain(nativeWindow);
  Renderer* renderer = engine->createRenderer();
  renderer->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true});

  // ---- views: the scene into its target, the present quad and the UI into the swapchain
  int drawableWidth = 0, drawableHeight = 0;
  SDL_GetWindowSize(window, &drawableWidth, &drawableHeight);
  // Requested render size is exact even if the desktop constrains the physical window.
  uint32_t width = options.width, height = options.height;
  FrameTextures frame;
  frame.create(*engine, width, height);

  utils::Entity cameraEntity = utils::EntityManager::get().create();
  Camera* camera = engine->createCamera(cameraEntity);
  View* sceneView = engine->createView();
  sceneView->setName("scene");
  sceneView->setCamera(camera);
  sceneView->setPostProcessingEnabled(false);
  sceneView->setAntiAliasing(View::AntiAliasing::NONE);
  sceneView->setDithering(View::Dithering::NONE);
  sceneView->setShadowingEnabled(true);
  sceneView->setShadowType(View::ShadowType::PCF);
  // the intermediate color buffer (refraction, SSR) in full precision: the network reads linear HDR
  sceneView->setRenderQuality({.hdrColorBuffer = View::QualityLevel::HIGH});
  sceneView->setRenderTarget(frame.target);
  sceneView->setViewport({0, 0, width, height});
  sceneView->setVelocityBuffers(frame.velocity, frame.velocityDepth);

  Scene* presentScene = engine->createScene();
  utils::Entity presentCameraEntity = utils::EntityManager::get().create();
  Camera* presentCamera = engine->createCamera(presentCameraEntity);
  presentCamera->setProjection(Camera::Projection::ORTHO, -1, 1, -1, 1, 0, 1);
  View* presentView = engine->createView();
  presentView->setName("present");
  presentView->setCamera(presentCamera);
  presentView->setScene(presentScene);
  presentView->setPostProcessingEnabled(false);
  presentView->setShadowingEnabled(false);
  presentView->setFrustumCullingEnabled(false);
  const auto initialPresent = client::letterbox(width,height,uint32_t(drawableWidth),uint32_t(drawableHeight));
  presentView->setViewport({initialPresent.left,initialPresent.bottom,initialPresent.width,initialPresent.height});
  PresentQuad quad;
  quad.create(*engine, *presentScene, dataDir + "/materials/present.filamat");
  quad.setTexture(frame.output);

  View* uiView = engine->createView();
  uiView->setName("ui");
  uiView->setPostProcessingEnabled(false);
  uiView->setShadowingEnabled(false);
  uiView->setBlendMode(View::BlendMode::TRANSLUCENT);
  uiView->setViewport({0, 0, uint32_t(drawableWidth), uint32_t(drawableHeight)});
  auto imgui = std::make_unique<filagui::ImGuiHelper>(engine, uiView, utils::Path());
  imgui->setDisplaySize(drawableWidth, drawableHeight);
  auto& platform = ImGui::GetPlatformIO();
  platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
    static std::string clipboard;
    char* text = SDL_GetClipboardText();
    clipboard = text ? text : "";
    SDL_free(text);
    return clipboard.c_str();
  };
  platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { SDL_SetClipboardText(text); };

  // ---- the NR pass
  fprintf(stderr, "[demo] model %s, kernels %s\n", modelDir.c_str(), kernelDir.c_str());
  auto nr = std::make_unique<NrPass>(device->handles(), width, height, modelDir, kernelDir, dataDir + "/shaders");

  // ---- the scene and the camera
  std::unique_ptr<LoadedScene> loaded;
  FirstPersonCamera fps;
  float exposure = 1.0f, environmentIntensity = 1.0f, sunIntensity = 1.0f, environmentRotation = 0.0f;
  int msaa = 1;
  auto applyMsaa = [&]() {
    // (the scene pass only: the structure pass with the motion vectors and the network stay single-sampled)
    sceneView->setMultiSampleAntiAliasingOptions({.enabled = msaa > 1, .sampleCount = (uint8_t)(msaa > 1 ? msaa : 1)});
  };
  bool backgroundEnvironment = false;
  bool animate = true;
  int animationIndex = 0;
  std::string importError;
  auto resetCamera = [&]() {
    const auto framing = client::fitBounds(loaded->boundsRadius(),45,double(width)/height);
    const float3 center = loaded->boundsCenter();
    fps.fov = 45; fps.nearPlane = float(framing.nearPlane); fps.farPlane = float(framing.farPlane);
    fps.lookAt(center + float3{0,0,float(framing.distance)},center);
    fps.moveSpeed = float(framing.distance);
  };
  auto loadScene = [&](int index) {
    engine->flushAndWait();
    const SceneView& view = scenes[index];
    const bool initialLoad = !loaded;
    try {
      client::validateSceneFile(view.model);
      // Construct the replacement before discarding the current scene.
      auto replacement = std::make_unique<LoadedScene>(*engine, view);
      sceneView->setScene(replacement->scene());
      loaded = std::move(replacement);
      selectedScene = index;
      importError.clear();
    } catch (const std::exception& e) {
      importError = std::string("cannot load ") + view.model + ": " + e.what();
      fprintf(stderr,"[import] %s\n",importError.c_str());
      return false;
    }
    sceneView->setScene(loaded->scene());
    exposure = view.exposure; environmentIntensity = view.environmentIntensity; sunIntensity = view.sunIntensity;
    environmentRotation = view.environmentRotation; backgroundEnvironment = view.backgroundEnvironment;
    msaa = view.msaa;
    applyMsaa();
    // the camera: the view's, or a look-at of the bounds
    if (view.hasCamera) {
      fps.lookAt(view.cameraPosition, view.cameraTarget);
      fps.fov = view.fov; fps.nearPlane = view.nearPlane; fps.farPlane = view.farPlane;
    } else {
      resetCamera();
    }
    fps.moveSpeed = view.moveSpeed > 0 ? view.moveSpeed : std::max(length(fps.orbitTarget - fps.position), 0.1f);
    if (initialLoad && !options.viewOverride.empty()) {
      float v[7] = {0, 0, 0, 0, 0, 0, fps.fov};
      sscanf(options.viewOverride.c_str(), "%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
      fps.lookAt({v[0], v[1], v[2]}, {v[3], v[4], v[5]});
      fps.fov = v[6];
    }
    const auto* newAnimator = loaded->animator();
    animationIndex = client::normalizeAnimationIndex(options.animation,newAnimator ? newAnimator->getAnimationCount() : 0);
    nr->resetHistory();
    return true;
  };
  if (!loadScene(selectedScene)) fail(importError);

  // ---- the UI state
  NrControls& controls = options.controls;
  int nrStyle = options.style, nrNetworkStyle = options.networkStyle;
  const char* styleNames[] = {"off", "natural", "cinematic", "custom"};
  const char* networkStyleNames[] = {"off", "natural", "cinematic"};
  bool historyReset = true;
  mat4 previousClipFromRotated;
  bool hasPreviousCamera = false;
  bool autoOrbit = false;
  bool mouseLook = false, mouseOrbit = false, mousePan = false;
  bool followWindow = false;
  int customWidth = int(width), customHeight = int(height);
  uint32_t requestedWidth = 0, requestedHeight = 0;
  char modelPath[4096] = {};
  std::string droppedPath;
  const bool benchmarking = options.benchmarkFrames > 0;
  client::BenchmarkSamples benchmark(options.benchmarkWarmup,options.benchmarkFrames);
  std::string benchmarkError;
  const auto benchmarkStart = std::chrono::steady_clock::now();
  float fpsValue = 0.0f;
  double cpuFrameMs = 0.0;
  uint32_t frameIndex = 0;
  bool running = true;
  bool pendingResize = false;
  int pendingScene = -1;   // a dropdown selection, loaded at the top of the next frame

  auto capture = [&](const std::string& name) {
    engine->flushAndWait();
    std::string prefix = options.capturePrefix.empty() ? "capture" : options.capturePrefix;
    nr->saveOutput(prefix + "-" + name + ".ppm", false);
    nr->saveOutput(prefix + "-" + name + "-scene.ppm", true);
    fprintf(stderr, "[demo] captured %s-%s.ppm\n", prefix.c_str(), name.c_str());
  };

  auto resize = [&](uint32_t w, uint32_t h) {
    engine->flushAndWait();
    width = w; height = h;
    frame.destroy(*engine);
    frame.create(*engine, width, height);
    sceneView->setRenderTarget(frame.target);
    sceneView->setViewport({0, 0, width, height});
    sceneView->setVelocityBuffers(frame.velocity, frame.velocityDepth);
    // Presentation follows the window; NR/scene keep the exact requested resolution.
    const auto viewport = client::letterbox(width,height,uint32_t(drawableWidth),uint32_t(drawableHeight));
    presentView->setViewport({viewport.left,viewport.bottom,viewport.width,viewport.height});
    uiView->setViewport({0, 0, uint32_t(drawableWidth), uint32_t(drawableHeight)});
    imgui->setDisplaySize(drawableWidth, drawableHeight);
    quad.setTexture(frame.output);
    nr->resize(width, height);
    historyReset = true;
  };

  auto lastTime = std::chrono::high_resolution_clock::now();
  auto fpsTime = lastTime;
  uint32_t fpsFrames = 0;
  float animationTime = 0.0f;
  const uint8_t* keys = SDL_GetKeyboardState(nullptr);

  while (running) {
    if (benchmarking && std::chrono::duration<double>(std::chrono::steady_clock::now()-benchmarkStart).count() > 300) {
      benchmarkError = "benchmark incomplete: time limit exceeded";
      break;
    }
    ImGuiIO& io = ImGui::GetIO();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_QUIT: running = false; break;
        case SDL_WINDOWEVENT:
          if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESIZED) pendingResize = true;
          break;
        case SDL_DROPFILE:
          if (!benchmarking) droppedPath = event.drop.file;
          SDL_free(event.drop.file);
          break;
        case SDL_TEXTINPUT: io.AddInputCharactersUTF8(event.text.text); break;
        case SDL_MOUSEMOTION:
          io.AddMousePosEvent((float)event.motion.x, (float)event.motion.y);
          if (benchmarking) break;
          if (mouseOrbit) fps.orbit(float(event.motion.xrel),float(event.motion.yrel));
          if (mousePan) fps.pan(float(event.motion.xrel),float(event.motion.yrel),float(drawableHeight));
          if (mouseLook) {
            const float distance = std::max(length(fps.position-fps.orbitTarget),0.001f);
            fps.yaw -= event.motion.xrel * 0.003f;
            fps.pitch = std::clamp(fps.pitch - event.motion.yrel * 0.003f, -1.55f, 1.55f);
            fps.orbitTarget = fps.position + fps.forward()*distance;
          }
          break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
          int button = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
          bool down = event.type == SDL_MOUSEBUTTONDOWN;
          io.AddMouseButtonEvent(button, down);
          if (!benchmarking && (!io.WantCaptureMouse || !down)) {
            if (event.button.button == SDL_BUTTON_LEFT) mouseOrbit = down;
            if (event.button.button == SDL_BUTTON_MIDDLE) mousePan = down;
            if (event.button.button == SDL_BUTTON_RIGHT) mouseLook = down;
            SDL_SetRelativeMouseMode(mouseLook ? SDL_TRUE : SDL_FALSE);
          }
          break;
        }
        case SDL_MOUSEWHEEL:
          io.AddMouseWheelEvent((float)event.wheel.x, (float)event.wheel.y);
          if (!benchmarking && !io.WantCaptureMouse) fps.zoom(float(event.wheel.y));
          break;
        case SDL_KEYUP:
        case SDL_KEYDOWN:
          io.AddKeyEvent(ImGuiMod_Ctrl,(event.key.keysym.mod & KMOD_CTRL) != 0);
          io.AddKeyEvent(ImGuiMod_Shift,(event.key.keysym.mod & KMOD_SHIFT) != 0);
          if (event.key.keysym.sym == SDLK_BACKSPACE) io.AddKeyEvent(ImGuiKey_Backspace,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_DELETE) io.AddKeyEvent(ImGuiKey_Delete,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_LEFT) io.AddKeyEvent(ImGuiKey_LeftArrow,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_RIGHT) io.AddKeyEvent(ImGuiKey_RightArrow,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_HOME) io.AddKeyEvent(ImGuiKey_Home,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_END) io.AddKeyEvent(ImGuiKey_End,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_RETURN) io.AddKeyEvent(ImGuiKey_Enter,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_a) io.AddKeyEvent(ImGuiKey_A,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_v) io.AddKeyEvent(ImGuiKey_V,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_c) io.AddKeyEvent(ImGuiKey_C,event.type == SDL_KEYDOWN);
          if (event.key.keysym.sym == SDLK_x) io.AddKeyEvent(ImGuiKey_X,event.type == SDL_KEYDOWN);
          if (event.type == SDL_KEYUP || io.WantCaptureKeyboard) break;
          if (benchmarking && event.key.keysym.sym != SDLK_ESCAPE) break;
          switch (event.key.keysym.sym) {
            case SDLK_ESCAPE: running = false; break;
            case SDLK_n: controls.enabled = !controls.enabled; historyReset = true; break;
            case SDLK_t: controls.temporal = !controls.temporal; historyReset = true; break;
            case SDLK_r: resetCamera(); hasPreviousCamera = false; historyReset = true; break;
            case SDLK_b: backgroundEnvironment = !backgroundEnvironment; break;
            case SDLK_c: capture("frame" + std::to_string(frameIndex)); break;
            default: break;
          }
          break;
        default: break;
      }
    }
    if (!droppedPath.empty()) {
      try {
        SceneView imported = sceneFromArguments(droppedPath,options.environmentArg);
        client::validateSceneFile(imported.model);
        scenes.push_back(std::move(imported));
        pendingScene = int(scenes.size())-1;
      } catch (const std::exception& e) { importError = e.what(); }
      droppedPath.clear();
    }
    if (pendingScene >= 0) {
      if (loadScene(pendingScene)) { hasPreviousCamera = false; historyReset = true; animationTime = 0; }
      pendingScene = -1;
    }
    if (requestedWidth && requestedHeight) {
      resize(requestedWidth,requestedHeight);
      SDL_SetWindowSize(window,int(requestedWidth),int(requestedHeight));
      requestedWidth = requestedHeight = 0;
      hasPreviousCamera = false;
    }
    if (pendingResize) {
      pendingResize = false;
      int w = 0, h = 0;
      SDL_GetWindowSize(window, &w, &h);
      if (w > 0 && h > 0) {
        if (benchmarking && frameIndex > 0 && (w != drawableWidth || h != drawableHeight)) {
          benchmarkError = "benchmark invalid: window resized during measurement"; running = false;
        }
        drawableWidth = w; drawableHeight = h;
        const auto viewport = client::letterbox(width,height,uint32_t(w),uint32_t(h));
        presentView->setViewport({viewport.left,viewport.bottom,viewport.width,viewport.height});
        uiView->setViewport({0,0,uint32_t(w),uint32_t(h)});
        imgui->setDisplaySize(w,h);
        if (followWindow && !benchmarking && w >= 64 && h >= 64 && (uint32_t(w) != width || uint32_t(h) != height)) {
          resize(uint32_t(w),uint32_t(h)); hasPreviousCamera = false;
        }
      }
    }

    // (the renderer paces the frames: when it asks to skip one, wait a little instead of spinning)
    if (!renderer->beginFrame(swapChain)) { SDL_Delay(1); continue; }
    auto now = std::chrono::high_resolution_clock::now();
    const double wallFrameMs = std::chrono::duration<double,std::milli>(now-lastTime).count();
    if (benchmarking && frameIndex > 0) benchmark.addFrame(frameIndex-1,wallFrameMs);
    float dt = std::min(float(wallFrameMs/1000.0), 0.1f);
    lastTime = now;
    fpsFrames++;
    if (std::chrono::duration<float>(now - fpsTime).count() >= 0.5f) {
      fpsValue = fpsFrames / std::chrono::duration<float>(now - fpsTime).count();
      fpsFrames = 0; fpsTime = now;
    }

    // ---- camera: WASD / QE (shift: faster), the mouse look, the scripted orbit
    if (!benchmarking && !io.WantCaptureKeyboard) {
      float speed = fps.moveSpeed * dt * (keys[SDL_SCANCODE_LSHIFT] ? 4.0f : 1.0f);
      const float3 oldPosition = fps.position;
      float3 f = fps.forward(), r = fps.right();
      if (keys[SDL_SCANCODE_W]) fps.position += f * speed;
      if (keys[SDL_SCANCODE_S]) fps.position -= f * speed;
      if (keys[SDL_SCANCODE_D]) fps.position += r * speed;
      if (keys[SDL_SCANCODE_A]) fps.position -= r * speed;
      if (keys[SDL_SCANCODE_E]) fps.position.y += speed;
      if (keys[SDL_SCANCODE_Q]) fps.position.y -= speed;
      fps.orbitTarget += fps.position-oldPosition;
    }
    if (autoOrbit) {
      // the scripted camera motion: a steady yaw (the verification's reprojection check assumes a pure rotation)
      fps.yaw -= options.orbitDegreesPerFrame * 3.14159265f / 180.0f;
    }
    fps.apply(*camera, (float)width / (float)height);
    camera->setExposure(exposure);
    loaded->setEnvironmentIntensity(environmentIntensity);
    loaded->setSunIntensity(sunIntensity);
    loaded->setBackgroundEnvironment(backgroundEnvironment);
    loaded->setEnvironmentRotation(environmentRotation);

    // ---- animation (skinning / morphing / node transforms: the motion vectors follow through Filament's history)
    gltfio::Animator* animator = loaded->animator();
    if (animator && animator->getAnimationCount() > 0 && animate) {
      animationTime += dt;
      size_t index = std::min((size_t)animationIndex, animator->getAnimationCount() - 1);
      animator->applyAnimation(index, std::fmod(animationTime, std::max(animator->getAnimationDuration(index), 1e-3f)));
      animator->updateBoneMatrices();
    }

    // ---- the camera's motion at infinity, for the motion unpack's background pixels: previous clip <- current clip
    // through the view rotations (a point at infinity does not see the translation)
    mat4 eyeFromWorld = camera->getViewMatrix();
    eyeFromWorld[3] = double4(0, 0, 0, 1);
    const mat4 clipFromRotated = camera->getProjectionMatrix() * eyeFromWorld;
    if (!hasPreviousCamera) { previousClipFromRotated = clipFromRotated; hasPreviousCamera = true; }
    // an unchanged camera leaves the background exactly where it was (the product with the inverse is not exactly 1)
    const mat4f background = clipFromRotated == previousClipFromRotated ? mat4f() : mat4f(previousClipFromRotated * inverse(clipFromRotated));
    previousClipFromRotated = clipFromRotated;

    // ---- the frame
    if (nr->chainTimedOut()) {
      engine->flushAndWait(); nr->fallBackToBarriers(); historyReset = true;
      if (benchmarking) { benchmarkError = "benchmark invalid: GPU chained wait timed out"; running = false; }
    }
    if (historyReset) { nr->resetHistory(); historyReset = false; }
    NrPass::Frame nrFrame = nr->beginFrame(controls, &background[0][0]);
    if (benchmarking) benchmark.addGpu(nr->timingsFrame(),nr->timingsFresh(),nr->timingsEnabled(),nr->timings().frameMs,nr->timings().networkMs);
    auto cpuStart = std::chrono::high_resolution_clock::now();
    NrPass* pass = nr.get();
    queueGpuWork(*engine, {}, [pass, nrFrame](void* commands, std::vector<GpuImage>&) { pass->recordStamp(commands, nrFrame, NrPass::kFrameStart); });
    renderer->render(sceneView);
    queueGpuWork(*engine, {frame.color, frame.velocity, frame.output}, [pass, nrFrame](void* commands, std::vector<GpuImage>& images) {
      pass->record(commands, nrFrame, images[0], images[1], images[2]);
      for (GpuImage& image : images) image.layout = NrPass::shaderReadOnlyLayout();
    });
    renderer->render(presentView);
    imgui->render(dt, [&](Engine*, View*) {
      ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
      ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Once);
      ImGui::SetNextWindowSizeConstraints(ImVec2(280,100),ImVec2(float(std::max(drawableWidth-20,280)),float(std::max(drawableHeight-20,100))));
      ImGui::Begin("DLSS 5 NR - Filament", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::PushItemWidth(140.0f);
      ImGui::Text("%s", nr->deviceName().c_str());
      ImGui::Text("NR: %s",controls.enabled ? (nr->timingsEnabled() && nr->timings().networkMs > 0 ? "ON (Tensor Core FP8)" : "ON / awaiting GPU sample") : "OFF (scene composite)");
      ImGui::Text("%.1f FPS / %.2f ms wall",fpsValue,fpsValue > 0 ? 1000.0f/fpsValue : 0.0f);
      ImGui::Text("CPU submit %.2f ms",cpuFrameMs);
      ImGui::Text("Render / NR: %u x %u",width,height);
      if (controls.enabled && nr->timingsEnabled() && nr->timings().networkMs > 0)
        ImGui::Text("Network GPU: %.3f ms",nr->timings().networkMs);
      else ImGui::TextUnformatted("Network GPU: no NR sample");
      if (benchmarking) {
        ImGui::Text("Benchmark warmup %d / samples %zu of %d",options.benchmarkWarmup,benchmark.networkMs.size(),options.benchmarkFrames);
        ImGui::TextUnformatted("Controls locked during measurement");
        ImGui::PopItemWidth(); ImGui::End(); return;
      }
      if (!importError.empty()) ImGui::TextWrapped("Import: %s",importError.c_str());
      if (ImGui::CollapsingHeader("Resolution",ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("512 x 512")) { requestedWidth = requestedHeight = 512; followWindow = false; }
        ImGui::SameLine();
        if (ImGui::Button("1024 x 1024")) { requestedWidth = requestedHeight = 1024; followWindow = false; }
        ImGui::InputInt("Width",&customWidth);
        ImGui::InputInt("Height",&customHeight);
        if (ImGui::Button("Apply exact size")) {
          if (customWidth >= 64 && customWidth <= 16384 && customHeight >= 64 && customHeight <= 16384) {
            requestedWidth = uint32_t(customWidth); requestedHeight = uint32_t(customHeight); followWindow = false;
          } else importError = "resolution must be 64..16384 in each dimension";
        }
        if (ImGui::Checkbox("Render follows window",&followWindow) && followWindow) pendingResize = true;
        ImGui::Text("Window: %d x %d",drawableWidth,drawableHeight);
      }
      if (ImGui::CollapsingHeader("DLSS NR", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enabled", &controls.enabled)) historyReset = true;
        if (ImGui::Checkbox("Temporal", &controls.temporal)) historyReset = true;
        ImGui::SliderFloat("Intensity", &controls.intensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Tone", &controls.localTone, 0.0f, 1.0f);
        ImGui::SliderFloat("Structure", &controls.localStructure, 0.0f, 1.0f);
        ImGui::Checkbox("Auto mask", &controls.autoMask);
        if (ImGui::Combo("Style", &nrStyle, styleNames, 4)) {
          controls.styleCustom = nrStyle == 3;
          controls.style = nrStyle < 3 ? nrStyle : nrNetworkStyle;
        }
        if (controls.styleCustom) {
          ImGui::SliderFloat("Exposure (EV)", &controls.styleExposure, -2.0f, 2.0f);
          ImGui::SliderFloat("Contrast", &controls.styleContrast, -1.0f, 1.0f);
          ImGui::SliderFloat("Gamma", &controls.styleGamma, 0.5f, 2.0f);
          ImGui::SliderFloat("Saturation", &controls.styleSaturation, -1.0f, 1.0f);
          ImGui::SliderFloat("Hue shift", &controls.styleHue, -0.5f, 0.5f);
          ImGui::SliderFloat("Vibrance", &controls.styleVibrance, 0.5f, 2.0f);
          ImGui::SliderFloat("Strength", &controls.styleStrength, 0.0f, 2.0f);
          if (ImGui::Combo("Network style", &nrNetworkStyle, networkStyleNames, 3)) controls.style = nrNetworkStyle;
          if (ImGui::Button("From natural")) loadStylePreset(controls, nrNetworkStyle, 1);
          ImGui::SameLine();
          if (ImGui::Button("From cinematic")) loadStylePreset(controls, nrNetworkStyle, 2);
          ImGui::SameLine();
          if (ImGui::Button("Neutral")) loadStylePreset(controls, nrNetworkStyle, 0);
        }
        ImGui::SliderFloat("Paper white", &controls.paperWhite, 0.25f, 4.0f);
        ImGui::SliderFloat("Color", &controls.colorStrength, 0.0f, 1.0f);
        if (ImGui::Button("Reset history")) historyReset = true;
      }
      if (ImGui::CollapsingHeader("GPU timings (ms)", ImGuiTreeNodeFlags_DefaultOpen)) {
        const NrTimings& t = nr->timings();
        ImGui::Text("scene       %6.3f", t.sceneMs);
        ImGui::Text("preprocess  %6.3f", t.preprocessMs);
        if (controls.enabled && nr->timingsEnabled()) ImGui::Text("network     %6.3f", t.networkMs);
        else ImGui::TextUnformatted("network     NR off / pending");
        ImGui::Text("composite   %6.3f", t.compositeMs);
        ImGui::Text("present+ui  %6.3f", t.presentMs);
        ImGui::Text("frame       %6.3f", t.frameMs);
        ImGui::Text("%ux%u, frame %u", nr->width(), nr->height(), nr->frameCount());
      }
      if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputText("GLB / glTF path",modelPath,sizeof(modelPath),ImGuiInputTextFlags_EnterReturnsTrue)) droppedPath = modelPath;
        if (ImGui::Button("Load model")) droppedPath = modelPath;
        ImGui::TextWrapped("Drop GLB/glTF. Left drag: orbit; middle: pan; wheel: zoom. Right drag: look; WASD/QE: move.");
        if (ImGui::Button("Reset camera (R)")) { resetCamera(); hasPreviousCamera = false; historyReset = true; }
        if (ImGui::BeginCombo("Demo scene", scenes[selectedScene].name.c_str())) {
          for (int i = 0; i < (int)scenes.size(); i++) {
            bool selected = i == selectedScene;
            if (ImGui::Selectable(scenes[i].name.c_str(), selected) && !selected) pendingScene = i;
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        ImGui::Text("%zu renderables, %zu lights", loaded->renderableCount(), loaded->lightCount());
        ImGui::Checkbox("Background", &backgroundEnvironment);
        ImGui::SliderFloat("Exposure", &exposure, 0.1f, 10.0f);
        ImGui::SliderFloat("IBL", &environmentIntensity, 0.0f, 4.0f);
        if (loaded->view().hasSun) ImGui::SliderFloat("Sun", &sunIntensity, 0.0f, 10.0f);
        ImGui::SliderFloat("Env rotation", &environmentRotation, -3.1416f, 3.1416f);
        {
          const char* msaaNames[] = {"off", "2x", "4x", "8x"};
          int msaaIndex = msaa >= 8 ? 3 : msaa >= 4 ? 2 : msaa >= 2 ? 1 : 0;
          if (ImGui::Combo("Scene MSAA", &msaaIndex, msaaNames, 4)) { msaa = 1 << msaaIndex; applyMsaa(); }
        }
        ImGui::Text("camera %.2f %.2f %.2f", fps.position.x, fps.position.y, fps.position.z);
        ImGui::Checkbox("Orbit", &autoOrbit);
      }
      if (animator && animator->getAnimationCount() > 0 && ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Animate", &animate);
        if (ImGui::BeginCombo("Clip", animator->getAnimationName((size_t)animationIndex))) {
          for (size_t i = 0; i < animator->getAnimationCount(); i++) {
            bool selected = (int)i == animationIndex;
            if (ImGui::Selectable(animator->getAnimationName(i), selected)) animationIndex = (int)i;
          }
          ImGui::EndCombo();
        }
      }
      ImGui::PopItemWidth();
      ImGui::End();
    });
    renderer->render(uiView);
    queueGpuWork(*engine, {}, [pass, nrFrame](void* commands, std::vector<GpuImage>&) { pass->recordStamp(commands, nrFrame, NrPass::kPresentEnd); });
    renderer->endFrame();
    nr->endFrame();
    cpuFrameMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - cpuStart).count();
    frameIndex++;

    // ---- the scripted verification sequence: static -> orbit -> settle -> NR off, with captures and raw dumps
    if (!options.capturePrefix.empty()) {
      const uint32_t f = frameIndex;
      if (f == 40) { capture("nr-static"); autoOrbit = true; }
      if (f == 99 || f == 100) {
        // the camera of the frame, for offline checks of the motion vectors (row-major text: projection, view)
        std::ofstream cameras(options.capturePrefix + "-cameras.txt", std::ios::app);
        mat4 p = camera->getProjectionMatrix(), v = camera->getViewMatrix();
        cameras << "frame " << f << "\n";
        for (mat4 const* m : {&p, &v}) {
          for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) cameras << (*m)[c][r] << (c < 3 ? " " : "\n"); }
        }
      }
      if (f == 99) {
        engine->flushAndWait();
        nr->saveRaw(options.capturePrefix + "-pre-scene.raw", 0);
        nr->saveRaw(options.capturePrefix + "-pre-velocity.raw", 2);
      }
      if (f == 100) {
        capture("nr-moving");
        nr->saveRaw(options.capturePrefix + "-moving-scene.raw", 0);
        nr->saveRaw(options.capturePrefix + "-moving-motion.raw", 1);
        nr->saveRaw(options.capturePrefix + "-moving-velocity.raw", 2);
        autoOrbit = false;
      }
      if (f == 160) { capture("nr-settled"); controls.enabled = false; historyReset = true; }
      if (f == 200) { capture("nr-off"); controls.enabled = true; historyReset = true; }
    }
    if (benchmarking) {
      if (benchmark.complete()) running = false;
      if (frameIndex > uint32_t(options.benchmarkWarmup + options.benchmarkFrames + 600) ||
          std::chrono::duration<double>(std::chrono::steady_clock::now()-benchmarkStart).count() > 300) {
        benchmarkError = "benchmark incomplete: insufficient fresh GPU samples within frame/time limit";
        running = false;
      }
    }
    if (options.frames > 0 && (int)frameIndex >= options.frames) running = false;
  }

  // ---- shutdown
  engine->flushAndWait();
  int exitCode = 0;
  if (benchmarking && nr->chainTimedOut()) benchmarkError = "benchmark invalid: GPU chained wait timed out during final drain";
  if (benchmarking) {
    if (!benchmark.complete() || !benchmarkError.empty()) {
      if (benchmarkError.empty()) benchmarkError = "benchmark interrupted before collecting all samples";
      fprintf(stderr,"[benchmark] %s (wall %zu, GPU %zu)\n",benchmarkError.c_str(),benchmark.frameMs.size(),benchmark.networkMs.size());
      std::ostringstream output;
      output << "{\n  \"complete\": false,\n  \"error\": " << client::jsonQuote(benchmarkError)
             << ",\n  \"wall_samples\": " << benchmark.frameMs.size() << ", \"gpu_samples\": " << benchmark.networkMs.size() << "\n}\n";
      try { client::writeTextFile(options.benchmarkOutput,output.str()); }
      catch (const std::exception& e) { fprintf(stderr,"[benchmark] %s\n",e.what()); }
      exitCode = 3;
    } else {
      VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties2.pNext = &driver;
      vkGetPhysicalDeviceProperties2(nr->context().physical(),&properties2);
      const auto& properties = properties2.properties;
      const auto wall = client::summarize(benchmark.frameMs);
      const auto gpu = client::summarize(benchmark.gpuFrameMs);
      const auto network = client::summarize(benchmark.networkMs);
      std::ostringstream output;
      // Escape POSIX path control characters as well as JSON quotes/backslashes.
      output << std::setprecision(9) << "{\n  \"complete\": true,\n  \"gpu\": " << client::jsonQuote(nr->deviceName())
             << ",\n  \"driver_version_raw\": " << properties.driverVersion
             << ",\n  \"driver_name\": " << client::jsonQuote(driver.driverName) << ",\n  \"driver_info\": " << client::jsonQuote(driver.driverInfo)
             << ",\n  \"backend\": \"Vulkan VK_NV_cuda_kernel_launch / FP8 Tensor Core PTX\",\n  \"nr_enabled\": true,\n  \"width\": " << width << ", \"height\": " << height
             << ",\n  \"window_width\": " << drawableWidth << ", \"window_height\": " << drawableHeight
             << ",\n  \"scene\": " << client::jsonQuote(scenes[selectedScene].model)
             << ",\n  \"temporal_enabled\": " << (controls.temporal ? "true" : "false")
             << ",\n  \"warmup_frames\": " << options.benchmarkWarmup << ", \"samples\": " << options.benchmarkFrames
             << ",\n  \"frame_wall_ms\": {\"median\": " << wall.median << ", \"p95\": " << wall.p95
             << "},\n  \"frame_gpu_ms\": {\"median\": " << gpu.median << ", \"p95\": " << gpu.p95
             << "},\n  \"network_gpu_ms\": {\"median\": " << network.median << ", \"p95\": " << network.p95 << "}\n}\n";
      try { client::writeTextFile(options.benchmarkOutput,output.str()); }
      catch (const std::exception& e) { fprintf(stderr,"[benchmark] %s\n",e.what()); exitCode = 3; }
      if (exitCode == 0) fprintf(stderr,"[benchmark] %s: wall median %.3f P95 %.3f ms, network median %.3f P95 %.3f ms\n",options.benchmarkOutput.c_str(),wall.median,wall.p95,network.median,network.p95);
    }
  }
  {
    const NrTimings& t = nr->timings();
    fprintf(stderr, "[demo] %u frames at %ux%u, last GPU timings (ms): scene %.3f preprocess %.3f network %.3f composite %.3f present+ui %.3f frame %.3f; %.1f fps\n",
            frameIndex, width, height, t.sceneMs, t.preprocessMs, t.networkMs, t.compositeMs, t.presentMs, t.frameMs, fpsValue);
  }
  nr.reset();
  loaded.reset();
  imgui.reset();
  quad.destroy(*engine);
  frame.destroy(*engine);
  engine->destroy(uiView);
  engine->destroy(presentView);
  engine->destroy(sceneView);
  engine->destroy(presentScene);
  engine->destroyCameraComponent(cameraEntity);
  engine->destroyCameraComponent(presentCameraEntity);
  utils::EntityManager::get().destroy(cameraEntity);
  utils::EntityManager::get().destroy(presentCameraEntity);
  engine->destroy(renderer);
  engine->destroy(swapChain);
  Engine::destroy(&engine);
  destroySharedContext(sharedContext);
  device.reset();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return exitCode;
}
