/*
 * A fake NTP server for testing dntpd -s without a network
 * (tools/arm-smoke/ntp.exp):  fakentp EPOCH
 * answers on 127.0.0.1:123 and [::1]:123 with a clock that read EPOCH
 * when it started and runs at the real rate, then goes into the
 * background.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	JAN_1970	2208988800UL	/* 1970 - 1900 in seconds */

static void
put_ts(unsigned char *p, double t)
{
	uint32_t sec = (uint32_t)((uint64_t)t + JAN_1970);
	uint32_t frac = (uint32_t)((t - (double)(uint64_t)t) * 4294967296.0);

	sec = htonl(sec);
	frac = htonl(frac);
	memcpy(p, &sec, 4);
	memcpy(p + 4, &frac, 4);
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec + tv.tv_usec / 1e6);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	struct sockaddr_storage from;
	struct pollfd pfd[2];
	unsigned char req[256], rep[48];
	socklen_t fromlen;
	double base, start, t;
	ssize_t n;
	int i, nfd;

	if (argc != 2) {
		fprintf(stderr, "usage: fakentp EPOCH\n");
		return (2);
	}
	base = strtod(argv[1], NULL);
	start = now();

	nfd = 0;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(123);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	pfd[nfd].fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (pfd[nfd].fd >= 0 &&
	    bind(pfd[nfd].fd, (struct sockaddr *)&sin, sizeof(sin)) == 0)
		nfd++;
	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_len = sizeof(sin6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(123);
	sin6.sin6_addr = in6addr_loopback;
	pfd[nfd].fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (pfd[nfd].fd >= 0 &&
	    bind(pfd[nfd].fd, (struct sockaddr *)&sin6, sizeof(sin6)) == 0)
		nfd++;
	if (nfd == 0) {
		perror("fakentp: bind");
		return (1);
	}
	if (daemon(0, 0) != 0) {
		perror("fakentp: daemon");
		return (1);
	}
	for (;;) {
		for (i = 0; i < nfd; i++)
			pfd[i].events = POLLIN;
		if (poll(pfd, nfd, -1) <= 0)
			continue;
		for (i = 0; i < nfd; i++) {
			if ((pfd[i].revents & POLLIN) == 0)
				continue;
			fromlen = sizeof(from);
			n = recvfrom(pfd[i].fd, req, sizeof(req), 0,
			    (struct sockaddr *)&from, &fromlen);
			if (n < 48)
				continue;
			t = base + (now() - start);
			memset(rep, 0, sizeof(rep));
			rep[0] = (req[0] & 0x38) | 4;	/* version, server */
			rep[1] = 2;			/* stratum */
			rep[2] = req[2];		/* poll */
			rep[3] = (unsigned char)-20;	/* precision */
			memcpy(rep + 12, "FAKE", 4);	/* refid */
			put_ts(rep + 16, t);		/* reference */
			memcpy(rep + 24, req + 40, 8);	/* originate */
			put_ts(rep + 32, t);		/* receive */
			put_ts(rep + 40, t);		/* transmit */
			sendto(pfd[i].fd, rep, sizeof(rep), 0,
			    (struct sockaddr *)&from, fromlen);
		}
	}
}
