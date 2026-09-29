import {createConnection} from "./connection.js";
import {createPhone} from "./phone.js";
import {createKeypad} from "./keypad.js";
import {createStatus} from "./status.js";
import {createAudio} from "./audio.js";
import {createAsc0} from "./asc0.js";
import {createCapture} from "./capture.js";
import {createFiles} from "./files.js";

const phone = createPhone();
const status = createStatus(50);
const audio = createAudio();
const capture = createCapture({phone, audio});
const asc0 = createAsc0();
const files = createFiles();
const keypad = createKeypad((message) => connection.send(message));
let runId = null;
let generation = null;
const connection = createConnection({
  onMessage(message) {
    switch (message.type) {
      case "state":
        capture.updateState(message);
        // A restarted bridge may not have identified any native run yet.
        if (runId && message.run_id === null) {
          capture.disconnect();
          status.setConnectionState("error");
          audio.reset();
          files.invalidate();
          break;
        }
        if (message.run_id && runId !== message.run_id) {
          phone.clear();
          status.clear();
          asc0.setText("");
          audio.reset();
          files.invalidate(true);
        }
        if (generation !== message.generation || runId !== message.run_id) {
          files.attach(message.run_id, message.generation);
          generation = message.generation;
        }
        runId = message.run_id;
        status.updateState(message);
        if (typeof message.asc0_available === "boolean") {
          asc0.setAvailable(message.asc0_available);
          files.setAvailable(message.asc0_available);
        }
        if (typeof message.audio_available === "boolean")
          audio.setAvailable(message.audio_available);
        phone.updateState(message);
        capture.refresh();
        keypad.updateState(message);
        if (typeof message.asc0_tx === "string") asc0.setText(message.asc0_tx);
        if (message.bridge === "connected") keypad.replayHeld();
        else files.invalidate();
        break;
      case "metrics":
        status.updateMetrics(message.stats);
        break;
      case "lifecycle":
        status.updateLifecycle(message.lifecycle);
        if (message.lifecycle?.phase === "stopped") capture.disconnect();
        break;
      case "transport":
        status.setConnectionState(message.bridge === "connected" ? "connected" : "error");
        if (message.bridge !== "connected") {
          capture.disconnect();
          audio.reset();
          files.invalidate();
        }
        break;
      case "asc0_tx":
        if (typeof message.text === "string") asc0.appendText(message.text);
        break;
      case "files_changed":
        files.refresh();
        break;
      case "frame":
        phone.drawFrame(message.buffer);
        capture.refresh();
        break;
      case "audio":
        audio.dispatchEnvelope(message.buffer);
        break;
    }
  },
  onOpen: () => {},
  onConnecting: () => status.setConnectionState("connecting"),
  onRetire() {
    capture.disconnect();
    audio.reset();
    files.invalidate();
    status.setConnectionState("error");
  },
});

window.addEventListener("blur", keypad.releaseAll);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) keypad.releaseAll();
  else connection.checkStartupDeadline();
});
window.addEventListener("pagehide", () => {
  // Release while the transport is still open, before retiring the socket.
  keypad.releaseAll();
  connection.pause();
});
window.addEventListener("pageshow", connection.resume);
connection.connect();
