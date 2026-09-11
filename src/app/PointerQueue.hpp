#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace np {

// ===========================================================================
// PointerQueue -- raw pointer events in, per-gesture stroke samples out
// ===========================================================================
//
// **Why this file exists.** Track A (brush wave 1) replaced the canvas block's
// one-position-per-render-frame read with a full-rate queue of pointer
// samples, and the logic that filled and drained that queue lived inside
// `main.cpp`'s SDL switch and `ui/MacPaintUI.cpp`'s canvas block -- the two
// places in this program nothing can reach without a window, a GPU and a
// human. That was the one path the wave could not test, and the wave-1 review
// found both of its highest-severity defects there:
//
//   1. every macOS pen stroke started at pressure 0, because SDL delivers a
//      tablet event's axes AFTER its position and each sample snapshotted the
//      axes as they stood when the POSITION arrived (the previous event's);
//   2. the queue ignored gesture boundaries: it held "this frame's events"
//      and was cleared every frame, while the canvas decides up/down from
//      ImGui, whose input trickling can report a button one or more frames
//      later than the frame whose poll loop saw it -- so a click after a
//      scroll painted nothing, and a lift-and-retouch inside one frame
//      painted a bridge from the end of one stroke to the start of the next.
//
// Every decision is now made here, on plain structs, with no SDL and no ImGui
// dependency, so `app/selftest/PointerQueue.cpp` can replay the exact event
// orders SDL produces and the exact frame boundaries ImGui produces. What is
// left outside is translation only: `main.cpp` turns each `SDL_Event` into a
// `PointerEvent` (one line per event type) and tells the queue how far ImGui
// has got through its own input queue once per frame; the canvas block claims
// a gesture when it begins a stroke and takes that gesture's samples.
//
// **The scalar pen state is NOT this module's.** `AppState::penPressure`,
// `penTilt`, `penDown` and the rest are still written by `main.cpp`'s
// `handlePenEvent()` from the same SDL events -- they are app-level readings
// (the DYNAMICS gutter, `dynamicInputsFor()`, `strokeHardwareInputsFor()`)
// with capability flags this module has no use for. This module keeps its
// own copy of the latest RAW axis values because it has to snapshot them into
// samples and patch them (section 2); `latestAxes()` exposes that copy for the
// selftest. The two are fed by the same events and cannot drift in value:
// both take the event's own number (pressure clamped to [0,1] in both).
//
// **Timestamps are used ONLY to associate events with each other, never in
// painting** (ADR-0003: deposition depends on distance, never on time or
// event count). `PointerSample::timestamp` exists so a PEN_AXIS event can find
// the position it belongs to (section 2); nothing downstream of
// `takeForStroke()` reads it, and `ui/MacPaintUI.cpp`'s conversion to a
// `StrokeSample` drops it. The UI sequence numbers of section 3 are not time
// at all -- they are positions in ImGui's input stream.

// One queued pointer sample. Window space (ImGui screen coordinates -- the
// space the canvas block reads `mouse` in, just above `xform.toCanvas()`),
// raw axis degrees (`app/PenAxes.hpp` converts them exactly once, at
// `strokeSampleFromPointer()`), and for a mouse the neutral reading those same
// conversions give 0/0/0 degrees (`penTiltNormalised(0,0) == 0`,
// `penAzimuthNormalised(0,0) == 0`, `penBarrelNormalised(0) == 0.5`), so a
// mouse sample needs nothing but `isPen = false`.
//
// **Window space, not canvas space**, because the conversion happens at drain
// time through the SAME view transform the canvas block uses for its own
// `canvasMouse` -- one inverse, not a second hand-derived one. That is only
// sound while a sample is drained on the frame the UI shows it; section 3's
// `endFrame()` rule is what guarantees no sample outlives the frame the canvas
// had its chance at it, so a pan or rotate between capture and drain cannot
// move a sample.
struct PointerSample {
  float x = 0.0f, y = 0.0f;
  float pressure = 1.0f;
  float tiltXDeg = 0.0f;
  float tiltYDeg = 0.0f;
  float rotationDeg = 0.0f;
  bool isPen = false;
  // SDL's `e.common.timestamp` of the event that queued this sample, in
  // nanoseconds. Association only -- see the header above.
  uint64_t timestamp = 0;
  // Which gesture (section 3) this sample belongs to. Never 0 for a queued
  // sample.
  uint64_t gesture = 0;
};

