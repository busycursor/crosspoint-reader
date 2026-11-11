#ifdef CROSSPOINT_DEV_API

#include "DevApi.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "SettingsJson.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/settings/SettingsSection.h"
#include "components/themes/BaseTheme.h"
#include "util/DictionaryRegistry.h"
#include "util/GrayToneTracker.h"

#ifdef SIMULATOR
#include <HalSystem.h>
#include <SimDisplayCapture.h>
#else
#include "network/FirmwareFlasher.h"
#endif

#ifndef CROSSPOINT_DEV_API_TOKEN
#define CROSSPOINT_DEV_API_TOKEN ""
#endif

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace dev_api {
namespace {

constexpr uint16_t kPort = 8090;
constexpr size_t kMaxLogLines = 400;
constexpr unsigned long kKeepAwakeMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kWifiRetryMs = 15000;
constexpr size_t kMaxBody = 256 * 1024;  // buffered request bodies; uploads stream to SD
constexpr const char* kUploadPath = "/dev/fs/upload";

void (*sleepFn)() = nullptr;
unsigned long lastRequestMs = 0;

// "API active" badge state. A changing request or a command it queued keeps
// the badge up; operationRunning covers work that blocks the main loop (a
// sync), and kBadgeHoldMs keeps it visible briefly after the last change.
constexpr unsigned long kBadgeHoldMs = 5000;
unsigned long lastChangeMs = 0;
bool operationRunning = false;
bool badgeShown = false;
std::atomic<bool> headerBadgeAllowed{false};  // read by the render task

// ---------------------------------------------------------------------------
// Request/response model shared by the HTTP and serial transports.

struct Request {
  std::string method;  // GET, POST, PUT, DELETE
  std::string path;
  std::map<std::string, std::string> query;
  std::string body;
  bool uploadOk = false;  // set by the transport for kUploadPath

  std::string arg(const char* name) const {
    const auto it = query.find(name);
    return it == query.end() ? std::string() : it->second;
  }
};

struct Response {
  int code = 200;
  std::string type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string streamFile;  // when set, the body is this SD file's content

  void json(int status, const JsonDocument& doc) {
    code = status;
    type = "application/json";
    body.clear();
    serializeJson(doc, body);
  }
  void message(int status, const char* text) {
    JsonDocument doc;
    doc[status >= 400 ? "error" : "status"] = text;
    json(status, doc);
  }
};

bool parseJson(const Request& req, JsonDocument& doc) { return !req.body.empty() && !deserializeJson(doc, req.body); }

std::string normalizedPath(std::string p) {
  if (p.empty() || p[0] != '/') p.insert(p.begin(), '/');
  return p;
}

// ---------------------------------------------------------------------------
// Observers: log lines (any task) and screen text (render task).

// One fixed buffer, in PSRAM when present: per-line heap strings would take
// small internal-RAM blocks that TLS needs contiguous.
constexpr size_t kLogLineMax = 240;
std::mutex logMutex;
char* logRing = nullptr;  // kMaxLogLines slots of kLogLineMax bytes
uint32_t logNextSeq = 0;  // sequence number of the next line

void onLogLine(const char* line) {
  std::lock_guard<std::mutex> lock(logMutex);
  if (!logRing) return;
  char* slot = logRing + (logNextSeq % kMaxLogLines) * kLogLineMax;
  size_t n = strnlen(line, kLogLineMax - 1);
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
  memcpy(slot, line, n);
  slot[n] = '\0';
  ++logNextSeq;
}

std::mutex screenMutex;
std::vector<std::string> screenBuilding;
std::set<std::tuple<int, int, std::string>> screenSeen;  // strips and passes redraw the same text
std::vector<std::string> screenShown;
uint32_t screenSeq = 0;

// Where decorateFrame() last drew the header badge. A frame drawn on top of
// the previous one (a popup) still holds it, so it is erased before the next
// scan; a cleared frame does not.
struct BadgeBox {
  bool valid = false;
  int x = 0, y = 0, size = 0;
};
BadgeBox lastBadge;

void onClear() {
  std::lock_guard<std::mutex> lock(screenMutex);
  screenBuilding.clear();
  screenSeen.clear();
  lastBadge.valid = false;
}
void onText(int x, int y, const char* text) {
  std::lock_guard<std::mutex> lock(screenMutex);
  if (screenSeen.emplace(x, y, text).second) screenBuilding.emplace_back(text);
}
void onDisplay() {
  std::lock_guard<std::mutex> lock(screenMutex);
  // A grayscale pass that draws no text must not blank the text of its page.
  if (!screenBuilding.empty()) screenShown = screenBuilding;
  ++screenSeq;
}
void decorateFrame() {
  if (lastBadge.valid) {
    renderer.fillRect(lastBadge.x - 1, lastBadge.y - 1, lastBadge.size + 2, lastBadge.size + 2, false);
    lastBadge.valid = false;
  }
  if (headerBadgeAllowed.load() && indicatorActive()) {
    lastBadge.valid = BaseTheme::drawApiBadgeInHeader(renderer, lastBadge.x, lastBadge.y, lastBadge.size);
  }
}
// Calibration keeps its pattern on the panel until the user interacts or the
// next changing request arrives; frames are composed but not shown meanwhile.
std::atomic<bool> panelHeld{false};
bool holdPanel() { return panelHeld.load(); }
const GfxScreenTap kScreenTap{onClear, onText, onDisplay, decorateFrame, holdPanel};

// ---------------------------------------------------------------------------
// Commands that must run from loop(), not inside a request.

enum class PendingKind { None, Open, Input, Sleep, Reboot };
struct Pending {
  PendingKind kind = PendingKind::None;
  std::string screen;
  std::string path;
  RemoteButton button = RemoteButton::None;
};
Pending pending;

// Wi-Fi reconnect pacing; reset to 0 to retry immediately.
unsigned long lastWifiAttemptMs = 0;

// ---------------------------------------------------------------------------
// Handlers

void getState(const Request&, Response& res) {
  JsonDocument doc;
  doc["activity"] = activityManager.currentActivityName();
  doc["uptime_ms"] = millis();
  doc["pending"] = pending.kind != PendingKind::None;
  {
    std::lock_guard<std::mutex> lock(screenMutex);
    doc["screen_seq"] = screenSeq;
    JsonArray text = doc["screen"].to<JsonArray>();
    for (const auto& s : screenShown) text.add(s);
  }
  const auto heap = HalMemory::getInternalHeap();
  doc["heap"]["free"] = heap.freeBytes;
  doc["heap"]["largest"] = heap.largestBlockBytes;
#ifdef BOARD_HAS_PSRAM
  const auto psram = HalMemory::getPsramHeap();
  doc["psram"]["free"] = psram.freeBytes;
  doc["psram"]["largest"] = psram.largestBlockBytes;
#endif
  doc["wifi"]["connected"] = WiFi.status() == WL_CONNECTED;
  doc["wifi"]["ssid"] = std::string(WiFi.SSID().c_str());
  doc["wifi"]["ip"] = std::string(WiFi.localIP().toString().c_str());
  doc["log_seq"] = logNextSeq;
  doc["orientation"] = static_cast<int>(renderer.getOrientation());
  res.json(200, doc);
}

void getInfo(const Request&, Response& res) {
  JsonDocument doc;
#ifdef CROSSPOINT_VERSION
  doc["version"] = CROSSPOINT_VERSION;
#endif
  doc["board"] = BoardConfig::ACTIVE.name;
  doc["display"]["width"] = HalDisplay::DISPLAY_WIDTH;
  doc["display"]["height"] = HalDisplay::DISPLAY_HEIGHT;
  if (const char* controller = display.controllerName()) doc["display"]["controller"] = controller;
  // Panel waveform revision (UC8279 LUT_VER): picks the stock gray pulse length.
  char variant[8];
  snprintf(variant, sizeof(variant), "0x%02X", static_cast<unsigned>(BoardConfig::ACTIVE.displayControllerVariant));
  doc["display"]["variant"] = std::string(variant);
  doc["sd"]["total"] = Storage.sdTotalBytes();
  uint64_t used = 0;
  if (Storage.sdUsedBytes(used)) doc["sd"]["used"] = used;
  doc["uptime_ms"] = millis();
#ifdef SIMULATOR
  doc["simulator"] = true;
#endif
  res.json(200, doc);
}

// Raw 1-bit framebuffer in panel order (1 = white, MSB first). The host tool
// rotates it using X-Orientation.
void getScreen(const Request& req, Response& res) {
  int orientation = 0;
#ifdef SIMULATOR
  // ?gray=1: the frame as shown, gray planes included (8 bits per pixel).
  if (req.arg("gray") == "1") {
    res.body.resize(static_cast<size_t>(HalDisplay::DISPLAY_WIDTH) * HalDisplay::DISPLAY_HEIGHT);
    if (!simCopyPresentedGray(reinterpret_cast<uint8_t*>(&res.body[0]), res.body.size())) {
      res.message(503, "no frame yet");
      return;
    }
    res.type = "application/octet-stream";
    res.headers.push_back({"X-Width", std::to_string(HalDisplay::DISPLAY_WIDTH)});
    res.headers.push_back({"X-Height", std::to_string(HalDisplay::DISPLAY_HEIGHT)});
    res.headers.push_back({"X-Orientation", std::to_string(static_cast<int>(renderer.getOrientation()))});
    res.headers.push_back({"X-Format", "gray8"});
    return;
  }
#else
  (void)req;
#endif
  {
    RenderLock lock;
    const uint8_t* buf = display.getFrameBuffer();
    const size_t n = display.getBufferSize();
    if (!buf || n == 0) {
      res.message(503, "no framebuffer");
      return;
    }
    res.body.assign(reinterpret_cast<const char*>(buf), n);
    orientation = static_cast<int>(renderer.getOrientation());
  }
  res.type = "application/octet-stream";
  res.headers.push_back({"X-Width", std::to_string(HalDisplay::DISPLAY_WIDTH)});
  res.headers.push_back({"X-Height", std::to_string(HalDisplay::DISPLAY_HEIGHT)});
  res.headers.push_back({"X-Orientation", std::to_string(orientation)});
}

void getLogs(const Request& req, Response& res) {
  const uint32_t since = strtoul(req.arg("since").c_str(), nullptr, 10);
  JsonDocument doc;
  JsonArray lines = doc["lines"].to<JsonArray>();
  {
    std::lock_guard<std::mutex> lock(logMutex);
    const uint32_t firstSeq = logNextSeq > kMaxLogLines ? logNextSeq - kMaxLogLines : 0;
    for (uint32_t seq = std::max(since, firstSeq); logRing && seq < logNextSeq; ++seq) {
      lines.add(static_cast<const char*>(logRing + (seq % kMaxLogLines) * kLogLineMax));
    }
    doc["next"] = logNextSeq;
    doc["dropped"] = since < firstSeq;
  }
  res.json(200, doc);
}

void postOpen(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body) || !body["screen"].is<const char*>()) {
    res.message(400, "expected {\"screen\": ...}");
    return;
  }
  pending = {PendingKind::Open, body["screen"].as<const char*>(), body["path"] | "", RemoteButton::None};
  res.message(202, "queued");
}

