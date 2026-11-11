#include "client_controls.h"
#include <cgltf.h>
#include <stb_image.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <memory>
namespace client {
namespace fs = std::filesystem;
namespace {
void validateImage(const std::vector<unsigned char>& bytes) {
  // KTX2 decoding belongs to Filament's texture provider. Check its container
  // signature/header here; PNG/JPEG and other stb images are fully decoded.
  const unsigned char ktxMagic[] = {0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a};
  if (bytes.size() >= sizeof(ktxMagic) && std::memcmp(bytes.data(),ktxMagic,sizeof(ktxMagic)) == 0) {
    if (bytes.size() < 80) throw std::runtime_error("truncated KTX2 image header");
    return;
  }
  if (bytes.empty() || bytes.size() > INT_MAX) throw std::runtime_error("invalid image size");
  int width = 0, height = 0, channels = 0;
  auto* pixels = stbi_load_from_memory(bytes.data(),int(bytes.size()),&width,&height,&channels,4);
  if (!pixels) throw std::runtime_error("cannot decode referenced glTF image");
  stbi_image_free(pixels);
}
}
// Parse and validate before gltfio can encounter its fixed skin palette limit.
// Check every declared skin, including ones referenced by secondary meshes/scenes.
void validateSceneFile(const std::string& path) {
  auto ext = fs::path(path).extension().string();
  std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){ return char(std::tolower(c)); });
  if (ext != ".glb" && ext != ".gltf") throw std::runtime_error("choose a .glb or .gltf file");
  cgltf_options options{};
  cgltf_data* raw = nullptr;
  const auto result = cgltf_parse_file(&options,path.c_str(),&raw);
  if (result != cgltf_result_success) throw std::runtime_error("cannot parse glTF (code " + std::to_string(result) + ")");
  std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,&cgltf_free);
  std::vector<size_t> skinCounts;
  for (size_t i=0;i<data->skins_count;++i) skinCounts.push_back(data->skins[i].joints_count);
  if (!skinCompatible(skinCounts))
    throw std::runtime_error("unsupported skin: more than 256 joints. Animated assets cannot be silently baked; use a compatible asset or an explicitly baked static copy.");
  if (cgltf_load_buffers(&options,data.get(),path.c_str()) != cgltf_result_success) throw std::runtime_error("cannot read glTF buffers");
  if (cgltf_validate(data.get()) != cgltf_result_success) throw std::runtime_error("invalid glTF data");
  for (size_t i = 0; i < data->images_count; ++i) {
    const auto& image = data->images[i];
    bool referenced = false;
    for (size_t t = 0; t < data->textures_count; ++t) {
      const auto& texture = data->textures[t];
      referenced |= texture.image == &image || texture.basisu_image == &image || texture.webp_image == &image;
    }
    std::vector<unsigned char> bytes;
    if (image.uri && std::strncmp(image.uri,"data:",5) != 0) {
      std::string uri = image.uri;
      cgltf_decode_uri(uri.data()); uri.resize(std::strlen(uri.c_str()));
      const auto imagePath = fs::path(path).parent_path() / uri;
      std::ifstream file(imagePath,std::ios::binary);
      if (!file || file.peek() == std::ifstream::traits_type::eof()) throw std::runtime_error("cannot read glTF image " + imagePath.string());
      if (referenced) bytes.assign(std::istreambuf_iterator<char>(file),{});
    } else if (referenced && image.uri) {
      const char* comma = std::strchr(image.uri,',');
      if (!comma || std::string(image.uri,size_t(comma-image.uri)).find(";base64") == std::string::npos) throw std::runtime_error("unsupported image data URI");
      const char* payload = comma+1;
      const size_t length = std::strlen(payload);
      if (length < 4 || length%4) throw std::runtime_error("invalid image data URI");
      const size_t count = length/4*3 - (payload[length-1]=='=') - (payload[length-2]=='=');
      void* decoded = nullptr;
      if (cgltf_load_buffer_base64(&options,count,payload,&decoded) != cgltf_result_success) throw std::runtime_error("invalid image base64");
      std::unique_ptr<void,decltype(&std::free)> owner(decoded,&std::free);
      const auto* start = static_cast<const unsigned char*>(decoded);
      bytes.assign(start,start+count);
    } else if (referenced && image.buffer_view) {
      const auto* start = cgltf_buffer_view_data(image.buffer_view);
      if (!start) throw std::runtime_error("cannot read embedded image");
      bytes.assign(start,start+image.buffer_view->size);
    }
    if (referenced) validateImage(bytes);
  }
}
}
