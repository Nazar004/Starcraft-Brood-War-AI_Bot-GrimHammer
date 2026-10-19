"""Read-only MPQ/CHK inspection. Does not launch StarCraft or the bot."""
import bz2
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path


def explode_binary(data):
    """PKWARE binary stream; canonical codes as documented by zlib/contrib/blast.

    Format reference: https://github.com/madler/zlib/blob/master/contrib/blast/blast.c
    This inspector only needs uncoded literals (the SCX/tileset archive format).
    """
    assert data[0] == 0 and 4 <= data[1] <= 6
    offset = 16

    def bits(n):
        nonlocal offset
        value = 0
        for i in range(n):
            value |= ((data[offset // 8] >> (offset % 8)) & 1) << i
            offset += 1
        return value

    def table(repetitions):
        lengths = [length & 15 for length in repetitions
                   for _ in range((length >> 4) + 1)]
        counts = Counter(lengths)
        symbols = sorted(range(len(lengths)), key=lambda i: (lengths[i], i))
        return counts, symbols

    def symbol(code_table):
        counts, symbols = code_table
        code = first = index = 0
        for length in range(1, 14):
            code = (code << 1) | (1 - bits(1))
            count = counts[length]
            if first <= code < first + count:
                return symbols[index + code - first]
            index += count
            first = (first + count) << 1
        raise ValueError("invalid PKWARE code")

    lengths = table([2, 35, 36, 53, 38, 23])
    distances = table([2, 20, 53, 230, 247, 151, 248])
    bases = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
    extras = [0] * 8 + list(range(1, 9))
    out = bytearray()
    while True:
        if not bits(1):
            out.append(bits(8))
            continue
        code = symbol(lengths)
        length = bases[code] + bits(extras[code])
        if length == 519:
            return bytes(out)
        extra = 2 if length == 2 else data[1]
        distance = (symbol(distances) << extra) + bits(extra) + 1
        assert distance <= len(out)
        for _ in range(length):
            out.append(out[-distance])

CRYPT = [0] * 0x500
seed = 0x100001
for low in range(256):
    for high in range(5):
        seed = (seed * 125 + 3) % 0x2AAAAB
        upper = (seed & 65535) << 16
        seed = (seed * 125 + 3) % 0x2AAAAB
        CRYPT[low + high * 256] = upper | (seed & 65535)


def mpq_hash(name, kind):
    a, b = 0x7FED7FED, 0xEEEEEEEE
    for c in name.upper().encode("ascii"):
        a = (CRYPT[(kind << 8) + c] ^ (a + b)) & 0xFFFFFFFF
        b = (c + a + b + (b << 5) + 3) & 0xFFFFFFFF
    return a


def decrypt(buf, key):
    out = []
    seed = 0xEEEEEEEE
    end = len(buf) // 4 * 4
    for (v,) in struct.iter_unpack("<I", buf[:end]):
        seed = (seed + CRYPT[0x400 + (key & 255)]) & 0xFFFFFFFF
        value = v ^ ((key + seed) & 0xFFFFFFFF)
        key = (((~key << 21) + 0x11111111) | (key >> 11)) & 0xFFFFFFFF
        seed = (value + seed + (seed << 5) + 3) & 0xFFFFFFFF
        out.append(value)
    return struct.pack("<" + "I" * len(out), *out) + buf[end:]


def read_mpq(path, name):
    data = Path(path).read_bytes()
    base = data.index(b"MPQ\x1a")
    data = data[base:]
    magic, hs, size, ver, shift, ht, bt, hn, bn = struct.unpack_from("<4sIIHHIIII", data)
    assert ver == 0
    hashes = decrypt(data[ht:ht + hn * 16], mpq_hash("(hash table)", 3))
    blocks = decrypt(data[bt:bt + bn * 16], mpq_hash("(block table)", 3))
    block = None
    for i in range(hn):
        a, b, loc, plat, index = struct.unpack_from("<IIHHI", hashes, i * 16)
        if (a, b) == (mpq_hash(name, 1), mpq_hash(name, 2)):
            block = struct.unpack_from("<IIII", blocks, index * 16)
            break
    if block is None:
        raise KeyError(name)
    pos, packed, length, flags = block
    raw = data[pos:pos + packed]
    sector = 512 << shift
    n = (length + sector - 1) // sector
    key = mpq_hash(name.split("\\")[-1], 3)
    if flags & 0x20000:
        key = ((key + pos) ^ length) & 0xFFFFFFFF
    if not flags & 0x300:
        return decrypt(raw, key) if flags & 0x10000 else raw
    assert not flags & 0x1000000, "single-unit compression unsupported"
    table = raw[:4 * (n + 1)]
    if flags & 0x10000:
        table = decrypt(table, (key - 1) & 0xFFFFFFFF)
    offsets = struct.unpack("<" + "I" * (n + 1), table)
    chunks = []
    for i in range(n):
        chunk = raw[offsets[i]:offsets[i + 1]]
        if flags & 0x10000:
            chunk = decrypt(chunk, (key + i) & 0xFFFFFFFF)
        expected = min(sector, length - i * sector)
        if len(chunk) < expected:
            kind = chunk[0] if flags & 0x200 else 8
            payload = chunk[1:] if flags & 0x200 else chunk
            if kind == 2:
                chunk = zlib.decompress(payload)
            elif kind == 16:
                chunk = bz2.decompress(payload)
            elif kind == 8:
                chunk = explode_binary(payload)
            else:
                raise ValueError(("compression", kind))
        assert len(chunk) == expected
        chunks.append(chunk)
    return b"".join(chunks)


def read_chk(path):
    chk = read_mpq(path, "staredit\\scenario.chk")
    off, sections = 0, {}
    while off + 8 <= len(chk):
        tag, size = struct.unpack_from("<4sI", chk, off)
        sections[tag] = chk[off + 8:off + 8 + size]
        off += 8 + size
    return sections


def terrain_walkability(sections, archive):
    width, height = struct.unpack("<HH", sections[b"DIM "])
    era = struct.unpack("<H", sections[b"ERA "])[0] & 7
    name = ["badlands", "platform", "install", "ashworld", "jungle",
            "desert", "ice", "twilight"][era]
    cv5 = read_mpq(archive, "tileset\\" + name + ".cv5")
    vf4 = read_mpq(archive, "tileset\\" + name + ".vf4")
    tiles = struct.unpack("<" + "H" * (width * height), sections[b"MTXM"])
    walk = []
    for y in range(height * 4):
        row = []
        for x in range(width * 4):
            tile = tiles[(y // 4) * width + x // 4]
            mega = struct.unpack_from("<H", cv5, (tile >> 4) * 52 + 20 + (tile & 15) * 2)[0]
            flags = struct.unpack_from("<H", vf4, mega * 32 + ((y % 4) * 4 + x % 4) * 2)[0]
            row.append(bool(flags & 1))
        walk.append(row)
    return walk


if __name__ == "__main__":
    sections = read_chk(sys.argv[1])
    print("dimensions", struct.unpack("<HH", sections[b"DIM "]),
          "tileset", struct.unpack("<H", sections[b"ERA "])[0])
    units = sections.get(b"UNIT", b"")
    print("unit types", Counter(struct.unpack_from("<H", units, i + 8)[0]
                                for i in range(0, len(units), 36)))
    print("sprites", [struct.unpack_from("<HHHBBH", sections[b"THG2"], i)
                      for i in range(0, len(sections.get(b"THG2", b"")), 10)])
