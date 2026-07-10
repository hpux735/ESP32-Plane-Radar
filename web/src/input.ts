// Tap discriminator — same shape as the firmware's BootTap state machine
// (~250 ms window; count taps, dispatch on quiet). Two gestures only:
// single = adjust the current screen, double = advance the screen ring.

export type Tap = "single" | "double";

export function makeTapDiscriminator(onTap: (t: Tap) => void, windowMs = 250) {
  let count = 0;
  let timer: number | null = null;

  function flush() {
    if (count === 0) return;
    count = 0;
    timer = null;
    onTap("single");
  }

  function tap() {
    count += 1;
    if (count >= 2) {
      // Double fires immediately — no reason to wait for a hypothetical 3rd.
      if (timer !== null) {
        clearTimeout(timer);
        timer = null;
      }
      count = 0;
      onTap("double");
      return;
    }
    if (timer !== null) clearTimeout(timer);
    timer = window.setTimeout(flush, windowMs);
  }

  return { tap };
}
