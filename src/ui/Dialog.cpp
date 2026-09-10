#include "ui/Dialog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cfloat>
#include <cstring>

#include "ui/AtelierTheme.hpp"
#include "ui/Fonts.hpp"

namespace np {

namespace {

ImVec4 tok(uint32_t rgb, float a = 1.0f) {
  return ImVec4(static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                static_cast<float>(rgb & 0xff) / 255.0f, a);
}

ImVec4 lift(uint32_t rgb, float t) {
  ImVec4 c = tok(rgb);
  c.x += (1.0f - c.x) * t;
  c.y += (1.0f - c.y) * t;
  c.z += (1.0f - c.z) * t;
  return c;
}

// The numeric field beside a slider, and the gap before a unit.
constexpr float kFieldWidth = 68.0f;
constexpr float kMinButtonWidth = 84.0f;

// The frame in which a footer consumed Return or Escape. -1 is never.
int g_keyHandledFrame = -1;

// Whether the body child opened by beginDialog() is still open. The footer
// closes it so the buttons sit below the scrolling region rather than inside
// it; endDialog() closes it for a dialog that drew no footer.
bool g_bodyOpen = false;

void endBodyIfOpen() {
  if (!g_bodyOpen) return;
  g_bodyOpen = false;
  ImGui::EndChild();
}

// The last frame on which the dialog is forced back to the centre. A dialog
// takes three frames to reach its size: the body child measures the previous
// frame's content (so it is empty on the frame it appears), and the window
// measures the previous frame's body. Centring only on the appearing frame
// centred a title bar and then let the content grow down off the screen --
// every dialog landed at the same y regardless of height. So the centre is
// re-asserted through the settling frames, and the user's drag wins after.
int g_centreUntilFrame = -1;

// One row's geometry: the label in its column, the cursor at the control.
struct Row {
  float startX = 0.0f;
  float avail = 0.0f;
};

Row beginRow(const char* label) {
  Row r;
  r.startX = ImGui::GetCursorPosX();
  const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  ImGui::AlignTextToFramePadding();
  if (label != nullptr && label[0] != '\0') {
    const float tw = ImGui::CalcTextSize(label).x;
    // Right-aligned into the column. A label wider than the column starts at
    // the row's left edge rather than pushing the control -- and is a label
    // to shorten (this header's note on `kDialogLabelColumn`).
    ImGui::SetCursorPosX(r.startX + std::max(0.0f, kDialogLabelColumn - tw));
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.0f, 0.0f);
  }
  ImGui::SetCursorPosX(r.startX + kDialogLabelColumn + spacing);
  r.avail = ImGui::GetContentRegionAvail().x;
  return r;
}

// The one place a control's id is derived from its label, so two rows with
// one label in the same dialog still get distinct ids: the caller wraps in
// PushID when that happens, the same rule ImGui itself has.
void idFor(const char* label, const char* suffix, char* out, size_t cap) {
  std::snprintf(out, cap, "##%s%s", label != nullptr ? label : "", suffix);
}

void unitAfter(const char* unit) {
  if (unit == nullptr || unit[0] == '\0') return;
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", unit);
}

float unitWidth(const char* unit) {
  if (unit == nullptr || unit[0] == '\0') return 0.0f;
  return ImGui::CalcTextSize(unit).x + ImGui::GetStyle().ItemInnerSpacing.x;
}

float buttonWidth(const char* label) {
  const ImGuiStyle& s = ImGui::GetStyle();
  return std::max(kMinButtonWidth, ImGui::CalcTextSize(label).x + s.FramePadding.x * 4.0f);
}

// A button in a token's fill with the on-accent text, for the default and the
// destructive choices. Pushes and pops around one Button() call.
bool tokenButton(const char* label, uint32_t fill, float width) {
  ImGui::PushStyleColor(ImGuiCol_Button, tok(fill));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lift(fill, 0.12f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, lift(fill, 0.25f));
  ImGui::PushStyleColor(ImGuiCol_Text, tok(kOnAccent));
  const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
  ImGui::PopStyleColor(4);
  return pressed;
}

bool keyPressedOnce(ImGuiKey key) { return ImGui::IsKeyPressed(key, false); }

}  // namespace

