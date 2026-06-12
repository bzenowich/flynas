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
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_NAME_LEN 32
#define MAX_SHELL_LEN 64
#define MAX_GROUPS_LEN 256
#define MAX_KEYS_LEN 65536
#define PW_CMD "/usr/sbin/pw"
#define SMARTCTL_CMD "/usr/local/sbin/smartctl"
#define FDISK_CMD "/sbin/fdisk"

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

    } else {
        die("unknown command");
    }

    return rc;
}
