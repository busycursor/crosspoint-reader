#include "SettingsJson.h"

#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "CrossPointSettings.h"

bool settingToJson(const SettingInfo& s, JsonDocument& doc) {
  doc["key"] = s.key;
  doc["name"] = I18N.get(s.nameId);
  doc["category"] = I18N.get(s.category);

  switch (s.type) {
    case SettingType::TOGGLE: {
      doc["type"] = "toggle";
      if (s.valuePtr) {
        doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
      }
      break;
    }
    case SettingType::ENUM: {
      const auto labels = s.enumLabels();
      const bool toggle = s.enumStringValues.empty() && labels.size() == 2 && labels[0] == StrId::STR_STATE_OFF &&
                          labels[1] == StrId::STR_STATE_ON;
      doc["type"] = toggle ? "toggle" : "enum";
      if (s.valuePtr) {
        doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
      } else if (s.valueGetter) {
        doc["value"] = static_cast<int>(s.valueGetter());
      }
      if (toggle) break;
      JsonArray options = doc["options"].to<JsonArray>();
      if (!s.enumStringValues.empty()) {
        for (const auto& opt : s.enumStringValues) {
          options.add(opt);
        }
      } else {
        for (const auto& opt : s.enumLabels()) {
          options.add(I18N.get(opt));
        }
      }
      break;
    }
    case SettingType::VALUE: {
      doc["type"] = "value";
      if (s.valuePtr) {
        doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
      }
      doc["min"] = s.valueRange.min;
      doc["max"] = s.valueRange.max;
      doc["step"] = s.valueRange.step;
      break;
    }
    case SettingType::STRING: {
      doc["type"] = "string";
      if (s.obfuscated) {
        // Device-bound credentials may contain invalid text after an SD card move.
        doc["value"] = nullptr;
        doc["hasPassword"] =
            s.stringGetter ? !s.stringGetter().empty()
                           : s.stringMaxLen > 0 && *(reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset) != '\0';
      } else if (s.stringGetter) {
        doc["value"] = s.stringGetter();
      } else if (s.stringMaxLen > 0) {
        doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
      }
      break;
    }
    default:
      return false;
  }

  return true;
}

bool applySettingJson(const SettingInfo& s, JsonVariantConst v) {
  switch (s.type) {
    case SettingType::TOGGLE: {
      const int val = v.as<int>() ? 1 : 0;
      if (s.valuePtr) {
        SETTINGS.*(s.valuePtr) = val;
      }
      return true;
    }
    case SettingType::ENUM: {
      const int val = v.as<int>();
      const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumLabels().size())
                                                    : static_cast<int>(s.enumStringValues.size());
      if (val >= 0 && val < maxVal) {
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
        } else if (s.valueSetter) {
          s.valueSetter(static_cast<uint8_t>(val));
        }
        return true;
      }
      break;
    }
    case SettingType::VALUE: {
      const int val = v.as<int>();
      if (val >= s.valueRange.min && val <= s.valueRange.max) {
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
        }
        return true;
      }
      break;
    }
    case SettingType::STRING: {
      const std::string val = v.as<std::string>();
      if (s.stringSetter) {
        s.stringSetter(val);
      } else if (s.stringMaxLen > 0) {
        char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
        strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
        ptr[s.stringMaxLen - 1] = '\0';
      }
      return true;
    }
    default:
      break;
  }
  return false;
}