// The SDL events this module cares about, reduced to what it decides on.
enum class PointerEventKind : uint8_t {
  PenDown,          // SDL_EVENT_PEN_DOWN
  PenUp,            // SDL_EVENT_PEN_UP
  PenMotion,        // SDL_EVENT_PEN_MOTION
  PenAxis,          // SDL_EVENT_PEN_AXIS
  MouseButtonDown,  // SDL_EVENT_MOUSE_BUTTON_DOWN
  MouseButtonUp,    // SDL_EVENT_MOUSE_BUTTON_UP
  MouseMotion,      // SDL_EVENT_MOUSE_MOTION
};

// The pen axes a sample carries. `Other` is every SDL axis nothing here reads
// (tangential pressure, distance, slider) -- it updates nothing.
enum class PointerAxis : uint8_t { Pressure, TiltX, TiltY, Rotation, Other };

struct PointerEvent {
  PointerEventKind kind = PointerEventKind::MouseMotion;
  // `e.common.timestamp`, nanoseconds. See section 2 for what it is used for.
  uint64_t timestamp = 0;
  // ImGui's input-event sequence number (`ImGuiContext::InputEventsNextEventId`)
  // read immediately BEFORE this SDL event was handed to
  // `ImGui_ImplSDL3_ProcessEvent()`: every ImGui input event this SDL event
  // (or anything after it) produces has a sequence number >= this, every
  // earlier one < this. Section 3.
  uint32_t uiSeq = 0;
  // Position events (PenDown/PenMotion, Mouse*): window space.
  float x = 0.0f, y = 0.0f;
  // PenAxis only.
  PointerAxis axis = PointerAxis::Other;
  float value = 0.0f;
  // MouseButtonDown/Up: the event's button is SDL_BUTTON_LEFT.
  // MouseMotion: SDL_BUTTON_LMASK is set in the event's own button state
  // (`e.motion.state` is the mask AT THE EVENT, not now).
  bool left = false;
  // Mouse*: `which == SDL_PEN_MOUSEID` -- SDL's own mouse emulation of a pen.
  bool isPenSynthesizedMouse = false;
};

