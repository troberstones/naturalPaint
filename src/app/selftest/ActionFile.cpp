#include "app/selftest/Support.hpp"

#include <cstring>

#include "io/ActionFile.hpp"
#include "io/Json.hpp"
#include "ops/Action.hpp"

namespace np {

// io/ActionFile + ops/Action (docs/automation-plan.md step 4) -- the action
// model and its `.npaction` text form.
//
// **What this section is guarding, and what it deliberately is not.** It never
// applies a command to a document: `app/selftest/Command.cpp` already owns
// that, and duplicating it here would mean two sections that fail together and
// tell you nothing about which layer broke. What is only visible here is the
// FILE: whether the version gate holds, whether the four refusals refuse,
// whether a re-save diffs as one line, and whether a parameter's double comes
// back bit for bit.
//
// The central fixture is **hand-typed**, and that is the point of it. Every
// other assertion below could pass with a decoder that agrees with its own
// encoder about something wrong -- a key order, a number format, a nesting
// level -- because both halves came from the same file. io/OpSerial's section
// states the rule and io/NpaintFile's 52-byte PSD fixture established it: a
// fixture that shares the encoder's assumptions cannot catch the encoder being
// wrong. So the fixture below is typed at a keyboard in a shape this build's
// writer never produces: four-space indent, `"name"` before `"npaction"`,
// parameters ahead of `"cmd"` in one step, `5.0e-1` where the writer would
// emit `0.5`, and the version key LAST.
//
// Headless, GPU-free. The library section writes and removes a
// selftest_actions/ scratch directory under $NP_ACTION_DIR, so nothing here
// can reach ~/Library/Application Support/naturalPaint.
bool runActionFileTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };
  // An action a refusal must leave exactly as it found it. Every refusal below
  // is handed one of these and re-checked against it: `readAction()` promises
  // it does not half-fill its output, and a half-filled action from a version
  // this build cannot read is precisely how a wrong file gets executed.
  const auto untouchedGuard = [] {
    Action a;
    a.name = "left alone";
    a.steps.push_back(Command{"flatten_image", JsonValue::object()});
    return a;
  };
  const auto isUntouched = [](const Action& a) {
    return a.name == "left alone" && a.steps.size() == 1 && a.steps[0].id == "flatten_image";
  };

  // =======================================================================
  // 1. The hand-typed fixture
  // =======================================================================
  //
  // docs/automation-plan.md §5's own example, retyped, using only command ids
  // this build registers. Nothing produced this text but a keyboard.
  static const char* const kHandTyped = R"NPACTION({
    "name"  : "Height prep 512",
    "steps" : [
        { "layer": "Base", "cmd": "select_layer" },
        { "cmd" : "flatten_image" },
        { "cmd":"filter_gaussian_blur",  "sigma":5.0e-1 },
        { "cmd": "set_layer_blend", "mode": "subtract", "layer": "Background" },
        { "cmd": "adjust_threshold", "threshold": 0.5, "amount": 1 },
        { "cmd": "image_size", "width": 512, "height": 512, "kernel": "catmull_rom" }
    ],
    "npaction" : 1
})NPACTION";

  {
    // The fixture's own shape is asserted before anything is asserted about
    // what it decodes to. Without this, someone tidying the file into the
    // writer's own layout would quietly delete the property the next two
    // checks exist to prove -- and both would still pass.
    const std::string text = kHandTyped;
    check(text.find("\"npaction\"") > text.find("\"steps\"") &&
              text.find("\"layer\": \"Base\", \"cmd\"") != std::string::npos,
          "fixture: it really is in a shape no writer here produces");
  }

  Action fixture;
  std::string err = "not cleared";
  const bool fixtureRead = readAction(kHandTyped, "hand-typed.npaction", &fixture, &err);
  check(fixtureRead && err.empty(), "hand-typed: the fixture reads, with no error left behind");
  check(fixture.name == "Height prep 512" && fixture.steps.size() == 6,
        "hand-typed: the name and all six steps");
  check(fixtureRead && fixture.steps.size() == 6 && fixture.steps[0].id == "select_layer" &&
            fixture.steps[1].id == "flatten_image" &&
            fixture.steps[2].id == "filter_gaussian_blur" &&
            fixture.steps[3].id == "set_layer_blend" && fixture.steps[4].id == "adjust_threshold" &&
            fixture.steps[5].id == "image_size",
        "hand-typed: the six command ids, in the order typed");
  check(fixtureRead && fixture.steps.size() == 6 && fixture.steps[0].params.size() == 1 &&
            fixture.steps[0].params.find("cmd") == nullptr &&
            fixture.steps[0].params.stringOr("layer", "") == "Base",
        "hand-typed: \"cmd\" becomes the id and is not left as a parameter");
  check(fixtureRead && fixture.steps.size() == 6 && fixture.steps[1].params.isObject() &&
            fixture.steps[1].params.size() == 0,
        "hand-typed: a step with no parameters is an empty object, not null");
  check(fixtureRead && fixture.steps.size() == 6 &&
            fixture.steps[2].params.numberOr("sigma", -1.0) == 0.5,
        "hand-typed: 5.0e-1 decodes to 0.5, exponent form and all");
  check(fixtureRead && fixture.steps.size() == 6 &&
            fixture.steps[4].params.numberOr("amount", -1.0) == 1.0 &&
            fixture.steps[5].params.numberOr("width", 0.0) == 512.0 &&
            fixture.steps[5].params.stringOr("kernel", "") == "catmull_rom",
        "hand-typed: a bare 1 is a number, and strings come through whole");
  {
    // The step whose parameters were typed in the opposite order to the
    // writer's. This is PRD P5's diffability property at the level a re-save
    // can destroy: if the model reordered, the next save would rewrite lines
    // nobody touched.
    const bool okOrder = fixtureRead && fixture.steps.size() == 6 &&
                         fixture.steps[3].params.members().size() == 2 &&
                         fixture.steps[3].params.members()[0].first == "mode" &&
                         fixture.steps[3].params.members()[1].first == "layer";
    check(okOrder, "hand-typed: parameters keep the order they were typed in");
  }

  // =======================================================================
  // 2. Round trip
  // =======================================================================
  {
    std::string first;
    std::string writeErr = "not cleared";
    const bool wrote = writeAction(fixture, &first, &writeErr);
    check(wrote && writeErr.empty() && !first.empty(), "write: the fixture's action serialises");
    check(wrote && first.rfind("{\n  \"npaction\": 1,\n  \"name\":", 0) == 0,
          "write: the version is the first line a person sees");
    check(wrote && !first.empty() && first.back() == '\n', "write: the file ends in a newline");

    Action back;
    std::string readErr;
    const bool reread = readAction(first, "roundtrip.npaction", &back, &readErr);
    std::string second;
    const bool rewrote = reread && writeAction(back, &second, nullptr);
    check(rewrote && first == second,
          "round trip: write -> read -> write is byte-identical");
    check(reread && back.steps.size() == fixture.steps.size() &&
              back.steps[5].params.stringOr("kernel", "") == "catmull_rom",
          "round trip: the model survives the trip through text");
  }
  {
    // Bit patterns, not values. io/Json's writer drops to %.17g only when
    // %.9g does not read back identical, so the interesting cases are the
    // ones that need it -- plus negative zero, whose sign is lost by every
    // "is it integral?" fast path that does not check for it.
    Action a;
    a.name = "floats";
    const double thirds = 1.0 / 3.0;
    const double tiny = 1e-300;
    const double negZero = -0.0;
    JsonValue p = JsonValue::object();
    p.set("sigma", JsonValue::number(thirds));
    p.set("tiny", JsonValue::number(tiny));
    p.set("negzero", JsonValue::number(negZero));
    a.steps.push_back(Command{"filter_gaussian_blur", std::move(p)});

    std::string text;
    Action back;
    const bool trip = writeAction(a, &text, nullptr) && readAction(text, "floats", &back, nullptr);
    bool identical = trip && back.steps.size() == 1;
    if (identical) {
      const double s = back.steps[0].params.numberOr("sigma", 0.0);
      const double t = back.steps[0].params.numberOr("tiny", 0.0);
      const double z = back.steps[0].params.numberOr("negzero", 1.0);
      identical = std::memcmp(&s, &thirds, sizeof(double)) == 0 &&
                  std::memcmp(&t, &tiny, sizeof(double)) == 0 &&
                  std::memcmp(&z, &negZero, sizeof(double)) == 0;
    }
    check(identical, "round trip: a parameter's double returns as the identical bits");
  }
  {
    // A layer name is user text and reaches the file through the escaper. A
    // newline written raw is io/ExportAs' old bug: a file that will not read
    // back at all.
    Action a;
    a.name = "quotes \" and \\ backslash";
    JsonValue p = JsonValue::object();
    p.set("layer", JsonValue::string("two\nlines \"quoted\"\twith a tab"));
    a.steps.push_back(Command{"select_layer", std::move(p)});
    std::string text;
    Action back;
    const bool trip = writeAction(a, &text, nullptr) && readAction(text, "escapes", &back, nullptr);
    check(trip && back.name == a.name && back.steps.size() == 1 &&
              back.steps[0].params.stringOr("layer", "") == "two\nlines \"quoted\"\twith a tab",
          "round trip: control characters and quotes in names survive");
  }

  // =======================================================================
  // 3. The refusals
  // =======================================================================
  //
  // Each of these is an absence-claim, which docs/automation-plan.md §8 calls
  // out as the shape that most often passes for the wrong reason -- so each
  // asserts three things, not one: that it refused, that the sentence names
  // the file and the reason, and that the caller's action is untouched.
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read = readAction(R"({ "name": "x", "steps": [] })", "no-version.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "no-version.npaction") &&
              contains(why, "npaction") && contains(why, "NOT read as version 1"),
          "refuse: no \"npaction\" key -- and not read as version 1 anyway");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read =
        readAction(R"({ "npaction": 2, "name": "x", "steps": [] })", "future.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "future.npaction") &&
              contains(why, "version 2") && contains(why, "version 1"),
          "refuse: a version this build does not read, named in the sentence");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read = readAction(R"({ "npaction": "1", "steps": [] })", "strver.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "strver.npaction") && contains(why, "a string"),
          "refuse: a version key holding a string, not a number");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read = readAction(R"({ "npaction": 1, "steps": 3 })", "notarray.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "notarray.npaction") &&
              contains(why, "array") && contains(why, "every file in a batch"),
          "refuse: \"steps\" that is not an array, rather than an empty action");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read = readAction(R"({ "npaction": 1, "name": "x" })", "nosteps.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "nosteps.npaction") && contains(why, "steps"),
          "refuse: a missing \"steps\" key (an explicit [] is accepted)");
  }
  {
    Action out;
    const bool read = readAction(R"({ "npaction": 1, "steps": [] })", "empty.npaction", &out, nullptr);
    check(read && out.steps.empty(), "accept: \"steps\": [] -- an empty list someone typed");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read = readAction(
        R"({ "npaction": 1, "steps": [ { "cmd": "flatten_image" }, { "cmd": "invert_gravity" } ] })",
        "newer.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "step 2") && contains(why, "invert_gravity") &&
              contains(why, "newer.npaction"),
          "refuse: an unknown command id, at LOAD and by name");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read =
        readAction(R"({ "npaction": 1, "steps": [ 7 ] })", "stepnum.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "step 1") && contains(why, "a number"),
          "refuse: a step that is not an object");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read =
        readAction(R"({ "npaction": 1, "steps": [ { "sigma": 4 } ] })", "nocmd.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "step 1") && contains(why, "cmd"),
          "refuse: a step with no \"cmd\" key naming a command");
  }
  {
    Action out = untouchedGuard();
    std::string why;
    const bool read = readAction("{ \"npaction\": 1, \"steps\": [ ", "trunc.npaction", &out, &why);
    check(!read && isUntouched(out) && contains(why, "trunc.npaction"),
          "refuse: a truncated file, naming the file");
  }
  {
    // The flat form's one reserved key (io/ActionFile.hpp §3). Without this
    // refusal the writer emits "cmd" twice, io/Json keeps the first duplicate,
    // and the parameter disappears on the round trip in silence.
    Action a;
    a.name = "collide";
    JsonValue p = JsonValue::object();
    p.set("cmd", JsonValue::string("this is a parameter, not the id"));
    a.steps.push_back(Command{"flatten_image", std::move(p)});
    std::string text = "untouched";
    std::string why;
    const bool wrote = writeAction(a, &text, &why);
    check(!wrote && contains(why, "step 1") && contains(why, "cmd") && contains(why, "reserved"),
          "refuse: a parameter called \"cmd\", rather than losing it silently");
  }

  // =======================================================================
  // 4. Names, ids and paths
  // =======================================================================
  {
    check(actionFileNameFor("Height prep 512") == "Height prep 512.npaction",
          "name: an ordinary action name is its own file name");
    const std::string escaped = actionFileNameFor("../../etc/passwd");
    check(!escaped.empty() && escaped.find('/') == std::string::npos && escaped[0] != '.' &&
              escaped == "_.._etc_passwd.npaction",
          "name: a name cannot walk out of the actions directory");
    check(actionFileNameFor("").empty() && actionFileNameFor("...").empty() &&
              actionFileNameFor("   ").empty(),
          "name: a name with nothing usable in it yields no file name");
    check(actionFileNameFor(std::string(300, 'a')).size() == 96 + std::strlen(".npaction"),
          "name: a very long name is capped inside every filesystem's limit");
  }
  {
    // The id table is keyed to the same enumerators as pointOpKindName(), and
    // must not BE pointOpKindName(): that returns display text ("Channel
    // Mixer"), which is UI copy and gets reworded. Nine kinds, nine distinct
    // ids, none of them a display string.
    const PointOpKind kinds[] = {PointOpKind::Levels,       PointOpKind::Curves,
                                 PointOpKind::Exposure,     PointOpKind::Saturation,
                                 PointOpKind::Grayscale,    PointOpKind::ChannelMixer,
                                 PointOpKind::Invert,       PointOpKind::Posterize,
                                 PointOpKind::Threshold};
    std::vector<std::string> ids;
    bool allNamed = true;
    for (const PointOpKind k : kinds) {
      const char* id = pointOpKindId(k);
      if (id == nullptr || *id == '\0') { allNamed = false; break; }
      for (const char* c = id; *c != '\0'; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || *c == '_')) allNamed = false;
      }
      ids.emplace_back(id);
    }
    std::vector<std::string> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    const bool distinct = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    check(allNamed && ids.size() == 9 && distinct,
          "ids: all nine point-op kinds have a distinct lower_snake_case id");
    check(ids.size() == 9 && ids[5] == "channel_mixer" &&
              std::string(pointOpKindName(PointOpKind::ChannelMixer)) == "Channel Mixer",
          "ids: the file id is not the display label it sits beside");
  }

  // =======================================================================
  // 5. actionFromLayerOps() -- PRD P6's converter
  // =======================================================================
  {
    Layer layer;
    layer.name = "Base";
    Op levels;
    levels.opClass = OpClass::PointA;
    levels.pointKind = PointOpKind::Levels;
    Op exposure;
    exposure.opClass = OpClass::PointA;
    exposure.pointKind = PointOpKind::Exposure;
    exposure.enabled = false;
    layer.ops.add(levels);
    layer.ops.add(exposure);

    const LayerOpsToActionResult r = actionFromLayerOps(layer, "Base grade");
    check(r.ok && r.error.empty() && r.action.name == "Base grade" && r.action.steps.size() == 3,
          "convert: a two-op layer becomes select_layer plus two steps");
    check(r.ok && r.action.steps.size() == 3 && r.action.steps[0].id == "select_layer" &&
              r.action.steps[0].params.stringOr("layer", "") == "Base",
          "convert: the target is named once, in a select_layer step");
    check(r.ok && r.action.steps.size() == 3 &&
              r.action.steps[1].params.stringOr("kind", "") == "levels" &&
              r.action.steps[1].params.boolOr("enabled", false) &&
              r.action.steps[2].params.stringOr("kind", "") == "exposure" &&
              !r.action.steps[2].params.boolOr("enabled", true),
          "convert: each op carries its kind id and its enabled flag");
    check(r.ok && r.action.steps.size() == 3 &&
              r.action.steps[1].params.find("layer") == nullptr &&
              r.action.steps[2].params.find("layer") == nullptr,
          "convert: the op steps do not repeat the layer name");
    check(r.ok && !r.warnings.empty(),
          "convert: the converter reports what it could not carry");

    // The tripwire, and it is written to survive the thing it is waiting for.
    // `add_layer_op` is registered by app/CommandsOpStack, which is empty on
    // this branch, so today the converter's output is refused at load -- which
    // is CORRECT, and is the load-time refusal proven against a real case
    // rather than a synthetic id. The moment that registration lands, this
    // flips to the other arm and demands that the converter carry every
    // parameter the registration advertises. It therefore cannot go stale
    // green: naturalpaint-audit-entries-spoil.md's failure mode is an
    // absence-claim that nobody re-measures, and this one re-measures itself.
    std::string text;
    const bool wrote = writeAction(r.action, &text, nullptr);
    Action back;
    std::string why;
    const bool loaded = readAction(text, "converted.npaction", &back, &why);
    const CommandSpec* opStep = findCommand("add_layer_op");
    if (opStep == nullptr) {
      check(wrote && !loaded && contains(why, "add_layer_op") && contains(why, "step 2"),
            "convert: with no add_layer_op registered, the file is refused at load");
    } else {
      bool carriesAll = wrote && loaded && back.steps.size() == 3;
      for (const std::string& key : opStep->paramNames) {
        // "layer" excepted: the converter names the target once, in the
        // select_layer step, and CommandSupport reads an absent "layer" as
        // "whatever select_layer last chose".
        if (key == "layer") continue;
        if (!carriesAll || back.steps[1].params.find(key) == nullptr) carriesAll = false;
      }
      check(carriesAll,
            "convert: add_layer_op exists now -- every parameter it declares is carried");
    }
  }
  {
    Layer layer;
    layer.name = "Base";
    Op spatial;
    spatial.opClass = OpClass::SpatialB;
    layer.ops.add(spatial);
    const LayerOpsToActionResult r = actionFromLayerOps(layer, "spatial");
    check(!r.ok && r.action.steps.empty() && contains(r.error, "entry 1") &&
              contains(r.error, "Base") && contains(r.error, "spatial op"),
          "convert: a non-point op refuses by name, having emitted nothing");
  }
  {
    Layer layer;
    Op unknown;
    unknown.opClass = OpClass::Unknown;
    unknown.unrecognised = {1, 2, 3, 4};
    layer.name = "Base";
    layer.ops.add(unknown);
    const LayerOpsToActionResult r = actionFromLayerOps(layer, "unknown");
    check(!r.ok && r.action.steps.empty() && contains(r.error, "unrecognised op"),
          "convert: an op a newer build wrote refuses the conversion");
  }
  {
    Layer layer;  // no name
    Op levels;
    layer.ops.add(levels);
    const LayerOpsToActionResult r = actionFromLayerOps(layer, "nameless");
    check(!r.ok && r.action.steps.empty() && contains(r.error, "no name"),
          "convert: an unnamed layer cannot be targeted, so it is refused");
  }
  {
    Layer layer;
    layer.name = "Flat";
    const LayerOpsToActionResult r = actionFromLayerOps(layer, "ungraded");
    check(r.ok && r.action.steps.size() == 1 && r.action.steps[0].id == "select_layer" &&
              r.warnings.empty(),
          "convert: an ungraded layer converts to a bare select_layer, no warnings");
  }

  // =======================================================================
  // 6. The library directory
  // =======================================================================
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string root = "selftest_actions";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const char* previousDir = std::getenv("NP_ACTION_DIR");
  const std::string savedDir = previousDir ? previousDir : "";
  setenv("NP_ACTION_DIR", root.c_str(), 1);

  check(actionsDirectoryPath() == root,
        "library: $NP_ACTION_DIR overrides the path, so the real one is untouched");
  {
    Action blur;
    blur.name = "Zebra blur";
    JsonValue p = JsonValue::object();
    p.set("sigma", JsonValue::number(4.0));
    blur.steps.push_back(Command{"filter_gaussian_blur", std::move(p)});
    Action flat;
    flat.name = "Alpha flatten";
    flat.steps.push_back(Command{"flatten_image", JsonValue::object()});

    std::string whyA;
    std::string whyB;
    const std::string pathA = actionsDirectoryPath() + "/" + actionFileNameFor(blur.name);
    const std::string pathB = actionsDirectoryPath() + "/" + actionFileNameFor(flat.name);
    const bool savedA = saveActionToFile(pathA, blur, &whyA);
    const bool savedB = saveActionToFile(pathB, flat, &whyB);
    check(savedA && savedB && whyA.empty() && whyB.empty(), "library: two actions save");

    // A file that is not an action, and a directory that looks like one:
    // neither may appear in a listing.
    { std::ofstream(root + "/notes.txt") << "not an action\n"; }
    fs::create_directories(root + "/decoy.npaction", ec);

    const std::vector<std::string> listed = listActionFiles(actionsDirectoryPath());
    check(listed.size() == 2 && listed[0] == pathB && listed[1] == pathA,
          "library: only .npaction regular files, sorted by name");

    Action loaded;
    std::string loadWhy;
    const bool didLoad = loadActionFromFile(pathA, &loaded, &loadWhy);
    check(didLoad && loadWhy.empty() && loaded.name == "Zebra blur" && loaded.steps.size() == 1 &&
              loaded.steps[0].params.numberOr("sigma", 0.0) == 4.0,
          "library: an action saved and loaded back is the same action");

    Action missing = untouchedGuard();
    std::string missingWhy;
    const bool loadedMissing =
        loadActionFromFile(root + "/nothing-here.npaction", &missing, &missingWhy);
    check(!loadedMissing && isUntouched(missing) && contains(missingWhy, "nothing-here.npaction"),
          "library: loading a file that is not there refuses, naming the path");
  }
  check(listActionFiles(root + "/never-created").empty(),
        "library: a library that does not exist yet lists as empty, not as an error");

  if (savedDir.empty()) unsetenv("NP_ACTION_DIR");
  else setenv("NP_ACTION_DIR", savedDir.c_str(), 1);
  fs::remove_all(root, ec);

  return ok;
}

}  // namespace np
