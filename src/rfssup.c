/*
 * REFSNT - support routines: byte order, checksums (CRC32-C, CRC-64/NVME,
 * SHA-256), the boot sector checksum, UTF-16 <-> UTF-8.
 */
#include "rfseng.h"

USHORT RfsGet16(const VOID *P) { const UCHAR *b = (const UCHAR *)P; return (USHORT)(b[0] | (b[1] << 8)); }
ULONG RfsGet32(const VOID *P) { const UCHAR *b = (const UCHAR *)P; return b[0] | (b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24); }
ULONGLONG RfsGet64(const VOID *P) { const UCHAR *b = (const UCHAR *)P; return RfsGet32(b) | ((ULONGLONG)RfsGet32(b + 4) << 32); }
VOID RfsPut16(PVOID P, USHORT V) { PUCHAR b = (PUCHAR)P; b[0] = (UCHAR)V; b[1] = (UCHAR)(V >> 8); }
VOID RfsPut32(PVOID P, ULONG V) { PUCHAR b = (PUCHAR)P; b[0] = (UCHAR)V; b[1] = (UCHAR)(V >> 8); b[2] = (UCHAR)(V >> 16); b[3] = (UCHAR)(V >> 24); }
VOID RfsPut64(PVOID P, ULONGLONG V) { RfsPut32(P, (ULONG)V); RfsPut32((PUCHAR)P + 4, (ULONG)(V >> 32)); }

