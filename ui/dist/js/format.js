export function fmt(ms) {
  if (!ms || ms < 0) return "0:00";
  const total = Math.floor(ms / 1000);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
}

export const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));

export const percent = ratio => `${Math.round(ratio * 100)}%`;

export const progressRatio = (positionMs, durationMs) =>
  durationMs && positionMs ? clamp(positionMs / durationMs, 0, 1) : 0;

export const db = value => `${Number(value).toFixed(1)} dB`;
