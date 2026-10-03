/*
 * libc smoke test for the arm64 port: a static program linked against the
 * cross-built libc.a (bin/arm-world lib/libc), run as /sbin/init.  Each
 * check prints one line; the last line is "libctest: PASS" or
 * "libctest: FAIL <n>".  Covers the aarch64 MD parts of libc and crt:
 *
 *   crt1/ctors, stdio + gdtoa (double and quad long double), errno in
 *   static TLS, malloc, setjmp/sigsetjmp, sigaction, getcontext /
 *   makecontext / swapcontext, fork + pipe + waitpid, pthread_create with
 *   __thread variables (TLS variant I).
 *
 * Build: tools/arm-smoke/build-libctest.sh OUT
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static int fail;
static int ctor_ran;

#define	CHECK(n, c)	do {						\
	if (!(c)) {							\
		printf("libctest: check %d failed: %s\n", (n), #c);	\
		if (!fail)						\
			fail = (n);					\
	}								\
} while (0)

__attribute__((constructor)) static void
ctor(void)
{
	ctor_ran = 1;
}

static void
t_stdio(void)
{
	char buf[128];
	long double ld;
	double d;

	snprintf(buf, sizeof(buf), "%d %s %.3f %x", -42, "str", 3.14159, 0xbeef);
	CHECK(10, strcmp(buf, "-42 str 3.142 beef") == 0);
	d = strtod("2.5e-3", NULL);
	CHECK(11, d == 0.0025);
	ld = strtold("1.000000000000000000000000000001", NULL);
	CHECK(12, ld > 1.0L && sizeof(ld) == 16);
	snprintf(buf, sizeof(buf), "%.30Lf", ld);
	CHECK(13, strcmp(buf, "1.000000000000000000000000000001") == 0);
	snprintf(buf, sizeof(buf), "%g %Lg", 1e300 * 10, (long double)-0.5);
	CHECK(14, strcmp(buf, "1e+301 -0.5") == 0);
	CHECK(15, isnan((double)NAN) && isinf((long double)INFINITY) &&
	    signbit(-0.0L) && isfinite(1.0L) && isnormal(1.0L));
	printf("stdio: %s\n", buf);
}

static void
t_errno_malloc(void)
{
	char *p[64];
	int i;

	errno = 0;
	CHECK(20, open("/nonexistent", O_RDONLY) == -1 && errno == ENOENT);
	CHECK(21, close(1234) == -1 && errno == EBADF);
	for (i = 0; i < 64; i++) {
		p[i] = malloc(16 << (i % 16));
		CHECK(22, p[i] != NULL);
		memset(p[i], i, 16 << (i % 16));
	}
	for (i = 0; i < 64; i++)
		CHECK(23, p[i][(16 << (i % 16)) - 1] == (char)i);
	for (i = 0; i < 64; i++)
		free(p[i]);
	printf("errno/malloc: ok\n");
}

static jmp_buf jb;
static sigjmp_buf sjb;
static volatile int nsig;

static void
jump(int depth)
{
	if (depth == 0)
		longjmp(jb, 7);
	jump(depth - 1);
}

static void
usr1(int sig)
{
	nsig++;
	siglongjmp(sjb, sig);
}

static void
t_jmp_signals(void)
{
	struct sigaction sa;
	volatile double f = 1.5;
	sigset_t set;
	int r;

	if ((r = setjmp(jb)) == 0)
		jump(10);
	CHECK(30, r == 7 && f == 1.5);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = usr1;
	CHECK(31, sigaction(SIGUSR1, &sa, NULL) == 0);
	if ((r = sigsetjmp(sjb, 1)) == 0) {
		raise(SIGUSR1);
		CHECK(32, 0);
	}
	CHECK(33, r == SIGUSR1 && nsig == 1);
	/* sigsetjmp(.., 1) restored the mask: SIGUSR1 is unblocked again. */
	sigprocmask(SIG_BLOCK, NULL, &set);
	CHECK(34, !sigismember(&set, SIGUSR1));
	printf("setjmp/signals: ok\n");
}

static ucontext_t uc_main, uc_func;
static int ctx_sum;

static void
ctxfunc(int a, int b, int c, int d, int e, int f, int g, int h)
{
	ctx_sum = a + b + c + d + e + f + g + h;
	swapcontext(&uc_func, &uc_main);
	ctx_sum *= 2;
}

