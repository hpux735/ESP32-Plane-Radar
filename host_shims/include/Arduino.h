#pragma once

// Minimal Arduino.h shim for native builds. Provides just enough surface
// (Serial, delay, millis) for the firmware source files that are compiled
// unmodified under USE_NATIVE. Guarded to C++ only so C sources in LovyanGFX
// (which get -include Arduino.h too) don't choke on <chrono>/<thread>.

#ifdef __cplusplus

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <thread>

class HardwareSerial {
 public:
  void begin(unsigned long) {}
  void println() { std::putchar('\n'); }
  void println(const char* s) { std::puts(s); }
  void print(const char* s) { std::fputs(s, stdout); }
  int printf(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    const int n = std::vprintf(fmt, ap);
    va_end(ap);
    return n;
  }
};

inline HardwareSerial Serial;

inline void delay(unsigned long ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline unsigned long millis() {
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start)
          .count());
}

#endif  // __cplusplus
