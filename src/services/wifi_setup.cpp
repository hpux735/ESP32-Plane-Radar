#include "services/wifi_setup.h"

#include "services/portal_customization.h"

#include "ui/layer_style.h"

#include <vector>

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/focus_points.h"
#include "services/metar_config.h"
#include "services/ota_update.h"
#include "services/radar_location.h"
#include "services/tap_sensor.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

// SNTP still needs to run so std::time() returns real Unix seconds.
// Timezone/DST is derived from Open-Meteo's `utc_offset_seconds` field
// per home lat/lon (see services::outdoor_temp) and applied at the
// cockpit-view rendering layer, so we no longer store or configure a
// POSIX TZ string on-device. configTime(0, 0, ...) sets the base to
// UTC — cockpit adds the home offset itself.
void applyTzAndStartSntp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("clock: SNTP started (UTC base — home tz applied via Open-Meteo)");
}

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;
// Latched once we've attached the coord/METAR/focus/hostname/runways params
// to the WiFiManager instance for the LAN portal. Not attached during the
// captive setup portal — that surface is stripped to just Wi-Fi credentials
// so first-time setup doesn't overwhelm the user with fields that only make
// sense once the device is online. A full BOOT-hold reset reboots the ESP,
// so this bool naturally resets to false on the next captive boot.
bool s_lan_extras_attached = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr int kRadiusParamLen = 8;
constexpr int kFocusJsonParamLen = 640;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";
constexpr char kRadiusInputAttrs[] =
    " type=\"number\" step=\"0.1\" min=\"1\"";
// Labels are intentionally terse — the JS enhancement in portal_customization.h
// prefixes each pair with a section header + hint line, so the WiFiManager
// label only needs to name the single field.
WiFiManagerParameter s_param_lat("radar_lat", "Latitude", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude", "0",
                                kCoordParamLen, kCoordInputAttrs);

