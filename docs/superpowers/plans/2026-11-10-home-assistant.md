# Home Assistant Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** HA light sliders and buttons in the reader's swipe-down sheet, and a Control page on the home menu that draws one HA dashboard, both configured by `/homeassistant.json`.

**Architecture:** Pure, host-tested parsing and formatting live in `lib/HomeAssistant` (config, dashboard model, formatting, state cache). `src/homeassistant` adds the device side: the config store (token obfuscation), `HaLink` (one FreeRTOS worker that joins the home network, makes REST calls and turns Wi-Fi off after idle), and a Control session (WebSocket on the device, REST polling in the simulator). The sheet and a new `ControlActivity` draw from cached states and update in place.

**Tech Stack:** C++20, ArduinoJson 7.4.2, `links2004/WebSockets` 2.7.3 (`WebSocketsClient`, device only), Arduino `HTTPClient`, FreeInk UI (`UiScreen`, `SliderRowProps`), GoogleTest, Python 3 (fake HA for the simulator).

**Spec:** `docs/superpowers/specs/2026-11-10-home-assistant-design.md`

## Global Constraints

- Config file: `/homeassistant.json`. State cache: `/.crosspoint/homeassistant-state.json`. Layout cache: `/.crosspoint/homeassistant-layout.json`.
- `url` must start with `http://`. Token sent as `Authorization: Bearer <token>`.
- Plain `token` in the file is replaced on first load by `token_obf` (`obfuscateToBase64()`), and the file is rewritten without `token`. A `token_obf` that does not decode counts as no token.
- HA work joins only the `wifi` SSID, which must be a saved network; join timeout 6000 ms; no scan.
- Wi-Fi idle-off: 120000 ms after the last HA activity, only if `HaLink` turned Wi-Fi on, never while Developer Mode is on, never from a network activity.
- Sheet: at most 4 light rows and 1 button row of at most 5 buttons; extras skipped with a log line.
- Control page repaint: at most once every 5000 ms; a tap's own result repaints at once.
- Sliders send `light.turn_on` with `brightness_pct` when the drag ends or on a step press, never while dragging.
- Failure lines (English strings): "Home network not available", "Home Assistant not responding", "Token rejected".
- No config file, invalid config, or a board without PSRAM: nothing new is shown anywhere.
- The repository is public: tests, fixtures and docs use made-up entities (`light.reading_lamp`, `scene.evening`, ...), never the user's.
- New user-facing strings go in `lib/I18n/translations/english.yaml`; run `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`.
- Format with `./bin/clang-format-fix -g` before each commit. Commits: busycursor identity, plain ASCII, `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, no `Claude-Session` line.

## Review Focus

1. A token revoked in HA (HTTP 401, or WebSocket `auth_invalid`) → "Token rejected", HA rows gray, no retry loop. Test: Task 6 `HaLinkStatus.Http401IsTokenRejected`.
2. An entity in the config or dashboard that HA does not know (REST 404, missing from the subscription) → shows "unavailable", and its taps still report failure cleanly. Tests: Task 3 `HaFormat.MissingEntityIsUnavailable`, Task 6 `HaLinkStatus.Http404KeepsLinkOk`.
3. Wi-Fi already up when HA acts (Developer Mode, file transfer) → HA never turns it off. Test: Task 6 `HaIdlePolicy.NeverReleasesWifiItDidNotStart`.
4. The device sleeps with Wi-Fi up or a call in flight → after wake, nothing joins until the next tap, and the worker is gone. Test: Task 6 `HaIdlePolicy.SleepResetsOwnership`; device check in Task 9.
5. Config edited by hand with a BOM, CRLF line ends, or unknown keys → still loads; a missing `wifi` or a non-`http://` URL → feature off with the reason logged. Tests: Task 1 `HaConfig.AcceptsBomAndCrlf`, `HaConfig.RejectsHttpsUrl`.

---

### Task 1: Config parsing (`lib/HomeAssistant/HaConfig`)

