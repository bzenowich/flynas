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

    } else {
        die("unknown command");
    }

    return rc;
}