// ---------------------------------------------------------------- window

bool beginDialog(const char* title, DialogWidth width) {
  const float w = width == DialogWidth::Wide ? kDialogWideWidth : kDialogStandardWidth;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  // `Appearing`, not `FirstUseEver`: a dialog dragged aside and reopened comes
  // back to the centre, which is where a sheet belongs, and a window whose
  // size changed between openings (Recover Documents with 0 vs 3 sessions) is
  // centred for the size it has now. `Always` for the settling frames after
  // that -- see g_centreUntilFrame.
  const int frame = ImGui::GetFrameCount();
  ImGui::SetNextWindowPos(vp->GetCenter(),
                          frame <= g_centreUntilFrame ? ImGuiCond_Always : ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  // Width pinned from both sides; height follows the content. The height cap
  // is on the BODY below, not on the window: a capped window would scroll its
  // footer away with everything else, and a dialog whose buttons have to be
  // scrolled to is a dialog with no buttons.
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
  const bool open = ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleVar();
  if (!open) return false;
  if (ImGui::IsWindowAppearing()) g_centreUntilFrame = frame + 4;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));

  // The body: sized to its content, up to 85% of the viewport less what the
  // title bar, the padding and the footer need; past that it scrolls and the
  // footer stays put underneath. `AutoResizeY` measures the previous frame's
  // content, which is why a dialog that grows (an export report appearing)
  // is right one frame later, and why nothing here has to say how tall it is.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float chrome = ImGui::GetFrameHeight() * 3.0f + style.WindowPadding.y * 2.0f +
                       style.ItemSpacing.y * 4.0f;
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                      ImVec2(FLT_MAX, vp->WorkSize.y * 0.85f - chrome));
  ImGui::BeginChild("##dialogBody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
  g_bodyOpen = true;
  return true;
}

void endDialog() {
  endBodyIfOpen();
  ImGui::PopStyleVar();
  ImGui::EndPopup();
}

// ---------------------------------------------------------------- controls

DialogEdit dialogSlider(const char* label, float* v, float lo, float hi, const char* fmt,
                        const char* unit) {
  DialogEdit e;
  const Row r = beginRow(label);
  const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
  const float trackW = r.avail - kFieldWidth - inner - unitWidth(unit);
  char id[128];
  idFor(label, "slider", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, trackW));
  // No value text on the track: the field beside it is the value, and a grab
  // drawn over digits was the thing being fixed.
  e.changed |= ImGui::SliderFloat(id, v, lo, hi, "");
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SameLine(0.0f, inner);
  idFor(label, "field", id, sizeof(id));
  ImGui::SetNextItemWidth(kFieldWidth);
  if (ImGui::InputScalar(id, ImGuiDataType_Float, v, nullptr, nullptr, fmt,
                         ImGuiInputTextFlags_AutoSelectAll)) {
    *v = std::clamp(*v, lo, hi);
    e.changed = true;
  }
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  unitAfter(unit);
  return e;
}

DialogEdit dialogSliderInt(const char* label, int* v, int lo, int hi, const char* unit) {
  DialogEdit e;
  const Row r = beginRow(label);
  const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
  const float trackW = r.avail - kFieldWidth - inner - unitWidth(unit);
  char id[128];
  idFor(label, "slider", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, trackW));
  e.changed |= ImGui::SliderInt(id, v, lo, hi, "");
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SameLine(0.0f, inner);
  idFor(label, "field", id, sizeof(id));
  ImGui::SetNextItemWidth(kFieldWidth);
  if (ImGui::InputScalar(id, ImGuiDataType_S32, v, nullptr, nullptr, "%d",
                         ImGuiInputTextFlags_AutoSelectAll)) {
    *v = std::clamp(*v, lo, hi);
    e.changed = true;
  }
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  unitAfter(unit);
  return e;
}

