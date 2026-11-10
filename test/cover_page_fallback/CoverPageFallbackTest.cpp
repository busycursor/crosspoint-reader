#include "CoverPageFallback.h"

#include <FsHelpers.h>
#include <gtest/gtest.h>

using cover_fallback::isBlankPage;
using cover_fallback::standInPage;

namespace {
std::string page(const std::string& body) {
  return "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Cover</title>"
         "<link href=\"a.css\" rel=\"stylesheet\"/></head><body>" +
         body + "</body></html>";
}

// The parser resolves src exactly like this (ChapterHtmlSlimParser).
std::string resolve(const std::string& contentBase, const std::string& html) {
  const size_t a = html.find("src=\"") + 5;
  std::string src = html.substr(a, html.find('"', a) - a);
  for (size_t p; (p = src.find("&amp;")) != std::string::npos;) src.replace(p, 5, "&");
  return FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + src));
}
}  // namespace

TEST(CoverPageFallback, StrippedCoverPagesAreBlank) {
  // The three real cases this exists for.
  EXPECT_TRUE(isBlankPage(page("<p class=\"cover\"></p>")));
  EXPECT_TRUE(isBlankPage(page("<figure class=\"imagefp\"></figure>")));
  EXPECT_TRUE(isBlankPage(page("\n<div class=\"ipadfp\"></div>\n")));
  EXPECT_TRUE(isBlankPage(page("<div>&nbsp;</div>")));
}

TEST(CoverPageFallback, PagesWithContentAreNotBlank) {
  EXPECT_FALSE(isBlankPage(page("<img src=\"cover.jpg\"/>")));
  EXPECT_FALSE(isBlankPage(page("<svg><image xlink:href=\"c.jpg\"/></svg>")));
  EXPECT_FALSE(isBlankPage(page("<IMG SRC=\"c.jpg\">")));
  EXPECT_FALSE(isBlankPage(page("<p>Chapter One</p>")));
  EXPECT_FALSE(isBlankPage(page("<p>&amp;</p>")));
  EXPECT_FALSE(isBlankPage("no body at all"));
  EXPECT_FALSE(isBlankPage(""));
}

TEST(CoverPageFallback, ImgPrefixedNamesAreNotImages) {
  // <imagefp> is not <image>; a class name is not a tag.
  EXPECT_TRUE(isBlankPage(page("<imagefp></imagefp>")));
}

TEST(CoverPageFallback, LargePagesAreNeverBlank) {
  EXPECT_FALSE(isBlankPage(page(std::string(9000, ' ')), 8192));
}

TEST(CoverPageFallback, StandInResolvesToTheCover) {
  EXPECT_EQ(resolve("OEBPS/", standInPage("OEBPS/", "OEBPS/Images/cover.jpg")), "OEBPS/Images/cover.jpg");
  EXPECT_EQ(resolve("OEBPS/xhtml/", standInPage("OEBPS/xhtml/", "OEBPS/images/c.jpg")), "OEBPS/images/c.jpg");
  EXPECT_EQ(resolve("", standInPage("", "cover.jpeg")), "cover.jpeg");
  EXPECT_EQ(resolve("a/b/", standInPage("a/b/", "x/My Cover 100%.jpg")), "x/My Cover 100%.jpg");
}

TEST(CoverPageFallback, StandInIsNotBlank) {
  EXPECT_FALSE(isBlankPage(standInPage("OEBPS/", "OEBPS/Images/cover.jpg")));
}
