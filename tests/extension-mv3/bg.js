let chain = Promise.resolve();
const set = (k, v) => (chain = chain.then(() => browser.storage.local.get("r")).then((o) => { const r = o.r || {}; r[k] = v; return browser.storage.local.set({ r }); }));
browser.runtime.onMessage.addListener((m, sender) => {
  if (m.type === "ping") return Promise.resolve({ pong: m.n, fromTab: !!sender.tab });
  if (m.type === "results") { for (const [k, v] of Object.entries(m.data)) set(k, v); return Promise.resolve(true); }
  if (m.type === "popup") { set("popup_msg", "ok"); return Promise.resolve({ hello: "popup" }); }
});
browser.runtime.onConnect.addListener((port) => {
  port.onMessage.addListener((d) => { set("port_" + port.name, d); port.postMessage({ echo: d }); });
});
browser.action.setBadgeText({ text: "7" });
browser.action.setBadgeBackgroundColor({ color: "#2255cc" });
browser.scripting.registerContentScripts([{ id: "reg1", matches: ["http://127.0.0.1/*"], js: ["reg.js"], runAt: "document_end", persistAcrossSessions: false }])
  .catch((e) => set("register_error", String(e)));
browser.declarativeNetRequest.updateDynamicRules({ addRules: [{ id: 50, priority: 1, action: { type: "block" }, condition: { urlFilter: "dyn-blocked.js", resourceTypes: ["script"] } }] })
  .then(() => set("dnr_dynamic_added", true)).catch((e) => set("dnr_dynamic_error", String(e)));
browser.tabs.onUpdated.addListener(async (id, change, tab) => {
  if (change.status !== "complete" || !/127\.0\.0\.1/.test(tab.url || "")) return;
  try {
    const r = await browser.scripting.executeScript({ target: { tabId: id }, func: (a, b) => { document.documentElement.setAttribute("data-exec", String(a + b)); return a + b; }, args: [20, 22] });
    await set("exec_result", r[0].result);
    const old = await browser.tabs.executeScript(id, { code: "document.title.length" });
    await set("tabs_exec_len", old[0]);
    await browser.tabs.insertCSS(id, { code: "body{outline:3px solid red}" });
    await set("css_ok", true);
    const port = browser.tabs.connect(id, { name: "tabport" });
    port.onMessage.addListener((m) => set("tabconnect_reply", m));
    port.postMessage("from-bg");
  } catch (e) { await set("exec_error", String(e)); }
});

// Scripts registered at runtime only apply to pages loaded afterwards, so reload what is open.
setTimeout(() => browser.tabs.query({}).then((ts) => ts.forEach((t) => browser.tabs.reload(t.id))), 1500);
browser.webNavigation.onCompleted.addListener((d) => set("webnav", d.url));
(async () => {
  const u = "http://127.0.0.1:8099/";
  await browser.cookies.set({ url: u, name: "t", value: "v1" });
  const c = await browser.cookies.get({ url: u, name: "t" });
  await set("cookie", c && c.value);
  await set("cookie_all", (await browser.cookies.getAll({ url: u })).length);
  await browser.cookies.remove({ url: u, name: "t" });
  await set("cookie_removed", (await browser.cookies.get({ url: u, name: "t" })) === null);
})().catch((e) => set("cookie_error", String(e)));
(async () => {
  await browser.cookies.set({ url: "http://example.test/", name: "n", value: "v2", domain: "example.test", path: "/" });
  await set("cookie_host_all", (await browser.cookies.getAll({})).map((c) => c.domain + ":" + c.name));
  await set("cookie_host_url", (await browser.cookies.getAll({ url: "http://example.test/" })).length);
})().catch((e) => set("cookie_host_error", String(e)));
const seen = [];
browser.webRequest.onBeforeRequest.addListener((d) => { seen.push(d.type + ":" + d.url.split("/").pop()); }, { urls: ["http://127.0.0.1/*"] });
browser.webRequest.onCompleted.addListener((d) => { if (d.url.endsWith("index.html") || d.url.endsWith(":8099/")) set("webreq_completed_status", d.statusCode); }, { urls: ["<all_urls>"] });
setTimeout(() => set("webreq_seen", seen.slice(0, 8)), 4000);
