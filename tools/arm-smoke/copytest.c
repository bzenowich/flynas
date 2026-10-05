/*
 * Test of the arm64 kernel copy routines (review-10-05 P1).
 *
 * 1. The kernel's memcpy.S, assembled into this program with its symbols
 *    renamed to t_memcpy/t_memmove/t_memset (build-utest.sh copytest OUT
 *    SRC/sys/platform/arm64/aarch64/memcpy.S -Dmemcpy=t_memcpy ...), is
 *    checked against byte loops for every src/dst alignment 0..15 and
 *    every length 0..300 plus some large ones, overlapping both ways for
 *    memmove, with guard bytes around the destination.
 * 2. copyin/copyout are exercised through pwrite/pread on a file with
 *    the same alignments and lengths, and must fail with EFAULT (or
 *    return a short count) for a buffer that runs into an unmapped page.
 *
 * Prints "COPYTEST ok" or the first mismatch.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void	*t_memcpy(void *, const void *, size_t);
void	*t_memmove(void *, const void *, size_t);
void	*t_memset(void *, int, size_t);

#define	BUF	(1 << 20)
#define	GUARD	64

static unsigned char *src, *dst, *ref;
static int nfail;

static void
fill(unsigned char *p, size_t n, unsigned seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(seed + i * 131 + (i >> 8));
}

static void
fail(const char *what, size_t sa, size_t da, size_t len)
{
	if (nfail++ < 10)
		printf("FAIL %s src+%zu dst+%zu len %zu\n", what, sa, da, len);
}

static int
lens(size_t i)
{
	static const size_t big[] = { 4095, 4096, 4097, 65536 + 7, 300000 };

	if (i <= 300)
		return ((int)i);
	i -= 301;
	return (i < sizeof(big) / sizeof(big[0]) ? (int)big[i] : -1);
}

static void
test_mem(void)
{
	size_t sa, da, i, k;
	int len;

	for (sa = 0; sa < 16; sa++)
	for (da = 0; da < 16; da++)
	for (i = 0; (len = lens(i)) >= 0; i++) {
		/* memcpy */
		fill(src, len + 32, (unsigned)(sa * 7 + len));
		memset(dst, 0xa5, len + 2 * GUARD + 16);
		memcpy(ref, dst, len + 2 * GUARD + 16);
		for (k = 0; k < (size_t)len; k++)
			ref[GUARD + da + k] = src[sa + k];
		if (t_memcpy(dst + GUARD + da, src + sa, len) !=
		    dst + GUARD + da ||
		    memcmp(dst, ref, len + 2 * GUARD + 16) != 0)
			fail("memcpy", sa, da, len);

		/* memset */
		memset(dst, 0xa5, len + 2 * GUARD + 16);
		memcpy(ref, dst, len + 2 * GUARD + 16);
		for (k = 0; k < (size_t)len; k++)
			ref[GUARD + da + k] = (unsigned char)(sa * 17 + 3);
		if (t_memset(dst + GUARD + da, (int)(sa * 17 + 3) | 0x7f00,
		    len) != dst + GUARD + da ||
		    memcmp(dst, ref, len + 2 * GUARD + 16) != 0)
			fail("memset", sa, da, len);

		/* memmove within one buffer, dst above and below src */
		if (len > 70000)
			continue;
		fill(dst, len + 2 * GUARD + 64, (unsigned)len);
		memcpy(ref, dst, len + 2 * GUARD + 64);
		for (k = len; k-- > 0;)
			ref[GUARD + 32 + da + k] = ref[GUARD + sa + k];
		if (t_memmove(dst + GUARD + 32 + da, dst + GUARD + sa, len) !=
		    dst + GUARD + 32 + da ||
		    memcmp(dst, ref, len + 2 * GUARD + 64) != 0)
			fail("memmove up", sa, da, len);

		fill(dst, len + 2 * GUARD + 64, (unsigned)len + 1);
		memcpy(ref, dst, len + 2 * GUARD + 64);
		for (k = 0; k < (size_t)len; k++)
			ref[GUARD + da + k] = ref[GUARD + 32 + sa + k];
		if (t_memmove(dst + GUARD + da, dst + GUARD + 32 + sa, len) !=
		    dst + GUARD + da ||
		    memcmp(dst, ref, len + 2 * GUARD + 64) != 0)
			fail("memmove down", sa, da, len);
	}
}

static void
test_copyio(const char *path)
{
	size_t sa, da, i;
	unsigned char *pg;
	ssize_t r;
	int fd, len;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror(path);
		nfail++;
		return;
	}
	for (sa = 0; sa < 16; sa++)
	for (da = 0; da < 16; da++)
	for (i = 0; (len = lens(i)) >= 0; i++) {
		fill(src, len + 16, (unsigned)(da * 5 + len));
		if (pwrite(fd, src + sa, len, 0) != len) {
			fail("copyin", sa, da, len);
			continue;
		}
		memset(dst, 0xa5, len + 2 * GUARD + 16);
		memcpy(ref, dst, len + 2 * GUARD + 16);
		memcpy(ref + GUARD + da, src + sa, len);
		if (pread(fd, dst + GUARD + da, len, 0) != len ||
		    memcmp(dst, ref, len + 2 * GUARD + 16) != 0)
			fail("copyout", sa, da, len);
	}

	/* A buffer that runs off the end of the mapping. */
	pg = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (pg == MAP_FAILED || munmap(pg + 4096, 4096) != 0) {
		perror("mmap");
		nfail++;
	} else {
		memset(pg, 1, 4096);
		r = pwrite(fd, pg + 4096 - 100, 200, 0);
		if (!(r == -1 && errno == EFAULT) && !(r >= 0 && r <= 100)) {
			printf("FAIL copyin fault: %zd errno %d\n", r, errno);
			nfail++;
		}
		r = pread(fd, pg + 4096 - 100, 200, 0);
		if (!(r == -1 && errno == EFAULT) && !(r >= 0 && r <= 100)) {
			printf("FAIL copyout fault: %zd errno %d\n", r, errno);
			nfail++;
		}
	}
	close(fd);
	unlink(path);
}

int
main(int argc, char **argv)
{
	src = malloc(BUF);
	dst = malloc(BUF);
	ref = malloc(BUF);
	if (src == NULL || dst == NULL || ref == NULL)
		return (1);
	test_mem();
	test_copyio(argc > 1 ? argv[1] : "/tmp/copytest.dat");
	if (nfail == 0)
		printf("COPYTEST ok\n");
	else
		printf("COPYTEST %d failures\n", nfail);
	return (nfail != 0);
}
