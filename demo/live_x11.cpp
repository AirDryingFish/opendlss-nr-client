#include "live_x11.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/keysym.h>
#ifdef LIVE_X11_SHM
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

Rect parseRegion(std::string_view value) {
  std::array<int, 4> parts{};
  for (size_t i = 0; i < parts.size(); ++i) {
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parts[i]);
    value.remove_prefix(static_cast<size_t>(end - value.data()));
    if (error != std::errc{} || (i == 3 ? !value.empty() : !value.starts_with(',')))
      throw std::runtime_error("Expected an X11 region in the form x,y,width,height");
    if (i != 3) value.remove_prefix(1);
  }
  return {parts[0], parts[1], parts[2], parts[3]};
}

Window parseWindow(std::string_view value) {
  const bool hex = value.starts_with("0x") || value.starts_with("0X");
  if (hex) value.remove_prefix(2);
  uint32_t xid = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), xid, hex ? 16 : 10);
  if (error != std::errc{} || end != value.data() + value.size() || xid == None)
    throw std::runtime_error("Expected an X11 window ID in decimal or 0x-prefixed hexadecimal");
  return xid;
}

void checkRegion(Rect r, const XWindowAttributes& root) {
  if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 ||
      r.x > root.width - r.w || r.y > root.height - r.h)
    throw std::runtime_error("The X11 capture region must be nonempty and inside the root window");
  if (r.x > std::numeric_limits<short>::max() || r.y > std::numeric_limits<short>::max())
    throw std::runtime_error("The capture origin exceeds the X11 GetImage coordinate range");
}

bool hasComposite(Display* d, int minimumMinor) {
  int event, error, major, minor;
  return XCompositeQueryExtension(d, &event, &error) && XCompositeQueryVersion(d, &major, &minor) &&
         (major > 0 || minor >= minimumMinor);
}

// A window manager can move the outer frame before the client receives its
// ConfigureNotify. Use the capture connection's view of the frame, decorations
// and border. If it is being reparented/destroyed, keep the last good image.
bool previewMayCover(Display* d, Window root, Window preview, Rect region) {
  Window frame = preview;
  for (;;) {
    Window treeRoot, parent, *children = nullptr;
    unsigned count;
    const bool found = XQueryTree(d, frame, &treeRoot, &parent, &children, &count);
    if (children) XFree(children);
    if (!found) return true;
    if (treeRoot != root) return false;
    if (parent == root) break;
    if (parent == None) return true;
    frame = parent;
  }

  XWindowAttributes a{};
  if (!XGetWindowAttributes(d, frame, &a)) return true;
  if (a.map_state != IsViewable) return false;
  int x, y;
  Window child;
  if (!XTranslateCoordinates(d, frame, root, -a.border_width, -a.border_width, &x, &y, &child))
    return true;
  return x < region.x + region.w && x + a.width + 2 * a.border_width > region.x &&
         y < region.y + region.h && y + a.height + 2 * a.border_width > region.y;
}

// Xlib reports stale drawables through a process-wide handler, even for
// synchronous calls. Only intercept errors on our private display.
class ErrorScope {
 public:
  explicit ErrorScope(Display* d) : lock_(mutex_), display_(d) {
    active_ = this;
    previous_.store(XSetErrorHandler(handler));
  }

  ~ErrorScope() {
    if (!synced_) XSync(display_, False);
    XSetErrorHandler(previous_.load());
    active_ = nullptr;
  }

  std::string message(const char* operation) const {
    if (!error_) return operation;
    char description[256]{};
    XGetErrorText(display_, error_, description, sizeof(description));
    return std::string(operation) + " (" + description + ")";
  }

  // A rejected optional MIT-SHM request may be retried using core X11.
  void clear() {
    error_ = 0;
    synced_ = false;
  }

  bool failed() {
    XSync(display_, False);
    synced_ = true;
    return error_ != 0;
  }

 private:
  static int handler(Display* d, XErrorEvent* event) {
    if (active_ && active_->display_ == d) {
      if (!active_->error_) active_->error_ = event->error_code;
      return 0;
    }
    auto previous = previous_.load();
    return previous ? previous(d, event) : 0;
  }

