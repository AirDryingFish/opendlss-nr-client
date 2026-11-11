#include "client_controls.h"
#include <cgltf.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cctype>
#include <memory>
namespace client {
namespace fs = std::filesystem;
// Parse and validate before gltfio can encounter its fixed skin palette limit.
// Check every declared skin, including ones referenced by secondary meshes/scenes.
void validateSceneFile(const std::string& path) {
  auto ext = fs::path(path).extension().string();
  std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){ return char(std::tolower(c)); });
  if (ext != ".glb" && ext != ".gltf")
    throw std::runtime_error("choose a .glb or .gltf file");
  cgltf_options options{};
  cgltf_data* raw = nullptr;
  const auto result = cgltf_parse_file(&options,path.c_str(),&raw);
  if (result != cgltf_result_success) throw std::runtime_error("cannot parse glTF (code " + std::to_string(result) + ")");
  std::unique_ptr<cgltf_data,decltype(&cgltf_free)> data(raw,&cgltf_free);
  std::vector<size_t> skinCounts;
  for (size_t i=0;i<data->skins_count;++i) skinCounts.push_back(data->skins[i].joints_count);
  if (!client::skinCompatible(skinCounts))
    throw std::runtime_error("unsupported skin: more than 256 joints. Animated assets cannot be silently baked; use a compatible asset or an explicitly baked static copy.");
  if (cgltf_load_buffers(&options,data.get(),path.c_str()) != cgltf_result_success)
    throw std::runtime_error("cannot read glTF buffers");
  for (size_t i = 0; i < data->images_count; ++i) {
    const auto& image = data->images[i];
    if (!image.uri || std::strncmp(image.uri,"data:",5) == 0) continue;
    std::string uri = image.uri;
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    const auto imagePath = fs::path(path).parent_path() / uri;
    std::ifstream file(imagePath,std::ios::binary);
    if (!file || file.peek() == std::ifstream::traits_type::eof())
      throw std::runtime_error("cannot read glTF image " + imagePath.string());
  }
  if (cgltf_validate(data.get()) != cgltf_result_success) throw std::runtime_error("invalid glTF data");
}

}