VOID RfsZero(PVOID P, ULONG N) { PUCHAR b = (PUCHAR)P; while (N--) *b++ = 0; }
VOID RfsCopy(PVOID D, const VOID *S, ULONG N) { PUCHAR d = (PUCHAR)D; const UCHAR *s = (const UCHAR *)S; while (N--) *d++ = *s++; }
VOID
RfsMove(PVOID D, const VOID *S, ULONG N)
{
    PUCHAR d = (PUCHAR)D;
    const UCHAR *s = (const UCHAR *)S;
    if (d < s) { while (N--) *d++ = *s++; }
    else { d += N; s += N; while (N--) *--d = *--s; }
}
int
RfsCmp(const VOID *A, const VOID *B, ULONG N)
{
    const UCHAR *a = (const UCHAR *)A, *b = (const UCHAR *)B;
    for (; N; N--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
PVOID
RfsAllocZero(ULONG Bytes)
{
    PVOID p = RfsHostAlloc(Bytes ? Bytes : 1);
    if (p) RfsZero(p, Bytes);
    return p;
}

/* ---- CRC32-C (Castagnoli, reflected 0x82F63B78) -------------------------------------------- */
static ULONG Crc32cTable[256];
static ULONGLONG Crc64Table[256];
static BOOLEAN TablesReady;

static VOID
MakeTables(VOID)
{
    ULONG i, k;
    for (i = 0; i < 256; i++) {
        ULONG c = i;
        ULONGLONG d = i;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78UL : c >> 1;
            d = (d & 1) ? (d >> 1) ^ 0x9A6C9329AC4BC9B5ULL : d >> 1;
        }
        Crc32cTable[i] = c;
        Crc64Table[i] = d;
    }
    TablesReady = TRUE;
}

ULONG
RfsCrc32c(ULONG Crc, const VOID *Data, ULONG Length)
{
    const UCHAR *p = (const UCHAR *)Data;
    ULONG c = ~Crc;
    if (!TablesReady) MakeTables();
    while (Length--) c = Crc32cTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* CRC-64/NVME: reflected 0xAD93D23594C93659, init and xorout all ones */
ULONGLONG
RfsCrc64(ULONGLONG Crc, const VOID *Data, ULONG Length)
{
    const UCHAR *p = (const UCHAR *)Data;
    ULONGLONG c = ~Crc;
    if (!TablesReady) MakeTables();
    while (Length--) c = Crc64Table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* ---- SHA-256 ---------------------------------------------------------------------------------- */
static const ULONG K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static VOID
Sha256Block(ULONG H[8], const UCHAR *p)
{
    ULONG w[64], a, b, c, d, e, f, g, h, i;
    for (i = 0; i < 16; i++) w[i] = ((ULONG)p[4 * i] << 24) | ((ULONG)p[4 * i + 1] << 16) | ((ULONG)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        ULONG s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        ULONG s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = H[0]; b = H[1]; c = H[2]; d = H[3]; e = H[4]; f = H[5]; g = H[6]; h = H[7];
    for (i = 0; i < 64; i++) {
        ULONG S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        ULONG ch = (e & f) ^ (~e & g);
        ULONG t1 = h + S1 + ch + K256[i] + w[i];
        ULONG S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        ULONG mj = (a & b) ^ (a & c) ^ (b & c);
        ULONG t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

VOID
RfsSha256(const VOID *Data, ULONG Length, UCHAR Digest[32])
{
    ULONG H[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const UCHAR *p = (const UCHAR *)Data;
    UCHAR tail[128];
    ULONG n = Length, i, t;
    ULONGLONG bits = (ULONGLONG)Length * 8;
    while (n >= 64) { Sha256Block(H, p); p += 64; n -= 64; }
    RfsZero(tail, sizeof(tail));
    RfsCopy(tail, p, n);
    tail[n] = 0x80;
    t = (n < 56) ? 64 : 128;
    for (i = 0; i < 8; i++) tail[t - 1 - i] = (UCHAR)(bits >> (8 * i));
    Sha256Block(H, tail);
    if (t == 128) Sha256Block(H, tail + 64);
    for (i = 0; i < 8; i++) {
        Digest[4 * i] = (UCHAR)(H[i] >> 24); Digest[4 * i + 1] = (UCHAR)(H[i] >> 16);
        Digest[4 * i + 2] = (UCHAR)(H[i] >> 8); Digest[4 * i + 3] = (UCHAR)H[i];
    }
}

ULONG
RfsChecksum(UCHAR Type, const VOID *Data, ULONG Length, PUCHAR Out)
{
    switch (Type) {
    case REFS_CK_CRC32C: RfsPut32(Out, RfsCrc32c(0, Data, Length)); return 4;
    case REFS_CK_CRC64: RfsPut64(Out, RfsCrc64(0, Data, Length)); return 8;
    case REFS_CK_SHA256: RfsSha256(Data, Length, Out); return 32;
    }
    return 0;
}

/* FILE_SYSTEM_RECOGNITION_STRUCTURE checksum: bytes 3 .. Size, skipping the checksum itself */
USHORT
RfsVbrChecksum(const UCHAR *Vbr, ULONG Size)
{
    USHORT cs = 0;
    ULONG i;
    for (i = 3; i < Size; i++) {
        if (i == REFS_VBR_CHECKSUM || i == REFS_VBR_CHECKSUM + 1) continue;
        cs = (USHORT)(((cs & 1) ? 0x8000 : 0) + (cs >> 1) + Vbr[i]);
    }
    return cs;
}

/* ---- UTF-16 <-> UTF-8 ------------------------------------------------------------------------- */
ULONG
RfsUtf16ToUtf8(const USHORT *In, ULONG InLen, char *Out, ULONG OutMax)
{
    ULONG i, n = 0;
    for (i = 0; i < InLen; i++) {
        ULONG c = In[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < InLen && In[i + 1] >= 0xDC00 && In[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (In[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) { if (n + 1 > OutMax) break; Out[n++] = (char)c; }
        else if (c < 0x800) { if (n + 2 > OutMax) break; Out[n++] = (char)(0xC0 | (c >> 6)); Out[n++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            if (n + 3 > OutMax) break;
            Out[n++] = (char)(0xE0 | (c >> 12)); Out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); Out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (n + 4 > OutMax) break;
            Out[n++] = (char)(0xF0 | (c >> 18)); Out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            Out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); Out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (n < OutMax) Out[n] = 0;
    return n;
}

ULONG
RfsUtf8ToUtf16(const char *In, ULONG InLen, PUSHORT Out, ULONG OutMax)
{
    const UCHAR *p = (const UCHAR *)In;
    ULONG i = 0, n = 0;
    while (i < InLen) {
        ULONG c = p[i], need, k;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) { need = 1; c &= 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; c &= 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; c &= 0x07; }
        else return (ULONG)-1;
        if (i + need >= InLen + (need ? 0 : 1) && need) { if (i + need > InLen - 1 + 1) return (ULONG)-1; }
        for (k = 1; k <= need; k++) {
            if (i + k >= InLen || (p[i + k] & 0xC0) != 0x80) return (ULONG)-1;
            c = (c << 6) | (p[i + k] & 0x3F);
        }
        i += need + 1;
        if (c >= 0x10000) {
            if (n + 2 > OutMax) return (ULONG)-1;
            c -= 0x10000;
            Out[n++] = (USHORT)(0xD800 + (c >> 10));
            Out[n++] = (USHORT)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 > OutMax) return (ULONG)-1;
            Out[n++] = (USHORT)c;
        }
    }
    return n;
}
