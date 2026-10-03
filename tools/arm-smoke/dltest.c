/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * rtld smoke test for the arm64 port: a dynamic program that needs
 * libdla.so and dlopen()s libdlb.so, checking TLS variant I across
 * threads (TLSDESC static and dynamic paths, dtv growth in a thread that
 * started before the dlopen, dlclose + reopen), the lazy PLT bind with
 * all argument registers live, dlsym/dladdr.  Run it with and without
 * LD_BIND_NOW=1.  The last line is "dltest: PASS" or "dltest: FAIL <n>".
 *
 * Build: tools/arm-smoke/build-dltest.sh ROOT (into ROOT/bin, ROOT/lib)
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int	*a_addr(void);
int	 a_check(long);
double	 a_fsum(double, double, double, double, double, double, double,
	    double, long, long);
extern __thread int a_tv;

static int fail;

#define	CHECK(n, c)	do {						\
	if (!(c)) {							\
		printf("dltest: check %d failed: %s\n", (n), #c);	\
		if (!fail)						\
			fail = (n);					\
	}								\
} while (0)

static __thread int main_tv = 7;
static int (*b_check)(long);
static int *(*b_addr)(void);
static pthread_barrier_t bar;

static double
fsum_expect(void)
{
	return (1.5 + 2 * 2.5 + 3 * 3.5 + 4 * 4.5 + 5 * 5.5 + 6 * 6.5 +
	    7 * 7.5 + 8 * 8.5 + 9 * 9 + 10 * 10);
}

static void *
thr(void *arg)
{
	long id = (long)arg;
	int *pa, *pb;

	CHECK(20, main_tv == 7 && a_tv == 1002);
	main_tv = (int)id;
	a_tv = 2000 + (int)id;
	CHECK(21, a_check(id) == 0 && a_check(id) == 0);
	pa = a_addr();
	CHECK(22, pa == &a_tv);
	/* Wait for main's dlopen of libdlb.so. */
	pthread_barrier_wait(&bar);
	pthread_barrier_wait(&bar);
	CHECK(23, b_check(id) == 0 && b_check(id) == 0);
	pb = b_addr();
	CHECK(24, *pb == 1002);
	*pb = 3000 + (int)id;
	sched_yield();
	CHECK(25, *b_addr() == 3000 + (int)id && a_tv == 2000 + (int)id &&
	    main_tv == (int)id);
	CHECK(26, a_fsum(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9, 10) ==
	    fsum_expect());
	return (pb);
}

static void *
openb(void)
{
	void *h;

	h = dlopen("libdlb.so", RTLD_NOW);
	CHECK(30, h != NULL);
	if (h == NULL) {
		printf("dlopen: %s\n", dlerror());
		return (NULL);
	}
	b_check = (int (*)(long))dlsym(h, "b_check");
	b_addr = (int *(*)(void))dlsym(h, "b_addr");
	CHECK(31, b_check != NULL && b_addr != NULL);
	CHECK(32, dlsym(h, "nonexistent") == NULL);
	return (h);
}

int
main(int argc, char **argv)
{
	pthread_t t[4];
	Dl_info info;
	void *h, *ret[4];
	double (*fs)(double, double, double, double, double, double, double,
	    double, long, long);
	long i;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("dltest: start, bind %s\n", getenv("LD_BIND_NOW") ? "now" :
	    "lazy");

	/* The first call goes through the lazy binder. */
	CHECK(1, a_fsum(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9, 10) ==
	    fsum_expect());
	CHECK(2, main_tv == 7 && a_tv == 1002 && a_addr() == &a_tv);
	CHECK(3, a_check(1) == 0);
	/*
	 * dltest takes a_fsum's address, so that is its canonical PLT
	 * entry in dltest; dlsym() (like FreeBSD's) returns the definition.
	 */
	fs = (double (*)(double, double, double, double, double, double,
	    double, double, long, long))dlsym(RTLD_DEFAULT, "a_fsum");
	CHECK(4, fs != NULL && dladdr((void *)fs, &info) != 0 &&
	    strstr(info.dli_fname, "libdla.so") != NULL &&
	    strcmp(info.dli_sname, "a_fsum") == 0);
	CHECK(5, fs(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9, 10) ==
	    fsum_expect());
	printf("static tls/plt: %s\n", fail ? "FAIL" : "ok");

	pthread_barrier_init(&bar, NULL, 5);
	for (i = 0; i < 4; i++)
		CHECK(10, pthread_create(&t[i], NULL, thr, (void *)(i + 2)) ==
		    0);
	pthread_barrier_wait(&bar);
	h = openb();
	pthread_barrier_wait(&bar);
	if (h == NULL)
		return (1);
	CHECK(33, b_check(1) == 0 && *b_addr() == 1002);
	for (i = 0; i < 4; i++)
		CHECK(11, pthread_join(t[i], &ret[i]) == 0);
	for (i = 0; i < 4; i++)
		CHECK(12, ret[i] != NULL && ret[i] != b_addr());
	CHECK(13, ret[0] != ret[1] && ret[2] != ret[3]);
	CHECK(14, main_tv == 7 && a_tv == 1002);
	printf("dlopen tls/threads: %s\n", fail ? "FAIL" : "ok");

	CHECK(40, dladdr((void *)b_check, &info) != 0 &&
	    strstr(info.dli_fname, "libdlb.so") != NULL);
	CHECK(41, dlclose(h) == 0);
	h = openb();
	CHECK(42, h != NULL && b_check(5) == 0 && *b_addr() == 1002);
	CHECK(43, dlopen("libnonexistent.so", RTLD_LAZY) == NULL &&
	    dlerror() != NULL);
	printf("dlclose/reopen: %s\n", fail ? "FAIL" : "ok");

	if (fail)
		printf("dltest: FAIL %d\n", fail);
	else
		printf("dltest: PASS\n");
	return (fail);
}
