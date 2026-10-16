/*
 * Reentrancy test for libpcompress (ROADMAP Phase 1).
 *
 * Runs several pcompress jobs concurrently in one process, each with its
 * own context, then sequentially reuses contexts. Each job round-trips a
 * file and compares bytes, and archives and extracts a small tree into
 * its own directory (ROADMAP P1). Modes cover zstd, lz4, lzma, adapt2,
 * dedupe, global dedupe, AES and SALSA20. Run under ASan/TSan to catch
 * shared state that only shows up as corruption.
 *
 * Usage: test_reentrant [threads] [rounds]
 */

/* nftw(); on macOS _XOPEN_SOURCE hides BYTE_ORDER, and nftw is visible anyway. */
#if !defined(_XOPEN_SOURCE) && !defined(__APPLE__)
#define	_XOPEN_SOURCE	700
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ftw.h>
#include <pcompress.h>

static char dir[200];

struct job {
	int id, rounds, failures;
	char in[240], cmp[240], out[240], pw[240], tree[240], xdir[240];
};

/* The library reads and then wipes the password file: write it each time. */
static void
write_pw(const char *path)
{
	FILE *f = fopen(path, "w");

	if (f != NULL) {
		fputs("reentrant-pw\n", f);
		fclose(f);
	}
}

/* Copy mode into buf, expanding %P to the password file. */
static void
expand(char *buf, size_t len, const char *mode, const char *pw)
{
	const char *p = strstr(mode, "%P");

	if (p == NULL)
		snprintf(buf, len, "%s", mode);
	else
		snprintf(buf, len, "%.*s%s%s", (int)(p - mode), mode, pw, p + 2);
}

/* "%P" is replaced by the job's password file (rewritten before each use). */
static const char *modes[] = {
	"-c zstd -l3 -s 1m",
	"-c lz4 -l3 -s 1m",
	"-c lzma -l6 -s 1m",
	"-c adapt2 -l6 -s 1m",
	"-c zstd -l9 -s 1m -D",
	"-c zstd -l6 -s 2m -G",
	"-c zstd -l3 -s 1m -e AES -w %P",
	"-c lz4 -l1 -s 1m -e SALSA20 -w %P",
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
rm_one(const char *p, const struct stat *sb, int flag, struct FTW *f)
{
	(void) sb; (void) flag; (void) f;
	return (remove(p));
}

static void
rm_tree(const char *p)
{
	(void) nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS);
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

/*
 * Archive the job's tree (two files) and extract it to the job's own
 * directory: extraction must not depend on the process working directory,
 * which other jobs share.
 */
static int
archive_round(struct job *j, int r)
{
	char args[2048], a[512], f[512], g[512];

	snprintf(a, sizeof (a), "%s.pz", j->tree);
	unlink(a);
	snprintf(args, sizeof (args), "pcompress -a -c %s -l3 -s 1m %s %s",
	    r % 2 ? "lz4" : "zstd", j->tree, j->tree);
	if (run(args) != 0)
		return (-1);
	/* Extracted members keep their full path below xdir. */
	snprintf(f, sizeof (f), "%s%s/a", j->xdir, j->tree);
	snprintf(g, sizeof (g), "%s%s/b", j->xdir, j->tree);
	unlink(f);
	unlink(g);
	snprintf(args, sizeof (args), "pcompress -d %s %s", a, j->xdir);
	if (run(args) != 0)
		return (-1);
	snprintf(a, sizeof (a), "%s/a", j->tree);
	if (!same_files(a, f))
		return (-1);
	snprintf(a, sizeof (a), "%s/b", j->tree);
	return (same_files(a, g) ? 0 : -1);
}

static void *
worker(void *arg)
{
	struct job *j = (struct job *)arg;
	char args[2048];
	int r;

	for (r = 0; r < j->rounds; r++) {
		char m[512];

		expand(m, sizeof (m), modes[(j->id + r) % NMODES], j->pw);
		write_pw(j->pw);

		{
			char pz[512];
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
		write_pw(j->pw);
		snprintf(args, sizeof (args), "pcompress -d -w %s %s.pz %s", j->pw,
		    j->cmp, j->out);
		if (run(args) != 0 || !same_files(j->in, j->out)) {
			fprintf(stderr, "job %d round %d: round trip failed (%s)\n", j->id, r, m);
			j->failures++;
		}
		if (archive_round(j, r) != 0) {
			fprintf(stderr, "job %d round %d: archive round trip failed\n", j->id, r);
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
		snprintf(jobs[i].pw, sizeof (jobs[i].pw), "%s/pw%d", dir, i);
		snprintf(jobs[i].tree, sizeof (jobs[i].tree), "%s/t%d", dir, i);
		snprintf(jobs[i].xdir, sizeof (jobs[i].xdir), "%s/x%d", dir, i);
		if (write_input(jobs[i].in, i, 5u * 1024 * 1024) != 0 ||
		    mkdir(jobs[i].tree, 0700) != 0 || mkdir(jobs[i].xdir, 0700) != 0) {
			perror(jobs[i].in);
			return (2);
		}
		{
			char a[512];

			snprintf(a, sizeof (a), "%s/a", jobs[i].tree);
			if (write_input(a, i + 100, 300000) != 0)
				return (2);
			snprintf(a, sizeof (a), "%s/b", jobs[i].tree);
			if (write_input(a, i + 200, 70000) != 0)
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

	rm_tree(dir);
	free(t); free(jobs);
	return (failures ? 1 : 0);
}
