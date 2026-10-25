"""LZ4 block decoder (the block format of the LZ4 specification, as LZ4_compress_default writes it).

A block is a series of sequences. Each starts with a token byte: its high nibble is the literal count, its low
nibble the match length minus 4; a nibble of 15 continues with bytes that are added on while they are 255. Then come
the literals, a little-endian u16 offset back into the output (1..65535), and the match is copied from there (it may
overlap what it writes). The last sequence has literals only.
"""


def _length(src, i, n):
    while True:
        b = src[i]
        i += 1
        n += b
        if b != 255:
            return n, i


def decompress_block(src, size):
    """The `size` bytes a block decodes to."""
    out = bytearray()
    i, end = 0, len(src)
    try:
        while True:
            token = src[i]
            i += 1
            lit = token >> 4
            if lit == 15:
                lit, i = _length(src, i, lit)
            out += src[i:i + lit]
            i += lit
            if i >= end:
                break
            off = src[i] | src[i + 1] << 8
            i += 2
            ml = token & 15
            if ml == 15:
                ml, i = _length(src, i, ml)
            ml += 4
            start = len(out) - off
            if off == 0 or start < 0:
                raise ValueError('LZ4 match offset %d at output %d' % (off, len(out)))
            if off >= ml:
                out += out[start:start + ml]
            else:                                # overlapping: the last `off` bytes repeat
                pattern = out[start:]
                reps, rest = divmod(ml, off)
                out += pattern * reps + pattern[:rest]
    except IndexError:
        raise ValueError('LZ4 block ends inside a sequence') from None
    if len(out) != size or i != end:
        raise ValueError('LZ4 block decodes to %d bytes, expected %d' % (len(out), size))
    return bytes(out)
