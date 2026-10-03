/* Phase 2 exit test for the arm64 port: a static DragonFly/aarch64 ELF
 * run as /sbin/init.  It opens /dev/console, writes one line and exits,
 * exercising exec, the svc syscall path, copyin and the return to EL0.
 * Syscall ABI: number in x8, arguments in x0-x7, svc #0; on error the
 * carry flag is set and x0 holds the errno.
 *
 *   clang-18 --target=aarch64-unknown-dragonfly -c uinit.S -o uinit.o
 *   ../lld/usr/bin/ld.lld-18 -static -e _start uinit.o -o init
 *   ../../bin/arm-mkiso root.iso DIR      # with DIR/sbin/init, DIR/dev/
 */
#define	SYS_exit	1
#define	SYS_write	4
#define	SYS_open	5
#define	O_RDWR		2

	/* The ABI note DragonFly's ELF image activator brands on. */
	.section .note.tag, "a"
	.balign	4
	.long	10			/* namesz */
	.long	4			/* descsz */
	.long	1			/* type: ABI tag */
	.asciz	"DragonFly"
	.balign	4
	.long	600521			/* __DragonFly_version */

	.text
	.globl	_start
_start:
	adr	x0, console
	mov	x1, #O_RDWR
	mov	x8, #SYS_open
	svc	#0
	b.cs	fail
	adr	x1, msg
	mov	x2, #(msgend - msg)
	mov	x8, #SYS_write
	svc	#0
	b.cs	fail
	mov	x0, #0
	b	out
fail:	add	x0, x0, #100		/* exit status 100 + errno */
out:	mov	x8, #SYS_exit
	svc	#0
1:	b	1b

console: .asciz	"/dev/console"
msg:	.ascii	"uinit: hello from aarch64 userland\n"
msgend:
