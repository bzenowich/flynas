/*
 * libm smoke test for the arm64 port: double, float and IEEE quad long
 * double (OpenBSD ld128) functions against known values, plus <fenv.h>
 * (arch/aarch64/fenv.c).  Last line is "libmtest: PASS" or
 * "libmtest: FAIL <n>".  Run under sh (it does not set up a console).
 *
 * Build: tools/arm-smoke/build-utest.sh libmtest OUT -lm
 */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fail;

#define	CHECK(n, c)	do {						\
	if (!(c)) {							\
		printf("libmtest: check %d failed: %s\n", (n), #c);	\
		if (!fail)						\
			fail = (n);					\
	}								\
} while (0)

/* Relative error within k ulps of the type's epsilon. */
#define	NEAR(a, b, eps, k)	(fabsl((long double)(a) - (long double)(b)) <= \
	    (k) * (eps) * fabsl((long double)(b)))

#define	E_L	2.718281828459045235360287471352662498L
#define	PI_L	3.141592653589793238462643383279502884L
#define	SQ2_L	1.414213562373095048801688724209698079L
#define	LN2_L	0.693147180559945309417232121458176568L

static volatile double vzero = 0.0;
static volatile long double vl;

/* Hide a constant argument from the compiler, which folds many calls. */
static long double
V(long double x)
{
	vl = x;
	return (vl);
}

static void
t_double(void)
{
	CHECK(10, NEAR(exp(V(1.0)), E_L, DBL_EPSILON, 2));
	CHECK(11, NEAR(sin(V(PI_L / 6)), 0.5L, DBL_EPSILON, 2));
	CHECK(12, NEAR(atan2(V(1.0), 1.0) * 4, PI_L, DBL_EPSILON, 2));
	CHECK(13, NEAR(pow(V(2.0), 0.5), SQ2_L, DBL_EPSILON, 2));
	CHECK(14, NEAR(log(V(2.0)), LN2_L, DBL_EPSILON, 2));
	CHECK(15, NEAR(sqrtf(V(2.0f)), SQ2_L, FLT_EPSILON, 1));
	CHECK(16, NEAR(cbrt(V(27.0)), 3.0L, DBL_EPSILON, 1));
	CHECK(17, NEAR(tgamma(V(5.0)), 24.0L, DBL_EPSILON, 4));
	CHECK(18, floor(V(-2.5)) == -3.0 && ceil(V(-2.5)) == -2.0 &&
	    round(V(2.5)) == 3.0 && rint(V(2.5)) == 2.0 && lrint(V(3.5)) == 4);
	CHECK(19, isnan(nan("")) && isinf(-1.0 / vzero) && ilogb(V(1024.0)) == 10);
	CHECK(20, NEAR(expf(V(1.0f)), E_L, FLT_EPSILON, 2) &&
	    NEAR(hypot(V(3.0), 4.0), 5.0L, DBL_EPSILON, 1));
	printf("double/float: ok\n");
}