  static inline std::mutex mutex_;
  static inline thread_local ErrorScope* active_ = nullptr;
  static inline std::atomic<XErrorHandler> previous_{nullptr};
  std::unique_lock<std::mutex> lock_;
  Display* display_;
  int error_ = 0;
  bool synced_ = false;
};

struct SelectionGrab {
  Display* d;
  explicit SelectionGrab(Display* display, Window root) : d(display) {
    Cursor crosshair = XCreateFontCursor(d, XC_crosshair);
    const int result = XGrabPointer(d, root, False, ButtonPressMask | ButtonReleaseMask | Button1MotionMask,
                                    GrabModeAsync, GrabModeAsync, None, crosshair, CurrentTime);
    XFreeCursor(d, crosshair);
    if (result != GrabSuccess)
      throw std::runtime_error("Could not grab the pointer for X11 source selection");
    if (XGrabKeyboard(d, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
      XUngrabPointer(d, CurrentTime);
      throw std::runtime_error("Could not grab the keyboard for X11 source selection");
    }
  }
  ~SelectionGrab() {
    XUngrabKeyboard(d, CurrentTime);
    XUngrabPointer(d, CurrentTime);
    XFlush(d);
  }
};

Rect dragRect(int x, int y, int endX, int endY) {
  return {std::min(x, endX), std::min(y, endY), std::abs(x - endX) + 1, std::abs(y - endY) + 1};
}

// Separate override-redirect windows give the drag outline a light center
// and dark border, without drawing into the application being selected.
struct Outline {
  Display* d;
  Window root;
  std::array<Window, 4> edges{};

  void show(Rect r) {
    const std::array<Rect, 4> positions{{{r.x, r.y, r.w, 1}, {r.x, r.y + r.h - 1, r.w, 1},
                                         {r.x, r.y, 1, r.h}, {r.x + r.w - 1, r.y, 1, r.h}}};
    XSetWindowAttributes a{};
    a.override_redirect = True;
    a.background_pixel = WhitePixel(d, DefaultScreen(d));
    a.border_pixel = BlackPixel(d, DefaultScreen(d));
    for (size_t i = 0; i < edges.size(); ++i) {
      const auto& edge = positions[i];
      if (!edges[i]) {
        edges[i] = XCreateWindow(d, root, edge.x - 1, edge.y - 1, edge.w, edge.h, 1,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel | CWBorderPixel, &a);
        XMapRaised(d, edges[i]);
      } else {
        XMoveResizeWindow(d, edges[i], edge.x - 1, edge.y - 1, edge.w, edge.h);
      }
    }
    XFlush(d);
  }

