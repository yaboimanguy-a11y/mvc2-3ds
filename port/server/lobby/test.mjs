// Tests worker.js in Node (no Cloudflare account needed):  node port/server/lobby/test.mjs
import worker, { Lobby } from "./worker.js";
import { DatabaseSync } from "node:sqlite";

/* the Durable Object's SQL storage, on Node's SQLite */
function sqlStorage() {
  const db = new DatabaseSync(":memory:");
  return {
    exec(q, ...args) {
      if (!args.length && q.trim().split(";").filter((x) => x.trim()).length > 1) { db.exec(q); return { toArray: () => [] }; }
      const st = db.prepare(q);
      const rows = /^\s*select/i.test(q) ? st.all(...args).map((r) => ({ ...r })) : (st.run(...args), []);
      return { toArray: () => rows };
    },
  };
}
const lobby = new Lobby({ storage: { sql: sqlStorage() } }, {});
const env = { LOBBY: { idFromName: () => "global", get: () => ({ fetch: (r) => lobby.fetch(r) }) } };
let fails = 0;
const check = (ok, what) => { if (!ok) { console.log("  FAIL:", what); fails++; } };

async function call(method, path, form, ip = "84.1.2.3") {
  const init = { method, headers: { "cf-connecting-ip": ip } };
  let url = "https://lobby.test" + path;
  if (form && method === "POST") init.body = new URLSearchParams(form).toString();
  else if (form) url += "?" + new URLSearchParams(form).toString();
  const res = await worker.fetch(new Request(url, init), env);
  const body = await res.text();
  const kv = {}, rows = [];
  for (const line of body.split("\n")) {
    if (line.startsWith("room\t") || line.startsWith("join=")) rows.push(line);
    else if (line.includes("=")) kv[line.slice(0, line.indexOf("="))] = line.slice(line.indexOf("=") + 1);
  }
  return { status: res.status, body, kv, rows, ok: body.startsWith("ok") };
}

const room = { name: "Ryu's room", mode: "koth", public: "1", build: "ABCD", disc: "1234", ver: "0.3",
               addr: "84.1.2.3:27886", lan: "192.168.1.20:27886", players: "1", max: "8", state: "open", id: "", secret: "" };
let h = await call("POST", "/v1/host", room);
check(h.ok && /^[1-9]\d{5}$/.test(h.kv.code) && h.kv.id && h.kv.secret, "open a room: " + h.body);
console.log("open a public room:", h.kv.code);
const { id, secret, code } = h.kv;

let l = await call("GET", "/v1/list", { build: "ABCD" }, "5.6.7.8");
check(l.rows.length === 1 && l.rows[0].includes("Ryu's room") && l.rows[0].includes("koth"), "list: " + l.body);
console.log("list:", JSON.stringify(l.rows));
l = await call("GET", "/v1/list", { build: "OTHER" }, "5.6.7.8");
check(l.rows.length === 0, "list for another build shows it");

let j = await call("POST", "/v1/join", { code, build: "ABCD", name: "Ken", addr: "5.6.7.8:40000", lan: "10.0.0.9:27886" }, "5.6.7.8");
check(j.ok && j.kv.addr === "84.1.2.3:27886" && j.kv.lan === "192.168.1.20:27886" && j.kv.mode === "koth", "join: " + j.body);
console.log("join by code: host at", j.kv.addr, "lan", j.kv.lan);
h = await call("POST", "/v1/host", { ...room, id, secret, code, players: "1" });
check(h.rows.length === 1 && h.rows[0] === "join=5.6.7.8:40000\t10.0.0.9:27886\tKen", "host hears the joiner: " + h.body);
console.log("host's next beat:", JSON.stringify(h.rows));
h = await call("POST", "/v1/host", { ...room, id, secret, code });
check(h.rows.length === 0, "a joiner is passed on once");

j = await call("POST", "/v1/join", { code, build: "XYZ", name: "Ken" }, "5.6.7.8");
check(!j.ok && j.kv.error.includes("different version"), "other build refused: " + j.body);
j = await call("POST", "/v1/join", { code: "999999", build: "ABCD" }, "5.6.7.8");
check(j.kv.error === "No room with that code", "unknown code");
h = await call("POST", "/v1/host", { ...room, id, secret: "wrong" });
check(h.status === 403, "someone else cannot change the room");

const priv = await call("POST", "/v1/host", { ...room, name: "Private", public: "0", addr: "" }, "9.9.9.9");
l = await call("GET", "/v1/list", { build: "ABCD" });
check(l.rows.length === 1, "a private room is not listed");
j = await call("POST", "/v1/join", { code: priv.kv.code, build: "ABCD", name: "Chun" }, "5.6.7.8");
check(j.ok && j.kv.addr === "9.9.9.9:27886", "private room by code, address from the request: " + j.body);
console.log("private room by code:", priv.kv.code, "->", j.kv.addr);

h = await call("POST", "/v1/host", { ...room, id, secret, code, players: "8" });
j = await call("POST", "/v1/join", { code, build: "ABCD" }, "5.6.7.8");
check(j.kv.error === "That room is full", "full room");

await call("POST", "/v1/close", { id, secret });
j = await call("POST", "/v1/join", { code, build: "ABCD" }, "5.6.7.8");
check(j.kv.error === "No room with that code", "closed room gone");