**Files:**
- Create: `lib/HomeAssistant/HaConfig.h`, `lib/HomeAssistant/HaConfig.cpp`
- Create: `test/home_assistant/CMakeLists.txt`, `test/home_assistant/HaConfigTest.cpp`
- Modify: `test/CMakeLists.txt` (FetchContent `bblanchon/ArduinoJson` tag `v7.4.2`; `add_subdirectory(home_assistant)`)

**Interfaces:**
- Produces:
  ```cpp
  namespace ha {
  enum class SheetKind : uint8_t { Light, Button };
  struct SheetEntry { std::string entity; std::string name; SheetKind kind; };
  struct Config {
    std::string url;        // no trailing slash
    std::string token;      // plain, only when the file had "token"
    std::string tokenObf;   // "token_obf" as stored
    std::string wifiSsid;
    std::string dashboard;  // empty: no Control item
    std::vector<SheetEntry> sheet;  // already capped: <=4 Light, <=5 Button
  };
  bool parseConfig(std::string_view json, Config& out, std::string* error);
  // Same keys, "token" omitted, "token_obf" written from cfg.tokenObf.
  std::string serializeConfig(const Config& cfg);
  constexpr size_t kMaxSheetLights = 4, kMaxSheetButtons = 5;
  }
  ```
- Sheet kinds: `light.*` → Light; `scene`, `script`, `switch`, `button`, `input_boolean`, `fan`, `remote`, `media_player` → Button; any other domain skipped.

- [ ] **Step 1: Write the failing tests** in `HaConfigTest.cpp`: `HaConfig.ParsesFullExample` (spec example: url, token, wifi `HomeNetwork`, 3 sheet entries with kinds Light, Light, Button, dashboard `dashboard-ereader`), `HaConfig.StripsTrailingSlash`, `HaConfig.RejectsHttpsUrl` (false, error mentions `http://`), `HaConfig.RequiresWifi`, `HaConfig.RequiresTokenOrTokenObf`, `HaConfig.AcceptsBomAndCrlf`, `HaConfig.IgnoresUnknownKeys`, `HaConfig.CapsSheet` (6 lights + 7 scenes → 4 + 5, in file order), `HaConfig.SkipsUnsupportedDomain` (`sensor.x` dropped), `HaConfig.SerializeDropsPlainToken` (round trip: output has `token_obf`, no `"token"` key, same sheet and names).
- [ ] **Step 2: Run** `cmake --build build/test --target HaConfigTest` — expected: fails to compile (no `HaConfig.h`).
- [ ] **Step 3: Implement** `parseConfig` / `serializeConfig` with ArduinoJson (`JsonDocument`, `deserializeJson` after skipping a UTF-8 BOM).
- [ ] **Step 4: Run** `ctest --test-dir build/test -R HaConfig` — expected: all pass.
- [ ] **Step 5: Commit** `Home Assistant: parse /homeassistant.json`.

### Task 2: Dashboard model (`lib/HomeAssistant/HaDashboard`)

**Files:**
- Create: `lib/HomeAssistant/HaDashboard.h`, `lib/HomeAssistant/HaDashboard.cpp`
- Test: `test/home_assistant/HaDashboardTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace ha {
  enum class CardKind : uint8_t { Entity, Weather };  // tile/button/entity/entities rows → Entity
  struct Card { CardKind kind; std::string entity; std::string name; bool slider; };
  struct Section { std::string title; std::vector<Card> cards; };
  struct Dashboard { std::string title; std::vector<Section> sections; };
  // view: index into "views"; returns false when the view is missing.
  bool parseDashboard(std::string_view lovelaceJson, size_t view, Dashboard& out);
  std::vector<std::string> dashboardEntities(const Dashboard& d);  // unique, in order
  }
  ```