void postInput(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body) || !body["button"].is<const char*>()) {
    res.message(400, "expected {\"button\": confirm|back|next|previous}");
    return;
  }
  const std::string b = body["button"].as<const char*>();
  const RemoteButton button = b == "confirm"    ? RemoteButton::Confirm
                              : b == "back"     ? RemoteButton::Back
                              : b == "next"     ? RemoteButton::Next
                              : b == "previous" ? RemoteButton::Previous
                                                : RemoteButton::None;
  if (button == RemoteButton::None) {
    res.message(400, "unknown button");
    return;
  }
  pending = {PendingKind::Input, "", "", button};
  res.message(202, "queued");
}

// --- settings: the same JSON as the web settings page ---

void getSettings(const Request& req, Response& res) {
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);
  const std::string keys = "," + req.arg("keys") + ",";
  const bool all = keys == ",,";
  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  JsonDocument one;
  forEachSetting(
      &sdFontSystem.registry(), &dictionaries,
      [&](const char* key) { return key && (all || keys.find("," + std::string(key) + ",") != std::string::npos); },
      [&](const SettingInfo& s) {
        one.clear();
        if (settingToJson(s, one)) arr.add(one);
      });
  res.json(200, out);
}

void postSettings(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body) || !body.is<JsonObject>()) {
    res.message(400, "expected {\"key\": value, ...}");
    return;
  }
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);
  JsonDocument out;
  JsonArray applied = out["applied"].to<JsonArray>();
  JsonArray rejected = out["rejected"].to<JsonArray>();
  for (const auto& s : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (!s.key || !body[s.key].is<JsonVariant>()) continue;
    (applySettingJson(s, body[s.key]) ? applied : rejected).add(s.key);
    body.remove(s.key);
  }
  for (JsonPairConst kv : body.as<JsonObjectConst>()) rejected.add(kv.key().c_str());
  if (applied.size() > 0) SETTINGS.saveToFile();
  res.json(200, out);
}

