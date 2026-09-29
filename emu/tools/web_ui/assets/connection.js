export function createConnection({onMessage, onOpen, onConnecting, onRetire}) {
  let socket = null;
  let identity = null;
  let validated = false;
  function validIdentity(message) {
    return (message.run_id === null || /^[0-9a-f]{32}$/.test(message.run_id)) &&
      Number.isSafeInteger(message.generation) && message.generation >= 0;
  }
  function matches(message) {
    return identity && message.run_id === identity.run_id &&
      message.generation === identity.generation;
  }
  let reconnectTimer = null;
  let startupTimer = null;
  let startupDeadline = null;
  let pageActive = true;
  const STARTUP_TIMEOUT_MS = 5000;
  const WS_FRAME = 1;

  function send(message) {
    if (validated && identity.run_id !== null && socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({...message, ...identity}));
    }
  }

  function clearStartupTimer() {
    clearTimeout(startupTimer);
    startupTimer = null;
    startupDeadline = null;
  }

  function retireSocket(attempt) {
    if (socket !== attempt) return;
    // Invalidate before close(): old events must never affect a replacement.
    socket = null;
    validated = false;
    clearStartupTimer();
    onRetire();
    clearTimeout(reconnectTimer);
    reconnectTimer = pageActive ? setTimeout(connect, 500) : null;
    attempt.close();
  }

  function checkStartupDeadline() {
    if (socket && startupDeadline !== null && Date.now() >= startupDeadline) {
      retireSocket(socket);
    }
  }

  function connect() {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
    if (!pageActive || socket) return;
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    onConnecting();
    const attempt = new WebSocket(`${scheme}://${location.host}/ws`);
    socket = attempt;
    startupDeadline = Date.now() + STARTUP_TIMEOUT_MS;
    startupTimer = setTimeout(() => retireSocket(attempt), STARTUP_TIMEOUT_MS);
    attempt.binaryType = "arraybuffer";
    attempt.addEventListener("open", () => {
      if (socket !== attempt) return;
      onOpen();
    });
    attempt.addEventListener("message", (event) => {
      if (socket !== attempt) return;
      if (typeof event.data === "string") {
        let message;
        try { message = JSON.parse(event.data); } catch { return; }
        if (message.type === "state") {
          if (!validIdentity(message)) return;
          if (validated && identity && (message.generation < identity.generation ||
              (message.generation === identity.generation && message.run_id !== identity.run_id))) return;
          if (message.run_id !== null && (!message.stats || !message.lifecycle ||
              !Array.isArray(message.keys) || !message.width || !message.height)) return;
          identity = {run_id: message.run_id, generation: message.generation};
          validated = true;
          clearStartupTimer();
          onMessage(message);
        } else if (validated && matches(message)) {
          onMessage(message);
        }
      } else {
        if (!validated || !identity) return;
        const bytes = new Uint8Array(event.data);
        if (bytes.length < 25) return;
        const run_id = Array.from(bytes.slice(1, 17), b => b.toString(16).padStart(2, "0")).join("");
        const generation = Number(new DataView(event.data).getBigUint64(17, true));
        if (!matches({run_id, generation})) return;
        if (bytes[0] === WS_FRAME)
          onMessage({type: "frame", buffer: event.data.slice(25)});
        else {
          const payload = new Uint8Array(bytes.length - 24);
          payload[0] = bytes[0];
          payload.set(bytes.slice(25), 1);
          onMessage({type: "audio", buffer: payload.buffer});
        }
      }
    });
    attempt.addEventListener("error", () => retireSocket(attempt));
    attempt.addEventListener("close", () => retireSocket(attempt));
  }

  function pause() {
    pageActive = false;
    if (socket) retireSocket(socket);
    clearStartupTimer();
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }

  function resume() {
    pageActive = true;
    if (!socket && reconnectTimer === null) connect();
    else checkStartupDeadline();
  }

  return {send, connect, pause, resume, checkStartupDeadline};
}
