#pragma once

#include <ArduinoJson.h>

#include "SettingsList.h"

// JSON form of one setting, shared by the web settings API and the dev API.
// Fills key, name, category, type, value and options/range. Returns false for
// a setting type that has no JSON form.
bool settingToJson(const SettingInfo& s, JsonDocument& doc);

// Applies a JSON value to one setting in memory (the caller saves). Returns
// true when the value was valid and applied.
bool applySettingJson(const SettingInfo& s, JsonVariantConst v);
