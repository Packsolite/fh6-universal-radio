import { setText } from "../dom.js";
import { fmt, progressRatio } from "../format.js";
import { icons } from "../icons.js";

export function activeSource(state) {
  return state?.sources?.available?.find(s => s.name === state?.sources?.active) || null;
}

export function renderNowPlaying(refs, state) {
  const track = state?.track || {};
  const source = activeSource(state);
  const playing = source?.playback_state === "playing";

  const hasArt = !!track.artwork_url;
  refs.art.classList.toggle("has-art", hasArt);
  if (hasArt && refs.img.getAttribute("src") !== track.artwork_url) refs.img.src = track.artwork_url;
  if (!hasArt) refs.img.removeAttribute("src");
  if (refs.backdrop) refs.backdrop.style.backgroundImage = hasArt ? `url("${track.artwork_url}")` : "";

  setText(refs.title, track.title || "Nothing playing");
  setText(
    refs.artist,
    track.artist ? (track.album ? `${track.artist} · ${track.album}` : track.artist) : "",
  );
  setText(refs.pos, fmt(track.position_ms));
  setText(refs.dur, fmt(track.duration_ms));
  refs.fill.style.width = progressRatio(track.position_ms, track.duration_ms) * 100 + "%";

  const want = playing ? "pause" : "play";
  if (refs.play.dataset.icon !== want) {
    refs.play.dataset.icon = want;
    refs.play.innerHTML = icons[want];
    refs.play.setAttribute("aria-label", playing ? "Pause" : "Play");
  }
}
