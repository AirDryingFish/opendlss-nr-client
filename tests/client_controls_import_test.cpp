// Standalone import boundary test, without Filament or a GPU.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
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
static std::string makeGlb(std::string json, std::string bin = {}) {
  while (json.size()%4) json += ' ';
  while (bin.size()%4) bin += '\0';
  std::string glb;
  auto word = [&](uint32_t v) { for (int b=0;b<4;++b) glb += char((v>>(b*8))&255); };
  word(0x46546c67); word(2); word(uint32_t(20+json.size()+(bin.empty()?0:8+bin.size())));
  word(uint32_t(json.size())); word(0x4e4f534a); glb += json;
  if (!bin.empty()) { word(uint32_t(bin.size())); word(0x004e4942); glb += bin; }
  return glb;
}
static std::string imageFixture(const std::string& image) {
  return "{\"asset\":{\"version\":\"2.0\"},\"images\":[" + image + "],\"textures\":[{\"source\":0}],\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]}";
}
int main(int argc, char** argv) {
  const auto dir = fs::temp_directory_path() / ("nr import with spaces " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  auto write = [&](std::string name,std::string data) { const auto path=dir/name; std::ofstream(path,std::ios::binary)<<data; return path.string(); };
  // The exact decoder used by LoadedScene must throw before a replacement commits.
  const auto missingHdr = (dir/"missing environment.hdr").string();
  const auto corruptHdr = write("corrupt environment.hdr","not an HDR image");
  auto workingScene = std::string("old scene");
  for (const auto& hdr : {missingHdr,corruptHdr}) {
    bool rejected = false;
    try {
      int width=0,height=0;
      auto* pixels = client::loadEnvironmentPixels(hdr,width,height);
      stbi_image_free(pixels);
      workingScene = "replacement";
    } catch (const std::exception& e) { rejected = std::string(e.what()).find(hdr) != std::string::npos; }
    assert(rejected && workingScene == "old scene");
  }
  std::string radiance = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
  radiance += std::string("\x80\x40\x20\x81",4);
  int hdrWidth=0,hdrHeight=0;
  auto* hdrPixels = client::loadEnvironmentPixels(write("valid environment.hdr",radiance),hdrWidth,hdrHeight);
  assert(hdrPixels && hdrWidth==1 && hdrHeight==1 && hdrPixels[0]>0);
  stbi_image_free(hdrPixels);
  client::validateSceneFile(write("valid.gltf",skinFixture(256)));
  client::validateSceneFile(write("valid.GlB",makeGlb(skinFixture(256))));
  const unsigned char png[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,11,73,68,65,84,120,156,99,248,15,4,0,9,251,3,253,251,94,107,43,0,0,0,0,73,69,78,68,174,66,96,130};
  write("texture name.png",std::string(reinterpret_cast<const char*>(png),sizeof(png)));
  client::validateSceneFile(write("encoded-image.gltf",imageFixture("{\"uri\":\"texture%20name.png\"}")));
  client::validateSceneFile(write("data-image.gltf",imageFixture("{\"uri\":\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII=\"}")));
  bool rejected = false;
  try { client::validateSceneFile(write("oversize.gltf",skinFixture(256,true))); }
  catch (const std::exception& e) { rejected = std::string(e.what()).find("256") != std::string::npos; }
  assert(rejected);
  // KTX2 rejection must cover actual bytes, declared format and BasisU sources.
  const unsigned char ktxMagic[] = {0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a};
  std::string ktx(reinterpret_cast<const char*>(ktxMagic),sizeof(ktxMagic));
  ktx.resize(80,'\0');
  write("compressed.bin",ktx);
  write("declared.KTX2",std::string(reinterpret_cast<const char*>(png),sizeof(png)));
  std::string embeddedKtx = imageFixture("{\"bufferView\":0,\"mimeType\":\"image/ktx2\"}");
  embeddedKtx.pop_back(); embeddedKtx += ",\"buffers\":[{\"byteLength\":80}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":80}]}";
  std::string basisu = imageFixture("{\"uri\":\"compressed.bin\"}");
  const auto source = basisu.find("\"textures\":[{\"source\":0}]");
  basisu.replace(source,std::string("\"textures\":[{\"source\":0}]").size(),"\"textures\":[{\"extensions\":{\"KHR_texture_basisu\":{\"source\":0}}}]");
  for (const auto& path : {
      write("ktx-magic.gltf",imageFixture("{\"uri\":\"compressed.bin\"}")),
      write("ktx-extension.gltf",imageFixture("{\"uri\":\"declared.KTX2\"}")),
      write("ktx-mime.gltf",imageFixture("{\"uri\":\"texture%20name.png\",\"mimeType\":\"image/ktx2\"}")),
      write("ktx-basisu.gltf",basisu),
      write("ktx-embedded.glb",makeGlb(embeddedKtx,ktx)),
      write("ktx-data.gltf",imageFixture("{\"uri\":\"data:image/ktx2;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII=\"}"))}) {
    rejected = false;
    try { client::validateSceneFile(path); }
    catch (const std::exception& e) { rejected = std::string(e.what()).find("PNG/JPEG") != std::string::npos; }
    assert(rejected);
  }
  write("damaged.png","not an image");
  std::string embedded = imageFixture("{\"bufferView\":0,\"mimeType\":\"image/png\"}");
  embedded.pop_back(); embedded += ",\"buffers\":[{\"byteLength\":3}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":3}]}";
  for (const auto& path : {
      write("damaged.gltf",imageFixture("{\"uri\":\"damaged.png\"}")),
      write("damaged-data.gltf",imageFixture("{\"uri\":\"data:image/png;base64,YmFk\"}")),
      write("damaged-embedded.glb",makeGlb(embedded,"bad")),
      write("broken.glb","bad binary"),
      write("missing-image.gltf",imageFixture("{\"uri\":\"absent.png\"}")),
      write("missing-buffer.gltf","{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":16,\"uri\":\"absent.bin\"}]}"),
      (dir/"absent.glb").string()}) {
    rejected = false; try { client::validateSceneFile(path); } catch (...) { rejected = true; }
    assert(rejected);
  }
  if (argc > 1) client::validateSceneFile(argv[1]);
  fs::remove_all(dir);
  std::cout << "client import: all checks passed\n";
}
