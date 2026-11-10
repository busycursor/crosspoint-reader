# Home Assistant support: design

## Intent

Control a few Home Assistant (HA) things from the reader without leaving the
book, and glance at a small HA dashboard from the home screen. Both work on
the home network only. Wi-Fi costs battery and a join takes seconds, so the
device must not join in the background, and the screen must not draw twice
("loads, then loads again") just because a connection came up.

## Goals

1. **Swipe-down sheet** (the reader's pull-down panel, `FrontlightPanelActivity`):
   under brightness and warmth, a short list of HA entities from the config
   file. Lights get a brightness slider and an on/off toggle (the same row
   style as brightness). Scenes, scripts, switches and buttons, if listed,
   share one row of buttons.
2. **Control page**: a new home menu item, between File Transfer and
   Settings, that draws one HA dashboard named in the config file. That
   dashboard is built in HA for this device only. Cards the device does not
   support are dropped from the layout, without a placeholder.
3. Control page interaction: show states; a tap toggles lights, switches,
   fans, input booleans and media players, and runs scenes, scripts and
   buttons. Light cards can show a brightness slider, using the same row
   component as the sheet.
4. Everything is configured by one JSON file on the SD card. No device UI
   or web page for setup.

## Non-goals

- Rendering arbitrary HA dashboards. Only the card subset below, in black
  and white.
- Custom cards (mushroom, camera, etc.), cameras, history graphs, maps.
- Climate setpoints, media transport controls, color pickers.
- Remote access (Nabu Casa, HTTPS through the internet). Local HTTP only.
- Discovering HA (mDNS browsing) or creating the token on the device.

## Anti-goals (a failure even if it works)

- Wi-Fi joining by itself: on boot, on wake, on opening the sheet, or in the
  background.
- A sheet that draws, then redraws when Wi-Fi comes up.
- Turning off Wi-Fi that something else needs (Developer Mode, file
  transfer, sync, KOReader sync).
- Changes visible to users without a config file: nothing new appears in
  the sheet or the home menu.

## Constraints

- X4 Pro (ESP32-S3, 8 MB PSRAM). The feature is enabled only on boards with
  PSRAM. Dashboard JSON, entity state and WebSocket buffers go in PSRAM.
- The HA token is a long-lived access token (about 180 characters).
- HA's REST API gives states and service calls. Dashboard layout and live
  updates need the WebSocket API (`/api/websocket`). The firmware already
  depends on `links2004/WebSockets` 2.7.3, which includes a client.
- Weather entities no longer carry a forecast attribute. The forecast comes
  from the `weather.get_forecasts` action with a returned response (REST:
  `POST /api/services/weather/get_forecasts?return_response`) or from the
  WebSocket `weather/subscribe_forecast`.
- Existing helpers: `joinSavedWifi()` (`src/network/WifiJoin.h`) and the
  saved networks in `WifiCredentialStore`.
- This repository is public. Nothing personal (hostnames, entity IDs,
  names) goes into code, tests or docs; examples use made-up entities.

## Decided with the user

- Sheet entities: a short list picked by the user, in the config file.
- Control page: one HA dashboard named in the config file, built for this
  device; unsupported cards are dropped.
- Interaction: view plus tap-to-toggle, and light sliders on the page too.
- Setup: a config file on the SD card.
- Wi-Fi behavior:
  - Sheet: HA rows draw at once from the last saved states. No Wi-Fi until
    the user taps a button or moves a slider. That control shows a working
    state while the device joins the home network and sends the command;
    then only that row updates. If the home network is not reachable within
    about 6 seconds, the HA rows gray out with a short note. The next tap
    tries again.
  - Control page: opening it joins Wi-Fi. It draws the saved layout and
    states first, then repaints only the cards whose state changed.
  - Wi-Fi stays up for 2 minutes after the last HA activity, then turns off.
- Control page while open: live (subscribe to state changes), repainting
  changed cards with partial refreshes, rate-limited.

## Design

### Config file

`/homeassistant.json` on the SD card (*my call*: the root, so it is easy to
find and edit). Absent or invalid file: the feature is off and nothing new
is shown. An invalid file logs the reason.

```json
{
  "url": "http://homeassistant.local:8123",
  "token": "<long-lived access token>",
  "wifi": "HomeNetwork",
  "sheet": [
    { "entity": "light.reading_lamp", "name": "Reading lamp" },
    { "entity": "light.desk_lamp" },
    { "entity": "scene.evening" }
  ],
  "dashboard": "dashboard-ereader"
}
```

- `url`: base URL, `http://` only.
- `token`: sent as `Authorization: Bearer`. It is written in plain text
  only once. On the first load, the device obfuscates it with
  `obfuscateToBase64()` (`lib/Serialization/ObfuscationUtils.h`, keyed to
  this chip, as the Wi-Fi and OPDS stores do), stores it as `token_obf`
  and rewrites the file without `token`. To change the token, add a new
  `token` field; it replaces the stored one on the next load. A `token_obf`
  that does not decode (file copied from another device) counts as no token.
- `wifi`: the SSID of the home network. It must be a saved network. HA work
  only ever joins this one, directly (no scan).
- `sheet`: entities in display order; `name` overrides the HA name.
  Lights become slider rows; other domains go in the button row.
- `dashboard`: the HA dashboard `url_path`. Omitted: no Control item.

### Pieces

- `lib/HomeAssistant/` (host-tested, no Arduino):
  - config parsing and validation;
  - the dashboard model: HA Lovelace JSON → a list of sections and cards
    the device supports, dropping the rest;
  - state formatting (state + unit, durations like `0:42:00`, "unavailable");
  - the cached-state file format.
- `src/homeassistant/`:
  - `HaLink`: owns the connection. It joins the home network when asked,
    tracks whether it turned Wi-Fi on, and turns it off 2 minutes after the
    last activity, but only if it turned it on and nothing else holds
    Wi-Fi (file transfer, sync, KOReader sync, Developer Mode).
  - REST calls for the sheet (`POST /api/services/<domain>/<service>`,
    `GET /api/states/<entity>`).
  - WebSocket session for the Control page: auth, `lovelace/config`,
    `subscribe_entities` filtered to the dashboard's entities, and the
    weather forecast.
  - The state cache: last known state per entity, saved on the SD card next
    to the config (`/.crosspoint/homeassistant-state.json`), and the last
    dashboard layout.
- Sheet: an HA section under the existing sliders, built from the config
  and the cache.
- `ControlActivity`: the Control page.
- Home menu: the Control item, shown only when the config names a dashboard.

### Supported cards (Control page)

Views of type `sections` (HA's default for new dashboards) and classic
masonry views. Inside them:

| card | drawn as |
|------|----------|
| `heading` | section title |
| `grid`, `vertical-stack`, `horizontal-stack` | containers, laid out in columns that fit the screen |
| `tile`, `button`, `entity` | name, state (with unit), tap action by domain; lights get a slider when the card has the `light-brightness` feature |
| `entities` | rows of name + state; toggleable rows get a toggle |
| `weather-forecast` | current condition and temperature, plus a few forecast days |

Anything else is dropped. Card options beyond the entity, name and the
features above are ignored.

### Behavior details

- **Tap actions**: lights, switches, fans, input booleans, media players and
  remotes → `homeassistant.toggle` or the domain's `toggle`; scenes →
  `scene.turn_on`; scripts → `script.turn_on`; buttons → `button.press`.
  Sensors do nothing.
- **Sliders**: send `light.turn_on` with `brightness_pct` when the drag ends
  or after a step press, not while dragging.
- **Working state**: the tapped control draws a pending look until the call
  returns or times out.
- **Failure**: the network is not reachable, HA does not answer, or the
  token is refused. The sheet grays the HA rows and shows one line ("Home
  network not available", "Home Assistant not responding", "Token
  rejected"). The Control page shows the same line in its header, keeping
  the cached cards.
- **Rate limit**: the Control page repaints at most once every 5 seconds
  (*my call*), batching changes. A tap's own result repaints at once.
- **Leaving**: closing the sheet or page does not turn Wi-Fi off at once;
  the 2-minute idle rule applies. Leaving the Control page closes its
  WebSocket session; nothing listens while the page is closed.
- **Sheet size**: at most 4 light rows and one button row of up to 5
  buttons (*my call*, so the sheet fits on screen); extra entries are
  skipped with a log line.

### Verification

- Host tests for config parsing, dashboard model (supported and dropped
  cards, nesting) and state formatting.
- Simulator: the sheet and Control page with a fake HA (canned REST and
  WebSocket responses), including the failure paths.
- Device: the user's real HA dashboard and sheet list, checking that no
  Wi-Fi join happens before the first tap, that Wi-Fi turns off after
  2 minutes idle, and that Developer Mode's Wi-Fi is never turned off.

## Left to the builder

- Exact layout metrics (row heights, columns per screen width).
- How far the dashboard model goes inside nested stacks (at least two levels).
- Number of forecast days shown (what fits).
- Cache file details and when it is written (after changes, not on every
  update).

## Starter setup (local, not in this repository)

Built during implementation, outside the repo:

- An HA dashboard for the device containing a weather forecast, a few
  sensors, a set of dimmable lights and one on/off device.
- `/homeassistant.json` on the device's SD card, with two room lights in
  the sheet.
