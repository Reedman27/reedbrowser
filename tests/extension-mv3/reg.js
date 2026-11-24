document.documentElement.setAttribute("data-reg", "1");
browser.runtime.onConnect.addListener((p) => p.onMessage.addListener((m) => p.postMessage("content-got:" + m)));
(async () => {
  const pong = await browser.runtime.sendMessage({ type: "ping", n: 5 });
  const port = browser.runtime.connect({ name: "cs" });
  port.onMessage.addListener((m) => browser.runtime.sendMessage({ type: "results", data: { port_cs_reply: m } }));
  port.postMessage("hello-bg");
  // result of the page's <script src=blocked-by-dnr.js>
  setTimeout(() => browser.runtime.sendMessage({ type: "results", data: { pong: pong, static_dnr: document.body.dataset.a || "none", dyn_dnr: document.body.dataset.b || "none", reg_attr: "1" } }), 800);
})();
