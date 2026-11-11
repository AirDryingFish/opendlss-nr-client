#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.h"

namespace client {

struct EnvironmentPreset {
  std::string id;
  std::string name;
  std::string path;
};

// The manifest is optional. Invalid manifests fail as a whole so an unsafe or
// ambiguous entry cannot silently change which preset an ID selects.
inline std::vector<EnvironmentPreset> loadEnvironmentPresets(const std::string& manifestPath) {
  namespace fs = std::filesystem;
  const fs::path manifest(manifestPath);
  if (!fs::exists(manifest)) return {};
  if (!fs::is_regular_file(manifest))
    throw std::runtime_error("environment preset manifest is not a file: " + manifestPath);
  std::ifstream file(manifest, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read environment preset manifest: " + manifestPath);
  const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const json::Value root = json::parse(bytes);
  if (root.kind != json::Value::Object || !root.has("version") ||
      root["version"].kind != json::Value::Number || root["version"].number != 1 ||
      !root.has("presets") || root["presets"].kind != json::Value::Array)
    throw std::runtime_error("invalid environment preset manifest: " + manifestPath);

  const fs::path base = fs::canonical(fs::absolute(manifest).parent_path());
  std::vector<EnvironmentPreset> presets;
  std::set<std::string> ids;
  for (const auto& entry : root["presets"].array) {
    auto text = [&](const char* key) {
      if (entry.kind != json::Value::Object || !entry.has(key) ||
          entry[key].kind != json::Value::String || entry[key].str().empty())
        throw std::runtime_error(std::string("invalid environment preset ") + key + " in " + manifestPath);
      const std::string value = entry[key].str();
      if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        throw std::runtime_error(std::string("invalid environment preset ") + key + " in " + manifestPath);
      return value;
    };
    EnvironmentPreset preset{text("id"), text("name"), {}};
    if (!ids.insert(preset.id).second)
      throw std::runtime_error("duplicate environment preset ID: " + preset.id);

    const std::string relativeName = text("path");
    const fs::path relative(relativeName);
    if (relative.is_absolute() || relative.has_root_path() ||
        relativeName.find(':') != std::string::npos || relativeName.find('\\') != std::string::npos)
      throw std::runtime_error("environment preset path must be relative: " + relativeName);
    for (const auto& component : relative)
      if (component == "..") throw std::runtime_error("environment preset path escapes its directory: " + relativeName);
    std::string extension = relative.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".hdr" || !fs::is_regular_file(base / relative))
      throw std::runtime_error("environment preset must name an existing HDR file: " + relativeName);
    const fs::path resolved = fs::canonical(base / relative);
    const fs::path confined = resolved.lexically_relative(base);
    if (confined.empty() || confined.is_absolute() || *confined.begin() == "..")
      throw std::runtime_error("environment preset path escapes its directory: " + relativeName);
    preset.path = resolved.string();
    presets.push_back(std::move(preset));
  }
  return presets;
}

}  // namespace client
