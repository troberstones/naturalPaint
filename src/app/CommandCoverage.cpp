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
    case MenuAction::ExportRegions:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "as ExportAs; io/ExportRegions is the loop the batch runner would reuse"};
    case MenuAction::Batch:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "opens a dialog. The batch itself is not a step -- it RUNS actions, and an "
              "action that could contain a batch of itself is a loop with no base case"};
    case MenuAction::Quit:
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
      // Registered as `numeric_transform`, scoped to the whole-active-layer
      // case (`app/TransformSession`'s `TransformTarget::Layer`) only -- a
      // live selection is refused by name rather than reaching for
      // `TransformTarget::SelectionPixels`, because that path belongs to
      // `app/TransformSession`, which this track was told not to touch (the
      // xform track is making Free Transform work on a multi-layer selection
      // through it). Rotate and the two scale percentages are
      // resolution-independent numbers; `translate_x`/`translate_y` are texels
      // and are in app/Batch.cpp's `kPixelUnitParams`; the pivot is not a
      // parameter at all -- it is recomputed from the replaying document's own
      // active-layer content bounds. See app/CommandsImage.cpp's
      // `doNumericTransform()`.
      return {CommandCoverageKind::Registered, "numeric_transform", nullptr};
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
      // Registered as `delete_selection`. The blocker this row states was
      // "how a step names the selection it acted through", and the answer is
      // the mechanism `save_selection_as_channel`/`load_channel_as_selection`
      // and `selectionBounded` already exist for -- app/Recorder.hpp §4's
      // channel-match rule refuses recording this step under a live marquee no
      // saved channel matches, naming the fix, exactly as it already does for
      // `filter_gaussian_blur`. No second mechanism was needed: an absent
      // selection silently clears the whole layer
      // (`core::clearThroughSelection()`'s own documented default), which is
      // the textbook case the flag was built for.
      return {CommandCoverageKind::Registered, "delete_selection", nullptr};
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
    case MenuAction::ShowRegions:
      return {CommandCoverageKind::NotRecordable, nullptr,
              "view furniture -- the regions it shows are recorded, it is not"};
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
    // --- PLAN.md phases 8 and 9, all three now Registered -------------------
    //
    // All three are pixel ops on the active layer, of exactly the shape the
    // seven `Registered` filters below have, and all three are functions of an
    // `OpenDocument` alone -- so app/Command.hpp §1's rule puts them IN.
    // `filter_inpaint`, `filter_remove_lighting_gradient` and `filter_offset`
    // are the rows; app/CommandsImage.cpp has the runners, app/CommandsImage.hpp
    // has the encoders, and app/Batch.cpp's `kPixelUnitParams` carries `sigma`.
    case MenuAction::Inpaint:
      // Its selection is the HOLE it fills rather than a bound, so an absent
      // one is a hard refusal (`inpaintRefusal()`'s `NoSelection`) rather than
      // "the whole canvas" -- `filter_inpaint` is `selectionBounded` anyway,
      // because the recorder's channel-match rule (app/Recorder.hpp §4) is the
      // exact protection this op needs against a live, unsaved marquee, even
      // though the flag's usual reading does not literally describe it. Named
      // as the second bounded exception (beside `crop_to_selection`) in
      // app/selftest/Command.cpp section H.
      return {CommandCoverageKind::Registered, "filter_inpaint", nullptr};
    case MenuAction::RemoveLightingGradient:
      return {CommandCoverageKind::Registered, "filter_remove_lighting_gradient", nullptr};
    case MenuAction::Offset:
      // `dx`/`dy` are recorded as `dx_fraction`/`dy_fraction` -- a FRACTION of
      // the canvas, not the texels `offsetByHalf()` resolves them to -- because
      // the canonical use is exactly "by half of whatever canvas this document
      // is", and only the fraction still means that after a resize. NOT
      // `selectionBounded`: `offsetRefusalFor()` refuses outright under ANY
      // live selection, so an absent one is never "the whole canvas" reading
      // that needed protecting.
      return {CommandCoverageKind::Registered, "filter_offset", nullptr};
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
