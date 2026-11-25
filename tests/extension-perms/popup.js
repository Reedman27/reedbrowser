const out = [];
const t = async (name, fn) => { try { out.push("OK   " + name + " -> " + JSON.stringify(await fn())); } catch (e) { out.push("DENY " + name + " -> " + (e.message || e)); } };
(async () => {
  await t("storage.local.set", () => browser.storage.local.set({ a: 1 }));
  await t("tabs.query (no tabs perm; host ok + activeTab-from-popup)", async () => (await browser.tabs.query({ active: true })).map((x) => ({ hasUrl: "url" in x })));
  await t("cookies.getAll (no cookies perm)", () => browser.cookies.getAll({}));
  await t("menus.create (no menus perm)", () => browser.menus.create({ id: "x", title: "x" }));
  await t("scripting.executeScript (no scripting perm)", () => browser.scripting.executeScript({ target: { tabId: 1 }, func: () => 1 }));
  await t("webRequest listener (no perm)", async () => { browser.webRequest.onBeforeRequest.addListener(() => {}, { urls: ["<all_urls>"] }); return "added"; });
  await t("runtime.getURL is sync", () => typeof browser.runtime.getURL("x"));
  document.getElementById("o").textContent = out.join("\n");
  fetch("http://127.0.0.1:8099/?result=" + encodeURIComponent(out.join("|"))).catch(() => {});
})();
