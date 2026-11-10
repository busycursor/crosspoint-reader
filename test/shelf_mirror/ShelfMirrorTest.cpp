#include <gtest/gtest.h>

#include <set>

#include "ShelfMirror.h"

using namespace shelf_mirror;

namespace {

RemoteBook book(const std::string& id) { return {id, "T", "A", "/opds/download/" + id + "/epub/"}; }

Record visible(const std::string& id) { return {id, "/C/" + id + ".epub", false}; }
Record hidden(const std::string& id) { return {id, "/C/" + id + ".epub", true}; }

ExistsFn existsIn(std::set<std::string> paths) {
  return [paths = std::move(paths)](const std::string& p) { return paths.count(p) != 0; };
}

// Every record's file is where the record says it is.
ExistsFn allPresent() {
  return [](const std::string&) { return true; };
}

using V = std::vector<std::string>;

V ids(const std::vector<RemoteBook>& books) {
  V out;
  for (const auto& b : books) out.push_back(b.id);
  return out;
}

V ids(const std::vector<Record>& recs) {
  V out;
  for (const auto& r : recs) out.push_back(r.id);
  return out;
}

}  // namespace

TEST(ShelfMirrorPaths, HiddenPathIsADotFolderBesideTheBook) {
  EXPECT_EQ(hiddenPath("/Calibre/A - B.epub"), "/Calibre/.cache/A - B.epub");
  EXPECT_EQ(currentPath(hidden("x")), "/C/.cache/x.epub");
  EXPECT_EQ(currentPath(visible("x")), "/C/x.epub");
}

TEST(ShelfMirrorPlan, EmptyManifestDownloadsEverything) {
  const Plan p = plan({book("a"), book("b")}, {}, true, allPresent());
  EXPECT_EQ(ids(p.download), (V{"a", "b"}));
  EXPECT_TRUE(p.hide.empty() && p.unhide.empty() && p.keep.empty());
}

TEST(ShelfMirrorPlan, VisibleBookOnShelfIsKept) {
  const Plan p = plan({book("a")}, {visible("a")}, true, allPresent());
  EXPECT_TRUE(p.download.empty());
  EXPECT_EQ(ids(p.keep), (V{"a"}));
}

TEST(ShelfMirrorPlan, BookOffShelfIsHiddenNotDeleted) {
  const Plan p = plan({book("a")}, {visible("a"), visible("gone")}, true, allPresent());
  EXPECT_EQ(ids(p.hide), (V{"gone"}));
  EXPECT_EQ(ids(p.keep), (V{"a"}));
}

TEST(ShelfMirrorPlan, HiddenBookBackOnShelfIsUnhiddenNotDownloaded) {
  const Plan p = plan({book("a")}, {hidden("a")}, true, existsIn({"/C/.cache/a.epub"}));
  EXPECT_EQ(ids(p.unhide), (V{"a"}));
  EXPECT_TRUE(p.download.empty());
}

TEST(ShelfMirrorPlan, HiddenBookOffShelfStaysHidden) {
  const Plan p = plan({book("a")}, {visible("a"), hidden("b")}, true, allPresent());
  EXPECT_EQ(ids(p.keep), (V{"a", "b"}));
  EXPECT_TRUE(p.hide.empty() && p.unhide.empty());
}

TEST(ShelfMirrorPlan, SwitchingShelvesSwapsVisibleSets) {
  // Shelf X held a and b; shelf Y holds b and c; c was cached from an earlier visit.
  const Plan p = plan({book("b"), book("c"), book("d")}, {visible("a"), visible("b"), hidden("c")}, true, allPresent());
  EXPECT_EQ(ids(p.hide), (V{"a"}));
  EXPECT_EQ(ids(p.keep), (V{"b"}));
  EXPECT_EQ(ids(p.unhide), (V{"c"}));
  EXPECT_EQ(ids(p.download), (V{"d"}));
}

TEST(ShelfMirrorPlan, IncompleteWalkNeverHides) {
  const Plan p = plan({book("a")}, {visible("a"), visible("b")}, false, allPresent());
  EXPECT_TRUE(p.hide.empty());
  EXPECT_EQ(ids(p.keep), (V{"a", "b"}));
}

TEST(ShelfMirrorPlan, EmptyListingNeverHides) {
  // Calibre-Web answers a deleted or hidden shelf with an empty 200 feed.
  const Plan p = plan({}, {visible("a")}, true, allPresent());
  EXPECT_TRUE(p.hide.empty());
  EXPECT_EQ(ids(p.keep), (V{"a"}));
}

