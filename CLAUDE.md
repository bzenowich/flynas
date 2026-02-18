# DragonFlyBSD Development Environment

## System Info
- DragonFlyBSD 6.4.2 (BSD, not Linux)
- Default shell: tcsh
- VM runs via QEMU with bridged networking (IP: 192.168.25.102)

## Remote Access
- Run commands on DragonFlyBSD via: `./dfly-exec.sh <command>`
- This SSHes as user `bz` to 192.168.25.102
- Set `DFLY_IP` env var to override the IP

## File Access
- User bz's home directory is mounted at `mnt/dfly` (relative to project root) via SSHFS
- Read/edit DragonFlyBSD files through `mnt/dfly/`

## Important Differences from Linux
- Package manager: `pkg` (not apt/dnf)
- Init system: rc.d scripts (not systemd)
- Compiler: gcc or clang from base
- Make: BSD make (not GNU make)
- No Linux binary emulation
