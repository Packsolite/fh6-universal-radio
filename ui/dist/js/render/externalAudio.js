import { api } from "../api.js";
import { $, el } from "../dom.js";
import { toast } from "../toast.js";

// External Audio card: pick a WASAPI capture device and a media session.
// Self-contained device/session state; ctx bridges back to the app config.
export function createExternalAudio(main, ctx) {
  let devices = [];
  let endpoint = "";
  let sessions = [];
  let sessionId = "";
  let sessionsAvailable = false;
  let loaded = false;
  let loading = false;

  const deviceSelect = el("select", { id: "ext-device", "aria-label": "External Audio capture device" });
  const sessionSelect = el("select", { id: "ext-session", "aria-label": "External Audio media session" });
  const refreshBtn = el("button", { type: "button", class: "btn ghost" }, "Refresh");
  const saveBtn = el("button", { type: "button", class: "btn filled" }, "Save");
  const hint = el("p", { class: "muted" });

  const card = el("section", { class: "card", id: "external-audio-card", hidden: true }, [
    el("h2", {}, "External Audio"),
    el(
      "p",
      { class: "muted" },
      "Select the Windows playback device used for audio capture and the media session used for metadata and next/previous commands.",
    ),
    el("label", { class: "field-label", for: "ext-device" }, "Capture device"),
    el("div", { class: "row" }, [deviceSelect, refreshBtn]),
    el("label", { class: "field-label", for: "ext-session" }, "Media session"),
    el("div", { class: "row" }, [sessionSelect, saveBtn]),
    hint,
  ]);

  const sourcesCard = $("#sources", main)?.closest(".card");
  if (sourcesCard) sourcesCard.insertAdjacentElement("afterend", card);
  else main.append(card);

  async function load(force = false) {
    if ((loaded && !force) || loading) return;
    loading = true;
    try {
      const r = await api.getExternalAudio();
      devices = Array.isArray(r.devices) ? r.devices : [];
      endpoint = r.endpoint_id || "";
      sessions = Array.isArray(r.media_sessions) ? r.media_sessions : [];
      sessionId = r.media_session_id || "";
      sessionsAvailable = !!r.media_sessions_available;
      loaded = true;
    } catch {
      devices = [];
      sessions = [];
      sessionsAvailable = false;
      loaded = false;
    } finally {
      loading = false;
    }
    render();
  }

  refreshBtn.addEventListener("click", () => load(true));

  saveBtn.addEventListener("click", async () => {
    try {
      const enabled = !!ctx.getConfig()?.external_audio?.enabled;
      const r = await api.putExternalAudio({
        enabled,
        endpoint_id: deviceSelect.value,
        media_session_id: sessionSelect.value,
      });
      loaded = false;
      await ctx.onSaved({
        enabled: !!r.enabled,
        endpoint_id: r.endpoint_id ?? deviceSelect.value,
        media_session_id: r.media_session_id ?? sessionSelect.value,
      });
      toast("External Audio settings saved");
    } catch (e) {
      toast(e.message, true);
    }
  });

  function render() {
    const state = ctx.getState();
    const enabled = !!ctx.getConfig()?.external_audio?.enabled;
    card.hidden = !enabled;
    if (!enabled) return;

    load();

    const deviceSig = `${endpoint}|${devices.map(d => `${d.id}:${d.name}:${d.is_default}`).join("|")}`;
    if (deviceSelect.dataset.sig !== deviceSig) {
      deviceSelect.dataset.sig = deviceSig;
      deviceSelect.replaceChildren(
        el("option", { value: "", selected: endpoint === "" }, "Default Windows playback device"),
        ...devices.map(d =>
          el(
            "option",
            { value: d.id, selected: endpoint === d.id },
            `${d.name || d.id}${d.is_default ? " (current default)" : ""}`,
          ),
        ),
      );
    }

    const sessionSig = `${sessionId}|${sessionsAvailable}|${sessions.map(s => `${s.id}:${s.name}:${s.is_current}`).join("|")}`;
    if (sessionSelect.dataset.sig !== sessionSig) {
      sessionSelect.dataset.sig = sessionSig;
      if (!sessionsAvailable) {
        sessionSelect.replaceChildren(
          el("option", { value: "", selected: true }, "Media session API is not available in this build"),
        );
        sessionSelect.disabled = true;
      } else {
        sessionSelect.replaceChildren(
          el("option", { value: "", selected: sessionId === "" }, "Current Windows media session"),
          ...sessions.map(s =>
            el(
              "option",
              { value: s.id, selected: sessionId === s.id },
              `${s.name || s.id}${s.is_current ? " (current)" : ""}`,
            ),
          ),
        );
        sessionSelect.disabled = false;
      }
    }

    const available = state?.sources?.available?.some(s => s.name === "external_audio");
    const active = state?.sources?.active === "external_audio";
    hint.textContent = !available
      ? "Enabled, but the source hasn't registered yet."
      : active
        ? "Active and on air."
        : "Ready. Switch to External Audio in Sources to go on air.";
  }

  return {
    render,
    invalidate: () => {
      loaded = false;
    },
  };
}
