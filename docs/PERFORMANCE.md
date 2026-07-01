# Pcompress Performance Guide

## Algorithm Selection Matrix

This table summarizes the trade-offs between compression algorithms available
in Pcompress. Select based on your data type and latency/ratio requirements.

| Algorithm | Speed    | Ratio    | Memory   | Effective Levels | Best For              |
|-----------|----------|----------|----------|------------------|-----------------------|
| lz4       | Fastest  | Low      | Low      | 1-3              | Real-time, streaming  |
| lzfx      | Fast     | Low-Med  | Low      | 1-5              | Fast compression      |
| zlib      | Medium   | Medium   | Low      | 1-9              | General purpose       |
| zstd      | Fast-Med | Med-High | Low-Med  | 1-14             | General purpose, fast |
| bzip2     | Slow     | High     | Medium   | 1-9              | Text, general data    |
| libbsc    | Medium   | High     | Medium   | 1-14             | Text, mixed data      |
| lzma      | Slow     | Highest  | High     | 1-14             | Maximum compression   |
| lzmaMt    | Med-Slow | Highest  | High     | 1-14             | Parallel max compress |
| ppmd      | Slow     | High     | Very High| 1-14             | Pure text data        |
| adapt     | Variable | High     | Variable | 1-14             | Mixed data types      |
| adapt2    | Slow     | Highest  | High     | 1-14             | Archives (default)    |

### Zstandard (zstd)

Zstandard provides an excellent balance of speed and compression ratio,
sitting between LZ4 and LZMA on the speed-vs-ratio spectrum. It is a strong
default choice for general-purpose compression where both speed and ratio
matter.

Pcompress maps its 14 compression levels to Zstandard's native 1-22 range:

| Pcompress Level | ZSTD Level | Profile      | Notes                          |
|-----------------|------------|--------------|--------------------------------|
| 1-3             | 1-3        | Fast         | LZ4-competitive speed          |
| 4-6             | 5, 7, 9    | Balanced     | Good ratio with reasonable speed|
| 7-9             | 12, 14, 16 | High         | Better ratio, slower           |
| 10-14           | 17-21      | Ultra        | LZMA-competitive ratio         |

Zstandard supports **multi-threaded compression** natively. When `-t` is set
to more than 1, Pcompress passes the thread count to ZSTD's internal worker
pool (up to 8 threads). Decompression is single-threaded.

### Adaptive Modes Explained

- **adapt**: Analyzes each chunk, selects PPMD for text or Bzip2 for binary
- **adapt2**: Analyzes each chunk, selects PPMD for text or LZMA for binary;
  slower but better compression ratio. Default for archive mode.

## Tuning Parameters

### Chunk Size (`-s`)

Chunk size controls the granularity of parallel processing:

| Chunk Size | Parallelism | Compression Ratio | Memory Usage        |
|------------|-------------|-------------------|---------------------|
| 1m         | High        | Lower             | ~6MB per thread     |
| 4m         | Good        | Good              | ~24MB per thread    |
| 8m (default)| Good       | Good              | ~48MB per thread    |
| 20m        | Moderate    | Better            | ~120MB per thread   |
| 64m        | Low         | Best              | ~384MB per thread   |

For adaptive modes, smaller chunks (2m-8m) often produce better results
because the analyzer can select the best algorithm per-chunk with finer
granularity.

For non-adaptive modes with a single data type, larger chunks generally
produce better compression ratios.

### Thread Count (`-t`)

By default, Pcompress uses one thread per CPU core. Adjust if:

- Memory is constrained: reduce threads to reduce total buffer allocation
- I/O bound: more threads than cores may help overlap I/O and compression
- Using LZMA at high levels: each thread may use 256MB+ of RAM

### Compression Level (`-l`)

Levels 1-14, where higher levels trade speed for better compression.

#### Feature Activation by Level

| Level | Dedup | Delta | Dispack | LZP | PackJPG |
|-------|-------|-------|---------|-----|---------|
| 1-3   | No    | No    | No      | No  | No      |
| 4     | Global 8KB | No | No   | No  | No      |
| 5     | Global 8KB | Adaptive | No | No | No   |
| 6-8   | Global 4KB | Adaptive | No | No | No   |
| 9     | Global 2KB | Adaptive | Yes | No | No   |
| 10    | Global 2KB | Adaptive+ | Yes | Yes | Yes |
| 11-14 | Global 2KB | Adaptive+ | Yes | Yes | Yes |

"Adaptive+" means delta encoding with extra rounds.

