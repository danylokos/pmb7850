"use strict";

const MAX_FIFO_FRAMES = 8 * 48000;
const PREBUFFER_FRAMES = 4800; // 100 ms at the required 48-kHz AudioContext rate.

class CemuAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.diagnostics = options?.processorOptions?.diagnostics === true;
    this.events = [];
    this.calls = 0;
    this.reset();
    this.port.onmessage = event => this.receive(event.data);
  }

  report(event) {
    if (this.diagnostics) this.events.push({frame: currentFrame, ...event});
  }

  reset() {
    this.report({kind: "reset", discarded: this.fifoFrames || 0});
    this.fifo = [];
    this.fifoOffset = 0;
    this.fifoFrames = 0;
    this.buffering = true;
    this.waitStartFrame = null;
  }

  drop(frames) {
    let remaining = Math.min(frames, this.fifoFrames);
    while (remaining > 0 && this.fifo.length) {
      const available = this.fifo[0].length - this.fifoOffset;
      const count = Math.min(remaining, available);
      this.fifoOffset += count;
      this.fifoFrames -= count;
      remaining -= count;
      if (this.fifoOffset === this.fifo[0].length) {
        this.fifo.shift();
        this.fifoOffset = 0;
      }
    }
  }

  receive(message) {
    if (!message) return;
    if (message.type === "reset") {
      this.reset();
      return;
    }
    if (message.type !== "pcm" || !(message.samples instanceof ArrayBuffer) ||
        !message.samples.byteLength || message.samples.byteLength % 2) return;
    const samples = new Int16Array(message.samples);
    if (samples.length > MAX_FIFO_FRAMES) {
      this.report({kind: "overflow", discarded: samples.length});
      return;
    }
    const excess = this.fifoFrames + samples.length - MAX_FIFO_FRAMES;
    if (excess > 0) {
      this.drop(excess);
      this.report({kind: "overflow", discarded: excess});
    }
    this.fifo.push(samples);
    this.fifoFrames += samples.length;
    if (this.buffering && this.waitStartFrame === null)
      this.waitStartFrame = currentFrame;
    this.report({kind: "receive", frames: samples.length, queue: this.fifoFrames});
  }

  process(_inputs, outputs) {
    const channels = outputs[0] || [];
    const frames = channels[0]?.length || 0;
    for (const channel of channels) channel.fill(0);
    let rendered = 0;
    const before = this.fifoFrames;
    if (frames && this.buffering && this.waitStartFrame !== null &&
        (before >= PREBUFFER_FRAMES || currentFrame - this.waitStartFrame >= PREBUFFER_FRAMES)) {
      this.report({kind: "play", reason: before >= PREBUFFER_FRAMES ? "threshold" : "timeout",
        waited: currentFrame - this.waitStartFrame, queue: before});
      this.buffering = false;
      this.waitStartFrame = null;
    }
    const buffering = this.buffering ? frames : 0;
    // Final PCM and the AudioContext are both 48 kHz: one sample per frame.
    while (!this.buffering && rendered < frames && this.fifoFrames) {
      const head = this.fifo[0];
      const count = Math.min(frames - rendered, head.length - this.fifoOffset);
      for (let i = 0; i < count; i++) {
        const sample = head[this.fifoOffset + i] / 32768;
        for (const channel of channels) channel[rendered + i] = sample;
      }
      rendered += count;
      this.drop(count);
    }
    const underrun = this.buffering ? 0 : frames - rendered;
    // Refill only after missing an output frame, not merely falling below the
    // threshold (or draining exactly at a callback boundary).
    if (underrun) {
      this.buffering = true;
      this.waitStartFrame = null;
    }
    this.report({kind: "process", consumed: before - this.fifoFrames,
      rendered, empty: frames - rendered, buffering, underrun, queue: this.fifoFrames,
      queueMs: 1000 * this.fifoFrames / 48000});
    if (this.diagnostics && ++this.calls % 40 === 0) {
      this.port.postMessage(this.events);
      this.events = [];
    }
    return true;
  }
}

registerProcessor("cemu-audio", CemuAudioProcessor);