// ---------------------------------------------------------------------------
// 1. What is queued, and what never is (unchanged from Track A; the review's
//    "checked and found correct" list, pinned by the selftest's section 6)
// ---------------------------------------------------------------------------
//
// * PEN_DOWN queues a sample and opens a pen gesture; PEN_MOTION queues one
//   only while that gesture is open (in contact). **Hover is never queued** --
//   a hovering pen reports motion continuously, and hover samples reaching a
//   stroke would paint a streak from wherever the pen hovered before it
//   touched.
// * A LEFT MOUSE_BUTTON_DOWN queues a sample and opens a mouse gesture -- a
//   plain click that produces no motion must still queue ONE sample, or
//   `StrokePath::flush()`'s stationary-click rule would find none and paint
//   nothing. MOUSE_MOTION queues only with LEFT held at the event AND a mouse
//   gesture open (motion with the button held but no queued down -- a drag in
//   from outside the window -- belongs to no gesture the canvas can claim,
//   and ImGui never saw a press for it either).
// * **Button-up is never queued**, pen or mouse. It ENDS the gesture.
// * **The pen's own SDL-synthesised mouse events (`SDL_PEN_MOUSEID`) never
//   queue a sample.** SDL mirrors every pen event as mouse input for apps that
//   do not handle pens; queued, each pen sample would be followed by a copy of
//   itself at a mouse's pressure 1.0 and neutral axes. They are still READ:
//   the synthesised button down/up is the event ImGui sees for a pen's
//   contact, so it is what dates a pen gesture in ImGui's input stream
//   (section 3). SDL's own advice (SDL_mouse.h) is to filter them this way.
// * **Touch-generated mouse events (`SDL_TOUCH_MOUSEID`) are kept.** Touch has
//   no event stream of its own into this queue, so they are the only samples a
//   finger ever produces.
//
// ---------------------------------------------------------------------------
// 2. Axes arrive after the position: the patch rule
// ---------------------------------------------------------------------------
//
// SDL delivers each pen axis as its own SDL_EVENT_PEN_AXIS, and only when its
// value changed (SDL_pen.c:425, `if (pen->axes[axis] != value)`); position
// events likewise only when the position changed (SDL_pen.c:481). The order
// within ONE device report, per backend, in SDL 3.2.24
// (`sdl3-src/src/video/...`, the tree `FETCHCONTENT_SOURCE_DIR_SDL3` names):
//
//   macOS  cocoa/SDL_cocoapen.m:118-139, ONE timestamp per NSEvent (:118):
//          Touch(:131) Motion(:132) Button x2(:133-134) PRESSURE(:135)
//          ROTATION(:136) XTILT(:137) YTILT(:138) TANGENTIAL(:139).
//          The Touch event's position is the pen's PREVIOUS one (SDL_pen.c:
//          335-336 read `pen->x/y` before :132 moves it), so a contact that
//          also moved queues TWO samples -- the down at the old position and a
//          motion at the new -- both before any axis.
//   Wayland wayland/SDL_waylandevents.c:2954-2989, ONE timestamp per tablet
//          frame (:2954): contact frame Motion(:2961) Touch(:2962); lift frame
//          Touch(:2964) Motion(:2965); otherwise Touch?(:2969) Motion?(:2973);
//          then every changed axis (:2977-2981), then buttons (:2983-2989).
//   Windows windows/SDL_windowsevents.c:1359-1396, ONE timestamp per
//          WM_POINTER message (:1359, millisecond-resolution message time):
//          Touch(up)?(:1366) Motion(:1374) Button x2(:1375-1376)
//          PRESSURE(:1379) ROTATION(:1383) XTILT(:1387) YTILT(:1391)
//          Touch(down)?(:1396) -- the down comes LAST, after the axes.
//   X11    x11/SDL_x11xinput2.c: XI_Motion is Motion(:468) then every axis
//          (:473-477); XI_ButtonPress is Touch alone (:421, "we expect an
//          XI_Motion event first anyway"). **Timestamp 0 everywhere**, which
//          `SDL_PushEvent()` replaces with `SDL_GetTicksNS()` PER EVENT
//          (events/SDL_events.c:1717-1718) -- so the events of one report do
//          NOT share a timestamp.
//   Android android/SDL_androidpen.c:60-90, timestamp 0 (so per-event, as
//          X11): Motion(:60) PRESSURE(:61) Buttons(:69,:72) Touch(:85,:90).
//   iOS    uikit/SDL_uikitpen.m: Motion(:126) then axes (:127-131) under one
//          UITouch timestamp; a press sends those and THEN Touch(down)
//          (:192-193); a release sends Touch(up) and then the axes (:201-202).
//   Web    emscripten/SDL_emscriptenevents.c:743-763, timestamp 0: Motion(:743)
//          Touch(:747/:750) Buttons(:753/:756) then axes (:759-763).
//
// In every backend a report's axes arrive after its position events, or (the
// Windows/iOS/Android contact) its down arrives after its axes. So a sample
// that snapshots the "latest axis" state when its position arrives carries the
// PREVIOUS report's axes in every backend except for those contacts -- the
// review's finding 1. The rule that fixes it:
//
//   **A PEN_AXIS event patches the queued samples of the OPEN pen gesture that
//   belong to its own report**, and always updates the latest-axis state (so
//   a later position that arrives before any new axis event -- SDL sends none
//   for an unchanged value -- snapshots the right value). A sample belongs to
//   the axis event's report when EITHER
//     (a) its timestamp equals the axis event's -- walking back from the newest
//         sample over every sample with that timestamp, which is what catches
//         macOS's down-and-motion pair; OR
//     (b) it is the NEWEST sample of the open gesture and no event of this same
//         axis has arrived since the last PEN_MOTION event (queued or not) --
//         the structural rule for the backends whose events carry per-event
//         timestamps (X11, Android, Web, and Windows outside message
//         processing, windows/SDL_windowsevents.c:150-152), where the only
//         evidence of a report boundary is a position event. The boundary is
//         the last MOTION, not the newest sample, because a down that arrives
//         after its own report's axes (Windows, X11, Android, iOS) already
//         snapshotted them: counting from the down would let the NEXT report
//         of a pen held still rewrite it.
//
// Checked against each order: macOS (a) patches both halves of a contact and
// every motion; Wayland (a) patches the down and every motion; Windows's down
// snapshots axes that already arrived, (a) patches every motion; X11's down
// snapshots the axes of the XI_Motion before it, (b) patches every motion;
// Android as X11; iOS's down snapshots, (a) patches motions; Web (b) patches
// the down and every motion.
//
// **Only the OPEN gesture is patched.** After PEN_UP an axis event updates the
// latest-axis state only: macOS and Windows send the lift report's axes
// (pressure falling to 0) after the Touch(up), and those belong to no sample.
//
// Where the rule is knowingly approximate, and why each is accepted:
//   * (b) cannot see a report boundary that has no position change. When the
//     pen reports twice at an unchanged position and an axis that did not
//     change in the first report changes in the second, (b) gives the first
//     report's sample the second report's value. Both are readings of the pen
//     at that same position; the difference is one report's worth of change,
//     and it applies only while that sample is still queued. (a) and (b) are
//     applied together on every backend, so this can also happen on a stamped
//     one -- accepted rather than keeping a per-driver table.
//   * Windows timestamps have millisecond resolution; two WM_POINTER messages
//     inside one millisecond share a timestamp, and (a) then patches both
//     reports' samples with the later one's axes -- values under a
//     millisecond apart.
//   * Web's lift report is Motion, Touch(up), axes: its final motion sample
//     is queued with the previous report's axes and the gesture is closed
//     before its own arrive. One report of lag on a stroke's last sample, on a
//     backend this program is not built for.
// Every other backend delivers each sample's own report's axes exactly.
//
// PEN_UP no longer zeroes `AppState::penPressure` (main.cpp): the lift's own
// report carries pressure 0 on every backend that reports pressure, and for a
// pen that reports none the zero was the only pressure its later strokes ever
// read. Nothing read the zero for anything else.
//
// ---------------------------------------------------------------------------
// 3. Gestures, and whose samples a stroke gets
// ---------------------------------------------------------------------------
//
// Every queued pen down or mouse down opens a GESTURE (a fresh id from one
// counter shared by pen and mouse); its up ends it. Each sample carries its
// gesture's id. A gesture also records WHERE IN IMGUI'S INPUT STREAM its down
// and its up sit (`PointerEvent::uiSeq`): a mouse gesture's from the button
// events themselves, a pen gesture's from the synthesised mouse button events
// SDL pushes right behind PEN_DOWN/PEN_UP (SDL_pen.c:375-384) -- that
// synthesised button is the press ImGui actually sees for a pen.
//
// Once per frame, after `ImGui::NewFrame()`, `main.cpp` passes
// `beginFrame()` the sequence number of the first input event ImGui has NOT
// processed yet. ImGui consumes its queue in order, so "this gesture's down
// is below that number" is exactly "`ImGui::IsMouseDown()` has already
// reported this press", and likewise for the up. That is the one fact the
// canvas's own `down` (ImGui's) and this queue's contents (SDL's) were
// missing: which frame ImGui gets round to each press. The rules:
//
//   * **A gesture's samples are offered only once ImGui has processed its
//     down.** Until then they wait, across frames -- the scroll-then-click
//     case, where ImGui holds the press back a frame behind a wheel event.
//   * **`claimGesture()` -- called when the canvas BEGINS a stroke -- returns
//     the oldest unclaimed gesture whose down ImGui has processed and whose
//     up it has not.** Argument: a stroke begins on a frame ImGui reports the
//     button down, so ImGui is inside exactly one gesture: the one whose press
//     it processed last and whose release it has not processed yet. Every
//     OLDER gesture's release precedes that press in the stream, so it has
//     been processed; every NEWER gesture's press follows it, so it has not.
//     The predicate therefore picks that one gesture and no other; "oldest"
//     is only a deterministic tie-break for input no single pointer can
//     produce (pen and mouse pressed together). "Oldest unclaimed un-ended
//     gesture" -- ended in the QUEUE's sense -- is not the same thing and fails
//     twice: a quick click whose down AND up arrived in one poll behind a
//     wheel event has already ended in the queue when ImGui first reports it
//     down (it must still be claimed), and a panel click that ImGui has not
//     finished trickling through is un-ended in the queue while ImGui is
//     already past it (it must not be).
//   * **`takeForStroke(g)` hands the stroke every offered sample of ITS OWN
//     gesture `g`**, in arrival order, and nothing else. Samples of a later
//     gesture stay queued (the lift-and-retouch case: ImGui processes the
//     release and stops before the next press, so the release frame's stroke
//     gets its own tail and the next stroke begins a frame later from its own
//     down sample).
//   * **`endFrame()` -- after `drawUI()`, every frame -- drops every offered
//     sample nobody took, and forgets every gesture whose up ImGui has
//     processed.** An offered sample belongs to a press ImGui has already
//     reported, so the canvas has had its frame to take it: it was over a
//     panel, a menu or a modal, the view was being panned or rotated, the
//     stroke was refused, or the canvas window did not draw at all. Dropping
//     it is Track A's per-frame rule ("every frame owns exactly its own
//     samples"), now applied only to input the canvas has actually been shown
//     -- which is what keeps a pan gesture from being painted when the pan
//     key is released mid-drag, and keeps window-space samples from being
//     converted through a view that changed after they were captured.
//
// `ImGui` input trickling (`ImGui::UpdateInputEvents()`, on by default, not
// overridden in src) is what guarantees a release and the next press of the
// same button are never processed in the same frame; with trickling off a
// lift-and-retouch inside one frame would show the canvas `down` throughout
// and the second stroke would not begin. `app/selftest/PointerQueue.cpp`
// replays the trickled frame boundaries measured with headless ImGui 1.92.9b.
//
// ---------------------------------------------------------------------------
// 4. Memory bound
// ---------------------------------------------------------------------------
//
// At most `kMaxSamples` samples and `kMaxGestures` gesture records, enforced
// at insertion: a sample pushed into a full queue evicts the OLDEST queued
// sample, a gesture opened with the record list full evicts the oldest record
// and its samples. Samples normally live at most one frame after ImGui
// processes their gesture's press (section 3), so the cap only binds when
// ImGui stops consuming input for seconds -- a stalled frame loop -- or when
// `beginFrame()` is never called at all. 4096 samples is 4 s of a 1000 Hz
// pointer, 20 s of a 200 Hz tablet; `sizeof(PointerSample)` is 48 bytes, so
// under 200 KB. Eviction drops the oldest because the queue is consumed in
// order: what a stalled loop can least afford to replay is what it would reach
// first.
class PointerQueue {
 public:
  static constexpr size_t kMaxSamples = 4096;
  static constexpr size_t kMaxGestures = 64;

