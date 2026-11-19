// The Worker (../worker.js) on this PC, without a Cloudflare account: D1 is played by node:sqlite (Node 22.5+),
// caches.default by a small in-memory cache that keeps Cache-Control's max-age.
//
//   node test/local.mjs test                         the Worker's own checks (prints "N of M checks failed")
//   node test/local.mjs serve [options]              http://127.0.0.1:<port>/board and /submit for the game
//       --port 8787          the port (loopback only)
//       --db <file>          a SQLite file instead of memory (":memory:")
//       --seed <n>           n made-up players to start with
//       --status <code>      every answer is that HTTP status (429: the quota is out)
//       --delay <ms>         every answer waits that long (a slow server: the game's timeout)
//       --log <file>         each request (method, path, headers, body) as one JSON line
//
// The game: GT_CLOUD_BOARD_URL=http://127.0.0.1:8787 (board.c takes plain http only for this PC).
import { DatabaseSync } from "node:sqlite";
import { readFileSync, appendFileSync } from "node:fs";
import http from "node:http";
import worker, { alias, parseSubmit, validName } from "../worker.js";

const here = new URL(".", import.meta.url);
const schema = readFileSync(new URL("../schema.sql", here), "utf8");

class Stmt {
  constructor(db, sql, args) { this.db = db; this.sql = sql; this.args = args; }
  bind(...args) { return new Stmt(this.db, this.sql, args); }
  async all() { return { results: this.db.prepare(this.sql).all(...this.args), success: true }; }
  async first() { return this.db.prepare(this.sql).get(...this.args) ?? null; }
  async run() {
    const r = this.db.prepare(this.sql).run(...this.args);
    return { success: true, meta: { changes: Number(r.changes) } };
  }
}
class D1 {
  constructor(file = ":memory:") { this.db = new DatabaseSync(file); this.db.exec(schema); }
  prepare(sql) { return new Stmt(this.db, sql, []); }
}

class EdgeCache {
  constructor() { this.m = new Map(); this.hits = 0; }
  async match(req) {
    const e = this.m.get(req.url);
    if (!e || Date.now() > e.until) return undefined;
    this.hits++;
    return new Response(e.body, { headers: e.headers });
  }
  async put(req, resp) {
    const age = /max-age=(\d+)/.exec(resp.headers.get("cache-control") || "");
    if (!age) return;
    this.m.set(req.url, { body: await resp.text(), headers: [...resp.headers], until: Date.now() + 1000 * +age[1] });
  }
  async delete(req) { return this.m.delete(req.url); }
}

function makeEnv(file) {
  return { DB: new D1(file), MIN_LAP_MS: "50000", MAX_LAP_MS: "600000", CACHE_SECONDS: "45" };
}

async function call(env, method, path, body, type = "application/json") {
  const pending = [];
  const init = { method, headers: body !== undefined ? { "content-type": type } : {} };
  if (body !== undefined) init.body = typeof body === "string" ? body : JSON.stringify(body);
  const resp = await worker.fetch(new Request("http://board.test" + path, init), env, { waitUntil: (p) => pending.push(p) });
  await Promise.all(pending);
  const text = await resp.text();
  let value = null;
  try { value = JSON.parse(text); } catch { /* not JSON */ }
  return { status: resp.status, value, headers: resp.headers };
}

