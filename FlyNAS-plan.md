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

### 2.3 Network API

| Endpoint | Method | Description |
|---|---|---|
| `/api/network/config` | GET | Current IP, netmask, gateway, DHCP status, MAC |
| `/api/network/config` | PUT | Set static IP or DHCP mode |
| `/api/network/timezone` | GET/PUT | Time zone (`/etc/localtime` symlink) |
| `/api/network/ntp` | GET/PUT | NTP server (`/etc/ntp.conf`) |

Implementation: Writes to `/etc/rc.conf` for network config. Runs `service netif restart` to apply.

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

### 2.5 Storage API

Uses HAMMER2 multi-volume support. A volume starts on one disk and can be expanded by adding more disks with `hammer2 volume-add`. Data is striped across volumes automatically by HAMMER2.

| Endpoint | Method | Description |
|---|---|---|
| `/api/disks` | GET | List all disks with serial, size, SMART health, and volume membership |
| `/api/volumes` | GET | List HAMMER2 volumes with size, usage, disk count |
| `/api/volumes` | POST | Create volume: `newfs_hammer2 -L <name> <device>`, mount it |
| `/api/volumes/:id` | GET | Volume details: disk list, usage, PFS list |
| `/api/volumes/:id/disks` | POST | Add disk to volume: `hammer2 volume-add <device> <mountpoint>` |
| `/api/volumes/:id/disks/:did` | DELETE | Remove disk from volume: `hammer2 volume-del <device> <mountpoint>` |
| `/api/volumes/:id/scrub` | POST | Trigger manual scrub: `hammer2 -f <device> bulkfree` |
| `/api/volumes/:id/scrub/schedule` | PUT | Enable/disable daily scrub |

**Volume lifecycle:**

```sh
# Create a new volume on /dev/da0
newfs_hammer2 -L data /dev/da0s1
mount_hammer2 /dev/da0s1@DATA /mnt/data

# Expand volume by adding /dev/da1
hammer2 volume-add /dev/da1s1 /mnt/data

# Later, remove a disk (data migrates off first)
hammer2 volume-del /dev/da1s1 /mnt/data
```

**Persistent mounts:** Volume mounts are written to `/etc/fstab`. Added disks are recorded in the `volume_disks` table so the UI can show which disks belong to which volume.

Shell scripts wrap `hammer2` commands. Scrub cron job: `hammer2 -f <device> bulkfree`.

### 2.6 Backup API

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

### 2.7 VM API

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

### 2.8 Monitoring API

Built-in service monitoring, replacing Uptime Kuma. Runs entirely within OpenResty using `ngx.timer` for background checks and `lua-resty-http` / `resty.socket` for probes.

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

### 2.9 Applications API

| Endpoint | Method | Description |
|---|---|---|
| `/api/apps` | GET | List available app templates |
| `/api/apps/:id/install` | POST | Create VM from template with chosen resources |

Pre-built templates for: Seafile, CryptPad, Forgejo, VaultWarden, Readeck, Jellyfin, RoundCube/smtp2go, DokuWiki, Wekan. Each template includes: ISO URL, default resource allocation, post-install script that configures the service, and a default monitor definition (type, target path, expected status).

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

**Status:** Dashboard page DONE (system/CPU/memory/volumes/disks cards, 5s polling). Login/setup flow DONE — JS owns screen transitions and API calls, C renders screens; setup wizard shows scannable TOTP QR code plus manual secret fallback; expired one-time tokens restart the flow. Accounts page DONE — users table (SSH toggle, keygen download, two-click delete), groups card with inline membership checkboxes; C queues packed page actions (low 4 bits action, rest row id) drained by JS each frame via `TakePageAction()`; accounts strings live in their own pool region (32768..49151). Remaining pages are placeholders.

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
| 6 | Network API + UI | 3 | — |
| 7 | Accounts API + UI | 3 | ✅ done |
| 8 | Storage API + UI (HAMMER2 multi-volume) | 3 | — |
| 9 | Backup API + UI (snapshots, CryFS, S3) | 8 | — |
| 10 | VM API + UI (QEMU/NVMM) | 8 | — |
| 11 | App templates + one-click install | 10 | — |
| 12 | Monitoring system + UI | 3 | — |
| 13 | Installer script | All | ◐ install.sh exists, needs rework for current layout |
| 14 | Testing & hardening | All | — |

---

## Resolved Decisions

1. **HAMMER2 multi-disk**: Use HAMMER2's native multi-volume support (`hammer2 volume-add` / `hammer2 volume-del`). No LVM or software RAID layer.
2. **Encrypted backup**: Use `cryfs-batch` (separate project, see `cryfs-batch/SPEC.md`). Implements CryFS security model without FUSE.
3. **Clay WASM toolchain**: Cross-compile on Linux. Deploy built artifacts (`.wasm` + `.js`) to DragonFlyBSD. No WASM toolchain needed on the NAS.
4. **Passwordless auth**: No user-chosen passwords. Initial setup enrolls admin TOTP (QR + manual secret); login verifies TOTP or emailed one-time code. System accounts get random argon2-hashed passwords, created via a setuid helper (`scripts/flynas-helper.c`) so nginx workers never run privileged commands.
5. **Privilege separation**: nginx runs `user www flynas`; DB is 664 www:flynas and `/usr/local/flynas` must stay www-writable for SQLite WAL files (deploy with `tar -xof`, see memory notes).

## Progress Log

- **2026-06-11**: Login/setup flow in Clay UI — auth screens in C, state machine + keyboard input in JS, public `GET /api/setup/status`, TOTP QR code on setup screen (vendored qrcode-generator). Fixed `is_setup_done` error swallowing. Verified end-to-end in headless Chrome against h2dev (login → TOTP → dashboard; QR decodes to correct otpauth URI).
- **Earlier**: VM bootstrap (openresty, argon2, setuid helper), session auth → passwordless conversion, dashboard API + Clay dashboard page.

## Open Questions

1. **Seafile on DragonFlyBSD VM**: Seafile typically runs on Linux. The VM approach (QEMU/NVMM with a Linux guest) handles this, but need to create/test the post-install automation scripts.
2. **Tap networking for VMs**: Need to configure bridge interface and tap devices. Verify DragonFlyBSD bridge/tap support and document setup.
3. **HAMMER2 volume-del behavior**: Verify that `hammer2 volume-del` migrates data off the disk before removal, or if FlyNAS needs to handle data migration manually before issuing the command.
