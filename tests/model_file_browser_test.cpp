// Test the real browser filesystem boundary without ImGui, Filament or a GPU.
#include "model_file_listing.h"

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
  const fs::path scratch = fs::temp_directory_path() / ("nr browser with spaces " +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path directory = scratch / "outside assets";
  fs::create_directories(directory / "Zebra");
  fs::create_directories(directory / "alpha.GLB");
  fs::create_directories(scratch / "target directory");
  auto write = [](const fs::path& path) { std::ofstream(path) << "model decoding belongs to scene import"; };
  write(directory / "Model##one.GLB");
  write(directory / "a.gLtF");
  write(directory / "ignore.txt");
  write(directory / "payload.dat");
  write(scratch / "external.glb");

  try {
    fs::create_directory_symlink(scratch / "target directory",directory / "linked folder");
    fs::create_symlink(scratch / "external.glb",directory / "External.GLB");
    fs::create_symlink(directory / "payload.dat",directory / "unsupported.glb");
    fs::create_symlink(directory / "missing.glb",directory / "broken.glb");
    const auto listing = client::listModelDirectory(directory);
    require(listing.error.empty() && listing.directory==fs::canonical(directory),
        "valid browser directory did not resolve to a canonical directory");
    const std::vector<std::string> names{"alpha.GLB","linked folder","Zebra","a.gLtF","External.GLB","Model##one.GLB"};
    require(listing.entries.size()==names.size(),"browser must show directories and supported model files only");
    for (size_t i=0;i<names.size();++i) {
      require(listing.entries[i].name==names[i],"browser sorting must put directories first, then fold filename case");
      require(listing.entries[i].directory==(i<3),"browser directory classification is wrong");
      require(listing.entries[i].path==fs::canonical(directory / names[i]),
          "browser entry paths must be canonical without changing their displayed names");
    }
    require(client::listModelDirectory(directory / "missing").error.size()>0,
        "missing browser directory must have a visible error");
    require(client::listModelDirectory(directory / "a.gLtF").error.size()>0,
        "opening a file as a directory must have a visible error");
    require(client::listModelDirectory(directory / "linked folder").directory==fs::canonical(scratch / "target directory"),
        "browser must allow navigation through an external directory symlink");
    require(client::checkedModelFile(directory / "Model##one.GLB")==fs::canonical(directory / "Model##one.GLB"),
        "Open must preserve legal filename punctuation and case-insensitive extensions");
    require(client::checkedModelFile(directory / "External.GLB")==fs::canonical(scratch / "external.glb"),
        "Open must allow models outside bundled assets");

    for (const auto& path : {directory / "missing.glb",directory / "ignore.txt",directory / "alpha.GLB",
                             directory / "unsupported.glb",directory / "broken.glb"}) {
      bool rejected = false;
      try { client::checkedModelFile(path); }
      catch (const std::exception& e) { rejected = std::string(e.what()).find(path.string())!=std::string::npos; }
      require(rejected,"Open must reject missing/unsupported/non-file selections with their path");
    }
    fs::remove(directory / "Model##one.GLB");
    bool removedRejected = false;
    try { client::checkedModelFile(listing.entries.back().path); }
    catch (const std::exception&) { removedRejected = true; }
    require(removedRejected,"Open must recheck a file removed after its directory was listed");
    fs::remove_all(scratch);
    std::cout << "model file browser: listing, filters, names, navigation and Open boundary passed\n";
  } catch (...) {
    fs::remove_all(scratch);
    throw;
  }
}
