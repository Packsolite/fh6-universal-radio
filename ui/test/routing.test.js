import { describe, it, expect, vi, beforeEach } from "vitest";
import { api } from "../dist/js/api.js";

function ok(body = {}) {
  return vi.fn().mockResolvedValue({ ok: true, json: async () => body });
}

function call() {
  const [path, opts] = global.fetch.mock.calls.at(-1);
  return { path, method: opts.method, body: opts.body ? JSON.parse(opts.body) : undefined };
}

beforeEach(() => {
  global.fetch = ok();
});

describe("audio routing", () => {
  it("source switch posts the chosen source", async () => {
    await api.switchSource("jellyfin");
    expect(call()).toEqual({
      path: "/api/source/switch",
      method: "POST",
      body: { source: "jellyfin" },
    });
  });

  it("each transport action maps to /api/source/<name>/<action>", async () => {
    for (const action of ["play", "pause", "stop", "next", "previous"]) {
      await api.transport("local_files", action);
      expect(call()).toMatchObject({ path: `/api/source/local_files/${action}`, method: "POST" });
    }
  });

  it("volume routes to /api/options as output_gain", async () => {
    await api.setGain(0.75);
    expect(call()).toEqual({ path: "/api/options", method: "POST", body: { output_gain: 0.75 } });
  });
});
