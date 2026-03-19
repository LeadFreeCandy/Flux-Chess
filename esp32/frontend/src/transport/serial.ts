import type { Transport } from "./index";
import { emitLog } from "./index";

export class SerialTransport implements Transport {
  private port: SerialPort | null = null;
  private readBuffer = "";
  private pending: {
    method: string;
    resolve: (v: unknown) => void;
    reject: (e: Error) => void;
  } | null = null;

  async connect(): Promise<void> {
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });
    await new Promise((r) => setTimeout(r, 2000));
    this.startReading();
  }

  private startReading(): void {
    if (!this.port?.readable) return;

    const reader = this.port.readable.getReader();
    const decoder = new TextDecoder();

    const readLoop = async () => {
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          const text = decoder.decode(value, { stream: true });
          this.readBuffer += text;
          this.processBuffer();
        }
      } catch (e) {
        console.error("Serial read error, restarting reader:", e);
      } finally {
        reader.releaseLock();
      }
      // Auto-restart if port is still open
      if (this.port?.readable) {
        console.log("Serial reader restarting...");
        this.startReading();
      }
    };
    readLoop();
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
          if ("result" in msg) {
            this.pending.resolve(msg.result);
          } else if ("error" in msg) {
            this.pending.reject(new Error(msg.error));
          }
          this.pending = null;
        }
      } catch {
        // non-JSON line
      }
    }
  }

  async call<Res>(
    method: string,
    params: Record<string, unknown>
  ): Promise<Res> {
    if (!this.port?.writable) throw new Error("Serial port not connected");
    if (this.pending) throw new Error("Serial transport is busy");

    const msg = JSON.stringify({ method, params }) + "\n";
    emitLog("tx", msg.trim());

    const writer = this.port.writable.getWriter();
    await writer.write(new TextEncoder().encode(msg));
    writer.releaseLock();

    return new Promise<Res>((resolve, reject) => {
      this.pending = { method, resolve: resolve as (v: unknown) => void, reject };
      setTimeout(() => {
        if (this.pending?.method === method) {
          this.pending = null;
          reject(new Error(`Serial timeout: ${method}`));
        }
      }, 5000);
    });
  }
}
