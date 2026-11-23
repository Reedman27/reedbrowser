// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
//
// In-page implementation of the `browser.*` / `chrome.*` WebExtensions API.
// Injected into the extension background page and into each extension's isolated
// content-script world. It is wrapped as `(function(__cfg){ ... })(cfg)` by the
// native side. __cfg = {ctx, id, uuid, handler, baseUrl, manifest, messages, version}.
"use strict";
if (self.__reedInstalled) return;
Object.defineProperty(self, "__reedInstalled", { value: true });

const { ctx, handler, baseUrl, manifest, messages } = __cfg;
const isBg = ctx === "background";
const NO_RECEIVER = "Could not establish connection. Receiving end does not exist.";

// ---- native bridge -------------------------------------------------------
function post(op, args) {
    return window.webkit.messageHandlers[handler]
        .postMessage({ op, args: args || {}, url: location.href, top: window === window.top })
        .then((env) => {
            if (env && env.ok) return env.value;
            throw new Error((env && env.error) || "extension bridge error");
        });
}
const reported = new Set();
function report(path) {
    if (reported.has(path)) return;
    reported.add(path);
    post("log", { msg: "unsupported API used: " + path }).catch(() => {});
}

// ---- events ----------------------------------------------------------------
class Ev {
    constructor(name) { this._l = new Set(); this._name = name; }
    addListener(f) { if (typeof f === "function") this._l.add(f); }
    removeListener(f) { this._l.delete(f); }
    hasListener(f) { return this._l.has(f); }
    hasListeners() { return this._l.size > 0; }
    _fire(...a) { for (const f of [...this._l]) { try { f(...a); } catch (e) { console.error(e); } } }
}
const events = {};
function ev(name) { return events[name] || (events[name] = new Ev(name)); }
// Events that exist but are never delivered yet: listeners are accepted, usage is reported.
function inertEv(name) {
    const e = new Ev(name);
    const add = e.addListener.bind(e);
    e.addListener = (f) => { report(name + " (listener registered, event is never fired)"); add(f); };
    return e;
}
Object.defineProperty(self, "__reedFire", {
    value: (name, args) => { const e = events[name]; if (e) e._fire(...args); return null; },
});

// runtime.onMessage dispatch with Firefox semantics: a listener may call sendResponse,
// return true to respond later, or return a Promise that resolves to the response.
Object.defineProperty(self, "__reedDeliver", {
    value: (msg, sender) => new Promise((resolve) => {
        const e = events["runtime.onMessage"];
        const ls = e ? [...e._l] : [];
        if (!ls.length) { resolve({ noReceiver: true }); return; }
        let done = false, holds = 0, hold = false;
        const finish = (v) => { if (!done) { done = true; resolve({ responded: true, value: v === undefined ? null : v }); } };
        const settle = () => { if (!done && holds === 0 && !hold) resolve({ responded: false }); };
        for (const f of ls) {
            let r;
            try { r = f(msg, sender, finish); } catch (err) { console.error(err); continue; }
            if (r && typeof r.then === "function") {
                holds++;
                r.then((v) => { holds--; if (v !== undefined) finish(v); else settle(); },
                       (err) => { holds--; console.error(err); settle(); });
            } else if (r === true) hold = true;
        }
        settle();
    }),
});

// Promise API that also accepts a trailing callback (chrome.* style).
function cbify(fn) {
    return function (...args) {
        let cb = null;
        if (args.length && typeof args[args.length - 1] === "function") cb = args.pop();
        const p = Promise.resolve().then(() => fn(...args));
        if (!cb) return p;
        p.then((v) => cb(v), (e) => {
            runtime.lastError = { message: String((e && e.message) || e) };
            try { cb(); } finally { runtime.lastError = undefined; }
        });
    };
}
function wrapAll(obj) {
    for (const k of Object.keys(obj)) if (typeof obj[k] === "function" && !(obj[k] instanceof Ev)) obj[k] = cbify(obj[k]);
    return obj;
}
function sendResult(r) {
    if (r && r.noReceiver) throw new Error(NO_RECEIVER);
    return r && r.responded ? r.value : undefined;
}

