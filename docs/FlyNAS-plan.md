# FlyNAS Implementation Plan

A NAS appliance built on DragonFlyBSD with HAMMER2, managed via a web UI.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                  Browser (UI)                   │
│          Clay (C → WASM) + HTML renderer        │
└──────────────────────┬──────────────────────────┘
                       │ HTTP/REST
┌──────────────────────┴──────────────────────────┐
│               OpenResty (nginx + LuaJIT)        │
│     REST API  ·  Static files  ·  Monitor worker │
│         Session auth  ·  Notifications          │
└──┬───────────┬───────────┬──────────────────────┘
   │           │           │
   ▼           ▼           ▼
  SQLite     HAMMER2    QEMU/NVMM
 (config)   (storage)    (VMs)
```

**Key principles:** Keep it simple. Lua for backend logic. C/WASM for frontend. Shell scripts for system operations. SQLite for persistent config only.

---

## Phase 1: Foundation

### 1.1 Base System Setup

- [ ] Install DragonFlyBSD with HAMMER2 root filesystem (dev VM `h2dev` runs UFS root + RAID6-patched HAMMER2 scratch disks)
- [x] Install packages: `openresty`, `lua51-cjson`, `libargon2` (qemu/rclone/git deferred to later phases)
- [x] SQLite access for LuaJIT — implemented as FFI binding (`util/db.lua`), no lsqlite3 dependency
- [x] Create `flynas` system user and group
- [x] Write `/usr/local/etc/rc.d/flynas` rc.d script (starts openresty with flynas prefix)

### 1.2 SQLite Schema

Single database file at `/usr/local/flynas/flynas.db`. Opened with WAL mode for concurrent reader/writer access across nginx workers.

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

-- Core
CREATE TABLE config (key TEXT PRIMARY KEY, value TEXT);  -- JSON stored as TEXT
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    username TEXT UNIQUE NOT NULL,
    pubkey TEXT,
    shell TEXT DEFAULT '/bin/sh',
    ssh_enabled INTEGER DEFAULT 0,
    created_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE groups (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL
);
CREATE TABLE user_groups (
    user_id INTEGER REFERENCES users(id),
    group_id INTEGER REFERENCES groups(id),
    PRIMARY KEY (user_id, group_id)
);

-- Storage (HAMMER2 multi-volume)
CREATE TABLE volumes (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    mountpoint TEXT NOT NULL,
    root_device TEXT NOT NULL,       -- device used in newfs_hammer2
    scrub_enabled INTEGER DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE volume_disks (
    id INTEGER PRIMARY KEY,
    volume_id INTEGER REFERENCES volumes(id),
    device TEXT NOT NULL,
    added_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE snapshots (
    id INTEGER PRIMARY KEY,
    volume_id INTEGER REFERENCES volumes(id),
    name TEXT NOT NULL,
    created_at TEXT DEFAULT (datetime('now')),
    retention TEXT CHECK (retention IN ('hourly','daily','weekly','yearly'))
);

-- Backup
CREATE TABLE s3_buckets (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    endpoint TEXT NOT NULL,
    access_key TEXT NOT NULL,
    secret_key TEXT NOT NULL,  -- encrypted at rest
    cryfs_password TEXT        -- encrypted at rest
);

-- VMs
CREATE TABLE vms (
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

-- Monitoring
CREATE TABLE monitors (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL
      CHECK (type IN ('http','tcp','ping','dns','keyword')),
    target TEXT NOT NULL,          -- URL, host:port, or hostname
    interval_sec INTEGER DEFAULT 60,
    timeout_ms INTEGER DEFAULT 5000,
    keyword TEXT,                  -- for keyword check type
    expected_status INTEGER,      -- for HTTP check (e.g. 200)
    enabled INTEGER DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE monitor_events (
    id INTEGER PRIMARY KEY,
    monitor_id INTEGER REFERENCES monitors(id) ON DELETE CASCADE,
    status TEXT NOT NULL CHECK (status IN ('up','down')),
    response_ms INTEGER,
    message TEXT,
    checked_at TEXT DEFAULT (datetime('now'))
);
CREATE INDEX idx_monitor_events_lookup
    ON monitor_events (monitor_id, checked_at DESC);

-- Notifications
CREATE TABLE notification_channels (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL CHECK (type IN ('email','webhook')),
    config TEXT NOT NULL           -- JSON as TEXT: {url:...} or {smtp_host:..., to:...}
);
CREATE TABLE monitor_notifications (
    monitor_id INTEGER REFERENCES monitors(id) ON DELETE CASCADE,
    channel_id INTEGER REFERENCES notification_channels(id) ON DELETE CASCADE,
    PRIMARY KEY (monitor_id, channel_id)
);

-- Applications (VM templates)
CREATE TABLE app_templates (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE NOT NULL,
    iso_url TEXT,
    default_cpus INTEGER,
    default_ram_mb INTEGER,
    default_disk_gb INTEGER,
    post_install_script TEXT
);
```

### 1.3 OpenResty Project Structure

```
/usr/local/flynas/
├── nginx.conf
├── lua/
│   ├── init.lua            -- Open SQLite DB, shared state
│   ├── router.lua          -- URL routing
│   ├── auth.lua            -- Session/token auth
│   ├── api/
│   │   ├── dashboard.lua
│   │   ├── network.lua
│   │   ├── accounts.lua
│   │   ├── storage.lua
│   │   ├── backup.lua
│   │   ├── vms.lua
│   │   └── monitors.lua
│   └── util/
│       ├── db.lua          -- SQLite helper (lsqlite3)
│       ├── exec.lua        -- Shell command runner
│       ├── json.lua        -- JSON encode/decode
│       └── notify.lua      -- Email/webhook notifications
├── scripts/
│   ├── snapshot-create.sh
│   ├── snapshot-prune.sh
│   ├── scrub.sh
│   ├── vm-start.sh
│   ├── vm-stop.sh
│   ├── vm-suspend.sh
│   ├── backup-sync.sh
│   ├── user-sync.sh
│   └── monitor-check.sh
├── static/                  -- Built WASM + HTML
│   ├── index.html
│   ├── clay.wasm
│   └── clay.js
└── cron/
    ├── hourly-snapshots.sh
    └── daily-scrub.sh
```

### 1.4 OpenResty Configuration

```nginx
worker_processes auto;

events { worker_connections 1024; }

http {
    lua_package_path '/usr/local/flynas/lua/?.lua;;';
    init_by_lua_file '/usr/local/flynas/lua/init.lua';
    init_worker_by_lua_file '/usr/local/flynas/lua/monitor_worker.lua';

    server {
        listen 443 ssl;
        ssl_certificate     /usr/local/flynas/ssl/cert.pem;
        ssl_certificate_key /usr/local/flynas/ssl/key.pem;

        # UI
        location / {
            root /usr/local/flynas/static;
            index index.html;
        }

        # API
        location /api/ {
            content_by_lua_file '/usr/local/flynas/lua/router.lua';
        }

    }

    # Redirect HTTP → HTTPS
    server {
        listen 80;
        return 301 https://$host$request_uri;
    }
}
```

---

## Phase 2: Backend API

All endpoints under `/api/`. JSON request/response. Session cookie auth (first user created becomes admin during initial setup).

### 2.1 Authentication — DONE (diverged: passwordless)

Implemented as passwordless two-step auth instead of username+password:

- `GET /api/setup/status` — public; tells UI whether to show setup wizard or sign-in
- `POST /api/setup` — step 1: create admin user record, generate TOTP secret, return secret + otpauth URI + one-time setup token
- `POST /api/setup/confirm` — step 2: verify TOTP code, create system account (random password via setuid helper), activate session
- `POST /api/login` — step 1: identify user, pick auth method (TOTP if enrolled, else verified-email code), return one-time login token
- `POST /api/login/verify` — step 2: verify TOTP/email code, set session cookie
- `GET /api/session` / `POST /api/logout`

Sessions in SQLite with expiry; pending (setup/login) sessions are
one-time use with 10 min TTL. System password hashed with argon2
(not bcrypt). Email codes sent via SMTP settings API.

### 2.2 Dashboard API

| Endpoint | Method | Description |
|---|---|---|
| `/api/dashboard/system` | GET | Hostname, OS version, uptime (`sysctl`) |
| `/api/dashboard/cpu` | GET | CPU usage (`sysctl kern.cp_time`) |
| `/api/dashboard/memory` | GET | Memory stats (`sysctl hw.physmem`, `vm.stats`) |
| `/api/dashboard/network` | GET | Interface info (`ifconfig`) |
| `/api/dashboard/disks` | GET | Disk sizes, SMART health (`smartctl`) |
| `/api/dashboard/volumes` | GET | HAMMER2 volume status, usage per PFS |

Implementation: Each Lua handler calls shell commands via `io.popen()` or `ngx.pipe`, parses output, returns JSON.

**Status: DONE** — all six endpoints implemented (`api/dashboard.lua`).

### 2.3 Network API — DONE (diverged: dntpd, not ntpd)

| Endpoint | Method | Description |
|---|---|---|
| `/api/network/config` | GET | Current IP, netmask, gateway, DHCP status, MAC (rc.conf + ifconfig + `route -n get default`) |
| `/api/network/config` | PUT | Set static IP or DHCP mode |
| `/api/network/timezone` | GET/PUT | Time zone (copies `/usr/share/zoneinfo/<zone>` to `/etc/localtime`, records name in `/var/db/zoneinfo` like tzsetup — not a symlink on DragonFly) |
| `/api/network/ntp` | GET/PUT | NTP server. **Plan change:** DragonFly base ships dntpd(8), not ntpd — no `/etc/ntp.conf`. Server lives in rc.conf `dntpd_flags`; empty server disables (`dntpd_enable="NO"` + onestop) |

Implementation: GETs read rc.conf/ifconfig unprivileged. PUTs go through
`flynas-helper` (`netconfig`, `timezone`, `ntp`): it validates IPs with
inet_pton, rejects timezone path traversal, rewrites rc.conf atomically
(deduplicating repeated keys), then runs `service netif restart`
(+ `routing restart` for static). The HTTP response can be lost if the
address changes — the UI arms the Apply button (two-click) and warns.

