#!/bin/sh
# Remote management over the NIC, as the Pi will be run: rc brings up
# DHCP on vtnet0 and starts sshd with no console interaction, and the
# host logs in over the network (QEMU user net, host port forwarded to
# the guest's port 22).  netmgmt.exp then checks the lease, default
# route, resolver and the sshd listener, reboots over ssh, and checks
# that DHCP and sshd come back with the same host key.
#   run-netmgmt.sh [-n] WORK [-- arm-vm args...]
#   -n  reuse WORK/root from an earlier run (only redo the image)
# WAIT= sets the timeout per step (default 900 s).
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
reuse=0
[ "${1:-}" = -n ] && { reuse=1; shift; }
W=${1:?usage: run-netmgmt.sh [-n] WORK [-- arm-vm args...]}
shift
[ "${1:-}" = -- ] && shift
set -e
mkdir -p "$W"
W=$(cd "$W" && pwd)
R="$W/root"
[ $reuse = 1 ] && [ -d "$R/etc" ] || "$D/bin/arm-installworld" "$R"

printf '/dev/vbd0\t/\tufs\trw\t1\t1\n' > "$R/etc/fstab"
cat > "$R/etc/rc.conf" <<EOI
hostname="flynas"
ifconfig_vtnet0="DHCP"
sshd_enable="YES"
EOI
# The host's key for root; host keys are left to rc (first boot).
rm -f "$W/id_ed25519"* "$W/known_hosts" "$R"/etc/ssh/ssh_host_*
ssh-keygen -q -t ed25519 -N '' -C netmgmt -f "$W/id_ed25519"
mkdir -p "$R/root/.ssh"
cp "$W/id_ed25519.pub" "$R/root/.ssh/authorized_keys"
{
    echo "$R/root/.ssh root wheel 700"
    echo "$R/root/.ssh/authorized_keys root wheel 600"
} >> "$R.metalog"
"$D/bin/arm-mkimg" -s 400m "$R" "$W/root.img"

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
# gssh: ssh to the guest, retrying until sshd answers (60 s).
cat > "$W/gssh" <<EOI
#!/bin/sh
i=0
until ssh -F /dev/null -i "$W/id_ed25519" -p $port \\
    -o UserKnownHostsFile="$W/known_hosts" -o StrictHostKeyChecking=accept-new \\
    -o BatchMode=yes -o ConnectTimeout=5 root@127.0.0.1 true 2>/dev/null; do
    i=\$((i + 1)); [ \$i -ge 30 ] && { echo "gssh: no sshd on port $port" >&2; exit 1; }
    sleep 2
done
exec ssh -F /dev/null -i "$W/id_ed25519" -p $port \\
    -o UserKnownHostsFile="$W/known_hosts" -o StrictHostKeyChecking=yes \\
    -o BatchMode=yes root@127.0.0.1 "\$@"
EOI
chmod +x "$W/gssh"
sed -e "s|@W@|$W|g" "$T/netmgmt.exp" > "$W/netmgmt.exp"

ARM_VM_REBOOT=1 ARM_VM_ARGS="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:$port-:22
    -device virtio-net-device,netdev=n0
    -drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-device,drive=d0" \
    "$T/vmexpect.py" -w "${WAIT:-900}" "$W/netmgmt.exp" -- \
    -m 2G -t 0 -a "vfs.root.mountfrom=ufs:vbd0" "$@"
