/*
 * The lobby directory for online VS on the 3DS (port/platform/3ds/lobby_3ds.c):
 * a Cloudflare Worker with one Durable Object holding the open rooms in memory.
 *
 * Rooms are hosted by a player's 3DS; this only tells consoles about each
 * other: the public room list, the 6-digit code of every room (private rooms
 * are joined only by their code), and each joiner's address passed to the
 * host so both consoles send to each other at once (that opens most home
 * routers without port forwarding).  No game data goes through it.
 *
 * Text in, text out (simple to read on the 3DS):
 *   POST /v1/host   form: id, secret (empty to open a room), name, mode
 *                   (1v1 | koth), public (0 | 1), build, disc, ver, addr
 *                   (the room's address as STUN saw it), lan, players, max,
 *                   state (open | playing).  Every 5 s while the room is open.
 *                   -> ok / id= / code= / secret= / ip= / join=addr<TAB>lan<TAB>name ...
 *   POST /v1/close  form: id, secret -> ok
 *   GET  /v1/list?build=   -> ok / room<TAB>code<TAB>name<TAB>mode<TAB>players<TAB>max<TAB>state ...
 *   POST /v1/join   form: code, build, name, addr, lan
 *                   -> ok / addr= / lan= / name= / mode= / ip=   or   error=...
 *   GET  /v1/ping   -> ok
 * A room not heard from for 45 s is gone.
 *
 * The leaderboard (kept in the Durable Object's SQLite storage):
 *   POST /v1/report form: pid (the console's player id), key (its secret,
 *                   kept on its SD card: the first report registers it), name,
 *                   match (the room's match id and fight number), opp (the
 *                   other player's id), result (win | loss | draw)
 *                   -> ok / counted | waiting | mismatch | limit
 *                   Both players' consoles report every fight; it counts only
 *                   when the two reports agree.  Then both ratings move (Elo,
 *                   1000 to start) and wins / losses / draws / win streaks.
 *                   The same two players count at most 20 times a day.
 *   GET  /v1/leaderboard?from=&n=  -> ok / total= / row<TAB>rank<TAB>name<TAB>
 *                   rating<TAB>wins<TAB>losses<TAB>draws<TAB>best streak ...
 *   GET  /v1/player?pid=  -> ok / rank= rating= wins= losses= draws= best= name=  (or "none")
 *   GET  /          the leaderboard as a web page
 */

const ROOM_TTL_MS = 45000;
const MAX_ROOMS = 300;
const MAX_ROOMS_PER_IP = 6;
const RATE_PER_MIN = 240;
const REPORT_TTL_MS = 15 * 60000;
const PAIR_PER_DAY = 20;

function text(body, status = 200) {
  return new Response(body.endsWith("\n") ? body : body + "\n", {
    status,
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" },
  });
}

function clean(s, n) {
  return String(s ?? "").replace(/[\t\r\n\x00-\x1f]/g, " ").trim().slice(0, n);
}

function addr(s) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?::(\d{1,5}))?$/.exec(String(s ?? "").trim());
  if (!m) return "";
  if ([1, 2, 3, 4].some((i) => Number(m[i]) > 255)) return "";
  const port = m[5] ? Number(m[5]) : 27886;
  if (port < 1 || port > 65535) return "";
  return `${m[1]}.${m[2]}.${m[3]}.${m[4]}:${port}`;
}

function rand(n, alphabet) {
  const b = new Uint8Array(n);
  crypto.getRandomValues(b);
  let s = "";
  for (const x of b) s += alphabet[x % alphabet.length];
  return s;
}

