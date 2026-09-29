export function createPhone() {
  const canvas = document.getElementById("lcd");
  const context = canvas.getContext("2d", {alpha: false});
  const phone = document.querySelector(".phone");
  const phoneModel = document.getElementById("phone-model");

  let ready = false;

  function drawBlank() {
    ready = false;
    context.fillStyle = "#cad5a7";
    context.fillRect(0, 0, canvas.width, canvas.height);
  }

  drawBlank();

  function updateState(state) {
    if (typeof state.model === "string" && state.model.length > 0) {
      const model = state.model.toUpperCase();
      phoneModel.textContent = model;
      phone.setAttribute("aria-label", `${model} phone controls`);
    }
    if (Number.isInteger(state.width) && Number.isInteger(state.height) &&
        state.width > 0 && state.height > 0 &&
        (canvas.width !== state.width || canvas.height !== state.height)) {
      canvas.width = state.width;
      canvas.height = state.height;
      canvas.style.aspectRatio = `${state.width} / ${state.height}`;
      drawBlank();
    }
  }

  function drawFrame(buffer) {
    if (buffer.byteLength !== canvas.width * canvas.height * 3) return;
    const rgb = new Uint8Array(buffer);
    const image = context.createImageData(canvas.width, canvas.height);
    for (let source = 0, target = 0; source < rgb.length; source += 3, target += 4) {
      image.data[target] = rgb[source];
      image.data[target + 1] = rgb[source + 1];
      image.data[target + 2] = rgb[source + 2];
      image.data[target + 3] = 255;
    }
    context.putImageData(image, 0, 0);
    ready = true;
  }

  return {updateState, drawFrame, clear: drawBlank, canvas,
    get ready() { return ready; },
    get model() { return phoneModel.textContent; }};
}