async function selfTest() {
  let checks = 0, fails = 0;
  const check = (ok, what) => { checks++; if (!ok) { fails++; console.log("FAIL: " + what); } };
  globalThis.caches = { default: new EdgeCache() };
  const env = makeEnv();
  const sub = (id, name, ms, car = 411) => call(env, "POST", "/submit", { player_id: id, name, car, time_ms: ms });

  // the alias: board.c's selftest checks the same value (SHA-256 of "gtlb1:0123abcd")
  check(await alias("0123abcd") === "edd6d2700d", "alias(0123abcd) = " + await alias("0123abcd"));

  check(validName("Racer") && validName("a b") && !validName("") && !validName("   ") && !validName("x".repeat(21)) &&
        !validName("<b>") && !validName("a&b") && !validName("é") && !validName("a\nb"), "names");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":411,"time_ms":77736}', env).row?.time_ms === 77736, "a good row");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":411,"time_ms":77736,"pc":"x"}', env).error, "a fifth key");
  check(parseSubmit('{"player_id":"0123ABCD","name":"A","car":411,"time_ms":77736}', env).error, "upper-case id");
  check(parseSubmit('{"player_id":"0123abcd00","name":"A","car":411,"time_ms":77736}', env).error, "10-digit id (an alias)");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":411,"time_ms":49999}', env).error, "a lap under 50 s");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":411,"time_ms":600001}', env).error, "a lap over 10 min");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":411,"time_ms":77736.5}', env).error, "a fraction");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":"411","time_ms":77736}', env).error, "car as text");
  check(parseSubmit('{"player_id":"0123abcd","name":"A","car":412,"time_ms":77736}', env).error, "another car");
  check(parseSubmit("[1,2]", env).error && parseSubmit("{", env).error, "not an object");

  let r = await sub("0123abcd", "Me", 80000);
  check(r.status === 200 && r.value.improved === true, "first submit: " + JSON.stringify(r.value));
  r = await sub("0123abcd", "Me later", 81000);
  check(r.status === 200 && r.value.improved === false, "a slower lap is not kept");
  r = await sub("0123abcd", "Me faster", 79000);
  check(r.status === 200 && r.value.improved === true, "a faster lap is kept");
  await sub("89abcdef", "Friend", 78000);
  await sub("fedcba98", "Third", 90000);
  r = await sub("fedcba98", "x", 40000);
  check(r.status === 400, "a cheat lap: " + r.status);
  r = await call(env, "POST", "/submit", "{}", "text/plain");
  check(r.status === 415, "text/plain: " + r.status);
  r = await call(env, "POST", "/submit", JSON.stringify({ player_id: "0123abcd", name: "A".repeat(600), car: 411, time_ms: 77736 }));
  check(r.status === 413, "a long body: " + r.status);

  r = await call(env, "GET", "/board");
  const rows = r.value?.rows ?? [];
  check(r.status === 200 && rows.length === 3, "GET: " + rows.length + " rows");
  check(rows.map((x) => x.time_ms).join() === "78000,79000,90000", "fastest first: " + rows.map((x) => x.time_ms));
  check(rows[1]?.name === "Me faster" && rows[1]?.id === await alias("0123abcd"), "the alias, not the id");
  check(!JSON.stringify(r.value).includes("0123abcd"), "no player_id in GET");
  const hits = globalThis.caches.default.hits;
  await sub("0123abcd", "x", 95000);                       // not better: the cache stays
  await call(env, "GET", "/board?bust=1");
  check(globalThis.caches.default.hits === hits + 1, "the second GET (any query string) from the cache");
  await sub("0123abcd", "Me fastest", 77000);              // better: the cache entry goes
  r = await call(env, "GET", "/board");
  check(r.value.rows[0].time_ms === 77000, "a better lap shows at once");
  check(r.headers.get("cache-control") === "public, max-age=45", "max-age 45");

  for (let i = 0; i < 230; i++) await sub((0x10000000 + i).toString(16), "P" + i, 100000 + i);
  r = await call(env, "GET", "/board?n=2");
  check(r.value.rows.length === 200, "the top 200 only: " + r.value.rows.length);   // (stale cache: same 4 rows?)
  check((await call(env, "PUT", "/board")).status === 405 && (await call(env, "GET", "/submit")).status === 405 &&
        (await call(env, "GET", "/")).status === 404, "405 / 404");
  const broken = { ...env, DB: { prepare() { throw new Error("D1 quota"); } } };
  globalThis.caches = { default: new EdgeCache() };
  r = await call(broken, "GET", "/board");
  check(r.status === 503, "D1 out: 503");

  console.log(`${fails} of ${checks} checks failed`);
  process.exit(fails ? 1 : 0);
}

function serve(args) {
  const opt = (name, d) => { const i = args.indexOf("--" + name); return i >= 0 ? args[i + 1] : d; };
  const port = +opt("port", 8787), status = +opt("status", 0), delay = +opt("delay", 0), log = opt("log", "");
  globalThis.caches = { default: new EdgeCache() };
  const env = makeEnv(opt("db", ":memory:"));
  const seed = +opt("seed", 0);
  for (let i = 0; i < seed; i++)
    env.DB.db.prepare("INSERT OR REPLACE INTO board VALUES (?, ?, 411, ?, ?)")
      .run((0x50000000 + i * 7919).toString(16),                     // some "iPad"s and twins: the rows' tags
           i % 4 === 1 ? "iPad" : i % 5 === 2 ? "Twin Racer" : "Cloud " + (i + 1), 70000 + i * 1500, Date.now());
  http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", async () => {
      const body = Buffer.concat(chunks);
      if (log) appendFileSync(log, JSON.stringify({ method: req.method, path: req.url, headers: req.headers,
                                                    body: body.toString("utf8") }) + "\n");
      if (delay) await new Promise((ok) => setTimeout(ok, delay));
      if (status) {
        res.writeHead(status, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: "forced " + status }));
        return;
      }
      const pending = [];
      const init = { method: req.method, headers: req.headers };
      if (req.method !== "GET" && req.method !== "HEAD") init.body = body;
      const resp = await worker.fetch(new Request(`http://127.0.0.1:${port}${req.url}`, init), env,
                                      { waitUntil: (p) => pending.push(p) });
      await Promise.all(pending);
      res.writeHead(resp.status, Object.fromEntries(resp.headers));
      res.end(Buffer.from(await resp.arrayBuffer()));
    });
  }).listen(port, "127.0.0.1", () => console.log(`board on http://127.0.0.1:${port} (seed ${seed}, status ${status || "real"})`));
}

const [mode, ...rest] = process.argv.slice(2);
if (mode === "test") await selfTest();
else if (mode === "serve") serve(rest);
else console.log("node test/local.mjs test | serve [--port n] [--db file] [--seed n] [--status code] [--delay ms] [--log file]");
