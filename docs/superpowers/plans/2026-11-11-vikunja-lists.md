# Vikunja Lists Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Lists row on Home that shows Vikunja projects and their open
items, lets the user tick items off offline, and sends the ticks to Vikunja
when the device is next online.

**Architecture:** A host-tested library `lib/Vikunja/` holds config
parsing, JSON models, the saved-copy format and the pending-tick rules. The
device side `src/vikunja/` holds the config store (obfuscation), the HTTP
client and sync. `src/activities/lists/` holds the screens. The split
mirrors `lib/HomeAssistant/` and `src/homeassistant/`.

**Tech Stack:** C++17, ArduinoJson, `HttpDownloader` (esp_http_client,
CA bundle), GoogleTest via `test/CMakeLists.txt`, Python fake server for the
simulator.

**Spec:** `docs/superpowers/specs/2026-11-11-vikunja-lists-design.md`

## Global Constraints

- Config path `/vikunja.json`. Saved copy under `/.crosspoint/vikunja/`
  (`projects.json`, `p<id>.json`). Pending ticks in
  `/.crosspoint/vikunja-pending.json`.
- Token sent as `Authorization: Bearer` (`FetchOptions::bearer`). At most
  `OpdsServer::MAX_CUSTOM_HEADERS` (2) extra headers, sent on every request.
- Plain `token` and header `value` are replaced by `token_obf` and
  `value_obf` (`obfuscation::obfuscateToBase64`) on first load, and the
  file is rewritten via `.tmp` + `Storage.replaceFile`.
- PSRAM boards and the simulator only (`#if defined(BOARD_HAS_PSRAM) ||
  defined(SIMULATOR)`, as in `src/homeassistant/HaConfigStore.cpp:37`).
- No limit on projects or tasks. A failed allocation keeps what was
  loaded and sets the "List incomplete" status.
- The Lists row sits directly above More in the list home menu. Cover
  Grid's order is not changed.
- Strings in `lib/I18n/translations/english.yaml`: `STR_LISTS` "Lists",
  `STR_COMPLETED_N` "Completed (%d)", `STR_LISTS_OFFLINE` "Offline: saved
  copy", `STR_LISTS_PENDING_N` "%d changes not sent", `STR_LISTS_SYNC_FAILED`
  "Sync failed", `STR_LISTS_TOKEN_REJECTED` "Token rejected",
  `STR_LISTS_ACCESS_REFUSED` "Cloudflare Access refused",
  `STR_LISTS_INCOMPLETE` "List incomplete", `STR_LISTS_SYNC` "Sync",
  `STR_FAVOURITE_LIST` "Favourite list", `STR_LISTS_EMPTY` "Nothing left".
- Commits need the user's approval first. The firmware-handoff skill runs
  before any commit that changes firmware.

## Review Focus

- **Ticking an item then losing power before the screen redraws.** The
  tick must already be on the card: the pending file is written before
  `requestUpdate()`. Pinned in Task 4 (`PendingTicks` round-trip) and
  Task 7 (the order of write vs redraw, checked in review).
- **A task deleted or already done on the server while a tick waits.**
  The tick is dropped, not retried forever. Pinned in Task 4
  (`resolveTick` cases).
- **Cloudflare Access refusing the headers.** It answers 302 to a login
  page, not 401. It must show "Cloudflare Access refused", not "Sync
  failed" or an empty list. Pinned in Task 5 (`classifyResponse`).
- **A title with emoji or characters the font lacks** (common in shopping
  lists). The row must draw without crashing, using the renderer's usual
  missing-glyph handling. Covered by a fixture title in Task 8.
- **The favourite project deleted on the server.** Lists must open the
  project list, and the setting must show None. Pinned in Task 3
  (`findProject` miss) and checked in Task 9.

---

### Task 1: Confirm the Vikunja API against a real server

No code. Needs the user's Cloudflare Service Auth policy on their server
and a Vikunja API token.

**Files:**
- Modify: `docs/superpowers/specs/2026-11-11-vikunja-lists-design.md` (add an "API notes" section)

- [ ] **Step 1:** With the Cloudflare service token read inside a script
  and never printed, request
  `GET /api/v1/info` with the two CF headers. Expected: HTTP 200 JSON
  with `version`.