  ~Outline() {
    for (Window edge : edges)
      if (edge) XDestroyWindow(d, edge);
  }
};

// Return -1 for windows without an ICCCM WM_STATE property.
long windowState(Display* d, Window w, Atom state) {
  if (state == None) return -1;
  Atom type;
  int format;
  unsigned long count, after;
  unsigned char* data = nullptr;
  int result = XGetWindowProperty(d, w, state, 0, 1, False, state, &type, &format, &count, &after, &data);
  const long value = result == Success && type == state && format == 32 && count == 1
                         ? *reinterpret_cast<long*>(data) : -1;
  if (data) XFree(data);
  return value;
}

// Follow the click into the managed client. Title-bar clicks land outside
// its contents, so search the frame's children in that case.
Window pickedClient(Display* d, Window root, Window frame, int x, int y, Atom state) {
  if (state == None) return frame;
  Window child = frame;
  while (child != None) {
    Window w = child;
    if (windowState(d, w, state) >= 0) return w;
    int localX, localY;
    if (!XTranslateCoordinates(d, root, w, x, y, &localX, &localY, &child)) break;
  }

  std::vector<Window> pending{frame};
  for (size_t i = 0; i < pending.size(); ++i) {
    if (i && windowState(d, pending[i], state) >= 0) return pending[i];
    Window parent, treeRoot, *children = nullptr;
    unsigned count = 0;
    if (!XQueryTree(d, pending[i], &treeRoot, &parent, &children, &count)) continue;
    for (unsigned j = 0; j < count; ++j) pending.push_back(children[j]);
    if (children) XFree(children);
  }
  return frame;
}

void pick(Display* d, Window root, bool region, Rect& r, Window& target) {
  SelectionGrab grab(d, root);
  Outline outline{d, root};
  bool pressed = false;
  for (;;) {
    XEvent event{};
    XNextEvent(d, &event);
    if ((event.type == KeyPress && XLookupKeysym(&event.xkey, 0) == XK_Escape) ||
        (event.type == ButtonPress && event.xbutton.button == Button3))
      throw LiveX11::SelectionCancelled();
    if (event.type == ButtonPress && event.xbutton.button == Button1) {
      r.x = event.xbutton.x_root;
      r.y = event.xbutton.y_root;
      target = event.xbutton.subwindow;
      pressed = true;
    }
    if (region && pressed && event.type == MotionNotify) {
      outline.show(dragRect(r.x, r.y, event.xmotion.x_root, event.xmotion.y_root));
    }
    if (pressed && event.type == ButtonRelease && event.xbutton.button == Button1) {
      if (region) {
        r = dragRect(r.x, r.y, event.xbutton.x_root, event.xbutton.y_root);
        target = None;
      } else if (target == None || target == root) {
        throw std::runtime_error("Select a window with the crosshair");
      }
      return;
    }
  }
}

struct ImageDeleter {
  void operator()(XImage* image) const { XDestroyImage(image); }
};
using Image = std::unique_ptr<XImage, ImageDeleter>;

#ifdef LIVE_X11_SHM
bool localShmAvailable(Display* d) {
  sockaddr_storage peer{};
  socklen_t length = sizeof(peer);
  return getpeername(ConnectionNumber(d), reinterpret_cast<sockaddr*>(&peer), &length) == 0 &&
         peer.ss_family == AF_UNIX && XShmQueryExtension(d);
}

struct SharedImage {
  Display* display;
  XShmSegmentInfo segment{};
  Image image;
  bool attached = false;
  bool marked = false;

  explicit SharedImage(Display* d) : display(d) { segment.shmid = -1; }
  ~SharedImage() {
    if (attached) XShmDetach(display, &segment);
    if (segment.shmaddr) shmdt(segment.shmaddr);
    if (!marked && segment.shmid >= 0) shmctl(segment.shmid, IPC_RMID, nullptr);
    if (image) image->data = nullptr;
  }

