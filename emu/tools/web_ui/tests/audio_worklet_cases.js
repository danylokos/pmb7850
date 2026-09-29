() => {
  const check = (condition, label) => { if (!condition) throw new Error(label); };
  const make = (diagnostics = true) => {
    window.currentFrame = 0;
    return new window.__CemuAudioProcessor({processorOptions: {diagnostics}});
  };
  const feed = (p, samples) => p.port.onmessage({data: {
    type: "pcm", samples: Int16Array.from(samples).buffer,
  }});
  const signal = (count, start = 0) => Int16Array.from(
    {length: count}, (_, i) => ((start + i) * 997 % 65536) - 32768);
  const compare = (output, expected, label) => {
    for (let i = 0; i < output.length; i++)
      check(output[i] === (expected[i] ?? 0) / 32768, `${label}: frame ${i}`);
  };
  const render = (p, expected, label) => {
    // Prefill to verify the renderer explicitly silences every unused frame.
    const out = [new Float32Array(128).fill(1), new Float32Array(128).fill(1)];
    check(p.process([], [out]) === true, "processor stays active");
    check(out[0].every((v, i) => v === out[1][i]), "channel copy");
    if (expected !== null) compare(out[0], expected, label);
    window.currentFrame += 128;
    return out[0];
  };
  const wait = (p, start = window.currentFrame) => {
    const queued = p.fifoFrames;
    while (window.currentFrame - start < 4800) {
      render(p, [], "prebuffer silence");
      check(p.fifoFrames === queued, "buffering preserves queued samples");
    }
  };

  // Idle time does not count toward the deadline. Later packets do not reset it.
  const startup = make();
  for (let i = 0; i < 50; i++) render(startup, [], "idle before first PCM");
  const first = window.currentFrame;
  feed(startup, signal(4799));
  render(startup, [], "one sample below threshold");
  check(startup.fifoFrames === 4799, "startup retains sub-threshold queue");
  feed(startup, signal(1, 4799));
  render(startup, signal(128), "threshold starts at next callback");
  check(startup.fifoFrames === 4672, "threshold is exactly 4800 samples");
  render(startup, signal(128, 128), "playing below threshold continues");
  check(window.currentFrame - first === 384, "threshold does not wait for timeout");

  const timeout = make();
  for (let i = 0; i < 50; i++) render(timeout, [], "idle before short sound");
  const arrived = window.currentFrame;
  feed(timeout, [1234]);
  render(timeout, [], "one-sample sound waits");
  feed(timeout, [5678]);
  wait(timeout, arrived);
  check(window.currentFrame - arrived === 4864, "deadline rounds to next callback");
  render(timeout, [1234, 5678], "timeout uses first arrival");
  for (let i = 0; i < 50; i++) render(timeout, [], "idle after underrun");
  feed(timeout, [4321]);
  wait(timeout);
  render(timeout, [4321], "one-sample recovery plays after fresh timeout");

  // An exact callback boundary is not yet an underrun: on-time PCM continues.
  const boundary = make();
  feed(boundary, signal(4864));
  for (let i = 0; i < 4864; i += 128)
    render(boundary, signal(128, i), "exact callback drain");
  feed(boundary, [7]);
  render(boundary, [7], "on-time tail needs no refill");
  feed(boundary, signal(4799));
  render(boundary, [], "actual underrun requires refill");
  feed(boundary, signal(1, 4799));
  render(boundary, signal(128), "recovery threshold starts without timeout");

  // Controlled jitter: 80 ms fits the remaining 97.33-ms cushion; 120 ms does not.
  const jitterResults = [];
  for (const pauseCallbacks of [30, 45]) {
    const p = make();
    const events = [];
    p.port.postMessage = batch => events.push(...batch);
    feed(p, signal(4800));
    render(p, signal(128), "jitter starts at threshold");
    let consumed = 128;
    for (let i = 0; i < pauseCallbacks; i++) {
      const count = Math.min(128, 4800 - consumed);
      render(p, signal(count, consumed), "delivery pause");
      consumed += count;
    }
    const resumeFrame = window.currentFrame;
    feed(p, signal(256, 4800));
    let recoveryFrame = window.currentFrame;
    if (pauseCallbacks === 30) {
      render(p, signal(128, consumed), "short pause continues without buffering");
      consumed += 128;
    } else {
      wait(p, resumeFrame);
      recoveryFrame = window.currentFrame;
      render(p, signal(128, consumed), "long pause recovers after timeout");
      consumed += 128;
    }
    while (consumed < 5056) {
      const count = Math.min(128, 5056 - consumed);
      render(p, signal(count, consumed), "complete jitter tail");
      consumed += count;
    }
    const processes = [...events, ...p.events].filter(e => e.kind === "process");
    const underruns = processes.filter(e => e.underrun > 0 && e.frame < resumeFrame);
    check(underruns.length === (pauseCallbacks === 30 ? 0 : 1), "pause underrun count");
    jitterResults.push({pauseMs: pauseCallbacks * 128 / 48, resumeFrame,
      underruns: underruns.length, underrunFrames: underruns.reduce((n, e) => n + e.underrun, 0),
      recoveryWaitFrames: recoveryFrame - resumeFrame, consumed});
  }

  // A full second prebuffered, split at non-quantum-aligned boundaries.
  const second = make();
  const samples = signal(48000);
  for (let i = 0; i < samples.length; i += 251) feed(second, samples.slice(i, i + 251));
  for (let i = 0; i < samples.length; i += 128) {
    render(second, samples.slice(i, i + 128), "one-second signal");
    check(second.fifoFrames === 48000 - i - 128, "one sample consumed per frame");
  }
  check(window.currentFrame === 48000, "48000 samples occupy exactly one second");
  render(second, [], "silence after one second");

  const results = [];
  // Arrival rate affects only silence or backlog, never the queued sample order.
  for (const speed of [0.5, 1, 2, 5, "changing"]) {
    const p = make();
    const reference = [];
    let sent = 0, consumed = 0, pending = 0, empty = 0;
    const next = () => {
      const before = reference.length;
      const output = render(p, null, `arrival speed ${speed}`);
      const count = before - p.fifoFrames;
      check(count === 0 || count === Math.min(128, before), "whole-rate consumption");
      const expected = reference.splice(0, count);
      compare(output, expected, `arrival speed ${speed}`);
      consumed += expected.length;
      empty += 128 - expected.length;
      check(p.fifoFrames === reference.length, "exact backlog count");
    };
    for (let q = 0; q < 300; q++) {
      const rate = speed === "changing" ? [0.5, 5, 1, 2][Math.floor(q / 75)] : speed;
      pending += 128 * rate;
      // Coalesced packets and alternating arrival gaps.
      if (q % 8 === 0 || q % 8 === 3 || q === 299) {
        const block = signal(pending, sent);
        feed(p, block.slice(0, 17));
        feed(p, block.slice(17));
        reference.push(...block);
        sent += pending;
        pending = 0;
      }
      next();
    }
    const backlog = reference.length;
    if (speed === 0.5) check(empty > 0, "slow arrival underruns");
    if (speed === 2 || speed === 5) check(backlog > 0, "fast arrival builds backlog");
    let drainCalls = 0;
    while (reference.length && drainCalls++ < Math.ceil(backlog / 128) + 40) next();
    check(reference.length === 0, "bounded tail drain");
    check(consumed === sent, "every submitted sample played exactly once");
    render(p, [], "drained silence");
    results.push({speed, sent, consumed, empty, backlog});
  }

  const p = make();
  render(p, [], "initial buffering");
  feed(p, [-32768, -1]);
  feed(p, [0, 1, 32767]);
  wait(p);
  render(p, [-32768, -1, 0, 1, 32767], "complete short tail across packets");
  check(p.fifoFrames === 0, "no stranded tail");
  render(p, [], "underrun after short tail");
  feed(p, [1234]);
  wait(p);
  render(p, [1234], "timeout underrun recovery");
  feed(p, new Int16Array(129));
  feed(p, [4321]);
  wait(p);
  render(p, new Int16Array(128), "intentional zero PCM");
  check(p.fifoFrames === 2, "zero samples consume frames");
  render(p, [0, 4321], "zero tail precedes next sample");

  feed(p, signal(4800));
  render(p, signal(128), "partially consumed packet");
  feed(p, [5, 6]);
  p.port.onmessage({data: {type: "reset"}});
  check(p.fifoFrames === 0 && p.fifo.length === 0 && p.fifoOffset === 0 &&
    p.buffering && p.waitStartFrame === null,
    "reset clears all queue state immediately");
  render(p, [], "reset silence");
  feed(p, [7, 8]);
  // Reset halfway through buffering must also discard the old deadline.
  for (let i = 0; i < 20; i++) render(p, [], "buffering before reset");
  p.port.onmessage({data: {type: "reset"}});
  render(p, [], "reset discards buffered PCM");
  feed(p, [7, 8]);
  wait(p);
  render(p, [7, 8], "fresh PCM after reset");

  feed(p, [9, 10]);
  for (const message of [null, undefined, false, {}, {type: "unknown"},
    {type: "pcm"}, {type: "pcm", samples: [1, 2]},
    {type: "pcm", samples: new Int16Array([1, 2])},
    {type: "pcm", samples: new ArrayBuffer(0)},
    {type: "pcm", samples: new ArrayBuffer(3)},
    {type: "pcm", samples: new ArrayBuffer((8 * 48000 + 1) * 2)}]) {
    p.port.onmessage({data: message});
    check(p.fifoFrames === 2, "invalid input leaves existing audio intact");
  }
  wait(p);
  render(p, [9, 10], "valid PCM survives malformed input");

  const invalid = make();
  invalid.receive({type: "pcm", samples: new ArrayBuffer(3)});
  invalid.receive({type: "pcm", samples: new ArrayBuffer((8 * 48000 + 1) * 2)});
  for (let i = 0; i < 50; i++) render(invalid, [], "invalid input never starts timer");
  check(invalid.waitStartFrame === null, "invalid input leaves deadline unset");
  feed(invalid, [11]);
  wait(invalid);
  render(invalid, [11], "first valid sample starts fresh deadline");

  // Overflow crosses both a partial packet and a whole packet, dropping oldest.
  const overflow = make();
  const limit = 8 * 48000;
  const full = signal(limit);
  feed(overflow, full.slice(0, 200));
  feed(overflow, full.slice(200, 300));
  feed(overflow, full.slice(300));
  render(overflow, full.slice(0, 128), "head before overflow");
  const extra = signal(333, limit);
  feed(overflow, extra);
  check(overflow.fifoFrames === limit, "eight-second memory bound");
  const retained = [...full.slice(333), ...extra];
  for (let i = 0; i < retained.length; i += 128)
    render(overflow, retained.slice(i, i + 128), "oldest excess dropped");
  check(overflow.fifoFrames === 0, "overflow tail drains completely");

  const diagnostic = make();
  const events = [];
  diagnostic.port.postMessage = batch => events.push(...batch);
  feed(diagnostic, new Int16Array(limit));
  feed(diagnostic, [1]);
  diagnostic.receive({type: "reset"});
  feed(diagnostic, [0, 32767]);
  wait(diagnostic);
  render(diagnostic, [0, 32767], "diagnostic short tail");
  render(diagnostic, [], "diagnostic idle");
  check(events.some(e => e.kind === "overflow" && e.discarded === 1), "overflow diagnostic");
  check(events.some(e => e.kind === "reset" && e.discarded === limit), "reset diagnostic");
  check(events.some(e => e.kind === "process" && e.consumed === 2 &&
    e.rendered === 2 && e.empty === 126 && e.underrun === 126 && e.buffering === 0 &&
    e.queue === 0 && e.queueMs === 0),
    "queue diagnostics count intentional zeros as rendered");
  check(events.some(e => e.kind === "process" && e.buffering === 128 &&
    e.underrun === 0 && e.consumed === 0 && e.queue === 2), "buffering is not underrun");
  check(events.some(e => e.kind === "play" && e.reason === "timeout" && e.waited === 4864),
    "timeout diagnostic");
  check(events.every(e => Number.isInteger(e.frame) && !("rate" in e) &&
    !("baseRate" in e)), "only fixed-rate diagnostics");
  const quiet = make(false);
  quiet.port.postMessage = () => { throw new Error("diagnostics must be opt-in"); };
  for (let i = 0; i < 40; i++) render(quiet, [], "quiet idle");
  check(quiet.events.length === 0, "no diagnostic history when disabled");
  return {rates: results, jitter: jitterResults};
}
