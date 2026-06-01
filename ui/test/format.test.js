import { describe, it, expect } from "vitest";
import { fmt, clamp, percent, progressRatio, db } from "../dist/js/format.js";

describe("format", () => {
  it("fmt renders ms as m:ss and guards empty/negative", () => {
    expect(fmt(0)).toBe("0:00");
    expect(fmt(-5)).toBe("0:00");
    expect(fmt(5000)).toBe("0:05");
    expect(fmt(65000)).toBe("1:05");
    expect(fmt(600000)).toBe("10:00");
  });

  it("clamp bounds within range", () => {
    expect(clamp(5, 0, 1)).toBe(1);
    expect(clamp(-1, 0, 1)).toBe(0);
    expect(clamp(0.5, 0, 1)).toBe(0.5);
  });

  it("percent rounds to whole percent", () => {
    expect(percent(0.5)).toBe("50%");
    expect(percent(0.736)).toBe("74%");
    expect(percent(0)).toBe("0%");
  });

  it("progressRatio guards zero/empty and clamps", () => {
    expect(progressRatio(1000, 0)).toBe(0);
    expect(progressRatio(0, 1000)).toBe(0);
    expect(progressRatio(500, 1000)).toBe(0.5);
    expect(progressRatio(5000, 1000)).toBe(1);
  });

  it("db formats with one decimal", () => {
    expect(db(0)).toBe("0.0 dB");
    expect(db(-6)).toBe("-6.0 dB");
    expect(db(2.5)).toBe("2.5 dB");
  });
});
