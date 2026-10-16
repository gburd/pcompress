/*
 * Reentrancy test for libpcompress (ROADMAP Phase 1).
 *
 * Runs several pcompress jobs concurrently in one process, each with its
 * own context, then sequentially reuses contexts. Each job round-trips a
 * file and compares bytes (ROADMAP P1). Run under ASan/TSan to catch
 * shared state that only shows up as corruption.
 *
 * Usage: test_reentrant [threads] [rounds]
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <pcompress.h>

static char dir[256];

struct job {
	int id, rounds, failures;
	char in[320], cmp[340], out[340];
};

static const char *modes[] = {
	"-c zstd -l3 -s 1m",
	"-c lz4 -l3 -s 1m",
	"-c lzma -l6 -s 1m",
	"-c adapt2 -l6 -s 1m",
	"-c zstd -l9 -s 1m -D",
	"-c zstd -l6 -s 2m -G",
};
#define	NMODES	(sizeof (modes) / sizeof (modes[0]))

static int
write_input(const char *path, int seed, size_t n)
{
	FILE *f = fopen(path, "wb");
	unsigned int s = (unsigned int)seed * 2654435761u + 1;
	size_t i;

	if (f == NULL)
		return (-1);
	for (i = 0; i < n; i++) {
		s = s * 1103515245u + 12345u;
		/* Half text-like, half noise: exercises several code paths. */
		fputc((i / 4096) % 2 ? (int)(s >> 16) : (int)("abcdefgh "[s % 9]), f);
	}
	return (fclose(f));
}

static int
same_files(const char *a, const char *b)
{
	FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
	int ca, cb, same = (fa && fb);

	while (same && (ca = fgetc(fa)) != EOF) {
		cb = fgetc(fb);
		if (ca != cb)
			same = 0;
	}
	if (same && fgetc(fb) != EOF)
		same = 0;
	if (fa) fclose(fa);
	if (fb) fclose(fb);
	return (same);
}

static int
run(char *args)
{
	pc_ctx_t *p = create_pc_context();
	int r;

	if (p == NULL)
		return (-1);
	r = init_pc_context_argstr(p, args);
	if (r == 0)
		r = start_pcompress(p);
	destroy_pc_context(p);
	return (r);
}

static void *
worker(void *arg)
{
	struct job *j = (struct job *)arg;
	char args[1024];
	int r;

	for (r = 0; r < j->rounds; r++) {
		const char *m = modes[(j->id + r) % NMODES];

		{
			char pz[400];
			snprintf(pz, sizeof (pz), "%s.pz", j->cmp);
			unlink(pz);	/* pcompress refuses to overwrite <target>.pz */
		}
		unlink(j->out);
		snprintf(args, sizeof (args), "pcompress %s %s %s", m, j->in, j->cmp);
		if (run(args) != 0) {
			fprintf(stderr, "job %d round %d: compress failed (%s)\n", j->id, r, m);
			j->failures++;
			continue;
		}
		snprintf(args, sizeof (args), "pcompress -d %s.pz %s", j->cmp, j->out);
		if (run(args) != 0 || !same_files(j->in, j->out)) {
			fprintf(stderr, "job %d round %d: round trip failed (%s)\n", j->id, r, m);
			j->failures++;
		}
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	int nthreads = argc > 1 ? atoi(argv[1]) : 4;
	int rounds = argc > 2 ? atoi(argv[2]) : 6;
	pthread_t *t;
	struct job *jobs;
	int i, failures = 0;

	snprintf(dir, sizeof (dir), "%s/pc_reentrant_%d",
	    getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
	if (mkdir(dir, 0700) != 0) {
		perror(dir);
		return (2);
	}
	t = calloc(nthreads, sizeof (*t));
	jobs = calloc(nthreads, sizeof (*jobs));
	for (i = 0; i < nthreads; i++) {
		jobs[i].id = i;
		jobs[i].rounds = rounds;
		snprintf(jobs[i].in, sizeof (jobs[i].in), "%s/in%d", dir, i);
		snprintf(jobs[i].cmp, sizeof (jobs[i].cmp), "%s/c%d", dir, i);
		snprintf(jobs[i].out, sizeof (jobs[i].out), "%s/o%d", dir, i);
		if (write_input(jobs[i].in, i, 5u * 1024 * 1024) != 0) {
			perror(jobs[i].in);
			return (2);
		}
	}

	/* 1. Sequential reuse: one context after another in one thread. */
	jobs[0].rounds = (int)NMODES;
	worker(&jobs[0]);
	failures += jobs[0].failures;
	jobs[0].failures = 0;
	jobs[0].rounds = rounds;
	printf("sequential: %s\n", failures ? "FAIL" : "PASS");

	/* 2. Concurrent contexts. */
	for (i = 0; i < nthreads; i++)
		pthread_create(&t[i], NULL, worker, &jobs[i]);
	for (i = 0; i < nthreads; i++) {
		pthread_join(t[i], NULL);
		failures += jobs[i].failures;
	}
	printf("concurrent (%d threads x %d rounds): %s\n", nthreads, rounds,
	    failures ? "FAIL" : "PASS");

	for (i = 0; i < nthreads; i++) {
		char p[400];
		unlink(jobs[i].in); unlink(jobs[i].out);
		snprintf(p, sizeof (p), "%s.pz", jobs[i].cmp); unlink(p);
	}
	rmdir(dir);
	free(t); free(jobs);
	return (failures ? 1 : 0);
}
