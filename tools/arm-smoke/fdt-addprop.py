#!/usr/bin/env python3
"""Add (or replace) a cell-list property in a flattened device tree.

    fdt-addprop.py IN.dtb OUT.dtb /node/path prop-name CELL...

CELLs are 32-bit integers (0x... accepted); none gives an empty property.
For testing without dtc, e.g. giving QEMU virt's PCIe host bridge a
dma-ranges (tools/arm-smoke/bounce.exp):

    ARM_VM_MACHINE=virt,gic-version=2,dumpdtb=virt.dtb bin/arm-vm -s 2 -m 2G K
    fdt-addprop.py virt.dtb dma.dtb /pcie@10000000 dma-ranges \\
        0x02000000 0 0x40000000  0 0x40000000  0 0x20000000
"""
import struct
import sys

BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


def pad4(n):
    return (n + 3) & ~3


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    src, dst, path, pname = sys.argv[1:5]
    value = b"".join(struct.pack(">I", int(c, 0) & 0xFFFFFFFF)
                     for c in sys.argv[5:])
    blob = open(src, "rb").read()
    (magic, _total, off_struct, off_strings, off_rsv, version, last_comp,
     boot_cpu, size_strings, size_struct) = struct.unpack(">10I", blob[:40])
    if magic != 0xD00DFEED or version < 17:
        sys.exit("not a v17 FDT")
    strings = bytearray(blob[off_strings:off_strings + size_strings])
    rsv = blob[off_rsv:off_struct]

    def stroff(name):
        key = name.encode() + b"\0"
        i = strings.find(key)
        while i > 0 and strings[i - 1] != 0:
            i = strings.find(key, i + 1)
        if i < 0:
            i = len(strings)
            strings.extend(key)
        return i

    nameoff = stroff(pname)
    newprop = struct.pack(">III", PROP, len(value), nameoff) + value + \
        b"\0" * (pad4(len(value)) - len(value))

    out = bytearray()
    stack = []
    found = False
    pos = off_struct
    skip_prop_in = None
    while True:
        tok, = struct.unpack(">I", blob[pos:pos + 4])
        if tok == BEGIN_NODE:
            end = blob.index(b"\0", pos + 4)
            name = blob[pos + 4:end].decode()
            nxt = pad4(end + 1)
            out += blob[pos:nxt]
            stack.append(name)
            cur = "/" + "/".join(n for n in stack[1:] if n)
            if cur == path:
                out += newprop
                found = True
                skip_prop_in = len(stack)
            pos = nxt
        elif tok == PROP:
            plen, poff = struct.unpack(">II", blob[pos + 4:pos + 12])
            nxt = pos + 12 + pad4(plen)
            if not (skip_prop_in == len(stack) and poff == nameoff):
                out += blob[pos:nxt]
            pos = nxt
        elif tok == END_NODE:
            if skip_prop_in == len(stack):
                skip_prop_in = None
            stack.pop()
            out += blob[pos:pos + 4]
            pos += 4
        elif tok == NOP:
            pos += 4
        elif tok == END:
            out += blob[pos:pos + 4]
            break
        else:
            sys.exit("bad FDT token %d at %#x" % (tok, pos))
    if not found:
        sys.exit("no node " + path)

    off_rsv2 = 40
    off_struct2 = off_rsv2 + len(rsv)
    off_strings2 = off_struct2 + len(out)
    total = off_strings2 + len(strings)
    hdr = struct.pack(">10I", magic, total, off_struct2, off_strings2,
                      off_rsv2, version, last_comp, boot_cpu, len(strings),
                      len(out))
    open(dst, "wb").write(hdr + rsv + bytes(out) + bytes(strings))


if __name__ == "__main__":
    main()
