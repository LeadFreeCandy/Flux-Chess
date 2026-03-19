import type { Transport } from "./index";
import { emitLog } from "./index";

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
    let idx: number;
    while ((idx = this.readBuffer.indexOf("\n")) !== -1) {
      const line = this.readBuffer.slice(0, idx).trim();
      this.readBuffer = this.readBuffer.slice(idx + 1);
      if (!line) continue;

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

    // Warn if buffer is growing large (possible partial data stuck)
    if (this.readBuffer.length > 1000) {
      console.warn(`[serial] readBuffer growing: ${this.readBuffer.length} chars, content: ${this.readBuffer.slice(0, 100)}...`);
    }
  }

  async call<Res>(
    method: string,
    params: Record<string, unknown>
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

    const writer = this.port.writable.getWriter();
    await writer.write(new TextEncoder().encode(msg));
    writer.releaseLock();

    return new Promise<Res>((resolve, reject) => {
      this.pending = {
        method,
        resolve: resolve as (v: unknown) => void,
        reject,
        sentAt: Date.now(),
      };
      setTimeout(() => {
        if (this.pending?.method === method) {
          this.timeoutCount++;
          console.error(`[serial] TIMEOUT: "${method}" after 5s (timeouts=${this.timeoutCount}, pending bufLen=${this.readBuffer.length})`);
          this.pending = null;
          reject(new Error(`Serial timeout: ${method}`));
        }
      }, 5000);
    });
  }
}