- Mapping: a `sections` view → one `Section` per entry of `sections`; a `heading` card starts a new `Section` with its `heading` text; a masonry view (`cards` at the top) → one untitled section. `grid`, `vertical-stack`, `horizontal-stack` are flattened in order, nesting up to 3 levels. `entities` → one `Card` per row (string or `{entity, name}`), rows of `type` other than an entity row are dropped. `slider` = the card's `features` contains `{"type": "light-brightness"}` and the entity is `light.*`. `weather-forecast` → `Weather`. Every other `type` (including `custom:*`) is dropped. Empty sections are removed.

- [ ] **Step 1: Write the failing tests**: `HaDashboard.SectionsViewWithHeadings`, `HaDashboard.MasonryViewIsOneSection`, `HaDashboard.FlattensNestedGridAndStacks` (grid → vertical-stack → tile kept, order preserved), `HaDashboard.EntitiesRowsBecomeCards` (string row and object row with name; a `type: divider` row dropped), `HaDashboard.DropsUnsupportedCards` (`custom:mushroom-template-card`, `picture-entity`, `markdown` → absent, no empty section), `HaDashboard.SliderOnlyForLightBrightness` (a `switch` tile with the feature → `slider == false`), `HaDashboard.WeatherForecastCard`, `HaDashboard.MissingViewFails`, `HaDashboard.EntitiesAreUnique`.
- [ ] **Step 2: Run** the target — expected: compile failure.
- [ ] **Step 3: Implement** `parseDashboard` and `dashboardEntities`.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R HaDashboard` — expected: pass.
- [ ] **Step 5: Commit** `Home Assistant: dashboard model with the supported card subset`.

### Task 3: States, formatting and actions (`lib/HomeAssistant/HaState`)

**Files:**
- Create: `lib/HomeAssistant/HaState.h`, `lib/HomeAssistant/HaState.cpp`
- Test: `test/home_assistant/HaStateTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace ha {
  struct EntityState {
    std::string state;        // "unavailable" when unknown to HA
    std::string name;         // friendly_name
    std::string unit;         // unit_of_measurement
    int8_t brightnessPct = -1;  // lights: 0..100, -1 when off or not a light
    std::string forecast;     // weather only: "Mon 72/58  Tue 70/55 ..." (preformatted)
  };
  using StateMap = std::map<std::string, EntityState>;
  // Parses one HA state object ({"entity_id","state","attributes"}).
  bool parseStateObject(std::string_view json, std::string& entityId, EntityState& out);
  std::string displayValue(const std::string& entityId, const EntityState& s);
  struct ServiceCall { std::string domain, service, entityId; int brightnessPct = -1; };
  // Tap action; false for read-only domains (sensor, binary_sensor, weather).
  bool tapAction(const std::string& entityId, ServiceCall& out);
  ServiceCall brightnessCall(const std::string& entityId, int pct);  // light.turn_on, brightness_pct
  bool isToggleable(const std::string& entityId);
  std::string serializeStates(const StateMap& m);
  bool parseStates(std::string_view json, StateMap& out);
  }
  ```
- `displayValue`: `on`/`off` → "On"/"Off"; numbers with unit → value with unit, trailing `.0` trimmed, at most 1 decimal ("80.1 °F"); `H:MM:SS` durations → "0:42" style kept as HA gives it; `unavailable`/`unknown` → "unavailable"; weather → `<condition> <temp><unit>` (condition from state, temperature from attributes, stored in `state`/`unit` by the parser).
- Tap actions: `light`, `switch`, `fan`, `input_boolean`, `media_player`, `remote` → `<domain>.toggle`; `scene` → `scene.turn_on`; `script` → `script.turn_on`; `button` → `button.press`.

- [ ] **Step 1: Write the failing tests**: `HaFormat.OnOff`, `HaFormat.NumberWithUnit` (`80.06` °F → `80.1 °F`; `62` % → `62 %`), `HaFormat.Duration` (`0:42:00` → `0:42:00`), `HaFormat.MissingEntityIsUnavailable` (`unknown` and `unavailable` → `unavailable`), `HaFormat.LightBrightnessFromAttributes` (brightness 128 → 50; off → -1), `HaFormat.WeatherConditionAndTemp`, `HaAction.TapActionsByDomain` (each domain above), `HaAction.SensorHasNoTap`, `HaAction.BrightnessCall`, `HaStates.RoundTrip`.
- [ ] **Step 2: Run** — expected: compile failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R 'HaFormat|HaAction|HaStates'` — expected: pass.
- [ ] **Step 5: Commit** `Home Assistant: state formatting, tap actions and the state cache format`.

