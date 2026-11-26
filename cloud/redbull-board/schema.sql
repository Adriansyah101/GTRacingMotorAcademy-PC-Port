CREATE TABLE IF NOT EXISTS board (
    player_id  TEXT    PRIMARY KEY,
    name       TEXT    NOT NULL,
    car        INTEGER NOT NULL,
    time_ms    INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS board_by_time ON board (time_ms, updated_at);
