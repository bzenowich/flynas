# FlyNAS Implementation Challenges

## 1. OpenResty + Lua Library Ecosystem

**Risk: Medium | Impact: High**

OpenResty and LuaJIT are both available in DPorts (`www/openresty`, `lang/luajit-openresty`). The challenge is the Lua C-binding libraries needed on top:

- `lsqlite3` — links against `libsqlite3`. Must verify the LuaJIT FFI ABI matches DragonFlyBSD's build. Install via luarocks or build from source.
- `lua-resty-http` — pure Lua, should work. Install via OPM (OpenResty Package Manager) or copy the `.lua` file directly.
- `bcrypt` binding — typically a C library; may need manual `CFLAGS`/`LDFLAGS` adjustment on DragonFlyBSD.
- OpenResty discourages LuaRocks in favor of OPM. Neither is well-tested on DragonFlyBSD.

**Mitigation:** Test each library installation early. For C bindings that won't build, use LuaJIT FFI to call system `.so` libraries directly (e.g., FFI binding to `libsqlite3` instead of lsqlite3).

---

## 2. WASM Toolchain for Clay UI

**Risk: High | Impact: High**

DragonFlyBSD ships clang in base, but the `wasm32` target may not be enabled in their LLVM build. No `wasi-sdk` or `emscripten` packages exist in DPorts.

**Decision: Cross-compile on Linux.**

Clay's WASM output (`.wasm` + `.js` + `index.html`) is a build artifact that only needs to be produced once per release. The workflow:

1. Build Clay UI on a Linux host with `clang --target=wasm32-wasi` (or via `wasi-sdk`)
2. Commit the built artifacts to the FlyNAS repo
3. Deploy the static files to `/usr/local/flynas/static/` on DragonFlyBSD
4. No WASM toolchain needed on the NAS itself

This also keeps the DragonFlyBSD install minimal — no compiler toolchain required at runtime.

---

## 3. rclone Availability

**Risk: Medium | Impact: High**

rclone is not packaged for DragonFlyBSD due to Go build failures (FUSE-related dependencies).

**Options:**

| Option | Effort | Notes |
|--------|--------|-------|
| Build from source with Go, disable FUSE mount | Low | Go is in DPorts. Only need `rclone sync`, not `rclone mount`. Build with `CGO_ENABLED=0` and strip FUSE. |
| Static cross-compile on Linux | Low | `GOOS=dragonfly GOARCH=amd64 go build` produces a static binary. Deploy to NAS. |
| Write S3 sync in Lua | High | Use `lua-resty-http` with AWS Signature V4. More work but zero external dependency. |

**Recommended:** Cross-compile rclone on Linux targeting `GOOS=dragonfly`. This is a standard Go cross-compilation and avoids FUSE issues entirely.

---

## 4. Encrypted Backup: CryFS vs rclone crypt

**Risk: High | Impact: Critical**

CryFS is not packaged for DragonFlyBSD, and it requires FUSE. DragonFlyBSD's FUSE only recently gained write + mmap support (March 2024) and may not be mature enough for an encryption layer where bugs mean data loss.

### Why CryFS's security model matters

