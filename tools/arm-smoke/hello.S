/* Smoke test for bin/arm-vm and the clang/lld cross toolchain: a Linux arm64
 * Image that prints over PL011 (QEMU virt: 0x09000000) and powers off via
 * PSCI SYSTEM_OFF.
 *
 *   clang-18 --target=aarch64-unknown-none-elf -c hello.S -o hello.o
 *   ../lld/usr/bin/ld.lld-18 -Ttext=0 -e _start hello.o -o hello.elf
 *   llvm-objcopy-18 -O binary hello.elf hello.bin
 *   ../../bin/arm-vm -t 20 hello.bin      # prints one line, exits in <1s
 */
	.section .text.boot, "ax"
	.globl _start
_start:
	b	1f			/* code0: branch past the header */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end - _start		/* image_size */
	.quad	0x2			/* flags: LE, 4K pages, anywhere */
	.quad	0, 0, 0			/* res2-4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */
1:	ldr	x1, =0x09000000
	adr	x2, msg
2:	ldrb	w3, [x2], #1
	cbz	w3, 3f
	strb	w3, [x1]
	b	2b
3:	ldr	w0, =0x84000008		/* PSCI_SYSTEM_OFF */
	hvc	#0
4:	wfi
	b	4b
msg:	.asciz	"arm-vm: hello from aarch64 (clang+lld Image)\r\n"
	.balign	8
	.ltorg
_end:
