"""TTT2's sound banks (NUS3BANK, little-endian), read and rebuilt with other audio.

    "NUS3", u32 size of the rest, "BANKTOC ", u32 table size, u32 chunk count, {4-byte tag, u32 size} per chunk;
    then the chunks {tag, u32 size, data} in that order: PROP, BINF (u32 0, u32 3, u8 name length with NUL, name,
    padding to 4, u32 the bank's id in the sound project), GRP, DTON, TONE, JUNK, PACK (the tones' audio)
    TONE: u32 count, {u32 offset, u32 size} per tone (from the chunk's data), the tones: 12 bytes (8 for a link
          tone, which plays other tones: {f32 weight, u32 tone index, ...} after its last 0xFFFFFFFF), u8 name length
          with NUL, name, padding to 4, {u32 0, u32 8, u32 offset into PACK, u32 size}, parameters; near the end,
          after the last 0xFFFFFFFF: {u32 sample rate, u32 channels, u32 samples}. Short entries (16 bytes or less)
          are empty places.
    a tone's audio: IDSP (dsp.py) or a RIFF WAVE file (16-bit PCM)

A fighter has four: se_Chr_<code> (shouts, damage and KO voices, the announcer's call of its name), se_Dwi_<code>
(win quotes), rm_Dst_<code> (intro lines), rm_Dwi_<code> (the long win quote one win pose streams).
"""
import struct

PACK_ALIGN = 16