// --- frontlight ---

void frontlightJson(JsonDocument& doc) {
  doc["present"] = Frontlight.present();
  doc["color_temperature"] = Frontlight.hasColorTemperature();
  doc["on"] = Frontlight.isOn();
  doc["brightness"] = Frontlight.brightness();
  doc["warmth"] = Frontlight.warmth();
}

void getFrontlight(const Request&, Response& res) {
  JsonDocument doc;
  frontlightJson(doc);
  res.json(200, doc);
}

void postFrontlight(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body)) {
    res.message(400, "expected {on?, brightness?, warmth?}");
    return;
  }
  if (!Frontlight.present()) {
    res.message(409, "no frontlight on this board");
    return;
  }
  if (body["brightness"].is<int>()) {
    const uint8_t v = static_cast<uint8_t>(std::clamp(body["brightness"].as<int>(), 0, 100));
    Frontlight.setBrightness(v);
    SETTINGS.frontlightBrightness = v;
  }
  if (body["warmth"].is<int>()) {
    const uint8_t v = static_cast<uint8_t>(std::clamp(body["warmth"].as<int>(), 0, 100));
    Frontlight.setWarmth(v);
    SETTINGS.frontlightWarmth = v;
  }
  if (body["on"].is<bool>()) {
    Frontlight.setOn(body["on"].as<bool>());
    SETTINGS.frontlightOn = body["on"].as<bool>() ? 1 : 0;
  }
  SETTINGS.saveToFile();
  JsonDocument doc;
  frontlightJson(doc);
  res.json(200, doc);
}

// --- power ---

void getPower(const Request&, Response& res) {
  JsonDocument doc;
  doc["battery_percent"] = powerManager.getBatteryPercentage();
  doc["usb_connected"] = gpio.isUsbConnected();
  doc["sleep_timeout_ms"] = SETTINGS.getSleepTimeoutMs();
  doc["api_keeps_awake_ms"] = kKeepAwakeMs;
  res.json(200, doc);
}

void postPower(const Request& req, Response& res) {
  JsonDocument body;
  const std::string action = parseJson(req, body) ? std::string(body["action"] | "") : std::string();
  if (action == "sleep") {
    pending = {PendingKind::Sleep};
    res.message(202, "sleeping; wake with the power button (the API is off while asleep)");
  } else if (action == "reboot") {
    pending = {PendingKind::Reboot};
    res.message(202, "rebooting");
  } else {
    res.message(400, "expected {\"action\": sleep|reboot}");
  }
}

// --- display ---

void postDisplay(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body) || body["refresh"] != "full") {
    res.message(400, "expected {\"refresh\": \"full\"}; orientation is the \"orientation\" setting");
    return;
  }
  {
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
  res.message(200, "full refresh done");
}

// --- Wi-Fi ---

void getWifi(const Request&, Response& res) {
  JsonDocument doc;
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["ssid"] = std::string(WiFi.SSID().c_str());
  doc["ip"] = std::string(WiFi.localIP().toString().c_str());
  doc["rssi"] = WiFi.RSSI();
  doc["api_port"] = kPort;
  JsonArray saved = doc["saved"].to<JsonArray>();
  for (const auto& c : WIFI_STORE.getCredentialSummaries()) saved.add(c.ssid);
  doc["last_ssid"] = WIFI_STORE.getLastConnectedSsid();
  res.json(200, doc);
}

// Save a network and switch to it. Usually sent over USB serial right after
// flashing, so a new device never needs its Wi-Fi screen.
void postWifi(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body) || !body["ssid"].is<const char*>()) {
    res.message(400, "expected {ssid, password}");
    return;
  }
  const std::string ssid = body["ssid"].as<const char*>();
  if (!WIFI_STORE.addCredential(ssid, body["password"] | "")) {
    res.message(500, "could not save network");
    return;
  }
  WIFI_STORE.setLastConnectedSsid(ssid);
  WiFi.disconnect(false);
  lastWifiAttemptMs = 0;
  res.message(200, "saved; connecting");
}

// --- OPDS servers ---

void getOpds(const Request&, Response& res) {
  JsonDocument doc;
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& s : OPDS_STORE.getServers()) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    o["url"] = s.url;
    o["username"] = s.username;
    o["has_password"] = !s.password.empty();
    JsonArray headers = o["headers"].to<JsonArray>();
    for (const auto& h : s.customHeaders) {
      if (!h.name.empty()) headers.add(h.name);
    }
  }
  res.json(200, doc);
}

// Add or replace (by name) one OPDS server. Secrets go through the store,
// which obfuscates them on disk.
void postOpds(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body) || !body["name"].is<const char*>() || !body["url"].is<const char*>()) {
    res.message(400, "expected {name, url, username?, password?, headers?: [{name, value}]}");
    return;
  }
  OpdsServer s;
  s.name = body["name"].as<const char*>();
  s.url = body["url"].as<const char*>();
  s.username = body["username"] | "";
  s.password = body["password"] | "";
  size_t i = 0;
  for (JsonObjectConst h : body["headers"].as<JsonArrayConst>()) {
    if (i >= OpdsServer::MAX_CUSTOM_HEADERS) break;
    s.customHeaders[i].name = h["name"] | "";
    s.customHeaders[i].value = h["value"] | "";
    ++i;
  }
  const auto& servers = OPDS_STORE.getServers();
  size_t idx = 0;
  while (idx < servers.size() && servers[idx].name != s.name) ++idx;
  const bool ok = idx < servers.size() ? OPDS_STORE.updateServer(idx, s) : OPDS_STORE.addServer(s);
  if (!ok || !OPDS_STORE.saveToFile()) {
    res.message(500, "could not save server");
    return;
  }
  res.message(200, "saved");
}

