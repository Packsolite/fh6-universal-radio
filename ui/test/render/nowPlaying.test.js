import { describe, it, expect } from "vitest";
import { renderNowPlaying, activeSource } from "../../dist/js/render/nowPlaying.js";

function makeRefs() {
  document.body.innerHTML = `
    <div class="hero" id="art"><div id="backdrop"></div><img id="img" /></div>
    <div id="title"></div><div id="artist"></div>
    <div id="fill"></div><span id="pos"></span><span id="dur"></span>
    <button id="play"></button>`;
  const byId = id => document.getElementById(id);
  return {
    art: byId("art"),
    backdrop: byId("backdrop"),
    img: byId("img"),
    title: byId("title"),
    artist: byId("artist"),
    fill: byId("fill"),
    pos: byId("pos"),
    dur: byId("dur"),
    play: byId("play"),
  };
}

describe("renderNowPlaying", () => {
  it("renders track, progress, artwork and the pause icon while playing", () => {
    const refs = makeRefs();
    renderNowPlaying(refs, {
      sources: {
        active: "local_files",
        available: [
          { name: "local_files", display_name: "Local Files", playback_state: "playing" },
        ],
      },
      track: {
        title: "Song",
        artist: "Artist",
        album: "Album",
        artwork_url: "http://art/cover.jpg",
        duration_ms: 200000,
        position_ms: 100000,
      },
    });
    expect(refs.title.textContent).toBe("Song");
    expect(refs.artist.textContent).toBe("Artist · Album");
    expect(refs.pos.textContent).toBe("1:40");
    expect(refs.dur.textContent).toBe("3:20");
    expect(refs.fill.style.width).toBe("50%");
    expect(refs.art.classList.contains("has-art")).toBe(true);
    expect(refs.img.getAttribute("src")).toBe("http://art/cover.jpg");
    expect(refs.backdrop.style.backgroundImage).toBe('url("http://art/cover.jpg")');
    expect(refs.play.dataset.icon).toBe("pause");
    expect(refs.play.getAttribute("aria-label")).toBe("Pause");
  });

  it("accepts a relative artwork_url served by the mod (/api/artwork)", () => {
    const refs = makeRefs();
    renderNowPlaying(refs, {
      sources: { active: "local_files", available: [{ name: "local_files" }] },
      track: { title: "Song", artwork_url: "/api/artwork?v=42" },
    });
    expect(refs.art.classList.contains("has-art")).toBe(true);
    expect(refs.img.getAttribute("src")).toBe("/api/artwork?v=42");
    expect(refs.backdrop.style.backgroundImage).toBe('url("/api/artwork?v=42")');
  });

  it("shows the empty state and play icon when nothing is playing", () => {
    const refs = makeRefs();
    renderNowPlaying(refs, { sources: { available: [] }, track: {} });
    expect(refs.title.textContent).toBe("Nothing playing");
    expect(refs.artist.textContent).toBe("");
    expect(refs.fill.style.width).toBe("0%");
    expect(refs.art.classList.contains("has-art")).toBe(false);
    expect(refs.backdrop.style.backgroundImage).toBe("");
    expect(refs.play.dataset.icon).toBe("play");
    expect(refs.play.getAttribute("aria-label")).toBe("Play");
  });

  it("activeSource resolves the active entry", () => {
    expect(
      activeSource({ sources: { active: "a", available: [{ name: "a" }, { name: "b" }] } }).name,
    ).toBe("a");
    expect(activeSource({ sources: { active: "z", available: [] } })).toBe(null);
  });
});