static void
t_long(void)
{
	long double ld, ld2;
	char buf[64];
	int q;

	CHECK(30, LDBL_MANT_DIG == 113 && sizeof(long double) == 16);
	CHECK(31, NEAR(expl(V(1.0L)), E_L, LDBL_EPSILON, 2));
	CHECK(32, NEAR(sqrtl(V(2.0L)), SQ2_L, LDBL_EPSILON, 1));
	CHECK(33, NEAR(logl(V(2.0L)), LN2_L, LDBL_EPSILON, 2));
	CHECK(34, NEAR(powl(V(2.0L), 0.5L), SQ2_L, LDBL_EPSILON, 2));
	CHECK(35, NEAR(sinl(V(PI_L / 6)), 0.5L, LDBL_EPSILON, 2) &&
	    NEAR(cosl(V(PI_L / 3)), 0.5L, LDBL_EPSILON, 2));
	CHECK(36, NEAR(atanl(V(1.0L)) * 4, PI_L, LDBL_EPSILON, 2));
	CHECK(37, NEAR(tgammal(V(5.0L)), 24.0L, LDBL_EPSILON, 8) &&
	    NEAR(lgammal(V(10.0L)), 12.801827480081469611207717874566706164L,
	    LDBL_EPSILON, 8));
	CHECK(38, NEAR(cbrtl(V(27.0L)), 3.0L, LDBL_EPSILON, 1) &&
	    NEAR(expm1l(V(1e-10L)), 1.00000000005000000000166666666670833e-10L,
	    LDBL_EPSILON, 2));
	CHECK(39, floorl(V(-2.5L)) == -3.0L && ceill(V(2.5L)) == 3.0L &&
	    truncl(V(-2.7L)) == -2.0L && ilogbl(V(0x1p1000L)) == 1000);
	CHECK(40, remquol(V(10.0L), 3.0L, &q) == 1.0L && (q & 7) == 3);
	CHECK(41, fmal(V(0x1p-56L), 0x1p-56L, 1.0L) == 1.0L + LDBL_EPSILON &&
	    nextafterl(V(1.0L), 2.0L) == 1.0L + LDBL_EPSILON);
	CHECK(42, nexttowardf(V(1.0f), 2.0L) == 1.0f + FLT_EPSILON &&
	    nexttoward(V(1.0), 0.0L) == 1.0 - DBL_EPSILON / 2);
	/* sqrtl (src/ld128/e_sqrtl.c): exact squares, subnormals, rounding. */
	CHECK(44, sqrtl(V((0x1p56L + 1) * (0x1p56L + 1))) == 0x1p56L + 1 &&
	    sqrtl(V(0x1p-16494L)) == 0x1p-8247L &&
	    sqrtl(V(9 * 0x1p-16480L)) == 3 * 0x1p-8240L && sqrtl(V(-0.0L)) == 0 && signbit(sqrtl(V(-0.0L))) &&
	    isnan(sqrtl(V(-1.0L))) && isinf(sqrtl(V((long double)INFINITY))));
	fesetround(FE_UPWARD);
	ld = sqrtl(V(2.0L));
	fesetround(FE_TOWARDZERO);
	ld2 = sqrtl(V(2.0L));
	fesetround(FE_TONEAREST);
	CHECK(45, ld == nextafterl(ld2, 2.0L) &&
	    (ld == sqrtl(V(2.0L)) || ld2 == sqrtl(V(2.0L))));
	snprintf(buf, sizeof(buf), "%.30Lf", sqrtl(V(2.0L)));
	CHECK(43, strcmp(buf, "1.414213562373095048801688724210") == 0);
	printf("long double: %s\n", buf);
}

/*
 * Every long double function against its double version: a gross bug in
 * the ld128 code (wrong exponent, sign, mantissa layout) shows as a
 * disagreement far above double precision.
 */
typedef long double (*lf1)(long double);
typedef double (*df1)(double);
typedef long double (*lf2)(long double, long double);
typedef double (*df2)(double, double);

