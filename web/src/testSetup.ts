// Vitest global setup — runs once per worker before each test file's own
// module code. See vitest.config.ts's `setupFiles`.
//
// Job: install an in-memory localStorage on happy-dom's `window`, since
// happy-dom (in this vitest version) exposes `window` but leaves
// `localStorage` undefined. state.ts and settings.ts both read/write
// localStorage at module-load time, so without this any test that
// imports either module errors on `setItem`.

function makeLocalStorageShim(): Storage {
  const store = new Map<string, string>();
  return {
    get length() { return store.size; },
    clear() { store.clear(); },
    getItem(k: string) { return store.get(k) ?? null; },
    setItem(k: string, v: string) { store.set(k, String(v)); },
    removeItem(k: string) { store.delete(k); },
    key(i: number) { return Array.from(store.keys())[i] ?? null; },
  };
}

// Install unconditionally if a happy-dom-style `window` is present:
// state.test.ts's afterEach nulls out window.localStorage, so we can't
// rely on "in window" as a sentinel — always overwrite with a fresh
// shim so unrelated tests aren't affected by that teardown.
if (typeof window !== "undefined") {
  Object.defineProperty(window, "localStorage", {
    configurable: true,
    writable: true,
    value: makeLocalStorageShim(),
  });
}
