/*
 * /sbin/init for the kmodtest module (see kmt_small.c): kldload
 * /kmodtest.ko, whose MOD_LOAD handler prints PASS or FAIL itself, then
 * kldunload it.  Prints "kldinit: loaded <id>" and "kldinit: unloaded",
 * or "kldinit: FAIL <step> <errno>".  Libc-free, on
 * ../smoke.h.
 */
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/fcntl.h>

#include "../smoke.h"

int
main(void)
{
	long id, v;
	int err;

	if (sc3(SYS_open, (long)"/dev/console", O_RDWR, 0, &err), err)
		return (100);
	id = sc3(SYS_kldload, (long)"/kmodtest.ko", 0, 0, &err);
	if (err) {
		putn("kldinit: FAIL load ", id);
		return (1);
	}
	putn("kldinit: loaded ", id);
	v = sc3(SYS_kldunload, id, 0, 0, &err);
	if (err) {
		putn("kldinit: FAIL unload ", v);
		return (2);
	}
	puts_("kldinit: unloaded\n");
	return (0);
}
