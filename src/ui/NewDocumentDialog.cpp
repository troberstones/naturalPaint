#include "ui/NewDocumentDialog.hpp"

#include <cstdlib>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/DocumentPresets.hpp"
#include "io/ClipboardImage.hpp"
#include "io/ImageIO.hpp"

// See ui/NewDocumentDialog.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp.
namespace np {
namespace {

bool g_newDocumentRequested = false;

const ImVec4 kError(0.95f, 0.45f, 0.40f, 1.0f);

}  // namespace

void requestNewDocumentDialog() { g_newDocumentRequested = true; }

void drawNewDocumentDialog(AppState& st) {
  // Session state, function-local statics -- UI state, not app state, the
  // same split every other dialog in ui/MacPaintUI.cpp makes (drawImageSizeDialog,
  // drawExportAsDialog, ...).
  static int width = 1280;
  static int height = 720;
  // The name of the preset last clicked, so the Remove button knows what to
  // remove and the list knows what to highlight. Cleared the moment
  // width/height is hand-edited, because at that point the fields no longer
  // describe that preset -- matching drawExportAsDialog's own "loading a
  // preset fills the fields; editing them is a new, unsaved thing" shape.
  static std::string selectedPresetName;
  static char newPresetNameBuf[96] = "";
  static std::string status;
  static bool statusIsError = false;

  // The clipboard probe is decoded once per dialog-open, not once per frame:
  // probeClipboardImage() decodes whatever image bytes are on the pasteboard,
  // and doing that 60 times a second while this modal sits open would be
  // pointless work for a value that cannot change without the app losing and
  // regaining key focus anyway (see io/ClipboardImage.hpp's own note on
  // SDL's macOS mime-type cache). "Check again" below re-probes on demand.
  static ClipboardImageProbe clipboardProbe;

  if (g_newDocumentRequested) {
    g_newDocumentRequested = false;
    status.clear();
    statusIsError = false;
    selectedPresetName.clear();
    clipboardProbe = probeClipboardImage();
    ImGui::OpenPopup("New Document");
  }
  if (!ImGui::BeginPopupModal("New Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  // Loaded on first open, never at startup (AppState::documentPresetsLoaded):
  // a preset file nobody asked for costs nothing, and --selftest's headless
  // run, which never opens this dialog, must never touch it.
  if (!st.documentPresetsLoaded) {
    st.documentPresetsLoaded = true;
    std::string err;
    if (!st.documentPresets.loadFromFile(defaultDocumentPresetsFilePath(), &err) &&
        !err.empty()) {
      status = err;
      statusIsError = true;
    } else if (!st.documentPresets.problems().empty()) {
      status = st.documentPresets.problems().front();
      statusIsError = true;
    }
  }
  const std::string presetsPath = defaultDocumentPresetsFilePath();

  // --- Presets --------------------------------------------------------------
  ImGui::TextUnformatted("Presets");
  const std::vector<DocumentPreset> presets = st.documentPresets.allPresets();
  bool selectedIsBuiltin = false;
  if (ImGui::BeginListBox("##newDocPresets", ImVec2(320.0f, 140.0f))) {
    for (const DocumentPreset& p : presets) {
      ImGui::PushID(p.name.c_str());
      const bool isSelected = p.name == selectedPresetName;
      char row[160];
      std::snprintf(row, sizeof(row), "%s  (%d x %d)", p.name.c_str(), p.width, p.height);
      if (ImGui::Selectable(row, isSelected)) {
        selectedPresetName = p.name;
        width = p.width;
        height = p.height;
      }
      if (isSelected) selectedIsBuiltin = p.builtin;
      // Visually distinguishes built-in from user presets, per this dialog's
      // brief -- built-ins are never editable or removable, and the tag says
      // so at a glance rather than only when a Remove click is refused.
      ImGui::SameLine();
      ImGui::TextDisabled(p.builtin ? "built-in" : "user");
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }

  // --- Size -------------------------------------------------------------
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::InputInt("Width", &width)) selectedPresetName.clear();
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::InputInt("Height", &height)) selectedPresetName.clear();

  // Never let an invalid size reach document creation -- validated with the
  // exact same function the preset store itself validates a hand-typed
  // `size` line with (app/DocumentPresets.hpp), so this dialog and a
  // hand-edited presets file are held to one rule, not two.
  const std::string sizeError =
      validateDocumentPresetSize(static_cast<int32_t>(width), static_cast<int32_t>(height));
  const bool validSize = sizeError.empty();
  if (!validSize) {
    ImGui::PushStyleColor(ImGuiCol_Text, kError);
    ImGui::TextWrapped("%s", sizeError.c_str());
    ImGui::PopStyleColor();
  }

  if (!validSize) ImGui::BeginDisabled();
  if (ImGui::Button("Create")) {
    st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(width),
                                           static_cast<int32_t>(height), WorkingSpace{}));
    ImGui::CloseCurrentPopup();
  }
  if (!validSize) ImGui::EndDisabled();

