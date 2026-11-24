const http = require("http");
const fs = require("fs");
const path = require("path");

const PORT = 3000;
const PASSWORDS = path.join(__dirname, "passwords.txt");
const STATIC = {
  "/": ["web.html", "text/html"],
  "/web.html": ["web.html", "text/html"],
  "/app.js": ["app.js", "text/javascript"],
  "/password.js": ["password.js", "text/javascript"],
  "/style.css": ["style.css", "text/css"]
};

function readUsers() {
  try { return JSON.parse(fs.readFileSync(PASSWORDS, "utf8")); }
  catch { return []; }
}

http.createServer((req, res) => {
  // Send the list of accounts
  if (req.url === "/users" && req.method === "GET") {
    res.writeHead(200, { "Content-Type": "application/json" });
    return res.end(JSON.stringify(readUsers()));
  }

  // Add one new account
  if (req.url === "/users" && req.method === "POST") {
    let body = "";
    req.on("data", (c) => { body += c; if (body.length > 10000) req.destroy(); });
    req.on("end", () => {
      try {
        const u = JSON.parse(body);
        const ok = u && typeof u.username === "string" && typeof u.salt === "string" &&
                   typeof u.hash === "string" && typeof u.iterations === "number";
        if (!ok) { res.writeHead(400); return res.end(); }
        const users = readUsers();
        if (users.some((x) => x.username === u.username)) { res.writeHead(409); return res.end(); }
        users.push({ username: u.username, salt: u.salt, hash: u.hash, iterations: u.iterations });
        fs.writeFileSync(PASSWORDS, JSON.stringify(users, null, 2) + "\n");
        res.writeHead(201); res.end();
      } catch { res.writeHead(400); res.end(); }
    });
    return;
  }

  // Serve the site files
  const file = STATIC[req.url.split("?")[0]];
  if (req.method === "GET" && file) {
    res.writeHead(200, { "Content-Type": file[1] });
    return fs.createReadStream(path.join(__dirname, file[0])).pipe(res);
  }
  res.writeHead(404); res.end();
}).listen(PORT, () => console.log("Open http://localhost:" + PORT));