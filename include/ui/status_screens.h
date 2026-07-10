#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();

/** Persistent "no network" banner: shown when Wi-Fi drops mid-session so the
 *  user gets a clear signal instead of a stale radar frame. Not a partial
 *  render — the whole screen is replaced. */
void statusScreenOffline();

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
