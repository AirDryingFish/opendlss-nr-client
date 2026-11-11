// Exercise preset discovery and path confinement without Filament or a GPU.
#include "environment_presets.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  const auto scratch = fs::temp_directory_path() / ("nr presets with spaces " +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(scratch / "environments/nested");
  const auto directory = scratch / "environments";
  const auto manifest = directory / "presets.json";
  auto write = [](const fs::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
  };
  write(directory / "studio.hdr", "HDR bytes are decoded by loadEnvironmentPixels");
  write(directory / "nested/sunset.HDR", "HDR bytes are decoded by loadEnvironmentPixels");
  write(scratch / "outside.hdr", "outside the environment directory");
  auto entry = [](const std::string& id, const std::string& path) {
    return "{\"id\":\"" + id + "\",\"name\":\"Display name\",\"path\":\"" + path + "\"}";
  };
  auto save = [&](const std::string& entries) {
    write(manifest, "{\"version\":1,\"presets\":[" + entries + "]}");
  };
  auto reject = [&](const std::string& bytes) {
    write(manifest, bytes);
    bool rejected = false;
    try { client::loadEnvironmentPresets(manifest.string()); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "unsafe or malformed preset manifest was accepted");
  };

  try {
    require(client::loadEnvironmentPresets((directory / "absent.json").string()).empty(),
        "missing optional manifest should produce no presets");
    save(entry("studio", "studio.hdr") + "," + entry("sunset", "nested/sunset.HDR"));
    const auto presets = client::loadEnvironmentPresets(manifest.string());
    require(presets.size() == 2, "valid relative HDR entries were not loaded");
    require(presets[0].id == "studio" && presets[0].name == "Display name",
        "preset identity or display name was lost");
    require(fs::path(presets[0].path) == fs::canonical(directory / "studio.hdr") &&
        fs::path(presets[1].path) == fs::canonical(directory / "nested/sunset.HDR"),
        "preset paths were not resolved relative to the manifest");
    save("");
    require(client::loadEnvironmentPresets(manifest.string()).empty(), "empty preset array should be valid");

    // Removing path confinement would expose files outside the shipped preset directory.
    for (const auto& path : {"../outside.hdr", "nested/../../outside.hdr", "/tmp/outside.hdr",
                            "https://example.com/file.hdr", "C:/outside.hdr", "studio.png", "missing.hdr",
                            "nested", "", "nested\\\\outside.hdr", "studio.hdr\\u0000ignored.hdr"}) {
      reject("{\"version\":1,\"presets\":[" + entry("unsafe", path) + "]}");
    }
    fs::create_symlink(scratch / "outside.hdr", directory / "linked.hdr");
    reject("{\"version\":1,\"presets\":[" + entry("linked", "linked.hdr") + "]}");
    fs::create_directory_symlink(scratch, directory / "linked-directory");
    reject("{\"version\":1,\"presets\":[" + entry("linked-dir", "linked-directory/outside.hdr") + "]}");
    // Duplicate IDs would make GUI/CLI selection depend on entry ordering.
    reject("{\"version\":1,\"presets\":[" + entry("same", "studio.hdr") + "," + entry("same", "nested/sunset.HDR") + "]}");
    for (const auto& bytes : {"not json", "{}", "[]", "{\"version\":2,\"presets\":[]}",
                             "{\"version\":\"1\",\"presets\":[]}", "{\"version\":1,\"presets\":{}}",
                             "{\"version\":1,\"presets\":[3]}",
                             "{\"version\":1,\"presets\":[{\"id\":\"bad\",\"path\":\"studio.hdr\"}]}",
                             "{\"version\":1,\"presets\":[{\"id\":5,\"name\":\"Bad\",\"path\":\"studio.hdr\"}]}",
                             "{\"version\":1,\"presets\":[{\"id\":\"\",\"name\":\"Bad\",\"path\":\"studio.hdr\"}]}"}) {
      reject(bytes);
    }
    if (argc > 1) {
      const auto shipped = client::loadEnvironmentPresets(argv[1]);
      require(shipped.size() >= 4, "shipped manifest should include several environment choices");
    }
    fs::remove_all(scratch);
    std::cout << "environment presets: all checks passed\n";
  } catch (...) {
    fs::remove_all(scratch);
    throw;
  }
}
