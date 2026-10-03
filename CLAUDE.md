# FlyNAS — DragonFlyBSD NAS Appliance

A NAS appliance on DragonFlyBSD (HAMMER2 + web UI). Backend is OpenResty
(nginx + LuaJIT) + SQLite; frontend is Clay compiled to WASM. See
`docs/FlyNAS-plan.md` for the full plan and progress log.

## Dev VM: h2dev

All development and testing runs against **h2dev**, a DragonFly 6.4 guest
launched and babysat by the sibling `../hammer2-raid6` harness (QEMU + slirp,
SSH forwarded to `127.0.0.1:2322`). See `../hammer2-raid6/harness/README.md`.

The `bin/` helpers resolve host, port and SSH key from the harness's
`vmenv.sh`, **not** from a `h2dev` entry in `~/.ssh/config` — the claude-box
sandbox has no `~/.ssh` at all, so anything typing `ssh h2dev` by hand is dead
in there. `bin/exec` and `bin/vm` work either way; use them.

Inside the sandbox the VM must also be running **on the host** (`host-run.sh`)
with the `net 127.0.0.1:2322` bridge in `sandbox.conf`, or a boxed QEMU falls
back to TCG and takes 8-12 minutes per boot. `bin/vm save <name>` /
`bin/vm load <name>` snapshot around that.

**The VM login shell is tcsh.** A bare `ssh h2dev '<sh script>'` will fail
(`Illegal variable name`) on anything with sh syntax. Always force sh — the
`bin/` helpers do this for you; when going direct, pipe to sh:

```sh
ssh h2dev sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT ..."
EOF
```

## bin/ harness

Use these instead of ad-hoc ssh/scp:

| Command | What |
|---|---|
| `bin/exec [cmd]` | Run a command on h2dev (sh-wrapped), or open a shell |
| `bin/check` | Lint all overlay Lua against the VM's LuaJIT (pre-deploy) |
| `bin/deploy [--restart] [--no-check]` | rsync overlay → rebuild setuid helper → reload + health-check |
| `bin/logs {flynas\|master\|access\|vm <name>\|console} [-f]` | Tail the right log |
| `bin/vm {status\|wait\|reset\|reboot\|coldboot\|stop\|save\|load\|snapshots\|console\|attach\|log}` | Control h2dev via the hammer2-raid6 harness (anything else is passed through to its `vmctl.sh`) |
| `bin/fresh-install [check\|push\|install\|start\|all]` | Run `install.sh` onto a freshly-reset guest and verify it — the only thing that exercises the install path (`bin/deploy` assumes FlyNAS is already there) |
| `bin/arm-hosttools` | arm64 port: build bmake/config(8)/mkdep/gencat for Linux from `../dragonfly` into `tools/host/bin` |
| `bin/arm-kbuild [KERNCONF] [targets]` | arm64 port: cross-build the aarch64 kernel on the host (clang-18 + `tools/lld`), objects in `tools/kobj/` |
| `bin/arm-vm [-g] [-s N] [-t SECS] [KERNEL]` | arm64 port: boot an aarch64 kernel on QEMU `virt` (Cortex-A72, GICv2); always TCG, timeout on by default |
| `bin/arm-mkiso OUT DIR` | arm64 port: write a plain ISO 9660 md root (lowercase names) for `bin/arm-vm -r` |
| `bin/arm-sysroot [DIR]` | arm64 port: stage the fork's `/usr/include` into `tools/sysroot` |
| `bin/arm-world DIR [targets] [-j2] [VAR=val]` | arm64 port: cross-build a userland dir with bmake (static; `ARM_SHARED=1` for PIC/shared), install into the sysroot |
| `bin/arm-x86build {sync\|build\|quick\|install\|boottest}` | arm64 port: build the fork as x86_64 in h2dev (`-j2`) and boot-test it once, to prove MI changes don't break x86 |

`bin/deploy` defaults to `service flynas reload`. Use `--restart` when
`init.lua`, `schema.sql`, a recipe, or `nginx.conf` changed — `init_by_lua`
re-seeds the DB and app catalog only at master start.

## Layout & deploy model

- `overlay/` mirrors the on-VM tree rooted at `/` (e.g.
  `overlay/usr/local/flynas/...`). `bin/deploy` rsyncs it, preserving runtime
  state (`flynas.db`, `ssl/`, `logs/` are never overwritten).
- The service runs as `www` in group `flynas`; `/usr/local/flynas` stays
  group-writable (775 dirs) so SQLite WAL files work across nginx workers.
- Privileged ops go through the **setuid helper** `flynas-helper.c`
  (root:flynas, 4750), rebuilt on the VM by `bin/deploy` (`cc`). It cannot be
  compiled on a Linux host — it uses BSD-only headers (`getmntinfo`,
  `struct statfs`).

## UI tests

`tools/uitest/` — Puppeteer driving system Chrome over an ssh `-L 8443` tunnel.

- `tools/uitest/run-all.sh` — the whole safe suite under one tunnel with one
  TOTP extraction and a pass/fail summary. Prefer this.
- The runners source `bin/_common.sh`, so the tunnel and every DB probe use the
  harness identity (no `~/.ssh/config` needed) and `FLYNAS_SSH` is exported for
  the node tests that read the DB. Background the raw `ssh $VM_SSH_OPTS ...`
  for a tunnel, never `vssh` — backgrounding a function gives you a subshell
  pid and killing it leaves the tunnel behind.
- Storage/backup and app-SSO tests are stateful (scratch disks + fstab, or a
  live provisioned guest) and stay as dedicated scripts (`run-app-sso.sh`,
  etc.). See `tools/uitest/README.md`.
- Never `pkill -f 'ssh … 8443'` to recycle a tunnel — it matches the killer's
  own command line and takes out the shell.

## DragonFly vs Linux

- Packages: `pkg` (not apt/dnf). Init: rc.d (not systemd). Make: BSD make.
- No Linux binary emulation; the WASM UI is cross-compiled on Linux
  (`ui/build.sh`) and only the artifacts ship to the NAS.

## Docs

`docs/` — `FlyNAS-plan.md` (plan + progress log), `challenges.md`,
`plan_july.md` (next steps), `develop_july.md` (dev-scaffolding eval),
`dfly-arm.md` (plan for porting DragonFly to arm64 / Raspberry Pi 4).
Retired legacy launch/mount scripts live in `attic/`.
