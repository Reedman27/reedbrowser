const i = document.getElementById("v");
browser.storage.local.get("opt").then((o) => { i.value = o.opt || ""; });
i.oninput = () => browser.storage.local.set({ opt: i.value });