WiFiManagerParameter s_param_metar_lat("metar_lat", "Latitude", "0",
                                       kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_metar_lon("metar_lon", "Longitude", "0",
                                       kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_metar_radius("metar_rad", "Reach (nm)", "45",
                                          kRadiusParamLen, kRadiusInputAttrs);

// Hidden by the JS chip editor on the LAN portal; only surfaces if scripting
// fails. Value is still a JSON array (the shape the save handler expects) —
// chips serialize back to this field on every change.
WiFiManagerParameter s_param_focus_json("focus_ring", "Focus places", "",
                                        kFocusJsonParamLen);

constexpr int kHostnameParamLen = 32;
WiFiManagerParameter s_param_hostname(
    "ota_host", "Local network name (advanced)",
    config::kPortalHostname, kHostnameParamLen);

// Layer visibility checkboxes — five overlays the user can turn on/off.
// Declared inline (no struct-of-pointers) to keep static-init ordering
// trivial: each WiFiManagerParameter and its attrs buffer live side-by-side
// as ordinary globals, no cross-object references before main() runs.
char s_lyr_coast_attrs[32] = "type=\"checkbox\"";
char s_lyr_land_attrs[32]  = "type=\"checkbox\"";
char s_lyr_rwlg_attrs[32]  = "type=\"checkbox\"";
char s_lyr_tags_attrs[32]  = "type=\"checkbox\"";
WiFiManagerParameter s_param_lyr_coast("lyr_coast", "Coastline", "T", 2,
                                       s_lyr_coast_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_param_lyr_land("lyr_land",  "Land shading", "T", 2,
                                      s_lyr_land_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_param_lyr_rwlg("lyr_rwlg",  "Airport runways", "T", 2,
                                      s_lyr_rwlg_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_param_lyr_tags("lyr_tags",  "Plane info tags", "T", 2,
                                      s_lyr_tags_attrs, WFM_LABEL_AFTER);

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);

  char metar_lat_buf[kCoordParamLen + 1];
  char metar_lon_buf[kCoordParamLen + 1];
  char metar_rad_buf[kRadiusParamLen + 1];
  snprintf(metar_lat_buf, sizeof(metar_lat_buf), "%.6f",
           services::metar_config::centerLat());
  snprintf(metar_lon_buf, sizeof(metar_lon_buf), "%.6f",
           services::metar_config::centerLon());
  snprintf(metar_rad_buf, sizeof(metar_rad_buf), "%.1f",
           services::metar_config::radiusNm());
  s_param_metar_lat.setValue(metar_lat_buf, kCoordParamLen);
  s_param_metar_lon.setValue(metar_lon_buf, kCoordParamLen);
  s_param_metar_radius.setValue(metar_rad_buf, kRadiusParamLen);

  const String ring_json = services::focus::currentRingJson();
  s_param_focus_json.setValue(ring_json.c_str(), kFocusJsonParamLen);

  Preferences prefs;
  String hostname = config::kPortalHostname;
  if (prefs.begin(kWifiPrefsNamespace, true)) {
    hostname = prefs.getString("ota_host", config::kPortalHostname);
    prefs.end();
    if (hostname.length() == 0) hostname = config::kPortalHostname;
  }
  s_param_hostname.setValue(hostname.c_str(), kHostnameParamLen);

  // Prefill each layer checkbox with the currently-persisted state so the
  // portal reflects reality on every load.
  auto seed = [](char* attrs, ui::layers::Layer l, WiFiManagerParameter& p) {
    snprintf(attrs, 32, "type=\"checkbox\"%s",
             ui::layers::enabled(l) ? " checked" : "");
    p.setValue("T", 2);
  };
  seed(s_lyr_coast_attrs, ui::layers::Layer::Coastline,    s_param_lyr_coast);
  seed(s_lyr_land_attrs,  ui::layers::Layer::Land,         s_param_lyr_land);
  seed(s_lyr_rwlg_attrs,  ui::layers::Layer::RunwaysLarge, s_param_lyr_rwlg);
  seed(s_lyr_tags_attrs,  ui::layers::Layer::AircraftTags, s_param_lyr_tags);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  services::metar_config::saveFromStrings(s_param_metar_lat.getValue(),
                                          s_param_metar_lon.getValue(),
                                          s_param_metar_radius.getValue());
  services::focus::saveRingJson(s_param_focus_json.getValue());
  services::ota::setHostname(s_param_hostname.getValue());
  // Route each layer checkbox to ui::layers::toggle only when its state
  // actually flipped — layer_style writes NVS on every toggle, so guard
  // against unnecessary flash churn on unchanged saves.
  auto apply = [](WiFiManagerParameter& p, ui::layers::Layer l) {
    const bool want = ui::radar::portalCheckboxChecked(p.getValue());
    if (want != ui::layers::enabled(l)) ui::layers::toggle(l);
  };
  apply(s_param_lyr_coast, ui::layers::Layer::Coastline);
  apply(s_param_lyr_land,  ui::layers::Layer::Land);
  apply(s_param_lyr_rwlg,  ui::layers::Layer::RunwaysLarge);
  apply(s_param_lyr_tags,  ui::layers::Layer::AircraftTags);
}

// LAN-only params. The captive portal deliberately shows just Wi-Fi so the
// first-time setup flow is "pick network, type password, done" — everything
// below only makes sense once the device is online and the browser has the
// full internet to do address lookups and airport searches against.
void attachLanExtraParams(WiFiManager& wm) {
  if (s_lan_extras_attached) return;
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_metar_lat);
  wm.addParameter(&s_param_metar_lon);
  wm.addParameter(&s_param_metar_radius);
  wm.addParameter(&s_param_focus_json);
  // Layer checkboxes appear grouped in the JS section under "Map layers".
  wm.addParameter(&s_param_lyr_coast);
  wm.addParameter(&s_param_lyr_land);
  wm.addParameter(&s_param_lyr_rwlg);
  wm.addParameter(&s_param_lyr_tags);
  wm.addParameter(&s_param_hostname);
  wm.setSaveParamsCallback(onPortalParamsSaved);
  // LAN menu: 'param' becomes visible because we now have params to show;
  // 'update' stays available for OTA firmware upload. 'exit' dropped — it
  // only loaded a static "exit" page that didn't actually do anything useful.
  static const char* lan_menu[] = {"wifi", "param", "info", "update"};
  std::vector<const char*> menu(lan_menu, lan_menu + 4);
  wm.setMenu(menu);
  s_lan_extras_attached = true;
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  // Inject the shared HTML/JS enhancement into every portal page:
  //   - always: kill iOS auto-caps on password fields
  //   - LAN /param page: install address+airport search, focus editor, and
  //     one-shot IP-geolocation auto-populate for empty coords
  s_wm.setCustomHeadElement(plane_radar::portal::kCustomHead);
  // Serve the embedded gzipped airport typeahead index at /data/airport_index.json.
  // Runs before WiFiManager registers its own routes so ours takes precedence.
  s_wm.setWebServerCallback([]() {
    if (!s_wm.server) return;
    s_wm.server->on("/data/airport_index.json", []() {
      const size_t len = static_cast<size_t>(
          _binary_data_airport_index_json_gz_end -
          _binary_data_airport_index_json_gz_start);
      s_wm.server->sendHeader("Content-Encoding", "gzip");
      s_wm.server->sendHeader("Cache-Control", "public, max-age=86400");
      s_wm.server->send_P(
          200, "application/json",
          reinterpret_cast<const char*>(_binary_data_airport_index_json_gz_start),
          len);
    });
    // POST /reset-settings — wipes every settings-related NVS namespace
    // (home, METAR, focus ring, range/runways, layers) and reboots. Leaves
    // the `wifi` namespace intact so the device rejoins the same Wi-Fi on
    // next boot — matches the web app's "Reset all to defaults" scope
    // (settings only, not credentials). BOOT-hold-3s remains the way to
    // additionally erase Wi-Fi credentials.
    s_wm.server->on("/reset-settings", HTTP_POST, []() {
      constexpr const char* kSettingsNamespaces[] = {
          "radar", "metar", "focus", "planeradar", "layers",
      };
      Preferences prefs;
      for (const char* ns : kSettingsNamespaces) {
        if (prefs.begin(ns, false)) {
          prefs.clear();
          prefs.end();
          Serial.printf("reset: cleared NVS ns '%s'\n", ns);
        }
      }
      s_wm.server->send(200, "application/json",
                        "{\"ok\":true,\"reboot_in_ms\":800}");
      delay(800);
      esp_restart();
    });
  });
  // Captive-portal menu: no 'param' (no LAN params attached yet), but keep
  // 'update' so a bad OTA can be recovered via captive-portal firmware upload
  // without needing a USB cable and esptool. 'exit' dropped — it only loaded
  // a static "exit" page that didn't actually do anything useful.
  static const char* captive_menu[] = {"wifi", "info", "update"};
  std::vector<const char*> menu(captive_menu, captive_menu + 3);
  s_wm.setMenu(menu);
  // Replace the WiFiManager h1 with a project-specific title on every page.
  s_wm.setTitle("Plane Radar");
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  attachLanExtraParams(s_wm);
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

// Tap-count discriminator. Waits kMultiTapWindowMs after the LAST tap; if
// no further taps arrive in that window, dispatches by count (1 → Single,
// 2 → Double). Double fires the moment the second tap lands. The window
// is 250 ms — enough for a human double-tap, tight enough to keep the
// perceptual delay on single-tap short. Triple-tap discrimination was
// removed when the app collapsed to a two-gesture ring (single = adjust
// current screen, double = advance to next).
BootTap bootButtonConsumeEvent() {
  // Accelerometer path first: the ADXL345 already discriminated single
  // vs double in hardware, so we skip the software window entirely and
  // return the event the moment tap_sensor::poll() latched it. Prefer
  // Double over Single if both are pending in the same tick (shouldn't
  // happen in practice — poll() suppresses SingleTap when DoubleTap
  // fires — but be defensive).
  if (services::tap_sensor::consumeDoubleTap()) return BootTap::Double;
  if (services::tap_sensor::consumeSingleTap()) return BootTap::Single;

  // BOOT-button path: count taps in a 250 ms window. Kept as a fallback
  // for developers who open the case, or for hardware without the
  // accelerometer wired in.
  constexpr unsigned long kMultiTapWindowMs = 250;
  static uint8_t s_pending_count = 0;
  static unsigned long s_last_tap_ms = 0;

  const bool tap = bootButtonConsumeTap();
  const unsigned long now = millis();

  if (tap) {
    ++s_pending_count;
    s_last_tap_ms = now;
    if (s_pending_count >= 2) {
      s_pending_count = 0;
      return BootTap::Double;
    }
    return BootTap::None;
  }
  if (s_pending_count > 0 && (now - s_last_tap_ms) > kMultiTapWindowMs) {
    s_pending_count = 0;
    return BootTap::Single;
  }
  return BootTap::None;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    static bool sntp_started = false;
    if (!sntp_started) {
      applyTzAndStartSntp();
      sntp_started = true;
    }
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