// --- SD card files ---

void getFs(const Request& req, Response& res) {
  const std::string path = normalizedPath(req.arg("path"));
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    res.message(404, "not found");
    return;
  }
  if (!f.isDirectory()) {
    f.close();
    res.type = "application/octet-stream";
    res.streamFile = path;
    return;
  }
  JsonDocument doc;
  JsonArray entries = doc["entries"].to<JsonArray>();
  char name[256];
  for (HalFile e = f.openNextFile(); e; e = f.openNextFile()) {
    e.getName(name, sizeof(name));
    JsonObject o = entries.add<JsonObject>();
    o["name"] = std::string(name);
    o["dir"] = e.isDirectory();
    if (!e.isDirectory()) o["size"] = e.fileSize64();
    e.close();
  }
  f.close();
  res.json(200, doc);
}

void putFs(const Request& req, Response& res) {
  const std::string path = normalizedPath(req.arg("path"));
  const size_t slash = path.rfind('/');
  if (slash > 0) Storage.ensureDirectoryExists(path.substr(0, slash).c_str());
  if (!Storage.writeFile(path.c_str(), String(req.body.c_str()))) {
    res.message(500, "write failed");
    return;
  }
  res.message(200, "written");
}

void deleteFs(const Request& req, Response& res) {
  const std::string path = normalizedPath(req.arg("path"));
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    res.message(404, "not found");
    return;
  }
  const bool dir = f.isDirectory();
  f.close();
  const bool ok = dir ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str());
  if (ok) library::markLibraryIndexDirty();  // the Library caches what is on the card
  ok ? res.message(200, "deleted") : res.message(500, "delete failed");
}

// The transport has already streamed the body into the file.
void postFsUpload(const Request& req, Response& res) {
  if (req.uploadOk) library::markLibraryIndexDirty();
  req.uploadOk ? res.message(200, "uploaded") : res.message(500, "upload failed");
}

// --- waveform experiments (UC8279; see WaveformExperiment.h) ---

bool directGrayOn = false;

#ifndef SIMULATOR
bool bankFromName(const std::string& name, freeink::ExperimentBank& out) {
  if (name == "overlay")
    out = freeink::ExperimentBank::Overlay;
  else if (name == "settle")
    out = freeink::ExperimentBank::Settle;
  else if (name == "direct")
    out = freeink::ExperimentBank::Direct;
  else
    return false;
  return true;
}
#endif

// {"bank": "overlay|settle|direct", "tables": {"0": [bytes...], ...}}; keys are
// LUT register offsets from 0x20. All tables are checked before any is loaded.
void postWaveform(const Request& req, Response& res) {
#ifdef SIMULATOR
  (void)req;
  res.message(501, "the simulator has no panel waveforms");
#else
  JsonDocument body;
  freeink::ExperimentBank bank;
  if (!parseJson(req, body) || !bankFromName(body["bank"] | "", bank) || !body["tables"].is<JsonObject>()) {
    res.message(400, "expected {bank: overlay|settle|direct, tables: {\"0\": [bytes], ...}}");
    return;
  }
  const size_t want = bank == freeink::ExperimentBank::Settle ? 42 : 49;
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> tables;
  for (JsonPairConst kv : body["tables"].as<JsonObjectConst>()) {
    const int index = atoi(kv.key().c_str());
    JsonArrayConst arr = kv.value().as<JsonArrayConst>();
    if (index < 0 || index > 4 || arr.size() != want) {
      res.message(400, "table keys 0..4, each with exactly 42 (settle) or 49 bytes");
      return;
    }
    std::vector<uint8_t> bytes;
    for (JsonVariantConst v : arr) bytes.push_back(static_cast<uint8_t>(v.as<int>()));
    tables.emplace_back(static_cast<uint8_t>(index), std::move(bytes));
  }
  JsonDocument doc;
  JsonArray loaded = doc["loaded"].to<JsonArray>();
  for (const auto& t : tables) {
    if (!display.setExperimentLut(bank, t.first, t.second.data(), t.second.size())) {
      res.message(409, "this display controller does not take experiment tables");
      return;
    }
    loaded.add(t.first);
  }
  LOG_INF("DEV", "Waveform: %u table(s) loaded", static_cast<unsigned>(tables.size()));
  res.json(200, doc);
#endif
}

// Back to stock: drop every experiment table and switch the experiment off.
void deleteWaveform(const Request&, Response& res) {
  directGrayOn = false;
#ifndef SIMULATOR
  display.clearExperimentLuts();
#endif
  LOG_INF("DEV", "Waveform: back to stock");
  res.message(200, "stock waveforms restored");
}

// {"direct": bool, "fullClear": bool, "codes": {"hold": 0..3, "white": .., "black": .., "gray": ..}}
// Codes are the calibrated 2-bit plane values for each action.
void postDisplayExperiment(const Request& req, Response& res) {
  JsonDocument body;
  if (!parseJson(req, body)) {
    res.message(400, "expected {direct?, fullClear?, codes?}");
    return;
  }
  if (body["codes"].is<JsonObject>()) {
    waveform_actions::ActionCodes c = gray_tracker::codes();
    c.hold = static_cast<uint8_t>((body["codes"]["hold"] | c.hold) & 3);
    c.toWhite = static_cast<uint8_t>((body["codes"]["white"] | c.toWhite) & 3);
    c.toBlack = static_cast<uint8_t>((body["codes"]["black"] | c.toBlack) & 3);
    c.toGray = static_cast<uint8_t>((body["codes"]["gray"] | c.toGray) & 3);
    if ((1 << c.hold | 1 << c.toWhite | 1 << c.toBlack | 1 << c.toGray) != 0xF) {
      res.message(400, "the four actions need four different codes");
      return;
    }
    gray_tracker::setCodes(c);
  }
  if (body["direct"].is<bool>()) {
    directGrayOn = body["direct"].as<bool>();
    gray_tracker::invalidate();  // start from a recorded stock page
  }
#ifndef SIMULATOR
  if (body["fullClear"].is<bool>()) display.setDirectFullClear(body["fullClear"].as<bool>());
#endif
  LOG_INF("DEV", "Waveform experiment: direct=%d", directGrayOn ? 1 : 0);
  JsonDocument doc;
  doc["direct"] = directGrayOn;
  const auto& c = gray_tracker::codes();
  doc["codes"]["hold"] = c.hold;
  doc["codes"]["white"] = c.toWhite;
  doc["codes"]["black"] = c.toBlack;
  doc["codes"]["gray"] = c.toGray;
  res.json(200, doc);
}

