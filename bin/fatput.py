#!/usr/bin/env python3
"""Copy a directory tree into a freshly made FAT32 file system image.

    fatput.py IMAGE SRCDIR [LABEL]

IMAGE must come straight from mkfs.fat -F 32 (nothing written to it
yet): the root directory is rewritten from scratch and clusters are
handed out in order from cluster 3.  For the Pi's boot partition
(bin/arm-pisd), since the host has no mtools and mounting needs root.

Names that fit 8.3 in one case are stored as short names (with the NT
lower-case flags for lower case), as Linux's vfat does by default;
anything else gets VFAT long-name entries and a NAME~N alias.  Files are
contiguous.  Timestamps are fixed (reproducible images).
"""
import os
import struct
import sys

ATTR_RO, ATTR_DIR, ATTR_ARCH, ATTR_LFN, ATTR_VOL = 0x01, 0x10, 0x20, 0x0F, 0x08
EOC = 0x0FFFFFFF
# 2026-01-01 00:00:00
FDATE = ((2026 - 1980) << 9) | (1 << 5) | 1
FTIME = 0
SHORT_OK = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')


class Fat32:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        self.bs = bs = self.f.read(512)
        if bs[510:512] != b'\x55\xaa' or bs[82:90] != b'FAT32   ':
            sys.exit('fatput: %s is not a FAT32 image' % path)
        (self.bps, self.spc, self.rsvd, self.nfats) = struct.unpack_from(
            '<HBHB', bs, 11)
        self.totsec = struct.unpack_from('<I', bs, 32)[0]
        self.fatsz, = struct.unpack_from('<I', bs, 36)
        self.rootclus, self.fsinfo = struct.unpack_from('<IH', bs, 44)
        self.csize = self.bps * self.spc
        self.data = self.rsvd + self.nfats * self.fatsz
        self.nclus = (self.totsec - self.data) // self.spc
        self.f.seek(self.rsvd * self.bps)
        raw = self.f.read(self.fatsz * self.bps)
        self.fat = list(struct.unpack('<%dI' % (len(raw) // 4), raw))
        if self.rootclus != 2 or any(self.fat[3:self.nclus + 2]):
            sys.exit('fatput: %s is not fresh from mkfs.fat' % path)
        self.next = 3

    def alloc(self, nbytes):
        """Contiguous chain for nbytes (at least one cluster)."""
        n = max(1, -(-nbytes // self.csize))
        first = self.next
        if first + n > self.nclus + 2:
            sys.exit('fatput: image full')
        for c in range(first, first + n - 1):
            self.fat[c] = c + 1
        self.fat[first + n - 1] = EOC
        self.next += n
        return first

    def write(self, clus, data):
        self.f.seek((self.data + (clus - 2) * self.spc) * self.bps)
        self.f.write(data)
        pad = -len(data) % self.csize
        if pad:
            self.f.write(b'\0' * pad)

    def close(self):
        raw = struct.pack('<%dI' % len(self.fat), *self.fat)
        for i in range(self.nfats):
            self.f.seek((self.rsvd + i * self.fatsz) * self.bps)
            self.f.write(raw)
        free = self.nclus - (self.next - 2)
        self.f.seek(self.fsinfo * self.bps + 488)
        self.f.write(struct.pack('<II', free, self.next))
        # BSD fsck_msdosfs also wants 55 AA at the end of the sector
        # after FSInfo, as Windows FORMAT writes it (mkfs.fat does not):
        # in the primary and the backup boot area.
        bk, = struct.unpack_from('<H', self.bs, 50)
        for base in (0, bk) if bk else (0,):
            self.f.seek((base + self.fsinfo + 1) * self.bps + self.bps - 2)
            self.f.write(b'\x55\xaa')
        self.f.close()


def short_form(name):
    """(11-byte short name, NT case flags) if name needs no LFN, else None."""
    if name in ('.', '..'):
        return None
    base, dot, ext = name.partition('.')
    if '.' in ext or not base or len(base) > 8 or len(ext) > 3:
        return None
    flags = 0
    for part, bit in ((base, 0x08), (ext, 0x10)):
        if part.upper() != part:
            if part.lower() != part:
                return None		# mixed case
            flags |= bit
        if any(c not in SHORT_OK for c in part.upper()):
            return None
    return ((base.upper().ljust(8) + ext.upper().ljust(3)).encode(), flags)


def alias(name, used):
    base, _, ext = name.rpartition('.')
    if not base:
        base, ext = name, ''
    clean = lambda s: ''.join(c for c in s.upper() if c in SHORT_OK)
    b, e = clean(base), clean(ext)[:3]
    for i in range(1, 1000000):
        tail = '~%d' % i
        s = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if s not in used:
            return s.encode()
    sys.exit('fatput: no alias for %s' % name)


def checksum(sn):
    s = 0
    for c in sn:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def dirent(sn, attr, clus, size, flags=0):
    return struct.pack('<11sBBBHHHHHHHI', sn, attr, flags, 0, FTIME, FDATE,
                       FDATE, clus >> 16, FTIME, FDATE, clus & 0xFFFF, size)


def lfn_entries(name, sn):
    u = name.encode('utf-16-le') + b'\0\0'
    u += b'\xff' * (-len(u) % 26)
    parts = [u[i:i + 26] for i in range(0, len(u), 26)]
    ck = checksum(sn)
    out = []
    for i in range(len(parts), 0, -1):
        p = parts[i - 1]
        seq = i | (0x40 if i == len(parts) else 0)
        out.append(struct.pack('<B10sBBB12sH4s', seq, p[0:10], ATTR_LFN, 0,
                               ck, p[10:22], 0, p[22:26]))
    return out


def plan(names):
    """Short name, flags and entry count for each child name."""
    used, res = set(), []
    for n in names:
        sf = short_form(n)
        if sf and sf[0].decode() not in used:
            used.add(sf[0].decode())
            res.append((n, sf[0], sf[1], None))
        else:
            sn = alias(n, used)
            used.add(sn.decode())
            res.append((n, sn, 0, lfn_entries(n, sn)))
    return res


def put_dir(fs, src, clus, parent, label=None):
    names = sorted(os.listdir(src))
    kids = plan(names)
    ents = []
    if parent is None:
        if label:
            ents.append(dirent(label.upper().ljust(11)[:11].encode(),
                               ATTR_VOL, 0, 0))
    else:
        ents.append(dirent(b'.          ', ATTR_DIR, clus, 0))
        ents.append(dirent(b'..         ', ATTR_DIR,
                           0 if parent == fs.rootclus else parent, 0))
    nents = len(ents) + sum(1 + len(l or []) for _, _, _, l in kids)
    if parent is None:
        # The root's first cluster is mkfs's cluster 2; extend if needed.
        need = max(1, -(-nents * 32 // fs.csize))
        if need > 1:
            more = fs.alloc((need - 1) * fs.csize)
            fs.fat[clus] = more
        chain = [clus] + list(range(fs.next - (need - 1), fs.next))
    else:
        chain = None
    for n, sn, flags, lfn in kids:
        p = os.path.join(src, n)
        if os.path.isdir(p):
            sub = sorted(os.listdir(p))
            subn = 2 + sum(1 + len(l or []) for _, _, _, l in plan(sub))
            c = fs.alloc(subn * 32)
            put_dir(fs, p, c, clus)
            ents += (lfn or []) + [dirent(sn, ATTR_DIR, c, 0, flags)]
        else:
            with open(p, 'rb') as f:
                data = f.read()
            c = 0				# an empty file has no cluster
            if data:
                c = fs.alloc(len(data))
                fs.write(c, data)
            ents += (lfn or []) + [dirent(sn, ATTR_ARCH, c, len(data), flags)]
    blob = b''.join(ents)
    if chain is None:
        fs.write(clus, blob)
    else:
        # Root: the chain may not be contiguous (cluster 2, then more).
        for i, c in enumerate(chain):
            fs.write(c, blob[i * fs.csize:(i + 1) * fs.csize])


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    fs = Fat32(sys.argv[1])
    put_dir(fs, sys.argv[2], fs.rootclus, None,
            sys.argv[3] if len(sys.argv) == 4 else None)
    fs.close()


if __name__ == '__main__':
    main()
