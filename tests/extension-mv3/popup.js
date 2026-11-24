browser.runtime.sendMessage({ type: "popup" }).then((r) => { document.getElementById("o").textContent = "bg says: " + r.hello; });
document.getElementById("opt").onclick = () => browser.runtime.openOptionsPage();
