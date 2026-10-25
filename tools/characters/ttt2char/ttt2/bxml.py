"""TTT2 binary XML ("BXML", big-endian): the item databases (`<code>.xml`), read and written byte for byte.

    header (0x20): "BXML", u32 file size, u32 string-table offset S, u32 0, u32 node count N, u32 0, u32 0, u32 0
    N nodes x 28 bytes from 0x20, in pre-order: u32 tag (offset into the string table), u32 depth, u32 next sibling
        (0xFFFFFFFF: none), u32 first child (0xFFFFFFFF: none), u32 0, u32 attribute count, u32 first attribute
        (0xFFFFFFFF when there is none)
    attributes from 0x20 + 28 N: {u32 name, u32 value} (offsets into the string table), in node order
    string table at S: NUL-terminated UTF-8, each string once, in order of first use (per node: the tag, then each
        attribute's name and value); the file ends after the last string.

An element is {'tag', 'attrs': [(name, value)], 'children': [...]}.
"""
import struct
from xml.sax.saxutils import quoteattr

NONE = 0xFFFFFFFF


def parse(d):
    """-> list of the top-level elements."""
    if d[:4] != b'BXML':
        raise ValueError('not a BXML file')
    size, strings, _, count = struct.unpack_from('>4I', d, 4)
    if size != len(d):
        raise ValueError('BXML size %d, file %d bytes' % (size, len(d)))
    attr_base = 0x20 + 28 * count

    def s(o):
        return d[strings + o:d.index(b'\0', strings + o)].decode('utf-8')

    nodes = [struct.unpack_from('>7I', d, 0x20 + 28 * i) for i in range(count)]

    def build(i):
        tag, depth, nxt, child, _, cnt, start = nodes[i]
        attrs = []
        for k in range(cnt):
            a, b = struct.unpack_from('>II', d, attr_base + 8 * (start + k))
            attrs.append((s(a), s(b)))
        el = dict(tag=s(tag), attrs=attrs, children=[])
        c = child
        while c != NONE:
            el['children'].append(build(c))
            c = nodes[c][2]
        return el

    roots, i = [], 0
    while i != NONE:
        roots.append(build(i))
        i = nodes[i][2]
    return roots


def write(roots):
    flat = []                                   # pre-order: [element, depth, next sibling, first child]

    def walk(el, depth):
        idx = len(flat)
        flat.append([el, depth, NONE, NONE])
        prev = None
        for c in el['children']:
            ci = walk(c, depth + 1)
            if prev is None:
                flat[idx][3] = ci
            else:
                flat[prev][2] = ci
            prev = ci
        return idx

    prev = None
    for r in roots:
        ri = walk(r, 0)
        if prev is not None:
            flat[prev][2] = ri
        prev = ri
    strtab = bytearray()
    offs = {}

    def so(t):
        if t not in offs:
            offs[t] = len(strtab)
            strtab.extend(t.encode('utf-8') + b'\0')
        return offs[t]

    nodes, attrs, na = bytearray(), bytearray(), 0
    for el, depth, nxt, child in flat:
        tag = so(el['tag'])
        cnt = len(el['attrs'])
        start = na if cnt else NONE
        for a, v in el['attrs']:
            attrs += struct.pack('>II', so(a), so(v))
        na += cnt
        nodes += struct.pack('>7I', tag, depth, nxt, child, 0, cnt, start)
    strings = 0x20 + len(nodes) + len(attrs)
    hdr = b'BXML' + struct.pack('>7I', strings + len(strtab), strings, 0, len(flat), 0, 0, 0)
    return hdr + bytes(nodes) + bytes(attrs) + bytes(strtab)


def to_xml(el, ind=0):
    """An element as indented XML text (for looking at a database)."""
    a = ''.join(' %s=%s' % (k, quoteattr(v)) for k, v in el['attrs'])
    if not el['children']:
        return '%s<%s%s/>\n' % ('  ' * ind, el['tag'], a)
    return '%s<%s%s>\n%s%s</%s>\n' % ('  ' * ind, el['tag'], a, ''.join(to_xml(c, ind + 1) for c in el['children']),
                                       '  ' * ind, el['tag'])
