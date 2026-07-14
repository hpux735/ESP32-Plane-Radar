#pragma once

#include <cstdint>

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();

/** Tap-pattern discriminator.
 *  Single = adjust current screen (range on radar, refresh on weather).
 *  Double = advance to the next screen in the ring. The button-side path
 *  waits config::kMultiTapWindowMs after the last tap to distinguish
 *  Single vs Double; the accelerometer path bypasses the window and
 *  reports each hardware-discriminated event immediately. */
enum class BootTap : uint8_t { None, Single, Double };
/** Consume the pending tap event (if any). Call once per loop after
 *  bootButtonPollLongPress(). */
BootTap bootButtonConsumeEvent();
