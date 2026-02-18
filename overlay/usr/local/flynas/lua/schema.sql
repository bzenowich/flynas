CREATE TABLE IF NOT EXISTS config (
    key TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY,
    username TEXT UNIQUE NOT NULL,
    pubkey TEXT,
    shell TEXT DEFAULT '/bin/sh',
    ssh_enabled INTEGER DEFAULT 0,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS groups (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL
);

CREATE TABLE IF NOT EXISTS user_groups (
    user_id INTEGER REFERENCES users(id),
    group_id INTEGER REFERENCES groups(id),
    PRIMARY KEY (user_id, group_id)
);

CREATE TABLE IF NOT EXISTS volumes (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    mountpoint TEXT NOT NULL,
    root_device TEXT NOT NULL,
    scrub_enabled INTEGER DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS volume_disks (
    id INTEGER PRIMARY KEY,
    volume_id INTEGER REFERENCES volumes(id),
    device TEXT NOT NULL,
    added_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS snapshots (
    id INTEGER PRIMARY KEY,
    volume_id INTEGER REFERENCES volumes(id),
    name TEXT NOT NULL,
    created_at TEXT DEFAULT (datetime('now')),
    retention TEXT CHECK (retention IN ('hourly','daily','weekly','yearly'))
);

CREATE TABLE IF NOT EXISTS s3_buckets (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    endpoint TEXT NOT NULL,
    access_key TEXT NOT NULL,
    secret_key TEXT NOT NULL,
    cryfs_password TEXT
);

CREATE TABLE IF NOT EXISTS monitors (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL
        CHECK (type IN ('http','tcp','ping','dns','keyword')),
    target TEXT NOT NULL,
    interval_sec INTEGER DEFAULT 60,
    timeout_ms INTEGER DEFAULT 5000,
    keyword TEXT,
    expected_status INTEGER,
    enabled INTEGER DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS vms (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    cpus INTEGER DEFAULT 1,
    ram_mb INTEGER DEFAULT 1024,
    disk_gb INTEGER DEFAULT 20,
    iso_path TEXT,
    ip_address TEXT,
    netmask TEXT,
    gateway TEXT,
    mac_address TEXT,
    post_install_script TEXT,
    monitor_id INTEGER REFERENCES monitors(id),
    status TEXT DEFAULT 'stopped'
        CHECK (status IN ('running','suspended','stopped'))
);

CREATE TABLE IF NOT EXISTS monitor_events (
    id INTEGER PRIMARY KEY,
    monitor_id INTEGER REFERENCES monitors(id) ON DELETE CASCADE,
    status TEXT NOT NULL CHECK (status IN ('up','down')),
    response_ms INTEGER,
    message TEXT,
    checked_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_monitor_events_lookup
    ON monitor_events (monitor_id, checked_at DESC);

CREATE TABLE IF NOT EXISTS notification_channels (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL CHECK (type IN ('email','webhook')),
    config TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS monitor_notifications (
    monitor_id INTEGER REFERENCES monitors(id) ON DELETE CASCADE,
    channel_id INTEGER REFERENCES notification_channels(id) ON DELETE CASCADE,
    PRIMARY KEY (monitor_id, channel_id)
);

CREATE TABLE IF NOT EXISTS app_templates (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    iso_url TEXT,
    default_cpus INTEGER,
    default_ram_mb INTEGER,
    default_disk_gb INTEGER,
    post_install_script TEXT
);
