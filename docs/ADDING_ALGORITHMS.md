# Adding Compression Algorithms to Pcompress

This guide describes how to add a new compression algorithm to Pcompress.

## Algorithm Interface

Every compression algorithm in Pcompress implements a uniform interface defined
by function pointer types in `utils/utils.h` and declared in `pcompress.h`.
You must implement the following functions:

### Required Functions

#### 1. Init Function

```c
int myalgo_init(void **data, int *level, int nthreads, uint64_t chunksize,
                int file_version, compress_op_t op);
```

- **Purpose**: Initialize per-session algorithm state.
- **Parameters**:
  - `data`: Output pointer to algorithm-private state (allocated here).
  - `level`: Pointer to compression level; can be clamped to valid range.
  - `nthreads`: Number of worker threads.
  - `chunksize`: Size of each compression chunk in bytes.
  - `file_version`: Pcompress file format version for compatibility.
  - `op`: `COMPRESS` or `DECOMPRESS`.
- **Returns**: 0 on success, -1 on failure.

#### 2. Compress Function

```c
int myalgo_compress(void *src, uint64_t srclen, void *dst,
                    uint64_t *dstlen, int level, uchar_t chdr,
                    int btype, void *data);
```

- **Purpose**: Compress a single buffer.
- **Parameters**:
  - `src`: Input buffer.
  - `srclen`: Input buffer size.
  - `dst`: Output buffer (pre-allocated by caller).
  - `dstlen`: In/out: on entry, size of `dst`; on exit, actual compressed size.
  - `level`: Compression level (1-14).
  - `chdr`: Chunk header byte (contains adaptive mode flags).
  - `btype`: Data type hint from analyzer (`DATA_TEXT`, `DATA_BINARY`, etc.).
  - `data`: Algorithm-private state from `myalgo_init`.
- **Returns**: 0 on success, -1 on failure (caller stores uncompressed).

#### 3. Decompress Function

```c
int myalgo_decompress(void *src, uint64_t srclen, void *dst,
                      uint64_t *dstlen, int level, uchar_t chdr,
                      int btype, void *data);
```

Same signature as compress. `dstlen` should be set to the original
uncompressed size on successful return.

#### 4. Deinit Function

```c
int myalgo_deinit(void **data);
```

Free all resources allocated by `myalgo_init`. Set `*data = NULL`.

#### 5. Properties Function

```c
void myalgo_props(algo_props_t *data, int level, uint64_t chunksize);
```

Fill in the `algo_props_t` structure:

```c
typedef struct {
    uint32_t buf_extra;              // Extra bytes needed in output buffer
    int compress_mt_capable;         // 1 if algo does internal MT compression
    int decompress_mt_capable;       // 1 if algo does internal MT decompression
    int single_chunk_mt_capable;     // 1 if MT works for single-chunk mode
    int is_single_chunk;             // Set by caller, not by props
    int nthreads;                    // Set by caller
    int c_max_threads;               // Max threads for compression
    int d_max_threads;               // Max threads for decompression
    int delta2_span;                 // Stride span for delta2 filter
    int deltac_min_distance;         // Min distance for delta compression
    cksum_t cksum;                   // Set by caller
} algo_props_t;
```

Key fields to set:
- `buf_extra`: How many extra bytes the output buffer needs beyond the input
  size (for worst-case expansion). For example, LZ4 needs `lz4_buf_extra()`.
- `compress_mt_capable` / `decompress_mt_capable`: Set to 1 if your algorithm
  handles threading internally (like LZMA-MT).
- `delta2_span`: Recommended stride for delta2 pre-processing (0 to use
  default).

#### 6. Stats Function

```c
void myalgo_stats(int show);
```

Print compression statistics. Called when `-C` flag is used. The `show`
parameter controls verbosity.

## Step-by-Step Integration

### Step 1: Create the Wrapper Source File

Create `myalgo_compress.c` at the top level. Use an existing file like
`lz4_compress.c` as a template. Implement all six functions.

Example skeleton:

```c
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils.h>
#include <pcompress.h>
#include <allocator.h>

/* Include your algorithm's header */
#include <myalgo.h>

void
myalgo_stats(int show)
{
    /* Print stats or no-op */
}

int
myalgo_init(void **data, int *level, int nthreads, uint64_t chunksize,
            int file_version, compress_op_t op)
{
    /* Clamp level to valid range */
    if (*level > 9) *level = 9;

    /* Allocate private state if needed */
    *data = NULL;
    return (0);
}

int
myalgo_deinit(void **data)
{
    if (*data) {
        free(*data);
        *data = NULL;
    }
    return (0);
}

void
myalgo_props(algo_props_t *data, int level, uint64_t chunksize)
{
    data->buf_extra = 1024;  /* Worst-case expansion overhead */
    data->compress_mt_capable = 0;
    data->decompress_mt_capable = 0;
    data->delta2_span = 0;
}

int
myalgo_compress(void *src, uint64_t srclen, void *dst,
                uint64_t *dstlen, int level, uchar_t chdr,
                int btype, void *data)
{
    /* Call your compression library */
    size_t out_size = *dstlen;
    int rv = my_library_compress(src, srclen, dst, &out_size, level);
    if (rv != 0)
        return (-1);
    *dstlen = out_size;
    return (0);
}

int
myalgo_decompress(void *src, uint64_t srclen, void *dst,
                  uint64_t *dstlen, int level, uchar_t chdr,
                  int btype, void *data)
{
    size_t out_size = *dstlen;
    int rv = my_library_decompress(src, srclen, dst, &out_size);
    if (rv != 0)
        return (-1);
    *dstlen = out_size;
    return (0);
}
```

