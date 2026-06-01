import { percent } from "../format.js";

// Volume slider. While the user is dragging, incoming state must not overwrite
// the slider, so a dirty flag suppresses render until shortly after commit.
export function createOutput(slider, out, onCommit) {
  let dirty = false;

  slider.addEventListener("input", () => {
    dirty = true;
    out.value = percent(parseFloat(slider.value));
  });

  slider.addEventListener("change", async () => {
    try {
      await onCommit(parseFloat(slider.value));
    } finally {
      setTimeout(() => {
        dirty = false;
      }, 400);
    }
  });

  return function render(state) {
    if (dirty) return;
    const gain = state?.audio?.output_gain ?? 0;
    if (Math.abs(parseFloat(slider.value) - gain) > 0.005) slider.value = gain;
    out.value = percent(gain);
  };
}
