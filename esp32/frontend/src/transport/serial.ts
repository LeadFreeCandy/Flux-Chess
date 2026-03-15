import type { Transport } from "./index";

export class SerialTransport implements Transport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<string> | null = null;
  private readBuffer = "";
  private pending: {
    method: string;
    resolve: (v: unknown) => void;
    reject: (e: Error) => void;
  } | null = null;

  async connect(): Promise<void> {
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });
    this.startReading();
  }

  private async startReading(): Promise<void> {
    if (!this.port?.readable) return;
    const decoder = new TextDecoderStream();
    this.port.readable.pipeTo(decoder.writable as WritableStream<Uint8Array>);
    this.reader = decoder.readable.getReader();

    (async () => {
      try {
        while (true) {
          const { value, done } = await this.reader!.read();
          if (done) break;
          this.readBuffer += value;
          this.processBuffer();
        }
      } catch {
        // Port closed or disconnected
      }
    })();
  }

  private processBuffer(): void {
    let newlineIdx: number;
    while ((newlineIdx = this.readBuffer.indexOf("\n")) !== -1) {
      const line = this.readBuffer.slice(0, newlineIdx).trim();
      this.readBuffer = this.readBuffer.slice(newlineIdx + 1);
      if (!line) continue;

      try {
        const msg = JSON.parse(line);
        if (this.pending && msg.method === this.pending.method) {
          this.pending.resolve(msg.result);
          this.pending = null;
        }
      } catch {
        // Skip non-JSON lines
      }
    }
  }

  async call<Res>(
    method: string,
    params: Record<string, unknown>
  ): Promise<Res> {
    if (!this.port?.writable) {
      throw new Error("Serial port not connected");
    }

    if (this.pending) {
      throw new Error("Serial transport is busy — one request at a time");
    }

    const msg = JSON.stringify({ method, params }) + "\n";
    const encoder = new TextEncoder();
    const writer = this.port.writable.getWriter();
    await writer.write(encoder.encode(msg));
    writer.releaseLock();

    return new Promise<Res>((resolve, reject) => {
      this.pending = {
        method,
        resolve: resolve as (v: unknown) => void,
        reject,
      };

      // Timeout after 5 seconds
      setTimeout(() => {
        if (this.pending?.method === method) {
          this.pending = null;
          reject(new Error(`Serial request timed out: ${method}`));
        }
      }, 5000);
    });
  }
}
