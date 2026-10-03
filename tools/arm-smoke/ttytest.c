/*
 * Console tty test for the arm64 port's PL011 driver: a libc-free
 * /sbin/init (smoke.h), driven over the serial line by ttytest.exp
 * (vmexpect.py).  It checks
 *
 *  1. a canonical-mode read with echo and VERASE editing;
 *  2. a 4000-byte write, which needs the transmit interrupt to refill
 *     the FIFO many times;
 *  3. a non-canonical read of exactly VMIN bytes;
 *  4. ^C from the tty, as the controlling terminal of our session,
 *     delivering SIGINT and interrupting a blocked read.
 *
 * Each step prints "ttytest: <step>?" when it is ready for input.  Ends
 * with "ttytest: PASS" or "ttytest: FAIL <n>".
 */
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/_termios.h>
#include <sys/ttycom.h>

#include "smoke.h"

static volatile int got_int;

static void
onint(int sig)
{
	got_int = sig;
}

static int
streq(const char *a, const char *b, long n)
{
	long i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return (0);
	return (b[n] == 0);
}

int
main(void)
{
	static char big[4000];
	struct termios t, raw;
	struct sigaction sa;
	char buf[64];
	long n;
	int err, i;

	if (sc3(SYS_open, (long)"/dev/console", O_RDWR, 0, &err), err)
		return (100);
	sc3(SYS_setsid, 0, 0, 0, 0);
	sc3(SYS_ioctl, 0, TIOCSCTTY, 0, &err);
	if (err) {
		putn("ttytest: FAIL TIOCSCTTY ", 0);
		return (1);
	}
	sc3(SYS_ioctl, 0, TIOCGETA, (long)&t, &err);
	if (err || (t.c_lflag & ICANON) == 0) {
		puts_("ttytest: FAIL 2\n");
		return (2);
	}

	/* 1 */
	puts_("ttytest: line?\n");
	n = sc3(SYS_read, 0, (long)buf, sizeof(buf), &err);
	if (err || !streq(buf, "hello world\n", n)) {
		putn("ttytest: FAIL 3, read ", n);
		return (3);
	}

	/* 2 */
	for (i = 0; i < (int)sizeof(big); i++)
		big[i] = (i % 80 == 79) ? '\n' : "0123456789"[i % 10];
	n = sc3(SYS_write, 0, (long)big, sizeof(big), &err);
	if (err || n != sizeof(big)) {
		putn("ttytest: FAIL 4, wrote ", n);
		return (4);
	}
	putn("ttytest: wrote ", n);

	/* 3 */
	raw = t;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 3;
	raw.c_cc[VTIME] = 0;
	sc3(SYS_ioctl, 0, TIOCSETAW, (long)&raw, &err);
	if (err) {
		puts_("ttytest: FAIL 5\n");
		return (5);
	}
	puts_("ttytest: raw?\n");
	n = sc3(SYS_read, 0, (long)buf, sizeof(buf), &err);
	if (err || !streq(buf, "abc", n)) {
		putn("ttytest: FAIL 6, read ", n);
		return (6);
	}
	sc3(SYS_ioctl, 0, TIOCSETAW, (long)&t, &err);

	/* 4 */
	for (i = 0; i < (int)sizeof(sa); i++)
		((char *)&sa)[i] = 0;
	sa.sa_handler = onint;
	sc3(SYS_sigaction, SIGINT, (long)&sa, 0, &err);
	puts_("ttytest: intr?\n");
	n = sc3(SYS_read, 0, (long)buf, sizeof(buf), &err);
	if (!err || n != 4 /* EINTR */ || got_int != SIGINT) {
		putn("ttytest: FAIL 7, read ", n);
		return (7);
	}

	puts_("ttytest: PASS\n");
	return (0);
}
