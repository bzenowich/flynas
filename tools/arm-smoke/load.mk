# load.mk: one round of the SMP load test, 16 jobs for make -r -j8.
#	make -r -j8 -f /root/load.mk R=round
JOBS=	1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16

all: ${JOBS:S/^/j/}

.for j in ${JOBS}
j${j}:
	@/bin/sh /root/job.sh ${j} ${R}
.endfor