DialogEdit dialogDrag(const char* label, float* v, float speed, float lo, float hi,
                      const char* fmt, const char* unit) {
  DialogEdit e;
  const Row r = beginRow(label);
  char id[128];
  idFor(label, "drag", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, r.avail - unitWidth(unit)));
  e.changed |= ImGui::DragFloat(id, v, speed, lo, hi, fmt);
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  unitAfter(unit);
  return e;
}

DialogEdit dialogInputInt(const char* label, int* v, const char* unit) {
  DialogEdit e;
  const Row r = beginRow(label);
  char id[128];
  idFor(label, "int", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, r.avail - unitWidth(unit)));
  // The step buttons ImGui's InputInt adds are dropped: a canvas size is typed,
  // not nudged one pixel at a time, and the two buttons were half the row.
  e.changed |= ImGui::InputScalar(id, ImGuiDataType_S32, v, nullptr, nullptr, "%d",
                                  ImGuiInputTextFlags_AutoSelectAll);
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  unitAfter(unit);
  return e;
}

DialogEdit dialogInputText(const char* label, char* buf, size_t cap, ImGuiInputTextFlags flags) {
  DialogEdit e;
  const Row r = beginRow(label);
  char id[128];
  idFor(label, "text", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, r.avail));
  e.changed |= ImGui::InputText(id, buf, cap, flags);
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  return e;
}

bool dialogBeginCombo(const char* label, const char* preview) {
  const Row r = beginRow(label);
  char id[128];
  idFor(label, "combo", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, r.avail));
  return ImGui::BeginCombo(id, preview);
}

bool dialogCombo(const char* label, int* idx, const char* const items[], int count) {
  bool changed = false;
  const int cur = std::clamp(*idx, 0, std::max(0, count - 1));
  if (dialogBeginCombo(label, count > 0 ? items[cur] : "")) {
    for (int i = 0; i < count; ++i) {
      const bool sel = i == cur;
      if (ImGui::Selectable(items[i], sel)) {
        *idx = i;
        changed = true;
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

bool dialogCheckbox(const char* label, bool* v) {
  beginRow(nullptr);
  return ImGui::Checkbox(label, v);
}

bool dialogRadioRow(const char* label, int* idx, const char* const items[], int count) {
  beginRow(label);
  bool changed = false;
  ImGui::PushID(label);
  for (int i = 0; i < count; ++i) {
    if (i > 0) ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::RadioButton(items[i], *idx == i) && *idx != i) {
      *idx = i;
      changed = true;
    }
  }
  ImGui::PopID();
  return changed;
}

DialogEdit dialogColor(const char* label, float rgb[3], ImGuiColorEditFlags flags) {
  DialogEdit e;
  const Row r = beginRow(label);
  char id[128];
  idFor(label, "color", id, sizeof(id));
  ImGui::SetNextItemWidth(std::max(40.0f, r.avail));
  e.changed |= ImGui::ColorEdit3(id, rgb, flags);
  e.settled |= ImGui::IsItemDeactivatedAfterEdit();
  return e;
}

void dialogLabelRow(const char* label, float* availOut) {
  const Row r = beginRow(label);
  if (availOut != nullptr) *availOut = r.avail;
}

// ---------------------------------------------------------------- prose

void dialogSection(const char* title) {
  ImGui::Spacing();
  char caps[96];
  size_t n = 0;
  for (; title[n] != '\0' && n + 1 < sizeof(caps); ++n)
    caps[n] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[n])));
  caps[n] = '\0';
  const UiFonts& fonts = uiFonts();
  if (fonts.mono != nullptr) ImGui::PushFont(fonts.mono, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  // Flush left: the default pads a stub of rule before the heading, which
  // reads as a bullet nobody asked for.
  ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, ImVec2(0.0f, 4.0f));
  ImGui::SeparatorText(caps);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (fonts.mono != nullptr) ImGui::PopFont();
}

