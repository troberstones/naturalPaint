#include "ui/NewDocumentDialog.hpp"

#include <cstdlib>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "app/AppState.hpp"
#include "core/CanvasLimits.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/DocumentPresets.hpp"
#include "io/ClipboardImage.hpp"
#include "io/ImageIO.hpp"
#include "ui/Dialog.hpp"

// See ui/NewDocumentDialog.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp.
namespace np {
namespace {

bool g_newDocumentRequested = false;


}  // namespace

void requestNewDocumentDialog() { g_newDocumentRequested = true; }

void drawNewDocumentDialog(AppState& st) {
  // Session state, function-local statics -- UI state, not app state, the
  // same split every other dialog in ui/MacPaintUI.cpp makes.
  static int width = 1280;
  static int height = 720;
  // The name of the preset last clicked, so the Remove button knows what to
  // remove and the list knows what to highlight. Cleared the moment
  // width/height is hand-edited, because at that point the fields no longer
  // describe that preset.
  static std::string selectedPresetName;
  static char newPresetNameBuf[96] = "";
  static std::string status;
  static bool statusIsError = false;

  // The clipboard probe is decoded once per dialog-open, not once per frame:
  // probeClipboardImage() decodes whatever image bytes are on the pasteboard,
  // and doing that 60 times a second while this modal sits open would be
  // pointless work for a value that cannot change without the app losing and
  // regaining key focus anyway (see io/ClipboardImage.hpp's own note on
  // SDL's macOS mime-type cache). "Check Again" below re-probes on demand.
  static ClipboardImageProbe clipboardProbe;

  if (g_newDocumentRequested) {
    g_newDocumentRequested = false;
    status.clear();
    statusIsError = false;
    selectedPresetName.clear();
    clipboardProbe = probeClipboardImage();
    ImGui::OpenPopup("New Document");
  }
  if (!beginDialog("New Document", DialogWidth::Wide)) return;

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

  // --- Size, from a preset or typed --------------------------------------------
  const std::vector<DocumentPreset> presets = st.documentPresets.allPresets();
  bool selectedIsBuiltin = false;
  float listW = 0.0f;
  dialogLabelRow("Preset", &listW);
  const float listH = ImGui::GetTextLineHeightWithSpacing() * 6.5f;
  if (ImGui::BeginListBox("##newDocPresets", ImVec2(listW, listH))) {
    for (const DocumentPreset& p : presets) {
      ImGui::PushID(p.name.c_str());
      const bool isSelected = p.name == selectedPresetName;
      char row[160];
      std::snprintf(row, sizeof(row), "%s   %d \xc3\x97 %d", p.name.c_str(), p.width, p.height);
      if (ImGui::Selectable(row, isSelected)) {
        selectedPresetName = p.name;
        width = p.width;
        height = p.height;
      }
      if (isSelected) selectedIsBuiltin = p.builtin;
      // Built-ins are never editable or removable, and the tag says so at a
      // glance rather than only when a Remove click is refused.
      if (!p.builtin) {
        ImGui::SameLine();
        ImGui::TextDisabled("user");
      }
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }
  if (dialogInputInt("Width", &width, "px")) selectedPresetName.clear();
  if (dialogInputInt("Height", &height, "px")) selectedPresetName.clear();

  // Never let an invalid size reach document creation -- validated with the
  // exact same function the preset store itself validates a hand-typed
  // `size` line with (app/DocumentPresets.hpp), so this dialog and a
  // hand-edited presets file are held to one rule, not two. Two bounds, asked
  // in this order, and the order is the message: a size that fails BOTH is a
  // nonsense number (a pasted extra digit), and saying so is more useful than
  // telling the user their 90000px canvas is 5.5x the GPU's limit.
  // core/CanvasLimits.hpp closes the window between the store's 32768 and the
  // adapter's usual 16384.
  std::string sizeError =
      validateDocumentPresetSize(static_cast<int32_t>(width), static_cast<int32_t>(height));
  if (sizeError.empty())
    sizeError = canvasDimensionRefusal(static_cast<int32_t>(width), static_cast<int32_t>(height));
  const bool validSize = sizeError.empty();
  dialogStatusLine(DialogStatus::Error, sizeError);

  // --- Save the size as a preset ----------------------------------------------
  dialogSection("Save as preset");
  {
    float avail = 0.0f;
    dialogLabelRow("Name", &avail);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float addW = ImGui::CalcTextSize("Add").x + style.FramePadding.x * 4.0f;
    ImGui::SetNextItemWidth(std::max(40.0f, avail - addW - style.ItemInnerSpacing.x));
    ImGui::InputText("##newPresetName", newPresetNameBuf, sizeof(newPresetNameBuf));
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(newPresetNameBuf[0] == '\0' || !validSize);
    if (ImGui::Button("Add", ImVec2(addW, 0.0f))) {
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
    ImGui::EndDisabled();
  }
  const bool canRemove = !selectedPresetName.empty() && !selectedIsBuiltin;
  dialogLabelRow(nullptr);
  ImGui::BeginDisabled(!canRemove);
  if (ImGui::SmallButton("Remove Selected Preset")) {
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
  ImGui::EndDisabled();
  dialogStatusLine(statusIsError ? DialogStatus::Error : DialogStatus::Info, status);

  // --- From the clipboard -----------------------------------------------------
  dialogSection("Clipboard");
  dialogLabelRow("Contents");
  ImGui::AlignTextToFramePadding();
  switch (clipboardProbe.status) {
    case ClipboardImageStatus::Empty:
      ImGui::TextDisabled("Empty.");
      break;
    case ClipboardImageStatus::NotAnImage:
      ImGui::TextDisabled("Not an image.");
      break;
    case ClipboardImageStatus::Unreadable:
      ImGui::PushStyleColor(ImGuiCol_Text, dialogStatusColor(DialogStatus::Error));
      ImGui::TextUnformatted(clipboardProbe.detail.c_str());
      ImGui::PopStyleColor();
      break;
    case ClipboardImageStatus::Image:
      ImGui::Text("Image, %u \xc3\x97 %u (%s)", clipboardProbe.width, clipboardProbe.height,
                  clipboardProbe.mimeType.c_str());
      break;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Check Again")) clipboardProbe = probeClipboardImage();
  dialogHint("As of the last time this window came to the front: copy, click back into "
             "naturalPaint, then Check Again.");

  // The clipboard's own extent gets the same ceiling as a typed one -- a
  // screenshot of a very wide multi-monitor desktop is the realistic way to
  // reach it, and it arrives here with no dialog field the user could have
  // read a refusal from.
  const std::string clipboardSizeError =
      clipboardProbe.status == ClipboardImageStatus::Image
          ? canvasDimensionRefusal(static_cast<int32_t>(clipboardProbe.width),
                                   static_cast<int32_t>(clipboardProbe.height))
          : std::string{};
  dialogStatusLine(DialogStatus::Error, clipboardSizeError);
  const bool hasClipboardImage =
      clipboardProbe.status == ClipboardImageStatus::Image && clipboardSizeError.empty();
  dialogLabelRow(nullptr);
  ImGui::BeginDisabled(!hasClipboardImage);
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
  ImGui::EndDisabled();

  DialogFooter footer;
  footer.commit = "Create";
  footer.commitEnabled = validSize;
  switch (dialogFooter(footer)) {
    case DialogAction::Commit:
      st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(width),
                                             static_cast<int32_t>(height), WorkingSpace{}));
      ImGui::CloseCurrentPopup();
      break;
    case DialogAction::Cancel:
      ImGui::CloseCurrentPopup();
      break;
    default:
      break;
  }
  endDialog();
}

}  // namespace np
