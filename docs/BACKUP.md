# libpcompress for backup programs

How the library's chunk-store pieces fit a backup tool, what the
`examples/backup/pcbackup.c` example leaves out and why, and how Argh!
(`argh`, `arghd`) is meant to use them. Companion to `ROADMAP.md`.

## Two ways to store a backup

**A seekable `.pz` container** (the unbuilt half of ROADMAP Phase 6) is
one file. It is today's `.pz` archive (a PAX stream of every file, cut
into chunks, each compressed and optionally encrypted) with a table at
the end mapping each archived path to the chunks that hold it. A reader
seeks to the table, finds a file's chunks, seeks to them, and extracts
that file without decompressing the rest. It is the `pcompress -a` user's
answer to "give me one file out of a 200 GB archive". Each archive still
stands alone: two archives of the same tree store everything twice
(global dedupe works within one file), and an archive is written once
and never changed.

**A chunk store** (built in 4.2.0) is a set of objects plus some small
records naming them:

- Files are cut by content (`pc_chunker_*`), so an unchanged region gives
  the same chunks wherever it sits.
- Each chunk is named by a keyed hash of its content (`pc_chunk_id`) and
  stored once as a sealed object (`pc_chunk_seal`: compressed, encrypted,
  authenticated).
- A snapshot is a manifest (`pc_manifest_*`) listing every path, its
  metadata and its chunk ids, itself sealed and stored as an object.
- `pc_index_*` remembers which ids are already stored.

Every snapshot shares every chunk any earlier snapshot already stored,
from any machine using the same key. A new snapshot costs only the
changed chunks and one manifest. Restoring one file reads one manifest
and that file's chunks. Deleting a snapshot is deleting its manifest,
plus a prune that removes objects nothing names any more.

| | Seekable `.pz` | Chunk store |
|---|---|---|
| Unit of storage | one file per archive | many small objects |
| Dedupe | within one archive | across all snapshots and machines |
| Incremental backup | no: each archive is complete | yes: only new chunks |
| Restore one file | seek in the archive | fetch its chunks |
| Delete old data | delete the whole archive | forget a snapshot, then prune |
| Fits object storage (S3, B2) | one large object, rewritten each time | many immutable objects, never rewritten |
| Readable by `pcompress -d` | yes, any version that knows the format | no: needs the store's program |
| New on-disk format | yes (`.pz` v11) | done (`PCK1`, `PCMF1`, `PCIDX1`, `PCKEY1`) |

## How the chunk store covers the Arq case

Arq's own format (arqbackup.com, "Arq 7 data format") is a chunk store:

- Blobs are LZ4-compressed, optionally encrypted chunks identified by
  SHA-256, packed into pack files or stored as standalone objects.
- A tree of Nodes gives each file its ordered list of chunk locations.
- A backup record per snapshot points at the root.
- An encrypted key set holds the master keys, wrapped by the password.

Each piece has a libpcompress equivalent:

| Arq | libpcompress |
|---|---|
| chunker (`chunkerVersion`) | `pc_chunker_*` (content-defined, deterministic) |
| blob id (SHA-256 of plaintext) | `pc_chunk_id`, keyed BLAKE2b-256: the store cannot confirm guessed content |
| blob (LZ4, then AES-CBC + HMAC-SHA256) | `pc_chunk_seal`: any algorithm, then XSalsa20 + BLAKE2b (encrypt-then-MAC) |
| tree / node | `pc_manifest_*` (one flat sorted list per snapshot) |
| encryptedkeyset.dat | `pc_keyfile_*` (scrypt-wrapped 256-bit key) |
| local blob cache / "already uploaded?" | `pc_index_*` |
| pack files, BlobLoc | **not provided**: the caller's choice (see Argh! below) |

The seekable container adds nothing an Arq-like tool needs, so it is not
built. It is worth doing only if `pcompress -a` users ask for single-file
extraction from large standalone archives.

## pcbackup: what it is and what it leaves out

`examples/backup/pcbackup.c` (about 770 lines) proves the API is
sufficient. It is an example, deliberately small: it exists to be read
and to be run by `make check`. The leaving-outs are features whose cost
falls on the tool, not the library.

- **Owners.** `chown` needs root, and restoring as root is where
  ownership goes wrong (a file restored to a uid that is now someone else
  on this machine). A real tool needs a policy: restore owners only as
  root, map names instead of numbers, and handle a uid that no longer
  exists. The manifest already records uid and gid. Because pcbackup
  cannot restore owners, it also clears setuid and setgid bits: a setuid
  file owned by whoever runs the restore is a hole.
