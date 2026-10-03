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
 * Unwinder smoke test for the arm64 port (lib/libgcc_eh, LLVM libunwind):
 * _Unwind_Backtrace, and _Unwind_ForcedUnwind through nested frames with
 * __attribute__((cleanup)) handlers (C with -fexceptions, compiler-rt's
 * __gcc_personality_v0), once through plain calls and once from a qsort()
 * comparator, i.e. through libc's frames.  The stop function longjmp()s
 * out at the end of the stack.  Build static and dynamic; the last line is
 * "ehtest: PASS" or "ehtest: FAIL <n>".
 *
 * Build: tools/arm-smoke/build-utest.sh ehtest OUT -fexceptions
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>

static int fail;

#define	CHECK(n, c)	do {						\
	if (!(c)) {							\
		printf("ehtest: check %d failed: %s\n", (n), #c);	\
		if (!fail)						\
			fail = (n);					\
	}								\
} while (0)

static jmp_buf done;
static int cleanups, stops;
static struct _Unwind_Exception exc;

/* Backtrace */

static int nframes;

static _Unwind_Reason_Code
trace(struct _Unwind_Context *ctx, void *arg)
{
	(void)arg;
	if (_Unwind_GetIP(ctx) != 0)
		nframes++;
	return (_URC_NO_REASON);
}

__attribute__((noinline)) static int
bt3(void)
{
	int r;

	nframes = 0;
	r = _Unwind_Backtrace(trace, NULL);
	__asm __volatile("" ::: "memory");	/* no tail call: keep bt3's frame */
	return (r);
}

__attribute__((noinline)) static int
bt2(void)
{
	return (bt3() + 1);
}

__attribute__((noinline)) static int
bt1(void)
{
	return (bt2() + 1);
}

/* Forced unwind */

static void
cleanup(int *p)
{
	cleanups += *p;
}

static _Unwind_Reason_Code
stop(int version, _Unwind_Action actions, _Unwind_Exception_Class cls,
    struct _Unwind_Exception *e, struct _Unwind_Context *ctx, void *arg)
{
	(void)version; (void)cls; (void)e; (void)ctx; (void)arg;
	stops++;
	if (actions & _UA_END_OF_STACK)
		longjmp(done, 1);
	return (_URC_NO_REASON);
}

__attribute__((noinline)) static void
raise_it(void)
{
	int c __attribute__((cleanup(cleanup))) = 1;

	memset(&exc, 0, sizeof(exc));
	exc.exception_class = 0x4446594e4f4e4500ULL;	/* "DFYNONE\0" */
	_Unwind_ForcedUnwind(&exc, stop, NULL);
	printf("ehtest: _Unwind_ForcedUnwind returned\n");
	c = 0;
}

__attribute__((noinline)) static void
level2(void)
{
	int c __attribute__((cleanup(cleanup))) = 10;

	raise_it();
	c = 0;
}

__attribute__((noinline)) static void
level1(void)
{
	int c __attribute__((cleanup(cleanup))) = 100;

	level2();
	c = 0;
}

static int
cmp(const void *a, const void *b)
{
	int c __attribute__((cleanup(cleanup))) = 1000;

	(void)a; (void)b;
	raise_it();
	c = 0;
	return (0);
}

int
main(void)
{
	int v[4] = { 4, 3, 2, 1 };

	setvbuf(stdout, NULL, _IONBF, 0);

	CHECK(1, bt1() == _URC_END_OF_STACK + 2);
	CHECK(2, nframes >= 4);
	printf("backtrace: %d frames\n", nframes);

	cleanups = stops = 0;
	if (setjmp(done) == 0) {
		level1();
		CHECK(10, 0);
	}
	CHECK(11, cleanups == 111);
	CHECK(12, stops >= 4);
	printf("forced unwind: cleanups %d, stop calls %d\n", cleanups, stops);

	cleanups = stops = 0;
	if (setjmp(done) == 0) {
		qsort(v, 4, sizeof(v[0]), cmp);
		CHECK(20, 0);
	}
	CHECK(21, cleanups == 1001);
	printf("through qsort: cleanups %d, stop calls %d\n", cleanups,
	    stops);

	if (fail)
		printf("ehtest: FAIL %d\n", fail);
	else
		printf("ehtest: PASS\n");
	return (fail);
}
