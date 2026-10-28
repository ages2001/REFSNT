# REFSNT - ReFS File System Driver for Windows NT

REFSNT is a from-scratch driver for Microsoft's **Resilient File System** on
Windows NT 3.1, 3.5, 3.51, 4.0, 2000, XP and XP x64. It reads and writes
**ReFS 1.2, 2.0 and 3.x** - the ReFS of Windows 8.1, Server 2016 TH2, and
Windows 10 / 11 and Server 2019 and later - copy-on-write, with every page
checksum kept right, it **formats ReFS 1.2, 2.0 and 3.14 volumes** of its own,
and it can **start Windows NT from a ReFS volume**.

It is built like its siblings [UDFNT](https://github.com/ages2001/UDFNT),
[EXTNT](https://github.com/ages2001/EXTNT),
[BTRFSNT](https://github.com/ages2001/BTRFSNT),
[NTFS3NT](https://github.com/ages2001/NTFS3NT),
[FATNT](https://github.com/ages2001/FATNT) and
[HPFSNT](https://github.com/ages2001/HPFSNT): one kernel-free engine that
understands the on-disk format - built into the driver, the tools and a Linux
test harness - with an NT driver layer on top.

## At a glance

| | |
|---|---|
| **Version** | 1.1 |
| **File system** | ReFS 3.x (Windows 10 1709+, 11, Server 2019+), ReFS 2.0 (Server 2016 TH2), ReFS 1.2 (Windows 8.1 / Server 2012 R2); ReFS 1.1 read the same way |
| **Windows** | NT 3.1, 3.5, 3.51, 4.0, 2000, XP (x86); XP x64 / Server 2003 x64 (amd64) |
| **Access** | read/write |
| **Paging file** | yes |
| **Starting Windows from it** | yes (ReFS 3.x), NT 3.1 - XP x64 |
| **Tools** | `refsinst` (installer), `refsfmt` (formatter: ReFS 1.2, 2.0, 3.14), `refschk` (checker), `refsboot` (boot setup) |

## Tested

On Windows NT 3.1, 3.51, 4.0, 2000 and XP x64 under QEMU, with ReFS 3.14
volumes made by Windows 11 - 4 KB clusters (16 KB metadata pages, CRC64) and
64 KB clusters (SHA-256) - holding block clones, snapshots, integrity streams
and thousands of files: copying files in and out, `fc /b`, renaming and
deleting, Explorer, a paging file on ReFS under memory pressure (XP x64: 400
MB touched with 512 MB of RAM), shutdown. Every one of these systems started
from a ReFS volume, was used, shut down and started again.

The same copy / compare / rename / delete run passes on a Windows 8.1 ReFS 1.2
volume and on Server 2016 TH2 ReFS 2.0 volumes on every NT version, and XP x64
pages to paging files on both (600 MB touched with 512 MB of RAM).

On Windows NT 3.1, `refsfmt` formatted three partitions of one disk as ReFS
3.14 (4 KB clusters), 2.0 (64 KB clusters) and 1.2, and REFSNT then copied,
compared, renamed and deleted files on each; NT 4.0 then did the same on those volumes. `refschk` found all three clean every time. `refsfmt` volumes of 1.4 GB to
300 GB pass `refschk` and the random-operation stress tests on Linux.

After every run - clean shutdown or power cut - `refschk` finds the volume
clean: every page checksum right, every key in order, no cluster lost or
referenced twice, every file readable.

## Features

**The engine** (`src/rfs*.c`)

- The boot sector, superblocks and both checkpoints; the Minstore B+ trees
  with embedded tables; the object table and its duplicate, the schema and
  parent-child tables; the container table and virtual cluster addresses; the
  allocators with their bitmaps and summaries; block reference counts; the
  upcase table and the volume label.
- CRC64 and SHA-256 page checksums.
- **Copy-on-write, as ReFS does it**: changed pages go to new clusters, are
  checksummed bottom-up, the object rows and both object tables are updated,
  and the other checkpoint is written last with a higher clock. Freed
  clusters are not reused before the checkpoint that frees them is on the
  disk, so a power cut always leaves the last checkpoint intact.

**Files**

- Create, write, truncate, delete, rename (also across directories, with
  replace); timestamps; the read-only, hidden, system and archive attributes.
- Small files are kept in their record and moved to clusters as they grow;
  sparse files are read.
- Files shared by block clones or snapshots are copied on write.
- Integrity-stream files are read with their checksums kept, but not written.

**Three on-disk generations, one engine**

- **ReFS 3.x** is the native layout: containers and virtual cluster addresses,
  thirteen checkpoint roots, block reference counts.
- **ReFS 1.2** is quite different - 16 KB metadata blocks addressed directly
  (no containers, no virtual addresses, no reference counts), six checkpoint
  roots, file data in block-unit extents, small directories and file records
  embedded in their rows. The B+ tree, embedded-table and copy-on-write code
  is shared and parametrised by version; 1.2's allocator, checkpoint and
  record layout live in `src/rfsv1.c`. Validated against a real Windows 8.1
  volume, and by writing thousands of files, directory splits, large files,
  renames and deletes with `refschk` clean every time. ReFS 1.1 (Windows 8 /
  Server 2012) shares the layout and is read the same way.
- **ReFS 2.0** sits halfway: the container family like 3.x, but 1.x node
  headers, attribute rows and record schemas, ten checkpoint roots, an
  implied 64 MiB container size and the container table keeping a band's
  physical cluster at value `+0x28`. Validated against Server 2016 TH2
  volumes with 64 KiB and 4 KiB clusters; a directory REFSNT makes matches
  Windows' field for field.

The formats are written up in [doc/REFS1.md](doc/REFS1.md) and
[doc/REFS2.md](doc/REFS2.md).

**The driver**

- Cache Manager integration (cached reads and writes, fast I/O, memory-mapped
  files and executables), paging I/O and a paging file on ReFS.
- Change notification (see *Limitations*), byte-range locks, the volume
  label, lock and dismount for the tools, Plug and Play removal on 2000 and
  later.
- Drive letters for ReFS partitions of any type; free space shown in
  Explorer.
- One i386 binary for NT 3.1 - XP, built to fit NT 3.x's 12 KB kernel stacks
  and with every import checked against NT 3.1's kernel at build time.

A volume with page checksum errors, or a damaged primary object table, is
mounted read-only.

## Installing

`bin\i386` holds the driver and tools for Windows NT 3.1 - XP (32-bit),
`bin\amd64` the same for XP x64 / Server 2003 x64. Copy `refsnt.sys` and
`refsinst.exe` of the matching architecture to one folder and run, from an
administrator account:

```
refsinst              install, started with the system (drive letters at boot,
                      paging file possible)
refsinst /BOOT        boot start, for starting Windows from a ReFS volume
refsinst /READONLY    mount ReFS volumes read-only
refsinst /NOLETTERS   no drive letters for ReFS partitions of other types
refsinst /CASE        case-sensitive names
refsinst /HIDEDOT     hide ".names"
refsinst /U           uninstall
```

A restart applies the change. On 64-bit Windows run the amd64 `refsinst`; the
32-bit one refuses to run there.

## Settings

Under `HKLM\SYSTEM\CurrentControlSet\Services\Refsnt` (all `REG_DWORD`;
`refsinst` sets them):

| Value | Default | Meaning |
|---|---|---|
| `EnableWriteSupport` | 1 | 0 mounts every ReFS volume read-only |
| `AssignDriveLetters` | 1 | give ReFS partitions of any type drive letters |
| `HideDotFiles` | 0 | 1 gives names starting with `.` the hidden attribute |
| `CaseSensitive` | 0 | exact-case lookups only |
| `FollowSymlinks` | 1 | opening a symbolic link opens its target |
| `MountOptical` | 0 | 1 mounts ReFS in CD / DVD / BD drives too |

## Tools

```
refsfmt  X: [/FS:ReFS|ReFSv1|ReFSv2|ReFSv3] [/A:4096|64K] [/V:label] [/T:xx] [/Y]
                                              format the volume
refschk  X: [/Q] [/V]                         check the volume
refsboot X: [/S:windir] [/T:xx] [/Q]          make the volume start Windows NT
refsboot X: /U                                undo it (plain ReFS again)
```

All three also take a partition of a disk (`\\.\PhysicalDrive1 /P:2 ...`) or
an image file, on Windows and on Linux.

- **refsfmt** formats a partition, a whole disk or an image, like Windows'
  `format /FS:ReFS`:
  - `/FS:ReFSv3` (also `/FS:ReFS`, the default): ReFS 3.14, the ReFS of
    Windows 10 1709 and later, Windows 11 and Server 2019 and later;
  - `/FS:ReFSv2`: ReFS 2.0 (Windows Server 2016 Technical Preview / TH2);
  - `/FS:ReFSv1`: ReFS 1.2 (Windows 8.1, Server 2012 R2), as Windows' own
    `format /FS:ReFSv1` names it;
  - `/R:1`, `/R:2`, `/R:3` are short for the same.

  `/A:` picks 4 KB (the default) or 64 KB clusters for 2.0 and 3.14; ReFS 1.2
  has 64 KB clusters. `/V:` sets the label (up to 32 characters). The
  partition type becomes `07` unless `/T:` says otherwise. ReFS 2.0 and 3.14
  volumes are a whole number of 64 MB containers and need at least 1344 MB.

  ReFS 2.x and 3.x are full of structures only Windows' own format defines -
  the log, the schema, the system tables - so `refsfmt` builds those volumes
  from **templates of volumes Windows formatted** (Server 2016 TH2 for 2.0,
  Windows 11 for 3.14), carried inside it compressed: their first containers
  are written as they are, then everything that depends on the size - the boot
  sector and its copy, the backup superblocks, both container tables, the
  allocators and their summaries - is made for the new volume through the
  engine, copy-on-write, and committed twice so that both checkpoints describe
  it. The log area is cleared. Every volume gets its own serial number, and
  3.14 volumes their own GUID; the label is set. ReFS 1.2 volumes are built
  from scratch. Each new volume is mounted again before `refsfmt` reports
  success.
- **refschk** walks every table and checks key order and page checksums, the
  allocators against their bitmaps and summaries, every cluster against the
  allocators (lost or doubly used ones), and reads every file. On a drive it
  locks the volume first (REFSNT writes everything out and steps aside). It
  does not repair. Exit codes: 0 clean, 1 problems found, 2 could not check.
- **refsboot** makes a ReFS volume start Windows NT, and undoes it (below).

## Starting Windows NT from ReFS

NTLDR reads only FAT and NTFS, so the files it needs also get a **FAT16 view**
kept inside the ReFS volume:

- sectors 0 - 1 hold the REFSNT boot loader with a FAT16 BPB; the ReFS boot
  sector moves to sector 2 (its copy at the end of the volume stays);
- the hidden system file `REFSBOOT.SYS`, low on the volume, holds the FAT, the
  root directory and the FAT subdirectories;
- the FAT's clusters (4 KB, or the ReFS cluster, at most 32 KB) **are** the
  clusters of the ReFS files themselves, so the view always shows their
  current data.

The view holds NTLDR, NTDETECT.COM, BOOT.INI and, from the Windows directory,
what the loader reads: the kernel, HAL, SYSTEM hive, NLS tables, fonts and
every boot driver. Boot files lying outside the 65518 clusters the FAT reaches
are moved inside; REFSNT allocates in that window last, and brings the view up
to date at every shutdown.

1. Format the partition as ReFS 3.x (512-byte sectors): `refsfmt X:` or
   Windows' `format X: /fs:refs`.
2. Copy NTLDR, NTDETECT.COM, BOOT.INI and the Windows tree onto it.
3. Install REFSNT into that Windows as a boot driver: `refsinst /BOOT` on it
   before copying, or the `Refsnt` service with Start 0, ImagePath
   `System32\drivers\refsnt.sys`, in its own group "REFSNT" appended to
   `ServiceGroupOrder`.
4. `refsboot X:` - writes the boot loader and the view; on disk images it also
   marks the partition active.

While sector 0 holds the boot loader, Windows 8 and later see the volume as
RAW; `refsboot X: /U` gives it its ReFS boot sector back (and the volume no
longer starts Windows NT). Run `refsboot` again after NTLDR, the kernel, a HAL
or a boot driver was changed by a system without REFSNT.

On Windows 2000 and XP, a system copied to a new disk gets C: from REFSNT on
its first start (the Mount Manager may add a second letter that once); from
the second start on, the volume has C: alone.

## Limitations

- Volumes `refsfmt` makes are checked by REFSNT, `refschk` and the stress
  tests; whether Windows 8.1 - 11 mount them has not been tried here (the
  2.0 / 3.14 ones are Windows' own volumes resized, so they should).
- The volume label can be changed on ReFS 2.0 and 3.x volumes, not on 1.2.
- Integrity-stream files are read but not written.
- Volumes with page checksum errors or a damaged primary object table are
  mounted read-only.
- Starting Windows NT needs a ReFS 3.x volume with 512-byte sectors.
- The universal i386 binary holds change notification pending and completes
  it at cleanup (NT 3.1 has no `FsRtlNotifyFull` package), so File Manager and
  Explorer refresh on demand but never spin; the amd64 build delivers live
  notification.
- Opportunistic locks are not granted.

## Building

On Linux with `gcc-mingw-w64-i686` / `gcc-mingw-w64-x86-64` (which ship the
DDK headers), `binutils` and `python3` - no Visual C++ or WDK:

```
sh build/build_cross.sh  OUTDIR     # i386: refsnt.sys, refsinst.exe, refsfmt.exe, refsboot.exe, refschk.exe
sh build/build_amd64.sh  OUTDIR     # amd64: the same for XP x64 / Server 2003 x64
bash test/buildlinux.sh             # Linux: rfstool (engine test tool), refsfmt, refsboot, refschk
```

Ready binaries are in `bin\i386` and `bin\amd64`; the i386 programs run on
NT 3.1 and later.

## Testing

```
bash test/boottest.sh IMAGE...               # the boot view on copies of ReFS images
python3 test/stress.py IMAGE ROUNDS [SEED]   # random operations against a model (3.x)
python3 test/stressv1.py IMAGE ROUNDS [SEED] # the same on ReFS 1.2
```

The stress tests run random creates, writes, truncates, renames and deletes
through the engine, compare every file with an in-memory model and require
`rfstool check` to find the image clean.

## Source layout

| Path | What |
|---|---|
| `src/refs.h`, `src/rfssup.c`, `rfsvol.c` | on-disk structures, boot sector, superblocks, checkpoints |
| `src/rfstree.c`, `rfsalloc.c`, `rfscommit.c` | Minstore B+ trees, allocators, copy-on-write commits |
| `src/rfsfile.c`, `rfsops.c`, `rfswrite.c`, `rfsupcase.c` | files, directories, operations, upcase |
| `src/rfsv1.c` | the ReFS 1.2 layout |
| `src/rfn*.c` | the NT driver: IRP handlers, Cache Manager, drive letters |
| `boot/refsboot.S` | the boot loader |
| `fmt/refsfmt.c`, `rfsmkfs.c`, `rfsmk1.c` | the formatter: 2.0 / 3.14 from templates, 1.2 from scratch |
| `fmt/rfstmpl.h`, `tools/rfstmpl.c` | the templates, and the tool that makes them from Windows-formatted volumes |
| `fmt/`, `chk/`, `inst/`, `tools/` | `refsboot`, `refschk`, `refsinst`, the Linux test tool |
| `doc/` | the ReFS 1.2 and 2.0 format notes |

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).
