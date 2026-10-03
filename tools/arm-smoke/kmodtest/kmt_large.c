/*
 * kmodtest, large code model half: every symbol address here is built
 * with MOVZ/MOVK (R_AARCH64_MOVW_UABS_G0_NC..G3).  See kmt_small.c.
 */
#include <sys/param.h>
#include <sys/systm.h>

extern int ticks;
extern int kmt_counter;

void	*kmt_large_ticks(void);
void	*kmt_large_self(void);
long	kmt_large_call(const char *);
int	kmt_large_counter(void);

void *
kmt_large_ticks(void)
{
	return (&ticks);
}

void *
kmt_large_self(void)
{
	return ((void *)kmt_large_self);
}

long
kmt_large_call(const char *s)
{
	size_t (*volatile fn)(const char *) = strlen;

	return (fn(s));
}

int
kmt_large_counter(void)
{
	return (kmt_counter);
}
