import { describe, it, expect, vi, beforeEach } from "vitest";
import { resetMemo } from "../../dist/js/store.js";
import { renderSources, visibleSources, sourcesSignature } from "../../dist/js/render/sources.js";

beforeEach(() => {
  resetMemo();
  document.body.innerHTML = '<div id="sources"></div>';
});

const node = () => document.getElementById("sources");

describe("visibleSources", () => {
  it("hides external_audio unless enabled in config", () => {
    const state = { sources: { available: [{ name: "local_files" }, { name: "external_audio" }] } };
    expect(visibleSources(state, {}).map(s => s.name)).toEqual(["local_files"]);
    expect(visibleSources(state, { external_audio: { enabled: true } }).map(s => s.name)).toEqual([
      "local_files",
      "external_audio",
    ]);
  });
});

describe("sourcesSignature", () => {
  it("changes when playback state changes", () => {
    const a = sourcesSignature(
      [{ name: "a", playback_state: "playing", auth_state: "none_required" }],
      "a",
    );
    const b = sourcesSignature(
      [{ name: "a", playback_state: "paused", auth_state: "none_required" }],
      "a",
    );
    expect(a).not.toBe(b);
  });
});

describe("renderSources", () => {
  it("renders tiles, marks the active one, shows details and auth notes, and fires switch", () => {
    const onSwitch = vi.fn();
    renderSources(
      node(),
      {
        sources: {
          active: "local_files",
          available: [
            {
              name: "local_files",
              display_name: "Local Files",
              playback_state: "playing",
              auth_state: "none_required",
              details: { track_count: 3 },
            },
            {
              name: "jellyfin",
              display_name: "Jellyfin",
              playback_state: "stopped",
              auth_state: "needs_auth",
              auth_instructions: "Configure Jellyfin in Settings",
            },
          ],
        },
      },
      {},
      onSwitch,
    );

    const tiles = node().querySelectorAll(".source");
    expect(tiles.length).toBe(2);
    expect(tiles[0].classList.contains("active")).toBe(true);
    expect(node().textContent).toContain("3 tracks indexed");
    expect(node().querySelector(".source-state.warn")).toBeTruthy();
    expect(node().textContent).toContain("Configure Jellyfin in Settings");

    tiles[1].click();
    expect(onSwitch).toHaveBeenCalledWith("jellyfin");
  });

  it("shows an empty-state message when no sources are visible", () => {
    renderSources(node(), { sources: { active: "", available: [] } }, {}, () => {});
    expect(node().querySelector(".source")).toBeNull();
    expect(node().querySelector(".source-empty")).toBeTruthy();
    expect(node().textContent).toContain("Open Settings");
  });

  it("skips DOM work when the signature is unchanged", () => {
    const state = {
      sources: {
        active: "a",
        available: [
          { name: "a", display_name: "A", playback_state: "playing", auth_state: "none_required" },
        ],
      },
    };
    renderSources(node(), state, {}, () => {});
    const first = node().querySelector(".source");
    renderSources(node(), state, {}, () => {});
    expect(node().querySelector(".source")).toBe(first);
  });
});
