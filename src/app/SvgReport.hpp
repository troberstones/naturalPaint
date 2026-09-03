#pragma once

// app/SvgReport -- what actually survived importing an `.svg`.
//
// io/SvgImport.hpp's own header states the gap this closes, the same shape
// io/PsdImport.hpp names for app/PsdReport: every fixture `--selftest` runs
// through the importer was written by this track's own author, so passing
// proves the reader agrees with its own author's reading of the SVG spec,
// not with Inkscape or Illustrator. That gap closes when a real exporter's
// file is opened and someone compares the shape list this prints against
// the file open in the exporting application (or a browser).
//
// Prints every imported shape's geometry (subpath/anchor counts, bounds,
// fill rule), its fill and stroke (on/off, straight sRGB re-encoded for
// readability, alpha), and the full refusal list -- so "did this file
// survive?" is a diff against the source file's own layer/object list
// rather than an impression formed by opening the result and looking.
//
// Headless, GPU-free, read-only: it imports and prints, and writes no files.
namespace np {

// Imports `path` through io/SvgImport and prints the resulting shape list
// and every refusal. Returns 0 when the file imported (`SvgImportResult::ok`
// -- which, per io/SvgImport.hpp, is true even when shapes were refused;
// only a file that could not be parsed as SVG at all fails), 1 otherwise.
int runSvgReport(const char* path);

}  // namespace np
