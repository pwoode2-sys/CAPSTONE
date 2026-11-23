const el = document.getElementById("hrVal");

el.textContent = "72";

section.classList.add("hidden");
section.classList.remove("hidden");

card.className = "card vital ok";

form.addEventListener("submit", (e) => {
  e.preventDefault();
  // read inputs and do the work
});

const user = document.getElementById("loginUser").value.trim();

sessionStorage.setItem("loggedIn", "true");
sessionStorage.getItem("loggedIn") === "true";
sessionStorage.removeItem("loggedIn");

setInterval(tick, 1000);

async function read() {
  const res = await fetch("http://192.168.4.1/vitals", { cache: "no-store" });
  if (!res.ok) throw new Error("HTTP " + res.status);
  return await res.json();
}

const controller = new AbortController();
setTimeout(() => controller.abort(), 2500);
fetch(url, { signal: controller.signal });

try {
  const data = await read();
} catch (err) {
  statusEl.textContent = "Connection lost";
}

el.textContent = data.hr ?? "--";

function classify(v, okMin, okMax) {
  if (v === null) return "none";
  return (v >= okMin && v <= okMax) ? "ok" : "crit";
}

history.push(value);
if (history.length > 30) history.shift();

const pts = history.map((v, i) => `${i * 3},${40 - v / 4}`).join(" ");
line.setAttribute("points", pts);

class VitalSign {
  constructor(key, okMin, okMax) {
    this.key = key; this.okMin = okMin; this.okMax = okMax;
  }
  classify(v) { return (v >= this.okMin && v <= this.okMax) ? "ok" : "crit"; }
}
const hr = new VitalSign("hr", 100, 160);

class DataSource { async read() { throw new Error("implement me"); } }
class SimulatedSource extends DataSource {
  async read() { return { hr: 140, spo2: 97, temp: 36.9 }; }
}

const app = new App();
app.init();