The CryFS white paper (https://eprint.iacr.org/2017/773.pdf) demonstrates security properties that rclone crypt fundamentally lacks:

| Property | CryFS | rclone crypt |
|----------|-------|--------------|
| File sizes hidden | Yes (fixed 32KB blocks) | No (visible within 16 bytes) |
| Directory structure hidden | Yes (flat blob storage) | No (hierarchy visible) |
| File count hidden | Yes (only block count visible) | No |
| Modification times hidden | Yes | No |
| Resistant to differential analysis | Yes | No |
| Resistant to watermarking attacks | Yes | No |
| Formal security proof (IND-aCFA) | Yes | No |
| Block manipulation protection | Yes (version tree) | No |

rclone crypt also has a critical flaw: every file is encrypted by a single master key. Sharing one encrypted file reveals the key to all files. Blocks can be silently truncated or relocated without detection.

### The core insight: CryFS encryption does not require FUSE

FUSE is only how CryFS presents the decrypted view to the user. The actual security model is a batch-friendly operation:

1. **Chunk** files into fixed 32KB blocks
2. **Encrypt** each block with XChaCha20-Poly1305, assign a random block ID
3. **Build** a left-max-data tree: inner nodes store pointers to child blocks, leaves store file data. Directory entries are also encrypted blocks, indistinguishable from data blocks.
4. **Track** block versions locally to detect rollback/deletion/reorder attacks
5. **Upload** the flat collection of identically-sized encrypted blobs to cloud storage

### Proposed solution: `cryfs-batch`

Build a standalone non-FUSE tool that implements CryFS's encryption model as a batch backup/restore utility. No FUSE needed.

**Architecture:**

```
Local files
    │
    ▼
┌──────────┐
│ Chunker  │  Split into fixed 32KB blocks
└────┬─────┘
     ▼
┌──────────┐
│ Encrypt  │  XChaCha20-Poly1305, random block ID per block
└────┬─────┘
     ▼
┌──────────┐
│ Tree     │  Left-max-data tree for metadata
│ Builder  │  Directory entries = encrypted blocks (indistinguishable)
└────┬─────┘
     ▼
┌──────────┐
│ Version  │  Local SQLite DB tracks expected block versions
│ Tracker  │  Detects rollback/deletion/reorder on restore
└────┬─────┘
     ▼
┌──────────┐
│ Upload   │  Sync block directory to S3 via rclone or direct S3 API
└──────────┘
```

**Implementation language:** Go (good crypto libraries, cross-compiles to DragonFlyBSD, ~1500 lines).

**Usage:**

```sh
# Encrypt a snapshot and upload to S3
cryfs-batch backup \
  --source /mnt/pool/snapshot-2024-01-15 \
  --remote s3://mybucket/encrypted \
  --password-file /usr/local/flynas/secrets/cryfs.key

# List files in encrypted backup (decrypts metadata locally)
cryfs-batch list \
  --remote s3://mybucket/encrypted \
  --password-file /usr/local/flynas/secrets/cryfs.key

# Restore specific path
cryfs-batch restore \
  --remote s3://mybucket/encrypted \
  --path /documents/report.pdf \
  --output /tmp/report.pdf \
  --password-file /usr/local/flynas/secrets/cryfs.key
```

**What an attacker observing the S3 bucket sees:** A flat directory of ~32KB encrypted blobs with random filenames. No file sizes, no directory structure, no file count, no timestamps. Only total storage volume is visible. Identical to what CryFS provides.

**Decision:** `cryfs-batch` is a separate open-source project. See `cryfs-batch/SPEC.md` for the full specification.

---

## 5. HAMMER2 Multi-Volume Support

**Risk: Medium | Impact: High**

**Decision:** Use HAMMER2's native multi-volume support (`hammer2 volume-add` / `hammer2 volume-del`). No LVM or software RAID layer.

This is the simplest approach — no extra abstraction between FlyNAS and the filesystem. A volume starts on one disk with `newfs_hammer2`, and additional disks are added with `hammer2 volume-add`. HAMMER2 stripes data across volumes automatically.

**Risks to mitigate:**

- **Tooling maturity:** `volume-add` and `volume-del` are less battle-tested than ZFS equivalents. FlyNAS should confirm each operation succeeded by re-reading volume state after the command returns. Add clear warnings in the UI before destructive operations.
- **`volume-del` behavior:** Need to verify whether `hammer2 volume-del` migrates data off the disk before removal, or if FlyNAS must handle migration. If data is not migrated automatically, FlyNAS should refuse removal of a disk that would cause data loss. This needs testing on the actual system.
- **No redundancy by default:** HAMMER2 multi-volume stripes but does not mirror (unlike ZFS mirror vdevs). Data loss on any single disk means volume loss. The mitigation is encrypted off-site backup via cryfs-batch, and clear UI messaging that multi-volume is not a substitute for backup.
- **Disk failure during operation:** If a `volume-add` or `volume-del` is interrupted (power loss, crash), the volume may be in an inconsistent state. FlyNAS should run `hammer2 cleanup` / `hammer2 emergency-mode` checks on boot and surface any issues in the dashboard.

---

## 6. Tap/Bridge Networking for VMs

**Risk: Low-Medium | Impact: Medium**

DragonFlyBSD supports `bridge` and `tap` interfaces, but operational details need attention:

- **Permissions:** `/dev/tap*` devices need to be accessible by the `flynas` user. Add `flynas` to the appropriate group, or set `devfs` rules.
- **Persistence:** Bridge and tap configuration must survive reboot. Add to `/etc/rc.conf`:
  ```sh
  cloned_interfaces="bridge0 tap0 tap1 tap2 tap3"
  ifconfig_bridge0="addm em0 up"
  ```
- **MAC generation:** Generate random locally-administered MACs (`52:54:00:xx:xx:xx` range, standard for QEMU).
- **Guest IP assignment:** No cloud-init equivalent for arbitrary guest OSes. The post-install script must handle network configuration inside the guest. For Linux guests, this means writing `/etc/network/interfaces` or a netplan config. For BSD guests, `/etc/rc.conf`.

**Mitigation:** Limit initial app templates to Linux guests with well-known network configuration paths. Provide a standardized post-install script framework.

---

## 7. Service Supervision (No systemd)

**Risk: Low | Impact: Medium**

DragonFlyBSD uses rc.d scripts. If OpenResty crashes, nothing restarts it automatically.

**Solution:** Use DragonFlyBSD's `daemon(8)` with `-r` (restart on exit):

```sh
# /usr/local/etc/rc.d/flynas
command="/usr/sbin/daemon"
command_args="-r -P /var/run/flynas.pid /usr/local/openresty/bin/openresty -g 'daemon off;'"
```

This is simple and sufficient for an appliance. No need for a process supervisor like `supervisord`.

---

## 8. smartmontools + ahci Driver

**Risk: Low | Impact: Low**

smartmontools on DragonFlyBSD has a known issue with the `ahci(4)` driver (bug #1412). The workaround is to specify the device type explicitly:

```sh
smartctl -d sat -a /dev/da0
```

The FlyNAS storage scripts should always pass `-d sat` when invoking smartctl on DragonFlyBSD.

---

## Priority Order

| Priority | Challenge | Action |
|----------|-----------|--------|
| 1 | WASM toolchain | Cross-compile on Linux (decided) |
| 2 | Encrypted backup | Build `cryfs-batch` (separate project, see `cryfs-batch/SPEC.md`) |
| 3 | HAMMER2 multi-volume | Test `volume-add`/`volume-del` behavior, especially `volume-del` data migration |
| 4 | rclone availability | Cross-compile on Linux for DragonFlyBSD |
| 5 | Lua library ecosystem | Test each library early, use FFI fallbacks |
| 6 | Tap/bridge networking | Document setup, limit to Linux guests initially |
| 7 | Service supervision | Use `daemon -r` in rc.d script |
| 8 | smartmontools | Always pass `-d sat` |
