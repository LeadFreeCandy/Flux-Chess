import type { CallOptions, Transport } from "./index";
import { commands } from "../generated/api";

export class HttpTransport implements Transport {
  private baseUrl: string;

  constructor() {
    this.baseUrl = window.location.origin;
  }

  private buildRequest(method: string, params: Record<string, unknown>): { url: string; init: RequestInit } {
    const cmd = commands[method as keyof typeof commands];
    if (!cmd) throw new Error(`Unknown command: ${method}`);
    const init: RequestInit = {
      method: cmd.method,
      headers: { "Content-Type": "application/json" },
    };
    if (cmd.method === "POST") init.body = JSON.stringify(params);
    return { url: `${this.baseUrl}${cmd.path}`, init };
  }

  async call<Res>(
    method: string,
    params: Record<string, unknown>,
    opts: CallOptions = {},
  ): Promise<Res> {
    const { url, init } = this.buildRequest(method, params);
    const timeoutMs = opts.timeoutMs ?? 5000;
    if (timeoutMs > 0) {
      init.signal = AbortSignal.timeout(timeoutMs);
    }
    const res = await fetch(url, init);
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}: ${res.statusText}`);
    }
    return res.json();
  }

  async send(method: string, params: Record<string, unknown>): Promise<void> {
    const { url, init } = this.buildRequest(method, params);
    // Fire-and-forget: kick off the request; don't await the body.
    fetch(url, init).catch(() => {});
  }
}
