#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace client {

struct AssetCatalogEntry {
  std::string name;      // Stable root-relative label, including the extension.
  std::string model;     // Canonical absolute model path.
  std::string viewJson;  // Canonical sibling view path, or empty; not parsed here.
};

// Discovery does not decode models or views. An invalid view therefore affects
// only scene import for that entry. Directory symlinks are not followed, while
// regular-file symlink aliases are accepted and deduplicated by canonical path.
// Missing/unreadable roots and unreadable entries are optional and skipped.
inline std::vector<AssetCatalogEntry> discoverAssets(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::error_code error;
  const fs::path base = fs::canonical(root,error);
  if (error || !fs::is_directory(base,error) || error) return {};
  auto supportedExtension = [](const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".glb" || extension == ".gltf";
  };

  std::map<std::string,AssetCatalogEntry> models;
  std::vector<fs::path> directories{base};
  while (!directories.empty()) {
    const fs::path directory = std::move(directories.back());
    directories.pop_back();
    fs::directory_iterator current(directory,fs::directory_options::skip_permission_denied,error), end;
    if (error) { error.clear(); continue; }
    while (current != end) {
      const fs::directory_entry entry = *current;
      auto inspect = [&]() {
        std::error_code ec;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec) return;
        if (fs::is_directory(status)) {
          directories.push_back(entry.path());
          return;
        }
        if (!entry.is_regular_file(ec) || ec) return;
        if (!supportedExtension(entry.path())) return;
        const fs::path model = fs::canonical(entry.path(),ec);
        // Import receives the canonical spelling, so it must also carry a
        // supported suffix (not e.g. model.glb -> payload.dat).
        if (ec || !supportedExtension(model)) return;
        fs::path relative = model.lexically_relative(base);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
          relative = entry.path().lexically_relative(base);
        AssetCatalogEntry candidate{relative.generic_string(),model.string(),{}};
        const fs::path view = model.parent_path() / "view.json";
        if (fs::is_regular_file(view,ec) && !ec) {
          const fs::path resolvedView = fs::canonical(view,ec);
          if (!ec) candidate.viewJson = resolvedView.string();
        }
        const auto found = models.find(candidate.model);
        if (found == models.end()) models.emplace(candidate.model,std::move(candidate));
        else if (candidate.name < found->second.name) found->second = std::move(candidate);
      };
      inspect();
      current.increment(error);
      if (error) { error.clear(); break; }
    }
  }

  std::vector<AssetCatalogEntry> entries;
  entries.reserve(models.size());
  for (auto& pair : models) entries.push_back(std::move(pair.second));
  std::sort(entries.begin(),entries.end(),[](const AssetCatalogEntry& a, const AssetCatalogEntry& b) {
    return a.name < b.name || (a.name == b.name && a.model < b.model);
  });
  return entries;
}

}  // namespace client
