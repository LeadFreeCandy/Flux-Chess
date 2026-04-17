// Lightweight diagnostics to catch main-thread freezes.
//
// - Detects when the main thread is blocked by measuring setInterval drift.
// - Keeps a rolling breadcrumb log of recent events (transport, listeners, errors).
// - Persists to localStorage so the log survives a tab kill+reload.
// - Dumps to console on freeze and on demand (Ctrl+Shift+D).

const MAX_BREADCRUMBS = 500;
const WATCHDOG_INTERVAL_MS = 250;
const FREEZE_THRESHOLD_MS = 750;     // block longer than this = "freeze"
const STORAGE_KEY = "fluxchess_diag_log";

type Breadcrumb = { ts: number; kind: string; data?: unknown };

const breadcrumbs: Breadcrumb[] = [];

function push(kind: string, data?: unknown) {
  const bc: Breadcrumb = { ts: Date.now(), kind, data };
  breadcrumbs.push(bc);
  if (breadcrumbs.length > MAX_BREADCRUMBS) breadcrumbs.splice(0, breadcrumbs.length - MAX_BREADCRUMBS);
}

export function diag(kind: string, data?: unknown) {
  push(kind, data);
}

function persist() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({
      savedAt: Date.now(),
      userAgent: navigator.userAgent,
      breadcrumbs,
    }));
  } catch {
    // localStorage quota or disabled — swallow
  }
}

function dump(label: string) {
  // eslint-disable-next-line no-console
  console.group(`[diag] ${label} — last ${breadcrumbs.length} events`);
  for (const bc of breadcrumbs) {
    // eslint-disable-next-line no-console
    console.log(new Date(bc.ts).toISOString().slice(11, 23), bc.kind, bc.data ?? "");
  }
  // eslint-disable-next-line no-console
  console.groupEnd();
}

export function installDiagnostics() {
  // ── Dump any log from a previous session (prior freeze) ──
  try {
    const prior = localStorage.getItem(STORAGE_KEY);
    if (prior) {
      // eslint-disable-next-line no-console
      console.warn("[diag] prior session log found — dumping, then clearing");
      // eslint-disable-next-line no-console
      console.log(JSON.parse(prior));
      localStorage.removeItem(STORAGE_KEY);
    }
  } catch {}

  push("diag/start");

  // ── Main-thread watchdog ──
  let lastTick = performance.now();
  setInterval(() => {
    const now = performance.now();
    const drift = now - lastTick - WATCHDOG_INTERVAL_MS;
    lastTick = now;
    if (drift > FREEZE_THRESHOLD_MS) {
      push("freeze", { drift_ms: Math.round(drift) });
      persist();
      // eslint-disable-next-line no-console
      console.error(`[diag] FREEZE detected — main thread blocked for ~${Math.round(drift)}ms`);
      dump("freeze");
    }
  }, WATCHDOG_INTERVAL_MS);

  // ── Periodic persist so we always have recent history on tab kill ──
  setInterval(persist, 2000);

  // ── Uncaught errors / promise rejections ──
  window.addEventListener("error", (e) => {
    push("window.error", { message: e.message, filename: e.filename, line: e.lineno, col: e.colno });
    persist();
  });
  window.addEventListener("unhandledrejection", (e) => {
    push("unhandledrejection", { reason: String(e.reason) });
    persist();
  });

  // ── Manual dump: Ctrl+Shift+D ──
  window.addEventListener("keydown", (e) => {
    if (e.ctrlKey && e.shiftKey && e.key === "D") {
      dump("manual");
    }
  });

  // ── Expose for console ──
  (window as unknown as { __diag: unknown }).__diag = {
    breadcrumbs,
    dump: () => dump("manual"),
    clear: () => { breadcrumbs.length = 0; localStorage.removeItem(STORAGE_KEY); },
  };
}
