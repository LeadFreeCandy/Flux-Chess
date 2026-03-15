export interface Transport {
  call<Res>(method: string, params: Record<string, unknown> | object): Promise<Res>;
}

// Serial log for the raw console
export type SerialLogEntry = { dir: "tx" | "rx"; data: string; ts: number };
type LogListener = (entry: SerialLogEntry) => void;
const listeners: LogListener[] = [];

export function onSerialLog(fn: LogListener) { listeners.push(fn); }
export function emitLog(dir: "tx" | "rx", data: string) {
  const entry = { dir, data, ts: Date.now() };
  listeners.forEach(fn => fn(entry));
}

let _transport: Transport | null = null;

function detectTransport(): "http" | "serial" {
  const host = window.location.hostname;
  if (host && host !== "localhost" && host !== "127.0.0.1") {
    return "http";
  }
  return "serial";
}

export function needsUserGesture(): boolean {
  const mode =
    __TRANSPORT__ === "auto" ? detectTransport() : __TRANSPORT__;
  return mode === "serial";
}

export async function initTransport(): Promise<void> {
  const mode =
    __TRANSPORT__ === "auto" ? detectTransport() : __TRANSPORT__;

  if (mode === "http") {
    const { HttpTransport } = await import("./http");
    _transport = new HttpTransport();
  } else {
    const { SerialTransport } = await import("./serial");
    const t = new SerialTransport();
    await t.connect();
    _transport = t;
  }
}

export const transport: Transport = {
  call<Res>(method: string, params: Record<string, unknown>): Promise<Res> {
    if (!_transport) {
      throw new Error("Transport not initialized. Call initTransport() first.");
    }
    return _transport.call(method, params);
  },
};