### Deduplication Block Size (`-B`)

Controls the average block size for deduplication:

| `-B` value | Block Size | RAM per GB of Data | Dedup Ratio |
|------------|------------|-------------------|-------------|
| 0          | 2 KB       | ~4 MB             | Highest     |
| 1          | 4 KB       | ~2 MB             | High        |
| 2 (default)| 8 KB       | ~1 MB             | Good        |
| 3          | 16 KB      | ~512 KB           | Moderate    |
| 4          | 32 KB      | ~256 KB           | Lower       |
| 5          | 64 KB      | ~128 KB           | Lowest      |

Smaller blocks give better dedup ratios at the cost of more memory for the
hash index and more overhead in the dedup metadata.

## Memory Usage Estimation

Total memory usage is approximately:

```
Memory = (chunk_size * 3 * num_threads)    # Input + output + work buffers
       + global_dedup_index                 # If -G enabled
       + algorithm_state                    # Algorithm-specific
```

Algorithm-specific memory overhead:
- LZ4, LZFX: negligible
- Zlib: ~256KB per thread
- Zstandard levels 1-6: ~1-10MB per thread
- Zstandard levels 7-14: ~10-100MB per thread
- Bzip2: ~8MB per thread
- LZMA levels 1-9: ~10-100MB per thread
- LZMA levels 10-12: ~100-300MB per thread
- LZMA levels 13-14: ~256MB+ per thread (up to 256MB dictionary)
- PPMD: ~64MB per thread minimum

## Environment Variables

| Variable                   | Purpose                                       |
|----------------------------|-----------------------------------------------|
| `ALLOCATOR_BYPASS=1`       | Use system malloc instead of slab allocator    |
| `PCOMPRESS_INDEX_MEM=N`    | Limit dedup index to N megabytes               |
| `PCOMPRESS_CACHE_DIR=path` | Directory for dedup temp files (use SSD)       |
| `PCOMPRESS_CHUNK_HASH_GLOBAL` | Override dedup block hash (default: SHA256) |

Valid values for `PCOMPRESS_CHUNK_HASH_GLOBAL`:
`SHA256`, `SHA512`, `KECCAK256`, `KECCAK512`, `BLAKE256`, `BLAKE512`,
`SKEIN256`, `SKEIN512`

## Checksum Performance

Checksum selection (`-S`) affects both security and performance:

| Checksum   | Size    | Speed     | Security |
|------------|---------|-----------|----------|
| CRC64      | 8 bytes | Fastest   | Non-crypto |
| BLAKE256   | 32 bytes| Very Fast | Strong   |
| BLAKE512   | 64 bytes| Very Fast | Strong   |
| SHA256     | 32 bytes| Fast (SSE/AVX) | Strong |
| SHA512     | 64 bytes| Fast (SSE/AVX) | Strong |
| KECCAK256  | 32 bytes| Medium    | Strong (SHA-3) |
| KECCAK512  | 64 bytes| Medium    | Strong (SHA-3) |

BLAKE2 (BLAKE256/BLAKE512) is the default and recommended choice for the
best balance of speed and security.

## Recommended Configurations

### Fast Archival (daily backups)

```sh
pcompress -a -c zlib -l3 -s8m /data backup
```

### Balanced Archival (Zstandard)

```sh
pcompress -a -c zstd -l5 -s8m /data backup
```
Zstandard at level 5 provides better compression than zlib at comparable or
faster speed, with native multi-threading support.

### Fast + Good Ratio (Zstandard low levels)

```sh
pcompress -a -c zstd -l2 -s8m -t4 /data backup
```
At levels 1-3, Zstandard is competitive with LZ4 on speed but achieves
20-30% better compression ratios. This is ideal when both speed and
reasonable compression matter.

### Maximum Compression (long-term storage)

```sh
pcompress -a -l14 -s20m /data archive
```

### Deduplication Focus (VM images, disk dumps)

```sh
pcompress -c lzma -l6 -G -B0 disk.img compressed
```

### Minimum Memory Usage

```sh
pcompress -c lzfx -l2 -s1m -t2 file.tar
```
Uses approximately 6MB RSS.

### Network Transfer (streaming)

```sh
tar cf - /data | pcompress -c lz4 -l1 -p - | ssh remote "pcompress -d - /data"
```

## Profiling

Use the `-C` flag to display compression statistics after completion. Use
`-CC` for additional detail including per-block dedup offset and length
information.

Use `-M` to display memory allocator statistics (slab allocator usage).
