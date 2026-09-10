#include "app/CommandCoverage.hpp"

namespace np {

// One `switch`, every enumerator, and `-Werror=switch` makes it a build error
// to add a menu action without deciding which of the three answers it gets.
// See app/CommandCoverage.hpp for why that is a switch and not a list, and why
// there are three answers rather than two.
CommandCoverage coverageFor(MenuAction action) {
  switch (action) {
    case MenuAction::None:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the absence of an action"};
    case MenuAction::NewCanvas:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "clears the solver texture, which is not a document at all"};
    case MenuAction::NewDocument:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "creates a document; the session owns which documents exist"};
    case MenuAction::Open:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the session owns which documents exist -- and in a batch the RUNNER owns opening, which is what lets it promise P4"};
    case MenuAction::OpenRecentEntry:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Open, and its param indexes a session-level recent list"};
    case MenuAction::ClearRecentMenu:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a session list, not a document"};
    case MenuAction::ImportImage:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "brings a file into the session; the batch runner owns file input"};
    case MenuAction::Save:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the batch runner owns writing, which is the whole of PRD P4"};
    case MenuAction::SaveAs:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Save"};
    case MenuAction::SaveCopy:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Save"};
    case MenuAction::SaveIncremental:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Save, and it reads the containing directory to choose a version"};
    case MenuAction::RecoverDocuments:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "reads app/Journal, which is session recovery state"};
    case MenuAction::Revert:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "re-reads the file the session opened"};
    case MenuAction::DuplicateDocument:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "creates a second document in the session"};
    case MenuAction::CloseDocument:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "removes a document from the session"};
    case MenuAction::ExportAs:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the batch runner owns writing (PRD P3 supplies the preset instead)"};
    case MenuAction::ExportStates:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as ExportAs; io/ExportStates is the loop the batch runner reuses"};
    case MenuAction::Batch:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "opens a dialog. The batch itself is not a step -- it RUNS actions, and an "
              "action that could contain a batch of itself is a loop with no base case"};    case MenuAction::Quit:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the process"};
    case MenuAction::Undo:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "navigates core::History, which is a stack of whole documents. Recording an undo would record a move through a history the replaying document does not have"};
    case MenuAction::Redo:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Undo"};
    case MenuAction::FreeTransform:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "begins an interactive gesture with on-canvas handles; the session owns the live transform"};
    case MenuAction::NumericTransform:
      return {CommandCoverageKind::NotYetRegistered, nullptr,
              "a document edit and genuinely recordable. It needs a transform matrix as a parameter and a policy for what a recorded transform means at another resolution (docs/automation-plan.md §5's unit rule), which is step 5 work rather than a registration"};
    case MenuAction::Cut:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the clipboard is process state shared with other applications"};
    case MenuAction::Copy:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Cut"};
    case MenuAction::CopyMerged:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Cut"};
    case MenuAction::Paste:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as Cut -- and what it pastes is whatever the clipboard holds at replay time, which is the definition of a step that is not reproducible"};
    case MenuAction::DeleteSelection:
      return {CommandCoverageKind::NotYetRegistered, nullptr,
              "a document edit and genuinely recordable. Left until the selection rows and the recorder agree on how a step names the selection it acted through -- registering it before that would put a step in a file whose meaning depends on state the file does not carry"};
    case MenuAction::SelectAll:
      return {CommandCoverageKind::Registered, "select_all", nullptr};
    case MenuAction::Deselect:
      return {CommandCoverageKind::Registered, "deselect", nullptr};
    case MenuAction::Reselect:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "restores AppState::lastDeselected, which is session state by construction"};
    case MenuAction::InvertSelection:
      return {CommandCoverageKind::Registered, "invert_selection", nullptr};
    case MenuAction::ClearCanvas:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the solver texture, not a document"};
    case MenuAction::LayerCommandItem:
      // A family: the id depends on `param`. app/selftest/CommandsLayers.cpp
      // walks the whole vocabulary instead.
      return {CommandCoverageKind::Registered, nullptr, nullptr};
    case MenuAction::LayerSetCommandItem:
      // A family: the id depends on `param`. app/selftest/CommandsLayers.cpp
      // walks the whole vocabulary instead.
      return {CommandCoverageKind::Registered, nullptr, nullptr};
    case MenuAction::SelectGrow:
      return {CommandCoverageKind::Registered, "select_grow", nullptr};
    case MenuAction::SelectShrink:
      return {CommandCoverageKind::Registered, "select_shrink", nullptr};
    case MenuAction::SelectFeather:
      return {CommandCoverageKind::Registered, "select_feather", nullptr};
    case MenuAction::SelectColourRange:
      // The gap's own reason said the parameter "would have to carry the
      // colour rather than referring to it". It does: `"colour": [r, g, b]`,
      // display-encoded sRGB, in the step. See app/CommandsOpStack.cpp §4.
      return {CommandCoverageKind::Registered, "select_colour_range", nullptr};
    case MenuAction::SelectLuminanceRange:
      return {CommandCoverageKind::Registered, "select_luminance_range", nullptr};
    case MenuAction::SelectUndoRefine:
      // **Not a gap, and this is the one classification in the file that had
      // to be argued from the code rather than from the menu item.** The other
      // five refines were listed beside it as gaps and are now registered, so
      // the obvious reading is that this one is next. It is not.
      //
      // `undoLastRefine()` pops `OpenDocument::refineUndoStack`. That member's
      // own comment (app/DocumentLifecycle.hpp) states at length why it is
      // NOT document data: a refine changes no pixel, `core::HistoryEntry`
      // holds nothing but a `core::Document`, and folding selections into
      // core::History would make an ordinary pixel Undo silently revert a
      // marquee drawn afterwards. So the stack lives beside `selection` and
      // `lastDeselected` -- per-session, never written to a file, gone when
      // the tab closes.
      //
      // A recorded `select_undo_refine` would therefore mean "undo whatever
      // refine this session last did", which on a replaying document is
      // whatever the *user* last did by hand, or nothing at all. That is not
      // a function of the document; it is a function of a history the file
      // cannot carry, exactly as Undo and Redo are a few dozen cases above.
      // The five rows above are how an action expresses a refine: it states
      // the refine it wants, rather than un-stating one it never made.
      return {CommandCoverageKind::NotRecordable, nullptr,
              "pops OpenDocument::refineUndoStack, which is per-session state that is deliberately outside core::History and outside the document file -- so a recorded undo would undo whatever the replaying session happened to do last. An action states the refine it wants instead"};
    case MenuAction::PaintModeItem:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "AppState -- the paint mode is a tool setting"};
    case MenuAction::ToolItem:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "AppState -- which tool is active"};
    case MenuAction::PauseSolver:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the simulation loop"};
    case MenuAction::ReloadShaders:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the GPU pipeline"};
    case MenuAction::FitToWindow:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "needs the canvas window size, which only exists inside a frame"};
    case MenuAction::Zoom100:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the view"};
    case MenuAction::ZoomIn:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the view"};
    case MenuAction::ZoomOut:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the view"};
    case MenuAction::MirrorX:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the VIEW mirror, not the document -- it flips what is drawn, not what is stored"};
    case MenuAction::MirrorY:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as MirrorX"};
    case MenuAction::ResetRotation:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the view"};
    case MenuAction::ResetView:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "the view"};
    case MenuAction::GrayscalePreview:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a view toggle; the document is unchanged"};
    case MenuAction::Rulers:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "chrome"};
    case MenuAction::Navigator:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a panel"};
    case MenuAction::Guides:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "view furniture"};
    case MenuAction::AddGuide:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "view furniture -- guides are not written to a .npaint"};
    case MenuAction::ClearGuides:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "view furniture"};
    case MenuAction::Grid:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "view furniture"};
    case MenuAction::Snap:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a tool behaviour"};
    case MenuAction::BrushSettings:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a panel"};
    case MenuAction::Pigment:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a panel"};
    case MenuAction::ImGuiDemo:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "a development window"};
    case MenuAction::ActivateDocument:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "chooses which open document is frontmost -- the session"};
    // --- PLAN.md phases 8 and 9, merged in after step 2's migration -------
    //
    // All three are pixel ops on the active layer, of exactly the shape the
    // seven `Registered` filters below have, and all three are functions of an
    // `OpenDocument` alone -- so app/Command.hpp §1's rule puts them IN, and
    // the only thing between them and a row is the row.
    //
    // They are `NotYetRegistered` rather than registered here because
    // registering one is not one line: it is a runner that reads and validates
    // the params (app/CommandsImage.cpp), a builder, a row in the table, and
    // for the two that take a length, an entry in app/Batch.cpp's pixel-unit
    // list so a recorded radius means the same thing at another resolution.
    // Doing that inside a merge, untested, is how a step lands in an action
    // file with a meaning nobody checked.
    case MenuAction::Inpaint:
      return {CommandCoverageKind::NotYetRegistered, nullptr,
              "a document edit and genuinely recordable: PRD D7's diffusion fill, ops/Inpaint through app/PixelOpBridge. It needs a runner for its `radius` and a pixel-unit entry in app/Batch, and one policy decision the other filters do not have -- its selection is the HOLE it fills rather than a bound on the result, so a recorded step must name the selection it acted through, which is the same blocker DeleteSelection is waiting on"};
    case MenuAction::RemoveLightingGradient:
      return {CommandCoverageKind::NotYetRegistered, nullptr,
              "a document edit and genuinely recordable: PRD D8's divide-by-a-blurred-copy. It needs a runner for its `sigma` and a pixel-unit entry in app/Batch -- sigma is in document texels, so a recorded value replayed at another resolution blurs a different fraction of the picture, which is exactly what that list exists to correct"};
    case MenuAction::Offset:
      return {CommandCoverageKind::NotYetRegistered, nullptr,
              "a document edit and genuinely recordable: PRD D8's offset with wrap. It needs a runner for `dx`/`dy`/`edge`, and its by-half default is a FRACTION of the canvas rather than a length -- so the recorded form has to choose between the fraction and the texels it resolved to, and only the fraction survives a change of resolution"};
    case MenuAction::TilePreview:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "shows the document tiled 3x3 and changes not one texel of it -- a view, like Toggle Grayscale Preview and Fit Window. The session owns it"};
    case MenuAction::GaussianBlur:
      return {CommandCoverageKind::Registered, "filter_gaussian_blur", nullptr};
    case MenuAction::Sharpen:
      return {CommandCoverageKind::Registered, "filter_sharpen", nullptr};
    case MenuAction::UnsharpMask:
      return {CommandCoverageKind::Registered, "filter_unsharp_mask", nullptr};
    case MenuAction::AddNoise:
      return {CommandCoverageKind::Registered, "filter_add_noise", nullptr};
    case MenuAction::Emboss:
      return {CommandCoverageKind::Registered, "filter_emboss", nullptr};
    case MenuAction::Median:
      return {CommandCoverageKind::Registered, "filter_median", nullptr};
    case MenuAction::MotionBlur:
      return {CommandCoverageKind::Registered, "filter_motion_blur", nullptr};
    case MenuAction::ImageSize:
      return {CommandCoverageKind::Registered, "image_size", nullptr};
    case MenuAction::CanvasSize:
      return {CommandCoverageKind::Registered, "canvas_size", nullptr};
    case MenuAction::CropToSelection:
      return {CommandCoverageKind::Registered, "crop_to_selection", nullptr};
    case MenuAction::TrimToContent:
      return {CommandCoverageKind::Registered, "trim_to_content", nullptr};
    case MenuAction::AdjustLevels:
      return {CommandCoverageKind::Registered, "adjust_levels", nullptr};
    case MenuAction::AdjustCurves:
      return {CommandCoverageKind::Registered, "adjust_curves", nullptr};
    case MenuAction::AdjustExposure:
      return {CommandCoverageKind::Registered, "adjust_exposure", nullptr};
    case MenuAction::AdjustChannelMixer:
      return {CommandCoverageKind::Registered, "adjust_channel_mixer", nullptr};
    case MenuAction::AdjustDesaturate:
      return {CommandCoverageKind::Registered, "adjust_desaturate", nullptr};
    case MenuAction::AdjustBrightnessContrast:
      return {CommandCoverageKind::Registered, "adjust_brightness_contrast", nullptr};
    case MenuAction::AdjustHueSaturation:
      return {CommandCoverageKind::Registered, "adjust_hue_saturation", nullptr};
    case MenuAction::AdjustVibrance:
      return {CommandCoverageKind::Registered, "adjust_vibrance", nullptr};
    case MenuAction::AdjustColorBalance:
      return {CommandCoverageKind::Registered, "adjust_color_balance", nullptr};
    case MenuAction::AdjustBlackAndWhite:
      return {CommandCoverageKind::Registered, "adjust_black_and_white", nullptr};
    case MenuAction::AdjustPhotoFilter:
      return {CommandCoverageKind::Registered, "adjust_photo_filter", nullptr};
    case MenuAction::AdjustInvert:
      return {CommandCoverageKind::Registered, "adjust_invert", nullptr};
    case MenuAction::AdjustPosterize:
      return {CommandCoverageKind::Registered, "adjust_posterize", nullptr};
    case MenuAction::AdjustThreshold:
      return {CommandCoverageKind::Registered, "adjust_threshold", nullptr};
    case MenuAction::AdjustGradientMap:
      return {CommandCoverageKind::Registered, "adjust_gradient_map", nullptr};
    case MenuAction::AdjustAutoTone:
      return {CommandCoverageKind::Registered, "adjust_auto_tone", nullptr};
    case MenuAction::AdjustAutoContrast:
      return {CommandCoverageKind::Registered, "adjust_auto_contrast", nullptr};
    case MenuAction::AdjustAutoColor:
      return {CommandCoverageKind::Registered, "adjust_auto_color", nullptr};
    case MenuAction::AdjustEqualize:
      return {CommandCoverageKind::Registered, "adjust_equalize", nullptr};
    case MenuAction::Count:
      // Not an action: the enum's own size marker.
      return {CommandCoverageKind::NotRecordable, nullptr, "the enum's count marker, not an action"};
  }
  return {CommandCoverageKind::NotRecordable, nullptr, "unclassified"};
}

}  // namespace np
