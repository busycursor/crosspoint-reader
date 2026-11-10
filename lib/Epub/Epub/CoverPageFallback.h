#pragma once

#include <string>

// Some EPUBs (often re-processed retail files) have a cover page whose image
// reference was stripped: an empty <p>, <div> or <figure> while the package
// metadata still names the cover image. The reader then shows a blank first
// page. These helpers decide when that happened and build a one-image stand-in
// page, so the normal image path draws the metadata cover there instead.
namespace cover_fallback {

// True when the page draws nothing: no image-like element and no visible text
// in <body>. Pages larger than maxBytes are never treated as blank.
bool isBlankPage(const std::string& html, size_t maxBytes = 8192);

// A minimal XHTML page showing one image. `contentBase` is the page's folder
// inside the EPUB (e.g. "OEBPS/", or "" at the root) and `coverHref` the
// cover's path from the EPUB root; the src is made relative to contentBase.
std::string standInPage(const std::string& contentBase, const std::string& coverHref);

}  // namespace cover_fallback
