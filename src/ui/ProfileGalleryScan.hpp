#pragma once

namespace np {

// --profile-gallery-scan [count width height] : headless benchmark of
// scanDocumentGallery() with and without its thumbnail cache (ui/DocumentGallery.hpp).
//
// Writes `count` gradient-filled `width` x `height` documents into a scratch
// directory under the temp dir (never the real Documents or the real cache),
// then times three scans of it: no cache, a cold cache (decode + write), and a
// warm cache. Exits non-zero if the warm scan decoded anything or its tiles
// differ from the cold scan's. Built to be run on a device, where the decode
// cost it measures is the one a launch pays.
int runProfileGalleryScan(int count, int width, int height);

}  // namespace np
