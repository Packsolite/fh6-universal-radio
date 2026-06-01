import { describe, it, expect } from "vitest";
import { readdirSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const distDir = join(dirname(fileURLToPath(import.meta.url)), "..", "dist");

function walk(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const path = join(dir, entry.name);
    return entry.isDirectory() ? walk(path) : [path];
  });
}

describe("performance budget", () => {
  it("ships JS + CSS under 60 KB (fonts excluded)", () => {
    const total = walk(distDir)
      .filter(f => /\.(js|css)$/.test(f))
      .reduce((sum, f) => sum + statSync(f).size, 0);
    expect(total).toBeLessThan(60 * 1024);
  });

  it("keeps the entry HTML under 8 KB", () => {
    expect(statSync(join(distDir, "index.html")).size).toBeLessThan(8 * 1024);
  });
});
