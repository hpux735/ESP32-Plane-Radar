#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/** Software window after the LAST tap during which further taps count as
 *  part of the same gesture. Both the hardware BOOT button path and the
 *  SDL SPACE-key path use this to distinguish Single vs Double. */
constexpr unsigned long kMultiTapWindowMs = 500UL;

// --- Focus ring ---
/** Conservative cap on user-editable focus airports. Home always occupies
 *  slot 0, so total ring capacity is kMaxFocusAirports + 1. Chosen so the
 *  double-tap cycle stays perceptually short (Home + N airports + Weather
 *  + Cockpit = N+3 screens to loop back). The portal-side JS and the web
 *  settings UI both enforce this same cap client-side; the firmware
 *  enforces it server-side in focus_points.cpp as the last line of defense. */
constexpr size_t kMaxFocusAirports = 6;

// --- Optional BME280 environmental sensor (I²C, address 0x76 or 0x77) ---
// Lives on the ESP32-C3's hardware I²C bus (Wire). Pins picked to avoid
// the SPI display on 0/1/3/4/10. Leave unconnected if no sensor is
// present — env_sensor.cpp probes on boot and silently disables itself
// when nothing answers.
constexpr int kBmeSdaPin = 6;
constexpr int kBmeSclPin = 7;

// --- Optional ADXL345 knock-the-case tap sensor (I²C, address 0x53) ---
// Lives on a DEDICATED software-I²C bus (SoftWire) on the C3's spare
// pins, so this sub-board plugs onto its own header row without daisy-
// chaining SDA/SCL over to the BME280. Wire ADXL345 SDO to GND for
// address 0x53; VCC/GND to 3V3/GND; SDA/SCL to the pins below. Leave
// unconnected if only the BOOT button is used for input — tap_sensor.cpp
// probes on boot and silently disables itself.
//
// WARNING: GPIO 2 is an ESP32-C3 strapping pin — it MUST read HIGH at
// boot (selects the normal SPI-flash boot path). Any commodity ADXL345
// breakout (Adafruit, generic AliExpress boards, etc.) ships with an
// on-board pull-up on SDA that satisfies this automatically. If you
// disconnect the sensor module, ALSO disconnect the wire from GPIO 2 —
// don't leave the pin floating with a dangling wire, or the next boot
// may pick the wrong boot mode. If your ADXL345 is a bare-chip module
// with no on-board pull-ups, wire a 4.7 kΩ (or 10 kΩ) resistor from
// GPIO 2 → 3V3 externally.
constexpr uint8_t kTapSensorI2cAddress = 0x53;
constexpr int kTapSdaPin = 2;
constexpr int kTapSclPin = 5;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
// Sutro Tower, San Francisco — public landmark, safe public default.
constexpr double kDefaultRadarLat = 37.7552;
constexpr double kDefaultRadarLon = -122.4528;

// --- METAR flight-category map defaults (overridden via WiFi setup portal) ---
// Center + radius chosen to match the pre-config auto-fit: geometric middle
// of the baked Bay Area airport list, with the farthest airport landing
// just inside the bezel (KRHV is the pole at ~26 nm from center).
constexpr float kDefaultMetarLat = 37.661f;
constexpr float kDefaultMetarLon = -122.160f;
constexpr float kDefaultMetarRadiusNm = 28.0f;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
