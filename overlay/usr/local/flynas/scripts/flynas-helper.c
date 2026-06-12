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

    } else {
        die("unknown command");
    }

    return rc;
}