  bool allocate(const XWindowAttributes& a, Rect r, ErrorScope& errors) {
    image.reset(XShmCreateImage(display, a.visual, a.depth, ZPixmap, nullptr, &segment, r.w, r.h));
    if (!image || image->bytes_per_line <= 0 ||
        size_t(r.h) > std::numeric_limits<size_t>::max() / size_t(image->bytes_per_line))
      return false;
    segment.shmid = shmget(IPC_PRIVATE, size_t(image->bytes_per_line) * r.h, IPC_CREAT | 0600);
    if (segment.shmid < 0) return false;
    void* address = shmat(segment.shmid, nullptr, 0);
    if (address == reinterpret_cast<void*>(-1)) return false;
    image->data = segment.shmaddr = static_cast<char*>(address);
    segment.readOnly = False;
    // Linux permits the server to attach a segment marked for deletion while
    // our mapping is live. The kernel will reclaim it even if capture crashes.
    marked = shmctl(segment.shmid, IPC_RMID, nullptr) == 0;
    if (!marked) return false;
    const bool sent = XShmAttach(display, &segment);
    attached = !errors.failed() && sent;
    errors.clear();
    return attached;
  }
};
#endif

void toRgba(XImage* image, const Visual& visual, std::vector<uint8_t>& rgba) {
  const int w = image->width, h = image->height;
  if (size_t(h) > rgba.max_size() / 4 / size_t(w))
    throw std::runtime_error("The X11 image exceeds the available RGBA buffer size");
  rgba.resize(size_t(w) * h * 4);
  // The usual TrueColor framebuffer is little-endian BGRX, including row padding.
  if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
      visual.red_mask == 0xff0000 && visual.green_mask == 0xff00 && visual.blue_mask == 0xff) {
    for (int y = 0; y < h; ++y) {
      const auto* src = reinterpret_cast<const uint8_t*>(image->data) + size_t(y) * image->bytes_per_line;
      auto* dst = rgba.data() + size_t(y) * w * 4;
      for (int x = 0; x < w; ++x, src += 4, dst += 4) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = 255;
      }
    }
    return;
  }
  const std::array<unsigned long, 3> masks{visual.red_mask, visual.green_mask, visual.blue_mask};
  const std::array<int, 3> shifts{std::countr_zero(masks[0]), std::countr_zero(masks[1]),
                                  std::countr_zero(masks[2])};
  auto* dst = rgba.data();
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x, dst += 4) {
      const unsigned long pixel = XGetPixel(image, x, y);
      for (size_t c = 0; c < masks.size(); ++c) {
        const auto maximum = masks[c] >> shifts[c];
        const auto value = (pixel & masks[c]) >> shifts[c];
        dst[c] = uint8_t((uint64_t(value) * 255 + maximum / 2) / maximum);
      }
      dst[3] = 255;
    }
  }
}

}  // namespace

struct LiveX11::State {
  std::unique_ptr<Display, decltype(&XCloseDisplay)> display{nullptr, &XCloseDisplay};
  Window root = None, target = None, preview = None;
  Atom wmState = None;
  Rect rect;
#ifdef LIVE_X11_SHM
  bool useShm = false;
  std::unique_ptr<SharedImage> shared;
#endif

  XWindowAttributes attributes() const {
    XWindowAttributes a{};
    if (!XGetWindowAttributes(display.get(), target ? target : root, &a))
      throw std::runtime_error(target ? "X11 capture window was closed or the XID is invalid"
                                      : "Could not query the X11 root window");
    return a;
  }
};

LiveX11::LiveX11(std::string region, std::string window) : state_(std::make_unique<State>()) {
  if (region.empty() == window.empty())
    throw std::runtime_error("Specify exactly one X11 capture region or window");
  State& s = *state_;
  s.display.reset(XOpenDisplay(nullptr));
  Display* d = s.display.get();
  if (!d) throw std::runtime_error("Could not open the X11 display; check DISPLAY and X server access");
  s.root = DefaultRootWindow(d);
#ifdef LIVE_X11_SHM
  s.useShm = localShmAvailable(d);
#endif
  if (region == "pick" || window == "pick") {
    pick(d, s.root, !region.empty(), s.rect, s.target);
  } else if (!region.empty()) {
    s.rect = parseRegion(region);
  } else {
    s.target = parseWindow(window);
  }

  ErrorScope errors(d);
  if (s.target) s.wmState = XInternAtom(d, "WM_STATE", True);
  if (window == "pick") s.target = pickedClient(d, s.root, s.target, s.rect.x, s.rect.y, s.wmState);
  auto a = s.attributes();
  if (a.c_class != InputOutput || a.visual->c_class != TrueColor ||
      !a.visual->red_mask || !a.visual->green_mask || !a.visual->blue_mask)
    throw std::runtime_error("X11 capture requires a TrueColor input/output window");
  if (s.target) {
    if (s.target == a.root)
      throw std::runtime_error("Use a capture region to capture the X11 root window");
    if (!hasComposite(d, 2))
      throw std::runtime_error("Window capture requires XComposite 0.2 or newer; use a capture region");
    Window child;
    XTranslateCoordinates(d, s.target, a.root, 0, 0, &s.rect.x, &s.rect.y, &child);
    s.rect.w = a.width;
    s.rect.h = a.height;
    // Automatic redirection keeps the window visible and provides a backing
    // pixmap even when a different application covers the source.
    XCompositeRedirectWindow(d, s.target, CompositeRedirectAutomatic);
    if (errors.failed()) throw std::runtime_error(errors.message("Could not redirect the X11 window"));
  } else {
    checkRegion(s.rect, a);
  }
}

LiveX11::~LiveX11() = default;
int LiveX11::width() const noexcept { return state_->rect.w; }
int LiveX11::height() const noexcept { return state_->rect.h; }
int LiveX11::x() const noexcept { return state_->rect.x; }
int LiveX11::y() const noexcept { return state_->rect.y; }

void LiveX11::setPreviewWindow(unsigned long xid) {
  State& s = *state_;
  if (s.preview == xid) return;
  s.preview = xid;
  if (xid == None) return;

  Display* d = s.display.get();
  ErrorScope errors(d);
  // A window move can expose fresh pixels between presents. Let the X server
  // restore them immediately from the current preview, also for window sources.
  if (hasComposite(d, 0)) {
    XCompositeRedirectWindow(d, xid, CompositeRedirectAutomatic);
    if (!errors.failed()) return;
    errors.clear();
  }
  XSetWindowAttributes a{};
  a.backing_store = Always;
  XChangeWindowAttributes(d, xid, CWBackingStore, &a);
  if (errors.failed())
    throw std::runtime_error(errors.message("Could not request backing storage for the X11 preview"));
}

LiveX11::ReadResult LiveX11::read(std::vector<uint8_t>& rgba) {
  State& s = *state_;
  Display* d = s.display.get();
  // The source can resize or unmap between the attribute and image requests.
  for (int attempt = 0; ; ++attempt) {
    ErrorScope errors(d);
    auto a = s.attributes();
    Rect pixels = s.rect;
    if (s.target) {
      if (a.map_state != IsViewable || windowState(d, s.target, s.wmState) == IconicState)
        return ReadResult::Unavailable;
      pixels = {a.border_width, a.border_width, a.width, a.height};
    } else {
      checkRegion(pixels, a);
      if (s.preview && previewMayCover(d, s.root, s.preview, pixels))
        return ReadResult::PreviewOverlap;
    }

    Image ordinary;
    XImage* image = nullptr;
#ifdef LIVE_X11_SHM
    if (s.useShm && (!s.shared || s.shared->image->width != pixels.w || s.shared->image->height != pixels.h)) {
      s.shared.reset();
      s.shared = std::make_unique<SharedImage>(d);
      if (!s.shared->allocate(a, pixels, errors)) {
        s.shared.reset();
        s.useShm = false;
      }
    }
#endif
    // A resize/remap may replace the backing pixmap while keeping the XID.
    Drawable drawable = s.target ? XCompositeNameWindowPixmap(d, s.target) : s.root;
#ifdef LIVE_X11_SHM
    if (s.shared) {
      if (XShmGetImage(d, drawable, s.shared->image.get(), pixels.x, pixels.y, AllPlanes)) {
        image = s.shared->image.get();
      } else {
        errors.clear();
      }
    }
#endif
    if (!image) {
      ordinary.reset(XGetImage(d, drawable, pixels.x, pixels.y, pixels.w, pixels.h, AllPlanes, ZPixmap));
      image = ordinary.get();
#ifdef LIVE_X11_SHM
      // Keep the fast path after transient geometry errors. If only the core
      // request succeeded, this server cannot read this drawable through SHM.
      if (image && s.shared) {
        s.shared.reset();
        s.useShm = false;
      }
#endif
    }
    if (s.target && drawable) XFreePixmap(d, drawable);
    if (!image || errors.failed()) {
      if (s.target && attempt == 0) continue;
      throw std::runtime_error(errors.message("Could not read the X11 capture pixels"));
    }
    // The WM may have moved our output onto the source during GetImage.
    if (!s.target && s.preview && previewMayCover(d, s.root, s.preview, pixels))
      return ReadResult::PreviewOverlap;
    toRgba(image, *a.visual, rgba);
    s.rect.w = pixels.w;
    s.rect.h = pixels.h;
    return ReadResult::Frame;
  }
}
