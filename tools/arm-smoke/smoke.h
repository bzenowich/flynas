/*
 * Shared glue for the libc-free arm64 smoke-test programs (sigtest.c,
 * ttytest.c, kmodtest/kldinit.c), each run as /sbin/init: the DragonFly
 * ELF note, _start (exit(main())), a raw syscall helper and console
 * output.  The ABI is in uinit.S.  Include it after the system headers.
 */
#ifndef _ARM_SMOKE_H_
#define	_ARM_SMOKE_H_

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

#endif /* !_ARM_SMOKE_H_ */
