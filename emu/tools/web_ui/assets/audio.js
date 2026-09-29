export function createAudio() {
  const WS_AUDIO_RESET = 2;
  const WS_SPEAKER_PCM = 3;
  const MAX_PENDING_AUDIO_BYTES = 2 * 1024 * 1024;
  let audioAvailable = false;
  let audioContext = null;
  let audioNode = null;
  let audioStarting = null;
  let pendingAudio = [];
  let pendingAudioBytes = 0;

  function resetAudio() {
    pendingAudio = [];
    pendingAudioBytes = 0;
    audioNode?.port.postMessage({type: "reset"});
  }

  function queueAudioEnvelope(buffer) {
    const copy = buffer.slice(0);
    const now = performance.now();
    while (pendingAudio.length &&
           (now - pendingAudio[0].received > 100 ||
            pendingAudioBytes + copy.byteLength > MAX_PENDING_AUDIO_BYTES)) {
      pendingAudioBytes -= pendingAudio.shift().buffer.byteLength;
    }
    if (copy.byteLength <= MAX_PENDING_AUDIO_BYTES) {
      pendingAudio.push({buffer: copy, received: now});
      pendingAudioBytes += copy.byteLength;
    }
  }

  function dispatchAudioEnvelope(buffer) {
    const bytes = new Uint8Array(buffer);
    if (!bytes.length) return;
    const kind = bytes[0];
    if (kind === WS_AUDIO_RESET) {
      resetAudio();
      return;
    }
    if (!audioAvailable || kind !== WS_SPEAKER_PCM) return;
    if (!audioNode) {
      queueAudioEnvelope(buffer);
      return;
    }
    const samples = buffer.slice(1);
    if (!samples.byteLength || samples.byteLength % 2) return;
    audioNode.port.postMessage({type: "pcm", samples}, [samples]);
  }

  async function ensureAudio() {
    if (!audioAvailable) return;
    if (!audioStarting) {
      audioStarting = (async () => {
        try {
          audioContext = new AudioContext({latencyHint: "interactive", sampleRate: 48000});
          if (audioContext.sampleRate !== 48000)
            throw new Error("browser did not provide a 48-kHz AudioContext");
          await audioContext.audioWorklet.addModule("/audio-worklet.js");
          audioNode = new AudioWorkletNode(audioContext, "cemu-audio");
          audioNode.connect(audioContext.destination);
          const queued = pendingAudio;
          pendingAudio = [];
          pendingAudioBytes = 0;
          for (const envelope of queued)
            if (performance.now() - envelope.received <= 100)
              dispatchAudioEnvelope(envelope.buffer);
        } catch (error) {
          audioNode?.disconnect();
          audioNode = null;
          if (audioContext) await audioContext.close().catch(() => {});
          audioContext = null;
          throw error;
        }
      })();
      // All concurrent consumers observe the same initialization failure.
      audioStarting.catch(() => { audioStarting = null; });
    }
    await audioStarting;
    if (audioContext.state === "suspended") await audioContext.resume();
    if (audioContext.state !== "running") throw new Error("audio playback is not running");
  }

  async function acquireFeed() {
    if (!audioAvailable) return null;
    let destination;
    try {
      await ensureAudio();
      const node = audioNode;
      destination = audioContext.createMediaStreamDestination();
      node.connect(destination);
      let released = false;
      return {stream: destination.stream, release() {
        if (released) return;
        released = true;
        try { node.disconnect(destination); }
        finally { destination.stream.getTracks().forEach(track => track.stop()); }
      }};
    } catch (error) {
      destination?.stream.getTracks().forEach(track => track.stop());
      throw new Error(`Audio capture could not start: ${error.message}`);
    }
  }

  function setAvailable(available) {
    audioAvailable = available;
    if (!audioAvailable) resetAudio();
  }

  document.addEventListener("pointerdown", () => { void ensureAudio().catch(() => {}); },
    {passive: true});
  document.addEventListener("keydown", () => { void ensureAudio().catch(() => {}); },
    {passive: true});

  return {setAvailable, acquireFeed, dispatchEnvelope: dispatchAudioEnvelope, reset: resetAudio};
}
