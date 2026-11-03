export const TARGET_TURNS = 5;
export function validSeconds(value) {
  return Number.isFinite(value) && value >= 0.2 && value <= 30;
}
export function createFeeder() {
  let active = null;
  return {
    start(channel, secondsPerTurn, now) {
      if (active || !Number.isInteger(channel) || channel < 1 || channel > 3 || !validSeconds(secondsPerTurn)) return false;
      active = { channel, started: now, duration: TARGET_TURNS * secondsPerTurn * 1000 };
      return true;
    },
    sample(now) {
      if (!active) return null;
      const fraction = Math.min(1, Math.max(0, (now - active.started) / active.duration));
      return { ...active, fraction, turns: fraction * TARGET_TURNS, remaining: Math.max(0, active.duration - (now - active.started)), done: fraction === 1 };
    },
    stop() { const previous = active; active = null; return previous; }
  };
}