  // Raw axis readings as the most recent PEN_AXIS events left them (pressure
  // clamped to [0,1]). What a pen sample snapshots when its position arrives.
  // Pressure starts at 1.0: a pen that never reports pressure paints at full
  // strength, the same "no information" reading `dynamicInputsFor()` gives.
  struct Axes {
    float pressure = 1.0f;
    float tiltXDeg = 0.0f;
    float tiltYDeg = 0.0f;
    float rotationDeg = 0.0f;
  };

  // The producer side -- main.cpp's SDL poll loop, one call per event.
  void push(const PointerEvent& e);

  // Once per frame, after `ImGui::NewFrame()`: the sequence number of the
  // first input event ImGui has not yet processed (section 3).
  void beginFrame(uint32_t uiProcessedBound) noexcept;

  // Section 3. 0 when there is no such gesture (a press this queue never saw:
  // ImGui input injected directly, with nothing pushed here).
  uint64_t claimGesture() noexcept;

  // Section 3. Empty for gesture 0 or an unknown gesture.
  std::vector<PointerSample> takeForStroke(uint64_t gesture);

  // Section 3. After `drawUI()`, every frame.
  void endFrame();

  size_t size() const noexcept { return samples_.size(); }
  size_t gestureCount() const noexcept { return gestures_.size(); }
  const Axes& latestAxes() const noexcept { return latest_; }

