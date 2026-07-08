#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

// In-memory shim of ESP32 Preferences (NVS) for native builds.
class Preferences {
 public:
  bool begin(const char* name, bool readonly = false) {
    _ns = name;
    _readonly = readonly;
    return true;
  }
  void end() {}

  bool isKey(const char* key) { return store().count(fullKey(key)) > 0; }
  void remove(const char* key) { store().erase(fullKey(key)); }

  size_t putUChar(const char* key, uint8_t v) {
    store()[fullKey(key)] = std::to_string(static_cast<int>(v));
    return 1;
  }
  uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
    auto it = store().find(fullKey(key));
    return it == store().end()
               ? defaultValue
               : static_cast<uint8_t>(std::stoi(it->second));
  }

  size_t putBool(const char* key, bool v) {
    store()[fullKey(key)] = v ? "1" : "0";
    return 1;
  }
  bool getBool(const char* key, bool defaultValue = false) {
    auto it = store().find(fullKey(key));
    return it == store().end() ? defaultValue : it->second == "1";
  }

  size_t putDouble(const char* key, double v) {
    store()[fullKey(key)] = std::to_string(v);
    return sizeof(double);
  }
  double getDouble(const char* key, double defaultValue = 0.0) {
    auto it = store().find(fullKey(key));
    return it == store().end() ? defaultValue : std::stod(it->second);
  }

 private:
  std::string fullKey(const char* key) const { return _ns + ":" + key; }
  static std::unordered_map<std::string, std::string>& store() {
    static std::unordered_map<std::string, std::string> s;
    return s;
  }

  std::string _ns;
  bool _readonly = false;
};
