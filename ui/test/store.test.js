import { describe, it, expect, beforeEach } from "vitest";
import { changed, resetMemo } from "../dist/js/store.js";

beforeEach(resetMemo);

describe("store memoization", () => {
  it("reports change on first sight, then suppresses identical signatures", () => {
    expect(changed("k", "a")).toBe(true);
    expect(changed("k", "a")).toBe(false);
    expect(changed("k", "b")).toBe(true);
    expect(changed("k", "b")).toBe(false);
  });

  it("tracks keys independently", () => {
    expect(changed("x", "1")).toBe(true);
    expect(changed("y", "1")).toBe(true);
    expect(changed("x", "1")).toBe(false);
  });

  it("resetMemo clears all keys", () => {
    changed("z", "1");
    resetMemo();
    expect(changed("z", "1")).toBe(true);
  });
});
