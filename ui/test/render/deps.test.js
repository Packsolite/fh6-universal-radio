import { describe, it, expect } from "vitest";
import { createDeps, depRow } from "../../dist/js/render/deps.js";

describe("depRow", () => {
  it("renders a downloading row with percent and fills the bar", () => {
    const row = depRow({
      name: "ffmpeg",
      downloading: true,
      downloaded_bytes: 50,
      total_bytes: 100,
      error: "",
    });
    expect(row.querySelector(".dep-name").textContent).toBe("ffmpeg");
    expect(row.querySelector(".dep-state").textContent).toBe("downloading 50%");
    expect(row.querySelector(".dep-fill").style.width).toBe("50%");
  });

  it("renders bytes when total is unknown", () => {
    const row = depRow({
      name: "yt-dlp",
      downloading: true,
      downloaded_bytes: 2_500_000,
      total_bytes: 0,
    });
    expect(row.querySelector(".dep-state").textContent).toBe("downloading 2.5 MB");
  });

  it("marks errors and shows the message", () => {
    const row = depRow({ name: "librespot", downloading: false, error: "network timeout" });
    expect(row.querySelector(".dep-state").classList.contains("err")).toBe(true);
    expect(row.querySelector(".dep-state").textContent).toBe("network timeout");
  });

  it("renders a ready row otherwise", () => {
    const row = depRow({ name: "ffmpeg", downloading: false, error: "" });
    expect(row.querySelector(".dep-state").textContent).toBe("ready");
  });
});

describe("createDeps", () => {
  it("mounts a hidden dependencies card", () => {
    document.body.innerHTML = "<main></main>";
    createDeps(document.querySelector("main"));
    const card = document.getElementById("deps-card");
    expect(card).toBeTruthy();
    expect(card.hidden).toBe(true);
  });
});