- [ ] **Step 2:** Using a Vikunja API token (read the same way, never
  printed), record for that version:
  - the projects list endpoint, its pagination parameters and the header
    that gives the page count;
  - how to list one project's tasks, both open and done (a view's
    `/projects/{id}/views/{view}/tasks` or a filtered task list), and
    whether the default list view filters out done tasks;
  - the task update request (expected `POST /api/v1/tasks/{id}` with the
    full task body), and whether a body with only `done` clears other
    fields.
- [ ] **Step 3:** Write synthetic fixtures with the same shape as each
  response (no real ids, titles or dates) in `test/vikunja/fixtures/`.
- [ ] **Step 4:** Write the findings into the spec's "API notes" section.
  Tasks 3 and 5 use those endpoints.

### Task 2: Config parsing

**Files:**
- Create: `lib/Vikunja/VikunjaConfig.{h,cpp}`
- Create: `test/vikunja/CMakeLists.txt`, `test/vikunja/VikunjaConfigTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(vikunja)`)

**Interfaces:**
- Produces:
  ```cpp
  namespace vikunja {
  struct Header { std::string name, value, valueObf; };
  struct Config {
    std::string url;       // no trailing slash
    std::string token, tokenObf;
    std::vector<Header> headers;  // at most kMaxHeaders
  };
  constexpr size_t kMaxHeaders = 2;
  bool parseConfig(std::string_view json, Config& out, std::string* error);
  std::string serializeConfig(const Config& cfg);  // writes token_obf/value_obf only
  }
  ```

- [ ] **Step 1: Write the failing tests**
  - `ParsesUrlTokenHeaders`: url `https://todo.example.com/` gives
    `url == "https://todo.example.com"`, plus the token and two headers.
  - `RejectsMissingUrl`, `RejectsNonHttpScheme` (`ftp://x`), and
    `RejectsNoToken` (neither `token` nor `token_obf`). Each returns false
    with a non-empty error.
  - `KeepsObfFieldsWhenNoPlain`: `token_obf` and `value_obf` are read into
    `tokenObf` and `valueObf`.
  - `ExtraHeadersIgnored`: 3 headers give `headers.size() == 2`.
  - `SerializeOmitsPlainValues`: the output contains `token_obf` and
    `value_obf`, and contains neither `"token":` nor `"value":`.
