-- GT Racing PC port: the Red Bull Time Trial's cloud leaderboard (RED_BULL_BOARD + CLOUD_BOARD), Cloudflare D1.
-- One row per player: the player's random id from redbull_board.txt ("me <id>", 8 hex digits), the name the
-- player goes by (PLAYER_NAME), the car (411, the F1 RB8 of the Time Trial) and the best lap in ms.
-- The id is never shown: GET /board hands out a short hash of it (worker.js, alias()), so a row cannot be
-- written in someone else's name by copying their id from the board.
--
--   npx wrangler d1 execute gt-redbull-board --remote --file=schema.sql

CREATE TABLE IF NOT EXISTS board (
    player_id  TEXT    PRIMARY KEY,
    name       TEXT    NOT NULL,
    car        INTEGER NOT NULL,
    time_ms    INTEGER NOT NULL,
    updated_at INTEGER NOT NULL            -- Unix time in ms of the last better lap
);

-- GET /board: the 200 best, fastest first (ties: the earlier lap first)
CREATE INDEX IF NOT EXISTS board_by_time ON board (time_ms, updated_at);
