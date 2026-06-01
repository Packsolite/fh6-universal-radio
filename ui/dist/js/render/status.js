import { setText } from "../dom.js";

export function renderStatus(node, state) {
  const ok = !!state?.game?.attached;
  node.className = "status " + (ok ? "ok" : "err");
  setText(node, ok ? "connected" : "bridge offline");
}
