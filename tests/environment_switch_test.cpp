// Exercise LoadedScene's failure transaction on the real, CPU-only NOOP backend.
#include "scene.h"

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Scene.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const fs::path dir = fs::temp_directory_path() /
      ("nr environment switch " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const fs::path model = dir / "triangle.gltf";
  std::ofstream(model) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
"buffers":[{"uri":"triangle.bin","byteLength":42}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
  {
    std::ofstream bin(dir / "triangle.bin",std::ios::binary);
    const std::array<float,9> vertices{0,0,0,1,0,0,0,1,0};
    const std::array<uint16_t,3> indices{0,1,2};
    bin.write(reinterpret_cast<const char*>(vertices.data()),sizeof(vertices));
    bin.write(reinterpret_cast<const char*>(indices.data()),sizeof(indices));
  }
  const fs::path corrupt = dir / "corrupt environment.hdr";
  std::ofstream(corrupt) << "not an HDR image";
  const fs::path wrongShape = dir / "wrong shape environment.hdr";
  {
    std::ofstream hdr(wrongShape,std::ios::binary);
    hdr << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    hdr.write("\x80\x40\x20\x81",4);
  }

  filament::Engine* engine = filament::Engine::create(filament::Engine::Backend::NOOP);
  if (!engine) { fs::remove_all(dir); std::cerr << "cannot create NOOP engine\n"; return 2; }
  int result = 0;
  try {
    SceneView initial;
    initial.model = model.string();
    LoadedScene loaded(*engine,initial);
    loaded.setEnvironmentIntensity(0.73f);
    loaded.setEnvironmentRotation(0.4f);
    auto* previousLight = loaded.scene()->getIndirectLight();
    auto* previousSkybox = loaded.scene()->getSkybox();
    require(previousLight && previousSkybox,"fixture must start with fallback light and solid skybox");
    for (bool background : {false,true}) {
      loaded.setBackgroundEnvironment(background);
      for (const auto& path : {dir / "missing environment.hdr",corrupt,wrongShape}) {
        bool rejected = false;
        try { loaded.setEnvironment(path.string()); }
        catch (const std::runtime_error& e) { rejected = std::string(e.what()).find(path.string()) != std::string::npos; }
        require(rejected,"failed environment must throw an error identifying its path");
        require(loaded.scene()->getIndirectLight()==previousLight,"failed environment replaced the current light");
        require(loaded.scene()->getSkybox()==previousSkybox,"failed environment replaced the current background");
        require(loaded.view().environment.empty(),"failed environment changed the current path");
        require(std::abs(previousLight->getIntensity()-0.73f)<1e-6f,"failed environment changed light intensity");
        require(std::abs(loaded.view().environmentIntensity-0.73f)<1e-6f,"current intensity was not preserved");
        require(std::abs(loaded.view().environmentRotation-0.4f)<1e-6f,"current rotation was not preserved");
        require(loaded.view().backgroundEnvironment==background,"requested background visibility was not preserved");
      }
    }
    std::cout << "environment replacement: six failure transactions preserve scene and current look\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    result = 1;
  }
  filament::Engine::destroy(&engine);
  fs::remove_all(dir);
  return result;
}
