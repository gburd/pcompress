/*
 * pcbackup: a minimal snapshot backup tool on the public libpcompress API
 * (ROADMAP Phase 7). It exists to prove the API is enough for an Arq-like
 * program; it is not a product: no remote stores, no scheduling, no locks
 * (one pcbackup at a time per repository).
 *
 *   pcbackup init     <repo>                     password from PCBACKUP_PASSWORD
 *   pcbackup backup   <repo> <dir>               prints the snapshot name
 *   pcbackup list     <repo> [<snapshot>]        snapshots, or one's entries
 *   pcbackup restore  <repo> <snapshot> <dest> [<path>]   all, or one entry;
 *                                                <dest> must not exist
 *   pcbackup forget   <repo> <snapshot>
 *   pcbackup prune    <repo>                     delete unreferenced objects
 *
 * Repository layout:
 *   <repo>/keyfile           pc_keyfile_create(): the key, wrapped by the password
 *   <repo>/objects/<id>      sealed chunks; <id> = keyed pc_chunk_id(), hex
 *   <repo>/snapshots/<time>-<id>  a sealed, encoded pc_manifest_t
 *   <repo>/index             pc_index_t of stored objects (a cache: prune rebuilds it)
 *
 * Snapshot times are in their names and not authenticated (the id is): a
 * store can make an old snapshot look new, not change its contents.
 *
 * Restore checks every object, snapshots included, against the id in its
 * name (an object does not say which chunk it is), so storage cannot swap
 * objects. It cannot write outside <dest>: <dest> is new, and the manifest
 * decoder refuses "..", absolute paths and entries under a symlink entry,
 * so every path below <dest> is one restore itself created as a directory.
 * Not restored: owners (needs root), hard links, special files.
 *
 *   cc $(pkg-config --cflags libpcompress) pcbackup.c -o pcbackup $(pkg-config --libs libpcompress)
 */
#define	_GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pcompress/pcompress.h>

#ifdef __APPLE__
#define	st_mtim	st_mtimespec
#endif

#define	AVG_CHUNK	(64 * 1024)
#define	MAX_CHUNK	(256 * 1024)
#define	MAX_OBJECT	(256 * 1024 * 1024)	/* manifests are the big ones */

static unsigned char key[32];
static pc_ctx_t *ctx;
static pc_index_t *idx;
static char repo[4096];
static long n_stored, n_dedup;

static void die(const char *) __attribute__((noreturn));

static void
die(const char *what)
{
	fprintf(stderr, "pcbackup: %s%s%s\n", what, errno ? ": " : "", errno ? strerror(errno) : "");
	exit(1);
}

/* snprintf that refuses to truncate: a cut-off path names another file. */
static void
xpath(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	if (r < 0 || (size_t)r >= n) {
		errno = ENAMETOOLONG;
		die("path");
	}
}

static void
hex(const unsigned char id[32], char out[65])
{
	int i;

	for (i = 0; i < 32; i++)
		sprintf(out + 2 * i, "%02x", id[i]);
}

static void *
read_all(const char *path, size_t *len)
{
	/* Repository files are regular files we wrote: no links, FIFOs or
	 * devices (which could block or point elsewhere), and bounded. */
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	unsigned char *b;
	struct stat st;
	ssize_t r;

	if (fd < 0)
		return (NULL);
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAX_OBJECT ||
	    (b = malloc(st.st_size ? (size_t)st.st_size : 1)) == NULL) {
		close(fd);
		return (NULL);
	}
	r = read(fd, b, (size_t)st.st_size);
	close(fd);
	if (r != (ssize_t)st.st_size) {
		free(b);
		return (NULL);
	}
	*len = (size_t)r;
	return (b);
}

/* fsync a directory, so renames into it survive a power loss. */
static void
sync_dir(const char *dir)
{
	char p[4200];
	int fd;

	xpath(p, sizeof (p), "%s/%s", repo, dir);
	if ((fd = open(p, O_RDONLY | O_DIRECTORY)) < 0 || fsync(fd) != 0)
		die(p);
	close(fd);
}

