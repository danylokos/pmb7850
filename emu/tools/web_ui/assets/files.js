export function createFiles() {
  const workspace = document.querySelector(".workspace");
  const filesStatus = document.getElementById("files-status");
  const filesConnect = document.getElementById("files-connect");
  const filesRefresh = document.getElementById("files-refresh");
  const filesUpload = document.getElementById("files-upload");
  const filesUploadInput = document.getElementById("files-upload-input");
  const filesMkdir = document.getElementById("files-mkdir");
  const filesList = document.getElementById("files-list");
  const filesBreadcrumbs = document.getElementById("files-breadcrumbs");
  const filesError = document.getElementById("files-error");
  const tabPhone = document.getElementById("tab-phone");
  const tabFiles = document.getElementById("tab-files");
  const mobileTabs = document.querySelector(".mobile-tabs");
  const filesPanel = document.getElementById("files-panel");
  const transfers = document.createElement("div");
  transfers.setAttribute("aria-live", "polite");
  filesError.after(transfers);
  function randomId() {
    // getRandomValues also works on an HTTP LAN origin.
    return Array.from(crypto.getRandomValues(new Uint8Array(24)),
      value => value.toString(16).padStart(2, "0")).join("");
  }
  const tab = randomId();
  const monitors = new Set();
  let filesState = "disconnected";
  let currentPath = "A:";
  let filesInitialized = false;
  let attachment = {run_id: null, generation: 0};
  let epoch = 0;
  let controller = new AbortController();

  function invalidate(clear = false) {
    epoch++;
    for (const stop of monitors) stop();
    transfers.replaceChildren();
    controller.abort();
    controller = new AbortController();
    filesInitialized = false;
    updateFilesState("disconnected");
    if (clear) {
      currentPath = "A:";
      filesList.replaceChildren();
      filesBreadcrumbs.replaceChildren();
      filesUploadInput.value = "";
      showFilesError();
    }
  }

  function attach(run_id, generation) {
    invalidate();
    attachment = {run_id, generation};
  }

  function isStale(error) {
    return error.name === "AbortError";
  }

  async function attachmentFetch(url, options = {}) {
    const ticket = epoch;
    const check = () => {
      if (ticket !== epoch) throw new DOMException("Retired attachment", "AbortError");
    };
    const response = await fetch(url, {...options, signal: controller.signal,
      headers: {...options.headers, "X-CEMU-Run": attachment.run_id || "",
        "X-CEMU-Generation": String(attachment.generation)}}).catch(error => {
          check(); throw error;
        });
    check();
    function guard(response) {
      for (const method of ["json", "text"]) {
        const read = response[method].bind(response);
        response[method] = async () => { const value = await read(); check(); return value; };
      }
      const clone = response.clone.bind(response);
      response.clone = () => guard(clone());
      return response;
    }
    return guard(response);
  }

  function beginTransfer(name) {
    const id = randomId();
    const row = document.createElement("div");
    const label = document.createElement("span");
    const cancel = fileAction("Cancel", async () => {
      try {
        await apiResponse(await attachmentFetch(`/api/files/transfers/${id}/cancel`, {
          method: "POST", headers: {"X-CEMU-Request": "1", "X-CEMU-Tab": tab},
        }));
        cancel.disabled = true;
      } catch (error) { await recoverFiles(error); }
    });
    label.textContent = `${name}: queued `;
    cancel.disabled = true;
    row.append(label, cancel);
    transfers.append(row);
    let timer;
    let stopped = false;
    const stop = () => { stopped = true; clearTimeout(timer); monitors.delete(stop); };
    monitors.add(stop);
    const finish = (state) => {
      stop();
      label.textContent = `${name}: ${state} `;
      cancel.remove();
    };
    const poll = async () => {
      try {
        const response = await attachmentFetch(`/api/files/transfers/${id}`, {
          headers: {"X-CEMU-Tab": tab},
        });
        if (stopped) return;
        if (response.status !== 404) {
          const value = await (await apiResponse(response)).json();
          if (stopped) return;
          label.textContent = `${name}: ${formatSize(value.completed)} / ${formatSize(value.total)} ${value.state} `;
          cancel.disabled = false;
          if (["completed", "cancelled", "error"].includes(value.state)) {
            stop(); cancel.remove();
            if (value.state === "cancelled" || value.state === "error") await initializeFiles(false);
            return;
          }
        }
      } catch (error) {
        if (isStale(error)) { stop(); return; }
        finish(error.message); await recoverFiles(error); return;
      }
      if (!stopped) timer = setTimeout(poll, 250);
    };
    timer = setTimeout(poll, 250);
    return {id, finish};
  }

  function childPath(path, name) {
    return `${path}\\${name}`;
  }

  function showFilesError(message = "") {
    filesError.textContent = message;
    filesError.hidden = !message;
  }

  function updateFilesState(state, error = null) {
    filesState = state;
    const ready = state === "ready";
    filesStatus.textContent = ready ? "Connected" :
      state === "connecting" ? "Connecting…" : error || "Not connected";
    filesConnect.textContent = state === "error" ? "Retry" : "Connect";
    filesConnect.disabled = state === "connecting";
    filesRefresh.disabled = !ready;
    filesUpload.disabled = !ready;
    filesMkdir.disabled = !ready;
    if (error) showFilesError(error);
  }

  async function apiResponse(response) {
    if (response.ok) return response;
    let message = `${response.status} ${response.statusText}`;
    try {
      const data = await response.clone().json();
      message = data.message || data.error || message;
    } catch {
      const text = await response.text();
      if (text.trim()) message = text.trim();
    }
    throw new Error(message);
  }

  async function connectFiles() {
    const retry = filesState === "error";
    updateFilesState("connecting");
    showFilesError();
    try {
      const response = await apiResponse(await attachmentFetch(`/api/files/connect?retry=${retry ? 1 : 0}`, {
        method: "POST", headers: {"X-CEMU-Request": "1"},
      }));
      const result = await response.json();
      updateFilesState(result.state, result.error);
      await refreshFiles();
    } catch (error) {
      if (isStale(error)) return;
      updateFilesState("error", error.message);
    }
  }

  function renderBreadcrumbs(path) {
    filesBreadcrumbs.replaceChildren();
    const parts = path.split("\\");
    let accumulated = "";
    parts.forEach((part, index) => {
      accumulated = index ? `${accumulated}\\${part}` : part;
      const target = accumulated;
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = part;
      button.addEventListener("click", () => {
        currentPath = target;
        refreshFiles();
      });
      filesBreadcrumbs.append(button);
      if (index < parts.length - 1) filesBreadcrumbs.append(" / ");
    });
  }

  function formatSize(size) {
    if (!Number.isInteger(size)) return "—";
    if (size < 1024) return `${size} B`;
    if (size < 1024 * 1024) return `${(size / 1024).toFixed(1)} KB`;
    return `${(size / (1024 * 1024)).toFixed(1)} MB`;
  }

  function fileAction(label, handler) {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = label;
    button.addEventListener("click", handler);
    return button;
  }

  function renderFiles(entries) {
    filesList.replaceChildren();
    const visible = entries.filter((entry) => entry.kind !== "parent");
    if (!visible.length) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      cell.colSpan = 3;
      cell.className = "files-empty";
      cell.textContent = "This folder is empty";
      row.append(cell);
      filesList.append(row);
      return;
    }
    for (const entry of visible) {
      const row = document.createElement("tr");
      const nameCell = document.createElement("td");
      const sizeCell = document.createElement("td");
      const actions = document.createElement("td");
      if (entry.kind === "folder") {
        const open = fileAction(entry.name, () => {
          currentPath = entry.path;
          refreshFiles();
        });
        open.className = "file-name folder-name";
        nameCell.append(open);
      } else {
        const name = document.createElement("span");
        name.className = "file-name";
        name.textContent = entry.name;
        nameCell.append(name);
        const download = document.createElement("a");
        download.textContent = "Download";
        download.href = `/api/files/download?${new URLSearchParams({path: entry.path, ...attachment})}`;
        download.addEventListener("click", () => {
          const transfer = beginTransfer(entry.name);
          download.href = `/api/files/download?${new URLSearchParams({
            path: entry.path, ...attachment, transfer: transfer.id, tab})}`;
        });
        actions.append(download);
      }
      sizeCell.textContent = entry.kind === "file" ? formatSize(entry.size) : "Folder";
      const remove = fileAction("Delete", async () => {
        if (!window.confirm(`Delete ${entry.name}?`)) return;
        try {
          await apiResponse(await attachmentFetch(`/api/files/delete?${new URLSearchParams({path: entry.path})}`, {
            method: "DELETE",
            headers: {"X-CEMU-Request": "1", "X-CEMU-Confirm": "delete"},
          }));
          await refreshFiles();
        } catch (error) { await recoverFiles(error); }
      });
      remove.className = "danger-action";
      actions.append(remove);
      row.append(nameCell, sizeCell, actions);
      filesList.append(row);
    }
  }

  async function refreshFiles() {
    if (filesState !== "ready") return;
    showFilesError();
    try {
      const response = await apiResponse(await attachmentFetch(
        `/api/files/list?${new URLSearchParams({path: currentPath})}`));
      const result = await response.json();
      currentPath = result.path;
      renderBreadcrumbs(currentPath);
      renderFiles(result.entries);
    } catch (error) {
      await recoverFiles(error);
    }
  }

  async function initializeFiles(refresh = true) {
    try {
      const response = await apiResponse(await attachmentFetch("/api/files/status"));
      const result = await response.json();
      updateFilesState(result.state, result.error);
      if (refresh && result.state === "ready") await refreshFiles();
    } catch (error) {
      if (isStale(error)) return;
      updateFilesState("error", error.message);
    }
  }

  async function recoverFiles(error) {
    if (isStale(error)) return;
    const ticket = epoch;
    // Status is authoritative: ordinary remote errors can leave OBEX usable.
    // Do not issue a second listing while recovering from a failed operation.
    await initializeFiles(false);
    if (ticket === epoch) showFilesError(error.message);
  }

  async function uploadFile(file, overwriteToken = null) {
    const path = childPath(currentPath, file.name);
    const headers = {"X-CEMU-Request": "1", "X-CEMU-Tab": tab};
    const transfer = beginTransfer(file.name);
    if (overwriteToken) headers["X-CEMU-Overwrite"] = overwriteToken;
    let response;
    try {
      response = await attachmentFetch(`/api/files/upload?${new URLSearchParams({path, transfer: transfer.id})}`, {
        method: "PUT", headers, body: file,
      });
    } catch (error) {
      transfer.finish("interrupted");
      throw error;
    }
    transfer.finish(response.ok ? "completed" : response.status === 409 ? "confirmation required" : "error");
    if (response.status === 409) {
      let conflict;
      try {
        conflict = await response.clone().json();
      } catch (error) {
        if (isStale(error)) throw error;
        // Text conflicts are reported through the normal error path below.
      }
      if (conflict?.error === "overwrite_confirmation_required") {
        if (window.confirm(`Overwrite ${file.name}?`)) {
          return uploadFile(file, conflict.token);
        }
        return;
      }
      transfer.finish(conflict?.error === "transfer_cancelled" ? "cancelled" : "error");
    }
    await apiResponse(response);
    await refreshFiles();
  }

  filesConnect.addEventListener("click", connectFiles);
  filesRefresh.addEventListener("click", refreshFiles);
  filesUpload.addEventListener("click", () => filesUploadInput.click());
  filesUploadInput.addEventListener("change", async () => {
    const file = filesUploadInput.files[0];
    filesUploadInput.value = "";
    if (!file) return;
    try { await uploadFile(file); } catch (error) { await recoverFiles(error); }
  });
  filesMkdir.addEventListener("click", async () => {
    const name = window.prompt("Folder name");
    if (!name) return;
    try {
      await apiResponse(await attachmentFetch("/api/files/mkdir", {
        method: "POST",
        headers: {"Content-Type": "application/json", "X-CEMU-Request": "1"},
        body: JSON.stringify({path: childPath(currentPath, name)}),
      }));
      await refreshFiles();
    } catch (error) { await recoverFiles(error); }
  });

  function selectMobileTab(name) {
    workspace.dataset.mobileView = name;
    tabPhone.classList.toggle("active", name === "phone");
    tabFiles.classList.toggle("active", name === "files");
    tabPhone.setAttribute("aria-selected", String(name === "phone"));
    tabFiles.setAttribute("aria-selected", String(name === "files"));
  }

  tabPhone.addEventListener("click", () => selectMobileTab("phone"));
  tabFiles.addEventListener("click", () => selectMobileTab("files"));

  function setAvailable(available) {
    tabFiles.hidden = !available;
    mobileTabs.hidden = !available;
    filesPanel.hidden = !available;
    if (!available) selectMobileTab("phone");
    if (available && !filesInitialized) {
      filesInitialized = true;
      initializeFiles();
    }
  }

  return {setAvailable, attach, invalidate, refresh: refreshFiles};
}