// Calibration: a known panel (half black, half white, via a full refresh),
// then ONE Direct pass whose two planes hold four bands, band k carrying code
// k (plane A = bit 0, plane B = bit 1). Each band shows what the table for
// that code does from black and from white. Panel coordinates: bands split the
// 800-pixel width, the 480-pixel height is black above 240 and white below.
// In portrait the bands run top to bottom (code 0 first); the white start is
// on the left, the black start on the right.
void postCalibrate(const Request&, Response& res) {
#ifdef SIMULATOR
  res.message(501, "the simulator has no panel waveforms");
#else
  const uint16_t w = HalDisplay::DISPLAY_WIDTH, h = HalDisplay::DISPLAY_HEIGHT, wb = w / 8;
  const size_t bytes = static_cast<size_t>(wb) * h;
  uint8_t* planeA = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  uint8_t* planeB = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  if (!planeA || !planeB) {
    heap_caps_free(planeA);
    heap_caps_free(planeB);
    res.message(500, "no PSRAM for calibration planes");
    return;
  }
  {
    RenderLock lock;
    uint8_t* fb = display.getFrameBuffer();
    for (uint16_t y = 0; y < h; ++y) memset(fb + static_cast<size_t>(y) * wb, y < h / 2 ? 0x00 : 0xFF, wb);
    for (uint16_t y = 0; y < h; ++y) {
      for (uint16_t xb = 0; xb < wb; ++xb) {
        const int code = (xb * 8) / (w / 4);
        planeA[static_cast<size_t>(y) * wb + xb] = (code & 1) ? 0xFF : 0x00;
        planeB[static_cast<size_t>(y) * wb + xb] = (code & 2) ? 0xFF : 0x00;
      }
    }
    display.displayBuffer(HalDisplay::FULL_REFRESH);
    if (!display.displayGrayscaleBase(HalDisplay::GrayscaleMode::Direct, HalDisplay::FAST_REFRESH)) {
      heap_caps_free(planeA);
      heap_caps_free(planeB);
      res.message(409, "this display has no Direct mode");
      return;
    }
    display.copyGrayscaleBuffers(planeA, planeB);
    display.displayGrayBuffer();
    display.cleanupGrayscaleBuffers(fb);
  }
  heap_caps_free(planeA);
  heap_caps_free(planeB);
  gray_tracker::invalidate();
  panelHeld.store(true);
  LOG_INF("DEV", "Calibration pattern shown; panel held until input");
  res.message(200, "pattern shown; restore with a full refresh and reopen a screen");
#endif
}

void getDisplayTimings(const Request&, Response& res) {
  JsonDocument doc;
  doc["direct"] = directGrayOn;
  JsonArray arr = doc["refreshes"].to<JsonArray>();
#ifndef SIMULATOR
  freeink::RefreshTimings t[6];
  const size_t n = display.recentRefreshTimings(t, 6);
  for (size_t i = 0; i < n; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["pass"] = t[i].pass ? t[i].pass : "";
    o["at_ms"] = t[i].atMs;
    o["stream_us"] = t[i].streamUs;
    o["lut_us"] = t[i].lutUs;
    o["power_on_us"] = t[i].powerOnUs;
    o["refresh_us"] = t[i].refreshUs;
    o["power_off_us"] = t[i].powerOffUs;
    o["total_us"] = t[i].totalUs;
  }
#endif
  res.json(200, doc);
}

// --- firmware ---

void postFlash(const Request& req, Response& res) {
#ifdef SIMULATOR
  (void)req;
  res.message(501, "the simulator cannot flash; rebuild it instead");
#else
  JsonDocument body;
  const std::string path = normalizedPath(parseJson(req, body) ? std::string(body["path"] | "") : std::string());
  if (path == "/" || !Storage.exists(path.c_str())) {
    res.message(400, "expected {\"path\": \"/firmware.bin\"} of an uploaded image");
    return;
  }
  LOG_INF("DEV", "Flashing %s", path.c_str());
  const auto result = firmware_flash::flashFromSdPath(path.c_str(), nullptr, nullptr);
  if (result != firmware_flash::Result::OK) {
    JsonDocument doc;
    doc["error"] = "flash failed";
    doc["result"] = static_cast<int>(result);
    res.json(500, doc);
    return;
  }
  pending = {PendingKind::Reboot};
  res.message(200, "flashed; rebooting into the new image");
#endif
}

// ---------------------------------------------------------------------------
// Routing

using Handler = void (*)(const Request&, Response&);
struct Route {
  const char* method;
  const char* path;
  Handler handler;
};
const Route kRoutes[] = {
    {"GET", "/dev/state", getState},
    {"GET", "/dev/info", getInfo},
    {"GET", "/dev/screen", getScreen},
    {"GET", "/dev/logs", getLogs},
    {"POST", "/dev/open", postOpen},
    {"POST", "/dev/input", postInput},
    {"GET", "/dev/settings", getSettings},
    {"POST", "/dev/settings", postSettings},
    {"GET", "/dev/frontlight", getFrontlight},
    {"POST", "/dev/frontlight", postFrontlight},
    {"GET", "/dev/power", getPower},
    {"POST", "/dev/power", postPower},
    {"POST", "/dev/display", postDisplay},
    {"GET", "/dev/wifi", getWifi},
    {"POST", "/dev/wifi", postWifi},
    {"GET", "/dev/opds", getOpds},
    {"POST", "/dev/opds", postOpds},
    {"GET", "/dev/fs", getFs},
    {"PUT", "/dev/fs", putFs},
    {"DELETE", "/dev/fs", deleteFs},
    {"POST", kUploadPath, postFsUpload},
    {"POST", "/dev/flash", postFlash},
    {"POST", "/dev/display/waveform", postWaveform},
    {"DELETE", "/dev/display/waveform", deleteWaveform},
    {"POST", "/dev/display/experiment", postDisplayExperiment},
    {"GET", "/dev/display/timings", getDisplayTimings},
    {"POST", "/dev/display/calibrate", postCalibrate},
};

