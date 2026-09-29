export function createCapture({phone, audio, maxDurationMs = 10 * 60 * 1000,
                               maxBytes = 128 * 1024 * 1024}) {
  const toolbar = document.getElementById("capture-toolbar");
  const screenshotButton = document.getElementById("capture-screenshot");
  const recordButton = document.getElementById("capture-record");
  const timer = document.getElementById("capture-timer");
  const notice = document.getElementById("capture-notice");
  let connected = false;
  let attachment = null;
  let job = null;
  const urls = new Map();
  const supportedType = types => typeof MediaRecorder !== "undefined" &&
    typeof MediaRecorder.isTypeSupported === "function" ?
    types.find(type => MediaRecorder.isTypeSupported(type)) : null;
  const audioMimeType = supportedType(["video/webm;codecs=vp8,opus", "video/webm"]);
  const videoMimeType = supportedType(["video/webm;codecs=vp8", "video/webm"]);
  const unavailable = !phone.canvas.captureStream ? "This browser cannot capture canvas video." :
    !audioMimeType && !videoMimeType ? "This browser cannot record WebM video." : "";

  function inform(message) {
    notice.textContent = message;
    notice.hidden = !message;
  }

  function refresh() {
    const state = job?.state || "idle";
    toolbar.dataset.state = state;
    screenshotButton.disabled = !phone.ready;
    recordButton.disabled = state === "stopping" || (!job && (!connected || !phone.ready || !!unavailable));
    const label = job ? (state === "starting" ? "Cancel recording startup" : "Stop recording") : "Record LCD";
    recordButton.setAttribute("aria-label", label);
    recordButton.title = job ? label : unavailable || (!connected ? "Recording requires a connected session" :
      !phone.ready ? "Waiting for an LCD frame" : "Record LCD");
    timer.hidden = state !== "recording" && state !== "stopping";
  }

  function filename(extension) {
    const model = phone.model.replace(/[^a-z0-9_-]/gi, "_") || "LCD";
    return `${model}-${new Date().toISOString().replace(/[:.]/g, "-")}.${extension}`;
  }

  function revoke(url) {
    clearTimeout(urls.get(url));
    urls.delete(url);
    URL.revokeObjectURL(url);
  }

  function download(blob, name) {
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    // Allow the browser to consume the download before revoking its URL.
    urls.set(url, setTimeout(() => revoke(url), 60_000));
  }

  function screenshot() {
    if (!phone.ready) return;
    const name = filename("png");
    // toBlob snapshots before its asynchronous callback, including on reattachment.
    phone.canvas.toBlob(blob => {
      if (blob) download(blob, name);
      else inform("The LCD screenshot could not be created.");
    }, "image/png");
  }

  function release(current) {
    clearInterval(current.redrawTimer);
    clearTimeout(current.limitTimer);
    clearTimeout(current.stopTimer);
    current.stream?.getTracks().forEach(track => track.stop());
    current.feed?.release();
    current.feed = null;
    current.canvas = null;
  }

  function finish(current) {
    release(current);
    if (current.chunks.length)
      download(new Blob(current.chunks, {type: current.recorder.mimeType || "video/webm"}), current.name);
    else
      inform("The browser produced no video data. Try a longer recording.");
    current.chunks = [];
    current.recorder = null;
    if (job === current) job = null;
    refresh();
  }

  function stop(reason = "") {
    const current = job;
    if (!current || current.state === "stopping") return;
    if (reason) inform(reason);
    if (current.state === "starting") {
      job = null;
      release(current);
    } else {
      current.state = "stopping";
      clearTimeout(current.limitTimer);
      // Very short native recordings can end before an encoder accepts its first
      // frame/audio packet. Freeze the picture and give it a brief initial drain.
      // Tracks stay live until the final encoded chunk has arrived.
      const drainMs = Math.max(0, 200 - (performance.now() - current.started));
      current.stopTimer = setTimeout(() => {
        clearInterval(current.redrawTimer);
        if (current.recorder?.state !== "inactive") current.recorder?.stop();
      }, drainMs);
    }
    refresh();
  }

  async function start() {
    if (job || !connected || !phone.ready) return;
    if (unavailable) { inform(unavailable); return; }
    inform("");
    const current = {state: "starting", chunks: [], bytes: 0, name: filename("webm")};
    job = current;
    refresh();
    try {
      const canvas = document.createElement("canvas");
      current.canvas = canvas;
      canvas.width = phone.canvas.width * 4;
      canvas.height = phone.canvas.height * 4;
      const context = canvas.getContext("2d", {alpha: false});
      context.imageSmoothingEnabled = false;
      const draw = () => context.drawImage(
        current.state === "stopping" ? canvas : phone.canvas,
        0, 0, canvas.width, canvas.height);
      draw();
      current.stream = canvas.captureStream(30);
      const feed = await audio.acquireFeed();
      // Stop or attachment changes may interrupt asynchronous audio startup.
      if (job !== current) { feed?.release(); return; }
      current.feed = feed;
      for (const track of feed?.stream.getAudioTracks() || []) current.stream.addTrack(track);
      // Do not declare an audio codec for video-only streams (Firefox requires
      // the requested codec list to match the tracks supplied to the recorder).
      const recordingType = current.stream.getAudioTracks().length ? audioMimeType : videoMimeType;
      if (!recordingType) throw new Error("This browser cannot record WebM with the available audio/video tracks.");
      const recorder = new MediaRecorder(current.stream, {mimeType: recordingType});
      current.recorder = recorder;
      recorder.ondataavailable = event => {
        if (!event.data.size) return;
        current.chunks.push(event.data);
        current.bytes += event.data.size;
        if (current.bytes >= maxBytes && job === current)
          stop("Recording stopped at the 128 MiB encoded-data limit; the clip was downloaded.");
      };
      recorder.onstop = () => finish(current);
      recorder.onerror = event => {
        inform(`Recording failed: ${event.error?.message || "browser encoder error"}. Available video will be downloaded.`);
        stop();
      };
      recorder.start(1000);
      current.state = "recording";
      current.started = performance.now();
      timer.textContent = "00:00";
      // Timers continue (with browser throttling) in hidden tabs; animation frames do not.
      const tick = () => {
        draw();
        current.stream.getVideoTracks()[0]?.requestFrame?.();
        const seconds = Math.floor((performance.now() - current.started) / 1000);
        timer.textContent = `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
        if (performance.now() - current.started >= maxDurationMs)
          stop("Recording stopped at the 10-minute limit; the clip was downloaded.");
      };
      tick();
      current.redrawTimer = setInterval(tick, 1000 / 30);
      current.limitTimer = setTimeout(() => stop("Recording stopped at the 10-minute limit; the clip was downloaded."), maxDurationMs);
      refresh();
    } catch (error) {
      release(current);
      current.chunks = [];
      if (job === current) {
        job = null;
        inform(`Recording could not start: ${error.message}`);
        refresh();
      }
    }
  }

  function updateState(state) {
    const next = `${state.run_id}:${state.generation}`;
    if (attachment !== null && attachment !== next) stop("Recording stopped because the attachment changed; the clip was downloaded.");
    attachment = next;
    if ((Number.isInteger(state.width) && state.width !== phone.canvas.width) ||
        (Number.isInteger(state.height) && state.height !== phone.canvas.height))
      stop("Recording stopped because the LCD dimensions changed; the clip was downloaded.");
    connected = state.bridge === "connected" && state.lifecycle?.phase !== "stopped";
    if (!connected) stop("Recording stopped because the emulator disconnected or stopped; the clip was downloaded.");
    refresh();
  }

  function disconnect() {
    connected = false;
    stop("Recording stopped because the emulator disconnected or stopped; the clip was downloaded.");
    refresh();
  }

  function align() {
    const rect = document.querySelector(".phone").getBoundingClientRect();
    const center = rect.top + rect.height / 2;
    toolbar.style.top = `${center}px`;
    const top = document.querySelector(".mobile-tabs").getBoundingClientRect().bottom ||
      document.querySelector(".app-bar").getBoundingClientRect().bottom;
    const dock = document.getElementById("asc0-panel").getBoundingClientRect();
    const bottom = dock.height ? dock.top : innerHeight;
    toolbar.style.setProperty("--capture-available-height", `${Math.max(0, 2 * Math.min(center - top, bottom - center))}px`);
  }
  const observer = new ResizeObserver(align);
  for (const selector of [".phone", ".workspace", ".app-bar", "#asc0-panel"])
    observer.observe(document.querySelector(selector));
  new MutationObserver(align).observe(document.querySelector(".workspace"), {attributes: true});
  window.addEventListener("resize", align);
  window.addEventListener("scroll", align, {passive: true});
  for (const type of ["keydown", "keyup"]) toolbar.addEventListener(type, event => event.stopPropagation());
  screenshotButton.addEventListener("click", screenshot);
  recordButton.addEventListener("click", () => job ? stop() : void start());
  window.addEventListener("pagehide", () => {
    disconnect();
    for (const url of urls.keys()) revoke(url);
  });
  if (unavailable) inform(unavailable);
  refresh();
  align();
  return {updateState, disconnect, refresh};
}