TEST(ShelfMirrorPlan, MissingFileIsForgottenAndDownloadedAgain) {
  const Plan p = plan({book("a")}, {visible("a"), visible("b")}, true, existsIn({}));
  EXPECT_EQ(ids(p.download), (V{"a"}));
  EXPECT_TRUE(p.keep.empty() && p.hide.empty() && p.unhide.empty());
}

TEST(ShelfMirrorPlan, MissingHiddenFileIsDownloadedAgain) {
  const Plan p = plan({book("a")}, {hidden("a")}, true, existsIn({"/C/a.epub"}));  // only the visible path exists
  EXPECT_EQ(ids(p.download), (V{"a"}));
  EXPECT_TRUE(p.unhide.empty());
}

TEST(ShelfMirrorPlan, DuplicateFeedEntriesDownloadOnce) {
  const Plan p = plan({book("a"), book("b"), book("a")}, {}, true, allPresent());
  EXPECT_EQ(ids(p.download), (V{"a", "b"}));
}

TEST(ShelfMirrorPlan, SurplusRecordForOneBookIsHidden) {
  const Record second{"a", "/C/a (2).epub", false};
  const Plan p = plan({book("a")}, {visible("a"), second}, true, allPresent());
  EXPECT_EQ(ids(p.keep), (V{"a"}));
  ASSERT_EQ(p.hide.size(), 1u);
  EXPECT_EQ(p.hide[0].path, "/C/a (2).epub");
}

TEST(ShelfMirrorPlan, EmptyIdFallsBackToHref) {
  const RemoteBook noId{"", "T", "A", "/opds/download/9/epub/"};
  const Plan p = plan({noId}, {{"/opds/download/9/epub/", "/C/t.epub", false}}, true, allPresent());
  EXPECT_TRUE(p.download.empty());
  EXPECT_EQ(p.keep.size(), 1u);
}

TEST(ShelfMirrorManifest, RoundTrips) {
  const std::vector<Record> recs{{"urn:uuid:1", "/Calibre/A - B.epub", false},
                                 {"urn:uuid:2", "/Calibre/C (2).epub", true}};
  std::vector<Record> parsed;
  ASSERT_TRUE(parseManifest(serializeManifest(recs), parsed));
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].id, "urn:uuid:1");
  EXPECT_FALSE(parsed[0].hidden);
  EXPECT_EQ(parsed[1].path, "/Calibre/C (2).epub");
  EXPECT_TRUE(parsed[1].hidden);
}

TEST(ShelfMirrorManifest, RejectsUnknownHeader) {
  std::vector<Record> parsed{{"stale", "/s.epub", false}};
  EXPECT_FALSE(parseManifest("something else\nurn:1\tv\t/x.epub\n", parsed));
  EXPECT_TRUE(parsed.empty());
}

TEST(ShelfMirrorManifest, EmptyTextIsAnEmptyManifest) {
  std::vector<Record> parsed;
  EXPECT_TRUE(parseManifest("", parsed));
  EXPECT_TRUE(parsed.empty());
}

TEST(ShelfMirrorManifest, SkipsMalformedLinesAndHandlesCrlf) {
  std::vector<Record> parsed;
  ASSERT_TRUE(parseManifest(
      "shelf-mirror "
      "v1\r\nnotab\r\n\tv\t/x.epub\r\nid\tv\t\r\nid\tx\t/bad.epub\r\nid\t/old-format.epub\r\nok\th\t/ok.epub\r\n",
      parsed));
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].id, "ok");
  EXPECT_EQ(parsed[0].path, "/ok.epub");
  EXPECT_TRUE(parsed[0].hidden);
}

TEST(ShelfMirrorManifest, DropsRecordsThatWouldBreakTheFormat) {
  const auto text =
      serializeManifest({{"bad\tid", "/x.epub", false}, {"id", "/bad\npath", false}, {"good", "/g.epub", false}});
  std::vector<Record> parsed;
  ASSERT_TRUE(parseManifest(text, parsed));
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].id, "good");
}

TEST(ShelfMirrorUniquePath, ReturnsPlainPathWhenFree) {
  EXPECT_EQ(uniquePath("/Calibre", "A - B.epub", existsIn({})), "/Calibre/A - B.epub");
  EXPECT_EQ(uniquePath("/Calibre/", "A - B.epub", existsIn({})), "/Calibre/A - B.epub");
}

TEST(ShelfMirrorUniquePath, NumbersCollisions) {
  EXPECT_EQ(uniquePath("/C", "A.epub", existsIn({"/C/A.epub", "/C/A (2).epub"})), "/C/A (3).epub");
}

TEST(ShelfMirrorUniquePath, HandlesNoExtension) { EXPECT_EQ(uniquePath("/C", "A", existsIn({"/C/A"})), "/C/A (2)"); }