void dispatch(const Request& req, Response& res) {
  lastRequestMs = millis();
  if (req.method != "GET") lastChangeMs = lastRequestMs | 1;
  if (req.method != "GET" && req.path != "/dev/display/calibrate" && panelHeld.exchange(false)) {
    activityManager.requestUpdate(true);
  }
  for (const auto& r : kRoutes) {
    if (req.path == r.path && req.method == r.method) {
      r.handler(req, res);
      return;
    }
  }
  res.message(404, "no such endpoint");
}

// Shared by both transports: create the upload target.
bool openUploadTarget(const std::string& rawPath, HalFile& file) {
  const std::string path = normalizedPath(rawPath);
  const size_t slash = path.rfind('/');
  if (slash > 0) Storage.ensureDirectoryExists(path.substr(0, slash).c_str());
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  return Storage.openFileForWrite("DEV", path, file);
}

// ---------------------------------------------------------------------------
// HTTP transport (port 8090, bearer token)

std::unique_ptr<WebServer> server;
bool serverStarted = false;
HalFile httpUpload;
bool httpUploadOk = false;

bool httpAuthorized() {
  const char* token = CROSSPOINT_DEV_API_TOKEN;
  if (token[0] == '\0') return false;
  const String header = server->header("Authorization");
  if (header.startsWith("Bearer ") && header.substring(7) == token) return true;
  return server->hasArg("token") && server->arg("token") == token;
}

const char* methodName(int m) {
  switch (m) {
    case HTTP_GET:
      return "GET";
    case HTTP_POST:
      return "POST";
    case HTTP_PUT:
      return "PUT";
    case HTTP_DELETE:
      return "DELETE";
    default:
      return "?";
  }
}

void sendHttp(const Response& res) {
  for (const auto& h : res.headers) server->sendHeader(h.first.c_str(), h.second.c_str());
  if (!res.streamFile.empty()) {
    HalFile f;
    if (!Storage.openFileForRead("DEV", res.streamFile, f)) {
      server->send(500, "text/plain", "open failed");
      return;
    }
    server->setContentLength(f.fileSize());
    server->send(200, res.type.c_str(), "");
    uint8_t chunk[2048];
    int n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) server->sendContent(reinterpret_cast<const char*>(chunk), n);
    f.close();
    return;
  }
  server->setContentLength(res.body.size());
  server->send(res.code, res.type.c_str(), "");
  if (!res.body.empty()) server->sendContent(res.body.data(), res.body.size());
}

void handleHttp() {
  Response res;
  if (!httpAuthorized()) {
    res.message(401, "unauthorized");
    sendHttp(res);
    return;
  }
  Request req;
  req.method = methodName(server->method());
  req.path = server->uri().c_str();
  for (int i = 0; i < server->args(); ++i) {
    const String name = server->argName(i);
    if (name == "plain") continue;
    req.query[name.c_str()] = server->arg(i).c_str();
  }
  if (server->hasArg("plain")) {
    const String body = server->arg("plain");
    if (body.length() > kMaxBody) {
      res.message(413, "body too large; use /dev/fs/upload");
      sendHttp(res);
      return;
    }
    req.body.assign(body.c_str(), body.length());
  }
  req.uploadOk = httpUploadOk;
  httpUploadOk = false;  // one upload per request
  dispatch(req, res);
  sendHttp(res);
}

void handleHttpUploadData() {
  HTTPUpload& up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    httpUploadOk = httpAuthorized() && openUploadTarget(server->arg("path").c_str(), httpUpload);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (httpUploadOk && httpUpload.write(up.buf, up.currentSize) != up.currentSize) httpUploadOk = false;
  } else {
    if (httpUpload.isOpen()) httpUpload.close();
    if (up.status == UPLOAD_FILE_ABORTED) httpUploadOk = false;
  }
}

HTTPMethod httpMethod(const char* m) {
  return strcmp(m, "POST") == 0     ? HTTP_POST
         : strcmp(m, "PUT") == 0    ? HTTP_PUT
         : strcmp(m, "DELETE") == 0 ? HTTP_DELETE
                                    : HTTP_GET;
}

void startServer() {
  server.reset(new WebServer(kPort));
  static const char* kHeaders[] = {"Authorization"};
  server->collectHeaders(kHeaders, 1);
  // Each route is registered so the server parses its body; all of them share
  // handleHttp(), which dispatches through kRoutes.
  for (const auto& r : kRoutes) {
    if (strcmp(r.path, kUploadPath) == 0) {
      server->on(r.path, HTTP_POST, handleHttp, handleHttpUploadData);
    } else {
      server->on(r.path, httpMethod(r.method), handleHttp);
    }
  }
  server->onNotFound(handleHttp);  // answers 404 (or 401) through the same path
  server->begin();
  serverStarted = true;
  LOG_INF("DEV", "Dev API listening on %s:%u", WiFi.localIP().toString().c_str(), kPort);
}

// ---------------------------------------------------------------------------
// Serial transport (USB). Physical access is the credential, so no token.
//
// Host -> device:  @@Q <id> <METHOD> <path[?query]> <bodyLength>
//                  @@B <id> <base64>          (repeated until bodyLength bytes)
// Device -> host:  @@A <id> <bytesReceived>   (after each body line; hosts keep
//                                             only a few lines in flight)
//  @@R <id> <code> <bodyLength> <content-type> [name=value&...]
//                  @@B <id> <base64>          (repeated)
//                  @@E <id>
// Ordinary log lines are interleaved; hosts ignore lines not starting "@@".

constexpr size_t kSerialLineMax = 4096;
constexpr size_t kSerialChunk = 384;  // raw bytes per @@B line

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t n =
        (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < len ? data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += i + 1 < len ? kB64[(n >> 6) & 63] : '=';
    out += i + 2 < len ? kB64[n & 63] : '=';
  }
  return out;
}

