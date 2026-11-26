/*
 * Fuzzing harness for the repair data (repair.c). A repair header is
 * hashed, so a fuzzer could not reach the decoder from random bytes;
 * instead each input makes a valid file and then damages it:
 *
 *   byte 0      parity percent (1 to 50)
 *   bytes 1-2   protected length / 16 (little-endian, at least 1)
 *   byte 3      number of damage records (at most 16)
 *   then        records of u32 offset (mod file size), u16 length, u8 value
 *   rest        seed for the protected bytes
 *
 * Checked: no crash or sanitizer report; a repair that reports success
 * left exactly the original protected bytes; and damage to a single
 * byte (one record of length 1) is always repaired.
 *
 * Built by "make fuzz-build" against the library's objects; -DFUZZ_AFL
 * builds a stdin driver for AFL++.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <utils.h>
#include <pc_alloc.h>
#include <repair.h>
#include <pcompress/pcompress.h>

static char path[512], out[512];

static int
fuzz_one(const uint8_t *data, size_t size)
{
	uint64_t plen, i, fsize, x = 0x9e3779b97f4a7c15ULL;
	unsigned char *orig, *back;
	unsigned int nrec, r, pct;
	struct stat sb;
	int fd, rv, one;

	if (size < 4)
		return (0);
	if (path[0] == '\0') {
		const char *t = getenv("TMPDIR");

		snprintf(path, sizeof (path), "%s/fuzz_repair.%d", t ? t : "/tmp", (int)getpid());
		snprintf(out, sizeof (out), "%s.out", path);
		pc_set_default_log(NULL, NULL, PC_LOG_NONE);
	}
	pct = data[0] % RP_PCT_MAX + 1;
	plen = 16 * (uint64_t)((data[1] | (data[2] << 8)) + 1);
	nrec = data[3] % 17;
	if (size < 4 + 7 * (size_t)nrec)
		return (0);
	for (i = 4 + 7 * (size_t)nrec; i < size; i++)
		x = (x ^ data[i]) * 0x100000001b3ULL;
	orig = pc_malloc(plen);
	back = pc_malloc(plen);
	for (i = 0; i < plen; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		orig[i] = (unsigned char)x;
	}
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd == -1 || write(fd, orig, plen) != (ssize_t)plen ||
	    rp_write(fd, plen, 0, (int)pct, NULL, NULL) != 0 || fstat(fd, &sb) != 0)
		abort();
	fsize = (uint64_t)sb.st_size;
	one = nrec == 1 && data[8] == 1 && data[9] == 0;	/* one record, length 1 */
	for (r = 0; r < nrec; r++) {
		const uint8_t *p = data + 4 + 7 * r;
		uint64_t off = (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint64_t)p[3] << 24)) % fsize;
		uint64_t len = p[4] | (p[5] << 8);
		unsigned char buf[65536];

		if (len > fsize - off)
			len = fsize - off;
		memset(buf, p[6], len);
		if (pwrite(fd, buf, len, (off_t)off) != (ssize_t)len)
			abort();
	}
	close(fd);
	unlink(out);
	rv = rp_repair(path, out);
	if (one && rv != 0)
		abort();
	if (rv == 0) {
		fd = open(out, O_RDONLY, 0);
		if (fd == -1 || read(fd, back, plen) != (ssize_t)plen ||
		    memcmp(back, orig, plen) != 0)
			abort();
		close(fd);
	}
	unlink(out);
	unlink(path);
	pc_mfree(orig);
	pc_mfree(back);
	return (0);
}

#ifdef FUZZ_AFL
int
main(void)
{
	static uint8_t buf[1 << 16];
	size_t n = fread(buf, 1, sizeof (buf), stdin);

	return (fuzz_one(buf, n));
}
#else
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	return (fuzz_one(data, size));
}
#endif