/* Write via a temporary name and rename, so a crash leaves no half object. */
static int
write_atomic(const char *path, const void *buf, size_t len)
{
	char tmp[4200];
	int fd;

	xpath(tmp, sizeof (tmp), "%s.tmp.%d", path, (int)getpid());
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0)
		return (-1);
	if (write(fd, buf, len) != (ssize_t)len || fsync(fd) != 0 || close(fd) != 0 ||
	    rename(tmp, path) != 0) {
		unlink(tmp);
		return (-1);
	}
	return (0);
}

static const char *
password(void)
{
	const char *pw = getenv("PCBACKUP_PASSWORD");

	if (pw == NULL || *pw == '\0') {
		errno = 0;
		die("set PCBACKUP_PASSWORD");
	}
	return (pw);
}

static void
open_repo(const char *r, int need_index)
{
	char p[4200];
	unsigned char *kf;
	size_t len;
	const char *pw = password();

	xpath(repo, sizeof (repo), "%s", r);
	xpath(p, sizeof (p), "%s/keyfile", repo);
	if ((kf = read_all(p, &len)) == NULL)
		die("cannot read keyfile");
	errno = 0;
	if (pc_keyfile_open(kf, len, pw, strlen(pw), key) != 0)
		die("wrong password or damaged keyfile");
	free(kf);
	if ((ctx = pc_new()) == NULL || pc_set_key(ctx, key, 32) != 0 ||
	    pc_set_algorithm(ctx, "zstd") != 0 || pc_set_level(ctx, 6) != 0)
		die("cannot set up libpcompress");
	pc_set_log(ctx, NULL, NULL, PC_LOG_NONE);
	if (need_index) {
		xpath(p, sizeof (p), "%s/index", repo);
		if ((idx = pc_index_open(p)) == NULL)
			die("cannot open index");
	}
}

static void
close_repo(void)
{
	if (idx != NULL && pc_index_close(idx) != 0)
		die("cannot write index");
	pc_free(ctx);
	memset(key, 0, sizeof (key));
}

/* Seal buf and store it as <repo>/<dir>/<name>. */
static void
put_object(const char *dir, const char *name, const void *buf, size_t len)
{
	char p[4200];
	size_t ol = pc_chunk_seal_bound(len);
	unsigned char *obj = malloc(ol);

	if (obj == NULL || pc_chunk_seal(ctx, buf, len, obj, &ol) != 0)
		die(pc_last_error(ctx));
	xpath(p, sizeof (p), "%s/%s/%s", repo, dir, name);
	if (write_atomic(p, obj, ol) != 0)
		die(p);
	free(obj);
}

/* Fetch and open an object; check it is the chunk its name says. */
static unsigned char *
get_object(const char *dir, const char *name, const unsigned char *want, size_t wantlen,
    size_t *len)
{
	char p[4200];
	unsigned char *obj, *out, id[32];
	size_t ol, pl;

	xpath(p, sizeof (p), "%s/%s/%s", repo, dir, name);
	if ((obj = read_all(p, &ol)) == NULL)
		die(p);
	/* The header's size is not yet authenticated: allocate only what we expect. */
	pl = pc_chunk_open_size(obj, ol);
	errno = 0;
	if (pl == (size_t)-1 || (wantlen != 0 && pl != wantlen) || pl > MAX_OBJECT ||
	    (out = malloc(pl ? pl : 1)) == NULL ||
	    pc_chunk_open(ctx, obj, ol, out, &pl) != 0) {
		fprintf(stderr, "pcbackup: %s: %s\n", p, pc_last_error(ctx));
		exit(1);
	}
	free(obj);
	pc_chunk_id(out, pl, key, 32, id);
	if (memcmp(id, want, 32) != 0) {
		fprintf(stderr, "pcbackup: %s: object is not the one its name says\n", p);
		exit(1);
	}
	*len = pl;
	return (out);
}

/* --- backup ------------------------------------------------------------- */

struct ids {
	unsigned char *ids;
	uint32_t *lens;
	size_t n, cap;
};

