import { describe, it, expect, beforeEach, vi } from "vitest";
import { toast } from "../dist/js/toast.js";

beforeEach(() => {
  document.body.innerHTML = "";
});

describe("toast", () => {
  it("announces info politely via role=status", () => {
    toast("Saved");
    const node = document.querySelector(".toast");
    expect(node.textContent).toBe("Saved");
    expect(node.getAttribute("role")).toBe("status");
    expect(node.getAttribute("aria-live")).toBe("polite");
    expect(node.classList.contains("err")).toBe(false);
  });

  it("announces errors assertively via role=alert", () => {
    toast("Boom", true);
    const node = document.querySelector(".toast.err");
    expect(node.getAttribute("role")).toBe("alert");
    expect(node.getAttribute("aria-live")).toBe("assertive");
  });

  it("auto-dismisses", () => {
    vi.useFakeTimers();
    toast("Bye");
    expect(document.querySelector(".toast")).toBeTruthy();
    vi.advanceTimersByTime(2400);
    expect(document.querySelector(".toast")).toBeNull();
    vi.useRealTimers();
  });
});
