// ==UserScript==
// @name         Claude token LED
// @namespace    vmynick
// @version      1.0
// @description  Villantja az asztali LED-et, amíg a claude.ai választ generál
// @match        https://claude.ai/*
// @grant        GM_xmlhttpRequest
// @connect      tokenled.local
// @connect      *
// @run-at       document-idle
// ==/UserScript==

(() => {
  const LED = 'http://tokenled.local';   // ha az mDNS nem megy: 'http://192.168.x.y'
  const TICK_MS = 100;                    // max 10 kérés/s
  const CHARS_PER_TOKEN = 4;              // becslés

  let lastEl = null, lastLen = 0, pend = 0, busy = false;

  function send() {
    if (busy || pend <= 0) return;
    const n = pend; pend = 0; busy = true;
    GM_xmlhttpRequest({
      method: 'GET',
      url: `${LED}/p?s=w&t=t&n=${n}`,
      timeout: 500,
      onload: () => { busy = false; },
      onerror: () => { busy = false; },
      ontimeout: () => { busy = false; },
    });
  }

  setInterval(() => {
    // A generálás alatt álló üzenet; ha a claude.ai UI változik, ezt a szelektort kell igazítani.
    const els = document.querySelectorAll('[data-is-streaming="true"]');
    const el = els[els.length - 1];
    if (!el) { lastEl = null; return; }
    const len = el.textContent.length;
    if (el !== lastEl) { lastEl = el; lastLen = 0; }
    const d = len - lastLen;
    lastLen = len;
    if (d > 0) pend += Math.max(1, Math.round(d / CHARS_PER_TOKEN));
    send();
  }, TICK_MS);
})();