// a room nobody hears from for 45 s is gone; the directory forgetting rooms keeps their code
const realNow = Date.now;
const p2 = await call("POST", "/v1/host", { ...room, id: "", secret: "" }, "7.7.7.7");
Date.now = () => realNow() + 46000;
l = await call("GET", "/v1/list", { build: "ABCD" });
check(l.rows.length === 0, "stale room listed");
const back = await call("POST", "/v1/host", { ...room, id: p2.kv.id, secret: p2.kv.secret, code: p2.kv.code }, "7.7.7.7");
check(back.ok && back.kv.code === p2.kv.code && back.kv.id === p2.kv.id, "re-opened with the same code: " + back.body);
Date.now = realNow;

for (let i = 0; i < 6; i++) await call("POST", "/v1/host", { ...room, id: "", secret: "" }, "3.3.3.3");
const many = await call("POST", "/v1/host", { ...room, id: "", secret: "" }, "3.3.3.3");
check(many.status === 429, "rooms per network limited");
let limited = false;
for (let i = 0; i < 300 && !limited; i++) limited = (await call("GET", "/v1/ping", null, "4.4.4.4")).status === 429;
check(limited, "requests per minute limited");

// ---- the leaderboard ----
const A = { pid: "aaaaaaaaaaaa", key: "key-of-console-a-123" }, B = { pid: "bbbbbbbbbbbb", key: "key-of-console-b-456" };
const C = { pid: "cccccccccccc", key: "key-of-console-c-789" };
const rep = (p, name, match, opp, result, ip = "1.1.1.1") =>
  call("POST", "/v1/report", { pid: p.pid, key: p.key, name, match, opp: opp.pid, result }, ip);
let r1 = await rep(A, "Ryu", "1a2b-1", B, "win");
check(r1.body.includes("waiting"), "first report waits: " + r1.body);
let r2 = await rep(B, "Ken", "1a2b-1", A, "loss", "2.2.2.2");
check(r2.body.includes("counted"), "second agreeing report counts: " + r2.body);
let lb = await call("GET", "/v1/leaderboard", null);
check(lb.kv.total === "2" && lb.rows.length === 0, "leaderboard total");
const rowsOf = (b) => b.split("\n").filter((l) => l.startsWith("row\t")).map((l) => l.split("\t"));
let rows = rowsOf(lb.body);
check(rows[0][2] === "Ryu" && Number(rows[0][3]) > 1000 && rows[1][2] === "Ken" && Number(rows[1][3]) < 1000, "ratings moved: " + lb.body);
console.log("after one fight:", rows.map((r) => r.slice(1).join(" ")).join(" | "));
// disagreeing reports count for nothing
await rep(A, "Ryu", "1a2b-2", B, "win");
r2 = await rep(B, "Ken", "1a2b-2", A, "win");
check(r2.body.includes("mismatch"), "disagreeing reports: " + r2.body);
// someone else using A's id
const forged = await call("POST", "/v1/report", { pid: A.pid, key: "not-the-right-key-000", name: "Ryu", match: "1a2b-3", opp: B.pid, result: "win" });
check(forged.status === 403, "another key for a registered id refused");
// a draw, a streak
await rep(A, "Ryu", "1a2b-4", B, "draw"); await rep(B, "Ken", "1a2b-4", A, "draw");
await rep(A, "Ryu", "77-1", C, "win"); await rep(C, "Chun", "77-1", A, "loss");
await rep(A, "Ryu", "77-2", C, "win"); await rep(C, "Chun", "77-2", A, "loss");
let me = await call("GET", "/v1/player", { pid: A.pid });
check(me.kv.rank === "1" && me.kv.wins === "3" && me.kv.losses === "0" && me.kv.draws === "1" && me.kv.best === "2", "player A: " + me.body);
console.log("player A:", JSON.stringify(me.kv));
let none = await call("GET", "/v1/player", { pid: "zzzzzzzzzzzz" });
check(none.body.includes("none"), "unknown player");
// the same pair, too often in a day
let lim = "";
for (let i = 0; i < 25; i++) { await rep(B, "Ken", `99-${i}`, C, "win"); lim = (await rep(C, "Chun", `99-${i}`, B, "loss")).body; }
check(lim.includes("limit"), "pair limit per day: " + lim);
const page = await worker.fetch(new Request("https://lobby.test/", { headers: { "cf-connecting-ip": "8.8.8.8" } }), env);
const html = await page.text();
check(page.headers.get("content-type").startsWith("text/html") && html.includes("<td>Ryu</td>") && !html.includes("<script"), "web page");
const evil = { pid: "eeeeeeeeeeee", key: "key-of-console-e-000" };
await call("POST", "/v1/report", { pid: evil.pid, key: evil.key, name: "<b>x</b>", match: "5-1", opp: A.pid, result: "loss" });
await rep(A, "Ryu", "5-1", evil, "win");
const html2 = await (await worker.fetch(new Request("https://lobby.test/", { headers: { "cf-connecting-ip": "8.8.8.8" } }), env)).text();
check(html2.includes("&lt;b&gt;x&lt;/b&gt;") && !html2.includes("<b>x</b>"), "names escaped on the page");
lb = await call("GET", "/v1/leaderboard", { n: 3 });
console.log("leaderboard:", rowsOf(lb.body).map((r) => r.slice(1).join(" ")).join(" | "));

console.log(fails ? `\n${fails} check(s) FAILED` : "\nall checks passed");
process.exit(fails ? 1 : 0);
