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
 * Shared library for dltest.c, built twice: as libdla.so (-DNAME=a, a
 * DT_NEEDED of dltest, so its TLS is in the static block) and as
 * libdlb.so (-DNAME=b, dlopen()ed, so its TLS is allocated on demand
 * through the dtv).  Both use TLSDESC (general dynamic) accesses.
 */
#include <stdint.h>
#include <string.h>

#define	CAT2(a, b)	a##_##b
#define	CAT(a, b)	CAT2(a, b)
#define	SYM(s)		CAT(NAME, s)

__thread int SYM(tv) = 1002;
static __thread char tbig[256] __attribute__((aligned(64))) = "tls-init";
static __thread long tzero;

int *
SYM(addr)(void)
{
	return (&SYM(tv));
}

/* 0 if the module's TLS block looks right in this thread. */
int
SYM(check)(long id)
{
	if (((uintptr_t)tbig & 63) != 0)
		return (1);
	if (tzero != 0 && tzero != id)
		return (2);
	if (tzero == 0 && strcmp(tbig, "tls-init") != 0)
		return (3);
	tzero = id;
	tbig[0] = 'T';
	return (0);
}

/* All argument registers in use: survives a lazy PLT bind. */
double
SYM(fsum)(double a, double b, double c, double d, double e, double f,
    double g, double h, long i, long j)
{
	return (a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h +
	    9 * i + 10 * j);
}