bool b64decode(const char* s, size_t len, std::string& out) {
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c == '=') break;
    const char* p = c ? strchr(kB64, c) : nullptr;
    if (!p) return false;
    acc = (acc << 6) | static_cast<uint32_t>(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string serialLine;
struct SerialRequest {
  bool active = false;
  std::string id;
  Request req;
  size_t expected = 0;
  size_t received = 0;
  HalFile upload;  // kUploadPath bodies stream here instead of into req.body
};
SerialRequest sreq;

void serialWrite(const std::string& line) {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && !defined(SIMULATOR)
  // The log path uses a 1 ms send timeout and drops on a full buffer; framed
  // lines wait for room instead, so a response is never cut short.
  const unsigned long start = millis();
  while (logSerial.availableForWrite() < static_cast<int>(line.size()) && millis() - start < 1000) delay(1);
#endif
  // One write per line: the serial driver keeps each write call whole, so
  // log lines from other tasks interleave between lines, not inside them.
  logSerial.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
}

void sendSerialChunks(const std::string& id, const uint8_t* data, size_t len) {
  for (size_t off = 0; off < len; off += kSerialChunk) {
    const size_t n = std::min(kSerialChunk, len - off);
    serialWrite("@@B " + id + " " + b64encode(data + off, n) + "\n");
  }
}

void sendSerial(const std::string& id, const Response& res) {
  std::string headers;
  for (const auto& h : res.headers) headers += (headers.empty() ? "" : "&") + h.first + "=" + h.second;
  HalFile f;
  size_t len = res.body.size();
  if (!res.streamFile.empty()) {
    if (!Storage.openFileForRead("DEV", res.streamFile, f)) {
      Response err;
      err.message(500, "open failed");
      sendSerial(id, err);
      return;
    }
    len = f.fileSize();
  }
  serialWrite("@@R " + id + " " + std::to_string(res.code) + " " + std::to_string(len) + " " + res.type +
              (headers.empty() ? "" : " " + headers) + "\n");
  if (f.isOpen()) {
    uint8_t chunk[kSerialChunk];
    int n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) sendSerialChunks(id, chunk, n);
    f.close();
  } else {
    sendSerialChunks(id, reinterpret_cast<const uint8_t*>(res.body.data()), res.body.size());
  }
  serialWrite("@@E " + id + "\n");
}

void finishSerialRequest() {
  if (sreq.upload.isOpen()) sreq.upload.close();
  Response res;
  dispatch(sreq.req, res);
  sendSerial(sreq.id, res);
  sreq = SerialRequest{};
}

void startSerialRequest(const std::string& line) {
  // @@Q <id> <METHOD> <target> <len>
  char id[32], method[16], target[1024];
  unsigned long len = 0;
  if (sscanf(line.c_str(), "@@Q %31s %15s %1023s %lu", id, method, target, &len) != 4) return;
  if (sreq.upload.isOpen()) sreq.upload.close();
  sreq = SerialRequest{};
  sreq.active = true;
  sreq.id = id;
  sreq.req.method = method;
  const std::string t = target;
  const size_t q = t.find('?');
  sreq.req.path = t.substr(0, q);
  if (q != std::string::npos) {
    size_t pos = q + 1;
    while (pos <= t.size()) {
      size_t amp = t.find('&', pos);
      if (amp == std::string::npos) amp = t.size();
      const std::string kv = t.substr(pos, amp - pos);
      const size_t eq = kv.find('=');
      if (!kv.empty()) {
        sreq.req.query[urlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1));
      }
      pos = amp + 1;
    }
  }
  sreq.expected = len;
  if (sreq.req.path == kUploadPath) {
    sreq.req.uploadOk = openUploadTarget(sreq.req.arg("path"), sreq.upload);
  } else if (len > kMaxBody) {
    Response res;
    res.message(413, "body too large; use /dev/fs/upload");
    sendSerial(sreq.id, res);
    sreq = SerialRequest{};
    return;
  }
  if (len == 0) finishSerialRequest();
}

void handleSerialBodyLine(const std::string& line) {
  // @@B <id> <base64>
  const size_t sp = line.find(' ', 4);
  if (sp == std::string::npos || line.compare(4, sp - 4, sreq.id) != 0) return;
  std::string chunk;
  if (!b64decode(line.data() + sp + 1, line.size() - sp - 1, chunk)) {
    sreq.req.uploadOk = false;
    return;
  }
  sreq.received += chunk.size();
  serialWrite("@@A " + sreq.id + " " + std::to_string(sreq.received) + "\n");
  if (sreq.upload.isOpen()) {
    if (sreq.upload.write(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size()) != chunk.size()) {
      sreq.req.uploadOk = false;
    }
  } else if (sreq.req.path != kUploadPath) {
    sreq.req.body += chunk;
  }
  if (sreq.received >= sreq.expected) finishSerialRequest();
}

void pollSerial() {
  while (logSerial.available() > 0) {
    const int c = logSerial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c != '\n') {
      if (serialLine.size() < kSerialLineMax) serialLine += static_cast<char>(c);
      continue;
    }
    std::string line;
    line.swap(serialLine);
    if (line.rfind("@@Q ", 0) == 0) {
      startSerialRequest(line);
    } else if (line.rfind("@@B ", 0) == 0 && sreq.active) {
      handleSerialBodyLine(line);
    }
  }
}

// ---------------------------------------------------------------------------
// Developer Mode switching

bool modeWasOn = false;

// Screens that bring Wi-Fi up for themselves; turning the mode off leaves
// their connection alone.
bool networkScreenInFront() {
  const std::string name = activityManager.currentActivityName();
  for (const char* n : {"CrossPointWebServer", "NetworkModeSelection", "WifiSelection", "CalibreConnect",
                        "OpdsBookBrowser", "PluginCatalog", "ShelfSync", "KOReaderSync"}) {
    if (name == n) return true;
  }
  return false;
}

