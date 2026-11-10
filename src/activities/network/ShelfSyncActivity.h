#pragma once

#include <OpdsClient.h>
#include <OpdsEntry.h>
#include <ShelfMirror.h>

#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/CatalogActivity.h"
#include "network/OpdsHttpTransport.h"

// Mirrors one Calibre-Web shelf into /Calibre on the SD card. Books on the
// shelf are downloaded or restored; synced books that are not on it move to
// /Calibre/.cache, which the library skips. Nothing is deleted. The first run,
// or "Shelf", lists the server's shelves.
class ShelfSyncActivity final : public CatalogActivity {
 public:
  ShelfSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override { return state == State::BROWSING ? static_cast<int>(rows.size()) : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawFooter() override;

  void startBrowse() override;
  bool hasSearch() const override { return false; }
  void performSearch(const std::string&) override {}
  void downloadFinished(bool) override {}

 private:
  struct Row {
    std::string label;
    std::string value;
    std::string href;  // shelf feed URL; empty for the action rows
    int action;
  };
  enum RowAction { ROW_SHELF, ROW_SYNC_AGAIN, ROW_CHOOSE_SHELF };

  struct Config {
    std::string serverUrl;
    std::string feedUrl;
    std::string title;
  };

  void showShelfPicker();
  void runSync();
  bool walkShelf(std::vector<shelf_mirror::RemoteBook>& out, bool& complete);
  void showSummary();
  void syncFailed(StrId message);  // logs a RESULT line, then shows the error
  void setRows(std::vector<Row> newRows);

  static bool loadConfig(Config& out);
  static bool saveConfig(const Config& cfg);
  static bool loadManifest(std::vector<shelf_mirror::Record>& out);
  static bool saveManifest(const std::vector<shelf_mirror::Record>& records);

  OpdsServer server;
  OpdsHttpTransport opdsTransport;
  freeink::opds::OpdsClient opdsClient{opdsTransport};

  Config config;
  bool pickingShelf = false;
  std::vector<Row> rows;
  std::vector<freeink::ui::ListItem> rowItems;

  int added = 0;
  int restored = 0;
  int hiddenCount = 0;
  int failed = 0;
  bool cancelled = false;
  bool incomplete = false;
};
