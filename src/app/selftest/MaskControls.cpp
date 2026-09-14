// T16's remaining mask controls. Every assertion calls the production
// function; the gestures go through real ImGui frames.
#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/CanvasView.hpp"
#include "app/LayerThumbClick.hpp"
#include "app/LayerThumbnail.hpp"
#include "app/SelfTest.hpp"
#include "app/StrokeSession.hpp"
#include "brush/MaskTools.hpp"
#include "brush/Taper.hpp"
#include "imgui.h"
#include "io/PsdExport.hpp"
#include "io/PsdImport.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {

bool runMaskControlsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-74s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("[selftest] mask controls: T16's Shift-click disable, Option-click view, and the "
              "other tools on a mask\n");

  constexpr int32_t kW = 256, kH = 256;

  auto fillRgb = [](Document& doc, size_t li, const std::array<float, 4>& premultiplied,
                    int32_t x0 = 0, int32_t x1 = kW - 1) {
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord at{x, y};
        doc.layers[li].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at),
                                                                         premultiplied);
      }
  };
  auto fillPigment = [](Document& doc, size_t li, const PigmentTexel& t) {
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) {
        const PixelCoord at{x, y};
        doc.layers[li].pigmentTiles->getOrCreate(tileCoordAt(at)).writeTexel(tileLocalOffset(at),
                                                                              t);
      }
  };
  auto maskRect = [](MaskTileStore& m, int32_t x0, int32_t y0, int32_t x1, int32_t y1, float v) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord at{x, y};
        m.getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at), v);
      }
  };
  auto maskAt = [](const MaskTileStore& m, int32_t x, int32_t y) {
    const PixelCoord at{x, y};
    return maskCoverage(m.find(tileCoordAt(at)), tileLocalOffset(at));
  };

  // ---- A. the compositor: a disabled mask composites as no mask ---------------
  std::printf("  -- A. every compositor leaf and both opaque-floor shortcuts --\n");
  {
    // `doc` has a mask on layer `li` that visibly changes the picture. The same
    // document with that mask disabled must be bit-identical to it with the
    // mask removed, with the floor on and off.
    auto disabledIsAbsent = [&](const Document& doc, size_t li, const char* what,
                                const char* nonVacuous) {
      Document disabled = doc;
      disabled.layers[li].maskEnabled = false;
      Document absent = doc;
      absent.layers[li].mask.reset();
      bool same = true;
      for (const bool floorOn : {true, false}) {
        setOpaqueFloorEnabledForTesting(floorOn);
        if (compositeDocumentPremultiplied(disabled) != compositeDocumentPremultiplied(absent))
          same = false;
      }
      setOpaqueFloorEnabledForTesting(true);
      check(compositeDocumentPremultiplied(doc) != compositeDocumentPremultiplied(absent),
            nonVacuous);
      check(same, what);
    };

    {  // A1: the main walk's own mask
      Document doc = Document::createBlank(kW, kH, WorkingSpace{});
      fillRgb(doc, 0, {0.8f, 0.0f, 0.0f, 1.0f});
      addLayer(doc, 1, makeRgbLayer("masked"));
      fillRgb(doc, 1, {0.0f, 0.4f, 0.0f, 0.6f});
      addLayerMask(doc, 1);
      maskRect(*doc.layers[1].mask, 0, 0, 127, 255, 0.0f);
      disabledIsAbsent(doc, 1, "A1 main walk: disabled mask == no mask",
                       "A1 setup: the enabled mask changes the picture");
      check(layerMaskCoverageAt(doc.layers[1], PixelCoord{10, 10}) == 0.0f,
            "A1 probe: enabled mask reads 0 where it hides");
      doc.layers[1].maskEnabled = false;
      check(layerMaskCoverageAt(doc.layers[1], PixelCoord{10, 10}) == 1.0f,
            "A1 probe: layerMaskCoverageAt reads 1 through a disabled mask");
    }
    {  // A2: an Adjustment layer's mask
      Document doc = Document::createBlank(kW, kH, WorkingSpace{});
      fillRgb(doc, 0, {0.3f, 0.3f, 0.3f, 1.0f});
      addLayer(doc, 1, makeAdjustmentLayer("grade"));
      Op op;
      op.opClass = OpClass::PointA;
      op.pointKind = PointOpKind::Exposure;
      op.exposure.stops = 1.0f;
      doc.layers[1].ops.add(op);
      addLayerMask(doc, 1);
      maskRect(*doc.layers[1].mask, 0, 0, 127, 255, 0.0f);
      disabledIsAbsent(doc, 1, "A2 adjustment layer: disabled mask == no mask",
                       "A2 setup: the enabled mask changes the picture");
    }
    {  // A3: a clip member's mask
      Document doc = Document::createBlank(kW, kH, WorkingSpace{});
      fillRgb(doc, 0, {0.6f, 0.0f, 0.0f, 1.0f}, 0, 180);
      addLayer(doc, 1, makeRgbLayer("member"));
      fillRgb(doc, 1, {0.0f, 0.0f, 0.5f, 0.7f});
      check(setLayerClipped(doc, 1, true).ok, "A3 setup: layer 1 clips to layer 0");
      addLayerMask(doc, 1);
      maskRect(*doc.layers[1].mask, 0, 0, 127, 255, 0.0f);
      disabledIsAbsent(doc, 1, "A3 clip member: disabled mask == no mask",
                       "A3 setup: the enabled mask changes the picture");
    }
    auto mixDoc = [&](float mass) {
      Document doc = Document::createBlank(kW, kH, WorkingSpace{});
      fillRgb(doc, 0, {0.2f, 0.2f, 0.6f, 0.5f});
      addLayer(doc, 1, makePigmentLayer("mix-lower"));
      PigmentTexel lo;
      lo.latent.c = {0.5f, 0.1f, 0.1f};
      lo.mass = mass;
      fillPigment(doc, 1, lo);
      addLayer(doc, 2, makePigmentLayer("mix-upper"));
      PigmentTexel hi;
      hi.latent.c = {0.1f, 0.5f, 0.1f};
      hi.mass = mass;
      fillPigment(doc, 2, hi);
      setLayerBlend(doc, 2, BlendMode::Mix);
      return doc;
    };
    {  // A4/A5: both halves of a mixed pair
      Document lower = mixDoc(0.7f);
      check(mixPairing(lower).mixedWithBelow[2], "A4 setup: the two Pigment layers pair");
      addLayerMask(lower, 1);
      maskRect(*lower.layers[1].mask, 0, 0, 127, 255, 0.0f);
      disabledIsAbsent(lower, 1, "A4 mixed pair, lower half: disabled mask == no mask",
                       "A4 setup: the enabled mask changes the picture");
      Document upper = mixDoc(0.7f);
      addLayerMask(upper, 2);
      maskRect(*upper.layers[2].mask, 0, 0, 127, 255, 0.0f);
      disabledIsAbsent(upper, 2, "A5 mixed pair, upper half: disabled mask == no mask",
                       "A5 setup: the enabled mask changes the picture");
    }

    // The floor sum is the shortcuts' only observable: a mask read wrongly
    // there skips less and paints the same pixels.
    auto floorSums = [&](const Document& doc, size_t li, size_t* enabled, size_t* disabled,
                         size_t* absent) {
      setOpaqueFloorEnabledForTesting(true);
      Document d = doc;
      takeOpaqueFloorSumForTesting();
      compositeDocumentPremultiplied(d);
      *enabled = takeOpaqueFloorSumForTesting();
      d.layers[li].maskEnabled = false;
      compositeDocumentPremultiplied(d);
      *disabled = takeOpaqueFloorSumForTesting();
      d.layers[li].mask.reset();
      compositeDocumentPremultiplied(d);
      *absent = takeOpaqueFloorSumForTesting();
    };
    {  // A6: the ordinary layer's floor test
      Document doc = Document::createBlank(kW, kH, WorkingSpace{});
      fillRgb(doc, 0, {0.3f, 0.0f, 0.0f, 0.5f});
      addLayer(doc, 1, makeRgbLayer("semi"));
      fillRgb(doc, 1, {0.0f, 0.2f, 0.0f, 0.4f});
      addLayer(doc, 2, makeRgbLayer("opaque"));
      fillRgb(doc, 2, {0.1f, 0.1f, 0.7f, 1.0f});
      addLayerMask(doc, 2);
      maskRect(*doc.layers[2].mask, 0, 0, 255, 255, 0.5f);
      size_t en = 0, dis = 0, ab = 0;
      floorSums(doc, 2, &en, &dis, &ab);
      std::printf("  [floor sums] ordinary: enabled %zu, disabled %zu, absent %zu\n", en, dis, ab);
      check(ab > en, "A6 setup: the enabled mask stops the floor from firing");
      check(dis == ab, "A6 floor shortcut: a disabled mask floors exactly like no mask");
    }
    {  // A7: the mixed pair's floor test, one leaf per half. One masked half
       // leaves the pair opaque, so both are masked and each is toggled.
      for (const size_t li : {size_t{2}, size_t{1}}) {
        Document doc = mixDoc(1.0f);
        addLayerMask(doc, 1);
        addLayerMask(doc, 2);
        maskRect(*doc.layers[1].mask, 0, 0, 255, 255, 0.5f);
        maskRect(*doc.layers[2].mask, 0, 0, 255, 255, 0.5f);
        size_t en = 0, dis = 0, ab = 0;
        floorSums(doc, li, &en, &dis, &ab);
        std::printf("  [floor sums] mix pair, layer %zu toggled: enabled %zu, disabled %zu, "
                    "absent %zu\n",
                    li, en, dis, ab);
        check(ab > en, li == 2 ? "A7 setup: both halves masked, the pair does not floor"
                               : "A7b setup: both halves masked, the pair does not floor");
        check(dis == ab, li == 2 ? "A7 mix-pair floor shortcut, upper half: disabled == no mask"
                                 : "A7b mix-pair floor shortcut, lower half: disabled == no mask");
      }
    }
  }

  // ---- B. the operation, undo, dirty tiles, the row and the thumbnail --------
  std::printf("  -- B. setLayerMaskEnabled, undo, incremental composite, row, thumbnail --\n");
  auto maskedDoc = [&](const char* name) {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, name);
    fillRgb(od.document, 0, {0.5f, 0.4f, 0.2f, 1.0f});
    addLayerMask(od.document, 0);
    maskRect(*od.document.layers[0].mask, 20, 20, 120, 120, 0.0f);
    maskRect(*od.document.layers[0].mask, 140, 140, 200, 200, 0.5f);
    od.recordEdit("fixture", EditKind::Content);
    return od;
  };
  {
    OpenDocument plain = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "plain");
    check(!setLayerMaskEnabled(plain.document, 0, false).ok,
          "B refusal: a layer with no mask cannot have one disabled");

    OpenDocument od = maskedDoc("toggle");
    check(!setLayerMaskEnabled(od.document, 0, true).ok,
          "B refusal: enabling an already-enabled mask is refused, not a no-op entry");
    od.document.layers[0].locked = true;
    check(!setLayerMaskEnabled(od.document, 0, false).ok,
          "B refusal: a locked layer refuses the toggle");
    od.document.layers[0].locked = false;

    const Document before = od.document;
    const size_t entries = od.history.entries().size();
    const DocumentOpResult off = recordLayerEdit(od, setLayerMaskEnabled(od.document, 0, false));
    check(off.ok && !od.document.layers[0].maskEnabled, "B disable: the mask is disabled");
    check(od.history.entries().size() == entries + 1 &&
              contains(od.history.entries().back().label, "disable mask"),
          "B disable: exactly one history entry, labelled 'disable mask'");
    check(maskAt(*od.document.layers[0].mask, 50, 50) == 0.0f &&
              maskAt(*od.document.layers[0].mask, 170, 170) ==
                  maskAt(*before.layers[0].mask, 170, 170),
          "B disable: the mask's texels are untouched");

    const DocumentDirtyTiles dirty = documentDirtyTiles(before, od.document);
    check(dirty.everything && dirty.reason == FullRecompositeReason::LayerMaskEnabledChanged,
          "B dirty tiles: the toggle recomposites, named as a mask enable change");

    check(contains(layerRowSubLine(od.document, 0), "MASK OFF"),
          "B row: the sub-line says MASK OFF");
    check(!contains(layerRowSubLine(before, 0), "MASK OFF") &&
              contains(layerRowSubLine(before, 0), "MASK"),
          "B row: an enabled mask still says MASK");

    auto redTexels = [](const LayerThumbnail& t) {
      size_t n = 0;
      for (size_t o = 0; o + 3 < t.rgba.size(); o += 4)
        if (t.rgba[o] == kMaskOffMarkRgb[0] && t.rgba[o + 1] == kMaskOffMarkRgb[1] &&
            t.rgba[o + 2] == kMaskOffMarkRgb[2])
          ++n;
      return n;
    };
    const size_t redOn = redTexels(layerMaskThumbnail(before, 0));
    const size_t redOff = redTexels(layerMaskThumbnail(od.document, 0));
    std::printf("  [thumbnail] X texels: enabled %zu, disabled %zu\n", redOn, redOff);
    check(redOn == 0 && redOff >= static_cast<size_t>(kLayerThumbPx),
          "B thumbnail: a disabled mask carries the red X, an enabled one none");

    const Document* undone = od.history.undo();
    if (undone != nullptr) od.document = *undone;
    check(undone != nullptr && od.document.layers[0].maskEnabled,
          "B undo: undo re-enables the mask");
  }

  // ---- C. persistence ----------------------------------------------------------
  std::printf("  -- C. .npaint and PSD carry the disabled state --\n");
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string dir = "selftest_maskcontrols";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    OpenDocument od = maskedDoc("persist");
    Document disabled = od.document;
    disabled.layers[0].maskEnabled = false;

    const std::string offPath = dir + "/off.npaint";
    const std::string onPath = dir + "/on.npaint";
    const bool savedOff = saveNpaint(disabled, offPath).ok;
    const bool savedOn = saveNpaint(od.document, onPath).ok;
    const NpaintLoadResult offBack = loadNpaint(offPath);
    const NpaintLoadResult onBack = loadNpaint(onPath);
    check(savedOff && offBack.ok && offBack.document.layers[0].mask.has_value() &&
              !offBack.document.layers[0].maskEnabled,
          ".npaint: a disabled mask reloads disabled");
    check(offBack.ok && offBack.document.layers[0].mask.has_value() &&
              maskAt(*offBack.document.layers[0].mask, 50, 50) == 0.0f,
          ".npaint: and its texels came back with it");
    check(savedOn && onBack.ok && onBack.document.layers[0].maskEnabled,
          ".npaint: an enabled mask reloads enabled");
    const NpaintVerifyResult verified = verifyNpaintRoundTrip(disabled, offPath);
    if (!verified.ok) std::printf("  [npaint verify] %s\n", verified.error.c_str());
    check(savedOff && verified.ok, ".npaint: verifyNpaintRoundTrip accepts a disabled mask");
    fs::remove_all(dir, ec);

    auto psdMasked = [](const PsdImportResult& r) -> const Layer* {
      if (!r.ok) return nullptr;
      for (const Layer& l : r.document.layers)
        if (l.mask.has_value()) return &l;
      return nullptr;
    };
    const PsdExportResult offPsd = writeLayeredPsd(disabled);
    const PsdExportResult onPsd = writeLayeredPsd(od.document);
    const PsdImportResult offImport =
        importPsd(std::span<const uint8_t>(offPsd.bytes.data(), offPsd.bytes.size()));
    const PsdImportResult onImport =
        importPsd(std::span<const uint8_t>(onPsd.bytes.data(), onPsd.bytes.size()));
    const Layer* offLayer = psdMasked(offImport);
    const Layer* onLayer = psdMasked(onImport);
    check(offPsd.ok && offLayer != nullptr && !offLayer->maskEnabled,
          "PSD: a disabled mask exports with flags bit 1 and imports disabled");
    check(offLayer != nullptr && maskAt(*offLayer->mask, 50, 50) < 0.01f,
          "PSD: and keeps its texels");
    check(onPsd.ok && onLayer != nullptr && onLayer->maskEnabled,
          "PSD: an enabled mask imports enabled");
  }

  // ---- D. the mask view ------------------------------------------------------
  std::printf("  -- D. Option-click view: the packed image and the status marker --\n");
  {
    OpenDocument od = maskedDoc("view pack");
    const std::vector<uint16_t> px = packLayerMaskViewHalf(od.document.layers[0], kW, kH);
    auto at = [&](int32_t x, int32_t y, int c) {
      return px[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4 + c];
    };
    check(px.size() == static_cast<size_t>(kW) * kH * 4, "view pack: one RGBA texel per pixel");
    check(at(50, 50, 0) == floatToHalf(0.0f) && at(5, 5, 0) == floatToHalf(1.0f),
          "view pack: hidden is black, untouched mask is white");
    check(at(170, 170, 0) == floatToHalf(srgbDecode(0.5f)) &&
              at(170, 170, 1) == at(170, 170, 0) && at(170, 170, 2) == at(170, 170, 0),
          "view pack: coverage 0.5 is decoded grey, R=G=B, so it displays mid grey");
    check(at(50, 50, 3) == floatToHalf(1.0f), "view pack: opaque -- it replaces the canvas");

    check(atelierViewStateMarkers(CanvasView{}, false).empty() &&
              atelierViewStateMarkers(CanvasView{}, true) == "MASK VIEW",
          "status bar: MASK VIEW appears exactly while a mask is viewed");
  }

  // ---- E. the gestures, through headless ImGui frames ------------------------
  std::printf("  -- E. the thumbnail gestures: plain, Shift and Option clicks --\n");
  {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    applyAtelierTheme();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.0f, 200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    io.IniFilename = nullptr;
    io.ConfigInputTrickleEventQueue = false;

    OpenDocument od = maskedDoc("gestures");
    addLayer(od.document, 1, makeRgbLayer("top, no mask"));
    od.recordEdit("second layer", EditKind::Structural);
    setActiveLayer(od, 1);

    const ImVec2 contentAt(20.0f, 20.0f), maskAt2(60.0f, 20.0f);
    LayerThumbClickResult last;
    auto frame = [&](ImVec2 mouse, bool down, bool shift, bool alt) {
      io.AddKeyEvent(ImGuiMod_Shift, shift);
      io.AddKeyEvent(ImGuiMod_Alt, alt);
      io.AddMousePosEvent(mouse.x, mouse.y);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
      ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
      if (ImGui::Begin("##maskgestures", nullptr,
                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings)) {
        for (const auto& [which, pos] :
             {std::pair{LayerThumb::Content, contentAt}, std::pair{LayerThumb::Mask, maskAt2}}) {
          const LayerThumbClickResult r = layerThumbButton(od, 0, which, pos.x, pos.y,
                                                           static_cast<float>(kLayerThumbPx));
          if (r.action != LayerThumbAction::None) last = r;
        }
      }
      ImGui::End();
      ImGui::EndFrame();
    };
    auto click = [&](ImVec2 thumb, bool shift, bool alt) {
      last = LayerThumbClickResult{};
      const ImVec2 p(thumb.x + 12.0f, thumb.y + 12.0f);
      frame(p, false, shift, alt);
      frame(p, true, shift, alt);
      frame(p, false, shift, alt);
      frame(ImVec2(390.0f, 190.0f), false, false, false);
    };
    const Layer& masked = od.document.layers[0];

    click(maskAt2, false, false);
    check(last.action == LayerThumbAction::TargetMask && od.maskIsEditTarget &&
              activeLayerIndex(od) == std::optional<size_t>(0) && masked.maskEnabled &&
              maskViewLayer(od) == nullptr,
          "E plain click on the mask: selects the row and aims at the mask, nothing else");

    const size_t entries = od.history.entries().size();
    setActiveLayer(od, 1);
    click(maskAt2, true, false);
    check(last.action == LayerThumbAction::ToggleMaskEnabled && !od.document.layers[0].maskEnabled &&
              od.history.entries().size() == entries + 1,
          "E Shift-click on the mask: disables it, one history entry");
    check(activeLayerIndex(od) == std::optional<size_t>(1) && !last.selectsRow,
          "E Shift-click: does not change the selected row");
    click(maskAt2, true, false);
    check(od.document.layers[0].maskEnabled, "E Shift-click again: re-enables it");
    click(contentAt, true, false);
    check(od.document.layers[0].maskEnabled && last.action == LayerThumbAction::TargetContent,
          "E Shift-click on the LAYER thumbnail: no toggle");

    click(maskAt2, false, true);
    check(last.action == LayerThumbAction::ToggleMaskView &&
              maskViewLayer(od) == &od.document.layers[0] && od.maskIsEditTarget,
          "E Option-click on the mask: shows it alone, row selected, mask targeted");
    const OpenDocument* markerDoc = &od;
    check(atelierViewStateMarkers(CanvasView{}, maskViewLayer(*markerDoc) != nullptr) ==
              "MASK VIEW",
          "E status bar: the marker follows the view");
    click(maskAt2, false, true);
    check(maskViewLayer(od) == nullptr, "E Option-click again: leaves the view");
    click(maskAt2, true, true);
    check(maskViewLayer(od) != nullptr && od.document.layers[0].maskEnabled,
          "E Shift+Option: Option wins -- view, no toggle");
    click(contentAt, false, false);
    check(maskViewLayer(od) == nullptr && !od.maskIsEditTarget,
          "E click on the layer thumbnail: leaves the view and aims at the pixels");
    click(maskAt2, false, true);
    addLayerMask(od.document, 1);  // so the other row has a mask the view could wrongly show
    setActiveLayer(od, 1);
    check(maskViewLayer(od) == nullptr,
          "E selecting another row, itself masked: the view no longer applies");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
  }

  // ---- F. the other tools on a mask ------------------------------------------
  std::printf("  -- F. Pencil, Eraser, Dodge/Burn, Smudge, Clone Stamp on a mask --\n");
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    addLayerMask(doc, 0);
    const Layer& l = doc.layers[0];
    check(strokeRouteFor(Tool::Pencil, &l, LayerEditTarget::Mask) == StrokeRoute::MaskPaint &&
              strokeRouteFor(Tool::Eraser, &l, LayerEditTarget::Mask) == StrokeRoute::MaskPaint,
          "F route: Pencil and Eraser -> mask-paint");
    check(strokeRouteFor(Tool::Dodge, &l, LayerEditTarget::Mask) == StrokeRoute::MaskTonal &&
              strokeRouteFor(Tool::Burn, &l, LayerEditTarget::Mask) == StrokeRoute::MaskTonal,
          "F route: Dodge and Burn -> mask-tonal");
    check(strokeRouteFor(Tool::Smudge, &l, LayerEditTarget::Mask) == StrokeRoute::MaskSmudge &&
              strokeRouteFor(Tool::CloneStamp, &l, LayerEditTarget::Mask) ==
                  StrokeRoute::MaskClone,
          "F route: Smudge -> mask-smudge, Clone Stamp -> mask-clone");
    check(strokeRouteFor(Tool::Heal, &l, LayerEditTarget::Mask) == StrokeRoute::MaskHeal &&
              strokeRouteWritesMask(StrokeRoute::MaskHeal) &&
              std::strcmp(strokeRouteName(StrokeRoute::MaskHeal), "mask-heal") == 0,
          "F route: Heal -> mask-heal, a mask route with a name");
    check(strokeRouteFor(Tool::Water, &l, LayerEditTarget::Mask) == StrokeRoute::None,
          "F route: Water still refuses a mask");
    check(std::strcmp(strokeRouteName(StrokeRoute::MaskTonal), "mask-tonal") == 0 &&
              std::strcmp(strokeRouteName(StrokeRoute::MaskSmudge), "mask-smudge") == 0 &&
              std::strcmp(strokeRouteName(StrokeRoute::MaskClone), "mask-clone") == 0 &&
              strokeRouteWritesLayer(StrokeRoute::MaskTonal) &&
              strokeRouteWritesLayer(StrokeRoute::MaskSmudge) &&
              strokeRouteWritesLayer(StrokeRoute::MaskClone),
          "F route: the three new routes have names and write a layer");
  }

  auto tipOf = [](float radius, float hardness) {
    BrushTip t;
    t.radius = radius;
    t.hardness = hardness;
    t.flow = 1.0f;
    t.opacity = 1.0f;
    t.linearRgb = {0.0f, 0.0f, 0.0f};
    return t;
  };
  // A mask-targeted stroke through StrokeSession, the path the canvas takes.
  auto strokeMask = [&](OpenDocument& od, Tool tool, const BrushTip& tip, Vec2 from, Vec2 to,
                        const AppState::CloneSourceState* clone, std::string* why) {
    od.maskIsEditTarget = true;
    StrokeSession s;
    if (!s.begin(od, 0, tip, tool, why, nullptr, DynamicInputs{}, clone)) return false;
    constexpr int kSteps = 24;
    for (int k = 0; k <= kSteps; ++k) {
      const float u = static_cast<float>(k) / kSteps;
      s.addPoint(from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u);
    }
    s.end();
    return true;
  };
  auto freshMask = [&](const char* name) {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, name);
    addLayerMask(od.document, 0);
    return od;
  };
  auto contentUntouched = [](const OpenDocument& od) {
    return od.document.layers[0].rgbTiles->occupiedTileCount() == 0;
  };
  {  // Pencil: hard-edged, so no texel is left between 0 and 1
    auto intermediates = [&](Tool tool) {
      OpenDocument od = freshMask("pencil");
      std::string why;
      strokeMask(od, tool, tipOf(20.0f, 0.2f), Vec2{60.0f, 128.0f}, Vec2{200.0f, 128.0f}, nullptr,
                 &why);
      size_t between = 0, hidden = 0;
      for (int32_t y = 96; y <= 160; ++y) {
        const float v = maskAt(*od.document.layers[0].mask, 128, y);
        if (v > 0.001f && v < 0.999f) ++between;
        if (v == 0.0f) ++hidden;
      }
      return std::array<size_t, 3>{between, hidden, contentUntouched(od) ? size_t{1} : size_t{0}};
    };
    const auto pencil = intermediates(Tool::Pencil);
    const auto brush = intermediates(Tool::Brush);
    std::printf("  [pencil] column texels between 0 and 1: pencil %zu, brush %zu\n", pencil[0],
                brush[0]);
    check(pencil[1] > 0 && pencil[0] == 0,
          "F Pencil: hides with a hard edge -- every texel is 0 or 1");
    check(brush[0] > 0, "F Pencil: the same soft tip on Brush does leave a soft edge");
    check(pencil[2] == 1, "F Pencil: the layer's own pixels are untouched");
  }
  {  // Eraser: reveals whatever the ink
    OpenDocument od = freshMask("eraser");
    maskRect(*od.document.layers[0].mask, 60, 60, 196, 196, 0.0f);
    std::string why;
    const bool began = strokeMask(od, Tool::Eraser, tipOf(16.0f, 1.0f), Vec2{80.0f, 128.0f},
                                  Vec2{180.0f, 128.0f}, nullptr, &why);
    check(began && maskAt(*od.document.layers[0].mask, 128, 128) > 0.99f,
          "F Eraser: pushes a hidden mask back to reveal, with black ink loaded");
    check(maskAt(*od.document.layers[0].mask, 128, 70) == 0.0f,
          "F Eraser: and leaves the mask off the stroke alone");
    check(contentUntouched(od) && contains(od.history.entries().back().label, "mask"),
          "F Eraser: layer pixels untouched, history names the mask");
  }
  {  // Dodge raises, Burn lowers
    auto toned = [&](Tool tool) {
      OpenDocument od = freshMask("tonal");
      maskRect(*od.document.layers[0].mask, 60, 60, 196, 196, 0.5f);
      std::string why;
      strokeMask(od, tool, tipOf(16.0f, 1.0f), Vec2{80.0f, 128.0f}, Vec2{180.0f, 128.0f}, nullptr,
                 &why);
      return std::array<float, 2>{maskAt(*od.document.layers[0].mask, 128, 128),
                                  maskAt(*od.document.layers[0].mask, 128, 70)};
    };
    const auto dodge = toned(Tool::Dodge);
    const auto burn = toned(Tool::Burn);
    std::printf("  [tonal] 0.5 -> dodge %.4f, burn %.4f\n", static_cast<double>(dodge[0]),
                static_cast<double>(burn[0]));
    check(dodge[0] > 0.6f, "F Dodge: raises coverage (reveals)");
    check(burn[0] < 0.4f, "F Burn: lowers coverage (hides)");
    check(dodge[1] == 0.5f && burn[1] == 0.5f, "F Dodge/Burn: off the stroke stays 0.5");
    OpenDocument untouched = freshMask("tonal on reveal");
    std::string why;
    strokeMask(untouched, Tool::Burn, tipOf(16.0f, 1.0f), Vec2{80.0f, 128.0f},
               Vec2{180.0f, 128.0f}, nullptr, &why);
    check(untouched.document.layers[0].mask->occupiedTileCount() == 0,
          "F Burn: coverage 1 is a fixed point, so a blank mask allocates nothing");
  }
  {  // Exit taper repaints a mask stroke from the mask it started on
    auto tapered = [&](bool on) {
      OpenDocument od = freshMask("taper");
      maskRect(*od.document.layers[0].mask, 60, 60, 196, 196, 0.5f);
      od.maskIsEditTarget = true;
      NativeBrush native;
      native.taperOut = BrushTaper{on, 40.0f, 10.0f};
      StrokeSession s;
      std::string why;
      s.begin(od, 0, tipOf(16.0f, 1.0f), Tool::Burn, &why, nullptr, DynamicInputs{}, nullptr,
              StabiliserParams{}, 1.0f, &native);
      for (int k = 0; k <= 24; ++k) s.addPoint(80.0f + 100.0f * static_cast<float>(k) / 24, 128.0f);
      s.end();
      // 12 px off the line: inside the full radius, outside the tapered one.
      // x=100 is only reached by dabs before the 40 px tail; x=176 by the tail.
      return std::array<float, 3>{maskAt(*od.document.layers[0].mask, 100, 140),
                                  maskAt(*od.document.layers[0].mask, 176, 140),
                                  contentUntouched(od) ? 1.0f : 0.0f};
    };
    const auto off = tapered(false);
    const auto on = tapered(true);
    std::printf("  [taper] burn head %.4f/%.4f, tail %.4f/%.4f (off/on)\n",
                static_cast<double>(off[0]), static_cast<double>(on[0]),
                static_cast<double>(off[1]), static_cast<double>(on[1]));
    check(on[1] > off[1] + 0.01f, "F exit taper on a mask: the tail burns less -- the fixture discriminates");
    check(std::fabs(on[0] - off[0]) < 1e-4f,
          "F exit taper on a mask: the repaint restores the mask, so the head is not burnt twice");
    check(on[2] == 1.0f, "F exit taper on a mask: the layer's own pixels stay untouched");
  }
  {  // Smudge drags hidden coverage into revealed coverage
    OpenDocument od = freshMask("smudge");
    maskRect(*od.document.layers[0].mask, 0, 0, 127, 255, 0.0f);
    BrushTip tip = tipOf(10.0f, 1.0f);
    tip.smudgeStrength = 0.9f;
    std::string why;
    const bool began =
        strokeMask(od, Tool::Smudge, tip, Vec2{100.0f, 128.0f}, Vec2{190.0f, 128.0f}, nullptr, &why);
    const float dragged = maskAt(*od.document.layers[0].mask, 150, 128);
    std::printf("  [smudge] coverage 22 px into the revealed half: %.4f\n",
                static_cast<double>(dragged));
    check(began && dragged < 0.9f, "F Smudge: smears hidden coverage into the revealed half");
    check(maskAt(*od.document.layers[0].mask, 150, 20) == 1.0f &&
              maskAt(*od.document.layers[0].mask, 60, 20) == 0.0f,
          "F Smudge: and nothing off the stroke moves");
    check(contentUntouched(od), "F Smudge: the layer's own pixels are untouched");
  }
  {  // Clone Stamp copies coverage from the offset
    OpenDocument od = freshMask("clone");
    maskRect(*od.document.layers[0].mask, 30, 30, 80, 80, 0.0f);
    AppState::CloneSourceState noSource;
    std::string refusal;
    check(!strokeMask(od, Tool::CloneStamp, tipOf(8.0f, 1.0f), Vec2{150.0f, 150.0f},
                      Vec2{160.0f, 150.0f}, &noSource, &refusal) &&
              !refusal.empty(),
          "F Clone Stamp: refuses without a source, as on pixels");
    AppState::CloneSourceState clone;
    clone.haveAnchor = true;
    clone.haveOffset = true;
    clone.offset = Vec2{-100.0f, -100.0f};
    std::string why;
    const bool began = strokeMask(od, Tool::CloneStamp, tipOf(8.0f, 1.0f), Vec2{150.0f, 155.0f},
                                  Vec2{160.0f, 155.0f}, &clone, &why);
    check(began && maskAt(*od.document.layers[0].mask, 155, 155) < 0.01f,
          "F Clone Stamp: copies the hidden square from (-100, -100)");
    check(maskAt(*od.document.layers[0].mask, 155, 190) == 1.0f &&
              maskAt(*od.document.layers[0].mask, 55, 55) == 0.0f,
          "F Clone Stamp: off the stroke and the source itself are unchanged");
    check(contentUntouched(od), "F Clone Stamp: the layer's own pixels are untouched");
  }
  auto healAt = [&](OpenDocument& od, Vec2 c, float radius, std::string* why) {
    return strokeMask(od, Tool::Heal, tipOf(radius, 1.0f), c, c, nullptr, why);
  };
  {  // Heal: a speck inside the dab, and one inside its rectangle but outside it
    OpenDocument od = freshMask("heal speck");
    maskRect(*od.document.layers[0].mask, 0, 0, 255, 255, 0.6f);
    maskRect(*od.document.layers[0].mask, 126, 126, 129, 129, 0.0f);
    maskRect(*od.document.layers[0].mask, 114, 114, 114, 114, 0.0f);
    od.recordEdit("fixture", EditKind::Content);
    const size_t entries = od.history.entries().size();
    const float farBefore = maskAt(*od.document.layers[0].mask, 20, 20);  // 0.6 as stored
    std::string why;
    const bool began = healAt(od, Vec2{128.0f, 128.0f}, 16.0f, &why);
    const MaskTileStore& m = *od.document.layers[0].mask;
    std::printf("  [mask heal] speck %.4f, outside-dab corner %.4f\n",
                static_cast<double>(maskAt(m, 127, 127)), static_cast<double>(maskAt(m, 114, 114)));
    check(began && std::fabs(maskAt(m, 127, 127) - 0.6f) < 0.01f,
          "F Heal: a speck inside the dab is filled from its surroundings");
    check(maskAt(m, 114, 114) == 0.0f && maskAt(m, 20, 20) == farBefore,
          "F Heal: coverage outside the dab is unchanged, even inside its rectangle");
    check(began && why.empty(), "F Heal: needs no clone source on a mask");
    check(od.history.entries().size() == entries + 1 &&
              contains(od.history.entries().back().label, "mask") && contentUntouched(od),
          "F Heal: one history entry naming the mask; layer pixels untouched");
    // Guarded: a heal that recorded nothing would undo past the mask itself.
    const Document* undone =
        od.history.entries().size() == entries + 1 ? od.history.undo() : nullptr;
    if (undone != nullptr) od.document = *undone;
    check(undone != nullptr && od.document.layers[0].mask.has_value() &&
              maskAt(*od.document.layers[0].mask, 127, 127) == 0.0f,
          "F Heal: undo puts the speck back");
  }
  {  // Heal: a linear gradient across the dab is harmonic, so it survives
    OpenDocument od = freshMask("heal gradient");
    MaskTileStore& m = *od.document.layers[0].mask;
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) {
        const PixelCoord at{x, y};
        m.getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at),
                                                     static_cast<float>(x) / 255.0f);
      }
    const MaskTileStore before = m;
    std::string why;
    const bool began = healAt(od, Vec2{128.0f, 128.0f}, 20.0f, &why);
    float worst = 0.0f;
    for (int32_t y = 106; y <= 150; ++y)
      for (int32_t x = 106; x <= 150; ++x)
        worst = std::max(worst, std::fabs(maskAt(*od.document.layers[0].mask, x, y) -
                                          maskAt(before, x, y)));
    std::printf("  [mask heal] gradient worst deviation %.6f\n", static_cast<double>(worst));
    check(began && worst < 0.005f, "F Heal: a linear gradient across the dab is preserved");
  }
  {  // Heal: the soft tip blends the fill over the original, one dab
    MaskTileStore store;
    maskRect(store, 0, 0, 255, 255, 0.6f);
    maskRect(store, 118, 118, 138, 138, 0.0f);
    MaskHealStroke h;
    h.begin(1.0f);
    h.healDab(store, tipOf(20.0f, 0.0f), Vec2{128.5f, 128.5f}, kW, kH, nullptr, nullptr);
    h.end();
    const float centreV = maskAt(store, 128, 128);
    const float edgeV = maskAt(store, 137, 128);
    std::printf("  [mask heal] soft tip: centre %.4f, 9 px out %.4f\n",
                static_cast<double>(centreV), static_cast<double>(edgeV));
    check(centreV > 0.55f, "F Heal: at the soft tip's centre the fill replaces the hole");
    check(edgeV > 0.02f && edgeV < 0.55f,
          "F Heal: toward the soft edge the fill is blended over the original");
  }
  {  // Heal: disabled and absent masks behave as for the other mask tools
    OpenDocument od = freshMask("heal disabled");
    maskRect(*od.document.layers[0].mask, 0, 0, 255, 255, 0.6f);
    maskRect(*od.document.layers[0].mask, 126, 126, 129, 129, 0.0f);
    od.document.layers[0].maskEnabled = false;
    std::string why;
    const bool began = healAt(od, Vec2{128.0f, 128.0f}, 16.0f, &why);
    check(began && std::fabs(maskAt(*od.document.layers[0].mask, 127, 127) - 0.6f) < 0.01f &&
              !od.document.layers[0].maskEnabled && contentUntouched(od),
          "F Heal on a disabled mask: heals its texels and leaves it disabled");

    OpenDocument absent = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "heal no mask");
    const Layer& al = absent.document.layers[0];
    check(strokeRouteFor(Tool::Heal, &al, resolveLayerEditTarget(true, &al)) ==
              strokeRouteFor(Tool::Heal, &al),
          "F Heal with no mask: resolves to the content route");
    std::string refusal;
    check(!healAt(absent, Vec2{128.0f, 128.0f}, 16.0f, &refusal) && !refusal.empty() &&
              !absent.document.layers[0].mask.has_value(),
          "F Heal with no mask: takes the pixel heal's source refusal, creates no mask");
  }

  std::printf("[selftest] mask controls %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
