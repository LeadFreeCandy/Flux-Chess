import type { CallOptions, Transport } from "./index";
import { emitLog } from "./index";
import { diag } from "../diagnostics";

export class SerialTransport implements Transport {
  private port: SerialPort | null = null;
  private readBuffer = "";
  private pending: {
    method: string;
    resolve: (v: unknown) => void;
    reject: (e: Error) => void;
    sentAt: number;
  } | null = null;
  private requestCount = 0;
  private responseCount = 0;
  private timeoutCount = 0;
  private busyCount = 0;

  async connect(): Promise<void> {
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });
    await new Promise((r) => setTimeout(r, 2000));
    this.startReading();
    console.log("[serial] connected");
  }

  private lastDataAt = Date.now();
  private currentReader: ReadableStreamDefaultReader<Uint8Array> | null = null;

  private startReading(): void {
    if (!this.port?.readable) {
      console.error("[serial] startReading: port not readable");
      return;
    }

    const reader = this.port.readable.getReader();
    this.currentReader = reader;
    const decoder = new TextDecoder();
    this.lastDataAt = Date.now();
    console.log("[serial] read loop started");

    const readLoop = async () => {
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) {
            console.warn("[serial] reader done signal");
            break;
          }
          this.lastDataAt = Date.now();
          const text = decoder.decode(value, { stream: true });
          this.readBuffer += text;
          this.processBuffer();
        }
      } catch (e) {
        console.error("[serial] read loop error:", e);
      } finally {
        this.currentReader = null;
        reader.releaseLock();
        console.warn("[serial] reader released");
      }
      if (this.port?.readable) {
        console.log("[serial] restarting read loop...");
        this.startReading();
      } else {
        console.error("[serial] port no longer readable, cannot restart");
      }
    };
    readLoop();

    // Watchdog: if no data received for 5s and we have a pending request,
    // cancel the reader to force a restart
    this.startReaderWatchdog();
  }

  private readerWatchdogTimer: ReturnType<typeof setInterval> | null = null;

  private startReaderWatchdog(): void {
    if (this.readerWatchdogTimer) clearInterval(this.readerWatchdogTimer);
    this.readerWatchdogTimer = setInterval(() => {
      const silent = Date.now() - this.lastDataAt;
      if (silent > 5000 && this.pending && this.currentReader) {
        console.error(`[serial] WATCHDOG: no data for ${silent}ms with pending "${this.pending.method}", forcing reader restart`);
        this.currentReader.cancel().catch(() => {});
      }
    }, 2000);
  }

  private processBuffer(): void {
    // Split once instead of slicing the buffer per line (O(n²) → O(n)).
    const parts = this.readBuffer.split("\n");
    this.readBuffer = parts.pop() ?? "";
    const t0 = performance.now();
    let processed = 0;

    for (const raw of parts) {
      const line = raw.trim();
      if (!line) continue;
      processed++;

      emitLog("rx", line);

      try {
        const msg = JSON.parse(line);
        if (this.pending && msg.method === this.pending.method) {
          const latency = Date.now() - this.pending.sentAt;
          this.responseCount++;
          if ("result" in msg) {
            this.pending.resolve(msg.result);
          } else if ("error" in msg) {
            this.pending.reject(new Error(msg.error));
          }
          this.pending = null;
          if (this.responseCount % 100 === 0) {
            console.log(`[serial] stats: ${this.responseCount} responses, ${this.timeoutCount} timeouts, ${this.busyCount} busy skips, last latency ${latency}ms`);
          }
        }
      } catch {
        // non-JSON line
      }
    }

    const elapsed = performance.now() - t0;
    if (elapsed > 50 || processed > 50) {
      diag("serial/processBuffer", { lines: processed, elapsed_ms: Math.round(elapsed), bufLen: this.readBuffer.length });
    }

    if (this.readBuffer.length > 10000) {
      console.warn(`[serial] readBuffer growing: ${this.readBuffer.length} chars`);
      diag("serial/bufferGrowing", { len: this.readBuffer.length });
    }
  }

  async call<Res>(
    method: string,
    params: Record<string, unknown>,
    opts: CallOptions = {},
  ): Promise<Res> {
    if (!this.port?.writable) throw new Error("Serial port not connected");
    if (this.pending) {
      this.busyCount++;
      const staleness = Date.now() - this.pending.sentAt;
      if (staleness > 2000) {
        console.error(`[serial] STALE pending request: "${this.pending.method}" sent ${staleness}ms ago, force-clearing`);
        this.pending.reject(new Error("Force-cleared stale request"));
        this.pending = null;
      } else {
        throw new Error("Serial transport is busy");
      }
    }

    this.requestCount++;
    const msg = JSON.stringify({ method, params }) + "\n";
    emitLog("tx", msg.trim());
    diag("serial/send", { method, bufLen: this.readBuffer.length });

    const writer = this.port.writable.getWriter();
    await writer.write(new TextEncoder().encode(msg));
    writer.releaseLock();

    const timeoutMs = opts.timeoutMs ?? 5000;

    return new Promise<Res>((resolve, reject) => {
      const sentAt = Date.now();
      this.pending = {
        method,
        resolve: resolve as (v: unknown) => void,
        reject,
        sentAt,
      };
      if (timeoutMs > 0) {
        setTimeout(() => {
          // Only reject if THIS call's pending is still in place (sentAt match
          // prevents a stale timer from rejecting a later same-method request).
          if (this.pending?.method === method && this.pending?.sentAt === sentAt) {
            this.timeoutCount++;
            console.error(`[serial] TIMEOUT: "${method}" after ${timeoutMs}ms (timeouts=${this.timeoutCount}, pending bufLen=${this.readBuffer.length})`);
            diag("serial/timeout", { method, bufLen: this.readBuffer.length });
            this.pending = null;
            reject(new Error(`Serial timeout: ${method}`));
          }
        }, timeoutMs);
      }
    });
  }

  async send(method: string, params: Record<string, unknown>): Promise<void> {
    if (!this.port?.writable) throw new Error("Serial port not connected");
    const msg = JSON.stringify({ method, params }) + "\n";
    emitLog("tx", msg.trim());
    diag("serial/send", { method, bufLen: this.readBuffer.length, fireAndForget: true });

    const writer = this.port.writable.getWriter();
    await writer.write(new TextEncoder().encode(msg));
    writer.releaseLock();
    // No pending slot, no response awaited. The firmware may still write a
    // response line when it finishes; processBuffer drops it because either
    // `pending` is null or its method doesn't match.
  }
}
