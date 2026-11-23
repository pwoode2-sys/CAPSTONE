"use strict";
/* ---------- 1. SETTINGS (change these here, nowhere else) ---------- */
const STORAGE_KEY  = "nn.users";   // name of the saved list in localStorage
const ITERATIONS   = 150000;       // how many times the scrambling repeats (slow on purpose)
const MIN_LENGTH   = 8;            // shortest allowed password
const MAX_ATTEMPTS = 5;            // wrong tries before a lockout
const LOCK_SECONDS = 30;           // lockout length
const USERS_FILE   = "password.json";   // accounts file read when the page opens


/* ---------- 2. SMALL HELPERS ---------- */

// Show a message in the page (red for errors, green for success).
function notify(message, isError = true) {
  const box = document.getElementById("authMsg");
  if (!box) { alert(message); return; }
  box.textContent = message;
  box.className = "msg " + (isError ? "error" : "good");
}

// localStorage can only hold TEXT, so we convert the list of users to text and back.
function loadUsers() {
  try {
    return JSON.parse(localStorage.getItem(STORAGE_KEY)) || [];
  } catch (err) {
    return [];   // saved data was damaged
  }
}

function saveUsers(users) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(users));
}

// Accounts from password.json (read-only). Needs the page to be served over http.
let fileUsers = [];

function isValidUser(u) {
  return u && typeof u.username === "string" && typeof u.salt === "string" &&
         typeof u.hash === "string" && typeof u.iterations === "number";
}

async function loadFileUsers() {
  try {
    const res = await fetch(USERS_FILE, { cache: "no-store" });
    if (!res.ok) return;
    const data = await res.json();
    if (Array.isArray(data)) fileUsers = data.filter(isValidUser);
  } catch (err) {
    // No file, or the page was opened as a local file: just use browser-saved accounts.
  }
}
const usersReady = loadFileUsers();

// Every known account: the file first, then ones created in this browser.
function allUsers() {
  const local = loadUsers().filter((u) => !fileUsers.some((f) => f.username === u.username));
  return fileUsers.concat(local);
}

// Download all accounts as password.json (replace the file on disk with it to keep them).
function downloadUsersFile() {
  const blob = new Blob([JSON.stringify(allUsers(), null, 2) + "\n"], { type: "application/json" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = "password.json";
  link.click();
  URL.revokeObjectURL(link.href);
}

// bytes -> "a3f9..." text
function bytesToHex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

// "a3f9..." text -> bytes
function hexToBytes(hex) {
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return bytes;
}

// Compare two texts without stopping at the first difference.
function safeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

function hashingAvailable() {
  return !!(window.crypto && window.crypto.subtle);
}

const NO_HASHING_MESSAGE =
  "This page can't hash passwords here. Open it from localhost or https.";


/* ---------- 3. THE HASH FUNCTION ---------- */
async function hashPassword(password, saltBytes, iterations) {
  const encoder = new TextEncoder();
  const key = await crypto.subtle.importKey(
    "raw", encoder.encode(password), "PBKDF2", false, ["deriveBits"]
  );
  const bits = await crypto.subtle.deriveBits(
    { name: "PBKDF2", salt: saltBytes, iterations: iterations, hash: "SHA-256" },
    key,
    256
  );
  return bytesToHex(new Uint8Array(bits));
}


/* ---------- 4. REGISTER ---------- */
async function doRegister() {
  if (!hashingAvailable()) { notify(NO_HASHING_MESSAGE); return; }

  const username = document.getElementById("regUser").value.trim();
  const password = document.getElementById("regPass").value;

  const confirm  = document.getElementById("regPass2").value;

  if (username === "") { notify("Please enter a username."); return; }
  if (password !== confirm) { notify("The two passwords don't match."); return; }
  if (password.length < MIN_LENGTH) {
    notify("Password must be at least " + MIN_LENGTH + " characters.");
    return;
  }

  await usersReady;
  const users = loadUsers();
  if (allUsers().some((u) => u.username === username)) {
    notify("That username is already taken.");
    return;
  }

  const salt = crypto.getRandomValues(new Uint8Array(16));   // different per user
  const hash = await hashPassword(password, salt, ITERATIONS);

  users.push({
    username: username,
    salt: bytesToHex(salt),
    hash: hash,
    iterations: ITERATIONS   // saved so ITERATIONS can be raised later without breaking old accounts
  });
  saveUsers(users);

  document.getElementById("regUser").value = "";
  document.getElementById("regPass").value = "";
  document.getElementById("regPass2").value = "";
  if (typeof showLoginForm === "function") showLoginForm();
  document.getElementById("loginUser").value = username;
  document.getElementById("loginPass").focus();
  notify("Account created. Enter your password to log in.", false);
}


/* ---------- 5. LOGIN ---------- */
async function doLogin() {
  if (!hashingAvailable()) { notify(NO_HASHING_MESSAGE); return; }

  // Lockout after too many wrong tries (sessionStorage so a refresh doesn't reset it)
  const lockedUntil = Number(sessionStorage.getItem("nn.lockedUntil")) || 0;
  const waitMs = lockedUntil - Date.now();
  if (waitMs > 0) {
    notify("Too many attempts. Try again in " + Math.ceil(waitMs / 1000) + " seconds.");
    return;
  }

  const username = document.getElementById("loginUser").value.trim();
  const password = document.getElementById("loginPass").value;

  await usersReady;
  const record = allUsers().find((u) => u.username === username);

  let passwordIsRight = false;
  if (record) {
    const hash = await hashPassword(password, hexToBytes(record.salt), record.iterations);
    passwordIsRight = safeEqual(hash, record.hash);
  } else {
    // Unknown username: do the same slow work so timing doesn't reveal it.
    await hashPassword(password, new Uint8Array(16), ITERATIONS);
  }

  if (passwordIsRight) {
    sessionStorage.removeItem("nn.failed");
    sessionStorage.setItem("loggedIn", "true");
    sessionStorage.setItem("loggedInUser", username);
    document.getElementById("loginPass").value = "";
    notify("", false);
    showView();   // defined in app.js
    return;
  }

  // Wrong: count it, maybe lock, and use ONE message for both "no such user" and "bad password".
  const failed = (Number(sessionStorage.getItem("nn.failed")) || 0) + 1;
  if (failed >= MAX_ATTEMPTS) {
    sessionStorage.setItem("nn.lockedUntil", String(Date.now() + LOCK_SECONDS * 1000));
    sessionStorage.setItem("nn.failed", "0");
  } else {
    sessionStorage.setItem("nn.failed", String(failed));
  }
  notify("Invalid username or password.");
}


/* ---------- 6. LOGOUT ---------- */
function logout() {
  sessionStorage.removeItem("loggedIn");
  sessionStorage.removeItem("loggedInUser");
  showView();   // defined in app.js
}
