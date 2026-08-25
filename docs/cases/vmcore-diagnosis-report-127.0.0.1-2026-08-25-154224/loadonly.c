/*
 * loadonly_tear.c v2 — storeless L1D read-path tear hunter
 *
 * Hot loop emits EXACTLY three ldr per element via inline asm (no stores,
 * no spills, no CSE): two reloads of A[i] + one golden load G[i].
 * Any mismatch => a pure load returned wrong bytes; forwarding path not
 * involved (no store anywhere near these lines since init).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define ELEMS (64 * 1024 / 8)
static uint64_t A[ELEMS], G[ELEMS];

static inline uint64_t ld(const uint64_t *p)
{
	uint64_t v;
	asm volatile("ldr %0, [%1]" : "=r"(v) : "r"(p));
	return v;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 60;
	struct timespec ts;
	uint64_t i, iter = 0, fails = 0;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	time_t end = ts.tv_sec + secs;

	for (i = 0; i < ELEMS; i++) {           /* one-time fill only */
		A[i] = 0x9E3779B97F4A7C15ull * (i + 0x1234567);
		G[i] = A[i];
	}
	asm volatile("" ::: "memory");          /* publish fills */

	while (1) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (ts.tv_sec >= end) break;

		for (i = 0; i < ELEMS; i++) {
			uint64_t v1 = ld(&A[i]);
			uint64_t v2 = ld(&A[i]);
			uint64_t g  = ld(&G[i]);
			if (__builtin_expect(v1 != g || v2 != g, 0)) {
				printf("TEAR iter=%llu idx=%llu v1=%016llx v2=%016llx g=%016llx "
				       "xor1=%016llx xor2=%016llx\n",
				       (unsigned long long)iter, (unsigned long long)i,
				       (unsigned long long)v1, (unsigned long long)v2,
				       (unsigned long long)g,
				       (unsigned long long)(v1 ^ g),
				       (unsigned long long)(v2 ^ g));
				fflush(stdout);
				fails++;
			}
		}
		iter++;
	}
	printf("done iters=%llu fails=%llu\n", (unsigned long long)iter,
	       (unsigned long long)fails);
	return fails ? 1 : 0;
}
