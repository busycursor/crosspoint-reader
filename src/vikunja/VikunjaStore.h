#pragma once

#include <VikunjaModel.h>
#include <VikunjaPending.h>
#include <VikunjaStatus.h>

#include <cstdint>
#include <vector>

// Vikunja lists: /vikunja.json, the saved copy under /.crosspoint/vikunja/,
// waiting ticks, and sync. A plain "token" or header "value" in the config
// is stored obfuscated (keyed to this chip) and the file is rewritten
// without it. All calls run on the UI task; sync blocks while it runs.
namespace vikunja {

constexpr const char* kConfigPath = "/vikunja.json";
// Tasks asked for per API page.
constexpr size_t kPageSize = 50;

void loadStore();
// PSRAM board (or simulator), a valid config, and a token that decodes.
bool enabled();

// The saved copy of the projects, in server order.
const std::vector<Project>& projects();
// The saved copy of one project's tasks. False when there is none.
bool loadTasks(int64_t projectId, TaskList& out);

PendingTicks& pending();
// Writes the waiting ticks to the SD card. True when written.
bool savePending();

// The full sync, once per visit to Lists (and on the Sync row): joins Wi-Fi
// if needed, reloads the projects, sends waiting ticks and, when projectId
// is not 0, loads that project's open items and newest done page into
// *tasks (also saved). On failure the saved copy is left as it was.
Status sync(int64_t projectId, TaskList* tasks);
// Loads one project's items into *tasks (also saved), without the rest of
// a sync: for a list first opened after this visit's sync.
Status refreshProject(int64_t projectId, TaskList* tasks);
// Whether this visit has tried the full sync, and fetched this project.
// Tried counts, so an offline visit does not try again on every screen.
bool syncedThisVisit();
// Items per page the server sends: kPageSize, or its lower cap once a sync
// has paged through something. A done list this long may have more.
size_t pageSize();
bool fetchedThisVisit(int64_t projectId);
// Appends the next page of done items with anything new, starting at
// *page (1 is the one sync loads), and saves it. *page is left at the page
// to ask for next; *more says whether one may follow.
Status loadMoreDone(int64_t projectId, int* page, TaskList& tasks, bool* more);
// Result of the last sync or page load.
Status lastStatus();

// After a failed Wi-Fi join, sync stays offline without trying again until
// this is called (the Sync row) or Lists is left.
void retryWifi();
// Leaving Lists: turns Wi-Fi off again if sync turned it on (never in
// Developer Mode), forgets a failed join, and ends the visit.
void endVisit();

}  // namespace vikunja