### Step 2: Declare Functions in `pcompress.h`

Add `extern` declarations for all six functions:

```c
extern int myalgo_compress(void *src, uint64_t srclen, void *dst,
    uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int myalgo_decompress(void *src, uint64_t srclen, void *dst,
    uint64_t *dstlen, int level, uchar_t chdr, int btype, void *data);
extern int myalgo_init(void **data, int *level, int nthreads, uint64_t chunksize,
    int file_version, compress_op_t op);
extern void myalgo_props(algo_props_t *data, int level, uint64_t chunksize);
extern int myalgo_deinit(void **data);
extern void myalgo_stats(int show);
```

### Step 3: Register the Algorithm in `pcompress.c`

In `pcompress.c`, locate the algorithm selection logic (search for existing
algorithm names like `"lz4"` or `"zlib"`). Add your algorithm to the
`if/else` chain:

```c
} else if (memcmp(algorithm, "myalgo", 6) == 0) {
    _compress_func = myalgo_compress;
    _decompress_func = myalgo_decompress;
    _init_func = myalgo_init;
    _deinit_func = myalgo_deinit;
    _stats_func = myalgo_stats;
    _props_func = myalgo_props;
```

Also add it to the `usage()` output so it appears in the help text.

### Step 4: Update the Build System

In `Makefile.in`, add your source files:

```makefile
MYALGOSRCS = myalgo_compress.c
MYALGOHDRS = $(MAINHDRS)
MYALGOOBJS = $(MYALGOSRCS:.c=.o)
```

Add the object to the `OBJS` list:

```makefile
OBJS = ... $(MYALGOOBJS) ...
```

Add a build rule:

```makefile
$(MYALGOOBJS): $(MYALGOSRCS) $(MYALGOHDRS)
	$(COMPILE) $(GEN_OPT) $(VEC_FLAGS) $(CPPFLAGS) $(@:.o=.c) -o $@
```

If your algorithm requires an external library, add the appropriate
`-I` and `-L`/`-l` flags, and update the `config` script to detect it.

### Step 5: Add to Adaptive Mode (Optional)

If your algorithm should be selectable in adaptive compression modes
(`adapt` / `adapt2`), update `adaptive_compress.c`:

1. Add an `ADAPT_COMPRESS_MYALGO` constant in `pcompress.h`
2. Add your algorithm to the adaptive selection logic in
   `adaptive_compress.c`
3. Add corresponding decompress dispatch in the adaptive decompressor

### Step 6: Testing

Add test cases to the test suite:

1. Create or update a `.tst` file in `test/` (see existing `t1.tst` through
   `t9.tst` for format)
2. Test at minimum:
   - Compression and decompression roundtrip
   - Multiple compression levels
   - Pipe mode (`-p`)
   - Combined with deduplication (`-D`, `-G`)
   - Combined with pre-processing filters (`-L`, `-P`)
   - Archive mode (`-a`)

Run: `make test`

## Real-World Example: Zstandard Integration

The Zstandard (`zstd`) algorithm was integrated into Pcompress following
this guide. The implementation in `zstd_compress.c` serves as a concrete
reference for adding algorithms with the following characteristics:

### Level Mapping

Pcompress uses levels 1-14, but Zstandard supports levels 1-22. The
`map_level()` function translates between them:

```c
static int
map_level(int level)
{
    if (level <= 0) return 1;
    if (level <= 3) return level;           /* 1-3 -> ZSTD 1-3 (fast) */
    if (level <= 6) return 3 + (level - 3) * 2;  /* 4-6 -> ZSTD 5,7,9 */
    if (level <= 9) return 10 + (level - 7) * 2;  /* 7-9 -> ZSTD 12,14,16 */
    if (level <= 14) return 16 + (level - 10);    /* 10-14 -> ZSTD 17-21 */
    return 22;
}
```

This is a common pattern when wrapping libraries whose level range differs
from Pcompress's 1-14 range.

### Multi-Threaded Compression

Zstandard supports internal multi-threading for compression. The wrapper
declares this capability via `algo_props_t`:

```c
void
zstd_props(algo_props_t *data, int level, uint64_t chunksize)
{
    data->compress_mt_capable = 1;   /* ZSTD handles MT internally */
    data->decompress_mt_capable = 0; /* Decompression is single-threaded */
    data->buf_extra = 0;             /* ZSTD handles buffer sizing */
    data->c_max_threads = 8;         /* Cap internal thread pool */
    data->delta2_span = 100;
    /* deltac_min_distance varies by level */
}
```

The thread count is passed to ZSTD in the compress function via
`ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, nthreads)`.

### Context Reuse

The init function creates persistent ZSTD contexts (`ZSTD_CCtx` /
`ZSTD_DCtx`) that are reused across chunks, avoiding repeated allocation.
The compress function calls `ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only)`
before each chunk to reset session state while preserving the allocated
memory.

### Source Location

- Wrapper: `zstd_compress.c` (top-level)
- Library: `zstd/` directory (git submodule)

## Checklist

- [ ] Implement all 6 interface functions
- [ ] Declare externs in `pcompress.h`
- [ ] Register algorithm name in `pcompress.c`
- [ ] Add to `usage()` help text
- [ ] Update `Makefile.in` with sources, headers, objects, and build rule
- [ ] Update `config` script if external library is needed
- [ ] Add to adaptive mode if appropriate
- [ ] Add test cases
- [ ] Update `README.md` with algorithm description
- [ ] Verify `buf_extra` is sufficient for worst-case expansion
