#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Specify exactly one source: root coordinates "x,y,w,h" or a window XID
// (decimal or 0x-prefixed hex). Either argument also accepts "pick".
class LiveX11 {
 public:
  struct SelectionCancelled : std::runtime_error {
    SelectionCancelled() : std::runtime_error("X11 source selection cancelled") {}
  };

  LiveX11(std::string region, std::string window);
  ~LiveX11();

  // Register the SDL preview after its renderer has created its final X11 window.
  void setPreviewWindow(unsigned long xid);

  enum class ReadResult { Frame, Unavailable, PreviewOverlap };
  // Top-down, tightly packed display-encoded RGBA8, with opaque alpha.
  // Only Frame updates the output. A minimized source yields Unavailable.
  // PreviewOverlap holds region capture while the preview can cover its input.
  // Closed source windows and capture errors throw std::runtime_error.
  ReadResult read(std::vector<uint8_t>& rgba);
  int width() const noexcept;
  int height() const noexcept;
  // Initial content origin in root coordinates, useful for placing a preview.
  int x() const noexcept;
  int y() const noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