void onModeChanged(const bool on) {
  LOG_INF("DEV", "Developer Mode %s", on ? "on" : "off");
  if (on) {
    lastWifiAttemptMs = 0;  // connect right away
    return;
  }
  if (serverStarted) {
    server->stop();
    server.reset();
    serverStarted = false;
  }
  if (!networkScreenInFront() && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi upkeep

// Several activities turn Wi-Fi off on exit; bring it back unless the user is
// choosing a network.
void keepWifiUp() {
  if (WiFi.status() == WL_CONNECTED || activityManager.currentActivityName() == "WifiSelection") return;
  const unsigned long now = millis();
  if (lastWifiAttemptMs != 0 && now - lastWifiAttemptMs < kWifiRetryMs) return;
  lastWifiAttemptMs = now;
#ifdef SIMULATOR
  WiFi.mode(WIFI_STA);
  WiFi.begin();
#else
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  auto cred = ssid.empty() ? WIFI_STORE.getCredentialAt(0) : WIFI_STORE.findCredential(ssid);
  if (!cred) return;  // send POST /dev/wifi over USB serial
  LOG_INF("DEV", "Dev API: connecting to %s", cred->ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // modem power save between requests
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
#endif
}

}  // namespace

void begin(void (*enterSleep)()) {
  sleepFn = enterSleep;
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && !defined(SIMULATOR)
  logSerial.setRxBufferSize(4096);  // room for several framed body lines
#endif
  logRing = static_cast<char*>(heap_caps_malloc(kMaxLogLines * kLogLineMax, MALLOC_CAP_SPIRAM));
  if (!logRing) LOG_ERR("DEV", "No PSRAM for the log ring; /dev/logs is empty");
  logLineTap = onLogLine;
  // CrossPoint loads saved networks only when a Wi-Fi screen opens; the
  // reconnect in keepWifiUp() needs them from boot.
  WIFI_STORE.loadFromFile();
  modeWasOn = enabled();
  LOG_INF("DEV", "Developer Mode %s", modeWasOn ? "on" : "off");
  gfxScreenTap = &kScreenTap;
  if (CROSSPOINT_DEV_API_TOKEN[0] == '\0') LOG_ERR("DEV", "Dev API built without a token; HTTP requests refused");
}

// Show or clear the badge when its state changes. Readers draw it in their
// status bar; other screens get it beside the header battery at display time.
void updateBadge() {
  if (operationRunning) {  // the main loop is back: whatever it ran has finished
    operationRunning = false;
    lastChangeMs = millis() | 1;
  }
  const std::string name = activityManager.currentActivityName();
  headerBadgeAllowed.store(name.find("Reader") == std::string::npos && name != "Sleep" && name != "Boot" &&
                           !name.empty());
  const bool active = indicatorActive();
  if (active != badgeShown) {
    badgeShown = active;
    activityManager.requestUpdate();
  }
}

void loop() {
  if (panelHeld.load() && (gpio.wasAnyPressed() || gpio.wasTouchActivity())) {
    panelHeld.store(false);
    LOG_INF("DEV", "Panel hold released by input");
    activityManager.requestUpdate(true);
  }
  updateBadge();
  pollSerial();  // always: a cable can switch Developer Mode on
  const bool on = enabled();
  if (on != modeWasOn) {
    modeWasOn = on;
    onModeChanged(on);
  }
  if (!on) return;
  keepWifiUp();
  if (!serverStarted) {
    if (WiFi.status() != WL_CONNECTED) return;
    startServer();
  }
  server->handleClient();
}

void applyPending() {
  const Pending cmd = pending;
  pending = {};
  if (cmd.kind != PendingKind::None) {
    operationRunning = true;
    lastChangeMs = millis() | 1;
  }
  switch (cmd.kind) {
    case PendingKind::None:
      return;
    case PendingKind::Input: {
      static const char* const kNames[] = {"none", "confirm", "back", "next", "previous"};
      const char* name = kNames[static_cast<int>(cmd.button)];
      if (mappedInputManager.injectRemoteClick(cmd.button)) {
        LOG_INF("DEV", "Input %s", name);
      } else {
        LOG_INF("DEV", "Input %s dropped: real input this frame", name);
      }
      return;
    }
    case PendingKind::Sleep:
      if (sleepFn) sleepFn();
      return;
    case PendingKind::Reboot:
      LOG_INF("DEV", "Rebooting");
      delay(100);
#ifdef SIMULATOR
      HalSystem::restart();
#else
      ESP.restart();
#endif
      return;
    case PendingKind::Open:
      break;
  }
  LOG_INF("DEV", "Open %s %s", cmd.screen.c_str(), cmd.path.c_str());
  const std::string& s = cmd.screen;
  if (s == "home")
    activityManager.goHome();
  else if (s == "sync")
    activityManager.goToShelfSync();
  else if (s == "settings") {
    // path picks a section: general, display, reader, controls, library, network
    static const char* const kSections[] = {"general", "display", "reader", "controls", "library", "network"};
    SettingsSection section = SettingsSection::End;
    for (size_t i = 0; i < std::size(kSections); ++i) {
      if (cmd.path == kSections[i]) section = static_cast<SettingsSection>(i);
    }
    activityManager.goToSettings(section);
  } else if (s == "library")
    activityManager.goToLibrary();
  else if (s == "files")
    activityManager.goToFileBrowser(cmd.path);
  else if (s == "transfer")
    activityManager.goToFileTransfer();
  else if (s == "control")
    activityManager.goToControl();
  else if (s == "more")
    activityManager.goToMore();
  else if (s == "opds")
    activityManager.goToBrowser();
  else if (s == "reader" && !cmd.path.empty())
    activityManager.goToReader(cmd.path);
  else if (s == "percent") {  // path = 0..100, in the open book
    if (!activityManager.devReaderJumpToPercent(atoi(cmd.path.c_str()))) LOG_ERR("DEV", "percent: no book open");
  } else if (s == "kosync") {
    if (!activityManager.devRunReaderSync()) LOG_ERR("DEV", "kosync: no book open");
  } else
    LOG_ERR("DEV", "Unknown screen: %s", s.c_str());
}

bool keepAwake() {
  return panelHeld.load() || (enabled() && lastRequestMs != 0 && millis() - lastRequestMs < kKeepAwakeMs);
}

bool enabled() { return SETTINGS.developerMode != 0; }

bool directGrayExperiment() { return enabled() && directGrayOn; }

bool indicatorActive() { return operationRunning || (lastChangeMs != 0 && millis() - lastChangeMs < kBadgeHoldMs); }

}  // namespace dev_api

#endif  // CROSSPOINT_DEV_API
