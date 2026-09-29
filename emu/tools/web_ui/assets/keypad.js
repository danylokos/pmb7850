export function createKeypad(send) {
  const phone = document.querySelector(".phone");
  const keyElements = new Map();
  const held = new Set();
  const pointerKeys = new Map();
  let availableKeys = new Set();

  for (const element of document.querySelectorAll("[data-key]")) {
    keyElements.set(element.dataset.key, element);
    element.hidden = true;
    element.disabled = true;
  }

  function setPressed(name, pressed) {
    const element = keyElements.get(name);
    if (!element || !availableKeys.has(name)) return;
    if (pressed && !held.has(name)) {
      held.add(name);
      element.classList.add("pressed");
      send({type: "key", key: name, pressed: true});
    } else if (!pressed && held.has(name)) {
      held.delete(name);
      element.classList.remove("pressed");
      send({type: "key", key: name, pressed: false});
    }
  }

  function releaseAll() {
    pointerKeys.clear();
    for (const name of held) {
      keyElements.get(name)?.classList.remove("pressed");
    }
    held.clear();
    send({type: "release_all"});
  }

  function updateState(state) {
    if (Array.isArray(state.keys) && state.keys.every((name) => typeof name === "string")) {
      const nextKeys = new Set(state.keys);
      for (const name of held) {
        if (!nextKeys.has(name)) {
          keyElements.get(name)?.classList.remove("pressed");
          held.delete(name);
        }
      }
      availableKeys = nextKeys;
      for (const [name, element] of keyElements) {
        const available = availableKeys.has(name);
        element.hidden = !available;
        element.disabled = !available;
      }
      phone.classList.toggle("has-horizontal-nav",
        availableKeys.has("left") || availableKeys.has("right"));
    }
  }

  for (const [name, element] of keyElements) {
    element.addEventListener("pointerdown", (event) => {
      event.preventDefault();
      const previous = pointerKeys.get(event.pointerId);
      if (previous && previous !== name) setPressed(previous, false);
      pointerKeys.set(event.pointerId, name);
      try {
        element.setPointerCapture(event.pointerId);
      } catch {
        // The window-level release handler remains the fallback when capture is
        // unavailable or the browser rejects it for this pointer.
      }
      setPressed(name, true);
    });
    element.addEventListener("lostpointercapture", (event) => {
      if (pointerKeys.get(event.pointerId) !== name) return;
      pointerKeys.delete(event.pointerId);
      setPressed(name, false);
    });
  }

  function releasePointer(event) {
    const name = pointerKeys.get(event.pointerId);
    if (!name) return;
    event.preventDefault();
    pointerKeys.delete(event.pointerId);
    setPressed(name, false);
  }

  // Capture-phase window listeners preserve the release even if pointer capture
  // is unavailable or pointer-up lands outside the original keypad element.
  window.addEventListener("pointerup", releasePointer, true);
  window.addEventListener("pointercancel", releasePointer, true);

  const keyboardMap = {
    "0": "0", "1": "1", "2": "2", "3": "3", "4": "4",
    "5": "5", "6": "6", "7": "7", "8": "8", "9": "9",
    "*": "star", "#": "hash", ArrowUp: "up", ArrowDown: "down",
    ArrowLeft: "left", ArrowRight: "right", Enter: "send", End: "power",
    q: "soft-left", Q: "soft-left", e: "soft-right", E: "soft-right",
  };

  window.addEventListener("keydown", (event) => {
    const name = keyboardMap[event.key];
    if (!name || event.repeat) return;
    event.preventDefault();
    setPressed(name, true);
  });
  window.addEventListener("keyup", (event) => {
    const name = keyboardMap[event.key];
    if (!name) return;
    event.preventDefault();
    setPressed(name, false);
  });

  function replayHeld() {
    for (const name of held) send({type: "key", key: name, pressed: true});
  }

  return {updateState, releaseAll, replayHeld};
}