### 2.4 Accounts API

| Endpoint | Method | Description |
|---|---|---|
| `/api/users` | GET | List users |
| `/api/users` | POST | Create user (runs `pw useradd`, creates home dir) |
| `/api/users/:id` | PUT | Update user (shell, SSH, pubkey) |
| `/api/users/:id` | DELETE | Remove user (`pw userdel`) |
| `/api/users/:id/keypair` | POST | Generate ed25519 keypair, return encrypted private key |
| `/api/groups` | GET/POST | List/create groups |
| `/api/groups/:id` | PUT/DELETE | Update/delete groups |
| `/api/groups/:id/members` | PUT | Set group membership |

Key generation: `ssh-keygen -t ed25519` for keypair. Encrypt private key with AES-128 passphrase via `openssl enc -aes-128-cbc`. Return encrypted private key as downloadable file.

**Status: DONE** (`api/users.lua`, `api/groups.lua`) — users/groups CRUD, SSH keys, per-user TOTP setup/confirm/disable, email set/verify, keypair generation (`POST /api/users/:id/keypair`: ed25519 via ssh-keygen, private key returned AES-128-CBC/PBKDF2 encrypted, public key auto-added to authorized_keys). Also added (not in original plan): SMTP settings API (`GET/PUT /api/settings/smtp`, `POST /api/settings/smtp/test`) for email-code auth.

### 2.5 Storage API — DONE (diverged: fixed disk set per volume)

**Plan change:** DragonFly 6.4 has no `hammer2 volume-add` / `volume-del` —
those commands don't exist in the 6.4 userland (verified on h2dev). HAMMER2
multi-volume filesystems are instead created across a fixed set of disks at
`newfs` time and cannot be grown or shrunk dynamically. FlyNAS therefore asks
for all member disks when a volume is created. Expansion = create a new
volume, or future `hammer2 growfs` after partition resize.

| Endpoint | Method | Description |
|---|---|---|
| `/api/disks` | GET | All disks with size, SMART health, volume membership, `system` flag (mounted but not ours) |
| `/api/volumes` | GET | Volumes from DB + live df usage + mounted flag |
| `/api/volumes` | POST | `{name, disks:[...]}` — format all disks (`newfs_hammer2 -L <name> /dev/d1 /dev/d2 ...`), mount at `/data/<name>`, add fstab entry |
| `/api/volumes/:id` | GET | Volume details: disks, usage |
| `/api/volumes/:id` | DELETE | Unmount, remove fstab entry, forget (disks not wiped) |
| `/api/volumes/:id/scrub` | POST | `hammer2 bulkfree /data/<name>` (synchronous; background job TODO for big volumes) |
| `/api/volumes/:id/scrub/schedule` | PUT | Toggle `scrub_enabled` flag (cron wiring in Phase 4) |

**Verified volume lifecycle (h2dev):**

```sh
# Create a volume spanning two raw disks (no partitioning needed)
newfs_hammer2 -L tank /dev/vbd1 /dev/vbd2
mount_hammer2 /dev/vbd1:/dev/vbd2@tank /data/tank
# fstab: /dev/vbd1:/dev/vbd2@tank  /data/tank  hammer2  rw  0  0

hammer2 bulkfree /data/tank          # scrub
hammer2 -s /data/tank volume-list    # member devices
```

All privileged ops go through `flynas-helper` (`volcreate`, `voldestroy`,
`scrub`, `vollist`): it validates label/disk names, refuses disks that are
mounted or referenced in fstab (protects the system disk), and edits fstab
atomically. Mountpoints live under `/data/<name>`.

### 2.6 Backup API — DONE (diverged: live-source backup, no snapshot mounts)

| Endpoint | Method | Description |
|---|---|---|
| `/api/snapshots` | GET | List snapshots with retention labels |
| `/api/snapshots` | POST | Create manual snapshot |
| `/api/snapshots/:id` | DELETE | Delete snapshot |
| `/api/snapshots/schedule` | GET/PUT | View/set snapshot schedule |
| `/api/s3` | GET/POST | List/add S3 bucket configs |
| `/api/s3/:id` | PUT/DELETE | Edit/remove S3 bucket config |
| `/api/backup/sync` | POST | Trigger cryfs-batch backup + rclone sync to S3 |
| `/api/backup/status` | GET | Current sync progress |
| `/api/restore/browse` | GET | Browse encrypted backup contents (via cryfs-batch list) |
| `/api/restore/share` | POST | Create temporary Seafile share from snapshot |
| `/api/config/export` | GET | Export full FlyNAS config as JSON |
| `/api/config/import` | POST | Import FlyNAS config from JSON |

**Snapshot retention cron** (runs hourly via `cron/hourly-snapshots.sh`):
1. Create new hourly snapshot: `hammer2 snapshot <volume_mount>`
2. Delete hourly snapshots older than 24h
3. Keep one daily snapshot per day for 7 days
4. Keep one weekly snapshot per week for 52 weeks
5. Keep one yearly snapshot per year indefinitely

**Encrypted backup flow (via cryfs-batch — separate project):**
1. `cryfs-batch backup --source <snapshot_mount> --remote ~/.cryfs-batch/blocks/ --password-file <keyfile>`
2. `rclone sync ~/.cryfs-batch/blocks/ remote:mybucket/encrypted/`
3. See `cryfs-batch/SPEC.md` for full specification

**Implementation notes (2026-06-12):**
- Snapshots: helper `snapcreate`/`snapdelete`/`snaplist` (HAMMER2 PFS ops); names
  are forced to a `snap-` prefix (`snap-{m,h,d,w,y}-<stamp>`) so `snapdelete` can
  never address a volume root PFS. Manual snapshots have `retention = NULL`.
- **Backup reads the live mountpoint, not a snapshot mount**: mounting a snapshot
  PFS of a multi-volume HAMMER2 panics the 6.4 kernel
  (`hammer2_base_delete: element not found` in flush; reproduced on h2dev's
  RAID6-patched kernel, full reboot + UFS fsck needed). Transient backup
  snapshots stay disabled until that kernel bug is fixed. Snapshot
  create/delete/pfs-list themselves are safe and verified.
- Sync job: API writes `run/backup-job.json` as key=value lines (not JSON —
  cjson escapes slashes and randomizes key order, unparseable from sh), then
  `flynas-helper backupsync` detaches `scripts/backup-sync.sh` as root
  (setsid + stdio to /dev/null). Lock dir `run/backup.lock` serializes runs;
  progress lands in `run/backup-status.json` for `GET /api/backup/status`.
- One cryfs-batch repo per (bucket, volume) under
  `/usr/local/flynas/backup/{blocks,cfg}/<bucket_id>/<volume>`; "Backup now"
  backs up all mounted volumes, then rclone-syncs per volume. The block store
  is chowned `www:flynas` after each run so `GET /api/restore/browse` can run
  `cryfs-batch list` directly as www (no helper round-trip).
- Endpoints starting with `/` are local directories (rclone local backend) —
  used for offline end-to-end testing; real S3 uses an rclone `:s3,...:`
  connection string built from the bucket row.
- `s3_buckets.name` doubles as the remote bucket name; secrets are stored
  plaintext in the DB (664 www:flynas) — the "encrypted at rest" idea was
  dropped since the key would have to live next to the DB anyway. GET /api/s3
  redacts `secret_key` and `cryfs_password` (returns `has_password`).
- `/api/restore/share` deferred to the Seafile/VM phase (step 10/11);
  `/api/config/export|import` deferred to step 13 (§5.2).
- Verified end-to-end on h2dev: snapshot CRUD + schedule, hourly cron
  (h/d/w/y promotion, retention prune of a faked 30h-old snapshot, no dup
  daily/weekly/yearly on re-run), backup sync (14 blocks of exactly 32,808 B),
  incremental re-sync (+1 block), restore-browse, and a full
  `cryfs-batch restore` from the synced copy (`cmp` clean against source).

### 2.7 VM API — DONE (diverged: QMP from Lua; console + LAN bridge deferred)

**Implementation notes (2026-06-14):** QEMU 9.2 + NVMM (`/dev/nvmm`,
`kldload nvmm`), verified working nested on h2dev. Privilege split:
the setuid helper does the root-only bits (`vmcreate` = qemu-img raw
image, `vmstart` = load nvmm + create/up tap + daemonized QEMU,
`vmstop` = SIGTERM→SIGKILL + tap/socket teardown, `vmdelete`); QMP
control (status/suspend=`stop`/resume=`cont`/powerdown) runs from Lua
(`util/qmp.lua`) over the per-VM unix socket. nginx (www, in flynas)
reaches the socket because the helper makes the run dir setgid-flynas
and `chmod 0770`s the sockets after launch (connect() needs write).
Disk images live at `/data/<volume>/vms/<name>.img` (or the system
`/usr/local/flynas/vms/` when no volume given); each VM gets `tap<1000+id>`.
Live status comes from QMP, not the DB.

**VM networking — NAT model (2026-06-14, diverged from LAN-bridge):** the
dev VM uses QEMU slirp (NAT), which can't be L2-bridged, so VM networking
is a host-internal NAT'd bridge instead of bridging the physical NIC. The
helper `netbridge up <uplink>` creates `flynas0` (10.77.0.1/24 gateway),
enables `net.inet.ip.forwarding`, and loads a pf ruleset (`nat on <uplink>
from 10.77.0.0/24`, permissive `pass all` so it can't lock out management,
plus a `flynas-fwd` rdr-anchor for future port-forwards); `vmstart` adds
each VM's tap to `flynas0` if present. Guests then get NAT'd outbound and,
being on the host's subnet, are directly probeable by the monitor worker.
API `GET/PUT /api/network/vmnet {enabled}` + a VMs-page "NAT network"
toggle. This never touches the management interface (verified SSH-safe on
h2dev).

