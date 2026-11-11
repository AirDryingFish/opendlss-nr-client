#pragma once

#include "model_file_listing.h"

#include <array>
#include <functional>
#include <string>
#include <utility>

namespace client {

// Draw once per ImGui frame, after the viewer's main window has ended. Opening
// and cancellation are deferred so popup creation/closure share draw's ID stack.
class ModelFileBrowser {
 public:
  explicit ModelFileBrowser(std::filesystem::path assetsRoot);
  void open(std::filesystem::path initialDirectory);
  bool isOpen() const { return openRequested_ || open_; }
  void cancel() { if (isOpen()) cancelRequested_ = true; }
  std::string draw();  // Canonical selected model path, or empty.
  const std::filesystem::path& directory() const { return directory_; }

  // Optional item observer; invoked immediately after each interactive item.
  std::function<void(const char* action,const std::string& value)> observe;

 private:
  bool navigate(const std::filesystem::path& directory);
  void go();
  std::string activateSelection();
  void notify(const char* action,const std::string& value) { if (observe) observe(action,value); }

  std::filesystem::path assetsRoot_, initialDirectory_, directory_, selection_;
  std::vector<ModelFileEntry> entries_;
  std::array<char,4096> pathInput_{};
  std::string selectedName_, error_;
  bool openRequested_ = false, open_ = false, cancelRequested_ = false;
};

}  // namespace client
