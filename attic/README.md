# Retired scaffolding

These target a defunct dev VM (bridged networking at 192.168.25.102) that
predates the current h2dev workflow. Kept for reference only — do not use.

- `dfly-exec.sh`   → superseded by `bin/exec`
- `launch-dfly.sh` → h2dev is launched by the `../hammer2-raid6` harness
- `mount-dfly.sh`  → sshfs to the dead IP; use `bin/exec` / rsync instead
