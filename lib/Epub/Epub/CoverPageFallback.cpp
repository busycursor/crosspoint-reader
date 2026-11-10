#include "CoverPageFallback.h"

#include <algorithm>
#include <cctype>

namespace cover_fallback {
namespace {

std::string lowered(const std::string& s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

bool hasElement(const std::string& lower, const char* name) {
  const std::string open = std::string("<") + name;
  for (size_t pos = lower.find(open); pos != std::string::npos; pos = lower.find(open, pos + 1)) {
    const char next = pos + open.size() < lower.size() ? lower[pos + open.size()] : '\0';
    if (next == '>' || next == '/' || std::isspace(static_cast<unsigned char>(next))) return true;
  }
  return false;
}

}  // namespace

bool isBlankPage(const std::string& html, const size_t maxBytes) {
  if (html.empty() || html.size() > maxBytes) return false;
  const std::string lower = lowered(html);
  for (const char* tag : {"img", "image", "svg", "object", "video", "canvas", "math"}) {
    if (hasElement(lower, tag)) return false;
  }
  size_t start = lower.find("<body");
  if (start == std::string::npos) return false;
  start = lower.find('>', start);
  if (start == std::string::npos) return false;
  const size_t end = lower.find("</body", start);
  const std::string body = html.substr(start + 1, (end == std::string::npos ? html.size() : end) - start - 1);

  // Visible text = characters outside tags, ignoring whitespace and &nbsp;-style entities.
  bool inTag = false;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (inTag) {
      if (c == '>') inTag = false;
      continue;
    }
    if (c == '<') {
      inTag = true;
    } else if (c == '&') {
      const size_t semi = body.find(';', i);
      if (semi == std::string::npos) return false;
      const std::string entity = lowered(body.substr(i + 1, semi - i - 1));
      if (entity != "nbsp" && entity != "#160" && entity != "#xa0" && entity != "#8203" && entity != "#x200b")
        return false;
      i = semi;
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::string standInPage(const std::string& contentBase, const std::string& coverHref) {
  std::string src;
  for (const char c : contentBase) {
    if (c == '/') src += "../";
  }
  for (const char c : coverHref) {
    if (c == '%') {
      src += "%25";
    } else if (c == '"') {
      src += "%22";
    } else if (c == '&') {
      src += "&amp;";
    } else {
      src += c;
    }
  }
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Cover</title></head>"
         "<body><img src=\"" +
         src + "\" alt=\"\"/></body></html>\n";
}

}  // namespace cover_fallback