### Task 4: Config store on the device (`src/homeassistant/HaConfigStore`)

**Files:**
- Create: `src/homeassistant/HaConfigStore.h`, `src/homeassistant/HaConfigStore.cpp`
- Modify: `src/main.cpp` (load after `WIFI_STORE.loadFromFile()`, line ~413)

**Interfaces:**
- Consumes: `ha::parseConfig`, `ha::serializeConfig` (Task 1); `obfuscateToBase64`, `deobfuscateFromBase64` (`lib/Serialization/ObfuscationUtils.h`); `WIFI_STORE.findCredential(ssid)`.
- Produces:
  ```cpp
  namespace ha {
  void loadConfigStore();             // reads /homeassistant.json once at boot
  bool enabled();                     // PSRAM board, valid file, token decodes, wifi SSID is saved
  const Config& config();             // valid only when enabled()
  const std::string& bearerToken();   // the decoded token
  }
  ```
- Behavior: compiled only with `BOARD_HAS_PSRAM`; elsewhere `enabled()` is `false`. On a plain `token`: obfuscate into `tokenObf`, write `serializeConfig()` to a temp file, rename over `/homeassistant.json`; log `HA config: token stored obfuscated`. Every rejection logs `HA config: <reason>` once.

- [ ] **Step 1: Implement** the store and the boot call.
- [ ] **Step 2: Verify in the simulator**: put the spec example (made-up token, `wifi` set to the simulator's saved network) at `.dev/qa/sim/sd/homeassistant.json`, `scripts/devctl.py sim start`, then `scripts/devctl.py logs --grep "HA config"` — expected: `token stored obfuscated`; `scripts/devctl.py fs get /homeassistant.json` shows `token_obf` and no `"token"`. Restart: no rewrite logged, `enabled()` true (log line `HA config: enabled, 3 sheet entries`).
- [ ] **Step 3: Commit** `Home Assistant: load the config and store the token obfuscated`.

### Task 5: Fake Home Assistant for the simulator

**Files:**
- Create: `scripts/fake_ha.py`, `scripts/test_fake_ha.py`

**Interfaces:**
- Produces: `python3 scripts/fake_ha.py --port 8123 --token TOKEN [--fixture scripts/ha_fixtures/home.json]`, serving `GET /api/states/<id>`, `GET /api/states`, `POST /api/services/<domain>/<service>` (updates its state table, toggles, `brightness_pct`), `POST /api/services/weather/get_forecasts?return_response`, and `GET /api/lovelace-fixture` (the fixture's dashboard JSON, used by the simulator's REST session). 401 on a wrong token; 404 for unknown entities. A `--down` flag makes every request hang past 6 s.
- Create: `scripts/ha_fixtures/home.json` with made-up entities (two lights, a scene, a switch, a weather entity, a temperature sensor, a duration sensor) and a `sections` dashboard using every supported card plus one `custom:` card.

- [ ] **Step 1: Write** `test_fake_ha.py` (unittest, starts the server on a free port): toggle flips state; `brightness_pct` sets brightness 0..255; wrong token → 401; unknown entity → 404.
- [ ] **Step 2: Run** `python3 scripts/test_fake_ha.py` — expected: fails (no server).
- [ ] **Step 3: Implement** `fake_ha.py` with `http.server`.
- [ ] **Step 4: Run** — expected: `OK`.
- [ ] **Step 5: Commit** `Add a fake Home Assistant for simulator tests`.

### Task 6: `HaLink`: worker, joins, REST, idle policy

**Files:**
- Create: `lib/HomeAssistant/HaIdlePolicy.h`, `lib/HomeAssistant/HaIdlePolicy.cpp` (pure)
- Create: `src/homeassistant/HaLink.h`, `src/homeassistant/HaLink.cpp`
- Modify: `src/network/WifiJoin.h/.cpp` (add `bool joinWifiNetwork(const std::string& ssid, unsigned long timeoutMs);`, no scan, reusing the saved password)
- Modify: `src/activities/Activity.h` (add `virtual bool allowsHomeAssistantWifiOff() const { return false; }`)
- Modify: `src/main.cpp` (call `haLink.tick()` each loop; `haLink.onSleep()` before the Wi-Fi shutdown at line ~505)
- Test: `test/home_assistant/HaIdlePolicyTest.cpp`, `test/home_assistant/HaLinkStatusTest.cpp`

**Interfaces:**
- Produces (pure):
  ```cpp
  namespace ha {
  enum class LinkStatus : uint8_t { Idle, Working, Ok, NoNetwork, NoAnswer, TokenRejected };
  LinkStatus statusFromHttp(int httpCode);  // 2xx/404 → Ok, 401/403 → TokenRejected, <=0 or 5xx → NoAnswer
  class IdlePolicy {
   public:
    void noteJoined(bool wifiWasAlreadyUp, uint32_t nowMs);
    void noteActivity(uint32_t nowMs);
    void noteSleep();  // forget ownership
    // True once: 120000 ms idle, we own Wi-Fi, dev mode off, activity allows.
    bool shouldRelease(uint32_t nowMs, bool developerMode, bool activityAllows);
  };
  }
  ```
- Produces (device):
  ```cpp
  class HaLink {
   public:
    bool submit(const ha::ServiceCall& call);   // false while a call is in flight
    ha::LinkStatus status() const;              // last outcome, thread-safe
    bool takeResult(std::string& entityId, ha::EntityState& fresh);  // after a call: the entity's new state
    void tick();       // main loop: idle release via IdlePolicy
    void onSleep();    // stop the worker, forget ownership
    void noteActivity();
  };
  extern HaLink haLink;
  ```
- Worker: one FreeRTOS task created on the first `submit`, stack 8192 bytes to start (log `uxTaskGetStackHighWaterMark` after each call and adjust), deleted in `onSleep()` and on release. Per call: `joinWifiNetwork(config().wifiSsid, 6000)` (→ `NoNetwork` on failure), `WifiPowerSaveGuard` for the exchange, `POST <url>/api/services/<domain>/<service>` with `{"entity_id": ..., "brightness_pct": ...}`, 5000 ms timeout, then `GET /api/states/<entity>` for the fresh state. Release: `WiFi.disconnect(false)`, `WiFi.mode(WIFI_OFF)`.
- `allowsHomeAssistantWifiOff()` returns `true` in `HomeActivity`, `EpubReaderActivity` (and the other readers via `isReaderActivity()` default), `FrontlightPanelActivity`, `ControlActivity`.

- [ ] **Step 1: Write the failing tests**: `HaLinkStatus.Http401IsTokenRejected`, `HaLinkStatus.Http404KeepsLinkOk`, `HaLinkStatus.TimeoutIsNoAnswer` (code -1), `HaIdlePolicy.ReleasesAfterTwoMinutes` (false at 119999 ms, true at 120000, then false), `HaIdlePolicy.ActivityResetsTimer`, `HaIdlePolicy.NeverReleasesWifiItDidNotStart`, `HaIdlePolicy.NeverDuringDeveloperMode`, `HaIdlePolicy.WaitsForAllowingActivity`, `HaIdlePolicy.SleepResetsOwnership`.
- [ ] **Step 2: Run** — expected: compile failure.
- [ ] **Step 3: Implement** `HaIdlePolicy` and `statusFromHttp`; run `ctest -R 'HaIdlePolicy|HaLinkStatus'` — expected: pass.
- [ ] **Step 4: Implement** `HaLink`, `joinWifiNetwork`, the `Activity` hook and the `main.cpp` calls.
- [ ] **Step 5: Build** `scripts/devctl.py build all` — expected: `built`.
- [ ] **Step 6: Commit** `Home Assistant: link worker with home-network joins and idle Wi-Fi off`.

### Task 7: Swipe-down sheet rows

**Files:**
- Modify: `src/activities/util/FrontlightPanelActivity.h/.cpp` (HA section after the warmth row, before the quick tiles; `computePanelBottom()` counts it)
- Create: `src/homeassistant/HaStateCache.h/.cpp` (loads/saves `/.crosspoint/homeassistant-state.json` with `ha::parseStates`/`serializeStates`; saves only when a state changed, at most once per call result)
- Modify: `lib/I18n/translations/english.yaml` (`STR_HA_NO_NETWORK: "Home network not available"`, `STR_HA_NO_ANSWER: "Home Assistant not responding"`, `STR_HA_TOKEN_REJECTED: "Token rejected"`)

**Interfaces:**
- Consumes: `ha::enabled()`, `ha::config().sheet` (Task 4); `haLink.submit/status/takeResult` (Task 6); `ha::tapAction`, `ha::brightnessCall`, `ha::displayValue` (Task 3); `addSliderRow(...)` (existing, `FrontlightPanelActivity.h:51`).
- Produces: `namespace ha { StateMap& stateCache(); void saveStateCacheIfDirty(); }`.
- Rows: each `Light` entry is a slider row (caption = `name` or cached friendly name; value = cached `brightnessPct`, 0 when off) with the lamp toggle; `Button` entries share one row of equal slots labeled by name, filled when on. A pending control draws its outline dashed. When `status()` is `NoNetwork`/`NoAnswer`/`TokenRejected`, HA rows draw in gray (dither) with the matching line above them; the next tap retries. Nothing calls `haLink` from `onEnter()`.

- [ ] **Step 1: Implement** the rows and wire slider release, step presses, lamp toggle and button taps to `haLink.submit`.
- [ ] **Step 2: Verify in the simulator** with `scripts/fake_ha.py` and a config pointing at it: open a book, open the sheet — `devctl.py logs --grep "WIFI|HA "` shows no join; tap the first light's toggle — a join and one `POST`, and only that row changes (`devctl.py screen` before/after). Restart the fake with `--down`, tap — rows gray with "Home Assistant not responding". Wrong token — "Token rejected".
- [ ] **Step 3: Build** `scripts/devctl.py build all`; **run** `ctest --test-dir build/test` — expected: all pass.
- [ ] **Step 4: Commit** `Reader sheet: Home Assistant light sliders and buttons`.

### Task 8: Control page

**Files:**
- Create: `src/homeassistant/HaSession.h` (interface), `src/homeassistant/HaWsSession.cpp` (device, `#ifndef SIMULATOR`), `src/homeassistant/HaRestSession.cpp` (simulator)
- Create: `src/activities/home/ControlActivity.h/.cpp`
- Modify: `src/activities/home/HomeActivity.h/.cpp` (`HomeMenuItem::CONTROL` between `FILE_TRANSFER` and `SETTINGS_MENU`, shown when `ha::enabled() && !ha::config().dashboard.empty()`; icon: the existing `UIIcon::Blocks`), `src/activities/ActivityManager.h/.cpp` (`goToControl()`)
- Modify: `english.yaml` (`STR_CONTROL: "Control"`)

**Interfaces:**
- Consumes: `ha::parseDashboard`, `ha::dashboardEntities` (Task 2); state functions (Task 3); `haLink` (Task 6, for taps, joins and `noteActivity()`); `ha::stateCache()` (Task 7).
- Produces:
  ```cpp
  class HaSession {  // owned by ControlActivity, worker-driven
   public:
    virtual ~HaSession() = default;
    virtual void start() = 0;   // join, auth, fetch layout, subscribe
    virtual void stop() = 0;    // close; called from onExit()
    // Drains changes since the last call; true when the layout changed.
    virtual bool poll(std::string* layoutJson, std::vector<std::pair<std::string, ha::EntityState>>* changes) = 0;
    virtual ha::LinkStatus status() const = 0;
  };
  std::unique_ptr<HaSession> makeHaSession();
  ```
- Device session: `WebSocketsClient` to `<url>/api/websocket`; `auth` with the token (`auth_invalid` → `TokenRejected`); `lovelace/config` with `url_path = dashboard`; `subscribe_entities` with `entity_ids = dashboardEntities()`; weather via `weather/subscribe_forecast` (`forecast_type: daily`). Buffers in PSRAM.
- Simulator session: layout from `GET /api/lovelace-fixture`, states polled with `GET /api/states/<id>` every 5000 ms.
- Page: draws `/.crosspoint/homeassistant-layout.json` and cached states at once, then `start()`; repaints changed cards at most every 5000 ms (a tap's result at once); taps per `ha::tapAction`; light cards with `slider` get a slider row; a failure line in the header keeps the cached cards. Saves the layout cache when it changed.

- [ ] **Step 1: Implement** `HaSession`, both sessions, `ControlActivity`, the menu item and `goToControl()`.
- [ ] **Step 2: Verify in the simulator** with the fake: Home shows Control between File Transfer and Settings; it opens with the fixture's cards (no `custom:` card), headings as titles; a tap on a light toggles it in the fake (`curl` the fake's state) and repaints at once; changing a sensor in the fake repaints within about 5 s. Without `dashboard` in the config, no Control item.
- [ ] **Step 3: Build** `scripts/devctl.py build all` and `pio run -e x4pro-gh_release`; **run** `ctest --test-dir build/test` — expected: all pass.
- [ ] **Step 4: Commit** `Home: Control page drawing a Home Assistant dashboard`.

### Task 9: Starter setup and device verification (local only)

**Files:** none in the repository. Local: `.dev/ha_setup.py` (git-ignored), the device's `/homeassistant.json`.

- [ ] **Step 1:** With `.dev/ha_setup.py` (WebSocket `lovelace/dashboards/create` with `url_path: dashboard-ereader`, `title: E-reader`, `require_admin: false`, `show_in_sidebar: false`; then `lovelace/config/save`), create the starter dashboard: a section with a `weather-forecast` card and a sensor tile, a section of sensor tiles, and a section of light tiles with the `light-brightness` feature plus an `entities` row for one on/off device. Check first that `dashboard-ereader` does not exist; change nothing else.
- [ ] **Step 2:** Write `/homeassistant.json` to the device (`devctl.py fs put`) with url, a long-lived token, `wifi` = the device's saved home SSID, sheet = two room lights, `dashboard: dashboard-ereader`. Flash (`scripts/devctl.py --host <ip> flash`; tell the user first, the device freezes during upload).
- [ ] **Step 3: Device checks** (with the user): token rewritten as `token_obf`; sheet shows two light rows and joins only on the first tap; Control page draws the starter dashboard and updates live; Wi-Fi off 2 minutes after the last HA action (logs); with Developer Mode on, Wi-Fi stays up; after sleep and wake, no join until a tap.

---

## Self-review notes

- Spec coverage: config and obfuscation (1, 4), card subset (2), formatting and actions (3), Wi-Fi rules and failure lines (6, 7), sheet (7), Control page and live updates (8), starter dashboard (9), PSRAM gating (4), no-config invisibility (4, 7, 8).
- Execution: native, in this session (no subagents). Upstream's firmware-handoff review step does not apply to this fork.