static const struct {
	const char *name;
	lf1 l;
	df1 d;
	double lo, hi;		/* input range */
} f1[] = {
	{ "acosl", acosl, acos, -1, 1 }, { "asinl", asinl, asin, -1, 1 },
	{ "atanl", atanl, atan, -50, 50 }, { "cosl", cosl, cos, -100, 100 },
	{ "sinl", sinl, sin, -100, 100 }, { "tanl", tanl, tan, -1.5, 1.5 },
	{ "coshl", coshl, cosh, -20, 20 }, { "sinhl", sinhl, sinh, -20, 20 },
	{ "tanhl", tanhl, tanh, -5, 5 }, { "acoshl", acoshl, acosh, 1, 1e6 },
	{ "asinhl", asinhl, asinh, -1e6, 1e6 },
	{ "atanhl", atanhl, atanh, -0.99, 0.99 },
	{ "expl", expl, exp, -700, 700 }, { "exp2l", exp2l, exp2, -1000, 1000 },
	{ "expm1l", expm1l, expm1, -5, 5 }, { "logl", logl, log, 1e-300, 1e300 },
	{ "log10l", log10l, log10, 1e-300, 1e300 },
	{ "log2l", log2l, log2, 1e-300, 1e300 },
	{ "log1pl", log1pl, log1p, -0.9, 100 },
	{ "sqrtl", sqrtl, sqrt, 0, 1e300 }, { "cbrtl", cbrtl, cbrt, -1e300, 1e300 },
	{ "erfl", erfl, erf, -4, 4 }, { "erfcl", erfcl, erfc, -4, 20 },
	{ "lgammal", lgammal, lgamma, 0.01, 100 },
	{ "tgammal", tgammal, tgamma, 0.01, 100 },
	{ "ceill", ceill, ceil, -1e5, 1e5 }, { "floorl", floorl, floor, -1e5, 1e5 },
	{ "truncl", truncl, trunc, -1e5, 1e5 }, { "roundl", roundl, round, -1e5, 1e5 },
	{ "rintl", rintl, rint, -1e5, 1e5 }, { "logbl", logbl, logb, -1e300, 1e300 },
	{ "fabsl", fabsl, fabs, -1e300, 1e300 },
};

static const struct {
	const char *name;
	lf2 l;
	df2 d;
	double lo, hi;
} f2[] = {
	{ "atan2l", atan2l, atan2, -10, 10 }, { "powl", powl, pow, 0.01, 30 },
	{ "hypotl", hypotl, hypot, -1e10, 1e10 },
	{ "fmodl", fmodl, fmod, -1e6, 1e6 },
	{ "remainderl", remainderl, remainder, -1e6, 1e6 },
	{ "nextafterl", nextafterl, nextafter, -10, 10 },
	{ "copysignl", copysignl, copysign, -10, 10 },
	{ "fmaxl", fmaxl, fmax, -10, 10 }, { "fminl", fminl, fmin, -10, 10 },
};

#define	NPTS	97

/* Points spread over [lo, hi]: linear, plus a log sweep for wide ranges. */
static double
pt(double lo, double hi, int i)
{
	if (lo >= 0 && hi / (lo > 0 ? lo : 1e-300) > 1e6 && (i & 1))
		return (lo > 0 ? lo : 1e-300) *
		    pow(hi / (lo > 0 ? lo : 1e-300), (double)i / NPTS);
	return (lo + (hi - lo) * ((double)i / NPTS + 0.0013 * (i % 7)) /
	    1.01);
}

static int
agree(long double l, double d)
{
	long double tol;

	if (isnan(d))
		return (isnan(l));
	if (isinf(d))
		return (isinf(l) && signbit(l) == signbit(d));
	tol = 8 * DBL_EPSILON * fabsl((long double)d);
	if (tol < 1e-300L)
		tol = 1e-300L;
	return (fabsl(l - d) <= tol);
}

static void
t_vs_double(void)
{
	unsigned i;
	int j, bad;
	double x, y, d;
	long double l;

	for (i = 0; i < sizeof(f1) / sizeof(f1[0]); i++) {
		bad = 0;
		for (j = 0; j <= NPTS; j++) {
			x = pt(f1[i].lo, f1[i].hi, j);
			d = f1[i].d(x);
			l = f1[i].l(x);
			if (!agree(l, d) && bad++ == 0)
				printf("libmtest: %s(%.17g) = %.21Lg, double "
				    "%.17g\n", f1[i].name, x, l, d);
		}
		CHECK(70 + (int)i, bad == 0);
	}
	for (i = 0; i < sizeof(f2) / sizeof(f2[0]); i++) {
		bad = 0;
		for (j = 0; j <= NPTS; j++) {
			x = pt(f2[i].lo, f2[i].hi, j);
			y = pt(f2[i].lo, f2[i].hi, (j * 37 + 11) % (NPTS + 1));
			d = f2[i].d(x, y);
			l = f2[i].l(x, y);
			/* nextafterl moves by a quad ulp, not a double one. */
			if (f2[i].l == nextafterl)
				d = x;
			if (!agree(l, d) && bad++ == 0)
				printf("libmtest: %s(%.17g, %.17g) = %.21Lg, "
				    "double %.17g\n", f2[i].name, x, y, l, d);
		}
		CHECK(110 + (int)i, bad == 0);
	}
	printf("long double vs double: %d+%d functions\n",
	    (int)(sizeof(f1) / sizeof(f1[0])), (int)(sizeof(f2) / sizeof(f2[0])));
}

