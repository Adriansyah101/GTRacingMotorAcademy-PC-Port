# Red Bull leaderboard server (CLOUD_BOARD)

This is the server for the Red Bull Time Trial leaderboard that the PC port can share online. It is a
Cloudflare Worker with one D1 (SQLite) table. The game talks to it from
`host/games/gtracing/features/board.c` through Windows' own WinHTTP.

This folder is not part of the release folder. The game uses the project's server by default; a
`GT_CLOUD_BOARD_URL` in `gtracing.ini` points it at another one.

| File | What it is |
|---|---|
| `schema.sql` | The table: `player_id` (primary key), `name`, `car`, `time_ms`, `updated_at`. |
| `worker.js` | Two endpoints: `GET /board` and `POST /submit`. |
| `wrangler.toml` | The Worker, its D1 binding and its limits. |
| `test/local.mjs` | The same Worker run on this PC, with Node's SQLite standing in for D1. |

## The two endpoints

### `GET /board`

Returns the 200 best laps, fastest first:

```json
{"v":1,"rows":[{"id":"edd6d2700d","name":"Racer","car":411,"time_ms":77736}]}
```

- `id` is not the player's id. It is the first 10 hex digits of SHA-256(`"gtlb1:" + player_id`). The game
  computes the same value to recognise its own row.
- The answer is kept in the edge cache (`caches.default`) for `CACHE_SECONDS` (30 to 60, default 45).
- The cache only works on a route of your own domain. On `*.workers.dev`, Cloudflare has no cache. Each game
  still asks at most once a minute.

### `POST /submit`

Takes exactly these four keys:

```json
{"player_id":"1a2b3c4d","name":"Racer","car":411,"time_ms":75868}
```

Anything else gets a 400 answer:

- another key, or a key missing;
- an id that is not 8 lower-case hex digits;
- a name that is not 1 to 20 printable ASCII characters, or that contains `<`, `>` or `&`;
- a car other than 411 (the F1 RB8);
- a lap outside `MIN_LAP_MS` to `MAX_LAP_MS`.

The defaults are 50 s and 10 min. A fast real lap on France Raceway is about 1:15.9. Raise `MIN_LAP_MS` once
you know how fast the best players really are.

A valid lap is saved with an UPSERT, and only a faster time replaces the stored one. The answer is
`{"ok":true,"improved":true|false}`.

When D1 is out of quota or cannot be reached, the server answers 503. The game then keeps its own board and
tries again 10 minutes later. It does the same on a 429.

## Deploying

You need a Cloudflare account and Node.js. `npx` downloads wrangler.

```
cd cloud/redbull-board
npx wrangler login
npx wrangler d1 create gt-redbull-board
```

1. Put the `database_id` printed by the last command into `wrangler.toml`.
2. Create the table, then deploy:

   ```
   npx wrangler d1 execute gt-redbull-board --remote --file=schema.sql
   npx wrangler deploy
   ```

3. For the edge cache, add a route on your own domain in `wrangler.toml` and deploy again.
4. Players put the address in `gtracing.ini`:

   ```
   GT_CLOUD_BOARD_URL=https://gt-redbull-board.<your-subdomain>.workers.dev
   ```

## Trying it without Cloudflare

```
node test/local.mjs test
node test/local.mjs serve --seed 12
```

- `test` runs the Worker's own checks: 28, using the same `alias()` value as the game's `--selftest`.
- `serve` runs the Worker at `http://127.0.0.1:8787` with 12 made-up players. The game takes plain `http`
  only for this PC: set `GT_CLOUD_BOARD_URL=http://127.0.0.1:8787`.
- `serve` also accepts:
  - `--status 429` to make every answer that status;
  - `--delay 9000` to make every answer that slow;
  - `--log <file>` to record each request.

## What a player sends, and what the server keeps

**The player's id** is a random number the game made once (`me <id>` in `redbull_board.txt`).

**The name** is the one the player goes by (PLAYER_NAME).

**The car** is always 411, and **the lap** is in ms.

Nothing else is sent:

- no Windows user name;
- no PC name;
- no folders.

The Worker does not log or store the sender's IP address. Cloudflare's own request logs are a matter of the
account's settings.

The id is a player's only key, so the board never shows it. The alias stops the easy way to post in another
player's name, which is copying their id off the board. It is not a login: an id is 32 bits, and a
determined person could search for the id behind an alias.