**Guest DHCP + port-forwards (2026-06-14):** dnsmasq serves DHCP/DNS on
`flynas0` (started/stopped with the bridge by the helper). Each VM gets a
deterministic reserved address `10.77.0.<100+id>` written to a
`dhcp-hostsfile` (rewritten by the API from the DB, reloaded via SIGHUP) —
so a guest that DHCPs lands on a known IP, and its auto-monitor targets it.
Inbound port-forwards are pf `rdr` rules in the `flynas-fwd` anchor:
`port_forwards` table, API `GET/POST /api/vms/:id/forwards` +
`DELETE /api/forwards/:id`, helper `pffwd <uplink>` rebuilds the anchor
from a www-written CSV spec (each field re-validated in C — www can't
inject raw pf). UI: VM rows show the IP and expand to manage forwards.
**Serial console (2026-06-15):** `GET /api/vms/:id/console` upgrades to a
WebSocket (`resty.websocket.server`) and proxies bytes both ways to the
VM's serial unix socket via two `ngx.thread` cosocket pumps (idle
timeouts loop so it stays open). UI: a "Console" button on running VMs
opens a JS-managed full-screen terminal overlay (outside Clay) — `<pre>`
output + keydown→WebSocket (Enter→CR, Backspace→DEL, arrows→ANSI). Real
guest output needs `console=ttyS0` in the guest (BIOS POST isn't on
serial with our flags); the WebSocket upgrade + the stopped-VM
"unavailable" path are test-covered.

**Still deferred:** automated guest OS install (ISO + unattended +
post-install script), and true LAN-identity bridging of the physical NIC
(real-hardware only — guests reach the LAN today via the host's
port-forwards).

| Endpoint | Method | Description |
|---|---|---|
| `/api/vms` | GET | List all VMs with status |
| `/api/vms` | POST | Create VM (allocate disk image, generate MAC if DHCP) |
| `/api/vms/:id` | GET | VM details |
| `/api/vms/:id` | DELETE | Remove VM and its disk image |
| `/api/vms/:id/start` | POST | Start VM via QEMU/NVMM |
| `/api/vms/:id/stop` | POST | Send ACPI shutdown, then force kill after timeout |
| `/api/vms/:id/suspend` | POST | QEMU monitor `stop` command |
| `/api/vms/:id/console` | GET | WebSocket to QEMU serial console |

**VM start command template:**
```sh
qemu-system-x86_64 \
  -machine type=q35,accel=nvmm \
  -smp cpus=$CPUS \
  -m ${RAM}M \
  -drive file=$DISK_IMG,format=raw,if=virtio \
  -cdrom $ISO \
  -netdev tap,id=net0,ifname=tap$VMID,script=no,downscript=no \
  -device virtio-net-pci,netdev=net0,mac=$MAC \
  -monitor unix:/var/run/flynas/vm-$VMID.sock,server,nowait \
  -daemonize -pidfile /var/run/flynas/vm-$VMID.pid
```

Network: Create `tap` interfaces bridged to physical NIC. Static IP configured inside guest via post-install script or cloud-init.

### 2.8 Monitoring API — DONE (diverged: raw cosockets, no lua-resty-http)

Built-in service monitoring, replacing Uptime Kuma. Runs entirely within OpenResty using `ngx.timer` for background checks and `lua-resty-http` / `resty.socket` for probes.

**Plan changes / implementation notes (2026-06-14):**
- **No `lua-resty-http` on the box** — HTTP/keyword checks issue a minimal
  `GET` over a raw `ngx.socket.tcp` cosocket (parse status line, optional
  body keyword search), `util/probe.lua`. TCP checks just connect.
- **ping/dns shell out via `resty.shell`** (`ngx.pipe`, non-blocking in the
  timer) using argv arrays (`{"ping","-c","1","-W",<ms>,host}` /
  `{"drill",host}`) — injection-safe; targets are also char-validated.