class Bank:
    def __init__(self, d):
        if d[:4] != b'NUS3' or d[8:16] != b'BANKTOC ':
            raise ValueError('not a NUS3BANK')
        toc, n = struct.unpack_from('<II', d, 16)
        order = [(d[24 + 8 * i:28 + 8 * i], struct.unpack_from('<I', d, 28 + 8 * i)[0]) for i in range(n)]
        pos = 0x14 + toc
        self.chunks = []
        for tag, size in order:
            if d[pos:pos + 4] != tag:
                raise ValueError('NUS3BANK chunk table disagrees at %s' % tag)
            self.chunks.append([tag, d[pos + 8:pos + 8 + size]])
            pos += 8 + size
        tone = self.chunk(b'TONE')
        count, = struct.unpack_from('<I', tone, 0)
        self.tones = []
        for i in range(count):
            o, s = struct.unpack_from('<II', tone, 4 + 8 * i)
            self.tones.append(bytearray(tone[o:o + s]))
        pack = self.chunk(b'PACK') if self.has_chunk(b'PACK') else b''
        places = [self._place(t) for t in self.tones]
        self.audio = [pack[o:o + s] if o is not None else None for o, s in places]
        self.source = [o for o, _ in places]           # tones with the same source share their audio

    def has_chunk(self, tag):
        return any(t == tag for t, _ in self.chunks)

    def chunk(self, tag):
        for t, data in self.chunks:
            if t == tag:
                return data
        raise KeyError(tag)

    def set_chunk(self, tag, data):
        for c in self.chunks:
            if c[0] == tag:
                c[1] = bytes(data)
                return
        raise KeyError(tag)

    @staticmethod
    def _name_at(t):
        """Offset of a tone's name length byte."""
        return 8 if t[4] == 0x7F else 12

    @staticmethod
    def _fields(t):
        """Offset of a tone's {0, 8, PACK offset, size} block, or None for an empty place or a link tone."""
        if len(t) <= 16:
            return None
        at = Bank._name_at(t)
        n = t[at]
        o = at + 1 + n
        o += (-o) % 4
        if o + 16 > len(t) or struct.unpack_from('<I', t, o + 4)[0] != 8:
            return None
        return o

    def _place(self, t):
        o = self._fields(t)
        if o is None:
            return None, 0
        return struct.unpack_from('<II', t, o + 8)

    def name(self, i):
        t = self.tones[i]
        if len(t) <= 16:
            return None
        at = self._name_at(t)
        return t[at + 1:at + t[at]].decode('latin1')

    def has_audio(self, i):
        return self._fields(self.tones[i]) is not None

    def format(self, i):
        """(sample rate, channels, samples) of a tone."""
        t = self.tones[i]
        o = bytes(t).rindex(b'\xff\xff\xff\xff') + 4
        return struct.unpack_from('<III', t, o)

    def replace(self, i, audio, rate, channels, samples):
        """Give tone i other audio (IDSP or WAVE bytes)."""
        t = self.tones[i]
        if self._fields(t) is None:
            raise ValueError('tone %d has no audio' % i)
        self.audio[i] = bytes(audio)
        self.source[i] = ('replaced', i)
        o = bytes(t).rindex(b'\xff\xff\xff\xff') + 4
        struct.pack_into('<III', t, o, rate, channels, samples)

    def append(self, tone, audio=None, share=None):
        """Add a tone entry (bytes; its PACK place is set on writing) -> its index. audio: its audio bytes (an entry
        with a PACK place), or share: the index of a tone whose audio it plays too."""
        self.tones.append(bytearray(tone))
        self.audio.append(bytes(audio) if audio is not None else (self.audio[share] if share is not None else None))
        self.source.append(self.source[share] if share is not None else ('appended', len(self.tones) - 1))
        return len(self.tones) - 1

    def renamed(self, i, name):
        """Tone i's entry with another name of the same length."""
        t = bytearray(self.tones[i])
        old = self.name(i).encode('latin1')
        new = name.encode('latin1')
        if len(new) != len(old):
            raise ValueError('a tone name can only change to one of the same length')
        at = self._name_at(t) + 1
        t[at:at + len(new)] = new
        return t

    def links(self, i):
        """The tones a link tone (an entry without audio) plays: [(offset in the entry, tone index)]."""
        t = bytes(self.tones[i])
        start = t.rindex(b'\xff\xff\xff\xff') + 4
        out = []
        for o in range(start, len(t) - 7, 4):
            if t[o:o + 4] == b'\x00\x00\x80\x3f':                  # weight 1.0, then the tone's index
                k, = struct.unpack_from('<I', t, o + 4)
                if k < len(self.tones) and self.has_audio(k):
                    out.append((o + 4, k))
        return out

    def rename(self, name, project_id):
        """The bank's name (BINF) and its id in the sound project."""
        binf = self.chunk(b'BINF')
        head = binf[:8]
        n = name.encode('latin1') + b'\0'
        body = bytearray(head) + bytes([len(n)]) + n
        while len(body) % 4:
            body.append(0)
        body += struct.pack('<I', project_id)
        self.set_chunk(b'BINF', body)

    def write(self):
        pack = bytearray()
        placed = {}                                     # source -> its offset
        for i, t in enumerate(self.tones):
            o = self._fields(t)
            if o is None or self.audio[i] is None:
                continue
            if not self.audio[i]:                       # a tone without sound: as it is
                continue
            if self.source[i] not in placed:
                while len(pack) % PACK_ALIGN:
                    pack.append(0)
                placed[self.source[i]] = len(pack)
                pack += self.audio[i]
            struct.pack_into('<II', t, o + 8, placed[self.source[i]], len(self.audio[i]))
        table = bytearray(struct.pack('<I', len(self.tones)))
        body = bytearray()
        start = 4 + 8 * len(self.tones)
        for t in self.tones:
            table += struct.pack('<II', start + len(body), len(t))
            body += t
            while len(body) % 4:
                body.append(0)
        self.set_chunk(b'TONE', table + body)
        if self.has_chunk(b'PACK'):
            self.set_chunk(b'PACK', pack)
        toc = struct.pack('<I', len(self.chunks)) + b''.join(tag + struct.pack('<I', len(data))
                                                            for tag, data in self.chunks)
        rest = b'BANKTOC ' + struct.pack('<I', len(toc)) + toc + b''.join(
            tag + struct.pack('<I', len(data)) + data for tag, data in self.chunks)
        return b'NUS3' + struct.pack('<I', len(rest)) + rest


def wave(channels, rate):
    """A 16-bit PCM RIFF WAVE file from int16 channel arrays of equal length."""
    import numpy as np
    data = np.stack(channels, 1).astype('<i2').tobytes()
    n = len(channels)
    fmt = struct.pack('<HHIIHH', 1, n, rate, rate * 2 * n, 2 * n, 16)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', len(body)) + body
