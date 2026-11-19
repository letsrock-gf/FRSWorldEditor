"""Reader for NFS World collision packs (chunk 0x3B801), shared by the col_*.py probes.

Pack (after the 8-byte 0x11 filler): u32 size, u32 section number, 8 zero bytes, then at 0x18 a table of
16-byte rows {tag, size << 8 | kind, count, offset relative to the row}: group 'CARP', group 'Arti' (count =
number of data rows), then 'Name' (the section number as text), one 'ca',<index> row per article and one 'ci'
row with all instances (64 bytes each).
"""
import struct

TRACKS = r"D:\NFS World\Data2015\TRACKS"
PACKS = TRACKS + r"\STREAML5RA_25000.BUN"


def chunks(buf):
    pos = 0
    while pos + 8 <= len(buf):
        cid, size = struct.unpack_from("<II", buf, pos)
        yield cid, pos + 8, size
        pos += 8 + size


def packs(buf):
    """(section number, pack bytes) for every 0x3B801 chunk."""
    for cid, off, size in chunks(buf):
        if cid == 0x3B801:
            data = buf[off:off + size]
            yield struct.unpack_from("<I", data, 12)[0], data


def rows(data):
    """(tag, size, kind, count, absolute offset) of every data row."""
    out = []
    for pos in range(0x38, 0x38 + 16 * struct.unpack_from("<I", data, 0x30)[0], 16):
        tag, info, count, off = struct.unpack_from("<4I", data, pos)
        out.append((tag, info >> 8, info & 0xFF, count, pos + off))
    return out


class Article:
    """Local-space triangles and barriers of one article."""

    def __init__(self, data, pos, size):
        (self.strips, self.strip_bytes, self.edges, self.edge_bytes, self.resolved, self.surfs,
         self.surf_bytes, self.flags) = struct.unpack_from("<4H2BHI", data, pos)
        base = pos + 16
        self.tris = []      # (p0, p1, p2, surface index, flags)
        self.tri_meta = []  # (strip, triangle in the strip, winding byte of the strip) per triangle
        self.barriers = []  # (p0, p1, surface index, flags); p0.y = bottom, p1.y = top
        for i in range(self.strips):
            cx, cy, cz, radius, off = struct.unpack_from("<3f2H", data, base + 16 * i)
            first = base + off
            count = data[first + 6]
            winding = data[first + 14]  # vertex 1: 0 as stored, 1 the other way round, 2 both ways
            self.tri_meta += [(i, k, winding) for k in range(count - 2)]
            pts = []
            for k in range(count):
                x, y, z, a, b = struct.unpack_from("<3h2B", data, first + 8 * k)
                pts.append((cx + x / 128.0, cy + y / 128.0, cz + z / 128.0))
                if k >= 2:
                    tri = (pts[k - 2], pts[k - 1], pts[k]) if k % 2 == 0 else (pts[k - 1], pts[k - 2], pts[k])
                    self.tris.append(tri + (a, b))
        for i in range(self.edges):
            x0, y0, z0, w0, x1, y1, z1, inv_len = struct.unpack_from("<3fI4f", data, base + self.strip_bytes + 32 * i)
            self.barriers.append(((x0, y0, z0), (x1, y1, z1), w0 & 0xFF, (w0 >> 8) & 0xFF))
        self.surfaces = struct.unpack_from("<%dI" % self.surfs, data, base + self.strip_bytes + self.edge_bytes)

    def points(self):
        for tri in self.tris:
            yield from tri[:3]
        for bar in self.barriers:
            yield from bar[:2]


class Instance:
    """64 bytes: inverse row 0 + width, u16 flags, u16 0, height, u16 0, u16 article, runtime pointer,
    inverse row 2 + length, inverse position + radius."""

    def __init__(self, data, pos):
        (r0x, r0y, r0z, self.width, self.flags, self.pad, self.height, self.group, self.article, self.ptr,
         r2x, r2y, r2z, self.length, tx, ty, tz, self.radius) = struct.unpack_from("<4f2Hf2HI8f", data, pos)
        self.row0 = (r0x, r0y, r0z)
        self.row2 = (r2x, r2y, r2z)
        self.inv_pos = (tx, ty, tz)

    def row1(self):
        a, b = self.row2, self.row0
        return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

    def to_world(self, p):
        """local = R * (world + inv_pos), so world = R^T * local - inv_pos (R rows = row0, row1, row2)."""
        r0, r1, r2 = self.row0, self.row1(), self.row2
        return tuple(r0[i] * p[0] + r1[i] * p[1] + r2[i] * p[2] - self.inv_pos[i] for i in range(3))


def game(p):
    """Collision axes (x, up, z) to the game's (x, y, z up)."""
    return (p[2], -p[0], p[1])


def load(data):
    """(articles by index, instances) of one pack."""
    arts, insts = {}, []
    for tag, size, kind, count, pos in rows(data):
        if tag >> 16 == 0x6361:
            arts[tag & 0xFFFF] = Article(data, pos, size)
        elif tag >> 16 == 0x6369:
            insts = [Instance(data, pos + 64 * i) for i in range(count)]
    return arts, insts


def section_centres():
    """{section number: (x, y, z)} from L5RA.BUN chunk 0x34110 (208-byte rows)."""
    buf = open(TRACKS + r"\L5RA.BUN", "rb").read()
    out = {}
    for cid, off, size in chunks(buf):
        if cid == 0x34110:
            for pos in range(off, off + size - 207, 208):
                number, = struct.unpack_from("<I", buf, pos + 8)
                out[number] = struct.unpack_from("<3f", buf, pos + 0x28)
    return out