- **Hard links.** Two paths for one inode need a "same as path X" entry
  (detected with `st_dev`/`st_ino`/`st_nlink` at backup time) and an
  ordering rule at restore. Without it each link is restored as a
  separate copy: correct contents, more space, and no shared identity.
  Needs a new entry type in `PCMF1` (types are a byte; there is room).
- **Special files** (devices, FIFOs, sockets). Restoring a device node
  needs root and is rarely what a user wants; a FIFO or socket is
  meaningless out of context. pcbackup skips them with a warning, and
  opens regular files with `O_NONBLOCK` so a FIFO swapped in mid-backup
  cannot hang it.
- **Locking.** Two pcbackups on one repository at once (say, a backup and
  a prune) race: prune can delete a chunk the backup has just decided is
  already stored. Avoiding that needs a repository lock, which is easy on
  one machine (`flock`) and hard on object storage (S3 has no locks;
  real tools use a lock object with a lease, or a two-phase prune that
  only deletes objects unreferenced for longer than any backup can run).
  It is the store's design, not the library's.
- Also not done: extended attributes, ACLs, sparse files, Windows
  attributes, remote stores, scheduling, retention policies.

## Argh! (`argh`, `arghd`)

Argh! is the planned open-source Arq-like tool: `argh` on the command
line, `arghd` the daemon that runs scheduled backups. libpcompress is its
data plane: everything from bytes on disk to sealed, named objects and
back. Argh! owns the rest: what to back up and when, where objects go,
and the repository's lifecycle.

### What Argh! takes from libpcompress, unchanged

- **Chunking:** `pc_chunker_*`, with sizes fixed per repository and
  recorded in its config, so dedupe stays stable across versions (Arq's
  `chunkerVersion`).
- **Identity:** `pc_chunk_id` keyed with the repository key. Object names
  then reveal nothing about content to the storage provider.
- **Objects:** `pc_chunk_seal`/`pc_chunk_open` for every chunk and every
  manifest, with `pc_set_algorithm` chosen per repository (zstd by
  default). On restore, compare `pc_chunk_id` of the opened object with
  the name it was fetched by.
- **Keys:** `pc_keyfile_create` at `argh init`, `pc_keyfile_open` at
  each run. The password changes by rewrapping the same key in a new key
  file, with no re-encryption of the data. A second key file can wrap the
  same key for a recovery password.
- **Snapshots:** one `pc_manifest_t` per backed-up folder per run.
- **Local cache:** `pc_index_*` in `arghd`'s cache directory. It is only
  a cache: losing it costs one extra lookup per chunk, never a wrong
  backup.
- **Many jobs in one process:** `arghd` can back up several folders at
  once. Every libpcompress call takes its own context, and the library
  is reentrant (ROADMAP Phase 1).

### What Argh! builds on top

- **Pack files.** Object stores charge per request, and a 64 KB chunk per
  PUT is slow and costly. Argh! appends sealed objects into packs of tens
  of megabytes and keeps a pack index (chunk id to pack, offset,
  length), as Arq does. Sealed objects are self-delimiting and
  authenticated, so a pack is a concatenation of them and a damaged pack
  index can be rebuilt by scanning.
- **Storage backends:** local directory, SFTP, S3-compatible (S3, B2,
  R2, MinIO). The library never does network I/O.
- **Tree structure:** per-folder manifests as above. For huge trees a
  manifest per directory (Arq's tree of Nodes) would make an unchanged
  subtree reusable by id. That is a change to the manifest's use, not
  its format.
- **Metadata policy:** owners, hard links (a new manifest entry type),
  extended attributes and ACLs (stored as their own sealed objects),
  macOS and Windows attributes.
- **Concurrency and pruning:** a repository lock or a two-phase prune
  ("mark, wait longer than the longest backup, sweep"), because object
  stores have no locks.
- **Scheduling, retention ("keep hourly for a day, daily for a month"),
  notifications, a CLI and later a GUI.**

### What libpcompress may still need for Argh!

These come from Argh!'s design, and none blocks starting it:

- **Callback or buffer-based streaming for `pc_chunk_seal`** of large
  inputs. Today the caller holds each chunk in memory, which is fine up
  to the chunker's maximum (default 256 KB).
- **A hard-link entry type** in `PCMF1`.
- **A Windows port** (`docs/PLATFORMS.md`): Argh! must run on Windows 11,
  so libpcompress must first.
- **Pack helpers**, only if two consumers turn out to want the same pack
  format.
