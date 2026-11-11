// Standalone cgltf implementation for the import boundary test, without Filament/GPU.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include "client_controls.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
static std::string skinFixture(int joints, bool addOversized = false) {
  std::string s = "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[";
  for (int i=0;i<joints;++i) s += (i ? ",{}" : "{}");
  s += "],\"skins\":[{\"joints\":[";
  for (int i=0;i<joints;++i) s += (i ? "," : "") + std::to_string(i);
  s += "]}";
  if (addOversized) { s += ",{\"joints\":["; for (int i=0;i<257;++i) s += (i ? "," : "") + std::to_string(i % joints); s += "]}"; }
  return s+"]}";
}
int main(int argc, char** argv) {
  const auto dir = fs::temp_directory_path() / ("nr import with spaces " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  auto write = [&](std::string name,std::string data) { const auto path=dir/name; std::ofstream(path)<<data; return path.string(); };
  // Catch rejecting the supported boundary before loading a valid scene.
  client::validateSceneFile(write("valid.gltf",skinFixture(256)));
  std::string json = skinFixture(256);
  while (json.size()%4) json += ' ';
  std::string glb;
  auto word = [&](uint32_t value) { for (int byte=0;byte<4;++byte) glb += char((value>>(byte*8))&255); };
  word(0x46546c67); word(2); word(uint32_t(20+json.size())); word(uint32_t(json.size())); word(0x4e4f534a); glb += json;
  client::validateSceneFile(write("valid.GlB",glb));
  write("texture name.png","image resource fixture");
  client::validateSceneFile(write("encoded-image.gltf","{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"texture%20name.png\"}]}"));
  // Catch overlooking a second, otherwise unreferenced skin before gltfio sees it.
  bool rejected = false;
  try { client::validateSceneFile(write("oversize.gltf",skinFixture(256,true))); }
  catch (const std::exception& e) { rejected = std::string(e.what()).find("256") != std::string::npos; }
  assert(rejected);
  for (const auto& path : {write("broken.glb","bad binary"), write("missing-image.gltf","{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"absent.png\"}]}"), write("missing-buffer.gltf", "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":16,\"uri\":\"absent.bin\"}]}"), (dir/"absent.glb").string()}) {
    rejected = false; try { client::validateSceneFile(path); } catch (...) { rejected = true; }
    assert(rejected);
  }
  if (argc > 1) client::validateSceneFile(argv[1]);
  fs::remove_all(dir);
  std::cout << "client import: all checks passed\n";
}
