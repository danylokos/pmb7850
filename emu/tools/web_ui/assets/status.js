export function createStatus(refreshIntervalMs) {
  const connectionState = document.getElementById("connection-state");
  const connectionLabel = document.getElementById("connection-label");
  const runtimeRunning = document.getElementById("runtime-running");
  const runtimeTicks = document.getElementById("runtime-ticks");
  const runtimeTickRate = document.getElementById("runtime-tick-rate");
  const runtimeGuestRate = document.getElementById("runtime-guest-rate");
  const runtimePc = document.getElementById("runtime-pc");

  function setConnectionState(state) {
    const labels = {
      connecting: "Connecting",
      connected: "Connected",
      error: "Disconnected",
    };
    connectionState.className = `connection ${state}`;
    connectionLabel.textContent = labels[state];
  }

  function updateState(state) {
    const connected = state.bridge === "connected";
    setConnectionState(connected ? "connected" : "error");
    // Bootstrap/re-attachment replaces queued work, retaining same-run age.
    clearTimeout(renderTimer);
    renderTimer = null;
    pending = null;
    lastRenderedAt = null;
    updateLifecycle(state.lifecycle);
    updateMetrics(state.stats, true);
  }

  function updateLifecycle(lifecycle) {
    if (!lifecycle) return;
    stopped = lifecycle.phase === "stopped";
    if (stopped) renderPending();
    connectionLabel.title = lifecycle.phase === "stopped" ?
      `Stopped: ${lifecycle.status}${lifecycle.reason ? ` — ${lifecycle.reason}` : ""}` : lifecycle.phase;
  }

  const metrics = document.getElementById("runtime-stats");
  let latest = null, displayed = null, pending = null;
  let freshnessTimer = null, renderTimer = null, lastRenderedAt = null;
  let stopped = false;

  function checkFreshness() {
    clearTimeout(freshnessTimer);
    const age = displayed ? displayed.ageAtReceipt + performance.now() - displayed.receivedAt : 0;
    const stale = displayed !== null && age >= 2000;
    metrics.classList.toggle("stale", stale);
    document.getElementById("measurement-stale").hidden = !stale;
    if (displayed !== null && !stale)
      freshnessTimer = setTimeout(checkFreshness, Math.max(1, 2000 - age));
  }
  document.addEventListener("visibilitychange", checkFreshness);

  // Round using integer arithmetic before formatting cumulative counters.
  function fixedInteger(value, divisor, digits) {
    const scale = 10n ** BigInt(digits);
    const rounded = (BigInt(value) * scale + divisor / 2n) / divisor;
    return `${rounded / scale}.${(rounded % scale).toString().padStart(digits, "0")}`;
  }

  function updateMetrics(stats, force = false) {
    if (!stats) return;
    const sequence = BigInt(stats.sample_sequence || "0");
    const measured = BigInt(stats.measured_ns || "0");
    const now = performance.now();
    let ageAtReceipt = Number(BigInt(stats.age_ns || "0")) / 1e6;
    if (latest && (sequence < latest.sequence || measured < latest.measured)) return;
    if (latest && (sequence === latest.sequence || measured === latest.measured)) {
      // Duplicates may increase age but never restart the freshness clock.
      ageAtReceipt = Math.max(ageAtReceipt, latest.ageAtReceipt + now - latest.receivedAt);
      latest.ageAtReceipt = ageAtReceipt;
      latest.receivedAt = now;
    }
    latest = {stats, sequence, measured, receivedAt: now, ageAtReceipt};
    pending = latest;
    if (force || stopped || lastRenderedAt === null || now - lastRenderedAt >= refreshIntervalMs) {
      renderPending();
    } else if (renderTimer === null) {
      renderTimer = setTimeout(renderPending, refreshIntervalMs - (now - lastRenderedAt));
    }
  }

  function renderPending() {
    clearTimeout(renderTimer);
    renderTimer = null;
    if (!pending) return;
    displayed = pending;
    pending = null;
    lastRenderedAt = performance.now();
    const {stats} = displayed;
    runtimeRunning.textContent = `${fixedInteger(stats.elapsed_ns, 1000000000n, 2)}s`;
    runtimeTicks.textContent = `${fixedInteger(stats.ticks, 1000000n, 1)}M`;
    runtimeTicks.title = `${stats.ticks} ticks; ${stats.guest_instructions} guest instructions`;
    runtimeTickRate.textContent = stats.rates_valid ? `${(stats.ticks_per_s / 1e6).toFixed(1)}M/s` : "--";
    runtimeGuestRate.textContent = stats.rates_valid ? `${(stats.guest_instructions_per_s / 1e6).toFixed(1)}M/s` : "--";
    const window = stats.rates_valid ? `Averaging window: ${stats.window_ns} ns (${(Number(stats.window_ns) / 1e9).toFixed(3)} s)` : "Rates unavailable: collecting one second of history";
    runtimeTickRate.title = runtimeGuestRate.title = window;
    runtimePc.textContent = `0x${Number(stats.pc).toString(16).padStart(6, "0")}`;
    checkFreshness();
  }

  function clear() {
    clearTimeout(freshnessTimer);
    clearTimeout(renderTimer);
    freshnessTimer = renderTimer = lastRenderedAt = null;
    latest = displayed = pending = null;
    stopped = false;
    metrics.classList.remove("stale");
    document.getElementById("measurement-stale").hidden = true;
    for (const element of [runtimeRunning, runtimeTicks, runtimeTickRate, runtimeGuestRate, runtimePc]) {
      element.textContent = "--";
      element.title = "";
    }
    connectionLabel.title = "";
  }
  return {updateState, updateMetrics, updateLifecycle, setConnectionState, clear};
}
