# ReFS 2.0 on-disk format (Windows Server 2016 Technical Preview 2)

Reverse-engineered from two volumes made by Server 2016 TH2 (`ReFS2_64K.vhd`,
`ReFS2_4096.vhd`) and the TH2 `refs.sys` / `uReFS.dll`. REFSNT reads and
writes ReFS 2.0 with the same engine it uses for 1.2 and 3.x; this note
records where 2.0 differs from 3.x (and where it takes the 1.2 form).

## Volume boot record

Identical layout to 3.x. Version is **major 2, minor 0** (`+0x28` / `+0x29`).
The VBR checksum algorithm is the same 16-bit rotate-add over the record. Two
differences from 3.x matter:

- **`+0x40` (bytes-per-container) is 0.** ReFS 2.0 does not record the band
  size in the VBR. It uses the standard **64 MiB** container, like 3.x, so
  `ClustersPerContainer = 64 MiB / cluster` (1024 for 64 KiB clusters, 16384
  for 4 KiB). REFSNT fills this in when it sees major 2 with a zero field.
- **`+0x2A` (checksum selector) is 0.** Page checksums are still **CRC64** -
  every page reference carries `cktype = 2` and an 8-byte CRC64 over the whole
  page - the selector field simply isn't populated at 2.0.

Both sample VHDs are a 10 GiB disk with an MBR (type `0x07`) and the ReFS
partition at LBA 2048; the `ReFS2_4096.vhd` is confirmed to use genuine
**4096-byte clusters** (`sectors-per-cluster = 8`, 512-byte sectors).

## Superblock and checkpoint

- Superblock `SUPB` at cluster 30 (and the last two clusters), same fields.
- Checkpoint `CHKP`, reference size `0x68`, same field offsets.
- **Ten checkpoint roots, not thirteen.** Indices 0-9 are the same tables as
  3.x: object(0), medium-allocator(1), container-allocator(2), schema(3),
  parent-child(4), object-table copy(5), block-refcount(6), container(7),
  container copy(8), schema copy(9). ReFS 2.0 has no container-index,
  integrity-stream or small-allocator roots (3.x indices 10, 11, 12).

## Node header

The Minstore node header is the **1.x one**: 0x20 bytes, rows from `+0x20`,
the index-array end (`keystart + 4*count`) at **`+0x18`** (3.x: 0x28 bytes,
bound at `+0x20`). The row header inside the node is the 3.x one (u16 value
length). Index (branch) rows hold **0x30-byte page references** while the
checkpoint's own references are 0x68 bytes.

## Container translation (virtual -> physical)

This is the one piece that differs materially and is easy to get wrong.

- Virtual cluster numbers are **container-addressed with a one-bit gap**:
  `container = vlcn >> (log2(ClustersPerContainer) + 1)` and
  `offset = vlcn & (ClustersPerContainer - 1)`. For 64 KiB clusters the shift
  is 11 and the mask is 1023, so container *c* owns virtual range
  `[c * 2 * 1024, c * 2 * 1024 + 1024)` - every other 1024-cluster slot is
  unused. (This matches 3.x's `ContainerShift = log2(cpc) + 1`.)
- The **container table** (root 7, its copy root 8) maps container id -> band.
  Its row key is the container id (u64). Its value holds the band's **physical
  cluster at offset `+0x28`** (a u64) - this is where 3.x instead keeps the
  physical cluster in the last 16 bytes of the row, so the 3.x reader finds
  zero here. REFSNT reads `+0x28` for 2.0.
- Containers absent from the table (container 0, the self/bootstrap region) are
  **identity-mapped**: physical == virtual. The checkpoint's own roots and the
  container/allocator tables live in the self region and are read directly.

With those three points, `physical = ContPhys[container] + offset` reproduces
every page location on both sample volumes (all ten roots, the whole object
table, schema, parent-child and allocator trees) with every CRC64 matching.

## Allocators

Allocator rows are the 3.x ones (format 1 bitmaps); the allocator root's
summary is `{covered, free}` at descriptor data `+0` / `+8` (1.x placement;
3.x keeps them at `+8` / `+0x10`). The container allocator also covers the
self region and is referenced physically.

## Objects, directories and files

2.0 sits between 1.2 and 3.x:

- **Object-table rows** are 3.x style (0xF0-byte value, the directory's next
  file id in the trailer at `+0xC8`), written to both object tables.
- **Record headers** (directory self record, file record) use the 3.x field
  offsets: attributes `+0x48`, security id `+0x50`, size `+0x58`,
  allocation `+0x60`, file id / next id `+0x80`.
- **Attribute rows, schemas and the embedded flag** are 1.x style: file
  record schema 0x110, stream table 0x120 with keyed extent rows
  `{vcn, count, lcn}` (0x20-byte value), directory self record schema 0x170,
  embedded flag 0x08; attribute rows sort by (type, name).
- A directory's self record carries a **0x38 link attribute**:
  `{u32 0, u32 payload, u16 0x0C, u16 1}`, a 72-byte index entry (parent
  object id at `+8`; 1.2 keeps it at `+0`), the name length at `+0x54` and the
  name key `{0x30, 2, name}` at `+0x5A`.
- Directory index entries are the old 72-byte form (object id at `+8`);
  reverse rows are 3.x style `{0x80000020, 0, id, 0}`.

## Status in REFSNT

- **Read and write**, both sample volumes (64 KiB and 4 KiB, CRC64). A
  directory self record made by REFSNT matches Windows' field for field.
- The sample volumes were freshly formatted and hold no user files, so the
  2.0 file record, extent value and reverse row forms are inferred from the
  TH2 driver and the 1.2 / 3.x neighbours rather than read from a Windows-made
  file; REFSNT reads back what it writes and `refschk` stays clean.