  ImGui::Separator();

  // --- Save / remove a user preset ------------------------------------------
  ImGui::TextUnformatted("Save current size as a preset");
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputText("##newPresetName", newPresetNameBuf, sizeof(newPresetNameBuf));
  ImGui::SameLine();
  if (ImGui::Button("Add Preset")) {
    std::string err;
    if (!st.documentPresets.add(newPresetNameBuf, static_cast<int32_t>(width),
                                static_cast<int32_t>(height), &err)) {
      status = err;
      statusIsError = true;
    } else if (!st.documentPresets.saveToFile(presetsPath, &err)) {
      status = err;
      statusIsError = true;
    } else {
      status = std::string("Saved preset '") + newPresetNameBuf + "'.";
      statusIsError = false;
      selectedPresetName = newPresetNameBuf;
      newPresetNameBuf[0] = '\0';
    }
  }

  const bool canRemove = !selectedPresetName.empty() && !selectedIsBuiltin;
  if (!canRemove) ImGui::BeginDisabled();
  if (ImGui::Button("Remove Selected Preset")) {
    std::string err;
    // The store itself refuses a built-in by name (app/DocumentPresets.hpp
    // section 1); canRemove above already keeps this button from firing on
    // one, but the refusal's own message is still what's shown on the rare
    // race (a preset removed from under this dialog some other way).
    if (!st.documentPresets.remove(selectedPresetName, &err)) {
      status = err;
      statusIsError = true;
    } else if (!st.documentPresets.saveToFile(presetsPath, &err)) {
      status = err;
      statusIsError = true;
    } else {
      status = "Removed preset.";
      statusIsError = false;
      selectedPresetName.clear();
    }
  }
  if (!canRemove) ImGui::EndDisabled();

  if (!status.empty()) {
    if (statusIsError) ImGui::PushStyleColor(ImGuiCol_Text, kError);
    ImGui::TextWrapped("%s", status.c_str());
    if (statusIsError) ImGui::PopStyleColor();
  }

  ImGui::Separator();

  // --- From Clipboard ---------------------------------------------------
  ImGui::TextUnformatted("From Clipboard");
  ImGui::SameLine();
  if (ImGui::SmallButton("Check again")) clipboardProbe = probeClipboardImage();

  switch (clipboardProbe.status) {
    case ClipboardImageStatus::Empty:
      ImGui::TextDisabled("Clipboard is empty.");
      break;
    case ClipboardImageStatus::NotAnImage:
      ImGui::TextDisabled("Clipboard does not contain an image.");
      break;
    case ClipboardImageStatus::Unreadable:
      ImGui::PushStyleColor(ImGuiCol_Text, kError);
      ImGui::TextWrapped("%s", clipboardProbe.detail.c_str());
      ImGui::PopStyleColor();
      break;
    case ClipboardImageStatus::Image:
      ImGui::Text("Clipboard image: %u x %u (%s)", clipboardProbe.width, clipboardProbe.height,
                  clipboardProbe.mimeType.c_str());
      break;
  }
  ImGui::TextDisabled(
      "Reflects the clipboard as of the last time this window came to the front -- copy, "
      "then click back into naturalPaint, before checking again.");

  const bool hasClipboardImage = clipboardProbe.status == ClipboardImageStatus::Image;
  if (!hasClipboardImage) ImGui::BeginDisabled();
  if (ImGui::Button("New From Clipboard")) {
    OpenDocument* od = st.documents.add(makeBlankOpenDocument(
        static_cast<int32_t>(clipboardProbe.width), static_cast<int32_t>(clipboardProbe.height),
        WorkingSpace{}));
    DecodedImage img;
    img.width = clipboardProbe.width;
    img.height = clipboardProbe.height;
    img.pixels = clipboardProbe.pixels;
    if (od != nullptr && img.valid()) {
      placeImageAsLayer(od->document, img);
      ImGui::CloseCurrentPopup();
    } else {
      status = "Could not place the clipboard image into the new document.";
      statusIsError = true;
    }
  }
  if (!hasClipboardImage) ImGui::EndDisabled();

  ImGui::Separator();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();

  ImGui::EndPopup();
}

}  // namespace np