static void
add_chunk(struct ids *v, const unsigned char *buf, size_t len)
{
	unsigned char id[32];
	char name[65];

	if (v->n == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 16;
		if ((v->ids = realloc(v->ids, v->cap * 32)) == NULL ||
		    (v->lens = realloc(v->lens, v->cap * sizeof (uint32_t))) == NULL)
			die("out of memory");
	}
	pc_chunk_id(buf, len, key, 32, id);
	memcpy(v->ids + 32 * v->n, id, 32);
	v->lens[v->n++] = (uint32_t)len;
	if (pc_index_has(idx, id) == 1) {
		n_dedup++;
		return;
	}
	hex(id, name);
	put_object("objects", name, buf, len);
	/* Only after the object is written: the index must not claim a missing one. */
	if (pc_index_add(idx, id) < 0)
		die("index");
	n_stored++;
}

static void
chunk_file(int fd, struct ids *v, uint64_t *size)
{
	static unsigned char buf[4 * MAX_CHUNK];
	size_t lens[4 * MAX_CHUNK / 64 + 1], have = 0;
	pc_chunker_t *ch = pc_chunker_new(AVG_CHUNK, 0, MAX_CHUNK);
	ssize_t r;
	int n, k;

	if (ch == NULL)
		die("chunker");
	*size = 0;
	for (;;) {
		size_t off = 0;

		r = read(fd, buf + have, sizeof (buf) - have);
		if (r < 0)
			die("read");
		n = r > 0 ? pc_chunker_feed(ch, buf + have, (size_t)r, lens, 4 * MAX_CHUNK / 64 + 1) :
		    pc_chunker_finish(ch, lens, 1);
		if (n < 0)
			die("chunker");
		have += (size_t)r;
		*size += (uint64_t)r;
		for (k = 0; k < n; k++) {
			add_chunk(v, buf + off, lens[k]);
			off += lens[k];
		}
		memmove(buf, buf + off, have - off);
		have -= off;
		if (r == 0)
			break;
	}
	pc_chunker_free(ch);
}

static void
walk(pc_manifest_t *m, const char *root, const char *rel)
{
	char p[4200], q[4200];
	DIR *d;
	struct dirent *de;

	xpath(p, sizeof (p), "%s%s%s", root, *rel ? "/" : "", rel);
	if ((d = opendir(p)) == NULL)
		die(p);
	while ((de = readdir(d)) != NULL) {
		struct stat st;
		pc_entry_t e;
		struct ids v = { 0 };
		char target[4096];
		ssize_t tl;
		int fd;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		xpath(q, sizeof (q), "%s%s%s", rel, *rel ? "/" : "", de->d_name);
		xpath(p, sizeof (p), "%s/%s", root, q);
		if (lstat(p, &st) != 0)
			die(p);
		memset(&e, 0, sizeof (e));
		e.path = q;
		e.mode = st.st_mode & 07777;
		e.uid = st.st_uid;
		e.gid = st.st_gid;
		e.mtime = st.st_mtim.tv_sec;
		e.mtime_nsec = (uint32_t)st.st_mtim.tv_nsec;
		if (S_ISREG(st.st_mode)) {
			e.type = PC_ENTRY_FILE;
			if ((fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)) < 0 ||
			    fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
				die(p);	/* replaced since lstat() */
			chunk_file(fd, &v, &e.size);
			close(fd);
			e.nchunks = v.n;
			e.ids = v.ids;
			e.lens = v.lens;
		} else if (S_ISDIR(st.st_mode)) {
			e.type = PC_ENTRY_DIR;
		} else if (S_ISLNK(st.st_mode)) {
			e.type = PC_ENTRY_SYMLINK;
			if ((tl = readlink(p, target, sizeof (target) - 1)) <= 0)
				die(p);
			target[tl] = '\0';
			e.target = target;
		} else {
			fprintf(stderr, "pcbackup: skipping special file %s\n", p);
			continue;
		}
		if (pc_manifest_add(m, &e) != 0) {
			errno = 0;
			die(q);
		}
		free(v.ids);
		free(v.lens);
		if (e.type == PC_ENTRY_DIR)
			walk(m, root, q);
	}
	closedir(d);
}