static void
t_context(void)
{
	static char stack[16384] __attribute__((aligned(16)));
	volatile int pass = 0;

	CHECK(40, getcontext(&uc_func) == 0);
	uc_func.uc_stack.ss_sp = stack;
	uc_func.uc_stack.ss_size = sizeof(stack);
	uc_func.uc_link = &uc_main;
	makecontext(&uc_func, (void (*)(void))ctxfunc, 8, 1, 2, 3, 4, 5, 6,
	    7, 8);
	CHECK(41, swapcontext(&uc_main, &uc_func) == 0);
	CHECK(42, ctx_sum == 36);
	/*
	 * Resume ctxfunc; it returns through uc_link back here.  DragonFly's
	 * getcontext() clears uc_link, and ctxfunc's swapcontext() saved
	 * into uc_func, so set it again.
	 */
	uc_func.uc_link = &uc_main;
	CHECK(43, swapcontext(&uc_main, &uc_func) == 0);
	CHECK(44, ctx_sum == 72);
	getcontext(&uc_main);
	if (pass++ == 0)
		setcontext(&uc_main);
	CHECK(45, pass == 2);
	printf("ucontext: ok\n");
}

static void
t_fork(void)
{
	char buf[16];
	pid_t pid;
	int fd[2], st;

	CHECK(50, pipe(fd) == 0);
	pid = fork();
	if (pid == 0) {
		close(fd[0]);
		write(fd[1], "child", 5);
		_exit(42);
	}
	CHECK(51, pid > 0);
	close(fd[1]);
	memset(buf, 0, sizeof(buf));
	CHECK(52, read(fd[0], buf, sizeof(buf)) == 5 &&
	    strcmp(buf, "child") == 0);
	CHECK(53, waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
	    WEXITSTATUS(st) == 42);
	close(fd[0]);
	printf("fork/pipe/wait: ok\n");
}

static __thread int tls_int = 5;
static __thread char tls_buf[100];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int shared;

static void *
thr(void *arg)
{
	int i;

	CHECK(60, tls_int == 5 && tls_buf[0] == 0);
	tls_int = (int)(long)arg;
	snprintf(tls_buf, sizeof(tls_buf), "t%ld", (long)arg);
	for (i = 0; i < 10000; i++) {
		pthread_mutex_lock(&mtx);
		shared++;
		pthread_mutex_unlock(&mtx);
	}
	errno = 100 + (int)(long)arg;
	sched_yield();
	CHECK(61, errno == 100 + (int)(long)arg);
	CHECK(62, tls_int == (int)(long)arg);
	return (tls_buf);
}

static void
t_threads(void)
{
	pthread_t t[4];
	void *ret;
	long i;

	tls_int = 99;
	for (i = 0; i < 4; i++)
		CHECK(63, pthread_create(&t[i], NULL, thr, (void *)i) == 0);
	for (i = 0; i < 4; i++) {
		CHECK(64, pthread_join(t[i], &ret) == 0);
		CHECK(65, ret != NULL);
	}
	CHECK(66, shared == 40000 && tls_int == 99);
	printf("pthreads: ok\n");
}

static int
run(int argc, char **argv)
{
	struct timespec ts;

	printf("libctest: start, argc %d, argv[0] %s, pid %d\n", argc,
	    argv[0], getpid());
	CHECK(1, ctor_ran);
	CHECK(2, clock_gettime(CLOCK_MONOTONIC, &ts) == 0);

	t_stdio();
	t_errno_malloc();
	t_jmp_signals();
	t_context();
	t_fork();
	t_threads();
	return (fail);
}

int
main(int argc, char **argv)
{
	int fd;

	/* init starts without descriptors. */
	if (fcntl(0, F_GETFD) == -1) {
		fd = open("/dev/console", O_RDWR);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	setvbuf(stdout, NULL, _IONBF, 0);

	/*
	 * libc's thread init takes over the console when it runs as pid 1
	 * (setsid, revoke, TIOCSCTTY: thr_init.c), which would revoke the
	 * descriptors above.  Run the tests in a child and report its status.
	 */
	if (getpid() == 1) {
		pid_t pid;
		int st;

		pid = fork();
		if (pid == 0)
			_exit(run(argc, argv));
		if (pid < 0 || waitpid(pid, &st, 0) != pid) {
			printf("libctest: FAIL 3 (fork/waitpid)\n");
			return (3);
		}
		fail = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		if (fail)
			printf("libctest: FAIL %d\n", fail);
		else
			printf("libctest: PASS\n");
		return (fail);
	}
	fail = run(argc, argv);
	printf("libctest: %s\n", fail ? "FAIL" : "PASS");
	return (fail);
}
