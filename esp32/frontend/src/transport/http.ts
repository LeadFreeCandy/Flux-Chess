import type { Transport } from "./index";
import { commands } from "../generated/api";

export class HttpTransport implements Transport {
  private baseUrl: string;

  constructor() {
    this.baseUrl = window.location.origin;
  }

  async call<Res>(
    method: string,
    params: Record<string, unknown>
  ): Promise<Res> {
    const cmd = commands[method as keyof typeof commands];
    if (!cmd) {
      throw new Error(`Unknown command: ${method}`);
    }

    const opts: RequestInit = {
      method: cmd.method,
      headers: { "Content-Type": "application/json" },
    };

    if (cmd.method === "POST") {
      opts.body = JSON.stringify(params);
    }

    const res = await fetch(`${this.baseUrl}${cmd.path}`, opts);
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}: ${res.statusText}`);
    }
    return res.json();
  }
}
