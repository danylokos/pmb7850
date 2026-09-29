export function createAsc0() {
  const asc0Output = document.getElementById("asc0-output");
  const asc0Copy = document.getElementById("asc0-copy");
  const asc0Minimize = document.getElementById("asc0-minimize");
  const asc0Panel = document.getElementById("asc0-panel");
  const asc0Text = document.createTextNode("");
  asc0Output.append(asc0Text);

  function setAsc0Tx(text) {
    if (asc0Text.data !== text) asc0Text.data = text;
    asc0Copy.disabled = text.length === 0;
    asc0Output.scrollTop = asc0Output.scrollHeight;
  }

  async function copyAsc0Output() {
    try {
      await navigator.clipboard.writeText(asc0Output.textContent);
      asc0Copy.title = "Copied";
    } catch {
      asc0Copy.title = "Copy failed";
    }
    setTimeout(() => { asc0Copy.title = "Copy ASC0 TX output"; }, 1200);
  }

  function setAsc0Minimized(minimized) {
    document.body.classList.toggle("asc0-minimized", minimized);
    asc0Output.hidden = minimized;
    asc0Minimize.setAttribute("aria-expanded", String(!minimized));
    asc0Minimize.title = minimized ? "Restore ASC0 TX" : "Minimize ASC0 TX";
    asc0Minimize.setAttribute("aria-label", asc0Minimize.title);
  }

  function appendAsc0Tx(text) {
    if (!text) return;
    const wasAtBottom = asc0Output.scrollHeight - asc0Output.scrollTop <=
      asc0Output.clientHeight + 2;
    const previousScrollTop = asc0Output.scrollTop;
    asc0Text.appendData(text);
    asc0Copy.disabled = false;
    if (wasAtBottom) asc0Output.scrollTop = asc0Output.scrollHeight;
    else asc0Output.scrollTop = previousScrollTop;
  }

  function setAvailable(available) {
    asc0Panel.hidden = !available;
    document.body.classList.toggle("asc0-unavailable", !available);
  }

  asc0Copy.addEventListener("click", copyAsc0Output);
  asc0Minimize.addEventListener("click", () => setAsc0Minimized(
    !document.body.classList.contains("asc0-minimized")));

  return {setAvailable, setText: setAsc0Tx, appendText: appendAsc0Tx};
}