// ---- runtime ---------------------------------------------------------------
const runtime = wrapAll({
    id: __cfg.id,
    lastError: undefined,
    getURL: (p) => baseUrl + String(p).replace(/^\//, ""),
    getManifest: () => JSON.parse(JSON.stringify(manifest)),
    sendMessage: (a, b) => {
        const message = (typeof a === "string" && b !== undefined && !isBg) ? b : (typeof a === "string" && b !== undefined ? b : a);
        return post("runtime.sendMessage", { message }).then(sendResult);
    },
    connect: (info) => {
        report("runtime.connect (stub: ports never deliver messages)");
        return { name: (info && info.name) || "", sender: undefined, postMessage() {}, disconnect() {},
                 onMessage: new Ev(), onDisconnect: new Ev() };
    },
    getPlatformInfo: () => ({ os: "linux", arch: "x86-64" }),
    getBrowserInfo: () => ({ name: "ReedBrowser", vendor: "ReedBrowser", version: __cfg.version, buildID: "0" }),
    openOptionsPage: () => post("runtime.openOptionsPage"),
    setUninstallURL: () => undefined,
    reload: () => { report("runtime.reload"); },
});
runtime.onMessage = ev("runtime.onMessage");
runtime.onConnect = ev("runtime.onConnect");
runtime.onInstalled = ev("runtime.onInstalled");
runtime.onStartup = ev("runtime.onStartup");
runtime.onUpdateAvailable = ev("runtime.onUpdateAvailable");
runtime.onSuspend = ev("runtime.onSuspend");
runtime.OnInstalledReason = { INSTALL: "install", UPDATE: "update", BROWSER_UPDATE: "browser_update" };

// ---- storage ---------------------------------------------------------------
function storageArea(name) {
    return wrapAll({
        get: (k) => post("storage.get", { area: name, keys: k === undefined ? null : k }),
        set: (o) => post("storage.set", { area: name, items: o }),
        remove: (k) => post("storage.remove", { area: name, keys: k }),
        clear: () => post("storage.clear", { area: name }),
        getBytesInUse: () => 0,
        setAccessLevel: () => undefined,
        QUOTA_BYTES: name === "sync" ? 102400 : 10485760,
        MAX_ITEMS: 512, MAX_WRITE_OPERATIONS_PER_HOUR: 1800, MAX_WRITE_OPERATIONS_PER_MINUTE: 120,
        QUOTA_BYTES_PER_ITEM: 8192,
    });
}
const storage = {
    local: storageArea("local"), sync: storageArea("sync"), session: storageArea("session"),
    managed: wrapAll({ get: () => ({}) }),
    onChanged: ev("storage.onChanged"),
};

// ---- tabs / windows (background only; content scripts get errors from the host) ----
const tabs = wrapAll({
    query: (q) => post("tabs.query", { q: q || {} }),
    get: (id) => post("tabs.get", { id }),
    getCurrent: () => undefined,
    create: (p) => post("tabs.create", { p: p || {} }),
    update: (a, b) => (typeof a === "number" ? post("tabs.update", { id: a, p: b || {} }) : post("tabs.update", { id: null, p: a || {} })),
    remove: (ids) => post("tabs.remove", { ids: Array.isArray(ids) ? ids : [ids] }),
    reload: (id) => post("tabs.reload", { id: typeof id === "number" ? id : null }),
    sendMessage: (id, message) => post("tabs.sendMessage", { id, message }).then(sendResult),
    executeScript: () => { report("tabs.executeScript"); throw new Error("tabs.executeScript is not supported by ReedBrowser yet"); },
    insertCSS: () => { report("tabs.insertCSS"); throw new Error("tabs.insertCSS is not supported by ReedBrowser yet"); },
    move: () => undefined,
    TAB_ID_NONE: -1,
});
for (const n of ["onCreated", "onUpdated", "onRemoved", "onActivated", "onReplaced", "onMoved", "onAttached", "onDetached"])
    tabs[n] = n === "onReplaced" || n === "onMoved" || n === "onAttached" || n === "onDetached" ? inertEv("tabs." + n) : ev("tabs." + n);

const winObj = { id: 1, focused: true, incognito: false, type: "normal", state: "normal", alwaysOnTop: false };
const windows = wrapAll({
    getCurrent: () => winObj, getLastFocused: () => winObj, getAll: () => [winObj], get: () => winObj,
    WINDOW_ID_NONE: -1, WINDOW_ID_CURRENT: -2,
});
windows.onFocusChanged = inertEv("windows.onFocusChanged");

// ---- browser action (state is kept natively for the toolbar; no popup UI yet) ----
function actionApi() {
    const a = {};
    for (const m of ["setIcon", "setTitle", "setBadgeText", "setBadgeBackgroundColor", "setBadgeTextColor", "setPopup", "enable", "disable", "show", "hide"])
        a[m] = (d) => post("action.set", { name: m, details: d === undefined ? null : d });
    for (const m of ["getTitle", "getBadgeText", "getPopup", "getBadgeBackgroundColor"])
        a[m] = (d) => post("action.get", { name: m });
    wrapAll(a);
    a.onClicked = ev("action.onClicked");
    return a;
}

// ---- alarms (pure JS, background page) -------------------------------------
const alarmTimers = new Map(), alarmInfo = new Map();
const alarms = wrapAll({
    create: (a, b) => {
        const name = typeof a === "string" ? a : ""; const info = typeof a === "string" ? b : a || {};
        if (alarmTimers.has(name)) { clearTimeout(alarmTimers.get(name)); clearInterval(alarmTimers.get(name)); }
        const delay = info.when ? Math.max(0, info.when - Date.now()) : (info.delayInMinutes || info.periodInMinutes || 0) * 60000;
        const period = (info.periodInMinutes || 0) * 60000;
        const fire = () => ev("alarms.onAlarm")._fire({ name, scheduledTime: Date.now(), periodInMinutes: info.periodInMinutes });
        alarmInfo.set(name, { name, scheduledTime: Date.now() + delay, periodInMinutes: info.periodInMinutes });
        const t = setTimeout(() => { fire(); if (period) alarmTimers.set(name, setInterval(fire, period)); else { alarmTimers.delete(name); alarmInfo.delete(name); } }, delay);
        alarmTimers.set(name, t);
    },
    get: (n) => alarmInfo.get(n === undefined ? "" : n),
    getAll: () => [...alarmInfo.values()],
    clear: (n) => { n = n === undefined ? "" : n; const t = alarmTimers.get(n); clearTimeout(t); clearInterval(t); alarmTimers.delete(n); return alarmInfo.delete(n); },
    clearAll: () => { for (const t of alarmTimers.values()) { clearTimeout(t); clearInterval(t); } alarmTimers.clear(); alarmInfo.clear(); return true; },
});
alarms.onAlarm = ev("alarms.onAlarm");

// ---- i18n -------------------------------------------------------------------
const i18n = {
    getMessage(name, subs) {
        const m = messages && messages[name];
        if (!m) return "";
        let s = m.message || "";
        if (m.placeholders) for (const [k, v] of Object.entries(m.placeholders)) s = s.replace(new RegExp("\\$" + k + "\\$", "gi"), v.content);
        if (subs !== undefined) { if (!Array.isArray(subs)) subs = [subs]; s = s.replace(/\$(\d)/g, (x, i) => (subs[i - 1] === undefined ? "" : subs[i - 1])); }
        return s;
    },
    getUILanguage: () => navigator.language,
    getAcceptLanguages: () => Promise.resolve([navigator.language]),
    detectLanguage: () => Promise.resolve({ isReliable: false, languages: [] }),
};

// ---- misc namespaces -----------------------------------------------------------
const permissions = wrapAll({
    getAll: () => ({ permissions: (manifest.permissions || []).filter((p) => !/[:*]/.test(p)), origins: (manifest.permissions || []).filter((p) => /[:*]/.test(p)).concat(manifest.host_permissions || []) }),
    contains: () => true,
    request: () => false,
    remove: () => false,
});
permissions.onAdded = inertEv("permissions.onAdded");
permissions.onRemoved = inertEv("permissions.onRemoved");

const webRequest = wrapAll({
    handlerBehaviorChanged: () => undefined,
    filterResponseData: () => { report("webRequest.filterResponseData"); throw new Error("webRequest.filterResponseData is not supported by ReedBrowser"); },
    MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20,
});
for (const n of ["onBeforeRequest", "onBeforeSendHeaders", "onSendHeaders", "onHeadersReceived", "onAuthRequired",
                 "onResponseStarted", "onBeforeRedirect", "onCompleted", "onErrorOccurred"])
    webRequest[n] = inertEv("webRequest." + n);

const webNavigation = wrapAll({ getFrame: () => null, getAllFrames: () => [] });
for (const n of ["onBeforeNavigate", "onCommitted", "onDOMContentLoaded", "onCompleted", "onCreatedNavigationTarget", "onErrorOccurred", "onHistoryStateUpdated", "onReferenceFragmentUpdated"])
    webNavigation[n] = inertEv("webNavigation." + n);

const menus = wrapAll({ create: () => 1, update: () => undefined, remove: () => undefined, removeAll: () => undefined, refresh: () => undefined });
menus.onClicked = inertEv("menus.onClicked");
menus.onShown = inertEv("menus.onShown");
menus.onHidden = inertEv("menus.onHidden");

const commands = wrapAll({ getAll: () => Object.entries(manifest.commands || {}).map(([name, c]) => ({ name, description: c.description || "", shortcut: (c.suggested_key && c.suggested_key.default) || "" })) });
commands.onCommand = inertEv("commands.onCommand");

const privacyProp = () => wrapAll({ get: () => ({ value: "default", levelOfControl: "not_controllable" }), set: () => undefined, clear: () => undefined });
const privacy = { network: { webRTCIPHandlingPolicy: privacyProp(), networkPredictionEnabled: privacyProp(), prefetchingEnabled: privacyProp(), hyperlinkAuditingEnabled: privacyProp() } };

const management = wrapAll({
    getSelf: () => ({ id: __cfg.id, name: manifest.name, version: manifest.version, enabled: true, type: "extension", installType: "normal" }),
});

// ---- assemble -----------------------------------------------------------------
const real = {
    runtime, storage, tabs, windows, alarms, i18n, permissions, webRequest, webNavigation, menus, contextMenus: menus,
    commands, privacy, management,
    extension: { getURL: runtime.getURL, inIncognitoContext: false, getBackgroundPage: () => (isBg ? window : null) },
    browserAction: actionApi(), action: actionApi(), pageAction: actionApi(),
    dns: wrapAll({ resolve: () => { report("dns.resolve"); throw new Error("dns.resolve is not supported by ReedBrowser"); } }),
};
function unsupported(path) {
    return new Proxy(function () {}, {
        get: (t, p) => (typeof p === "symbol" ? undefined : unsupported(path + "." + String(p))),
        apply: () => {
            report(path);
            const err = Promise.reject(new Error(path + " is not supported by ReedBrowser yet"));
            err.catch(() => {});
            return err;
        },
    });
}
const api = new Proxy(real, {
    get: (t, p) => (p in t ? t[p] : typeof p === "symbol" || p === "then" ? undefined : unsupported(String(p))),
});
Object.defineProperty(self, "browser", { value: api, configurable: true, writable: true });
Object.defineProperty(self, "chrome", { value: api, configurable: true, writable: true });
