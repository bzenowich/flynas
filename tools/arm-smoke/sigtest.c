/*
 * Signal test for the arm64 port: a static, libc-free DragonFly/aarch64
 * program run as /sbin/init (see uinit.S for the ABI notes).  It checks
 * sendsig()/sigtramp/sigreturn():
 *
 *  1. kill(getpid(), SIGUSR1) to an SA_SIGINFO handler: the handler's
 *     arguments, and that the callee-saved and FP registers it clobbers
 *     come back intact through sigreturn.
 *  2. a load from an unmapped address: SIGSEGV with si_addr and the
 *     fault address in x3; the handler skips the load by advancing
 *     mc_elr in the ucontext, so sigreturn must honour the edited
 *     context.
 *  3. sigreturn() with a forged EL1h PSTATE must fail with EINVAL.
 *
 * Prints "sigtest: PASS" or "sigtest: FAIL <n>" on /dev/console, exits
 * with 0 or n.  Built by tools/arm-smoke/build-sigtest.sh.
 */
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/ucontext.h>
#include <sys/syscall.h>
#include <sys/fcntl.h>

__asm__(
	"	.section .note.tag, \"a\"\n"
	"	.balign	4\n"
	"	.long	10, 4, 1\n"
	"	.asciz	\"DragonFly\"\n"
	"	.balign	4\n"
	"	.long	600521\n"
	"	.text\n"
	"	.globl	_start\n"
	"_start:\n"
	"	bl	main\n"
	"	mov	x8, #1\n"		/* SYS_exit(main()) */
	"	svc	#0\n"
	"1:	b	1b\n");

static long
sc3(long n, long a, long b, long c, int *err)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x8 __asm__("x8") = n;
	long nzcv;

	__asm__ __volatile__("svc #0; mrs %4, nzcv"
	    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x8), "=r" (nzcv)
	    : : "memory", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11",
	      "x12", "x13", "x14", "x15", "x16", "x17", "cc");
	if (err)
		*err = (nzcv >> 29) & 1;
	return (x0);
}

static void
puts_(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sc3(SYS_write, 0, (long)s, n, 0);
}

static void
putn(const char *s, unsigned long v)
{
	char buf[20];
	int i = sizeof(buf);

	puts_(s);
	buf[--i] = 0;
	buf[--i] = '\n';
	do {
		buf[--i] = "0123456789abcdef"[v & 15];
		v >>= 4;
	} while (v);
	puts_(&buf[i]);
}

static volatile int got_usr1, got_segv, fail;

#define	CHECK(n, c)	do { if (!(c) && !fail) fail = (n); } while (0)

static void
usr1(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;

	CHECK(10, sig == SIGUSR1);
	CHECK(11, si->si_signo == SIGUSR1);
	CHECK(12, ((unsigned long)uc & 15) == 0);
	CHECK(13, uc->uc_mcontext.mc_len == sizeof(mcontext_t));
	CHECK(14, uc->uc_mcontext.mc_fpformat == _MC_FPFMT_VFP);
	/* The interrupted d8 (see main) is in the saved FP state. */
	CHECK(15, uc->uc_mcontext.mc_fpregs[16] == 0x4045000000000000ul);
	/* Trash what sigreturn must restore. */
	__asm__ __volatile__("mov x19, #7; mov x28, #7; fmov d8, x19; "
	    "fmov d31, x19; msr fpcr, xzr" : : : "x19", "x28", "d8", "d31");
	got_usr1++;
}

static void
segv(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;

	CHECK(20, sig == SIGSEGV);
	CHECK(21, (unsigned long)si->si_addr == 0x18);
	CHECK(22, uc->uc_mcontext.mc_elr != 0);
	uc->uc_mcontext.mc_elr += 4;		/* skip the faulting ldr */
	uc->uc_mcontext.mc_x[9] = 0x5a5a;	/* and change its result */
	got_segv++;
}

int
main(void)
{
	struct sigaction sa;
	ucontext_t bad;
	unsigned long x19, x28, d8, d31, fpcr, v;
	long pid;
	int err, i;

	if (sc3(SYS_open, (long)"/dev/console", O_RDWR, 0, &err), err)
		return (100);

	for (i = 0; i < (int)sizeof(sa); i++)
		((char *)&sa)[i] = 0;
	sa.sa_sigaction = usr1;
	sa.sa_flags = SA_SIGINFO;
	sc3(SYS_sigaction, SIGUSR1, (long)&sa, 0, &err);
	CHECK(1, !err);
	sa.sa_sigaction = segv;
	sc3(SYS_sigaction, SIGSEGV, (long)&sa, 0, &err);
	CHECK(2, !err);

	/* 1: registers survive a handler that trashes them. */
	pid = sc3(SYS_getpid, 0, 0, 0, 0);
	__asm__ __volatile__(
	    "mov x19, #0x1919; mov x28, #0x2828\n"
	    "mov x9, #0x4045000000000000; fmov d8, x9\n"
	    "mov x9, #0x3131; fmov d31, x9\n"
	    "mov x9, #(1 << 24); msr fpcr, x9\n"	/* FZ */
	    "mov x0, %5; mov x1, %6; mov x8, %7; svc #0\n"
	    "mov %0, x19; mov %1, x28; fmov %2, d8; fmov %3, d31\n"
	    "mrs %4, fpcr; msr fpcr, xzr"
	    : "=&r" (x19), "=&r" (x28), "=&r" (d8), "=&r" (d31), "=&r" (fpcr)
	    : "r" (pid), "r" ((long)SIGUSR1), "r" ((long)SYS_kill)
	    : "x0", "x1", "x8", "x9", "x19", "x28", "d8", "d31", "memory", "cc");
	CHECK(3, got_usr1 == 1);
	CHECK(4, x19 == 0x1919 && x28 == 0x2828);
	CHECK(5, d8 == 0x4045000000000000ul && d31 == 0x3131);
	CHECK(6, fpcr == (1 << 24));

	/* 2: SIGSEGV, resumed past the fault by an edited context. */
	__asm__ __volatile__("mov x9, #0x18; ldr x9, [x9]; mov %0, x9"
	    : "=r" (v) : : "x9", "memory");
	CHECK(7, got_segv == 1);
	CHECK(8, v == 0x5a5a);

	/* 3: a forged privileged PSTATE is refused. */
	for (i = 0; i < (int)sizeof(bad); i++)
		((char *)&bad)[i] = 0;
	bad.uc_mcontext.mc_spsr = 0x5;		/* EL1h */
	v = sc3(SYS_sigreturn, (long)&bad, 0, 0, &err);
	CHECK(9, err && v == 22);

	if (fail) {
		putn("sigtest: FAIL ", fail);
		return (fail);
	}
	puts_("sigtest: PASS\n");
	return (0);
}
