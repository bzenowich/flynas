/*
 * flynas-helper.c — setuid root helper for FlyNAS
 *
 * Installed as /usr/local/flynas/bin/flynas-helper
 * Owned root:flynas, mode 4750
 *
 * Only allows whitelisted operations for system user/group management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <grp.h>

#define MAX_NAME_LEN 32
#define MAX_SHELL_LEN 64
#define MAX_GROUPS_LEN 256
#define MAX_KEYS_LEN 65536
#define MAX_VOL_DISKS 8
#define PW_CMD "/usr/sbin/pw"
#define SMARTCTL_CMD "/usr/local/sbin/smartctl"
#define FDISK_CMD "/sbin/fdisk"
#define NEWFS_HAMMER2_CMD "/sbin/newfs_hammer2"
#define MOUNT_HAMMER2_CMD "/sbin/mount_hammer2"
#define HAMMER2_CMD "/sbin/hammer2"
#define UMOUNT_CMD "/sbin/umount"
#define FSTAB_PATH "/etc/fstab"
#define DATA_ROOT "/data"
#define SERVICE_CMD "/usr/sbin/service"
#define RCCONF_PATH "/etc/rc.conf"
#define ZONEINFO_DIR "/usr/share/zoneinfo"
#define LOCALTIME_PATH "/etc/localtime"
#define ZONEDB_PATH "/var/db/zoneinfo"

static const char *allowed_shells[] = {
    "/bin/sh",
    "/bin/csh",
    "/bin/tcsh",
    "/usr/local/bin/bash",
    "/usr/local/bin/zsh",
    "/usr/sbin/nologin",
    NULL
};

/* Validate name: ^[a-z_][a-z0-9_-]*$, max 32 chars */
static int valid_name(const char *s)
{
    size_t len;

    if (!s || !*s)
        return 0;
    if (!(islower((unsigned char)s[0]) || s[0] == '_'))
        return 0;

    len = strlen(s);
    if (len > MAX_NAME_LEN)
        return 0;

    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Validate shell against whitelist */
static int valid_shell(const char *s)
{
    if (!s)
        return 0;
    for (int i = 0; allowed_shells[i]; i++) {
        if (strcmp(s, allowed_shells[i]) == 0)
            return 1;
    }
    return 0;
}

/* Validate disk name: ^[a-z]+[0-9]+$ (e.g. vbd0, da1, ad2) */
static int valid_disk(const char *s)
{
    size_t i = 0, len;

    if (!s || !*s)
        return 0;
    len = strlen(s);
    if (len > 16)
        return 0;

    while (s[i] && islower((unsigned char)s[i]))
        i++;
    if (i == 0 || !s[i])
        return 0;
    while (s[i]) {
        if (!isdigit((unsigned char)s[i]))
            return 0;
        i++;
    }
    return 1;
}

/* Validate comma-separated list of names */
static int valid_name_list(const char *s)
{
    char buf[MAX_GROUPS_LEN + 1];
    char *tok, *saveptr;

    if (!s || !*s)
        return 0;
    if (strlen(s) > MAX_GROUPS_LEN)
        return 0;

    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        if (!valid_name(tok))
            return 0;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return 1;
}

static void die(const char *msg)
{
    fprintf(stderr, "flynas-helper: %s\n", msg);
    exit(1);
}

/* Execute a command with given args (NULL-terminated) */
static int run(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        die("fork failed");
    if (pid == 0) {
        execv(path, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

/* Read password from stdin, pipe to pw usermod -h 0 */
static int do_passwd(const char *username)
{
    char passwd[256];
    ssize_t n;
    int pipefd[2];
    pid_t pid;
    int status;

    n = read(STDIN_FILENO, passwd, sizeof(passwd) - 1);
    if (n <= 0)
        die("failed to read password from stdin");
    /* Strip trailing newline */
    if (n > 0 && passwd[n - 1] == '\n')
        n--;
    passwd[n] = '\0';

    if (pipe(pipefd) < 0)
        die("pipe failed");

    pid = fork();
    if (pid < 0)
        die("fork failed");

    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        char *args[] = { "pw", "usermod", (char *)username, "-h", "0", NULL };
        execv(PW_CMD, args);
        _exit(127);
    }

    close(pipefd[0]);
    write(pipefd[1], passwd, strlen(passwd));
    write(pipefd[1], "\n", 1);
    close(pipefd[1]);

    /* Clear password from memory */
    memset(passwd, 0, sizeof(passwd));

    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

/* Write SSH keys from stdin to ~username/.ssh/authorized_keys */
static int do_sshkeys_write(const char *username)
{
    struct passwd *pw;
    char path[512];
    char sshdir[512];
    char keys[MAX_KEYS_LEN];
    ssize_t n;
    FILE *f;

    pw = getpwnam(username);
    if (!pw)
        die("user not found");

    snprintf(sshdir, sizeof(sshdir), "%s/.ssh", pw->pw_dir);
    snprintf(path, sizeof(path), "%s/.ssh/authorized_keys", pw->pw_dir);

    /* Create .ssh directory if needed */
    if (access(sshdir, F_OK) != 0) {
        if (mkdir(sshdir, 0700) != 0)
            die("failed to create .ssh directory");
        if (chown(sshdir, pw->pw_uid, pw->pw_gid) != 0)
            die("failed to chown .ssh directory");
    }

    /* Read keys from stdin */
    n = read(STDIN_FILENO, keys, sizeof(keys) - 1);
    if (n < 0)
        die("failed to read keys from stdin");
    keys[n] = '\0';

    f = fopen(path, "w");
    if (!f)
        die("failed to open authorized_keys");

    fprintf(f, "%s", keys);
    fclose(f);

    /* Set ownership and permissions */
    if (chown(path, pw->pw_uid, pw->pw_gid) != 0)
        die("failed to chown authorized_keys");
    if (chmod(path, 0600) != 0)
        die("failed to chmod authorized_keys");

    return 0;
}

/* Remove authorized_keys file */
static int do_sshkeys_remove(const char *username)
{
    struct passwd *pw;
    char path[512];

    pw = getpwnam(username);
    if (!pw)
        die("user not found");

    snprintf(path, sizeof(path), "%s/.ssh/authorized_keys", pw->pw_dir);

    if (access(path, F_OK) == 0) {
        if (unlink(path) != 0)
            die("failed to remove authorized_keys");
    }
    return 0;
}

/* ---- HAMMER2 volume management ------------------------------- */

/* A disk is "in use" if any mounted filesystem or fstab entry
 * references /dev/<disk> (including slices and multi-volume
 * specials like /dev/vbd1:/dev/vbd2@DATA). */
static int dev_referenced(const char *haystack, const char *disk)
{
    char dev[64];
    const char *p = haystack;

    snprintf(dev, sizeof(dev), "/dev/%s", disk);
    while ((p = strstr(p, dev)) != NULL) {
        char next = p[strlen(dev)];
        /* vbd1 must not match vbd10 */
        if (!isdigit((unsigned char)next))
            return 1;
        p += strlen(dev);
    }
    return 0;
}

static int disk_in_use(const char *disk)
{
    struct statfs *mntbuf;
    int n, i;
    FILE *f;
    char line[1024];

    n = getmntinfo(&mntbuf, MNT_NOWAIT);
    for (i = 0; i < n; i++) {
        if (dev_referenced(mntbuf[i].f_mntfromname, disk))
            return 1;
    }

    f = fopen(FSTAB_PATH, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] != '#' && dev_referenced(line, disk)) {
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

/* Split a comma-separated disk list into validated names */
static int parse_disks(const char *arg, char disks[][32])
{
    char buf[512];
    char *tok, *saveptr;
    int count = 0;

    if (!arg || strlen(arg) >= sizeof(buf))
        die("invalid disk list");
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        if (!valid_disk(tok))
            die("invalid disk name");
        if (count >= MAX_VOL_DISKS)
            die("too many disks");
        strncpy(disks[count], tok, 31);
        disks[count][31] = '\0';
        count++;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (count == 0)
        die("no disks given");
    return count;
}

static int fstab_has_mountpoint(const char *mountpoint)
{
    FILE *f = fopen(FSTAB_PATH, "r");
    char line[1024], dev[256], mnt[256];

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%255s %255s", dev, mnt) == 2 &&
            strcmp(mnt, mountpoint) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* Remove fstab lines whose mountpoint matches; atomic via rename */
static void fstab_remove_mountpoint(const char *mountpoint)
{
    FILE *in, *out;
    char line[1024], dev[256], mnt[256];
    char tmppath[] = FSTAB_PATH ".flynas.tmp";

    in = fopen(FSTAB_PATH, "r");
    if (!in)
        die("cannot read fstab");
    out = fopen(tmppath, "w");
    if (!out) {
        fclose(in);
        die("cannot write fstab temp file");
    }
    while (fgets(line, sizeof(line), in)) {
        if (line[0] != '#' &&
            sscanf(line, "%255s %255s", dev, mnt) == 2 &&
            strcmp(mnt, mountpoint) == 0)
            continue;
        fputs(line, out);
    }
    fclose(in);
    if (fclose(out) != 0)
        die("fstab write failed");
    if (chmod(tmppath, 0644) != 0 || rename(tmppath, FSTAB_PATH) != 0)
        die("fstab rename failed");
}

/* volcreate <label> <disk1,disk2,...>:
 * newfs_hammer2 across all disks, mount at /data/<label>, add fstab */
static int do_volcreate(const char *label, const char *disklist)
{
    char disks[MAX_VOL_DISKS][32];
    char mountpoint[256], special[768], dev[64];
    char *newfs_args[MAX_VOL_DISKS + 5];
    char devbufs[MAX_VOL_DISKS][64];
    int count, i, rc;
    FILE *f;

    count = parse_disks(disklist, disks);
    for (i = 0; i < count; i++) {
        if (disk_in_use(disks[i])) {
            fprintf(stderr, "flynas-helper: disk %s is in use\n", disks[i]);
            exit(1);
        }
    }

    snprintf(mountpoint, sizeof(mountpoint), "%s/%s", DATA_ROOT, label);
    if (fstab_has_mountpoint(mountpoint))
        die("volume already exists");

    /* newfs_hammer2 -L <label> /dev/d1 [/dev/d2 ...] */
    int a = 0;
    newfs_args[a++] = "newfs_hammer2";
    newfs_args[a++] = "-L";
    newfs_args[a++] = (char *)label;
    for (i = 0; i < count; i++) {
        snprintf(devbufs[i], sizeof(devbufs[i]), "/dev/%s", disks[i]);
        newfs_args[a++] = devbufs[i];
    }
    newfs_args[a] = NULL;
    rc = run(NEWFS_HAMMER2_CMD, newfs_args);
    if (rc != 0)
        die("newfs_hammer2 failed");

    /* special = /dev/d1[:/dev/d2...]@label */
    special[0] = '\0';
    for (i = 0; i < count; i++) {
        snprintf(dev, sizeof(dev), "%s/dev/%s", i ? ":" : "", disks[i]);
        strlcat(special, dev, sizeof(special));
    }
    strlcat(special, "@", sizeof(special));
    strlcat(special, label, sizeof(special));

    mkdir(DATA_ROOT, 0755);
    if (mkdir(mountpoint, 0755) != 0 && access(mountpoint, F_OK) != 0)
        die("cannot create mountpoint");

    char *mount_args[] = { "mount_hammer2", special, mountpoint, NULL };
    rc = run(MOUNT_HAMMER2_CMD, mount_args);
    if (rc != 0)
        die("mount_hammer2 failed");

    f = fopen(FSTAB_PATH, "a");
    if (!f)
        die("cannot append to fstab");
    fprintf(f, "%s\t%s\thammer2\trw\t0\t0\n", special, mountpoint);
    fclose(f);

    printf("%s\n", mountpoint);
    return 0;
}

/* voldestroy <label>: umount /data/<label>, drop fstab entry.
 * Data on the disks is not wiped; a later volcreate reformats. */
static int do_voldestroy(const char *label)
{
    char mountpoint[256];
    int rc;

    snprintf(mountpoint, sizeof(mountpoint), "%s/%s", DATA_ROOT, label);

    char *umount_args[] = { "umount", mountpoint, NULL };
    rc = run(UMOUNT_CMD, umount_args);
    /* Tolerate "not mounted", fail on busy: umount exits 1 either
     * way, so check whether it is still mounted afterwards */
    struct statfs sb;
    if (statfs(mountpoint, &sb) == 0 &&
        strcmp(sb.f_mntonname, mountpoint) == 0)
        die("umount failed (volume busy?)");
    (void)rc;

    fstab_remove_mountpoint(mountpoint);
    rmdir(mountpoint);
    return 0;
}

/* ---- Snapshots ------------------------------------------------ */

/* Snapshot PFS names are forced to a "snap-" prefix so snapdelete
 * can never address a volume's root PFS. */
static int valid_snapname(const char *s)
{
    size_t len;

    if (!s || strncmp(s, "snap-", 5) != 0)
        return 0;
    len = strlen(s);
    if (len < 6 || len > 64)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) ||
              c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static void volume_mountpoint(const char *label, char *buf, size_t size)
{
    struct statfs sb;

    snprintf(buf, size, "%s/%s", DATA_ROOT, label);
    if (statfs(buf, &sb) != 0 || strcmp(sb.f_mntonname, buf) != 0)
        die("volume not mounted");
}

static int do_snapcreate(const char *label, const char *snapname)
{
    char mountpoint[256];

    volume_mountpoint(label, mountpoint, sizeof(mountpoint));
    char *args[] = { "hammer2", "snapshot", mountpoint, (char *)snapname, NULL };
    return run(HAMMER2_CMD, args);
}

static int do_snapdelete(const char *label, const char *snapname)
{
    char mountpoint[256];

    volume_mountpoint(label, mountpoint, sizeof(mountpoint));
    char *args[] = { "hammer2", "-s", mountpoint, "pfs-delete",
                     (char *)snapname, NULL };
    return run(HAMMER2_CMD, args);
}

static int do_snaplist(const char *label)
{
    char mountpoint[256];

    volume_mountpoint(label, mountpoint, sizeof(mountpoint));
    char *args[] = { "hammer2", "-s", mountpoint, "pfs-list", NULL };
    return run(HAMMER2_CMD, args);
}

/* backupsync: detach scripts/backup-sync.sh as root. The job spec
 * is read by the script from RUN_DIR/backup-job.json; concurrency
 * is the script's problem (status-file lock). */
#define BACKUP_SCRIPT "/usr/local/flynas/scripts/backup-sync.sh"

static int do_backupsync(void)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork failed");
    if (pid == 0) {
        setsid();
        /* Detach stdio so nginx's pipe read returns immediately */
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        char *args[] = { "sh", BACKUP_SCRIPT, NULL };
        execv("/bin/sh", args);
        _exit(127);
    }
    return 0;
}

/* ---- Network / time configuration ---------------------------- */

/* Validate interface name: ^[a-z]+[0-9]+$ (vtnet0, em0, re0) */
static int valid_iface(const char *s)
{
    return valid_disk(s);
}

static int valid_ipv4(const char *s)
{
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1;
}

/* Hostname for NTP: letters, digits, dot, dash */
static int valid_hostname(const char *s)
{
    size_t len;

    if (!s || !*s)
        return 0;
    len = strlen(s);
    if (len > 255)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

/* Replace every "key=..." line in rc.conf with key="value" (or drop
 * it when value is NULL); appends if absent. Atomic via rename.
 * rc.conf accumulates duplicates (installer + harness) — all
 * occurrences are removed so the appended line wins. */
static void rcconf_set(const char *key, const char *value)
{
    FILE *in, *out;
    char line[1024];
    char tmppath[] = RCCONF_PATH ".flynas.tmp";
    size_t keylen = strlen(key);

    in = fopen(RCCONF_PATH, "r");
    if (!in)
        die("cannot read rc.conf");
    out = fopen(tmppath, "w");
    if (!out) {
        fclose(in);
        die("cannot write rc.conf temp file");
    }
    while (fgets(line, sizeof(line), in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=')
            continue;
        fputs(line, out);
    }
    if (value)
        fprintf(out, "%s=\"%s\"\n", key, value);
    fclose(in);
    if (fclose(out) != 0)
        die("rc.conf write failed");
    if (chmod(tmppath, 0644) != 0 || rename(tmppath, RCCONF_PATH) != 0)
        die("rc.conf rename failed");
}

static int run_service(const char *name, const char *action)
{
    char *args[] = { "service", (char *)name, (char *)action, NULL };
    return run(SERVICE_CMD, args);
}

/* netconfig <iface> dhcp
 * netconfig <iface> static <ip> <netmask> <gateway>
 * Writes rc.conf and restarts netif (+ routing for static). The
 * caller's HTTP connection may drop if the address changes. */
static int do_netconfig(int argc, char *argv[])
{
    const char *iface = argv[2];
    const char *mode = argv[3];
    char key[64], value[128];
    int rc;

    if (!valid_iface(iface))
        die("invalid interface name");
    snprintf(key, sizeof(key), "ifconfig_%s", iface);

    if (strcmp(mode, "dhcp") == 0) {
        if (argc != 4)
            die("usage: flynas-helper netconfig <iface> dhcp");
        rcconf_set(key, "DHCP");
        rcconf_set("defaultrouter", NULL);  /* DHCP supplies the route */
    } else if (strcmp(mode, "static") == 0) {
        if (argc != 7)
            die("usage: flynas-helper netconfig <iface> static <ip> <netmask> <gateway>");
        if (!valid_ipv4(argv[4]) || !valid_ipv4(argv[5]) || !valid_ipv4(argv[6]))
            die("invalid IPv4 address");
        snprintf(value, sizeof(value), "inet %s netmask %s", argv[4], argv[5]);
        rcconf_set(key, value);
        rcconf_set("defaultrouter", argv[6]);
    } else {
        die("netconfig: unknown mode (use dhcp or static)");
    }

    rc = run_service("netif", "restart");
    if (rc != 0)
        die("netif restart failed");
    /* routing restart installs/clears the static default route;
     * under DHCP dhclient handles it */
    if (strcmp(mode, "static") == 0) {
        rc = run_service("routing", "restart");
        if (rc != 0)
            die("routing restart failed");
    }
    return 0;
}

/* timezone <zone>: copy /usr/share/zoneinfo/<zone> to /etc/localtime
 * and record the name in /var/db/zoneinfo (like tzsetup) */
static int do_timezone(const char *zone)
{
    char src[512], buf[8192];
    struct stat st;
    FILE *in, *out;
    size_t n;

    if (!zone || !*zone || strlen(zone) > 64 || zone[0] == '/' ||
        strstr(zone, ".."))
        die("invalid timezone");
    for (const char *p = zone; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '/' || *p == '_' ||
              *p == '-' || *p == '+'))
            die("invalid timezone");
    }

    snprintf(src, sizeof(src), "%s/%s", ZONEINFO_DIR, zone);
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
        die("unknown timezone");

    in = fopen(src, "r");
    if (!in)
        die("cannot read zoneinfo file");
    out = fopen(LOCALTIME_PATH ".flynas.tmp", "w");
    if (!out) {
        fclose(in);
        die("cannot write localtime temp file");
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            die("localtime write failed");
        }
    }
    fclose(in);
    if (fclose(out) != 0)
        die("localtime write failed");
    if (chmod(LOCALTIME_PATH ".flynas.tmp", 0644) != 0 ||
        rename(LOCALTIME_PATH ".flynas.tmp", LOCALTIME_PATH) != 0)
        die("localtime rename failed");

    out = fopen(ZONEDB_PATH, "w");
    if (!out)
        die("cannot write " ZONEDB_PATH);
    fprintf(out, "%s\n", zone);
    fclose(out);
    return 0;
}

/* ntp <server>: enable dntpd against the given server
 * ntp off: disable dntpd */
static int do_ntp(const char *server)
{
    if (strcmp(server, "off") == 0) {
        rcconf_set("dntpd_enable", "NO");
        /* onestop: plain stop refuses once dntpd_enable=NO */
        run_service("dntpd", "onestop");
        return 0;
    }
    if (!valid_hostname(server))
        die("invalid NTP server");
    rcconf_set("dntpd_enable", "YES");
    rcconf_set("dntpd_flags", server);
    if (run_service("dntpd", "restart") != 0)
        die("dntpd restart failed");
    return 0;
}

/* ---- Virtual machines (QEMU + NVMM) -------------------------- */

#define QEMU_CMD "/usr/local/bin/qemu-system-x86_64"
#define QEMU_IMG_CMD "/usr/local/bin/qemu-img"
#define KLDLOAD_CMD "/sbin/kldload"
#define IFCONFIG_CMD "/sbin/ifconfig"
#define VM_RUN_DIR "/var/run/flynas"
#define VM_SYS_DIR "/usr/local/flynas/vms"
#define VM_BRIDGE "flynas0"

static int iface_exists(const char *name);   /* defined with the bridge ops */

/* unsigned integer in [min,max] */
static int valid_uint(const char *s, long min, long max)
{
    char *end;
    long v;

    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    v = strtol(s, &end, 10);
    return (*end == '\0' && v >= min && v <= max);
}

/* MAC: xx:xx:xx:xx:xx:xx (hex) */
static int valid_mac(const char *s)
{
    if (!s || strlen(s) != 17)
        return 0;
    for (int i = 0; i < 17; i++) {
        if ((i % 3) == 2) {
            if (s[i] != ':')
                return 0;
        } else if (!isxdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

/* tap name: ^tap[0-9]+$ */
static int valid_tap(const char *s)
{
    if (!s || strncmp(s, "tap", 3) != 0 || !s[3])
        return 0;
    for (const char *p = s + 3; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    return 1;
}

/* Path must live under /data/ or the system VM dir, no traversal,
 * restricted charset. Used for disk images and ISOs. */
static int valid_vmpath(const char *p)
{
    size_t len;

    if (!p || !*p)
        return 0;
    len = strlen(p);
    if (len >= 512 || strstr(p, ".."))
        return 0;
    if (strncmp(p, "/data/", 6) != 0 &&
        strncmp(p, VM_SYS_DIR "/", strlen(VM_SYS_DIR) + 1) != 0)
        return 0;
    for (const char *c = p; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '/' || *c == '.' ||
              *c == '_' || *c == '-'))
            return 0;
    }
    return 1;
}

/* Build the disk image path for <name> on <volume> ("-" = system dir).
 * Verifies a data volume is actually mounted. */
static void vm_image_path(const char *name, const char *volume,
                          char *buf, size_t size)
{
    if (strcmp(volume, "-") == 0) {
        snprintf(buf, size, "%s/%s.img", VM_SYS_DIR, name);
    } else {
        char mp[256];
        struct statfs sb;
        if (!valid_name(volume))
            die("invalid volume");
        snprintf(mp, sizeof(mp), "%s/%s", DATA_ROOT, volume);
        if (statfs(mp, &sb) != 0 || strcmp(sb.f_mntonname, mp) != 0)
            die("volume not mounted");
        snprintf(buf, size, "%s/%s/vms/%s.img", DATA_ROOT, volume, name);
    }
}

static void vm_paths(const char *name, char *sock, char *serial, char *pid,
                     size_t size)
{
    snprintf(sock, size, "%s/vm-%s.sock", VM_RUN_DIR, name);
    snprintf(serial, size, "%s/vm-%s.serial", VM_RUN_DIR, name);
    snprintf(pid, size, "%s/vm-%s.pid", VM_RUN_DIR, name);
}

/* vmcreate <name> <gb> <volume|->: allocate a raw disk image */
static int do_vmcreate(const char *name, const char *gb, const char *volume)
{
    char path[512], dir[512], size_arg[32];
    char *p;

    if (!valid_uint(gb, 1, 4096))
        die("invalid disk size (GB)");
    vm_image_path(name, volume, path, sizeof(path));

    /* mkdir -p the parent directory */
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    p = strrchr(dir, '/');
    if (p) {
        *p = '\0';
        mkdir(DATA_ROOT, 0755);   /* harmless if it exists */
        mkdir(dir, 0755);
    }
    if (access(path, F_OK) == 0)
        die("disk image already exists");

    snprintf(size_arg, sizeof(size_arg), "%sG", gb);
    char *args[] = { "qemu-img", "create", "-f", "raw", path, size_arg, NULL };
    if (run(QEMU_IMG_CMD, args) != 0)
        die("qemu-img create failed");
    printf("%s\n", path);
    return 0;
}

/* vmstart <name> <cpus> <ram_mb> <imagepath> <tap> <mac|-> <iso|->:
 * load nvmm, bring up the tap, launch QEMU daemonized under NVMM. */
static int do_vmstart(char *argv[])
{
    const char *name = argv[2], *cpus = argv[3], *ram = argv[4];
    const char *image = argv[5], *tap = argv[6], *mac = argv[7], *iso = argv[8];
    char sock[256], serial[256], pid[256];
    char drivebuf[600], netbuf[128], devbuf[128];
    char qmpbuf[300], serbuf[300];
    struct stat st;

    if (!valid_name(name)) die("invalid vm name");
    if (!valid_uint(cpus, 1, 256)) die("invalid cpus");
    if (!valid_uint(ram, 64, 1048576)) die("invalid ram");
    if (!valid_vmpath(image) || stat(image, &st) != 0 || !S_ISREG(st.st_mode))
        die("invalid disk image");
    if (!valid_tap(tap)) die("invalid tap name");
    if (strcmp(mac, "-") != 0 && !valid_mac(mac)) die("invalid mac");
    if (strcmp(iso, "-") != 0 &&
        (!valid_vmpath(iso) || stat(iso, &st) != 0 || !S_ISREG(st.st_mode)))
        die("invalid iso path");

    /* Run dir setgid-flynas + umask 007 so QEMU's QMP/serial unix
     * sockets come out group-writable (mode 0770 root:flynas). nginx
     * (www, in flynas) needs *write* on the socket file to connect(). */
    mkdir(VM_RUN_DIR, 0770);
    {
        struct group *g = getgrnam("flynas");
        if (g) chown(VM_RUN_DIR, 0, g->gr_gid);
        chmod(VM_RUN_DIR, 02770);
    }
    umask(007);
    vm_paths(name, sock, serial, pid, sizeof(sock));

    /* Already running? */
    if (access(pid, F_OK) == 0) {
        FILE *pf = fopen(pid, "r");
        long oldpid = 0;
        if (pf) { if (fscanf(pf, "%ld", &oldpid) != 1) oldpid = 0; fclose(pf); }
        if (oldpid > 0 && kill((pid_t)oldpid, 0) == 0)
            die("vm already running");
    }

    /* NVMM accelerator (idempotent; ignore "already loaded") */
    char *kld[] = { "kldload", "nvmm", NULL };
    run(KLDLOAD_CMD, kld);

    /* Bring up the tap (create may fail if it exists — tolerate) */
    char *tc[] = { "ifconfig", (char *)tap, "create", NULL };
    run(IFCONFIG_CMD, tc);
    char *tu[] = { "ifconfig", (char *)tap, "up", NULL };
    run(IFCONFIG_CMD, tu);

    /* Join the NAT bridge if it's been set up (else tap stays
     * standalone — VM runs but has no network). */
    if (iface_exists(VM_BRIDGE)) {
        char *addm[] = { "ifconfig", VM_BRIDGE, "addm", (char *)tap, NULL };
        run(IFCONFIG_CMD, addm);
    }

    snprintf(drivebuf, sizeof(drivebuf),
        "file=%s,format=raw,if=virtio", image);
    snprintf(netbuf, sizeof(netbuf),
        "tap,id=net0,ifname=%s,script=no,downscript=no", tap);
    if (strcmp(mac, "-") == 0)
        snprintf(devbuf, sizeof(devbuf), "virtio-net-pci,netdev=net0");
    else
        snprintf(devbuf, sizeof(devbuf), "virtio-net-pci,netdev=net0,mac=%s", mac);
    snprintf(qmpbuf, sizeof(qmpbuf), "unix:%s,server,nowait", sock);
    snprintf(serbuf, sizeof(serbuf), "unix:%s,server,nowait", serial);

    char *a[40];
    int n = 0;
    a[n++] = "qemu-system-x86_64";
    a[n++] = "-machine"; a[n++] = "type=q35,accel=nvmm";
    a[n++] = "-m"; a[n++] = (char *)ram;
    a[n++] = "-smp"; a[n++] = (char *)cpus;
    a[n++] = "-drive"; a[n++] = drivebuf;
    a[n++] = "-netdev"; a[n++] = netbuf;
    a[n++] = "-device"; a[n++] = devbuf;
    if (strcmp(iso, "-") != 0) {
        a[n++] = "-cdrom"; a[n++] = (char *)iso;
        a[n++] = "-boot"; a[n++] = "d";
    }
    a[n++] = "-qmp"; a[n++] = qmpbuf;
    a[n++] = "-serial"; a[n++] = serbuf;
    a[n++] = "-vga"; a[n++] = "none";
    a[n++] = "-display"; a[n++] = "none";
    a[n++] = "-daemonize"; a[n++] = "-pidfile"; a[n++] = pid;
    a[n] = NULL;

    if (run(QEMU_CMD, a) != 0)
        die("qemu launch failed");

    /* QEMU creates the QMP/serial unix sockets 0750 regardless of
     * umask. Wait for them, then widen to 0770 so nginx (www, flynas
     * group) can connect() — connect needs write on the socket. */
    for (int i = 0; i < 30 && access(sock, F_OK) != 0; i++)
        usleep(100000);
    chmod(sock, 0770);
    chmod(serial, 0770);
    return 0;
}

/* vmstop <name> <tap>: force-stop a VM (graceful ACPI powerdown is
 * attempted by the API over QMP first) and tear down its tap/sockets. */
static int do_vmstop(const char *name, const char *tap)
{
    char sock[256], serial[256], pid[256];
    FILE *pf;
    long vmpid = 0;

    if (!valid_name(name)) die("invalid vm name");
    if (!valid_tap(tap)) die("invalid tap name");
    vm_paths(name, sock, serial, pid, sizeof(sock));

    pf = fopen(pid, "r");
    if (pf) {
        if (fscanf(pf, "%ld", &vmpid) != 1) vmpid = 0;
        fclose(pf);
    }
    if (vmpid > 0 && kill((pid_t)vmpid, 0) == 0) {
        kill((pid_t)vmpid, SIGTERM);
        for (int i = 0; i < 30 && kill((pid_t)vmpid, 0) == 0; i++)
            usleep(100000);          /* up to 3s for a clean exit */
        if (kill((pid_t)vmpid, 0) == 0)
            kill((pid_t)vmpid, SIGKILL);
    }

    char *td[] = { "ifconfig", (char *)tap, "destroy", NULL };
    run(IFCONFIG_CMD, td);
    unlink(sock);
    unlink(serial);
    unlink(pid);
    return 0;
}

/* vmdelete <name> <volume|->: remove the disk image (VM must be stopped) */
static int do_vmdelete(const char *name, const char *volume)
{
    char path[512], pid[256], sock[256], serial[256];

    if (!valid_name(name)) die("invalid vm name");
    vm_paths(name, sock, serial, pid, sizeof(sock));
    if (access(pid, F_OK) == 0) {
        FILE *pf = fopen(pid, "r");
        long vmpid = 0;
        if (pf) { if (fscanf(pf, "%ld", &vmpid) != 1) vmpid = 0; fclose(pf); }
        if (vmpid > 0 && kill((pid_t)vmpid, 0) == 0)
            die("vm is running");
    }
    vm_image_path(name, volume, path, sizeof(path));
    if (access(path, F_OK) == 0 && unlink(path) != 0)
        die("failed to remove disk image");
    return 0;
}

/* ---- VM NAT network (internal bridge + pf NAT) --------------- */
/* flynas0 is a host-internal bridge; the host is the guests' gateway
 * at 10.77.0.1/24, pf NATs them out the uplink, and (being on the
 * same subnet) the host can probe guests directly for monitoring.
 * This does NOT touch the management interface. */

#define VM_GW_CIDR "10.77.0.1/24"
#define VM_SUBNET "10.77.0.0/24"
#define PFCTL_CMD "/usr/sbin/pfctl"
#define SYSCTL_CMD "/sbin/sysctl"
#define PF_CONF VM_RUN_DIR "/pf.conf"
#define DNSMASQ_CMD "/usr/local/sbin/dnsmasq"
#define DNSMASQ_CONF "/usr/local/flynas/conf/dnsmasq.conf"
#define DHCP_HOSTS VM_RUN_DIR "/dhcp-hosts"
#define DNSMASQ_PID VM_RUN_DIR "/dnsmasq.pid"
#define PF_FWD_SPEC VM_RUN_DIR "/fwd-spec"
#define PF_FWD_ANCHOR "flynas-fwd"
#define VM_NET_PREFIX "10.77.0."

/* Read a pid from a file; 0 if absent/unreadable. */
static long read_pidfile(const char *path)
{
    FILE *f = fopen(path, "r");
    long pid = 0;
    if (f) {
        if (fscanf(f, "%ld", &pid) != 1) pid = 0;
        fclose(f);
    }
    return pid;
}

static void start_dnsmasq(void)
{
    long pid = read_pidfile(DNSMASQ_PID);
    if (pid > 0 && kill((pid_t)pid, 0) == 0)
        return;   /* already running */
    /* hostsfile must exist before dnsmasq starts */
    if (access(DHCP_HOSTS, F_OK) != 0) {
        FILE *f = fopen(DHCP_HOSTS, "w");
        if (f) fclose(f);
    }
    char *args[] = { "dnsmasq", "-C", DNSMASQ_CONF, NULL };
    run(DNSMASQ_CMD, args);   /* dnsmasq daemonizes itself */
}

static void stop_dnsmasq(void)
{
    long pid = read_pidfile(DNSMASQ_PID);
    if (pid > 0 && kill((pid_t)pid, 0) == 0)
        kill((pid_t)pid, SIGTERM);
    unlink(DNSMASQ_PID);
}

static int iface_exists(const char *name)
{
    char *args[] = { "ifconfig", (char *)name, NULL };
    return run(IFCONFIG_CMD, args) == 0;
}

/* netbridge up <uplink>: ensure flynas0, forwarding, and pf NAT */
static int do_netbridge_up(const char *uplink)
{
    FILE *f;

    if (!valid_iface(uplink))
        die("invalid uplink interface");

    char *kld_bridge[] = { "kldload", "if_bridge", NULL };
    run(KLDLOAD_CMD, kld_bridge);
    char *kld_pf[] = { "kldload", "pf", NULL };
    run(KLDLOAD_CMD, kld_pf);

    if (!iface_exists(VM_BRIDGE)) {
        char *mk[] = { "ifconfig", "bridge", "create", "name", VM_BRIDGE, NULL };
        if (run(IFCONFIG_CMD, mk) != 0)
            die("failed to create " VM_BRIDGE);
    }
    char *ip[] = { "ifconfig", VM_BRIDGE, "inet", VM_GW_CIDR, "alias", NULL };
    run(IFCONFIG_CMD, ip);   /* alias: idempotent if already set */
    char *up[] = { "ifconfig", VM_BRIDGE, "up", NULL };
    run(IFCONFIG_CMD, up);

    char *fwd[] = { "sysctl", "net.inet.ip.forwarding=1", NULL };
    if (run(SYSCTL_CMD, fwd) != 0)
        die("failed to enable ip forwarding");

    /* Permissive ruleset (pass all = no filtering, just NAT + a
     * port-forward anchor) so enabling pf can't lock out management. */
    mkdir(VM_RUN_DIR, 0770);
    f = fopen(PF_CONF, "w");
    if (!f)
        die("cannot write pf.conf");
    fprintf(f,
        "ext_if = \"%s\"\n"
        "nat on $ext_if from %s to any -> ($ext_if)\n"
        "rdr-anchor \"flynas-fwd\"\n"
        "pass all\n",
        uplink, VM_SUBNET);
    fclose(f);

    char *en[] = { "pfctl", "-e", NULL };
    run(PFCTL_CMD, en);      /* ignore "already enabled" */
    char *load[] = { "pfctl", "-f", PF_CONF, NULL };
    if (run(PFCTL_CMD, load) != 0)
        die("pfctl -f failed");

    start_dnsmasq();
    return 0;
}

/* dhcpreload: SIGHUP dnsmasq so it re-reads the reservations file */
static int do_dhcpreload(void)
{
    long pid = read_pidfile(DNSMASQ_PID);
    if (pid > 0 && kill((pid_t)pid, 0) == 0)
        kill((pid_t)pid, SIGHUP);
    return 0;
}

/* pffwd <uplink>: rebuild the flynas-fwd rdr anchor from PF_FWD_SPEC,
 * a www-written CSV of "proto,hostport,guestip,guestport" lines. Each
 * field is validated here (www must not inject raw pf rules). */
static int do_pffwd(const char *uplink)
{
    FILE *in, *out;
    char line[256];
    char tmp[] = VM_RUN_DIR "/fwd.rules";

    if (!valid_iface(uplink))
        die("invalid uplink");

    out = fopen(tmp, "w");
    if (!out)
        die("cannot write rules file");

    in = fopen(PF_FWD_SPEC, "r");
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char proto[8], gip[32];
            int hport, gport;
            line[strcspn(line, "\n")] = '\0';
            if (line[0] == '\0')
                continue;
            if (sscanf(line, "%7[a-z],%d,%31[0-9.],%d",
                       proto, &hport, gip, &gport) != 4) {
                fclose(in); fclose(out);
                die("malformed forward spec");
            }
            if (strcmp(proto, "tcp") != 0 && strcmp(proto, "udp") != 0) {
                fclose(in); fclose(out);
                die("forward proto must be tcp or udp");
            }
            if (hport < 1 || hport > 65535 || gport < 1 || gport > 65535) {
                fclose(in); fclose(out);
                die("forward port out of range");
            }
            if (strncmp(gip, VM_NET_PREFIX, strlen(VM_NET_PREFIX)) != 0 ||
                !valid_ipv4(gip)) {
                fclose(in); fclose(out);
                die("forward target must be a 10.77.0.x address");
            }
            fprintf(out,
                "rdr pass on %s inet proto %s to port %d -> %s port %d\n",
                uplink, proto, hport, gip, gport);
        }
        fclose(in);
    }
    fclose(out);

    /* Load (or clear) the anchor. -f on an empty file flushes it. */
    char *args[] = { "pfctl", "-a", PF_FWD_ANCHOR, "-f", tmp, NULL };
    if (run(PFCTL_CMD, args) != 0)
        die("pfctl anchor load failed");
    return 0;
}

/* netbridge down: tear the NAT network down */
static int do_netbridge_down(void)
{
    stop_dnsmasq();
    char *dis[] = { "pfctl", "-d", NULL };
    run(PFCTL_CMD, dis);
    if (iface_exists(VM_BRIDGE)) {
        char *rm[] = { "ifconfig", VM_BRIDGE, "destroy", NULL };
        run(IFCONFIG_CMD, rm);
    }
    char *fwd[] = { "sysctl", "net.inet.ip.forwarding=0", NULL };
    run(SYSCTL_CMD, fwd);
    return 0;
}

int main(int argc, char *argv[])
{
    int rc;

    if (argc < 2)
        die("usage: flynas-helper <command> [args...]");

    /* Ensure we're running as root (setuid should handle this) */
    if (geteuid() != 0)
        die("must be run as root (setuid)");

    /* Clear environment for safety */
    clearenv();
    setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin", 1);

    /* Drop inherited descriptors (nginx listen sockets!) so daemons
     * spawned below (dhclient, dntpd, backup-sync.sh) can't hold
     * ports 80/443 open after an nginx restart */
    closefrom(3);

    const char *cmd = argv[1];

    if (strcmp(cmd, "useradd") == 0) {
        if (argc != 4)
            die("usage: flynas-helper useradd <username> <shell>");
        if (!valid_name(argv[2]))
            die("invalid username");
        if (!valid_shell(argv[3]))
            die("invalid shell");
        char *args[] = { "pw", "useradd", argv[2], "-m", "-s", argv[3], NULL };
        rc = run(PW_CMD, args);

    } else if (strcmp(cmd, "userdel") == 0) {
        if (argc != 3)
            die("usage: flynas-helper userdel <username>");
        if (!valid_name(argv[2]))
            die("invalid username");
        char *args[] = { "pw", "userdel", argv[2], "-r", NULL };
        rc = run(PW_CMD, args);

    } else if (strcmp(cmd, "usermod") == 0) {
        if (argc != 5)
            die("usage: flynas-helper usermod <username> shell|groups <value>");
        if (!valid_name(argv[2]))
            die("invalid username");

        if (strcmp(argv[3], "shell") == 0) {
            if (!valid_shell(argv[4]))
                die("invalid shell");
            char *args[] = { "pw", "usermod", argv[2], "-s", argv[4], NULL };
            rc = run(PW_CMD, args);
        } else if (strcmp(argv[3], "groups") == 0) {
            if (!valid_name_list(argv[4]))
                die("invalid group list");
            char *args[] = { "pw", "usermod", argv[2], "-G", argv[4], NULL };
            rc = run(PW_CMD, args);
        } else {
            die("usermod: unknown subcommand (use shell or groups)");
        }

    } else if (strcmp(cmd, "passwd") == 0) {
        if (argc != 3)
            die("usage: flynas-helper passwd <username>");
        if (!valid_name(argv[2]))
            die("invalid username");
        rc = do_passwd(argv[2]);

    } else if (strcmp(cmd, "groupadd") == 0) {
        if (argc != 3)
            die("usage: flynas-helper groupadd <name>");
        if (!valid_name(argv[2]))
            die("invalid group name");
        char *args[] = { "pw", "groupadd", argv[2], NULL };
        rc = run(PW_CMD, args);

    } else if (strcmp(cmd, "groupdel") == 0) {
        if (argc != 3)
            die("usage: flynas-helper groupdel <name>");
        if (!valid_name(argv[2]))
            die("invalid group name");
        char *args[] = { "pw", "groupdel", argv[2], NULL };
        rc = run(PW_CMD, args);

    } else if (strcmp(cmd, "groupmod") == 0) {
        if (argc != 4)
            die("usage: flynas-helper groupmod <oldname> <newname>");
        if (!valid_name(argv[2]))
            die("invalid old group name");
        if (!valid_name(argv[3]))
            die("invalid new group name");
        char *args[] = { "pw", "groupmod", argv[2], "-n", argv[3], NULL };
        rc = run(PW_CMD, args);

    } else if (strcmp(cmd, "groupmembers") == 0) {
        if (argc != 4)
            die("usage: flynas-helper groupmembers <name> <user1,user2,...>");
        if (!valid_name(argv[2]))
            die("invalid group name");
        if (!valid_name_list(argv[3]))
            die("invalid member list");
        char *args[] = { "pw", "groupmod", argv[2], "-M", argv[3], NULL };
        rc = run(PW_CMD, args);

    } else if (strcmp(cmd, "sshkeys") == 0) {
        if (argc != 4)
            die("usage: flynas-helper sshkeys <username> write|remove");
        if (!valid_name(argv[2]))
            die("invalid username");
        if (strcmp(argv[3], "write") == 0) {
            rc = do_sshkeys_write(argv[2]);
        } else if (strcmp(argv[3], "remove") == 0) {
            rc = do_sshkeys_remove(argv[2]);
        } else {
            die("sshkeys: unknown subcommand (use write or remove)");
        }

    } else if (strcmp(cmd, "smart") == 0) {
        /* Read-only SMART info+health; -d sat works around ahci bug #1412 */
        if (argc != 3)
            die("usage: flynas-helper smart <disk>");
        if (!valid_disk(argv[2]))
            die("invalid disk name");
        char dev[64];
        snprintf(dev, sizeof(dev), "/dev/%s", argv[2]);
        char *args[] = { "smartctl", "-d", "sat", "-i", "-H", dev, NULL };
        rc = run(SMARTCTL_CMD, args);

    } else if (strcmp(cmd, "diskinfo") == 0) {
        /* Read-only geometry summary (fdisk -s prints "N cyl M hd K sec") */
        if (argc != 3)
            die("usage: flynas-helper diskinfo <disk>");
        if (!valid_disk(argv[2]))
            die("invalid disk name");
        char dev[64];
        snprintf(dev, sizeof(dev), "/dev/%s", argv[2]);
        char *args[] = { "fdisk", "-s", dev, NULL };
        rc = run(FDISK_CMD, args);

    } else if (strcmp(cmd, "volcreate") == 0) {
        if (argc != 4)
            die("usage: flynas-helper volcreate <label> <disk1,disk2,...>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        rc = do_volcreate(argv[2], argv[3]);

    } else if (strcmp(cmd, "voldestroy") == 0) {
        if (argc != 3)
            die("usage: flynas-helper voldestroy <label>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        rc = do_voldestroy(argv[2]);

    } else if (strcmp(cmd, "scrub") == 0) {
        if (argc != 3)
            die("usage: flynas-helper scrub <label>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        char mountpoint[256];
        snprintf(mountpoint, sizeof(mountpoint), "%s/%s", DATA_ROOT, argv[2]);
        char *args[] = { "hammer2", "bulkfree", mountpoint, NULL };
        rc = run(HAMMER2_CMD, args);

    } else if (strcmp(cmd, "vollist") == 0) {
        /* Read-only: devices backing a mounted volume */
        if (argc != 3)
            die("usage: flynas-helper vollist <label>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        char mountpoint[256];
        snprintf(mountpoint, sizeof(mountpoint), "%s/%s", DATA_ROOT, argv[2]);
        char *args[] = { "hammer2", "-s", mountpoint, "volume-list", NULL };
        rc = run(HAMMER2_CMD, args);

    } else if (strcmp(cmd, "snapcreate") == 0) {
        if (argc != 4)
            die("usage: flynas-helper snapcreate <label> <snapname>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        if (!valid_snapname(argv[3]))
            die("invalid snapshot name");
        rc = do_snapcreate(argv[2], argv[3]);

    } else if (strcmp(cmd, "snapdelete") == 0) {
        if (argc != 4)
            die("usage: flynas-helper snapdelete <label> <snapname>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        if (!valid_snapname(argv[3]))
            die("invalid snapshot name");
        rc = do_snapdelete(argv[2], argv[3]);

    } else if (strcmp(cmd, "snaplist") == 0) {
        /* Read-only: PFS list including snapshots */
        if (argc != 3)
            die("usage: flynas-helper snaplist <label>");
        if (!valid_name(argv[2]))
            die("invalid volume label");
        rc = do_snaplist(argv[2]);

    } else if (strcmp(cmd, "backupsync") == 0) {
        if (argc != 2)
            die("usage: flynas-helper backupsync");
        rc = do_backupsync();

    } else if (strcmp(cmd, "netconfig") == 0) {
        if (argc < 4)
            die("usage: flynas-helper netconfig <iface> dhcp|static ...");
        rc = do_netconfig(argc, argv);

    } else if (strcmp(cmd, "timezone") == 0) {
        if (argc != 3)
            die("usage: flynas-helper timezone <zone>");
        rc = do_timezone(argv[2]);

    } else if (strcmp(cmd, "ntp") == 0) {
        if (argc != 3)
            die("usage: flynas-helper ntp <server>|off");
        rc = do_ntp(argv[2]);

    } else if (strcmp(cmd, "vmcreate") == 0) {
        if (argc != 5)
            die("usage: flynas-helper vmcreate <name> <gb> <volume|->");
        if (!valid_name(argv[2]))
            die("invalid vm name");
        rc = do_vmcreate(argv[2], argv[3], argv[4]);

    } else if (strcmp(cmd, "vmstart") == 0) {
        if (argc != 9)
            die("usage: flynas-helper vmstart <name> <cpus> <ram_mb> <image> <tap> <mac|-> <iso|->");
        rc = do_vmstart(argv);

    } else if (strcmp(cmd, "vmstop") == 0) {
        if (argc != 4)
            die("usage: flynas-helper vmstop <name> <tap>");
        rc = do_vmstop(argv[2], argv[3]);

    } else if (strcmp(cmd, "vmdelete") == 0) {
        if (argc != 4)
            die("usage: flynas-helper vmdelete <name> <volume|->");
        rc = do_vmdelete(argv[2], argv[3]);

    } else if (strcmp(cmd, "netbridge") == 0) {
        if (argc == 3 && strcmp(argv[2], "down") == 0) {
            rc = do_netbridge_down();
        } else if (argc == 4 && strcmp(argv[2], "up") == 0) {
            rc = do_netbridge_up(argv[3]);
        } else {
            die("usage: flynas-helper netbridge up <uplink> | down");
        }

    } else if (strcmp(cmd, "dhcpreload") == 0) {
        rc = do_dhcpreload();

    } else if (strcmp(cmd, "pffwd") == 0) {
        if (argc != 3)
            die("usage: flynas-helper pffwd <uplink>");
        rc = do_pffwd(argv[2]);

    } else {
        die("unknown command");
    }

    return rc;
}
