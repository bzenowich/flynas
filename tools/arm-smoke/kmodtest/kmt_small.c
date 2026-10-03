/*
 * kmodtest: a kernel module that checks the aarch64 kernel linker's
 * relocation of ET_REL objects (cpu/aarch64/misc/elf_machdep.c).  This
 * file is built with the default small code model (ADRP + ADD/LDR/STR
 * lo12, CALL26/JUMP26, ABS64/PREL32 data) and kmt_large.c with
 * -mcmodel=large (MOVZ/MOVK G0..G3); the asm below adds the short-range
 * branch and literal forms, aimed at another section so the assembler
 * cannot resolve them itself.  MOD_LOAD prints "kmodtest: PASS" or
 * "kmodtest: FAIL <mask>".
 */
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/module.h>

extern int ticks;

void	*kmt_large_ticks(void);
void	*kmt_large_self(void);
long	kmt_large_call(const char *);
int	kmt_large_counter(void);
long	kmt_asm(long);

int kmt_counter = 41;			/* ADRP + LDR/STR in both files */
static uint16_t kmt_h = 0x1234;		/* LDST16 */
static uint8_t kmt_b = 0x56;		/* LDST8 */
uint64_t kmt_q[2] __aligned(16) = { 0x1111, 0x2222 };	/* LDST64 */

/* ABS64 data relocations to a kernel symbol and to this module. */
static void *kmt_ptrs[] = { &ticks, (void *)kmt_large_self, (void *)strlen };

/* PREL32 / PREL64 to a kernel symbol. */
__asm__(
	"	.section .rodata\n"
	"	.balign	8\n"
	"	.globl	kmt_prel\n"
	"kmt_prel:\n"
	"	.word	ticks - .\n"
	"	.word	0\n"
	"	.quad	strlen - .\n"
	/*
	 * kmt_asm(x): returns 7 through TBZ (TSTBR14), CBZ (CONDBR19),
	 * LDR literal (LD_PREL_LO19), ADR (ADR_PREL_LO21) and B (JUMP26),
	 * all crossing into .text.kmt_far, and checks an LDST64 load.
	 */
	"	.text\n"
	"	.balign	4\n"
	"	.globl	kmt_asm\n"
	"kmt_asm:\n"
	"	tbz	x0, #0, kmt_far_tbz\n"
	"	mov	x0, #-1\n"
	"	ret\n"
	"kmt_back1:\n"
	"	cbz	x1, kmt_far_cbz\n"
	"	mov	x0, #-2\n"
	"	ret\n"
	"kmt_back2:\n"
	"	ldr	x2, kmt_far_lit\n"
	"	adr	x3, kmt_far_lit\n"
	"	ldr	x3, [x3]\n"
	"	cmp	x2, x3\n"
	"	b.ne	1f\n"
	"	adrp	x4, kmt_q\n"
	"	ldr	x4, [x4, :lo12:kmt_q+8]\n"
	"	mov	x5, #0x2222\n"
	"	cmp	x4, x5\n"
	"	b.ne	1f\n"
	"	b	kmt_far_ret\n"
	"1:	mov	x0, #-3\n"
	"	ret\n"
	"	.section .text.kmt_far, \"ax\"\n"
	"	.balign	8\n"
	"kmt_far_lit:\n"
	"	.quad	7\n"
	"kmt_far_tbz:\n"
	"	mov	x1, #0\n"
	"	b	kmt_back1\n"
	"kmt_far_cbz:\n"
	"	b	kmt_back2\n"
	"kmt_far_ret:\n"
	"	mov	x0, x2\n"
	"	ret\n"
	"	.text\n");
extern const int32_t kmt_prel[];

static int
kmt_check(void)
{
	volatile uint16_t *hp = &kmt_h;
	volatile uint8_t *bp = &kmt_b;
	int fail = 0;
	char *p;

	if (kmt_ptrs[0] != (void *)&ticks)
		fail |= 1 << 0;
	if (kmt_large_ticks() != (void *)&ticks)
		fail |= 1 << 1;
	if (kmt_ptrs[1] != kmt_large_self() ||
	    kmt_large_self() != (void *)kmt_large_self)
		fail |= 1 << 2;
	if (strlen("hello") != 5 || kmt_large_call("abc") != 3 ||
	    ((size_t (*)(const char *))kmt_ptrs[2])("ab") != 2)
		fail |= 1 << 3;
	kmt_counter++;
	if (kmt_large_counter() != 42)
		fail |= 1 << 4;
	if (*hp != 0x1234 || *bp != 0x56)
		fail |= 1 << 5;
	if (kmt_q[0] != 0x1111 || kmt_q[1] != 0x2222)
		fail |= 1 << 6;
	p = (char *)kmt_prel;
	if (p + kmt_prel[0] != (char *)&ticks ||
	    p + 8 + *(const int64_t *)(p + 8) != (char *)strlen)
		fail |= 1 << 7;
	if (kmt_asm(0) != 7)
		fail |= 1 << 8;
	return (fail);
}

static int
kmt_modevent(module_t mod, int type, void *data)
{
	int fail;

	switch (type) {
	case MOD_LOAD:
		fail = kmt_check();
		if (fail)
			kprintf("kmodtest: FAIL %#x\n", fail);
		else
			kprintf("kmodtest: PASS\n");
		return (0);
	case MOD_UNLOAD:
		kprintf("kmodtest: unloaded\n");
		return (0);
	}
	return (EOPNOTSUPP);
}

static moduledata_t kmt_mod = { "kmodtest", kmt_modevent, NULL };
DECLARE_MODULE(kmodtest, kmt_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
