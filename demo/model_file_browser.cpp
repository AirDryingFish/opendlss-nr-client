#include "model_file_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>

namespace client {
namespace fs = std::filesystem;

ModelFileBrowser::ModelFileBrowser(fs::path assetsRoot) : assetsRoot_(std::move(assetsRoot)) {}

void ModelFileBrowser::open(fs::path initialDirectory) {
  initialDirectory_ = std::move(initialDirectory);
  openRequested_ = true;
  cancelRequested_ = false;
}

bool ModelFileBrowser::navigate(const fs::path& directory) {
  auto listing = listModelDirectory(directory);
  if (!listing.error.empty()) { error_ = std::move(listing.error); return false; }
  const std::string text = listing.directory.string();
  if (text.size()>=pathInput_.size()) { error_ = "Directory path is too long"; return false; }
  directory_ = std::move(listing.directory);
  entries_ = std::move(listing.entries);
  pathInput_.fill('\0');
  std::copy(text.begin(),text.end(),pathInput_.begin());
  selection_.clear();
  selectedName_.clear();
  error_.clear();
  return true;
}

void ModelFileBrowser::go() {
  if (!pathInput_[0]) { error_ = "Enter a directory or model path"; return; }
  fs::path requested(pathInput_.data());
  if (requested.is_relative() && !directory_.empty()) requested = directory_ / requested;
  std::error_code error;
  if (fs::is_regular_file(requested,error) && !error) {
    try {
      const fs::path selected = checkedModelFile(requested);
      if (navigate(selected.parent_path())) {
        selection_ = selected;
        selectedName_ = selected.filename().string();
      }
    } catch (const std::exception& e) { error_ = e.what(); }
  } else {
    navigate(requested);
  }
}

std::string ModelFileBrowser::activateSelection() {
  if (selection_.empty()) return {};
  std::error_code error;
  if (fs::is_directory(selection_,error) && !error) { navigate(selection_); return {}; }
  try { return checkedModelFile(selection_).string(); }
  catch (const std::exception& e) { error_ = e.what(); return {}; }
}

std::string ModelFileBrowser::draw() {
  constexpr const char* title = "Open GLB / glTF model###model-file-browser";
  if (openRequested_ && cancelRequested_) {
    openRequested_ = cancelRequested_ = open_ = false;
    return {};
  }
  if (openRequested_) {
    openRequested_ = false;
    open_ = true;
    selection_.clear();
    selectedName_.clear();
    error_.clear();
    navigate(initialDirectory_.empty() ? assetsRoot_ : initialDirectory_);
    ImGui::OpenPopup(title);
  }
  if (!open_) return {};

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x*0.5f,display.y*0.5f),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::max(32.0f,std::min(720.0f,display.x-20.0f)),
      std::max(32.0f,std::min(520.0f,display.y-20.0f))),ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(title,&open_,ImGuiWindowFlags_NoSavedSettings)) {
    if (!ImGui::IsPopupOpen(title)) open_ = false;
    return {};
  }
  if (cancelRequested_) {
    cancelRequested_ = false;
    open_ = false;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return {};
  }

  const bool assets = ImGui::Button("Assets");
  notify("assets",assetsRoot_.string());
  if (assets) navigate(assetsRoot_);
  ImGui::SameLine();
  const char* home = std::getenv("HOME");
  const bool homeClicked = ImGui::Button("Home");
  notify("home",home ? home : "");
  if (homeClicked) {
    if (home && *home) navigate(home);
    else error_ = "Home directory is not available";
  }
  ImGui::SameLine();
  const fs::path parent = directory_.parent_path();
  ImGui::BeginDisabled(directory_.empty() || parent==directory_);
  const bool up = ImGui::Button("Up");
  notify("up",parent.string());
  ImGui::EndDisabled();
  if (up) navigate(parent);

  const float goWidth = ImGui::CalcTextSize("Go").x + ImGui::GetStyle().FramePadding.x*2;
  ImGui::SetNextItemWidth(-goWidth-ImGui::GetStyle().ItemSpacing.x);
  const bool entered = ImGui::InputText("##directory",pathInput_.data(),pathInput_.size(),ImGuiInputTextFlags_EnterReturnsTrue);
  notify("path",pathInput_.data());
  ImGui::SameLine();
  const bool goClicked = ImGui::Button("Go");
  notify("go",pathInput_.data());
  if (entered || goClicked) go();
  ImGui::TextUnformatted("GLB / glTF files. Double-click a folder or model, or select and press Open.");

  bool activate = false;
  const float errorHeight = error_.empty() ? 0.0f : ImGui::CalcTextSize(error_.c_str(),nullptr,false,
      ImGui::GetContentRegionAvail().x).y + ImGui::GetStyle().ItemSpacing.y;
  const float footerHeight = ImGui::GetTextLineHeightWithSpacing() + errorHeight +
      ImGui::GetFrameHeightWithSpacing();
  ImGui::BeginChild("model-files",ImVec2(0,std::max(1.0f,ImGui::GetContentRegionAvail().y-footerHeight)),ImGuiChildFlags_Borders);
  for (size_t index=0;index<entries_.size();++index) {
    const auto& entry = entries_[index];
    const std::string canonical = entry.path.string();
    ImGui::PushID(canonical.c_str());
    // Also distinguish aliases and literal ### strings, which ImGui's string
    // hashing otherwise treats as an ID suffix separator.
    ImGui::PushID(static_cast<int>(index));
    const ImVec2 labelPosition = ImGui::GetCursorScreenPos();
    const bool selected = entry.path==selection_ && entry.name==selectedName_;
    const bool clicked = ImGui::Selectable("##entry",selected,ImGuiSelectableFlags_AllowDoubleClick,
        ImVec2(0,ImGui::GetTextLineHeight()));
    notify("entry",canonical);
    const bool activated = clicked && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter));
    if (clicked) { selection_ = entry.path; selectedName_ = entry.name; error_.clear(); }
    const std::string label = entry.directory ? "[dir] " + entry.name : entry.name;
    ImGui::GetWindowDrawList()->AddText(labelPosition,ImGui::GetColorU32(ImGuiCol_Text),label.c_str());
    ImGui::PopID();
    ImGui::PopID();
    if (activated) { activate = true; break; }
  }
  if (entries_.empty()) ImGui::TextUnformatted("No folders or GLB / glTF files in this directory.");
  ImGui::EndChild();

  ImGui::TextUnformatted(selectedName_.empty() ? "No model selected" : selectedName_.c_str());
  if (!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
  ImGui::BeginDisabled(selection_.empty());
  const bool openClicked = ImGui::Button("Open");
  notify("open",selection_.string());
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool cancelClicked = ImGui::Button("Cancel");
  notify("cancel",{});

  std::string chosen;
  if (activate || openClicked) chosen = activateSelection();
  if (!chosen.empty() || cancelClicked) {
    open_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  return chosen;
}

}  // namespace client
