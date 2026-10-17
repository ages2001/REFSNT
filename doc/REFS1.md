# ReFS 1.x on-disk format - analysis of a real ReFS 1.2 volume

Source: `ReFS1.vhd`, a 10 GiB dynamic VHD formatted by **Windows 8.1** (label
`REFS1`, 64 KiB clusters - the only size Windows 8.1 offers). The volume fills
the whole disk (no partition table). Windows put on it only what it creates
itself (`$RECYCLE.BIN\S-1-5-21-…-1001\desktop.ini`); 408 sectors of the 10 GiB
are non-zero. Every statement below was checked on this image with
`tools/refs1dump.py` unless it is marked **[?]** (not determinable from this
image) or **[I]** (inferred). Offsets are hex.

All checksums on the volume verify: the boot sector's, the three superblocks',
both checkpoints', every page reference, and the per-block checksums of the
two system streams.

## 1. Units and addresses

- Sector 512 bytes, cluster 64 KiB (`0x80` sectors), **metadata page = one
  16 KiB "block"**. Every address on a 1.x volume - checkpoint, page reference,
  allocator range, file extent - is a **16 KiB block number** from the start of
  the volume. A cluster is 4 blocks; data extents are cluster-aligned, metadata
  pages are single blocks.
- No virtual addresses, no container table: every block number is physical.
- No log: there is no redo log object (3.x OIDs 9/A) and no log area. A commit
  is copy-on-write pages plus a checkpoint, nothing else.

## 2. Boot sector (sector 0; copy in sector N−1)