- **Single timer in worker 0 only** (`ngx.worker.id() == 0` guard in
  `monitor_worker.lua`); 10s tick, due-monitor scan via SQLite
  `strftime` age compare, probes run concurrently with `ngx.thread`,
  events written sequentially (FFI queries don't yield), notifications
  fire on status change (prev ≠ new, so first check also notifies),
  events pruned > 90 days hourly.
- Notifications (`util/notify.lua`): webhook = JSON POST over cosocket,
  email = reuse `util/smtp.lua` + global SMTP config. Verified live on
  h2dev: all 5 check types (up + down paths), concurrent probing,
  uptime %, pause/resume, channel assignment, and a real webhook POST
  delivered to a local listener.

| Endpoint | Method | Description |
|---|---|---|
| `/api/monitors` | GET | List all monitors with current status |
| `/api/monitors` | POST | Create monitor (type, target, interval, etc.) |
| `/api/monitors/:id` | GET | Monitor details + recent event history |
| `/api/monitors/:id` | PUT | Update monitor config |
| `/api/monitors/:id` | DELETE | Delete monitor and its event history |
| `/api/monitors/:id/pause` | POST | Disable monitor |
| `/api/monitors/:id/resume` | POST | Re-enable monitor |
| `/api/monitors/:id/history` | GET | Paginated event log (filterable by date range) |
| `/api/monitors/summary` | GET | Aggregate: total up/down/paused counts, overall uptime % |
| `/api/notifications` | GET/POST | List/create notification channels |
| `/api/notifications/:id` | PUT/DELETE | Update/delete notification channel |
| `/api/monitors/:id/notifications` | PUT | Assign notification channels to a monitor |

**Check types:**

| Type | How it works |
|---|---|
| `http` | `lua-resty-http` request to URL. Check status code matches `expected_status` (default 200). Optionally search response body for `keyword`. |
| `tcp` | `resty.socket` connect to `host:port`. Up if connection succeeds within timeout. |
| `ping` | Shell out to `ping -c 1 -W <timeout>`. Up if exit code 0. |
| `dns` | Shell out to `drill <hostname>` (or `host`). Up if resolution succeeds. |
| `keyword` | Same as `http` but status is determined by presence/absence of `keyword` in response body. |

**Background check loop (`lua/monitor_worker.lua`):**

```lua
-- Runs via ngx.timer.every in init_worker_by_lua
-- On each tick:
--   1. Query monitors table for enabled monitors whose
--      last check was >= interval_sec ago
--   2. For each due monitor, run the appropriate check
--   3. INSERT result into monitor_events
--   4. If status changed (up→down or down→up):
--      a. Look up notification channels for this monitor
--      b. Fire notifications via notify.lua (webhook POST
--         or email via SMTP)
--   5. Prune monitor_events older than 90 days
```

Timer interval: 10 seconds (checks all monitors whose interval has elapsed). Each check runs non-blocking using OpenResty's cosocket API. Multiple checks run concurrently via `ngx.thread.spawn`.

**Event retention:** Keep 90 days of history by default (configurable in `config` table). Pruning runs once per hour within the worker timer.

**Uptime calculation:** `GET /api/monitors/:id` returns `uptime_24h`, `uptime_7d`, `uptime_30d` computed as percentage of `up` events over total events in each window.

### 2.9 Applications API — DONE (catalog + install orchestration; guest provisioning deferred)

| Endpoint | Method | Description |
|---|---|---|
| `/api/apps` | GET | List available app templates |
| `/api/apps/:id/install` | POST | Create VM from template with chosen resources |

Pre-built templates for: Seafile, CryptPad, Forgejo, VaultWarden, Readeck, Jellyfin, RoundCube/smtp2go, DokuWiki, Wekan. Each template includes: ISO URL, default resource allocation, post-install script that configures the service, and a default monitor definition (type, target path, expected status).

**Implementation notes (2026-06-14):** Catalog seeded idempotently in
`init.lua` (9 apps; `app_templates` gained `description` + `monitor_type/
port/path/expected` columns). `POST /api/apps/:id/install {name, cpus?,
ram_mb?, disk_gb?, volume?, ip_address?}` creates a VM from the template
(defaults filled from the row), tags `vms.app_template`, and — when an IP
is given — auto-creates the template's monitor (`http://<ip>:<port><path>`)
linked via `vms.monitor_id`; deleting the VM cascades that monitor (§4.1).
Apps page in Clay UI (catalog list + shared name/IP install form),
`tools/uitest/test-apps.mjs` (catalog renders, one-click Seafile install,
VM appears on VMs page, cleanup). **Deferred (the genuinely hard part):**
fetching/staging guest ISOs, unattended OS install, and running the
`post_install_script` inside the guest — needs the VM LAN bridge first
(installed apps aren't reachable until then; see §2.7). **Fixed a
foundational bug:** `util/db.lua` bound params via `ipairs`/`#`, which stop
at the first `nil` — an optional column left NULL silently truncated every
later bind (mac/app_template were being dropped). Now uses `select('#',...)`
(LuaJIT here has no `table.pack`).

---

### 2.10 Identity / SSO API — IN PROGRESS (OIDC provider over the SQLite directory)

**Decision (2026-06-24):** Installable VM apps need a shared user
directory. We do **not** stand up OpenLDAP or Kerberos. The existing
`users`/`groups`/`user_groups` tables in SQLite stay the single source of
truth; FlyNAS exposes them to apps as an **OpenID Connect provider**
running inside the existing OpenResty (host-side), not a separate IdP VM.

**Why OIDC, not LDAP/Kerberos:**
- Our auth is **passwordless** (session + TOTP + SSH keys) — there is no
  password in the DB. LDAP simple-bind *needs* a password, so LDAP would
  force re-introducing the exact credential we deleted. OIDC lets FlyNAS
  run the login ceremony its own way (TOTP) and hand the app a signed
  token — a perfect fit for passwordless.
- Kerberos wants nobody in the catalog (web apps don't use ticket SSO) and
  brings realm/keytab/time-sync fragility. Killed outright.
- The priority apps (Seafile, Forgejo, Jellyfin, VaultWarden, CryptPad)
  all speak OIDC; VaultWarden + CryptPad are OIDC-*only*. One protocol
  covers all five.

**Source of truth stays SQLite — extend, don't refactor.** New tables:

```sql
CREATE TABLE oidc_clients (        -- one per installed app
  id INTEGER PRIMARY KEY,
  vm_id INTEGER REFERENCES vms(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  client_id TEXT UNIQUE NOT NULL,
  client_secret_hash TEXT NOT NULL,   -- argon2, reuse util/argon2
  redirect_uris TEXT NOT NULL,        -- newline-separated allowlist
  created_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE app_grants (          -- who may access which app + role
  user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
  client_id INTEGER REFERENCES oidc_clients(id) ON DELETE CASCADE,
  role TEXT DEFAULT 'user',           -- user|admin → groups claim
  PRIMARY KEY (user_id, client_id)
);
CREATE TABLE oidc_codes (          -- short-lived auth codes + PKCE
  code TEXT PRIMARY KEY,
  client_id TEXT NOT NULL,
  user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
  redirect_uri TEXT NOT NULL,
  nonce TEXT, scope TEXT,
  code_challenge TEXT, code_challenge_method TEXT,
  expires_at INTEGER NOT NULL         -- unix seconds; 60s TTL
);
```

**Endpoints (all under `/api/oidc/*`; issuer = `https://<host>/api/oidc`):**

| Endpoint | Auth | Description |
|---|---|---|
| `GET /api/oidc/.well-known/openid-configuration` | public | Discovery document |
| `GET /api/oidc/jwks` | public | Public signing key (RS256, as JWK) |
| `GET /api/oidc/authorize` | **our session** | Authorization-code start; reuses the `flynas_session` cookie — no new login UX. No session → 302 to the SPA login with a `return` param |
| `POST /api/oidc/token` | client_secret + PKCE | code → `id_token` + `access_token` (form-encoded, not JSON) |
| `GET /api/oidc/userinfo` | Bearer access_token | `sub`/`email`/`name`/`groups` |
| `GET/POST/DELETE /api/oidc/clients[/:id]` | admin session | Client registration (manual + install-time) |

**Signing:** RS256. Keypair generated once at first use
(`/usr/local/flynas/oidc_key.pem`, `0600`), published via JWKS. JWT
sign/verify shells out to base `openssl` (no privilege needed, so it runs
directly via `io.popen` like `util/exec.run_shell`, *not* through the
setuid helper). PKCE S256 supported (`resty.sha256`).

**Claims:** `groups` is emitted from `user_groups` plus the per-app
`app_grants.role`, so apps map admin vs. user from a single directory.

**Install integration (the payoff, deferred with guest provisioning):**
catalog install will auto-mint an `oidc_client` (client_id/secret +
redirect URI) and the per-app `post_install_script` will write the app's
OIDC config pointing at the issuer — so installing Seafile yields working
SSO with no manual setup. This rides on §2.9 guest provisioning, still
deferred behind the VM LAN bridge.

**Per-app caveats to document, not oversell:**
- VaultWarden — OIDC is *login only*; the vault stays end-to-end
  encrypted behind the user's master password.
- Jellyfin — needs the SSO plugin baked into the guest image.
- RoundCube — authenticates against IMAP, not the directory; out of scope.

**Status — VERIFIED on h2dev (2026-06-24):** schema + migrations,
`util/jwt.lua` (b64url + RS256 sign/verify + SHA-256, all over `openssl`),
`util/oidc_keys.lua` (lazy keygen + JWK export), `api/oidc.lua` (discovery,
jwks, authorize, token, userinfo, client CRUD), and router wiring landed.
Full authorization-code + PKCE flow exercised end-to-end: client mint →
authorize (session-cookie reuse) → 302 with code → token (RS256 id_token +
access_token) → userinfo. id_token claims correct (`sub`/`name`/
`preferred_username`/`nonce`/`groups:["admin"]`); `email` correctly omitted
when the user has none. Negatives all reject: code replay → `invalid_grant`,
wrong PKCE verifier → `invalid_grant/pkce`, no/garbage bearer → `invalid_token`.
**SPA `return`-param handoff — DONE (2026-06-28):** `enterMain()` (the single
post-login chokepoint, reached by both already-have-session and fresh-login
paths) now calls `consumeOidcReturn()`, which reads `?return=`, rejects any
non-local / protocol-relative / non-`/api/oidc/authorize` value (open-redirect
guard), and `window.location.replace()`s back to finish the flow. Backend 302
shape (`/?return=%2Fapi%2Foidc%2Fauthorize%3F…`) confirmed against the guard;
guard accept/reject cases unit-tested.
**Gotcha:** this OpenResty has no `resty.sha256` (same gap as `resty.http`,
§2.8) — PKCE S256 hashing moved to `openssl dgst` in `util/jwt.sha256_b64url`.
The http-context error log is `/var/log/flynas/error.log`, *not*
`logs/error.log` (that's only the master log). **`app_grants` write path + enforcement — DONE (2026-06-29):** policy is
**grants gate access** — a non-admin with no grant for a client is denied at
`/authorize` (spec-style `redirect_uri?error=access_denied&state=`); admins
always pass (and already carry the `admin` group via `user_groups`). Admin
CRUD added: `GET /api/oidc/clients/:id/grants` (list user+role) and
`PUT …/grants` (replace the full set, `{grants:[{user_id,role}]}`, role ∈
{user,admin}; mirrors `groups.set_members`). Grant role still feeds the
`groups` claim. Verified on h2dev via forged-session API test: ungranted
non-admin → `access_denied`; after PUT → `code=` issued; CRUD round-trips;
bad role → 400; non-admin on the admin endpoint → 403; admin SSO unaffected
(browser e2e still green). **Grants admin UI — DONE (2026-06-29):** an "App
access (SSO)" card on the Apps page lists registered `oidc_clients`; selecting
one opens a user×grant matrix (checkbox to grant/revoke, per-grant user/admin
role toggle), wired to `GET/PUT …/clients/:id/grants`. New Clay exports
(ClearClients/AddClient/SetSelectedClient/ClearGrants/AddGrant/SetSsoMsg) +
JS glue; string pool grown 144K→160K for the new SSO pool region. Verified
on h2dev with `tools/uitest/test-sso-ui.mjs` (grant→DB role=user, toggle→admin,
revoke→empty, all checked against the DB).

**Auto-client-mint at install — SCAFFOLDED (2026-06-29; guest delivery still
deferred).** `app_templates` gained `oidc_redirect_path` (NULL ⇒ app isn't
SSO; seeded for Seafile `/oauth/callback` and Forgejo
`/user/oauth2/flynas/callback`, more as each app's recipe is verified). New
`app_provisioning` table stashes, per VM, the minted `oidc_client_id`, the
**once-only plaintext** `oidc_secret`, the rendered `redirect_uri`/`issuer`,
and a `delivered` flag. `oidc.create_client`'s mint core was extracted to a
shared `mint_client`; `oidc.provision_for_vm(conn, vm, tpl)` mints a
VM-scoped client (`<vm>-sso`, redirect = `http://<guest-ip>:<port><path>`)
and writes the stash. `apps.install` calls it best-effort (logs and continues
on failure; the VM already exists). Verified on h2dev: installing Seafile
mints the client + stash row (secret held, `delivered=0`) and returns an
`sso` block; installing DokuWiki (no redirect path) mints nothing; VM delete
sweeps both. **Still deferred — the actual guest delivery:**
`apps.deliver_provisioning(vm_id)` is an inert documented stub; it needs the
§2.9 VM LAN bridge + in-guest exec to render the app's OIDC config from the
stash, push it into the guest, then set `delivered=1` and NULL the secret.

---

### 2.11 Guest Provisioning — PLANNED (make app VMs functional: OS + app + SSO)

**Problem.** Everything up to §2.10 stops at an *empty* VM: `apps.install`
creates the disk, mints the OIDC client, and stashes provisioning, but the
disk boots to nothing — no guest OS, no app, no config push, and
`deliver_provisioning` is inert. This section is the plan to close that gap.

**Decisions (2026-06-30):**
- **Provisioning mechanism: cloud-init + Docker.** One generic Linux cloud
  image + a per-VM NoCloud seed ISO (label `cidata`) carrying `user-data`.
  cloud-init installs Docker, runs the app's official container, **and writes
  the app's OIDC config from the stash at first boot.** One uniform mechanism
  across all apps. This collapses guest SSO delivery into seed-build — no
  in-guest exec channel needed, so `deliver_provisioning` becomes "render +
  build the seed," not "SSH into the guest."
- **Base image: Alpine (nocloud), not Debian.** ~50MB vs ~350MB → smaller
  backing file, faster stage, less COW churn, smaller attack surface. musl is
  a non-issue because the apps run in containers that bring their own libc;
  Alpine-as-Docker-host is well-trodden (`apk add docker`). *Caveat:* Alpine's
  cloud-init/NoCloud is younger than Debian's (Alpine historically shipped
  `tiny-cloud`); Step 0 de-risks this, fallbacks are Alpine `tiny-cloud` or a
  minimal custom seed-consuming init, or Debian 12 if it fights.
- **VMM: stay QEMU + NVMM.** Firecracker was considered and rejected — it
  talks to `/dev/kvm` directly (no KVM on DragonFly; only NVMM), has no NVMM
  backend, and its whole value (125ms boot / seccomp jailer) is a multi-month
  KVM→NVMM Rust rewrite that barely applies to long-lived services. If VM
  *density/lightness* ever matters, spike QEMU's native `microvm` machine type
  (virtio-mmio, direct `-kernel` boot) on NVMM instead — same VMM we run.
- **First app (vertical slice): Forgejo** — single container, clean OIDC,
  redirect path already seeded.

**Step 0 — de-risk spike (h2dev).** Stage the Alpine nocloud qcow2 under
QEMU+NVMM, hand-build a `cidata` seed ISO that writes a file + starts a
container, boot it, confirm cloud-init consumes the seed. Also validates the
qcow2-backing (Step 2) + seed-ISO (Step 3) mechanics on the real target before
any app work. If cloud-init fights on Alpine, fall back per the caveat above.

**Ordered plan:**

1. **Stage base cloud image (host). DONE (h2dev 2026-06-30/07-01).** Alpine
   `generic` qcow2 cached at `/usr/local/flynas/images/alpine.qcow2` (system
   dir). **Must be patched with `no_timer_check`** (see step 7): the stock image
   flakily kernel-panics at boot under NVMM ("IO-APIC + timer doesn't work").
   Patch procedure (needs a Linux host — DragonFly can't write ext4): the image
   is a partitionless whole-disk ext4 with extlinux; use `debugfs -w` (rootless)
   to rewrite `/boot/extlinux.conf`'s APPEND line, adding `no_timer_check` and
   moving `console=ttyS0,115200n8` last (also fixes the step-0 `/dev/console`=
   framebuffer issue). Installer TODO: fetch + apply this once. [[nvmm-guest-ioapic-panic]]
2. **qcow2 backing in the helper (`flynas-helper.c`). DONE (verified h2dev
   2026-06-30).** `do_vmcreate` gained an optional 4th arg `[base|-]`: base ⇒ a
   COW qcow2 overlay (`create -f qcow2 -b <VM_IMAGES_DIR>/<base> -F qcow2 <disk>
   <gb>g`, 208KiB overlay), `-` ⇒ raw as before. `do_vmstart` picks the drive
   format by **reading the qcow2 magic** (`is_qcow2()`), so the disk stays named
   `<name>.img` and Lua `image_path`/delete are untouched. New `valid_basename`
   (basename-only, no `..`/`/`) + existence check reject traversal. `exec.vm_create`
   passes the base through. Verified: overlay+backing correct, raw regression
   intact, `../../etc/passwd` and missing-base rejected.
3. **Seed ISO generation (`vmseed` helper verb). DONE (verified h2dev
   2026-06-30).** `flynas-helper vmseed <name> <hostname>` reads the full
   cloud-config **user-data from stdin** (via `run_with_stdin`, like
   `passwd`/`sshkeys` — keeps the once-only OIDC secret out of argv and off
   www-writable paths), synthesizes `meta-data` (`instance-id: flynas-<name>` +
   `local-hostname`), and `mkisofs -volid cidata` → `/usr/local/flynas/seeds/
   <name>.iso` (root 0600). `do_vmstart` **auto-attaches** that ISO as a 2nd
   `if=virtio` drive when it exists — no arg threading, plain VMs unaffected.
   `vmdelete` sweeps the seed + staging dir. `exec.vm_seed(name,hostname,
   user_data)` wraps it. Verified end-to-end: overlay → vmseed → vmstart
   auto-attach → cloud-init consumed the seed (`hostname=appvm` + runcmd marker
   on serial); empty-stdin rejected; delete sweeps the seed.
4. **Per-app recipe → cloud-init template (Forgejo first). DONE (renderer
   verified h2dev 2026-06-30; live guest boot deferred to step 7).**
   `app_templates` gained a `cloud_init` column, loaded from per-app recipe
   files `lua/recipes/<app>.yaml` (idempotent UPDATE at init, so redeploys
   propagate fixes; scales to the other 8 apps in step 8). `recipes/forgejo.yaml`
   installs Docker (+ the `cgroups`-before-`docker` fix from step 0), runs
   `codeberg.org/forgejo/forgejo` (single container, SQLite), and registers the
   FlyNAS OIDC login source via `forgejo admin auth add-oauth`; all output to
   `/dev/ttyS0` (step-0 console gotcha). `util/cloudinit.render` fills `{{KEY}}`
   placeholders and **errors on any unresolved one** (no half-rendered secret
   reaches a guest); `apps.render_user_data(conn, vm, tpl)` assembles the vars
   from the VM + OIDC stash (client_id from `oidc_clients`, once-only secret +
   redirect/issuer from `app_provisioning`). Verified: render consistency test
   (luajit) — all 6 recipe placeholders map, guard fires, extra vars ignored;
   recipe loads into the live DB (2067 B), `apps.lua` require-chain resolves on
   restart. The **seed-build wiring** (render → `vm_seed` → `delivered=1`, NULL
   secret) lands in step 6 (first Start), the **live Forgejo boot + OIDC CLI
   tuning** in step 7.
5. **Issuer reachability (the TLS wrinkle). DONE (verified h2dev 2026-06-30).**
   Split-horizon, since an installed app is reached two ways: the user's
   **browser** (LAN) and the app's own **server-to-server** OIDC calls (the
   10.77.0.1 bridge). Solution:
   - nginx serves `/api/oidc/` over **plain http on :80** (no 301) so a guest
     reaches discovery/token/jwks/userinfo at `http://10.77.0.1/api/oidc/*`
     (no cert to trust); everything else on :80 still redirects to https.
   - `issuer()` is now **request-relative** (scheme+Host): a guest fetching over
     the bridge sees `http://10.77.0.1/api/oidc` for issuer *and* the `iss`
     claim `/token` emits — self-consistent by construction; a browser on 443
     still sees the https issuer.
   - `discovery()` splits only `authorization_endpoint` out to a browser-facing
     base: a new `config.external_url` (e.g. `https://nas.example.com`) →
     `<external_url>/api/oidc/authorize`; unset ⇒ request-relative fallback (dev).
   - install stashes `BRIDGE_ISSUER` (`http://10.77.0.1/api/oidc`) — the value
     the guest can reach — not the admin's https request host.
   Verified: guest-sim (Host 10.77.0.1 on :80) discovery/jwks all bridge-http +
   200 (no redirect); non-oidc :80 still 301s; 443 gives https issuer;
   `external_url` set ⇒ authorize→external, token/issuer stay bridge.
   **Known gap for step 6:** the stashed `redirect_uri` (and Forgejo `ROOT_URL`)
   still use the guest IP:port; for real browser SSO they must be the
   external_url + the app's **port-forward** — wire that with step 6.
6. **Wire install orchestration. DONE (verified h2dev 2026-06-30).**
   `apps.install` now: creates the disk as a **COW qcow2 overlay** of the cached
   Alpine base (`BASE_IMAGE`); auto-creates a **port-forward** (`host_port =
   20000+vm.id → guest app port`) so the app is LAN-reachable; and mints/stashes
   the OIDC client with a **browser-facing** `redirect_uri` (`http://<external
   host>:<host_port><path>`, external host from `config.external_url` or the
   admin's request Host). It does **not** boot the VM. On the **first Start**
   (`vms.start`, gated by a new `vms.provisioned` flag): `render_user_data`
   (recipe + stash, `ROOT_URL` derived from the registered redirect so they
   always match) → `exec.vm_seed` (auto-attached by vmstart) → set
   `provisioned=1`, `delivered=1`, NULL the secret. Renders fail loudly rather
   than boot an unprovisioned app. `deliver_provisioning` superseded (no-op).
   **Gotcha fixed:** cdrtools `mkisofs` drops to the real uid when it detects
   it's setuid, so it couldn't read the root-only seed dir under the helper —
   added `run_root()` (forces child ruid=0 via `setreuid`) for the mkisofs exec.
   Verified via forged-session API: install → qcow2 overlay + forward 20001→3000
   + browser-facing redirect; first Start → seed built/attached, flags flipped,
   secret nulled, rendered `ROOT_URL`/`--key`/`--auto-discover-url` all correct;
   delete sweeps everything.
7. **Verify end-to-end (Forgejo). DONE — full browser SSO round-trip green
   (h2dev 2026-07-01).** The complete chain works in headless Chrome: Forgejo
   "Sign in with flynas" → FlyNAS `/authorize` (session reuse) → code → Forgejo
   token exchange at `http://10.77.0.1/api/oidc/token` over the bridge →
   id_token validated → **signed into Forgejo as the FlyNAS user**
   (`tools/uitest/test-app-sso.mjs` + `run-app-sso.sh`, two ssh -L tunnels: 443
   for authorize, →guest:3000 for Forgejo). **Real bug this caught:** the token
   endpoint only accepted `client_secret_post`, but Forgejo (and most clients)
   use `client_secret_basic` — every exchange was failing `invalid_grant` (nil
   client_id); fixed to parse the Basic header + advertise both. Getting here: Brought up flynas0, installed + started
   Forgejo, watched provisioning over serial. **Hit + fixed the hard blocker:**
   the stock Alpine image flakily kernel-panics under NVMM ("IO-APIC + timer
   doesn't work") — reliably at ≥2048MB, independent of smp/machine/acpi. Fix =
   `no_timer_check` in the base image's extlinux cmdline (patched via `debugfs`
   on a Linux host; 3/3 clean boots at 2048MB after). With that, the **whole
   pipeline runs end to end**: overlay boots → cloud-init → `apk add docker` (+
   cgroups fix) → Forgejo container pulled over NAT + started → internal
   healthcheck passes → OIDC source registration attempted. **Two last-mile
   recipe issues found + fixed (confirming re-run pending):** (a) Docker `-p`
   publishing was unreachable externally — minimal Alpine lacks `iptables`
   (added); (b) `add-oauth` hit SQLite "database is locked" vs the running
   server — added `FORGEJO__database__SQLITE_TIMEOUT` + an idempotent retry.
   **Both fixes CONFIRMED (re-run 2026-07-01):** Forgejo home `200` +
   `<title>Forgejo…</title>` (iptables fix ✓, external reachability just needed
   Forgejo's ~2min init), and `OIDC registration: ok` after the retry rode
   through one transient "database is locked" (SQLITE_TIMEOUT+retry ✓). The
   **"Sign in with flynas"** button is live on Forgejo's `/user/login`
   (`user/oauth2/flynas`) — and because `add-oauth --auto-discover-url` had to
   **fetch discovery from `http://10.77.0.1/api/oidc` from inside the guest**,
   this proves Step 5's bridge issuer works with a real guest, not just a
   simulated curl. **Then the interactive browser round-trip closed it out**
   (see the DONE note above — the `client_secret_basic` fix was the last bug).
   **Follow-ups noted (not blockers):**
   - **dnsmasq reservation race:** the guest got a dynamic IP despite a correct
     reservation (SIGHUP/timing); worked around by retargeting the port-forward.
     Fix so app-VM IPs are deterministic.
   - **SameSite=Strict session cookie:** fine here (FlyNAS + app both `localhost`
     = same site). In production with different hostnames the Strict cookie won't
     ride the cross-site nav into `/authorize` → re-login each SSO; consider
     `SameSite=Lax` for the session cookie.
   - Forgejo reserves the username "admin" (test-data collision only; the
     round-trip completed with a chosen username).
8. **Generalize.** Once Forgejo is green, port the recipe pattern to the other 8
   (a compose/user-data body per template): Seafile, Jellyfin, VaultWarden, etc.

**Open decisions — all resolved (2026-06-30):**
- ~~Auto-start on install vs. provision-on-first-Start~~ → **first-Start** (Step 6).
- ~~Issuer HTTP-on-bridge vs. CA-trust~~ → **plain HTTP on the bridge** (Step 5).
- ~~Confirm `mkisofs`~~ → present (`/usr/local/bin/mkisofs`, cdrtools); Step 0
  spike used it to build the `cidata` seed.

---

## Phase 3: Frontend (Clay UI)

### 3.1 Build Setup — DONE

- Write UI in C using `clay.h` (single header, ~4KB)
- Compile to WASM with `clang --target=wasm32` (`ui/build.sh`, cross-compiled on Linux)
- Use Clay's HTML renderer for browser output
- JS glue code: fetch API data, format strings, write into wasm string pool
- No JS framework dependencies (one vendored MIT lib: `ui/qrcode.js` for TOTP QR)

### 3.2 UI Pages

| Page | Route | Content |
|---|---|---|
| Dashboard | `/` | System info cards, CPU/memory gauges, disk/volume status, monitor summary |
| Monitoring | `/monitoring` | Monitor list with status badges, uptime bars, response time graphs, add/edit/delete monitors, notification channel config |
| Network | `/network` | Timezone picker, NTP config, IP configuration form |
| Accounts | `/accounts` | User table with add/edit/delete, group management |
| Storage | `/storage` | Volume overview, disk list, add/remove disk from volume, scrub controls |
| Backup | `/backup` | Snapshot list, S3 config, sync controls, restore browser |
| VMs | `/vms` | VM list with status, create/start/stop/delete controls |
| Apps | `/apps` | App catalog grid, one-click install |
| Settings | `/settings` | Config import/export, SSL cert upload |

### 3.3 UI Implementation Approach

- Clay handles layout computation in WASM
- HTML renderer outputs DOM elements positioned by Clay
- JS fetches `/api/*` endpoints, passes data into WASM memory
- WASM returns layout render commands, JS applies to DOM
- Polling every 5s for dashboard metrics (or WebSocket for live updates later)
- Style reference: TrueNAS SCALE — dark sidebar nav, card-based dashboard, data tables

**Status:** Dashboard page DONE (system/CPU/memory/volumes/disks cards, 5s polling). Login/setup flow DONE — JS owns screen transitions and API calls, C renders screens; setup wizard shows scannable TOTP QR code plus manual secret fallback; expired one-time tokens restart the flow. Accounts page DONE — users table (SSH toggle, keygen download, two-click delete), groups card with inline membership checkboxes; C queues packed page actions (low 4 bits action, rest row id) drained by JS each frame via `TakePageAction()`; accounts strings live in their own pool region (32768..49151). Storage page DONE — volume cards (usage gauge, scrub now, auto-scrub toggle, two-click delete), disk list with free-disk checkboxes + create form; storage strings at 49152..65535. Network page DONE — IP config card (live address, DHCP/static mode toggle, two-click Apply since netif restart can drop the session) + Time card (timezone, NTP server with empty-to-disable); network strings at 65536..81919; page-action packing widened from 4 to 6 bits for the new action codes. Backup page DONE — snapshots card (per-volume snapshot-now, retention tags, two-click delete, auto-snapshot toggle), S3 card (bucket rows with backup-now/browse/delete, status line polled every 2s while a sync runs, add-bucket form with masked secret inputs), restore browser card (pseudo-root lists volumes as directories, `../` navigation); backup strings at 81920..98303. Monitoring page DONE — summary card (up/down/paused + 24h uptime), monitors card (status dot, type·target, uptime%/last-response, Pause/Resume, two-click delete, expandable per-monitor notification-channel checkboxes, add form with type-cycle button + interval), notification-channels card (add with type-cycle email/webhook + target, two-click delete); page live-refreshes every 5s; monitoring strings at 98304..114687 (STRING_POOL_SIZE now 114688); page actions 27..35. VMs page DONE — VM list (status dot, spec, lifecycle buttons that switch by state: Start/Delete when stopped, Suspend/Stop when running, Resume/Stop when suspended) + create form (name/vCPU/RAM/disk) + a "NAT network" toggle (flynas0); VM rows show the bridge IP and expand to add/remove port-forwards; running VMs have a "Console" button opening a JS serial-terminal overlay (outside Clay) over a WebSocket; live-refreshes every 5s (paused for 3s after any interaction, and fully pausable via `window.__flynasPauseRefresh` for deterministic UI tests); VM strings at 114688..131071 (STRING_POOL_SIZE now 131072); page actions 36..41. Apps page DONE — install catalog (one shared "install as" name + optional static-IP form, per-app rows with description/defaults + Install button); app strings at 131072..147455 (STRING_POOL_SIZE now 147456); page action 42. The VMs/Monitoring/Apps live-refresh pauses while a form field is focused or a form has unsaved content, so the 5s rebuild can't drop keystrokes. Remaining page (Settings) is a placeholder.

---

## Phase 4: System Integration

### 4.1 Monitoring Integration

- Monitor worker starts automatically via `init_worker_by_lua` — no separate process or runtime
- When a VM or app is created, a default monitor is auto-created based on the app template's monitor definition (e.g., HTTP check on Seafile's `/api2/ping/`, TCP check on Forgejo's SSH port)
- When a VM is deleted, its associated monitor is also deleted
- Dashboard page shows a compact monitor summary card: count of services up/down, overall uptime percentage
- Monitoring page (Uptime Kuma style) shows:
  - List of monitors with colored status badge (green=up, red=down, grey=paused)
  - Per-monitor uptime bar (90-day history, one cell per day, color-coded)
  - Response time sparkline graph (last 24h)
  - Click to expand: full event log, edit config, manage notifications

### 4.2 Cron Jobs

```
# /etc/crontab additions
0 * * * *  flynas  /usr/local/flynas/cron/hourly-snapshots.sh
0 3 * * *  flynas  /usr/local/flynas/cron/daily-scrub.sh
```

### 4.3 Seafile Group Sync

- When a group is created/modified in FlyNAS, call Seafile API to sync as a tag/group
- Runs via `lua-resty-http` from the accounts API handler
- Seafile API: `POST /api2/groups/` and `PUT /api2/groups/:id/`

### 4.4 SSL Certificates

- Generate self-signed cert on first boot
- UI option to upload custom cert/key
- Future: Let's Encrypt integration via `acme.sh`

---

## Phase 5: Packaging & Installation

### 5.1 Installer

- Shell script (`install.sh`) that:
  1. Partitions disks (HAMMER2 for data pool)
  2. Installs packages via `pkg`
  3. Deploys `/usr/local/flynas/` directory
  4. Creates SQLite database and runs schema
  5. Seeds app templates
  6. Generates SSL cert
  7. Enables and starts services
  8. Prints URL for initial web setup

### 5.2 Config Import/Export

- Export: Dump `config`, `users`, `groups`, `s3_buckets` (minus secrets), `vms`, `app_templates` to JSON
- Import: Validate JSON, apply to database, sync system state (create users, restore cron schedules, etc.)

---

## Implementation Order

| Step | What | Depends On | Status |
|---|---|---|---|
| 1 | Base system + packages | — | ✅ done (h2dev dev VM) |
| 2 | SQLite schema + db.lua helper | 1 | ✅ done (FFI binding) |
| 3 | OpenResty skeleton + auth | 2 | ✅ done (passwordless TOTP/email) |
| 4 | Dashboard API (system stats) | 3 | ✅ done |
| 5 | Clay UI scaffold + dashboard page | 4 | ✅ done (incl. login/setup flow + QR) |
| 6 | Network API + UI | 3 | ✅ done |
| 7 | Accounts API + UI | 3 | ✅ done |
| 8 | Storage API + UI (HAMMER2 multi-volume) | 3 | ✅ done |
| 9 | Backup API + UI (snapshots, CryFS, S3) | 8 | ✅ done (live-source backup; restore/share + config import/export deferred) |
| 10 | VM API + UI (QEMU/NVMM) | 8 | ✅ done (console + LAN bridge deferred) |
| 11 | App templates + one-click install | 10 | ✅ done (catalog + VM-from-template; guest OS provisioning deferred) |
| 12 | Monitoring system + UI | 3 | ✅ done (raw cosockets; dashboard summary card deferred) |
| 13 | Installer script | All | ◐ install.sh exists, needs rework for current layout |
| 14 | Testing & hardening | All | — |

---

## Resolved Decisions

1. **HAMMER2 multi-disk**: Use HAMMER2's native multi-volume support with the disk set fixed at creation (`newfs_hammer2` across multiple raw disks). No LVM or software RAID layer. `volume-add`/`volume-del` do not exist on DragonFly 6.4, so volumes cannot be grown/shrunk after creation.
2. **Encrypted backup**: Use `cryfs-batch` (separate project, see `cryfs-batch/SPEC.md`). Implements CryFS security model without FUSE.
3. **Clay WASM toolchain**: Cross-compile on Linux. Deploy built artifacts (`.wasm` + `.js`) to DragonFlyBSD. No WASM toolchain needed on the NAS.
4. **Passwordless auth**: No user-chosen passwords. Initial setup enrolls admin TOTP (QR + manual secret); login verifies TOTP or emailed one-time code. System accounts get random argon2-hashed passwords, created via a setuid helper (`scripts/flynas-helper.c`) so nginx workers never run privileged commands.
5. **Privilege separation**: nginx runs `user www flynas`; DB is 664 www:flynas and `/usr/local/flynas` must stay www-writable for SQLite WAL files (deploy with `tar -xof`, see memory notes).

## Progress Log

- **2026-09-07**: Step-7 follow-ups closed and the **fresh-install path proven
  end to end** on a pristine guest (overlay reset from the locked base).
  - **SameSite=Strict → Lax** on the session cookie (`auth.lua`). SSO begins
    with a top-level nav from the app to `/authorize`, which is cross-site the
    moment FlyNAS and the app differ in hostname; a Strict cookie does not ride
    it, so every sign-on became a re-login. The logout cookie had to change
    with it — mismatched attributes leave the original in place. Verified on
    the wire, twice.
  - **dnsmasq reservation race — root cause was not SIGHUP timing.**
    `sync_dhcp` already runs at install time while the VM is `stopped`, long
    before the guest boots. The bug was `dhcp-range=10.77.0.50,10.77.0.250`,
    a pool *overlapping* the reservation range (`10.77.0.<100+id>`): losing the
    race yielded a valid 12h dynamic lease, not a missing one. Now
    `dhcp-range=10.77.0.0,static` — reservations only, so the same race costs a
    retry instead of a wrong address. `forge1` came up on its reserved
    `10.77.0.101` on first boot (the July run had been retargeted to `.102`).
  - **Installer reworked (step 13).** The old one created a `flynas` *user* and
    chowned the tree to it; there is no such user — nginx runs `www` in the
    `flynas` group, and the tree needs 2775 setgid for SQLite's WAL files. It
    also installed only the web stack. Now also: dnsmasq/qemu/cdrtools,
    `images/` + `seeds/` (0700, one-time OIDC secrets), OIDC keypair with
    deterministic ownership, `nvmm_load="YES"`, if_bridge/pf, the snapshot
    cron, and the Alpine base via `FLYNAS_ALPINE_IMAGE`. It still cannot
    *produce* that image — patching `no_timer_check` means writing ext4, which
    DragonFly cannot do — so it takes a supplied one and warns when absent.
  - **Verified on a virgin `v6.4.2-RELEASE #11` guest:** install.sh clean →
    service up (`health=200`, `setup_done:false`) → admin via `/api/setup` →
    bridge + dnsmasq up → Forgejo installed (deterministic `.101`) → seed ISO
    built → QEMU with `accel=nvmm` → cloud-init: docker → container pulled →
    healthcheck → **`OIDC registration: ok`** (242s) → Forgejo serves 200 with
    "Sign in with flynas" on `/user/login`.
  - **Not re-verified: the browser SSO click-through.** `/usr/bin/google-chrome`
    in the claude-box sandbox is a dangling symlink into `/opt/google`, which
    `sandbox.conf` does not mount. The chain up to it is confirmed, and the
    July browser round-trip is unchanged code — but the click itself was not
    re-run. Add `ro /opt/google` to `sandbox.conf`, or run it outside the box.
  - **uitest tunnels could pass without a tunnel.** `run-all.sh` /
    `run-app-sso.sh` forwarded 8443, which inside the sandbox is the box's own
    HTTPS proxy, and ssh defaults `ExitOnForwardFailure=no` — so the bind
    failed, ssh stayed up, and the health check hit the proxy. Both now use
    `FLYNAS_PORT` (default 19443) and make the forward fatal.
  - **pf port-forward untestable from here:** the rdr anchor is correct
    (`rdr pass on vtnet0 ... 20001 -> 10.77.0.101:3000`) but loopback never
    traverses `vtnet0`, so a LAN client is needed to exercise it.

- **2026-06-30**: Guest provisioning kickoff (§2.11). Decided the mechanism —
  **cloud-init + Docker on an Alpine `generic` cloud image**, per-VM NoCloud
  `cidata` seed; **QEMU+NVMM kept, Firecracker rejected** (KVM-only, no NVMM
  backend, multi-month rewrite for no gain on long-lived services); issuer =
  **plain HTTP on the `10.77.0.1` bridge**; **provision on first Start**, not
  install; **Forgejo** the first vertical slice. **Step 0 spike VERIFIED on
  h2dev:** qcow2 COW overlay boots under NVMM with a `mkisofs -volid cidata`
  seed; cloud-init applies hostname/`write_files`/`runcmd`; `apk add docker` +
  `docker run hello-world` prints "Hello from Docker!". Two recipe gotchas
  surfaced: image `/dev/console` = framebuffer `tty0` not `ttyS0` (write
  markers to `/dev/ttyS0`); Docker needs `rc-service cgroups start` before
  `docker start`. **Steps 1–2 DONE + verified:** base image staged at
  `/usr/local/flynas/images/alpine.qcow2`; `flynas-helper` `vmcreate` gained
  `[base|-]` → COW qcow2 overlay (208KiB) or raw as before, `vmstart` picks the
  format via qcow2-magic sniff (`is_qcow2`) so plain VMs stay raw and Lua paths
  are untouched; `valid_basename` blocks traversal. **Step 3 DONE + verified:**
  `vmseed` helper verb builds a NoCloud `cidata` seed from stdin user-data →
  `/usr/local/flynas/seeds/<name>.iso` (root 0600); `vmstart` auto-attaches it
  as a 2nd virtio drive when present; `vmdelete` sweeps it; `exec.vm_seed`
  wraps it. Verified end-to-end on h2dev (overlay → seed → boot → cloud-init
  consumed). **Step 4 DONE + verified:** `app_templates.cloud_init` loaded from
  per-app `lua/recipes/<app>.yaml` files; `recipes/forgejo.yaml` (Docker +
  cgroups fix + Forgejo container + `admin auth add-oauth` OIDC source);
  `util/cloudinit.render` (`{{KEY}}` fill, errors on unresolved) +
  `apps.render_user_data` (vars from VM + OIDC stash). Verified: luajit render
  test (placeholders map, guard fires) + recipe loads into live DB, require
  chain resolves. **Step 5 DONE + verified:** OIDC bridge reachability —
  nginx serves `/api/oidc/` over plain http on :80 (guest-facing, no 301);
  `issuer()` request-relative (scheme+Host) so a guest over 10.77.0.1 gets a
  self-consistent `iss`; `discovery()` splits `authorization_endpoint` to a
  browser-facing `config.external_url` (fallback request-relative); install
  stashes `BRIDGE_ISSUER`. Verified guest-sim discovery/jwks over :80, the
  external_url split, 443 unaffected. Known gap: stashed `redirect_uri` /
  Forgejo `ROOT_URL` still guest-IP-based — fix in step 6 with external_url +
  port-forward. **Step 6 DONE + verified:** `apps.install` builds a qcow2
  overlay of the Alpine base, auto-creates a port-forward (20000+id→app port),
  and stashes a browser-facing redirect_uri (external host + host_port);
  first Start (`vms.start`, gated by `vms.provisioned`) renders the recipe →
  `vm_seed` → provisioned=1/delivered=1/secret nulled. Fixed a setuid mkisofs
  priv-drop (`run_root` forces child ruid=0). Verified via forged-session API
  (install→overlay+forward+redirect; start→seed attached, flags, rendered
  ROOT_URL/key/discover-url correct).
- **2026-07-01**: Step 7 live E2E (in progress). Fixed the hard blocker — stock
  Alpine flakily kernel-panics under NVMM ("IO-APIC + timer doesn't work");
  baked `no_timer_check` into the base image's extlinux cmdline via `debugfs`
  on a Linux host (whole-disk ext4, rootless), 3/3 clean boots at 2048MB after.
  Whole provisioning pipeline then ran end to end on h2dev: install → qcow2
  overlay → seed → boot → cloud-init → Docker (cgroups fix) → Forgejo container
  pulled+started → healthcheck passed → OIDC registration attempted. Two
  last-mile recipe fixes applied + CONFIRMED on the re-run: Forgejo home 200 +
  "Sign in with flynas" button live on /user/login, `OIDC registration: ok`
  (retry rode through a transient SQLite lock). add-oauth fetching discovery
  from inside the guest proves the bridge issuer end-to-end. Only the
  interactive browser SSO click remains (needs slirp tunnels + puppeteer).
- **2026-07-01 (later)**: Step 7 DONE — **full browser SSO round-trip green**.
  Drove headless Chrome (two ssh -L tunnels) through Forgejo "Sign in with
  flynas" → FlyNAS /authorize → token exchange over the bridge → signed into
  Forgejo as the FlyNAS user. Caught + fixed a real bug: the token endpoint only
  accepted `client_secret_post`, but Forgejo (and most clients) use
  `client_secret_basic` — every exchange was failing. `tools/uitest/test-app-sso
  .mjs` + `run-app-sso.sh` added. §2.11 steps 1-7 complete; only step 8
  (generalize to the other 8 apps) remains. Follow-ups: dnsmasq reservation
  race, SameSite=Strict cookie for cross-host production SSO.
- **2026-06-15**: Serial console. `GET /api/vms/:id/console` upgrades to a
  WebSocket (`resty.websocket.server`) and bridges it to the VM's serial
  unix socket with two `ngx.thread` cosocket pumps; UI "Console" button
  opens a JS terminal overlay (outside Clay — `<pre>` + keydown→ws).
  Verified: overlay open/close, WebSocket upgrade through nginx, and the
  stopped-VM "console unavailable" path (test-vms). Real byte streaming
  needs a guest with `console=ttyS0` (SeaBIOS POST isn't on serial with
  `-vga none -display none`, and its one-shot output predates a late
  connect). **UI-test determinism:** added a `window.__flynasPauseRefresh`
  flag the tests set after login so the 5s auto-refresh never races
  clicks/keystrokes; lifecycle clicks use a `clickUntil` retry and
  form-fill steps settle ~300ms between fields. All four UI tests green.
- **2026-06-14 (latest+2)**: Guest DHCP + port-forwards. dnsmasq on
  `flynas0` (started/stopped by the helper with the bridge) hands each VM
  a reserved `10.77.0.<100+id>` via a SIGHUP-reloaded dhcp-hostsfile that
  the API rewrites from the DB; VM create/install assign the IP and the
  app auto-monitor now targets it. Port-forwards: `port_forwards` table,
  `GET/POST /api/vms/:id/forwards` + `DELETE /api/forwards/:id`, helper
  `pffwd` rebuilds the `flynas-fwd` pf rdr anchor from a re-validated CSV
  spec (no raw-rule injection from www). VMs page shows each VM's IP and
  expands to add/remove forwards. Verified on h2dev: enable → dnsmasq up,
  create → reservation in dhcp-hosts, add forward → rdr rule in anchor,
  delete/disable → all cascade-cleaned; test-vms covers it end-to-end
  (NAT enable → create → forward add/remove → lifecycle → delete →
  disable). UI-test note: the refresh debounce needs interaction events to
  register, so the form-fill steps settle ~300ms between fields. Tests
  green: test-vms, test-apps, test-monitoring.
- **2026-06-14 (latest+1)**: VM networking — NAT'd internal bridge
  (`flynas0`). Helper `netbridge up <uplink>`/`down` (create bridge +
  10.77.0.1/24 gateway, `net.inet.ip.forwarding=1`, pf NAT for
  10.77.0.0/24 with permissive `pass all`); `vmstart` joins each tap to
  `flynas0`; API `GET/PUT /api/network/vmnet`; VMs-page "NAT network"
  toggle. Verified on h2dev: enable → pf Enabled + NAT rule + forwarding +
  bridge, VM tap joins, disable → clean, **vtnet0/SSH untouched
  throughout**. Chose NAT over physical-NIC bridge because the dev VM's
  slirp can't be L2-bridged (user decision); gives guests outbound NAT +
  host-side monitor reachability without LAN bridging. Gotchas: `pfctl`
  is at `/usr/sbin/pfctl` not `/sbin` (wrong path → silent execv fail →
  pf never enabled); pf `($if:0)` nat target doesn't parse on this pf,
  use `($if)`. Also added a refresh debounce: the VMs/Monitoring 5s
  auto-refresh now pauses for 3s after any interaction (keystroke or page
  action) so a rebuild can't drop an in-flight click — fixes recurring
  create-form flakiness in the UI tests. Deferred: guest DHCP, inbound
  port-forwards (rdr), serial console. Tests green: test-vms (incl. NAT
  toggle), test-monitoring, test-apps.
- **2026-06-14 (latest)**: Step 11 done. App catalog (9 templates seeded
  in init.lua) + `api/apps.lua` (list + install = VM-from-template with
  auto-monitor linked via `vms.monitor_id`, cascaded on VM delete), Apps
  page in Clay UI, `tools/uitest/test-apps.mjs` (one-click Seafile install
  → VM on VMs page → cleanup, green). **Found+fixed a foundational bug:**
  `util/db.lua` bound params with `ipairs`/`#`, truncating at the first
  `nil` — an optional NULL column silently dropped all later binds (mac,
  app_template, ip_address). Switched to `select('#', ...)` (`table.pack`
  is absent in this LuaJIT — its absence crashed `init_by_lua`, caught via
  error.log). Also hardened the VMs/Monitoring auto-refresh to pause while
  a form is focused or has content (the 5s rebuild was racing UI clicks/
  keystrokes). Real guest provisioning (ISO + unattended install +
  post-install script) deferred — needs the VM LAN bridge. Caveat: the
  puppeteer tunnel (`ssh -f -L 8443`) drops intermittently in this
  sandbox and masquerades as UI failures — run the tunnel and the test in
  one shell (`ssh -N -L … & … ; kill $!`) for reliable results.
- **2026-06-14 (later)**: Step 10 done. VM hosting on QEMU 9.2 + NVMM
  (DragonFly ships `/boot/kernel/nvmm.ko`; works nested on h2dev since
  the Linux host has `kvm_intel nested=Y` and launches it `-cpu host`).
  Helper commands `vmcreate`/`vmstart`/`vmstop`/`vmdelete`; QMP control
  from Lua (`util/qmp.lua`); `api/vms.lua` (CRUD + start/stop/suspend/
  resume, live QMP status); VMs page in Clay UI; `tools/uitest/test-vms.mjs`
  (create→start→suspend→resume→stop→delete, all green). Gotchas:
  (1) `qemu -daemonize` rejects `-nographic` — use `-display none`;
  (2) QEMU creates the QMP unix socket 0750 regardless of umask, so www
  (flynas group) can't connect() — helper makes the run dir setgid-flynas
  and chmods sockets 0770 after launch; (3) don't gate live status on the
  pidfile (QEMU writes it 0600 root, www can't read) — use QMP reachability;
  (4) the 5s page auto-refresh races puppeteer clicks and can drop
  keystrokes mid-form — refresh now pauses while an input is focused, and
  tests settle ~800ms between click and assert; (5) `pkg` left qemu
  half-installed after a disk-full abort (7.9G of stale `/var/crash` dumps)
  — `pkg install -fy qemu` to repair, `df` only after `sync`. See
  [[nvmm-qemu-virtualization]]. NVMM persistence across reboot
  (`nvmm_load="YES"` in loader.conf) is an installer TODO; the helper
  kldloads it on demand meanwhile.
- **2026-06-14**: Step 12 done. Monitoring system — `util/probe.lua` (5
  check types: http/keyword via raw cosocket GET, tcp connect,
  ping/dns via `resty.shell` argv), `util/notify.lua` (webhook
  cosocket POST + email via smtp), `monitor_worker.lua`
  (`init_worker_by_lua`, worker-0-only 10s `ngx.timer.every`,
  concurrent `ngx.thread` probes, status-change notifications, 90-day
  prune), `api/monitors.lua` (monitors CRUD + pause/resume + history +
  summary + uptime windows; notification-channel CRUD; per-monitor
  channel assignment), nginx `init_worker_by_lua_file`, router wiring,
  Monitoring page in Clay UI, `tools/uitest/test-monitoring.mjs`.
  Verified end-to-end on h2dev: all 5 check types (up+down), webhook
  delivered to a local listener, full UI flow green. Gotchas: (1) box
  has no `lua-resty-http` — hand-rolled HTTP over `ngx.socket.tcp`;
  (2) `resty.shell` argv form keeps ping/dns injection-safe and
  non-blocking in the timer; (3) the page's 5s live-refresh races with
  puppeteer's instant clicks — UI tests need short settle sleeps
  between click and assertion (real users rarely hit the 120ms
  window). Deferred: dashboard compact monitor-summary card (§4.1),
  response-time sparkline + 90-day uptime grid, monitor history
  pagination UI. `pkill -f 'ssh … 8443'` self-kills the agent shell
  (matches its own command line, exit 144) — gate the tunnel with a
  `curl` health check instead.
- **2026-06-12 (evening)**: Step 9 done. Backup API (snapshots CRUD + schedule, S3 bucket CRUD, sync trigger + status, restore browse), helper snapcreate/snapdelete/snaplist/backupsync, `scripts/backup-sync.sh`, `cron/hourly-snapshots.sh`, Backup page in Clay UI, `tools/uitest/test-backup.mjs`. cryfs-batch cross-compiles for DragonFly (`GOOS=dragonfly go build`, static binary); rclone installed from pkg. **Kernel panic found**: mounting a snapshot PFS of a multi-volume HAMMER2 panics 6.4 (`hammer2_base_delete` during flush) — backup now reads the live mountpoint instead; needs a kernel-side fix before point-in-time backups (candidate bug for the hammer2-raid6 harness). Gotchas: (1) daemons spawned via the setuid helper inherited nginx's listen sockets — after a netif restart, dhclient held ports 80/443 and nginx couldn't start; fixed with `closefrom(3)` in the helper, stale deployments need `pkill dhclient`; (2) cjson escapes `/` and randomizes key order — the sync job file is key=value lines, not JSON; (3) Clay UI: a button whose label duplicates a nearby title breaks text-targeted UI tests (S3 card title renamed "New bucket"); (4) panic recovery: QMP `system_reset`, then fsck `/dev/vbd0s1d` from single-user over the serial socket. Test volume/users/buckets removed from h2dev afterwards (fstab discipline).
- **2026-06-12 (later)**: Step 6 done. Network API (config/timezone/ntp) + helper commands (`netconfig`/`timezone`/`ntp`) + Network page in Clay UI. Verified on h2dev: DHCP→static→DHCP round-trip survives netif restart (slirp re-DHCPs), timezone EDT/UTC round-trip, traversal rejected, NTP enable/disable. Plan changes: dntpd not ntpd (server in rc.conf `dntpd_flags`); `/etc/localtime` is a copy not symlink (+ `/var/db/zoneinfo`). Gotchas: `service X stop` refuses once `X_enable="NO"` — use `onestop`; UI tests need preconditions seeded (tank volume, testuser1/testgrp) and poll-based waits (newfs can take 20s, /api/disks smartctl probes are slow).
- **2026-06-12**: Steps 7+8 done. Accounts: keypair endpoint, Accounts page (users/groups/membership/SSH/keygen-download), verified in headless Chrome. Storage: helper volcreate/voldestroy/scrub/vollist, storage API, Storage page (disk picker, create/delete/scrub), verified end-to-end on vbd1–vbd4. Found+fixed: (1) user/group DELETE silently failed via user_groups FK (no cascade); (2) **LuaJIT `pipe:close()` on this platform always returns true** — all helper exit codes were swallowed; exec.lua/users.lua now append an in-band `EXIT:<code>` marker. Plan change: no `volume-add`/`volume-del` on DragonFly 6.4 (open question 3 resolved). Test volumes removed from h2dev afterwards — fstab entries pointing at harness scratch disks would break boot when the harness regenerates them.
- **2026-06-11**: Login/setup flow in Clay UI — auth screens in C, state machine + keyboard input in JS, public `GET /api/setup/status`, TOTP QR code on setup screen (vendored qrcode-generator). Fixed `is_setup_done` error swallowing. Verified end-to-end in headless Chrome against h2dev (login → TOTP → dashboard; QR decodes to correct otpauth URI).
- **Earlier**: VM bootstrap (openresty, argon2, setuid helper), session auth → passwordless conversion, dashboard API + Clay dashboard page.

## Open Questions

1. **Seafile on DragonFlyBSD VM**: Seafile typically runs on Linux. The VM approach (QEMU/NVMM with a Linux guest) handles this, but need to create/test the post-install automation scripts.
2. ~~**Tap networking for VMs**~~: RESOLVED 2026-06-14 — `if_tap` + `if_bridge` work on 6.4. Implemented as a host-internal **NAT'd bridge** (`flynas0` 10.77.0.1/24 + pf NAT + ip forwarding), not a physical-NIC bridge, because the dev VM's slirp interface can't be L2-bridged (see §2.7). Each VM's `tap<1000+id>` joins `flynas0`. True physical-NIC bridging (guests with LAN identities) is real-hardware-only; the NAT model gives guests outbound + host-side monitor reachability now.
3. ~~**HAMMER2 volume-del behavior**~~: RESOLVED 2026-06-12 — `volume-add`/`volume-del` do not exist in DragonFly 6.4's hammer2(8). Volume disk sets are fixed at creation; see §2.5.