 private:
  struct Gesture {
    uint64_t id = 0;
    bool pen = false;
    uint32_t downSeq = 0;
    bool ended = false;
    uint32_t endSeq = 0;
    bool claimed = false;
  };

  // "ImGui has processed the event with this sequence number" -- serial-
  // number order, so the 32-bit counter's wrap (after ~4e9 input events) is
  // harmless.
  bool processed(uint32_t seq) const noexcept;
  Gesture* find(uint64_t id) noexcept;
  const Gesture* find(uint64_t id) const noexcept;
  uint64_t openGesture(bool pen, uint32_t downSeq);
  void endGesture(uint64_t id, uint32_t endSeq) noexcept;
  void enqueue(const PointerEvent& e, bool pen, uint64_t gesture);
  void patchAxis(const PointerEvent& e) noexcept;

  std::vector<PointerSample> samples_;
  std::vector<Gesture> gestures_;
  uint64_t nextGesture_ = 1;
  uint64_t openPen_ = 0;
  uint64_t openMouse_ = 0;
  // The pen gesture whose down/up still waits for the synthesised mouse
  // button event that dates it in ImGui's stream (section 3).
  uint64_t penDownSeqPending_ = 0;
  uint64_t penUpSeqPending_ = 0;
  Axes latest_;
  // Rule (b)'s memory: which axes have already been reported since the last
  // PEN_MOTION event. Indexed by `PointerAxis` (Other excluded).
  std::array<bool, 4> axisSinceMotion_{};
  uint32_t uiProcessedBound_ = 0;
  bool haveBound_ = false;
};

}  // namespace np
