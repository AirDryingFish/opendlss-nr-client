// Asset discovery exercises real files and links; no renderer or GPU is needed.
#include "asset_catalog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  const fs::path scratch = fs::temp_directory_path() / ("nr asset catalog with spaces " +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path root = scratch / "assets";
  fs::create_directories(root / "pack with spaces/deep");
  fs::create_directories(root / "bad view");
  fs::create_directories(root / "zeta/view.json");
  auto write = [](const fs::path& path, const std::string& bytes) {
    std::ofstream(path,std::ios::binary) << bytes;
  };
  write(root / "root.GLB","asset decoding belongs to scene import");
  write(root / "pack with spaces/deep/Character.GlTf","asset decoding belongs to scene import");
  write(root / "pack with spaces/deep/view.json",R"({"name":"Authored camera","camera":{"position":[1,2,3]}})");
  write(root / "bad view/Scene.glb","asset decoding belongs to scene import");
  write(root / "bad view/view.json","malformed JSON must not poison discovery");
  write(root / "zeta/Model.glb","binary variant");
  write(root / "zeta/Model.gltf","text variant");
  write(root / "ignore.png","not a scene");
  write(scratch / "payload.dat","a canonical target with an unsupported extension");

  try {
    require(client::discoverAssets(scratch / "missing").empty(),"missing root must produce an empty catalog");
    require(client::discoverAssets(root / "root.GLB").empty(),"file root must produce an empty catalog");
    fs::create_symlink(root / "pack with spaces/deep/Character.GlTf",root / "duplicate.GLTF");
    fs::create_symlink(root / "absent.glb",root / "missing.GLB");
    fs::create_symlink(scratch / "payload.dat",root / "disguised.glb");
    fs::create_directory_symlink(root,root / "cycle");
    fs::create_directory_symlink(root / "pack with spaces",root / "directory-alias");

    const auto entries = client::discoverAssets(root);
    const std::vector<std::string> names{"bad view/Scene.glb","pack with spaces/deep/Character.GlTf",
        "root.GLB","zeta/Model.glb","zeta/Model.gltf"};
    require(entries.size()==names.size(),"nested scenes, extension cases, or canonical deduplication failed");
    for (size_t i=0;i<entries.size();++i) {
      require(entries[i].name==names[i],"catalog labels/order must be stable relative model paths");
      require(fs::path(entries[i].model)==fs::canonical(root / names[i]),"model path must be canonical and absolute");
    }
    require(fs::path(entries[0].viewJson)==fs::canonical(root / "bad view/view.json"),
        "malformed sibling view must remain per-entry metadata without dropping the model");
    require(fs::path(entries[1].viewJson)==fs::canonical(root / "pack with spaces/deep/view.json"),
        "custom sibling view metadata was lost");
    require(entries[2].viewJson.empty() && entries[3].viewJson.empty() && entries[4].viewJson.empty(),
        "absent views and directories named view.json must not become view files");

    const auto repeated = client::discoverAssets(root / ".." / "assets");
    require(repeated.size()==entries.size(),"equivalent root spelling changed the catalog");
    for (size_t i=0;i<entries.size();++i)
      require(repeated[i].name==entries[i].name && repeated[i].model==entries[i].model &&
          repeated[i].viewJson==entries[i].viewJson,"repeated discovery must be deterministic");
    fs::remove_all(scratch);
    std::cout << "asset catalog: recursive discovery, view metadata, aliases and cycles passed\n";
  } catch (const std::exception& e) {
    fs::remove_all(scratch);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