static int
cmd_backup(const char *r, const char *dir)
{
	pc_manifest_t *m = pc_manifest_new();
	unsigned char *buf, id[32];
	char name[96], h[65];
	size_t len;
	struct timespec ts;

	open_repo(r, 1);
	walk(m, dir, "");
	if (pc_manifest_encode(m, NULL, &len) != 0 || (buf = malloc(len)) == NULL ||
	    pc_manifest_encode(m, buf, &len) != 0)
		die("manifest");
	/* <time>-<id>: sorts by time, and restore checks the id. */
	clock_gettime(CLOCK_REALTIME, &ts);
	pc_chunk_id(buf, len, key, 32, id);
	hex(id, h);
	xpath(name, sizeof (name), "%010lld.%09ld-%s", (long long)ts.tv_sec, ts.tv_nsec, h);
	/* Objects durable first, then the index, then the snapshot naming them. */
	sync_dir("objects");
	if (pc_index_sync(idx) != 0)
		die("index");
	put_object("snapshots", name, buf, len);
	sync_dir("snapshots");
	printf("%s\n", name);
	fprintf(stderr, "pcbackup: %zu entries, %ld chunks stored, %ld already present\n",
	    pc_manifest_count(m), n_stored, n_dedup);
	free(buf);
	pc_manifest_free(m);
	close_repo();
	return (0);
}

/* --- reading snapshots -------------------------------------------------- */

/* Parse 64 hex digits into id. */
static int
parse_id(const char *s, unsigned char id[32])
{
	int i;

	if (strspn(s, "0123456789abcdef") != 64 || s[64] != '\0')
		return (0);
	for (i = 0; i < 32; i++)
		sscanf(s + 2 * i, "%2hhx", &id[i]);
	return (1);
}

/* <digits>.<digits>-<64 hex>: anything else in snapshots/ is not ours. */
static int
valid_name(const char *s, unsigned char id[32])
{
	const char *d = strchr(s, '-');

	return (d != NULL && d > s && strspn(s, "0123456789.") == (size_t)(d - s) &&
	    parse_id(d + 1, id));
}

static pc_manifest_t *
load_snapshot(const char *name)
{
	unsigned char *buf, id[32];
	size_t len;
	pc_manifest_t *m;

	if (!valid_name(name, id)) {
		errno = 0;
		die("bad snapshot name");
	}
	buf = get_object("snapshots", name, id, 0, &len);
	if ((m = pc_manifest_decode(buf, len)) == NULL) {
		errno = 0;
		die("snapshot manifest is invalid");
	}
	free(buf);
	return (m);
}

static int
cmd_list(const char *r, const char *snap)
{
	pc_manifest_t *m;
	pc_entry_t e;
	size_t i;

	open_repo(r, 0);
	if (snap == NULL) {
		char p[4200];
		struct dirent **v;
		int n, k;

		xpath(p, sizeof (p), "%s/snapshots", repo);
		if ((n = scandir(p, &v, NULL, alphasort)) < 0)
			die(p);
		for (k = 0; k < n; k++) {
			unsigned char id[32];

			if (valid_name(v[k]->d_name, id))
				printf("%s\n", v[k]->d_name);
			free(v[k]);
		}
		free(v);
	} else {
		m = load_snapshot(snap);
		for (i = 0; pc_manifest_get(m, i, &e) == 0; i++)
			printf("%c %06o %10llu %s%s%s\n", "?fdl"[e.type], (unsigned)e.mode,
			    (unsigned long long)e.size, e.path, e.target ? " -> " : "",
			    e.target ? e.target : "");
		pc_manifest_free(m);
	}
	close_repo();
	return (0);
}

/* Create one entry below dfd. Parents exist (manifest order) and are ours. */
static void
restore_entry(int dfd, const pc_entry_t *e)
{
	size_t k;
	int fd;

	/*
	 * Everything is created new (EEXIST is an error), so a path that a
	 * case-insensitive file system folds onto an earlier one, say a
	 * symlink, stops the restore instead of being followed.
	 */
	if (e->type == PC_ENTRY_DIR) {
		if (mkdirat(dfd, e->path, 0700) != 0)
			die(e->path);
		return;
	}
	if (e->type == PC_ENTRY_SYMLINK) {
		if (symlinkat(e->target, dfd, e->path) != 0)
			die(e->path);
		return;
	}
	/* O_NOFOLLOW|O_EXCL: never write through a link or over an existing file. */
	if ((fd = openat(dfd, e->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600)) < 0)
		die(e->path);
	for (k = 0; k < e->nchunks; k++) {
		char name[65];
		size_t len;
		unsigned char *buf;

		hex(e->ids + 32 * k, name);
		buf = get_object("objects", name, e->ids + 32 * k, e->lens[k], &len);
		if (len != e->lens[k] || write(fd, buf, len) != (ssize_t)len)
			die(e->path);
		free(buf);
	}
	/* Owners are not restored, so neither are setuid/setgid bits. */
	if (fchmod(fd, e->mode & 01777) != 0 || close(fd) != 0)
		die(e->path);
}