| Off | Size | Value here | Field |
|---:|---:|---|---|
| 00 | 3 | 0 | jump (not bootable) |
| 03 | 8 | `ReFS\0\0\0\0` | file system name |
| 10 | 4 | `FSRS` | recognition structure |
| 14 | 2 | 0x200 | its size |
| 16 | 2 | 0xBAA1 | its checksum (Microsoft's `ComputeFileSystemInformationChecksum`, bytes 3..0x1FF without 0x16/0x17) - verified |
| 18 | 8 | 0x1400000 | sectors in the volume **including** the copy |
| 20 | 4 | 0x200 | bytes a sector |
| 24 | 4 | 0x80 | sectors a cluster |
| 28 | 1+1 | 1, 2 | version 1.2 |
| 2A | 2 | 0 | (3.x: checksum type) |
| 2C | 4 | 0x0A | flags **[?]** |
| 38 | 8 | 0xF80AAF130AAECE46 | serial (Windows shows the low 32 bits) |
| 40 | 8 | 0 | container size (none in 1.x) |

The copy is the **last sector of the volume, N−1** (N = the count at 0x18),
byte-identical.

## 3. Page header (48 bytes, every metadata page)

| Off | Size | Field |
|---:|---:|---|
| 00 | 8 | the page's own block number |
| 08 | 8 | transaction (checkpoint sequence) that wrote it; 0 for superblocks |
| 10 | 16 | table ID (object ID of the table the page belongs to; 0 for superblock/checkpoint) - high 8 bytes then low 8 bytes, the low part holds the ID |
| 20 | 8 | 1 on every page |
| 28 | 8 | 0 |

No signature. The page's checksum is not in the page: it is in the reference
that points to it.

## 4. Page reference (24 bytes)

| Off | Size | Field |
|---:|---:|---|
| 00 | 8 | block number |
| 08 | 2 | flags (0) |
| 0A | 1 | checksum type: **2 = CRC64** |
| 0B | 1 | checksum offset from +08: 8 |
| 0C | 2 | checksum length: 8 |
| 0E | 2 | 0 |
| 10 | 8 | checksum |

CRC64 is **CRC-64/NVME** (reflected polynomial 0x9A6C9329AC4BC9B5, init and
xorout all ones) over the **whole 16 KiB page**. For the superblock's and
checkpoint's self-reference the 24-byte reference is zeroed first. Verified on
every reference of the volume.

## 5. Superblock - blocks 0x1E, N−3, N−2

N = volume size in blocks (0xA0000 here): copies at 0x1E, 0x9FFFD, 0x9FFFE,
identical except for their own block number and checksum.

| Off | Field | Here |
|---:|---|---|
| 30 | volume GUID | 7b3493ae-d655-477d-8ed7-746c2043f7ba |
| 48 | 8: generation | 1 |
| 50 / 54 | offset / count of checkpoint block numbers | 0xA0 / 2 |
| 58 / 5C | offset / size of the self-reference | 0xB0 / 0x18 |
| A0 | checkpoint blocks | **0x19B9, 0x13353** (far apart, in different 64 MiB chunks) |
| B0 | self-reference | |

## 6. Checkpoint (two fixed blocks; the higher sequence is current)

| Off | Field | Here (0x19B9 / 0x13353) |
|---:|---|---|
| 08 | page header sequence = checkpoint sequence | 8 / 7 |
| 34 / 36 | version 1 / 2 | |
| 38 / 3C | self-reference offset / size | 0x80 / 0x18 |
| 40 | **checkpoint sequence** (the current one is the higher) | 8 / 7 |
| 48 | end of the used data | 0x128 |
| 4C | room for roots | 0x20 |
| 50 | 8: 0 **[?]** | |
| 58 | number of roots | **6** |
| 5C | 6 × u32 offsets of the root references | 0x98 … 0x110 |
| 80 | self-reference | |
| 98 | 6 root references (24 bytes each) | |

Commits alternate between the two blocks; both copies here hold the same roots
(the last two mounts changed nothing). **Root order** (the earlier guess was
wrong):

| Root | Table | Schema | Here |
|---:|---|---|---|
| 0 | object table (ID 2) | 0xE030 | 6 rows |
| 1 | **large allocator** (ID 0xE) | 0xE010 | 1 row |
| 2 | **medium allocator** (ID 0xD) | 0xE010 | 4 rows |
| 3 | **small allocator** (ID 0xC) | 0xE010 | 5 rows |
| 4 | schema table (ID 1) | 0xE060 | 13 rows |
| 5 | parent-child table (ID 3) | 0xE040 | 3 rows |

## 7. B+ tree pages

**Root page:** page header (0x30), then the **index root** at page+0x30:

| Off | Size | Field |
|---:|---:|---|
| 00 | 4 | its size = offset of the node header (0xF0 global tables, 0xE8 directories, 0x38 allocators, 0xA8 file/directory record, 0x88 stream) |
| 04 | 2 | 0x28 (offset of the header data) |
| 06 | 2 | kind: 0 global tables, 1 directories/files/streams, 0xE010 allocators, **0xE020 private allocators** |
| 08 | 4 | 0 or 1 **[?]** |
| 0C, 10 | 4, 4 | schema, schema again |
| 14 | 4 | 2, or 0x20002 for system objects/streams **[?]** |
| 18 | 8 | 0 |
| 20 | 8 | **rows in the whole table** (verified on every table) |
| 28 | … | header data (table-specific, see below) |

**Node header** (32 bytes), offsets from the node header: data start (0x20),
data end, free bytes (data end + free = start of the offset array), level,
flags (2 = root), 0, offset-array start, row count, offset-array end, 0. The
offset array holds **plain u32 offsets** (no 0xFFFF marker in the high half,
unlike 3.x), in key order; rows are stored in insertion order.

**Row** header: u32 size, u16 key offset, u16 key size, u16 flags, u16 value
offset, u16 value size, u16 0. Key and value may overlap (allocator, extent,
schema and parent-child rows: the key is the value's first bytes). Row flag
**8 = the value is an embedded table** (index root + node, same layout as a
page from +0x30).

**The root node does not fill the page.** Its size is fixed by the schema
(schema row u32[13]: 0x3660 for the global tables, 0x3668 for directories);
the rest of the page, at page+0x30+size, holds the table's **private
allocator** (§9). Allocator root pages have none and fill the page.

Branch (index) pages: none on this volume (every table fits in its root) **[?]**.

## 8. Schema table (13 rows)

Same 0x50-byte row layout as 3.x: u32[7] = key comparison rule, u32[9/10] =
the schema ID, u32[13] = root node size, u32[15] = page size 0x4000.

| ID | Rule | Used by |
|---|---:|---|
| 4 | 1 | **[?]** |
| 6 | 3 | **private allocators** (kind 0xE020) |
| 0x110 | 9 | file / directory records (embedded, rows of attributes) |
| 0x120 | 3 | streams (extent rows) |
| 0x130 | 9 | directories (rule 9: name collation) |
| 0x140 | 1 | security table (OID 0x530) |
| 0x150 | 1 | volume information (OID 0x500) |
| 0xE010 | 3 | global allocators |
| 0xE030 | 6 | object table |
| 0xE040 | 8 | parent-child table |
| 0xE050 | 3 | **[?]** (only in 1.x) |
| 0xE060 | 2 | schema table |
| 0xE070 | 3 | **[?]** (only in 1.x) |

## 9. Allocation: three global allocators and one private allocator per table

Allocator row (key = first 16 bytes of the value): u64 start block, u64 length
(blocks), u32 value size, u32 bits, u32 log2(blocks a bit), u32 free bits,
u32 start / u32 length of the largest free run, u64 0, u32 bitmap offset
(0x38), u32 bitmap bytes, bitmap (1 = in use, least significant bit first).
Allocator root header data: u64 blocks managed, u64 blocks free. All counts
and largest-run fields agree with the bitmaps.

| Allocator | Bit | Rows here | Summary |
|---|---|---|---|
| large (root 1) | 0x1000 blocks = 64 MiB | one row over the whole volume; chunks 0, 1, 0x13, 0x9F in use | 0xA0000 / 0x9C000 free |
| medium (root 2) | 4 blocks = 1 cluster | one row (0x1000 blocks) for each 64 MiB chunk the large one gave out | 0x4000 / 0x3CFC free |
| small (root 3) | 1 block | ranges taken from the medium one: [0, 0x28) boot area, [0x28, 0x2C4), and one cluster each for the two checkpoints and the end superblocks | 0x2D0 / 0x29C free |

The global allocators' own pages (0x22, 0x2A, 0x2B) and the old ones (0x20,
0x21, freed) come from the small allocator.

**Growing the medium allocator [I]:** when no medium row has the run asked
for, REFSNT takes the first free bit (chunk) of the large row, sets it
(large summary free - 0x1000 blocks) and adds a medium row over the chunk -
start = chunk x 0x1000, length 0x1000, the other fields and the value size as
in the rows Windows made, bitmap all free - and adds 0x1000 blocks to the
medium summary's managed and free counts. Both summaries count blocks.

**Private allocators [new]:** every other table owns whole clusters and
allocates its pages in them itself. At the end of its root page (page +
0x30 + root node size) is an embedded allocator table: index root with kind
0xE020, schema 6, header data {u64 blocks 4, u64 free 3}, and one row: the
table's cluster (4 blocks), bit = 1 block, bitmap = the block holding the
current root. Each table's pages cycle inside its cluster by copy-on-write
(object table 0x2C4/5/6, volume information 0x2D0-0x2D3, root directory
0x2E0-0x2E2, …). A table that outgrows its cluster presumably adds rows with
more clusters from the medium allocator **[?]**.

Free space = free blocks of the three summaries (large + medium + small) **[I]**; the free blocks of the private allocators are not counted there.

## 10. Object table (root 0)

Key: 16 bytes {u64 0, u64 object ID}. Value 48 bytes: page reference (24),
u64 (0x1000 for 0x500, else 0) **[?]**, u32 8, u32 8, **u64 next file ID** (for
directories; same value as in the directory's own record).

| OID | What |
|---|---|
| 0x500 | volume information |
| 0x520 | "File System Metadata" directory (hidden: no name row in the root, linked by the parent-child table only) |
| 0x530 | security descriptors |
| 0x600 | root directory |
| 0x701, 0x702 | `$RECYCLE.BIN`, `S-1-5-21-…-1001` |

Not present in 1.x: upcase objects 7/8 (the upcase table is in 0x500), log
9/A, trash 0xD, object table copy, block reference counts, 0x501/0x540/0x541.

## 11. Parent-child table (root 5)

Key = value = 32 bytes {u64 0, u64 parent OID, u64 0, u64 child OID}:
(0x600, 0x520), (0x600, 0x701), (0x701, 0x702).

## 12. Volume information (OID 0x500)

| Key (u64) | Row |
|---|---|
| 0x510 | label, UTF-16 without terminator (`REFS1`) |
| 0x520 | 464 bytes: +0x80 `01 02 01 02` (version made with / last mounted with), +0x84 u32 0x180 **[?]**, +0x90 FILETIME (format time), +0xA0 FILETIME (later; last mount?), +0xB0 u64 0x2000 **[?]**, +0xB8 u64 0x140 **[?]** |
| 0x530 | **upcase table**: an embedded stream (schema 0x120) with one extent, 8 blocks at 0x2D4 = 65536 u16, **with a CRC64 for each block** in the extent |
| 0x540 | a 64 KiB stream (4 blocks at 0x2DC, CRC64 each) holding a copy of the 0x520 record's fields with `01 02 00 00` / 0x184 **[?]** |

The 1.x upcase table is **not** the 3.x one: 226 characters differ (3.x
tables map some capitals to small letters, e.g. U+0178 → U+00FF; 1.x maps
U+00FF → U+0178 like NTFS). A 1.x driver must use the volume's table.

## 13. Security descriptors (OID 0x530)

Key 16 bytes: {u64 value size, u32 1, u32 hash}. Value: u32 hash, u32 1,
u32 size, self-relative SECURITY_DESCRIPTOR. Files and directories refer to
a descriptor by the **hash** (record +0x60) with 1 at +0x64.

## 14. Directories

A directory is a table (schema 0x130) rooted from the object table. Rows:

| Key | Row |
|---|---|
| `10 00 00 00` | the directory's own record: an embedded table (schema 0x110), header data as a file's (§15) with: attributes 0x10 (+hidden/system), +0x50 the directory's OID, +0x60 security hash, +0x90 **next file ID**, +0x98 1. Its rows: a `0x38` row (back-link: parent OID at +0x0C, times, attributes, and the parent's name-row key `30 00 02 00` + name; the root's is uninitialised data) and an `$I30` row (key {u64 0x94, u32 0x90, "$I30"}, a fixed 148-byte NTFS-style index-root stub, identical everywhere) |
| `20 00 00 80` + u32 0 + u64 directory OID + u64 file ID | file ID → name: value {u64 0, u16 0x0C (name offset), u16 name bytes, UTF-16 name} |
| `30 00 01 00` + UTF-16 name | **file**: value is the file's embedded record table (§15) |
| `30 00 02 00` + UTF-16 name | **subdirectory**: 72-byte value: u64 child OID, u64 0, 4 FILETIMEs (created, modified, changed, accessed), 16 × 0, u32 attributes (incl. 0x10000000), u32 0 |

Rows sort as bytes of their key type (0x10 < 0x20 < 0x30); names within a type
by rule 9 (case-insensitive with the volume's upcase table) **[I: only two
names in one directory here]**. File IDs are per directory: the root starts at
1, `File System Metadata` holds 0x100 (`Volume Direct IO File`) and 0x200
(`Security Descriptor Stream`).

## 15. Files

The name row's value (row flag 8) is an embedded table (schema 0x110, index
root 0xA8 bytes). Header data at value+0x28:

| Value off | Field |
|---:|---|
| 28, 30, 38, 40 | FILETIMEs: created, modified, changed, accessed |
| 48 | attributes (desktop.ini: 0x26) |
| 50 | parent directory OID |
| 58 | file ID within the directory |
| 60 / 64 | security hash / 1 |
| 68 | file size |
| 70 | allocated size (whole clusters) |
| 98 | 1 |

Its rows are attributes. `$DATA`: key {u64 0x160 (the value size), u32 0x80,
u16 0}; value = a stream table (schema 0x120, index root 0x88): header data
u32 1 at +0x28 **[?]**, allocated size at +0x34, size at +0x3C, valid size at
+0x44 (unaligned u64s); rows = extents, 0x20 bytes plus checksums:

| Off | Field |
|---:|---|
| 00 | first block of the file this extent maps (0 here) |
| 08 | blocks |
| 10 | first block on the volume |
| 18 | u16 flags (1 = integrity: checksums follow), u8 checksum type (2 = CRC64), u8 8, u16 checksum bytes (8 a block) |
| 20 | per-block checksums (integrity streams only) |

The key is the extent's first 16 bytes. `desktop.ini` (129 bytes) has one
extent of 4 blocks at block 0x2F4, flags 0 (no checksums); its data is the
file's text.

## 16. What a 1.x implementation can reuse from REFSNT's 3.x engine

Same: B+ tree node/row format (but 48-byte page header, index root at +0x30,
plain offsets, a root node smaller than the page), schema rules and key
comparison, the directory/file row scheme, CRC64, copy-on-write commit with
two alternating checkpoints. Different: 16 KiB block addressing, 24-byte
references, 6 roots in another order, the three-level allocator with private
per-table allocators, no log, no block refcounts, upcase in object 0x500,
file record offsets past 0x48, extent rows in blocks with optional per-block
checksums.

## 17. Still unknown - what a second image would settle

This volume is almost empty, so these could not be seen:

1. **Branch pages**: a directory with a few thousand files (index nodes; how
   a table's private allocator grows beyond its first cluster).
2. **Large / fragmented files**: a file over 64 MiB and a file written in
   pieces (several extents; how the medium allocator asks the large one for
   chunks).
3. **Name order**: a directory with names differing in case and accents.
4. **Integrity streams on user files** (a volume formatted with
   `format /fs:refs /i:enable`, or a file with integrity turned on: do user
   files get per-block checksums? `desktop.ini` here has none), alternate data
   streams, hard links.
5. **Deletes and renames**, and the same volume before and after one change
   (to see which pages a commit rewrites and when freed blocks come back).
6. The meaning of the boot-sector flags (0x0A), schemas 4 / 0xE050 / 0xE070,
   the object-table value +0x18, and the 0x540 stream.