async function sha256(s) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.rooms = new Map();   // id -> room
    this.codes = new Map();   // code -> id
    this.rate = new Map();    // ip -> { t, n }
    this.sql = state.storage && state.storage.sql;
    if (this.sql)
      this.sql.exec(`
        CREATE TABLE IF NOT EXISTS players (pid TEXT PRIMARY KEY, keyhash TEXT NOT NULL, name TEXT NOT NULL,
          rating INTEGER NOT NULL DEFAULT 1000, wins INTEGER NOT NULL DEFAULT 0, losses INTEGER NOT NULL DEFAULT 0,
          draws INTEGER NOT NULL DEFAULT 0, streak INTEGER NOT NULL DEFAULT 0, best INTEGER NOT NULL DEFAULT 0,
          seen INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX IF NOT EXISTS players_rating ON players (rating DESC, wins DESC);
        CREATE TABLE IF NOT EXISTS reports (match TEXT NOT NULL, pid TEXT NOT NULL, opp TEXT NOT NULL,
          result TEXT NOT NULL, t INTEGER NOT NULL, PRIMARY KEY (match, pid));
        CREATE TABLE IF NOT EXISTS pairs (pair TEXT NOT NULL, day INTEGER NOT NULL, n INTEGER NOT NULL,
          PRIMARY KEY (pair, day));`);
  }

  rows(q, ...args) {
    return this.sql.exec(q, ...args).toArray();
  }

  /* one fight's result, from one of its two players */
  async report(f, now) {
    if (!this.sql) return text("error=No leaderboard storage", 503);
    const pid = (f.get("pid") || "").trim(), key = f.get("key") || "", opp = (f.get("opp") || "").trim();
    const match = (f.get("match") || "").trim(), result = f.get("result") || "";
    const name = clean(f.get("name"), 16) || "Player";
    if (!/^[a-z0-9]{12}$/.test(pid) || !/^[a-z0-9]{12}$/.test(opp) || pid === opp) return text("error=Bad player id", 400);
    if (key.length < 16 || key.length > 64) return text("error=Bad player key", 400);
    if (!/^[0-9a-f]{1,16}-\d{1,4}$/.test(match)) return text("error=Bad match id", 400);
    if (!["win", "loss", "draw"].includes(result)) return text("error=Bad result", 400);
    const kh = await sha256(key);
    const me = this.rows("SELECT keyhash FROM players WHERE pid = ?", pid)[0];
    if (me && me.keyhash !== kh) return text("error=That player id belongs to another console", 403);
    if (!me) this.sql.exec("INSERT INTO players (pid, keyhash, name, seen) VALUES (?, ?, ?, ?)", pid, kh, name, now);
    else this.sql.exec("UPDATE players SET name = ?, seen = ? WHERE pid = ?", name, now, pid);
    this.sql.exec("DELETE FROM reports WHERE t < ?", now - REPORT_TTL_MS);
    this.sql.exec("INSERT OR IGNORE INTO reports (match, pid, opp, result, t) VALUES (?, ?, ?, ?, ?)", match, pid, opp, result, now);
    const other = this.rows("SELECT opp, result FROM reports WHERE match = ? AND pid = ?", match, opp)[0];
    if (!other) return text("ok\nwaiting");
    this.sql.exec("DELETE FROM reports WHERE match = ? AND (pid = ? OR pid = ?)", match, pid, opp);
    const agree = other.opp === pid && ((result === "draw" && other.result === "draw") ||
      (result === "win" && other.result === "loss") || (result === "loss" && other.result === "win"));
    if (!agree) return text("ok\nmismatch");
    const day = Math.floor(now / 86400000), pair = [pid, opp].sort().join(":");
    this.sql.exec("DELETE FROM pairs WHERE day < ?", day - 1);
    const n = this.rows("SELECT n FROM pairs WHERE pair = ? AND day = ?", pair, day)[0]?.n ?? 0;
    if (n >= PAIR_PER_DAY) return text("ok\nlimit");
    this.sql.exec("INSERT INTO pairs (pair, day, n) VALUES (?, ?, 1) ON CONFLICT (pair, day) DO UPDATE SET n = n + 1", pair, day);
    this.apply(result === "draw" ? null : result === "win" ? pid : opp, pid, opp);
    const r = this.rows("SELECT rating FROM players WHERE pid = ?", pid)[0];
    return text(`ok\ncounted\nrating=${r ? r.rating : 1000}`);
  }

  /* Elo: K 40 for a player's first 20 fights, then 24 */
  apply(winner, a, b) {
    const pa = this.rows("SELECT * FROM players WHERE pid = ?", a)[0], pb = this.rows("SELECT * FROM players WHERE pid = ?", b)[0];
    if (!pa || !pb) return;
    const ea = 1 / (1 + 10 ** ((pb.rating - pa.rating) / 400));
    const sa = winner === null ? 0.5 : winner === a ? 1 : 0;
    const ka = pa.wins + pa.losses + pa.draws < 20 ? 40 : 24, kb = pb.wins + pb.losses + pb.draws < 20 ? 40 : 24;
    const ra = Math.round(pa.rating + ka * (sa - ea)), rb = Math.round(pb.rating + kb * ((1 - sa) - (1 - ea)));
    const upd = (p, rating, s) => {
      if (s === 1) this.sql.exec("UPDATE players SET rating = ?, wins = wins + 1, streak = streak + 1, best = MAX(best, streak + 1) WHERE pid = ?", rating, p.pid);
      else if (s === 0) this.sql.exec("UPDATE players SET rating = ?, losses = losses + 1, streak = 0 WHERE pid = ?", rating, p.pid);
      else this.sql.exec("UPDATE players SET rating = ?, draws = draws + 1, streak = 0 WHERE pid = ?", rating, p.pid);
    };
    upd(pa, ra, sa);
    upd(pb, rb, 1 - sa);
  }

  board(from, n) {
    return this.rows(`SELECT pid, name, rating, wins, losses, draws, best FROM players
                      WHERE wins + losses + draws > 0 ORDER BY rating DESC, wins DESC, pid LIMIT ? OFFSET ?`, n, from);
  }

  leaderboard(f) {
    if (!this.sql) return text("error=No leaderboard storage", 503);
    const from = Math.max(0, Math.min(100000, Number(f.get("from")) || 0)), n = Math.max(1, Math.min(50, Number(f.get("n")) || 20));
    const total = this.rows("SELECT COUNT(*) AS c FROM players WHERE wins + losses + draws > 0")[0].c;
    let out = `ok\ntotal=${total}\n`;
    this.board(from, n).forEach((p, i) =>
      (out += `row\t${from + i + 1}\t${p.name}\t${p.rating}\t${p.wins}\t${p.losses}\t${p.draws}\t${p.best}\n`));
    return text(out);
  }

  player(f) {
    if (!this.sql) return text("error=No leaderboard storage", 503);
    const p = this.rows("SELECT * FROM players WHERE pid = ?", (f.get("pid") || "").trim())[0];
    if (!p || p.wins + p.losses + p.draws === 0) return text("ok\nnone");
    const rank = this.rows(`SELECT COUNT(*) AS c FROM players WHERE wins + losses + draws > 0 AND
                            (rating > ? OR (rating = ? AND (wins > ? OR (wins = ? AND pid < ?))))`,
                           p.rating, p.rating, p.wins, p.wins, p.pid)[0].c + 1;
    return text(`ok\nrank=${rank}\nrating=${p.rating}\nwins=${p.wins}\nlosses=${p.losses}\ndraws=${p.draws}\nbest=${p.best}\nname=${p.name}`);
  }

  page() {
    const rows = this.sql ? this.board(0, 100) : [];
    const tr = rows.map((p, i) => `<tr><td>${i + 1}</td><td>${esc(p.name)}</td><td>${p.rating}</td><td>${p.wins}</td>` +
      `<td>${p.losses}</td><td>${p.draws}</td><td>${p.best}</td></tr>`).join("");
    const html = `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>MvC2 3DS leaderboard</title>
<style>
:root { --bg: #0e0b1c; --fg: #f0f0f8; --dim: #9a9cb2; --gold: #ffcc40; --row: #1b1630; }
@media (prefers-color-scheme: light) { :root { --bg: #f6f5fb; --fg: #17142a; --dim: #5d5a73; --gold: #9a6a00; --row: #ebe8f6; } }
body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.4 system-ui, sans-serif; }
main { max-width: 720px; margin: 0 auto; padding: 24px 16px; }
h1 { font-size: 24px; margin: 0 0 4px; } p { color: var(--dim); margin: 0 0 20px; }
table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { text-align: right; padding: 8px 6px; } th:nth-child(2), td:nth-child(2) { text-align: left; }
th { color: var(--dim); font-weight: 600; font-size: 13px; } tbody tr:nth-child(odd) { background: var(--row); }
tbody tr:nth-child(-n+3) td:first-child { color: var(--gold); font-weight: 700; }
td:nth-child(2) { overflow-wrap: anywhere; }
</style></head><body><main><h1>Marvel vs. Capcom 2 on 3DS: leaderboard</h1>
<p>Online VS results that both players' consoles reported. Rating starts at 1000.</p>
${rows.length ? `<table><thead><tr><th>#</th><th>Player</th><th>Rating</th><th>W</th><th>L</th><th>D</th><th>Best streak</th></tr></thead><tbody>${tr}</tbody></table>`
               : "<p>No results yet.</p>"}