void dialogText(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ImGui::TextWrapped("%s", buf);
}

void dialogHint(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", buf);
  ImGui::PopStyleColor();
}

ImVec4 dialogStatusColor(DialogStatus kind) {
  switch (kind) {
    case DialogStatus::Error: return tok(kError);
    case DialogStatus::Warning: return tok(kWarning);
    case DialogStatus::Info: break;
  }
  return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

void dialogStatusLine(DialogStatus kind, const std::string& text) {
  if (text.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, dialogStatusColor(kind));
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- footer

DialogAction dialogFooter(const DialogFooter& f) {
  const ImGuiStyle& s = ImGui::GetStyle();
  endBodyIfOpen();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  DialogAction action = DialogAction::None;
  const float rowStartX = ImGui::GetCursorPosX();
  const float avail = ImGui::GetContentRegionAvail().x;

  // Left edge: the destructive third choice, or the note.
  float leftUsed = 0.0f;
  if (f.alternate != nullptr) {
    const float w = buttonWidth(f.alternate);
    ImGui::BeginDisabled(!f.alternateEnabled);
    const bool pressed = f.alternateDestructive ? tokenButton(f.alternate, kError, w)
                                                : ImGui::Button(f.alternate, ImVec2(w, 0.0f));
    ImGui::EndDisabled();
    if (pressed) action = DialogAction::Alternate;
    leftUsed = w;
    ImGui::SameLine(0.0f, 0.0f);
  } else if (f.note != nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", f.note);
    leftUsed = ImGui::CalcTextSize(f.note).x;
    ImGui::SameLine(0.0f, 0.0f);
  }

  // Right edge: Cancel, then the default.
  const float cancelW = f.cancel != nullptr ? buttonWidth(f.cancel) : 0.0f;
  const float commitW = f.commit != nullptr ? buttonWidth(f.commit) : 0.0f;
  float rightW = cancelW + commitW;
  if (f.cancel != nullptr && f.commit != nullptr) rightW += s.ItemSpacing.x;
  ImGui::SetCursorPosX(rowStartX + std::max(leftUsed + s.ItemSpacing.x, avail - rightW));

  if (f.cancel != nullptr) {
    if (ImGui::Button(f.cancel, ImVec2(cancelW, 0.0f))) action = DialogAction::Cancel;
    if (f.commit != nullptr) ImGui::SameLine();
  }
  if (f.commit != nullptr) {
    ImGui::BeginDisabled(!f.commitEnabled);
    if (tokenButton(f.commit, f.commitDestructive ? kError : kAccent, commitW))
      action = DialogAction::Commit;
    ImGui::EndDisabled();
  }

  // The keys, once per press. Read after the buttons so a click and a key in
  // the same frame resolve to the click. Return with no enabled default does
  // nothing rather than something else; Escape on an OK-only dialog is that
  // OK, because dismissing is the only thing the dialog offers.
  if (action == DialogAction::None) {
    if (keyPressedOnce(ImGuiKey_Escape)) {
      if (f.cancel != nullptr) action = DialogAction::Cancel;
      else if (f.commit != nullptr && f.commitEnabled) action = DialogAction::Commit;
    } else if (keyPressedOnce(ImGuiKey_Enter) || keyPressedOnce(ImGuiKey_KeypadEnter)) {
      if (f.commit != nullptr && f.commitEnabled) action = DialogAction::Commit;
    }
    if (action != DialogAction::None) g_keyHandledFrame = ImGui::GetFrameCount();
  }
  return action;
}

bool dialogHandledKeyThisFrame() { return g_keyHandledFrame == ImGui::GetFrameCount(); }

}  // namespace np
