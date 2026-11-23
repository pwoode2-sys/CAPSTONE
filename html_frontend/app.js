"use strict";

/* ---------- SETTINGS ---------- */
const ESP32_URL   = "http://192.168.4.1/vitals";  // the ESP32 must send CORS headers
const POLL_MS     = 1000;   // how often to read
const TIMEOUT_MS  = 2500;   // give up on a slow read
const HISTORY_MAX = 30;     // points kept for each graph

const $ = (id) => document.getElementById(id);


/* ---------- VITAL SIGNS ---------- */
class VitalSign {
  constructor(key, label, unit, okMin, okMax, decimals = 0) {
    this.key = key; this.label = label; this.unit = unit;
    this.okMin = okMin; this.okMax = okMax; this.decimals = decimals;
  }
  classify(v) {
    if (typeof v !== "number" || Number.isNaN(v)) return "none";
    return (v >= this.okMin && v <= this.okMax) ? "ok" : "crit";
  }
  format(v) {
    return typeof v === "number" ? v.toFixed(this.decimals) : "--";
  }
}

const VITALS = [
  new VitalSign("hr",   "Heart rate",         "bpm", 100, 160, 0),
  new VitalSign("spo2", "Oxygen saturation",  "%",   95,  100, 0),
  new VitalSign("temp", "Temperature",        "°C",  36.5, 37.5, 1)
];


/* ---------- DATA SOURCES ---------- */
class DataSource {
  async read() { throw new Error("implement me"); }
}

class Esp32Source extends DataSource {
  async read() {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
    try {
      const res = await fetch(ESP32_URL, { cache: "no-store", signal: controller.signal });
      if (!res.ok) throw new Error("HTTP " + res.status);
      return await res.json();
    } finally {
      clearTimeout(timer);
    }
  }
}


/* ---------- THE APP ---------- */
class App {
  constructor() {
    this.source = new Esp32Source();
    this.history = {};
    this.timer = null;
    this.busy = false;     // stops reads from piling up if one is slow
  }

  init() {
    this.buildCards();

    $("loginForm").addEventListener("submit", (e) => { e.preventDefault(); doLogin(); });
    $("registerForm").addEventListener("submit", (e) => { e.preventDefault(); doRegister(); });
    $("logoutBtn").addEventListener("click", logout);
    $("exportBtn").addEventListener("click", downloadUsersFile);
    $("tabLogin").addEventListener("click", showLoginForm);
    $("tabRegister").addEventListener("click", showRegisterForm);

    this.refreshView();
  }

  buildCards() {
    $("vitals").innerHTML = VITALS.map((v) => `
      <article class="card none" id="card-${v.key}">
        <h2>${v.label}</h2>
        <div class="reading"><span id="val-${v.key}">--</span> <span class="unit">${v.unit}</span></div>
        <div class="badge" id="badge-${v.key}">No data</div>
        <p class="range">Normal range: ${v.okMin}–${v.okMax} ${v.unit}</p>
        <svg viewBox="0 0 100 40" preserveAspectRatio="none" aria-hidden="true">
          <polyline id="line-${v.key}" points=""></polyline>
        </svg>
      </article>`).join("");
    VITALS.forEach((v) => (this.history[v.key] = []));
  }

  // Show the right section for the current login state.
  refreshView() {
    const loggedIn = sessionStorage.getItem("loggedIn") === "true";
    $("publicView").classList.toggle("hidden", loggedIn);
    $("dashboardView").classList.toggle("hidden", !loggedIn);
    $("sessionBar").classList.toggle("hidden", !loggedIn);
    $("whoami").textContent = sessionStorage.getItem("loggedInUser") || "";
    if (loggedIn) this.start(); else this.stop();
  }

  start() {
    if (this.timer) return;
    this.tick();
    this.timer = setInterval(() => this.tick(), POLL_MS);
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
  }

  async tick() {
    if (this.busy) return;
    this.busy = true;
    try {
      const data = await this.source.read();
      $("status").textContent = "Receiving data";
      this.render(data);
    } catch (err) {
      $("status").textContent = "Connection lost";
      this.render({});
    } finally {
      this.busy = false;
    }
  }

  render(data) {
    VITALS.forEach((v) => {
      const value = data[v.key] ?? null;
      const state = v.classify(value);

      $("val-" + v.key).textContent = v.format(value);
      $("card-" + v.key).className = "card " + state;
      $("badge-" + v.key).textContent =
        state === "ok" ? "In range" : state === "crit" ? "Out of range" : "No data";

      if (state !== "none") {
        const h = this.history[v.key];
        h.push(value);
        if (h.length > HISTORY_MAX) h.shift();
      }
      this.drawLine(v);
    });
  }

  // Graph spans one "normal range" above and below the normal band.
  drawLine(v) {
    const span = v.okMax - v.okMin;
    const lo = v.okMin - span, hi = v.okMax + span;
    const h = this.history[v.key];
    const pts = h.map((val, i) => {
      const x = (i * 100) / (HISTORY_MAX - 1);
      const y = 40 - ((Math.min(hi, Math.max(lo, val)) - lo) / (hi - lo)) * 40;
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).join(" ");
    $("line-" + v.key).setAttribute("points", pts);
  }
}


/* ---------- GLUE (called by password.js and the tabs) ---------- */
function showLoginForm() {
  $("loginForm").classList.remove("hidden");
  $("registerForm").classList.add("hidden");
  $("tabLogin").setAttribute("aria-pressed", "true");
  $("tabRegister").setAttribute("aria-pressed", "false");
  $("authMsg").textContent = "";
}

function showRegisterForm() {
  $("registerForm").classList.remove("hidden");
  $("loginForm").classList.add("hidden");
  $("tabRegister").setAttribute("aria-pressed", "true");
  $("tabLogin").setAttribute("aria-pressed", "false");
  $("authMsg").textContent = "";
}

function showView() { app.refreshView(); }

let app;
document.addEventListener("DOMContentLoaded", () => {
  app = new App();
  app.init();
});