/* Identities that hold to (nearly) full quad precision. */
static void
t_long_identities(void)
{
	long double x, r;
	int j, q, bad[8] = { 0 };

	for (j = 1; j <= NPTS; j++) {
		x = (long double)j * 1.37L + 1 / 3.0L;
		r = sqrtl(x);
		bad[0] += !NEAR(r * r, x, LDBL_EPSILON, 2);
		r = cbrtl(-x);
		bad[1] += !NEAR(r * r * r, -x, LDBL_EPSILON, 4);
		bad[2] += !NEAR(expl(logl(x)), x, LDBL_EPSILON, 8 * j);
		r = sinl(x);
		bad[3] += !NEAR(r * r + cosl(x) * cosl(x), 1.0L, LDBL_EPSILON, 4);
		bad[4] += !NEAR(powl(x, 3.0L), x * x * x, LDBL_EPSILON, 4);
		r = remquol(x * 7, x, &q);
		bad[5] += !(fabsl(r) < LDBL_EPSILON * 64 * x && (q & 7) == 7);
		r = remquol(x * 7 + 0.25L * x, -x, &q);
		bad[6] += !(NEAR(r, 0.25L * x, LDBL_EPSILON, 64) && q == -7);
		bad[7] += !(fmodl(x * 5 + 1, x) == fmodl(x * 5 + 1, -x));
	}
	for (j = 0; j < 8; j++)
		CHECK(130 + j, bad[j] == 0);
	printf("long double identities: ok\n");
}

static void
t_fenv(void)
{
	volatile double a = 1.0, b = 3.0, r1, r2;
	fenv_t env;

	CHECK(50, feclearexcept(FE_ALL_EXCEPT) == 0 &&
	    fetestexcept(FE_ALL_EXCEPT) == 0);
	r1 = a / vzero;
	CHECK(51, isinf(r1) && fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO);
	r1 = a / b;
	CHECK(52, fetestexcept(FE_INEXACT) != 0);
	CHECK(53, fegetround() == FE_TONEAREST);
	CHECK(54, fegetenv(&env) == 0 && fesetround(FE_UPWARD) == 0 &&
	    fegetround() == FE_UPWARD);
	r2 = a / b;
	CHECK(55, r2 > r1);
	fesetround(FE_DOWNWARD);
	r1 = a / b;
	CHECK(56, r1 < r2);
	CHECK(57, rint(V(2.5)) == 2.0 && rint(V(-2.5)) == -3.0 &&
	    rintl(V(-2.5L)) == -3.0L);
	CHECK(58, fesetenv(&env) == 0 && fegetround() == FE_TONEAREST);
	feclearexcept(FE_ALL_EXCEPT);
	CHECK(59, feraiseexcept(FE_OVERFLOW) == 0 &&
	    fetestexcept(FE_ALL_EXCEPT) == FE_OVERFLOW);
	feclearexcept(FE_ALL_EXCEPT);
	printf("fenv: ok\n");
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	t_double();
	t_long();
	t_vs_double();
	t_long_identities();
	t_fenv();
	printf("libmtest: %s", fail ? "FAIL" : "PASS");
	if (fail)
		printf(" %d", fail);
	printf("\n");
	return (fail);
}