</main></body></html>`;
    return new Response(html, { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" } });
  }

  sweep(now) {
    for (const [id, r] of this.rooms)
      if (now - r.seen > ROOM_TTL_MS) {
        this.rooms.delete(id);
        this.codes.delete(r.code);
      }
    for (const [ip, e] of this.rate) if (now - e.t > 60000) this.rate.delete(ip);
  }

  limited(ip, now) {
    let e = this.rate.get(ip);
    if (!e || now - e.t > 60000) e = { t: now, n: 0 };
    e.n++;
    this.rate.set(ip, e);
    return e.n > RATE_PER_MIN;
  }

  newCode() {
    for (let i = 0; i < 50; i++) {
      const c = rand(6, "0123456789");
      if (c[0] !== "0" && !this.codes.has(c)) return c;
    }
    return "";
  }

  async fetch(request) {
    const url = new URL(request.url);
    const now = Date.now();
    const ip = request.headers.get("cf-connecting-ip") || "0.0.0.0";
    this.sweep(now);
    if (this.limited(ip, now)) return text("error=Too many requests, try again in a minute", 429);
    const f = request.method === "POST" ? new URLSearchParams(await request.text()) : url.searchParams;
    switch (url.pathname) {
      case "/":
        return this.page();
      case "/v1/ping":
        return text("ok");
      case "/v1/report":
        return this.report(f, now);
      case "/v1/leaderboard":
        return this.leaderboard(f);
      case "/v1/player":
        return this.player(f);
      case "/v1/host":
        return this.host(f, ip, now);
      case "/v1/close": {
        const r = this.rooms.get(f.get("id") || "");
        if (r && r.secret === f.get("secret")) {
          this.rooms.delete(r.id);
          this.codes.delete(r.code);
        }
        return text("ok");
      }
      case "/v1/list":
        return this.list(f, now);
      case "/v1/join":
        return this.join(f, ip, now);
      default:
        return text("error=Unknown request", 404);
    }
  }

  host(f, ip, now) {
    const id = f.get("id") || "";
    let r = id ? this.rooms.get(id) : undefined;
    if (r && r.secret !== f.get("secret")) return text("error=Not your room", 403);
    if (!r) {
      if (this.rooms.size >= MAX_ROOMS) return text("error=The lobby is full, try again later", 503);
      let mine = 0;
      for (const x of this.rooms.values()) if (x.ip === ip) mine++;
      if (mine >= MAX_ROOMS_PER_IP) return text("error=Too many rooms from your network", 429);
      // a room the directory forgot (it restarted): keep its id and code when free
      const oldCode = /^[1-9]\d{5}$/.test(f.get("code") || "") ? f.get("code") : "";
      const code = oldCode && !this.codes.has(oldCode) ? oldCode : this.newCode();
      if (!code) return text("error=No free room code, try again", 503);
      const secret = id && f.get("secret") ? f.get("secret") : rand(16, "abcdefghijklmnopqrstuvwxyz0123456789");
      r = { id: id || rand(12, "abcdefghijklmnopqrstuvwxyz0123456789"), secret, code, joins: [] };
      this.rooms.set(r.id, r);
      this.codes.set(code, r.id);
    }
    r.ip = ip;
    r.seen = now;
    r.name = clean(f.get("name"), 24) || "Room";
    r.mode = f.get("mode") === "koth" ? "koth" : "1v1";
    r.pub = f.get("public") === "1";
    r.build = clean(f.get("build"), 16);
    r.disc = clean(f.get("disc"), 16);
    r.ver = clean(f.get("ver"), 16);
    r.addr = addr(f.get("addr")) || `${ip}:27886`;
    r.lan = addr(f.get("lan"));
    r.players = Math.max(1, Math.min(8, Number(f.get("players")) || 1));
    r.max = Math.max(2, Math.min(8, Number(f.get("max")) || 8));
    r.state = f.get("state") === "playing" ? "playing" : "open";
    let out = `ok\nid=${r.id}\ncode=${r.code}\nsecret=${r.secret}\nip=${ip}\n`;
    for (const j of r.joins) if (now - j.t < 30000) out += `join=${j.addr}\t${j.lan}\t${j.name}\n`;
    r.joins = [];
    return text(out);
  }

  list(f, now) {
    const build = clean(f.get("build"), 16);
    const rows = [];
    for (const r of this.rooms.values()) {
      if (!r.pub || (build && r.build !== build) || now - r.seen > ROOM_TTL_MS) continue;
      rows.push(r);
    }
    rows.sort((a, b) => (a.state === b.state ? b.players - a.players : a.state === "open" ? -1 : 1));
    let out = "ok\n";
    for (const r of rows.slice(0, 40))
      out += `room\t${r.code}\t${r.name}\t${r.mode}\t${r.players}\t${r.max}\t${r.state}\n`;
    return text(out);
  }

  join(f, ip, now) {
    const code = (f.get("code") || "").trim();
    const r = this.rooms.get(this.codes.get(code) || "");
    if (!r || now - r.seen > ROOM_TTL_MS) return text("error=No room with that code");
    const build = clean(f.get("build"), 16);
    if (build && r.build && build !== r.build)
      return text(`error=That room runs a different version (v${r.ver}); both need the same release`);
    if (r.players >= r.max) return text("error=That room is full");
    const me = addr(f.get("addr")) || `${ip}:27886`;
    const j = { addr: me, lan: addr(f.get("lan")), name: clean(f.get("name"), 16) || "Player", t: now };
    r.joins = r.joins.filter((x) => x.addr !== j.addr).slice(-7);
    r.joins.push(j);
    return text(`ok\naddr=${r.addr}\nlan=${r.lan}\nname=${r.name}\nmode=${r.mode}\nip=${ip}\n`);
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname !== "/" && !url.pathname.startsWith("/v1/")) return text("error=Not found", 404);
    const stub = env.LOBBY.get(env.LOBBY.idFromName("global"));
    return stub.fetch(request);
  },
};
