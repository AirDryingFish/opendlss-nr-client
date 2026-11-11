#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace client {

struct ModelFileEntry {
  std::filesystem::path path;
  std::string name;
  bool directory = false;
};

struct ModelDirectoryListing {
  std::filesystem::path directory;
  std::vector<ModelFileEntry> entries;
  std::string error;
};

inline bool hasModelFileExtension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(),extension.end(),extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension==".glb" || extension==".gltf";
}

// Check again when Open is pressed: the cached directory entry may have changed.
// Full glTF parsing and texture validation remain the scene importer's job.
inline std::filesystem::path checkedModelFile(const std::filesystem::path& path) {
  namespace fs = std::filesystem;
  std::error_code error;
  if (!hasModelFileExtension(path) || !fs::is_regular_file(path,error) || error)
    throw std::runtime_error("Choose an existing GLB or glTF file: " + path.string());
  const fs::path resolved = fs::canonical(path,error);
  // The importer receives the canonical spelling and checks its suffix too.
  if (error || !hasModelFileExtension(resolved))
    throw std::runtime_error("Cannot open GLB or glTF file: " + path.string());
  return resolved;
}

inline ModelDirectoryListing listModelDirectory(const std::filesystem::path& directory) {
  namespace fs = std::filesystem;
  ModelDirectoryListing listing;
  std::error_code error;
  listing.directory = fs::canonical(directory,error);
  if (error || !fs::is_directory(listing.directory,error) || error) {
    listing.error = "Cannot browse directory: " + directory.string();
    return listing;
  }
  fs::directory_iterator current(listing.directory,error), end;
  if (error) {
    listing.error = "Cannot read directory " + directory.string() + ": " + error.message();
    return listing;
  }
  while (current!=end) {
    const auto& entry = *current;
    std::error_code entryError;
    const bool isDirectory = entry.is_directory(entryError);
    if (!entryError && isDirectory) {
      const fs::path resolved = fs::canonical(entry.path(),entryError);
      if (!entryError) listing.entries.push_back({resolved,entry.path().filename().string(),true});
    } else if (!entryError && hasModelFileExtension(entry.path())) {
      try { listing.entries.push_back({checkedModelFile(entry.path()),entry.path().filename().string(),false}); }
      catch (const std::runtime_error&) {}  // Broken links or entries that disappeared are not selectable.
    }
    current.increment(error);
    if (error) {
      listing.error = "Cannot read directory " + directory.string() + ": " + error.message();
      break;
    }
  }
  auto folded = [](std::string name) {
    std::transform(name.begin(),name.end(),name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
  };
  std::sort(listing.entries.begin(),listing.entries.end(),[&](const ModelFileEntry& a,const ModelFileEntry& b) {
    if (a.directory!=b.directory) return a.directory;
    const auto left = folded(a.name), right = folded(b.name);
    return left!=right ? left<right : a.name<b.name;
  });
  return listing;
}

}  // namespace client
