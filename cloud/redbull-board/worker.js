// GT Racing PC port: the Red Bull Time Trial's cloud leaderboard (RED_BULL_BOARD + CLOUD_BOARD).
// A Cloudflare Worker over one D1 (SQLite) table, schema.sql.  The offline board of board.c (this PC's player and
// the friends met in multiplayer) shared through a small self-hosted server, reached by the game with WinHTTP
// (host/games/gtracing/features/board.c).
//
//   GET  /board   the 200 best laps, fastest first:
//                 {"v":1,"rows":[{"id":"<10 hex>","name":"...","car":411,"time_ms":77736}, ...]}
//                 "id" is alias(player_id), never the player_id itself.  Kept in the edge cache (caches.default)
//                 for CACHE_SECONDS (30..60, default 45), so a crowd opening the menu reads D1 once a minute or
//                 so per Cloudflare location.  The cache works on a route of your own domain; on *.workers.dev
//                 Cloudflare has no cache, so every GET reads D1 (the game asks at most once a minute).
//   POST /submit  {"player_id":"<8 hex>","name":"...","car":411,"time_ms":77736}, those four keys and no more.
//                 Checked: JSON, the id, the name (1..20 printable ASCII, no < > &: the game's own rule, board.c
//                 clean_name), the car and a lap time within MIN_LAP_MS..MAX_LAP_MS (a lap faster than the F1 RB8
//                 can go is a cheat).  An UPSERT that only ever makes a player's time better.
//                 200 {"ok":true,"improved":true|false}, 400 {"error":"..."} for a bad request.
//
// The player_id is a random number the game made once (redbull_board.txt, "me <id>"): no account, no Windows user
// name, no PC name.  It is the only key a player has, so the board never shows it: GET hands out
// alias(id) = the first 10 hex digits of SHA-256("gtlb1:" + id), the same the game computes to find its own row.
// That is not a login: an id is 32 bits, a determined cheater could search for the id behind an alias.  What it
// stops is the easy way, copying an id off the board and sending a time in that player's name.
//
// Nothing here logs or keeps the sender's IP address.

const TOP = 200;
const CAR = 411;                                   // the Red Bull Time Trial's car, the F1 RB8 (board.c RB_CAR)
const NAME_MAX = 20;                               // board.c clean_name keeps 20 characters
const BODY_MAX = 512;

function json(status, value, extra = {}) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store", ...extra },
  });
}

function intVar(env, name, fallback, lo, hi) {
  const v = Number.parseInt(env?.[name] ?? "", 10);
  return Number.isInteger(v) && v >= lo && v <= hi ? v : fallback;
}

// the limits of a believable lap.  A fast real lap (France Raceway, F1 RB8) is about 1:15.9; MIN_LAP_MS (default 50 s)
// is far below anything driven, MAX_LAP_MS (default 10 min) only keeps junk out.  wrangler.toml [vars] sets both.
function limits(env) {
  return {
    min: intVar(env, "MIN_LAP_MS", 50000, 1000, 3600000),
    max: intVar(env, "MAX_LAP_MS", 600000, 1000, 3600000),
    cache: intVar(env, "CACHE_SECONDS", 45, 30, 60),
  };
}

export async function alias(playerId) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode("gtlb1:" + playerId));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("").slice(0, 10);
}

// a name as board.c makes them: printable ASCII without < > &, at most 20, not only spaces
export function validName(s) {
  return typeof s === "string" && s.length >= 1 && s.length <= NAME_MAX && /^[\x20-\x7e]+$/.test(s) &&
    !/[<>&]/.test(s) && s.trim().length > 0;
}

// the request's body as a submission, or a reason it is not one
export function parseSubmit(text, env) {
  let v;
  try {
    v = JSON.parse(text);
  } catch {
    return { error: "not JSON" };
  }
  if (!v || typeof v !== "object" || Array.isArray(v)) return { error: "not a JSON object" };
  const keys = Object.keys(v).sort().join(",");
  if (keys !== "car,name,player_id,time_ms") return { error: "the keys must be player_id, name, car, time_ms" };
  const { player_id, name, car, time_ms } = v;
  if (typeof player_id !== "string" || !/^[0-9a-f]{8}$/.test(player_id)) return { error: "bad player_id" };
  if (!validName(name)) return { error: "bad name" };
  if (car !== CAR) return { error: "bad car" };
  const lim = limits(env);
  if (!Number.isInteger(time_ms) || time_ms < lim.min || time_ms > lim.max) return { error: "time_ms out of range" };
  return { row: { player_id, name, car, time_ms } };
}

const UPSERT =
  "INSERT INTO board (player_id, name, car, time_ms, updated_at) VALUES (?1, ?2, ?3, ?4, ?5) " +
  "ON CONFLICT(player_id) DO UPDATE SET name = excluded.name, car = excluded.car, " +
  "time_ms = excluded.time_ms, updated_at = excluded.updated_at WHERE excluded.time_ms < board.time_ms";

const TOP_QUERY = `SELECT player_id, name, car, time_ms FROM board ORDER BY time_ms ASC, updated_at ASC LIMIT ${TOP}`;

// one cache entry for every GET /board, whatever its query string (a query string cannot get round the cache)
function cacheKey(url) {
  return new Request(url.origin + "/board", { method: "GET" });
}

async function getBoard(url, env, ctx) {
  const cache = globalThis.caches?.default;
  const key = cacheKey(url);
  if (cache) {
    const hit = await cache.match(key);
    if (hit) return hit;
  }
  const { results } = await env.DB.prepare(TOP_QUERY).all();
  const rows = [];
  for (const r of results ?? [])
    rows.push({ id: await alias(r.player_id), name: r.name, car: r.car, time_ms: r.time_ms });
  const resp = new Response(JSON.stringify({ v: 1, rows }), {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": `public, max-age=${limits(env).cache}`,
    },
  });
  if (cache) ctx.waitUntil(cache.put(key, resp.clone()));
  return resp;
}

async function submit(request, url, env, ctx) {
  const type = request.headers.get("content-type") || "";
  if (!type.toLowerCase().startsWith("application/json")) return json(415, { error: "application/json only" });
  const buf = await request.arrayBuffer();
  if (buf.byteLength > BODY_MAX) return json(413, { error: "too long" });
  let text;
  try {
    text = new TextDecoder("utf-8", { fatal: true }).decode(buf);
  } catch {
    return json(400, { error: "not UTF-8" });
  }
  const p = parseSubmit(text, env);
  if (p.error) return json(400, { error: p.error });
  const r = p.row;
  const res = await env.DB.prepare(UPSERT).bind(r.player_id, r.name, r.car, r.time_ms, Date.now()).run();
  const improved = (res?.meta?.changes ?? 0) > 0;
  const cache = globalThis.caches?.default;
  if (improved && cache) ctx.waitUntil(cache.delete(cacheKey(url)));   // this location shows it at once
  return json(200, { ok: true, improved });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    try {
      if (url.pathname === "/board") {
        if (request.method !== "GET") return json(405, { error: "GET only" }, { allow: "GET" });
        return await getBoard(url, env, ctx);
      }
      if (url.pathname === "/submit") {
        if (request.method !== "POST") return json(405, { error: "POST only" }, { allow: "POST" });
        return await submit(request, url, env, ctx);
      }
      return json(404, { error: "not found" });
    } catch {
      // D1 out of its daily quota or not reachable: the game keeps its own board (board.c falls back offline)
      return json(503, { error: "unavailable" }, { "retry-after": "600" });
    }
  },
};
