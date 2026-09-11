#include "app/selftest/Support.hpp"

#include <filesystem>
#include <fstream>

#include "app/Command.hpp"
#include "app/CommandsRegions.hpp"
#include "app/Recorder.hpp"
#include "app/RegionTool.hpp"
#include "core/RegionOps.hpp"
#include "io/ExportRegions.hpp"
#include "io/RegionSerial.hpp"
#include "ops/DocumentTransform.hpp"

namespace np {

// core/Region + core/RegionOps + app/RegionTool + io/RegionSerial +
// io/ExportRegions -- PLAN.md gap-closing wave, track `region`. `Tool::Frame`
// and `Tool::Slice`, and the document-level region model they both need
// (docs/ui.md §4a: both tools named a concept that did not exist).
//
// Six sections, each pinned to the production line that would make it lie:
//
//   A. The model itself (core/RegionOps): add/delete/rename/move/resize,
//      unique naming across both kinds, and the empty-rectangle refusal.
//   B. `.npaint` persistence (io/RegionSerial + io/NpaintFile's `np:regions`):
//      an exact round trip, the "no regions -> no attribute" byte-identity
//      claim, and a payload this build cannot parse being refused whole
//      rather than partially decoded.
//   C. The geometry-edit hookup core/Region.hpp §4 argues and
//      ops/DocumentTransform.cpp implements: crop, canvas size, image size
//      and a quarter turn each keep a region's rectangle correct, or drop it
//      when it no longer fits.
//   D. The gesture (app/RegionTool): one history entry per completed
//      define/move/resize/delete, a click-without-a-drag creating nothing,
//      and undo restoring the region list exactly.
//   E. Export (io/ExportRegions): N files at the right pixel extents and
//      names, a partly-off-canvas region exporting the intersection, and one
//      wholly off-canvas exporting nothing.
//   F. Recordability (app/CommandsRegions.cpp): the five commands run
//      through `applyCommand()`, addressed by name, each a real
//      `core::History` entry; an unknown region name refused by name.
//
// Headless and GPU-free throughout -- nothing here needs a window, and
// app/RegionTool's own header says the UI half (the overlay, the options
// row, the two menu items and their dialog) is out of this suite's reach for
// the same reason every on-canvas gesture in ui/MacPaintUI.cpp is, which is
// why the decisions were lifted into a headless module in the first place.
bool runRegionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] region: the model, .npaint persistence, the geometry-edit hookup, the "
      "gesture, export, and recordability\n");

  // =========================================================================
  // A. The model (core/RegionOps)
  // =========================================================================
  std::printf("  -- A. add / delete / rename / move / resize, and unique naming --\n");
  {
    Document doc = Document::createBlank(200, 150, WorkingSpace{});

    // A degenerate rectangle is refused by name, on both axes independently.
    const LayerOpResult zeroW = addRegion(doc, RegionKind::Frame, 10, 10, 0, 40);
    const LayerOpResult zeroH = addRegion(doc, RegionKind::Frame, 10, 10, 40, 0);
    check(!zeroW.ok && !zeroH.ok && doc.regions.empty(),
          "A: a zero-width or zero-height rectangle is refused, nothing added");

    const LayerOpResult f1 = addRegion(doc, RegionKind::Frame, 0, 0, 50, 50);
    const LayerOpResult f2 = addRegion(doc, RegionKind::Frame, 60, 0, 50, 50);
    const LayerOpResult s1 = addRegion(doc, RegionKind::Slice, 0, 60, 30, 30);
    check(f1.ok && f2.ok && s1.ok && doc.regions.size() == 3,
          "A: three regions added, two Frames and a Slice");
    check(doc.regions[0].name == "Frame 1" && doc.regions[1].name == "Frame 2" &&
              doc.regions[2].name == "Slice 1",
          "A: default names are stem-plus-number, numbered per kind");
    check(doc.regions[0].id != 0 && doc.regions[0].id != doc.regions[1].id &&
              doc.regions[1].id != doc.regions[2].id,
          "A: every region gets a distinct, nonzero, stable id");

    // Names are unique ACROSS kinds, core/Region.hpp's own rule -- a Slice
    // named "Frame 1" would collide with the Frame that already has that name.
    const LayerOpResult collide = addRegion(doc, RegionKind::Slice, 10, 10, 20, 20, "Frame 1");
    check(collide.ok && collide.index < doc.regions.size() &&
              doc.regions[collide.index].name != "Frame 1" &&
              doc.regions[collide.index].name.rfind("Frame 1", 0) == 0,
          "A: a caller-supplied name colliding with another KIND is disambiguated too");

    // Renaming to the name a region already has is a no-op, not a self-collision
    // that grows a suffix.
    const size_t idx0 = 0;
    const LayerOpResult renameSelf = renameRegion(doc, idx0, "Frame 1");
    check(renameSelf.ok && doc.regions[idx0].name == "Frame 1",
          "A: renaming a region to its own current name does not suffix itself");

    // Renaming to a name already in use elsewhere is disambiguated the same
    // way a colliding add is -- and lands on "Frame 1 3", not "Frame 1 2",
    // because the `collide` add above already claimed "Frame 1 2".
    const LayerOpResult renameCollide = renameRegion(doc, 1, "Frame 1");
    check(renameCollide.ok && doc.regions[1].name == "Frame 1 3",
          "A: renaming onto an existing name lands on the smallest free \" N\" suffix");

    // Move and resize.
    const LayerOpResult moved = moveRegion(doc, 2, 5, 5);
    check(moved.ok && doc.regions[2].x == 5 && doc.regions[2].y == 5 &&
              doc.regions[2].width == 30 && doc.regions[2].height == 30,
          "A: move changes the origin and keeps the size");
    const LayerOpResult resized = resizeRegion(doc, 2, 5, 5, 12, 8);
    check(resized.ok && doc.regions[2].width == 12 && doc.regions[2].height == 8,
          "A: resize sets the whole rectangle");
    const LayerOpResult resizedToEmpty = resizeRegion(doc, 2, 5, 5, 0, 8);
    check(!resizedToEmpty.ok && doc.regions[2].width == 12,
          "A: resizing to an empty rectangle is refused, the prior rectangle kept");

    // Delete, and the id counter is never reused.
    const uint64_t deletedId = doc.regions[1].id;
    const uint64_t counterBefore = doc.nextRegionId;
    const LayerOpResult del = deleteRegion(doc, 1);
    check(del.ok && doc.regions.size() == 3, "A: delete removes exactly the named region");
    const LayerOpResult reAdd = addRegion(doc, RegionKind::Frame, 1, 1, 5, 5);
    check(reAdd.ok && doc.regions.back().id != deletedId &&
              doc.nextRegionId == counterBefore + 1,
          "A: a new region after a delete never reuses the deleted id");
  }

  // =========================================================================
  // B. `.npaint` persistence (io/RegionSerial, io/NpaintFile's np:regions)
  // =========================================================================
  std::printf("  -- B. io/RegionSerial's encoding, and the np:regions attribute --\n");
  {
    // B1. The wire format round-trips exactly, including a name that is not
    // ASCII-only (io/RegionSerial's u16-length-prefixed byte string, not a
    // C string).
    RegionCarrier carrier;
    carrier.nextRegionId = 42;
    Region a;
    a.id = 7;
    a.kind = RegionKind::Frame;
    a.name = "Cover";
    a.x = -10;
    a.y = 3;
    a.width = 400;
    a.height = 300;
    Region b;
    b.id = 9;
    b.kind = RegionKind::Slice;
    b.name = "caf\xc3\xa9 slice";  // "café slice", UTF-8
    b.x = 0;
    b.y = 0;
    b.width = 64;
    b.height = 64;
    carrier.regions = {a, b};

    const std::string wire = serializeRegions(carrier);
    check(wire.rfind(kRegionSerialPrefix, 0) == 0,
          "B: the serialised value begins with the version prefix");
    RegionCarrier back;
    std::string err;
    check(deserializeRegions(wire, &back, &err) && err.empty(),
          "B: the wire format decodes with no error");
    check(back == carrier, "B: and decodes to EXACTLY the carrier that was serialised");

    // B2. A payload this build does not recognise is refused whole, by name,
    // and decodes nothing -- never a partial list. Corrupt the kind byte of
    // the second region (byte 8 [nextRegionId] + 2 [count] + [8 id + 1 kind +
    // ... ] of the first record, then the second record's own id (8) puts us
    // at its kind byte).
    {
      const std::string prefix = kRegionSerialPrefix;
      std::string hexPart = wire.substr(prefix.size());
      // Byte offset of region B's kind byte: 8 (nextRegionId) + 2 (count) +
      // [8 id + 1 kind + 4 x + 4 y + 4 w + 4 h + 2+len name] for region A.
      const size_t aRecordBytes = 8 + 1 + 4 + 4 + 4 + 4 + 2 + a.name.size();
      const size_t kindByteOffset = 8 + 2 + aRecordBytes + 8;
      const size_t hexPos = kindByteOffset * 2;
      check(hexPos + 1 < hexPart.size(), "B: (setup) the corrupt offset lands inside the payload");
      hexPart[hexPos] = 'f';
      hexPart[hexPos + 1] = 'f';  // kind byte 0xFF: neither 0 (Frame) nor 1 (Slice)
      const std::string corrupted = prefix + hexPart;
      RegionCarrier corruptOut;
      std::string corruptErr;
      const bool decoded = deserializeRegions(corrupted, &corruptOut, &corruptErr);
      check(!decoded && !corruptErr.empty() && corruptOut.regions.empty(),
            "B: an unrecognised kind byte refuses the WHOLE payload, decoding nothing");
    }

    // B3. `.npaint` round trip: a document with regions saves and loads back
    // exactly, including the id counter; a document with NONE writes no
    // attribute at all, and its file is therefore byte-identical (capture
    // date masked) to one saved before this feature existed.
    namespace fs = std::filesystem;
    const fs::path scratch = fs::temp_directory_path() / "np-selftest-region";
    std::error_code fsErr;
    fs::remove_all(scratch, fsErr);
    fs::create_directories(scratch, fsErr);

    auto bytesWithoutCapDate = [](const std::string& path) -> std::vector<unsigned char> {
      std::ifstream in(path, std::ios::binary);
      std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
      static const std::string kNeedle = "capDate";
      for (size_t i = 0; i + kNeedle.size() <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, kNeedle.data(), kNeedle.size()) != 0) continue;
        for (size_t j = i; j < std::min(i + 47, bytes.size()); ++j) bytes[j] = 0;
      }
      return bytes;
    };

    Document plain = Document::createBlank(64, 64, WorkingSpace{});
    const std::string barePath = (scratch / "bare.npaint").string();
    const NpaintSaveResult bareSaved = saveNpaint(plain, barePath);
    check(bareSaved.ok, "B: a region-free document saves");

    Document withRegions = plain;
    addRegion(withRegions, RegionKind::Frame, 0, 0, 32, 32, "Cover");
    addRegion(withRegions, RegionKind::Slice, 32, 32, 32, 32, "Icon");
    const std::string withPath = (scratch / "with.npaint").string();
    const NpaintSaveResult withSaved = saveNpaint(withRegions, withPath);
    check(withSaved.ok, "B: a document with two regions saves");
    const NpaintLoadResult loadedBack = loadNpaint(withPath);
    check(loadedBack.ok && loadedBack.warnings.empty() &&
              loadedBack.document.regions.size() == 2,
          "B: and loads back clean, with both regions");
    check(loadedBack.ok && loadedBack.document.regions == withRegions.regions,
          "B: the regions round-trip EXACTLY -- ids, kinds, names, rectangles");
    check(loadedBack.ok && loadedBack.document.nextRegionId == withRegions.nextRegionId,
          "B: and so does the id counter, so a region added after reopening cannot reuse one");

    Document clearedAgain = withRegions;
    clearedAgain.regions.clear();
    const std::string clearedPath = (scratch / "cleared.npaint").string();
    saveNpaint(clearedAgain, clearedPath);
    const std::vector<unsigned char> bareBytes = bytesWithoutCapDate(barePath);
    const std::vector<unsigned char> clearedBytes = bytesWithoutCapDate(clearedPath);
    const std::vector<unsigned char> withBytes = bytesWithoutCapDate(withPath);
    check(!bareBytes.empty() && bareBytes == clearedBytes,
          "B: clearing every region gives a file BYTE-IDENTICAL to one that never had any -- "
          "np:regions is written only when there are regions");
    check(withBytes.size() > bareBytes.size() && withBytes != bareBytes,
          "B: and the file WITH regions really is bigger and really differs");
  }

  // =========================================================================
  // C. The geometry-edit hookup (ops/DocumentTransform.cpp, core/Region.hpp §4)
  // =========================================================================
  std::printf("  -- C. crop, canvas size, image size and rotate keep regions correct --\n");
  {
    // C1. Crop: a region translates by (-x, -y) and is intersected with the
    // new canvas; one that ends up empty is REMOVED.
    {
      Document doc = Document::createBlank(200, 150, WorkingSpace{});
      addRegion(doc, RegionKind::Frame, 10, 10, 50, 50, "Kept");    // survives, shifted
      addRegion(doc, RegionKind::Frame, 5, 5, 20, 20, "Clipped");   // partly clipped away
      addRegion(doc, RegionKind::Slice, 150, 100, 40, 40, "Gone");  // wholly outside after crop

      const DocumentTransformResult r = cropDocument(doc, 10, 10, 60, 60, nullptr);
      check(r.ok, "C: crop succeeds");
      check(doc.regions.size() == 2, "C: the wholly-outside region is dropped, the other two kept");
      const Region* kept = findRegionById(doc, doc.regions[0].id);
      check(kept != nullptr, "C: (setup) the surviving region is findable");
      bool foundKept = false, foundClipped = false;
      for (const Region& reg : doc.regions) {
        if (reg.name == "Kept") {
          foundKept = reg.x == 0 && reg.y == 0 && reg.width == 50 && reg.height == 50;
        } else if (reg.name == "Clipped") {
          // Origin (5,5) - (10,10) = (-5,-5), intersected with [0,60)x[0,60):
          // becomes (0,0), and the far corner (25,25)-(10,10)=(15,15) is
          // inside, so the clipped rect is [0,15)x[0,15).
          foundClipped = reg.x == 0 && reg.y == 0 && reg.width == 15 && reg.height == 15;
        }
      }
      check(foundKept, "C: a fully-inside region translates exactly by (-x, -y)");
      check(foundClipped, "C: a partly-outside region translates AND is clipped to the new canvas");
    }

    // C2. Image size: a region scales by the same ratio as every pixel store.
    {
      Document doc = Document::createBlank(100, 100, WorkingSpace{});
      addRegion(doc, RegionKind::Frame, 10, 20, 30, 40);
      DocumentTransformParams params;
      const DocumentTransformResult r = resizeDocumentImage(doc, 200, 50, params, nullptr);
      check(r.ok, "C: image size succeeds");
      check(doc.regions.size() == 1, "C: image size does not drop a region (it cannot move one off-canvas)");
      const Region& reg = doc.regions[0];
      // Scale factors: 200/100 = 2x horizontally, 50/100 = 0.5x vertically.
      check(reg.x == 20 && reg.width == 60, "C: image size scales x and width by the horizontal ratio");
      check(reg.y == 10 && reg.height == 20, "C: image size scales y and height by the vertical ratio");
    }

    // C3. Rotate 90 degrees: a region's corners map through the same matrix
    // as every layer, tightest exact box, no rounding margin -- checked
    // against an expectation computed independently here (map the four
    // corners with the same `mat3MapPoint()` the production code uses, then
    // floor/ceil by hand), which is what would catch the header's named
    // mistake of reusing `transformedRegion()`'s one-pixel-outset margin:
    // that margin would make every edge of the result one texel bigger than
    // what is computed below.
    {
      Document doc = Document::createBlank(100, 60, WorkingSpace{});
      addRegion(doc, RegionKind::Frame, 10, 5, 20, 10);
      const Mat3 rot = transformRotate90(1, 100, 60);
      const Point2 c0 = mat3MapPoint(rot, Point2{10.0f, 5.0f});
      const Point2 c1 = mat3MapPoint(rot, Point2{30.0f, 5.0f});
      const Point2 c2 = mat3MapPoint(rot, Point2{10.0f, 15.0f});
      const Point2 c3 = mat3MapPoint(rot, Point2{30.0f, 15.0f});
      const float minX = std::min({c0.x, c1.x, c2.x, c3.x});
      const float minY = std::min({c0.y, c1.y, c2.y, c3.y});
      const float maxX = std::max({c0.x, c1.x, c2.x, c3.x});
      const float maxY = std::max({c0.y, c1.y, c2.y, c3.y});
      const int32_t expectedX = static_cast<int32_t>(std::floor(minX));
      const int32_t expectedY = static_cast<int32_t>(std::floor(minY));
      const uint32_t expectedW =
          static_cast<uint32_t>(static_cast<int32_t>(std::ceil(maxX)) - expectedX);
      const uint32_t expectedH =
          static_cast<uint32_t>(static_cast<int32_t>(std::ceil(maxY)) - expectedY);

      DocumentTransformParams params;
      const DocumentTransformResult r = transformDocument(doc, rot, 60, 100, params, nullptr);
      check(r.ok, "C: a 90-degree rotate succeeds");
      check(doc.regions.size() == 1, "C: the region survives a rotate that keeps it on-canvas");
      const Region& reg = doc.regions[0];
      check(reg.x == expectedX && reg.y == expectedY && reg.width == expectedW &&
                reg.height == expectedH,
            "C: rotate maps the region to the TIGHTEST box containing its mapped corners -- no "
            "one-pixel rounding margin");
      // Rotating 90 degrees swaps which axis is which, so a 20x10 rectangle
      // becomes 10x20 -- the discriminating case a margin bug would still
      // pass by accident (12x22, off by one on both axes) but a transposed-
      // axes bug would not.
      check(expectedW == 10 && expectedH == 20,
            "C: (setup) a 20x10 rectangle rotated a quarter turn is 10x20, exactly");
    }
  }

  // =========================================================================
  // D. The gesture (app/RegionTool): one history entry per commit, undo
  // =========================================================================
  std::printf("  -- D. the gesture: one history entry per commit, and undo --\n");
  {
    OpenDocument od = makeBlankOpenDocument(200, 150, WorkingSpace{});
    RegionSession session;

    // D1. A click without a drag creates nothing, and costs no history entry.
    const size_t before1 = od.history.entries().size();
    regionBeginDefine(session, od.id, RegionKind::Frame, 20.0f, 20.0f);
    const CommandResult clickOnly =
        regionCommitDefine(session, od, RegionKind::Frame, 20.0f, 20.0f, false, false);
    check(!clickOnly.ok && od.document.regions.empty(),
          "D: a click without a drag creates no region");
    check(clickOnly.status.empty(),
          "D: and says nothing -- a click is not a mistake, so it is not a refusal to show");
    check(od.history.entries().size() == before1,
          "D: and therefore costs no history entry");

    // D2. A real drag commits ONE history entry and selects the new region.
    regionBeginDefine(session, od.id, RegionKind::Frame, 10.0f, 10.0f);
    const size_t before2 = od.history.entries().size();
    const CommandResult defined =
        regionCommitDefine(session, od, RegionKind::Frame, 60.0f, 40.0f, false, false);
    check(defined.ok && od.document.regions.size() == 1, "D: a real drag defines a region");
    check(od.history.entries().size() == before2 + 1,
          "D: defining a region is exactly ONE history entry");
    check(!od.document.regions.empty() && session.selectedId == od.document.regions.back().id,
          "D: and the new region is selected");

    // D3. Move: select, begin, commit -- one entry. A selection click (the
    // same begin, released where it started) is not an edit at all.
    const size_t before3 = od.history.entries().size();
    regionBeginMove(session, od.document, 30.0f, 20.0f);
    const CommandResult selectClick = regionCommitMove(session, od, 30.0f, 20.0f);
    check(!selectClick.ok && selectClick.status.empty() &&
              od.history.entries().size() == before3,
          "D: clicking a region to select it moves nothing and costs no history entry");
    regionBeginMove(session, od.document, 30.0f, 20.0f);
    const CommandResult moved = regionCommitMove(session, od, 35.0f, 25.0f);
    check(moved.ok, "D: a move commits");
    check(od.history.entries().size() == before3 + 1, "D: moving a region is exactly ONE history entry");
    check(!od.document.regions.empty() && od.document.regions[0].x == 15 &&
              od.document.regions[0].y == 15,
          "D: and lands where the pointer took it (10,10 grabbed at 30,20, released at 35,25)");

    // D4. Resize: begin on a corner handle, commit -- one entry. Degenerate
    // (dragged onto its own fixed opposite corner) is refused, BY NAME.
    const size_t before4 = od.history.entries().size();
    const Region& sel = *findRegionById(od.document, session.selectedId);
    const std::array<Point2, 4> handles = regionHandlePoints(sel);
    regionBeginResize(session, od.document, 2 /* bottom-right */);
    const CommandResult degenerate =
        regionCommitResize(session, od, handles[0].x, handles[0].y);  // onto the fixed TL
    check(!degenerate.ok && !degenerate.status.empty(),
          "D: resizing a region to zero size is refused, with a reason");
    check(od.history.entries().size() == before4,
          "D: a refused resize costs no history entry");
    regionBeginResize(session, od.document, 2);
    const CommandResult resized =
        regionCommitResize(session, od, handles[2].x + 10.0f, handles[2].y + 5.0f);
    check(resized.ok, "D: a real resize commits");
    check(od.history.entries().size() == before4 + 1,
          "D: a real resize is exactly ONE history entry");

    // D5. Delete: one entry, selection cleared. Deleting with nothing
    // selected is refused with no error text (not a mistake to surface).
    const size_t before5 = od.history.entries().size();
    const CommandResult deleted = regionDeleteSelected(session, od);
    check(deleted.ok && od.document.regions.empty() && session.selectedId == 0,
          "D: delete removes the selected region and clears the selection");
    check(od.history.entries().size() == before5 + 1, "D: delete is exactly ONE history entry");
    const CommandResult deleteAgain = regionDeleteSelected(session, od);
    check(!deleteAgain.ok && deleteAgain.status.empty(),
          "D: deleting with nothing selected does nothing and says nothing");

    // D6. Undo restores the region list exactly -- `core::HistoryEntry`
    // snapshotting the whole `Document`, core/Region.hpp §3's whole argument.
    if (const Document* prior = od.history.undo()) {
      check(prior->regions.size() == 1, "D: undoing the delete gives back the one region it removed");
    } else {
      check(false, "D: undo after the delete returns a prior snapshot");
    }
  }

  // =========================================================================
  // D'. The gesture RECORDS: every commit goes through applyCommand()
  // =========================================================================
  //
  // The canvas and the options row call exactly these functions, so arming
  // the process recorder around them is testing the real wiring
  // (app/selftest/CommandCallsites.cpp section A's argument). A commit that
  // reached `core::RegionOps` directly would pass every assertion in D above
  // -- the history entry is the same -- and record nothing here.
  std::printf("  -- D'. the gesture records: one step per commit, and a click records none --\n");
  {
    OpenDocument od = makeBlankOpenDocument(200, 150, WorkingSpace{});
    RegionSession session;
    Recorder& rec = sessionRecorder();
    rec.arm(od);
    regionBeginDefine(session, od.id, RegionKind::Slice, 20.0f, 20.0f);
    regionCommitDefine(session, od, RegionKind::Slice, 20.0f, 20.0f, false, false);  // a click
    regionBeginDefine(session, od.id, RegionKind::Slice, 20.0f, 30.0f);
    regionCommitDefine(session, od, RegionKind::Slice, 70.0f, 60.0f, false, false);
    regionBeginMove(session, od.document, 40.0f, 40.0f);
    regionCommitMove(session, od, 40.0f, 40.0f);  // a selection click
    regionBeginMove(session, od.document, 40.0f, 40.0f);
    regionCommitMove(session, od, 45.0f, 41.0f);
    regionBeginResize(session, od.document, 2);
    regionCommitResize(session, od, 80.0f, 90.0f);
    regionRenameSelected(session, od, "Hero");
    regionRenameSelected(session, od, "Hero");  // unchanged: not an edit
    regionDeleteSelected(session, od);
    const std::vector<Command> steps = rec.steps();
    rec.stop();
    const char* const expected[] = {"add_region", "move_region", "resize_region", "rename_region",
                                    "delete_region"};
    bool idsMatch = steps.size() == 5;
    for (size_t i = 0; idsMatch && i < 5; ++i) idsMatch = steps[i].id == expected[i];
    if (!idsMatch) {
      std::printf("     recorded %zu step(s):", steps.size());
      for (const Command& c : steps) std::printf(" %s", c.id.c_str());
      std::printf("\n");
    }
    check(idsMatch,
          "D': define, move, resize, rename, delete record as exactly those five steps, in order "
          "-- and the click, the selection click and the unchanged rename record nothing");
    check(steps.size() == 5 && steps[0].params.stringOr("kind", "") == "Slice" &&
              steps[0].params.numberOr("x", -1) == 20 && steps[0].params.numberOr("y", -1) == 30 &&
              steps[0].params.numberOr("rect_width", -1) == 50 &&
              steps[0].params.numberOr("rect_height", -1) == 30,
          "D': the add_region step carries the dragged rectangle, (20,30)+50x30, and its kind");
    check(steps.size() == 5 && steps[3].params.stringOr("region", "") == "Slice 1" &&
              steps[3].params.stringOr("new_name", "") == "Hero",
          "D': the rename step addresses the region by its old name");
    check(steps.size() == 5 && steps[4].params.stringOr("region", "") == "Hero",
          "D': and the delete after it by the new one");
  }

  // =========================================================================
  // E. Export (io/ExportRegions)
  // =========================================================================
  std::printf("  -- E0. the size the dialog promises is each region's, not the document's --\n");
  {
    // `validateRegionExport()` is what the Export Frames and Slices dialog
    // shows per file and refuses on. It used to be `validateExportRequest()`
    // against the DOCUMENT's extent, which printed "1024 x 1024" over a
    // 430x290 Frame and computed a resize for an image no file would be.
    // 50% is the discriminating setting: the document at 50% is 50x40, and
    // neither region's answer is that.
    Document doc = Document::createBlank(100, 80, WorkingSpace{});
    addRegion(doc, RegionKind::Frame, 0, 0, 40, 30, "Cover");     // wholly inside
    addRegion(doc, RegionKind::Slice, 80, 0, 40, 30, "Edge");     // half off the right edge
    addRegion(doc, RegionKind::Slice, 200, 200, 10, 10, "Gone");  // wholly outside
    ExportRequest half;
    half.format = ImageFormat::Png;
    half.resize.mode = ExportResizeMode::Percent;
    half.resize.percent = 50.0f;

    DocumentRegion rect;
    check(regionExportRect(doc, doc.regions[1], &rect) && rect.x == 80 && rect.y == 0 &&
              rect.width == 20 && rect.height == 30,
          "E0: a region half off the canvas exports its intersection, (80,0)+20x30");
    check(!regionExportRect(doc, doc.regions[2], &rect),
          "E0: and one wholly outside exports nothing");

    const ExportValidation cover = validateRegionExport(doc, doc.regions[0], half);
    const ExportValidation edge = validateRegionExport(doc, doc.regions[1], half);
    check(cover.ok && cover.outWidth == 20 && cover.outHeight == 15,
          "E0: a 40x30 Frame at 50% is promised as 20x15 -- not the document's 50x40");
    check(edge.ok && edge.outWidth == 10 && edge.outHeight == 15,
          "E0: and the half-off Slice as its 20x30 intersection at 50%, 10x15");
    const ExportValidation gone = validateRegionExport(doc, doc.regions[2], half);
    check(!gone.ok && gone.error.find("Gone") != std::string::npos,
          "E0: a region wholly off the canvas is refused, by its own name");
  }

  std::printf("  -- E. export: N files, the right pixel extents, the intersection rule --\n");
  {
    namespace fs = std::filesystem;
    const fs::path scratch = fs::temp_directory_path() / "np-selftest-region-export";
    std::error_code fsErr;
    fs::remove_all(scratch, fsErr);
    fs::create_directories(scratch, fsErr);

    Document doc = Document::createBlank(100, 80, WorkingSpace{});
    addRegion(doc, RegionKind::Frame, 0, 0, 40, 30, "Cover");     // wholly inside
    addRegion(doc, RegionKind::Slice, 80, 0, 40, 30, "Edge");     // half off the right edge
    addRegion(doc, RegionKind::Slice, 200, 200, 10, 10, "Gone");  // wholly outside

    ExportRegionsRequest request;
    request.outputDirectory = scratch.string();
    request.documentName = "doc";
    request.nameTemplate = "{name}";
    request.format.format = ImageFormat::Png;

    const ExportStatesReport plan = planRegionExport(doc, request);
    check(plan.ok, "E: the plan succeeds (a document without an output directory, or with no "
                  "regions, is exercised by io/ExportRegions' own header contract, not repeated "
                  "here)");
    check(plan.items.size() == 3, "E: the plan has one item per region, including the skipped one");

    const ExportStatesReport report = exportDocumentRegions(doc, request);
    check(report.ok, "E: the export run succeeds overall");
    check(report.written() == 2 && report.skipped() == 1,
          "E: two regions are written, the wholly-outside one is skipped");

    for (const ExportStateItem& item : report.items) {
      if (item.stateName == "Gone") {
        check(item.outcome == ExportItemOutcome::Skipped && !item.reason.empty(),
              "E: the wholly-outside region is Skipped, named, with a reason");
        continue;
      }
      check(item.outcome == ExportItemOutcome::Written && !item.path.empty(),
            ("E: '" + item.stateName + "' was written").c_str());
      if (item.outcome != ExportItemOutcome::Written) continue;

      std::ifstream in(item.path, std::ios::binary);
      const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
      std::string decErr;
      const DecodedImage decoded =
          decodeImageLinear(bytes.data(), bytes.size(), &decErr);
      check(decoded.valid(), ("E: '" + item.stateName + "' decodes as a valid image").c_str());
      if (item.stateName == "Cover") {
        check(decoded.width == 40 && decoded.height == 30,
              "E: a wholly-inside region exports its own exact extent (40x30)");
      } else if (item.stateName == "Edge") {
        // [80,120) x [0,30) intersected with the 100x80 canvas is [80,100) x
        // [0,30): 20x30, the intersection the header promises, not the
        // region's own 40x30.
        check(decoded.width == 20 && decoded.height == 30,
              "E: a partly-off-canvas region exports the INTERSECTION (20x30), not its own rect");
      }
    }
  }

  // =========================================================================
  // F. Recordability (app/CommandsRegions.cpp): the five commands, through
  // `applyCommand()`, addressed by name.
  // =========================================================================
  std::printf("  -- F. recordability: the five commands, through applyCommand(), by name --\n");
  {
    OpenDocument od = makeBlankOpenDocument(200, 150, WorkingSpace{});
    const size_t base = od.history.entries().size();

    JsonValue addParams = JsonValue::object();
    addParams.set("kind", JsonValue::string("Frame"));
    addParams.set("x", JsonValue::number(10));
    addParams.set("y", JsonValue::number(10));
    addParams.set("rect_width", JsonValue::number(40));
    addParams.set("rect_height", JsonValue::number(30));
    addParams.set("name", JsonValue::string("Cover"));
    const CommandResult added = applyCommand(od, Command{"add_region", addParams});
    check(added.ok && od.document.regions.size() == 1 && od.document.regions[0].name == "Cover",
          "F: add_region, through applyCommand(), creates the named region");
    check(od.history.entries().size() == base + 1,
          "F: and it is a real core::History entry, not a bypass of recordLayerEdit()");

    JsonValue moveParams = JsonValue::object();
    moveParams.set("region", JsonValue::string("Cover"));
    moveParams.set("x", JsonValue::number(5));
    moveParams.set("y", JsonValue::number(5));
    const CommandResult moved = applyCommand(od, Command{"move_region", moveParams});
    check(moved.ok && od.document.regions[0].x == 5 && od.document.regions[0].y == 5,
          "F: move_region, addressed by name, moves it");

    JsonValue resizeParams = JsonValue::object();
    resizeParams.set("region", JsonValue::string("Cover"));
    resizeParams.set("x", JsonValue::number(5));
    resizeParams.set("y", JsonValue::number(5));
    resizeParams.set("rect_width", JsonValue::number(60));
    resizeParams.set("rect_height", JsonValue::number(45));
    const CommandResult resized = applyCommand(od, Command{"resize_region", resizeParams});
    check(resized.ok && od.document.regions[0].width == 60 && od.document.regions[0].height == 45,
          "F: resize_region sets the whole rectangle");

    JsonValue renameParams = JsonValue::object();
    renameParams.set("region", JsonValue::string("Cover"));
    renameParams.set("new_name", JsonValue::string("Hero"));
    const CommandResult renamed = applyCommand(od, Command{"rename_region", renameParams});
    check(renamed.ok && od.document.regions[0].name == "Hero", "F: rename_region renames it");

    // An unknown name is refused BY NAME, on every one of the four commands
    // that address one -- not a silent no-op.
    JsonValue badTarget = JsonValue::object();
    badTarget.set("region", JsonValue::string("No Such Region"));
    const CommandResult badMove = applyCommand(od, Command{"move_region", badTarget});
    check(!badMove.ok && badMove.status.find("No Such Region") != std::string::npos,
          "F: move_region on an unknown name is refused, naming the region it could not find");

    JsonValue deleteParams = JsonValue::object();
    deleteParams.set("region", JsonValue::string("Hero"));
    const size_t beforeDelete = od.history.entries().size();
    const CommandResult deleted = applyCommand(od, Command{"delete_region", deleteParams});
    check(deleted.ok && od.document.regions.empty(), "F: delete_region removes it");
    check(od.history.entries().size() == beforeDelete + 1,
          "F: and delete_region is its own history entry too");
  }

  // Every section above was sabotage-proven against its own production line
  // (uniqueRegionName()'s collision scan, the crop/transform region hooks in
  // ops/DocumentTransform.cpp, addRegion()'s empty-rectangle refusal,
  // io/RegionSerial's kind-byte and intersection rules, and
  // app/CommandsRegions.cpp's resolveRegionTarget() name lookup) in the
  // commit history of this wave's `region` track rather than left as a live
  // edit in this file -- see that track's report for which line went red
  // under each.

  return ok;
}

}  // namespace np