/* A restored directory's mode, without following a link put in its place. */
static void
dir_mode(int dfd, const pc_entry_t *e)
{
	int fd = openat(dfd, e->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

	if (fd < 0 || fchmod(fd, e->mode & 01777) != 0)
		die(e->path);
	close(fd);
}

static void
set_times(int dfd, const pc_entry_t *e)
{
	struct timespec t[2];

	t[0].tv_sec = t[1].tv_sec = (time_t)e->mtime;
	t[0].tv_nsec = t[1].tv_nsec = e->mtime_nsec;
	(void) utimensat(dfd, e->path, t, AT_SYMLINK_NOFOLLOW);
}

static int
cmd_restore(const char *r, const char *snap, const char *dest, const char *one)
{
	pc_manifest_t *m;
	pc_entry_t e;
	size_t i;
	int dfd;

	open_repo(r, 0);
	m = load_snapshot(snap);
	if (mkdir(dest, 0700) != 0)
		die(dest);	/* must be new: nothing below it we did not create */
	if ((dfd = open(dest, O_RDONLY | O_DIRECTORY)) < 0)
		die(dest);
	if (one != NULL) {
		/* One entry: create its parent directories, then it. */
		char p[4096];
		size_t j;

		if (pc_manifest_find(m, one, &e) < 0) {
			errno = 0;
			die("no such path in snapshot");
		}
		xpath(p, sizeof (p), "%s", one);
		for (j = 1; p[j]; j++) {
			if (p[j] != '/')
				continue;
			p[j] = '\0';
			if (mkdirat(dfd, p, 0700) != 0 && errno != EEXIST)
				die(p);
			p[j] = '/';
		}
		restore_entry(dfd, &e);
		if (e.type == PC_ENTRY_DIR)
			dir_mode(dfd, &e);
		set_times(dfd, &e);
	} else {
		for (i = 0; pc_manifest_get(m, i, &e) == 0; i++)
			restore_entry(dfd, &e);
		/* Directory modes and times last, deepest first, once their contents exist. */
		for (i = pc_manifest_count(m); i-- > 0; ) {
			pc_manifest_get(m, i, &e);
			if (e.type == PC_ENTRY_DIR)
				dir_mode(dfd, &e);
			set_times(dfd, &e);
		}
	}
	close(dfd);
	pc_manifest_free(m);
	close_repo();
	return (0);
}

static int
cmd_forget(const char *r, const char *snap)
{
	char p[4200];
	unsigned char id[32];

	open_repo(r, 0);
	if (!valid_name(snap, id)) {
		errno = 0;
		die("bad snapshot name");
	}
	xpath(p, sizeof (p), "%s/snapshots/%s", repo, snap);
	if (unlink(p) != 0)
		die(p);
	close_repo();
	return (0);
}

/* --- prune -------------------------------------------------------------- */

static int
cmp_id(const void *a, const void *b)
{
	return (memcmp(a, b, 32));
}

/*
 * Mark every chunk the snapshots use, delete other objects, and rebuild
 * the index from what is left (it is only a cache of the objects/ dir).
 */
static int
cmd_prune(const char *r)
{
	char p[4200], tmpidx[4200];
	unsigned char *live = NULL, id[32];
	size_t nlive = 0, cap = 0, i;
	struct dirent *de;
	DIR *d;
	long removed = 0, kept = 0;
	pc_index_t *ni;

	open_repo(r, 0);
	xpath(p, sizeof (p), "%s/snapshots", repo);
	if ((d = opendir(p)) == NULL)
		die(p);
	while ((de = readdir(d)) != NULL) {
		pc_manifest_t *m;
		pc_entry_t e;

		if (!valid_name(de->d_name, id))
			continue;
		m = load_snapshot(de->d_name);
		for (i = 0; pc_manifest_get(m, i, &e) == 0; i++) {
			size_t k;

			for (k = 0; k < e.nchunks; k++) {
				if (nlive == cap && (live = realloc(live, (cap = cap ? cap * 2 : 1024) * 32)) == NULL)
					die("out of memory");
				memcpy(live + 32 * nlive++, e.ids + 32 * k, 32);
			}
		}
		pc_manifest_free(m);
	}
	closedir(d);
	qsort(live, nlive, 32, cmp_id);
	/*
	 * Remove the index before deleting any object: if prune stops halfway,
	 * an index that still lists deleted objects would make the next backup
	 * skip them and write a snapshot that cannot be restored. Without one,
	 * backup just stores those chunks again.
	 */
	xpath(p, sizeof (p), "%s/index", repo);
	if (unlink(p) != 0 && errno != ENOENT)
		die(p);
	sync_dir(".");
	xpath(tmpidx, sizeof (tmpidx), "%s/index.new", repo);
	unlink(tmpidx);
	if ((ni = pc_index_open(tmpidx)) == NULL)
		die(tmpidx);
	xpath(p, sizeof (p), "%s/objects", repo);
	if ((d = opendir(p)) == NULL)
		die(p);
	while ((de = readdir(d)) != NULL) {
		if (!parse_id(de->d_name, id))
			continue;	/* not an object (e.g. a crashed write's .tmp) */
		if (nlive > 0 && bsearch(id, live, nlive, 32, cmp_id) != NULL) {
			if (pc_index_add(ni, id) < 0)
				die("index");
			kept++;
			continue;
		}
		xpath(p, sizeof (p), "%s/objects/%s", repo, de->d_name);
		if (unlink(p) != 0)
			die(p);
		removed++;
	}
	closedir(d);
	if (pc_index_close(ni) != 0)
		die("index");
	xpath(p, sizeof (p), "%s/index", repo);
	if (rename(tmpidx, p) != 0)
		die(p);
	sync_dir(".");
	fprintf(stderr, "pcbackup: %ld objects removed, %ld kept\n", removed, kept);
	free(live);
	close_repo();
	return (0);
}

static int
cmd_init(const char *r)
{
	const char *pw = password();
	unsigned char *kf = malloc(pc_keyfile_size());
	char p[4200];

	if (mkdir(r, 0700) != 0 && errno != EEXIST)
		die(r);
	xpath(p, sizeof (p), "%s/keyfile", r);
	if (access(p, F_OK) == 0) {
		errno = 0;
		die("repository already initialised");
	}
	if (kf == NULL || pc_keyfile_create(pw, strlen(pw), key, kf) != 0)
		die("keyfile");
	if (write_atomic(p, kf, pc_keyfile_size()) != 0)
		die(p);
	xpath(p, sizeof (p), "%s/objects", r);
	if (mkdir(p, 0700) != 0)
		die(p);
	xpath(p, sizeof (p), "%s/snapshots", r);
	if (mkdir(p, 0700) != 0)
		die(p);
	memset(key, 0, sizeof (key));
	free(kf);
	return (0);
}

int
main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";

	if (strcmp(c, "init") == 0 && argc == 3)
		return (cmd_init(argv[2]));
	if (strcmp(c, "backup") == 0 && argc == 4)
		return (cmd_backup(argv[2], argv[3]));
	if (strcmp(c, "list") == 0 && (argc == 3 || argc == 4))
		return (cmd_list(argv[2], argc == 4 ? argv[3] : NULL));
	if (strcmp(c, "restore") == 0 && (argc == 5 || argc == 6))
		return (cmd_restore(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : NULL));
	if (strcmp(c, "forget") == 0 && argc == 4)
		return (cmd_forget(argv[2], argv[3]));
	if (strcmp(c, "prune") == 0 && argc == 3)
		return (cmd_prune(argv[2]));
	fprintf(stderr, "usage: pcbackup init|backup|list|restore|forget|prune <repo> ...\n");
	return (2);
}
