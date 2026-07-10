#include "services/wifi_setup.h"

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

constexpr char kClockPrefsNamespace[] = "clock";
constexpr char kClockPrefsTzKey[] = "tz";
// SF Bay Area — matches the Sutro Tower default home coord. Users in other
// regions edit this via the portal.
constexpr char kDefaultTz[] = "PST8PDT,M3.2.0,M11.1.0/2";

String loadStoredTz() {
  Preferences prefs;
  if (!prefs.begin(kClockPrefsNamespace, true)) return String(kDefaultTz);
  String tz = prefs.getString(kClockPrefsTzKey, kDefaultTz);
  prefs.end();
  if (tz.length() == 0) tz = kDefaultTz;
  return tz;
}

void applyTzAndStartSntp() {
  const String tz = loadStoredTz();
  configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
  Serial.printf("clock: TZ=%s, SNTP started\n", tz.c_str());
}

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr int kRadiusParamLen = 8;
constexpr int kFocusJsonParamLen = 640;
constexpr int kTzParamLen = 40;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";
constexpr char kRadiusInputAttrs[] =
    " type=\"number\" step=\"0.1\" min=\"1\"";
constexpr char kFocusJsonAttrs[] =
    " maxlength=\"640\" placeholder='[{\"name\":\"SFO\",\"lat\":37.62,\"lon\":-122.38,\"range_idx\":1}]'";

WiFiManagerParameter s_param_lat("radar_lat", "Home latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Home longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

WiFiManagerParameter s_param_metar_lat("metar_lat",
                                       "METAR map center latitude (deg)", "0",
                                       kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_metar_lon("metar_lon",
                                       "METAR map center longitude (deg)", "0",
                                       kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_metar_radius("metar_rad",
                                          "METAR map radius (nm)", "45",
                                          kRadiusParamLen, kRadiusInputAttrs);

WiFiManagerParameter s_param_focus_json(
    "focus_ring",
    "Focus airports (JSON: [{name,lat,lon,range_idx}, ...])", "",
    kFocusJsonParamLen, kFocusJsonAttrs);

WiFiManagerParameter s_param_tz("clock_tz", "Time zone (POSIX TZ)",
                                kDefaultTz, kTzParamLen);

constexpr int kHostnameParamLen = 32;
WiFiManagerParameter s_param_hostname(
    "ota_host", "mDNS hostname (OTA + web portal)",
    config::kPortalHostname, kHostnameParamLen);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

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

  const String tz = loadStoredTz();
  s_param_tz.setValue(tz.c_str(), kTzParamLen);

  Preferences prefs;
  String hostname = config::kPortalHostname;
  if (prefs.begin(kWifiPrefsNamespace, true)) {
    hostname = prefs.getString("ota_host", config::kPortalHostname);
    prefs.end();
    if (hostname.length() == 0) hostname = config::kPortalHostname;
  }
  s_param_hostname.setValue(hostname.c_str(), kHostnameParamLen);

  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
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
  const char* tz = s_param_tz.getValue();
  if (tz != nullptr && tz[0] != '\0') {
    Preferences prefs;
    if (prefs.begin(kClockPrefsNamespace, false)) {
      prefs.putString(kClockPrefsTzKey, tz);
      prefs.end();
      Serial.printf("clock: TZ saved (%s) — SNTP re-applied\n", tz);
      configTzTime(tz, "pool.ntp.org", "time.nist.gov");
    }
  }
  services::ota::setHostname(s_param_hostname.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_metar_lat);
  wm.addParameter(&s_param_metar_lon);
  wm.addParameter(&s_param_metar_radius);
  wm.addParameter(&s_param_focus_json);
  wm.addParameter(&s_param_tz);
  wm.addParameter(&s_param_hostname);
  wm.addParameter(&s_param_runways);
  wm.setSaveParamsCallback(onPortalParamsSaved);
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
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
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
