const TOP = 200;
const CAR = 411;
const NAME_MAX = 20;
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

export function validName(s) {
  return typeof s === "string" && s.length >= 1 && s.length <= NAME_MAX && /^[\x20-\x7e]+$/.test(s) &&
    !/[<>&]/.test(s) && s.trim().length > 0;
}

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
  if (improved && cache) ctx.waitUntil(cache.delete(cacheKey(url)));
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
      return json(503, { error: "unavailable" }, { "retry-after": "600" });
    }
  },
};