- [ ] **Step 2:** Build and run. `cmake --build build/test --target VikunjaTest && ctest --test-dir build/test -R Vikunja`
  (cmake and ctest from PlatformIO's `tool-cmake` package).
  Expected: compile failure.
- [ ] **Step 3: Implement** with ArduinoJson, following `lib/HomeAssistant/HaConfig.cpp`.
- [ ] **Step 4:** Run again. Expected: PASS.

### Task 3: Projects, tasks and the saved copy

**Files:**
- Create: `lib/Vikunja/VikunjaModel.{h,cpp}`
- Test: `test/vikunja/VikunjaModelTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace vikunja {
  struct Project { int64_t id; std::string title; bool archived; };
  struct Task { int64_t id; std::string title; bool done; std::string doneAt; double position; };
  // Appends one API page; false on malformed JSON. Archived projects are kept
  // with archived=true (the screen hides them).
  bool appendProjectsPage(std::string_view json, std::vector<Project>& out);
  bool appendTasksPage(std::string_view json, std::vector<Task>& out);
  std::string serializeProjects(const std::vector<Project>&);
  bool parseProjects(std::string_view json, std::vector<Project>& out);
  std::string serializeTasks(const std::vector<Task>&);
  bool parseTasks(std::string_view json, std::vector<Task>& out);
  const Project* findProject(const std::vector<Project>&, int64_t id);
  }
  ```

- [ ] **Step 1: Write the failing tests** using the Task 1 fixtures:
  - `ParsesProjectsFixture`: counts and an archived flag match the fixture.
  - `ParsesTasksFixture`: id, title, done, done time and position match.
  - `AppendsSecondPage`: two pages give the sum of their counts.
  - `MalformedPageFails`.
  - `SavedCopyRoundTrips`: serialize then parse gives equal vectors.
  - `FindProjectMiss` returns nullptr for an unknown id.
  - `EmojiTitleKept`: a title of `"Eggs 🥚"` survives a round trip byte for byte.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3: Implement.** The `appendTasksPage` field names come from Task 1's notes.
- [ ] **Step 4:** Run. Expected: PASS.

### Task 4: Pending ticks and the merged view

**Files:**
- Create: `lib/Vikunja/VikunjaPending.{h,cpp}`
- Test: `test/vikunja/VikunjaPendingTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace vikunja {
  struct Tick { int64_t taskId; bool done; };
  class PendingTicks {
   public:
    // Adds a tick; a tick that undoes a pending one removes it instead.
    void toggle(int64_t taskId, bool done, bool serverDone);
    const std::vector<Tick>& all() const;
    void remove(int64_t taskId);
    std::string serialize() const;
    bool parse(std::string_view json);
  };
  // Saved tasks with ticks applied, split for the two screens. Open keeps
  // position order; done is most recently done first (ticked-now first).
  void splitTasks(const std::vector<Task>& saved, const PendingTicks&,
                  std::vector<const Task*>& open, std::vector<const Task*>& done);
  enum class TickAction { Send, Drop };
  // Server state for a waiting tick: nullptr means deleted.
  TickAction resolveTick(const Tick&, const Task* serverTask);
  }
  ```

- [ ] **Step 1: Write the failing tests**
  - `TickThenUntickCancels`: `toggle(5,true,false)` then
    `toggle(5,false,false)` leaves `all()` empty.
  - `UntickDoneItemIsPending`: `toggle(5,false,true)` gives one tick
    `{5,false}`.
  - `RoundTrip`: serialize then parse gives the same ticks.
  - `SplitAppliesTicks`: a saved open task with a pending done tick appears
    in `done` and not in `open`, and is first in `done`.
  - `SplitOpenKeepsPosition`.
  - `ResolveDeletedDrops`, `ResolveAlreadyInStateDrops`, `ResolveDiffersSends`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Ask the user to approve a commit of Tasks 2 to 4
  (library and tests only, no firmware change, so the lighter review
  applies).

### Task 5: Config store, HTTP client and sync (device side)

**Files:**
- Create: `src/vikunja/VikunjaStore.{h,cpp}`: the config load, obfuscation
  and rewrite (copy `src/homeassistant/HaConfigStore.cpp:21-73`), plus
  reading and writing the saved copy and pending files.
- Create: `src/vikunja/VikunjaClient.{h,cpp}`: requests.
- Create: `src/vikunja/VikunjaSync.{h,cpp}`: push, then reload.
- Create: `lib/Vikunja/VikunjaStatus.{h,cpp}` and `test/vikunja/VikunjaStatusTest.cpp`.
- Modify: `src/main.cpp` (call `vikunja::loadStore()` next to `ha::loadConfigStore()`).

**Interfaces:**
- Produces:
  ```cpp
  namespace vikunja {
  // lib (host-tested)
  enum class Status { Ok, Offline, ServerDown, TokenRejected, AccessRefused, Incomplete };
  Status classifyResponse(int httpStatus, bool redirectedToOtherOrigin);
  // src
  void loadStore();
  bool enabled();
  const std::vector<Project>& projects();
  bool loadTasks(int64_t projectId, std::vector<Task>& out);  // from SD
  PendingTicks& pending();
  bool savePending();  // true when written
  // Joins Wi-Fi with joinSavedWifi(10000) if not connected, sends ticks, and
  // reloads projects (and, when projectId != 0, that project's tasks).
  Status sync(int64_t projectId);
  }
  ```

- [ ] **Step 1: Failing tests for `classifyResponse`**:
  - 200 gives Ok.
  - 401 and 403 give TokenRejected.
  - 0 gives Offline.
  - 500 and 530 give ServerDown.
  - 302, or any status with `redirectedToOtherOrigin` true, gives AccessRefused.
- [ ] **Step 2:** Run and see it FAIL. Implement, then run and see it PASS.
- [ ] **Step 3: Implement the store, client and sync.**
  - Requests go through `HttpDownloader::fetchUrl` with `FetchOptions`:
    `bearer`, `headers`, `statusOut` and `captureErrorBody` false.
  - Pages are read into a PSRAM string one at a time and appended with
    the Task 3 functions.
  - A task update needs a body request. Add
    `HttpDownloader::postJson(url, body, FetchOptions, std::string& out)`
    next to `postForm` in `src/network/HttpDownloader.{h,cpp}`. It sends
    `Content-Type: application/json` and the same auth and headers. Reuse
    `postForm`'s client setup rather than copying it.
  - Saved-copy writes go through `.tmp` + `Storage.replaceFile`.
  - Sync drops a tick only on `TickAction::Drop`, or after a 2xx update.
    It stops at the first network error.
- [ ] **Step 4:** Build the firmware: `pio run -e <x4 pro env>`. Expected:
  success with no new warnings.

### Task 6: Fake Vikunja server

**Files:**
- Create: `scripts/fake_vikunja.py`, `scripts/test_fake_vikunja.py`, `scripts/vikunja_fixtures/home.json`

**Interfaces:**
- Produces: `python3 scripts/fake_vikunja.py --port 18456 --token TOKEN
  [--require-header NAME=VALUE] [--down] [--per-page N]`. It serves the
  endpoints recorded in Task 1, paginated by `--per-page`.
  - A wrong token gets 401.
  - A missing required header gets 302 to another host, like Cloudflare.
  - An update replaces the whole task, as Vikunja does.

- [ ] **Step 1:** Write `test_fake_vikunja.py` (pattern:
  `scripts/test_fake_ha.py`). Cover these cases:
  - listing pages;
  - 401 on a bad token;
  - 302 on a missing header;
  - an update with only `done` blanking the title.
- [ ] **Step 2:** Run `python3 scripts/test_fake_vikunja.py`. Expected: FAIL.
- [ ] **Step 3:** Implement the server. The fixture holds two projects,
  one archived project, and one task titled `"Eggs 🥚"`.
- [ ] **Step 4:** Run. Expected: PASS.

### Task 7: Lists screens and the Home row

**Files:**
- Create: `src/activities/lists/ListsActivity.{h,cpp}` (project list),
  `src/activities/lists/TaskListActivity.{h,cpp}` (open items or completed,
  chosen by a `bool showDone` constructor flag).
- Create: `src/components/icons/lists.h`, rasterised from the lucide
  `list-checks` icon with `.dev/icons/raster.py` (threshold 210, rotate
  90), as `more.h` was.
- Modify:
  - `src/components/themes/BaseTheme.h` (`UIIcon::Lists`) and `src/components/themes/lyra/LyraTheme.cpp` (map it to the icon);
  - `src/activities/ActivityManager.{h,cpp}` (`HomeMenuItem::LISTS` before `MORE`, `goToLists()`, `"Lists"` in the `goHome` name map);
  - `src/activities/home/HomeActivity.cpp` (`if (vikunja::enabled()) order.add(HomeMenuItem::LISTS);` before `MORE` in the list branch only, plus the label, icon and activation cases);
  - `src/dev/DevApi.cpp` (`open lists`);
  - `lib/I18n/translations/english.yaml` (the strings in Global Constraints).

**Interfaces:**
- Consumes: Task 5's `vikunja::` functions, and Task 4's `splitTasks` and `PendingTicks::toggle`.
- Produces: `ActivityManager::goToLists()`, and
  `TaskListActivity(renderer, mappedInput, int64_t projectId, bool showDone, bool openedAsFavourite)`.

- [ ] **Step 1: ListsActivity.**
  - Header "Lists" and a status line.
  - Rows: non-archived projects, then a "Sync" row.
  - Back calls `goHome(HomeMenuItem::LISTS)`.
  - On enter, if a favourite is set and `findProject` finds it, it replaces
    itself with that project's `TaskListActivity`. Otherwise it runs
    `sync(0)` and shows the project list.
  - Input follows `MoreActivity.cpp` (ButtonNavigator, `rowTouch`, Confirm).
- [ ] **Step 2: TaskListActivity.**
  - On enter, it runs `sync(projectId)` while showing the saved copy.
  - Rows are the open items from `splitTasks`. The last row is "Completed (N)".
    "Nothing left" shows when there are no open items.
  - Confirm on an item, in this order: `pending().toggle(...)`, then
    `savePending()`, then `requestUpdate()`.
  - If the save fails, the change is undone and "Sync failed" is shown.
  - The status line shows the first match of Status (from the last sync),
    then "N changes not sent".
  - The done view (`showDone`) lists done items, and ticking marks them
    not done.
  - Back: the done view returns to the open view. The open view returns to
    `ListsActivity`, or goes Home when `openedAsFavourite` is set.
- [ ] **Step 3:** Long titles are cut with the renderer's ellipsis
  helper, as other list rows are.
- [ ] **Step 4:** Build for the simulator and the device. Expected: success.

### Task 8: Simulator check

- [ ] **Step 1:**
  - Start `scripts/fake_vikunja.py` with `--require-header CF-Access-Client-Id=test --per-page 2`.
  - Put a `/vikunja.json` pointing at it on the simulator's SD.
- [ ] **Step 2:** Use devctl to `open lists`, take a `screen` capture, tick
  two items, then `open lists` again. Expected:
  - the Home row is above More;
  - the archived project is hidden;
  - the ticked items move to Completed;
  - the fake server's state shows them done with their titles intact;
  - the "Eggs 🥚" row draws.
- [ ] **Step 3:** Restart the fake server with `--down`, then tick an item.
  Expected:
  - the status line reads "Offline: saved copy" or "Sync failed", followed by "1 changes not sent";
  - after a simulator restart, the tick is still shown;
  - after restarting the server without `--down` and running Sync, the tick reaches it.
- [ ] **Step 4:** Check each error status. Expected:
  - a wrong token shows "Token rejected";
  - a missing header shows "Cloudflare Access refused".
- [ ] **Step 5:** Check that `/vikunja.json` on the simulator SD no longer
  has `"token":` or `"value":`.

### Task 9: Favourite list setting

**Files:**
- Modify:
  - `src/CrossPointSettings.{h,cpp}`: `int64_t vikunjaFavourite = 0`, saved as `vikunjaFavourite` in toJson and fromJson, next to `uiTheme`;
  - `src/activities/settings/SettingsActivity.{h,cpp}`: `SettingAction::FavouriteList`, added in `SettingsSection::Network` only when `vikunja::enabled()`.
- Create: `src/activities/lists/FavouritePickerActivity.{h,cpp}`.

- [ ] **Step 1: Implement.**
  - The picker lists "None" plus the non-archived saved projects, with a
    check mark on the current one.
  - Confirm saves `SETTINGS.vikunjaFavourite` and returns.
  - A favourite that `findProject` doesn't find shows None selected.
- [ ] **Step 2: Simulator check.**
  - Set a favourite, then `open lists`. Expected: the project opens directly, and Back goes Home.
  - Delete that project on the fake server and Sync. Expected: Lists opens the project list, and the picker shows None.
- [ ] **Step 3: Host tests.** `ctest --test-dir build/test`. Expected: all pass.

### Task 10: Handoff and device check

- [ ] **Step 1:** Run the `firmware-handoff` skill
  (`.agents/skills/firmware-handoff/SKILL.md`).
- [ ] **Step 2:** Ask the user for approval to flash, and warn them that
  the device freezes during the flash. Then `devctl flash`.
- [ ] **Step 3:** The user checks on the device, against their real
  instance:
  - with Wi-Fi off, tick items;
  - reboot, and the ticks are still there;
  - once online, the ticks reach Vikunja and the items keep their titles.
- [ ] **Step 4:** Ask the user to approve the commit. Before any push,
  scan the diff for personal info (the hostname, tokens, list contents).

## Changes made during implementation

The design doc is current; where this plan differs, the design and code win.

- `Task` became `TaskList` (one title buffer plus a row array) because
  allocations under 4 KB land on the internal heap. `position` and `doneAt`
  were dropped. `splitTasks` returns indices.
- The saved task copy is `p<id>.txt` (tab-separated lines), not JSON.
- A tick is sent by editing `done` in the server's own task text
  (`setTopLevelDone`), and `postJson(url, body, FetchOptions)` sends it.
  Ticks carry their project id so sent ticks update the saved copy.
- Sync fetches projects before sending ticks. A failed Wi-Fi join is not
  retried until the Sync row is picked or Lists is left.
- Paging stops on an empty page, not a short one. Completed loads 50 at a
  time with a "Load more" row, so the row reads "Completed" without a count.
- Screens: `ListsActivity`, `TaskListActivity` (open and done views) and
  `FavouriteListActivity`; shared state changes only under `RenderLock`.
- Strings: "Offline" and "%d not sent" replace the longer forms.
- Back from the favourite goes to the list of lists, like any list; there
  is no `openedAsFavourite` flag. The favourite is the list starred in
  Vikunja unless one is picked in Settings, whose first option is
  "Starred in Vikunja" (not "None").
- HTTPS is not certificate-checked: the SDK's wolfSSL transport has no CA
  bundle (see the design's Constraints).
- `VikunjaClient` and `VikunjaSync` were folded into `src/vikunja/VikunjaStore`.
- The fake server's page cap flag is `--per-page-max`.
- A tick the server refuses with a 4xx other than 401 is dropped so it
  cannot block later ticks. "Load more" uses the server's real page size,
  learned while paging, and skips pages already loaded.
