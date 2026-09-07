# FlyNAS — Dev/Test Scaffolding Evaluation (July 2026)

*Written 2026-07-01. Scope: how we launch, kill, observe, and push files to the
dev VM(s), and how we run tests — with suggested improvements.*

## Current state

### Two QEMU environments, and this repo points at the wrong one

Actual development happens on **h2dev** — a DragonFly 6.4 VM launched from the
sibling `../hammer2-raid6` project's harness:

```
qemu-system-x86_64 -name h2dev -enable-kvm -smp 4 -m 4G -nographic
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2322-:22        # slirp SSH
  -chardev socket,...,logfile=logs/console.log,logappend=on   # serial → log
  -chardev socket,id=mon,path=run/qmp.sock ...                # QMP
  -pidfile run/qemu.pid -daemonize
```

That harness is good: persistent serial log, QMP control socket, pidfile, and a
`bin/` toolkit (`console` tail/grep/interact, `qmp`, `push`/`pull` rsync,
`watcher` panic-detector daemon). SSH access is via the `h2dev` alias in
`~/.ssh/config` (127.0.0.1:2322, ControlMaster + ControlPersist).

Meanwhile, **everything in this repo is stale**, pointing at a defunct
bridged-networking VM at 192.168.25.102:

| Script | Status |
|---|---|
| `launch-dfly.sh` | Legacy: OVMF + graphical + physical-NIC bridge (`br0`/`tap0`, mutates host networking with sudo). Not how h2dev runs. |
| `dfly-exec.sh` | `ssh root@192.168.25.102` — dead IP. Real path is `ssh h2dev`. |
| `mount-dfly.sh` | sshfs to the dead IP; `mnt/dfly` is currently an unmounted husk (one stale `var/` entry). |
| `docs/CLAUDE.md` | Moved out of the repo root, so it **no longer auto-loads** as project instructions — and its content (user `bz`, 192.168.25.102, sshfs) predates the h2dev workflow entirely. |

### Deploy: no script at all

The overlay (`overlay/usr/local/flynas/...`) is the deployable artifact, but
deployment is ad-hoc knowledge: tar over ssh with `tar -xof` (ownership
matters — `/usr/local/flynas` must stay www-writable for SQLite WAL), rebuild
`flynas-helper.c` on the VM (setuid), restart/reload openresty, re-run
`init.lua` migrations. None of that is executable; it's folklore in the plan
doc and session memory. This is the single biggest gap.

### Logging / observability

- **h2dev serial**: captured to `../hammer2-raid6/logs/console.log`
  (append-mode) with `bin/console tail|grep|follow`. Good.
- **FlyNAS logs on the VM**: two files with a known gotcha —
  `/var/log/flynas/error.log` (http-context) vs `logs/error.log` (master
  only). No wrapper; every debugging session rediscovers this.
- **App-VM (nested guest) consoles**: the flynas `vmstart` wires serial to a
  unix socket only — no `logfile=` — so guest console output (cloud-init,
  Docker pulls, recipe failures) is lost unless someone was attached at the
  moment it happened. Step-7 debugging required live serial-watching for this
  exact reason.

### Kill / recover

- h2dev: QMP socket + `bin/qmp system_reset` + the watcher's panic
  classification (used during the HAMMER2 snapshot-mount panic). Adequate.
- No stop/start/status wrapper visible from *this* repo; you must know to cd
  into `../hammer2-raid6`.
- No snapshot/rollback workflow: h2dev's system disk is a qcow2
  (`overlay-system.qcow2`) but nothing scripts "checkpoint before risky test /
  roll back after" — the backup-panic recovery (fsck from single-user over
  serial) was manual archaeology.

### File transfer

- `../hammer2-raid6/bin/push`/`pull` (rsync with sane excludes) exist but
  target `/root/h2/` for the kernel project — not reusable as-is.
- sshfs mount is the documented path but is stale/unmounted; rsync/scp over
  the `h2dev` ControlMaster connection is what actually gets used.

### Tests

- `tools/uitest/*.mjs`: 11 puppeteer tests + 3 runner scripts. The README is
  honest about the fragility: ssh `-L` tunnels drop and masquerade as UI
  failures; `pkill -f` self-kills the shell; TOTP secret extraction from the
  DB is a manual incantation; per-test preconditions (seed `tank` volume,
  `testuser1`, clean vms table…) are prose, not code.
- No single "run the whole suite" entry point; each test opens its own
  tunnel-lifecycle dance. `run-app-sso.sh` shows the mature pattern (tunnel +
  health-gate + trap cleanup) but it's per-test.
- No pre-deploy checks: a Lua syntax error only surfaces after deploy when
  `init_by_lua` fails on the VM.

## Suggested improvements (ordered)

### 1. `bin/` harness in this repo (highest leverage: deploy)

Create `dfly/bin/` mirroring the hammer2-raid6 pattern, VM-details in one
place:

- **`bin/deploy`** — the missing piece. rsync/tar the overlay to h2dev with
  correct ownership (`tar -xof` semantics), compile `flynas-helper.c` +
  install setuid, reload openresty, then health-gate on
  `curl -ks https://…/api/health`. Optional `--full` for schema-touching
  deploys (restart, verify migrations in error.log). Every session currently
  re-derives this.
- **`bin/exec`** — replace `dfly-exec.sh` with `exec ssh h2dev "$@"`.
- **`bin/logs`** — `logs flynas|nginx|console [-f]`: tails the right file
  (`/var/log/flynas/error.log` vs master log vs serial console) so the
  two-error-logs gotcha is encoded once.
- **`bin/vm`** — status/stop/reset for h2dev by delegating to the
  hammer2-raid6 harness (don't duplicate QMP code; shell out to
  `../hammer2-raid6/bin/qmp`).

### 2. Fix the stale root scripts + CLAUDE.md

- Move `CLAUDE.md` back to the repo root (it must live there to auto-load)
  and rewrite it for reality: `ssh h2dev`, the bin/ harness, overlay deploy
  flow, the two log paths, uitest invocation. This file is the highest-value
  doc in the repo for agent-assisted development — right now it actively
  misleads.
- Delete or clearly quarantine `launch-dfly.sh`, `mount-dfly.sh`,
  `dfly-exec.sh` (e.g. `attic/`). Keeping dead 192.168.25.102 paths around
  guarantees future confusion.

### 3. Persist app-VM consoles (small product change, big debug win)

In `flynas-helper.c` `do_vmstart`, change the serial chardev from bare
`unix:` socket to the socket+logfile form (exactly what the h2dev harness
does):

```
-chardev socket,id=ser,path=<run>/vm-<id>-serial.sock,server=on,wait=off,logfile=<log>/vm-<id>-console.log,logappend=on
-serial chardev:ser
```

Live console (WebSocket UI) keeps working; cloud-init/provisioning output
survives for post-mortem. Add log rotation/truncate-on-start. With 8 more
recipes to bring up (§2.11 step 8), this pays for itself immediately —
step-7 debugging was live-serial-watching because nothing persisted.
Expose the last N lines via an API endpoint later (nice-to-have for the UI).

### 4. One test runner + tunnel manager

`tools/uitest/run-all.sh`:
- open ONE health-gated tunnel (the `run-app-sso.sh` pattern: `& T=$!`,
  trap cleanup, `curl` gate — never `pkill -f`),
- extract `TOTP_SECRET` once,
- seed preconditions via API (tank volume, testuser1/testgrp), run tests in
  dependency order, clean up (volumes especially — stale fstab entries break
  the next boot),
- summarize pass/fail.

The per-test prose preconditions in the README become `seed()`/`teardown()`
code. This also gives step 8 a repeatable "recipe green?" check:
parameterize `run-app-sso.sh` by app name/port instead of hardcoding
`forge1`/3000/20001.

### 5. Pre-deploy checks (cheap, catches the common failure)

A `bin/check` run by `bin/deploy` before shipping:
- `luajit -bl` (or `luacheck`) over `overlay/**/*.lua` — syntax errors
  currently surface only as an init_by_lua failure on the VM,
- compile `flynas-helper.c` locally with `-fsyntax-only` (clang) to catch
  obvious breakage before the on-VM build,
- `ui/build.sh` if `ui/` changed (the overlay's `clay.wasm` must not go
  stale relative to `main.c`).

### 6. Snapshot/rollback for risky tests

h2dev's disk is already qcow2. Add `bin/vm checkpoint` / `bin/vm rollback`:
guest-quiesce (or accept crash-consistency), `qmp` `snapshot-save` /
`snapshot-load` (or offline `qemu-img snapshot` with the VM stopped). Two
past incidents justify this: the HAMMER2 snapshot-mount kernel panic (manual
single-user fsck recovery) and the disk-full half-installed qemu. Kernel
work on the RAID6 patch will keep generating panics; cheap rollback makes
the whole VM disposable.

### 7. Longer-term (note, don't build yet)

- **Second, clean h2dev instance** for installer testing (step 13): the
  current VM is hand-evolved; `install.sh` can only be honestly tested
  against a fresh DragonFly install. The hammer2-raid6 harness +
  `overlay-system.qcow2` backing-file pattern makes cloning cheap.
- **Nested-guest debugging helper**: `bin/guest <vm-name> console|ssh|ip` —
  resolves the guest IP from the flynas DB (the `run-app-sso.sh` sqlite
  query, scripted) and attaches to its serial socket on h2dev. Will be used
  constantly during recipe fan-out.
- **Watcher adoption**: the hammer2-raid6 watcher (ssh-liveness → panic
  classify → capture → recover) could babysit long soak tests of the
  monitoring worker / backup cron, but is overkill for day-to-day app work.

## Priority order

| # | Item | Why first |
|---|---|---|
| 1 | `bin/deploy` + `bin/logs` + `bin/exec` | Removes per-session folklore; everything else builds on it |
| 2 | CLAUDE.md back to root, rewritten; retire stale scripts | Currently actively misleading |
| 3 | App-VM serial `logfile=` | Directly de-risks §2.11 step 8 (8 recipes to debug) |
| 4 | `run-all.sh` + parameterized app-SSO runner | Repeatable "recipe green" gate for step 8 |
| 5 | `bin/check` pre-deploy lint | Cheap; kills the deploy-then-init-fails loop |
| 6 | checkpoint/rollback | Safety net for kernel + destructive storage tests |
