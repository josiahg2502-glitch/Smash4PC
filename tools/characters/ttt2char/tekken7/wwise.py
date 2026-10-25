"""Tekken 7 PC's Wwise sound banks (`WwiseAudio/Windows/*.bnk`, bank version 118), read-only.

    bank: chunks {4-byte tag, u32 size} (little-endian): BKHD, DIDX (media index: {u32 media id, u32 offset into
          DATA, u32 size}), DATA (the embedded media), HIRC (objects: {u8 type, u32 size, u32 id, body})
    HIRC: type 4 event (u32 action count, action ids), type 3 action (u16 action type, u32 target id), type 2 sound
          (u32 codec plugin, u8 stream type, u32 media id, u32 size, ...)
    event and other names are not stored: an object's id is the 32-bit FNV-1 hash of its lower-case name

A medium is a RIFF WAVE file ("WEM"); Tekken 7's voices use Wwise's IMA ADPCM (format tag 2): blocks of 36 bytes per
channel (the channels' blocks one after the other), each a header {s16 sample, u8 step index, u8 0} and 32 bytes of
4-bit codes, low half first, 64 samples per block.
"""
import struct

import numpy as np

_IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
              88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
              658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
              3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
              16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
_IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def fnv(name):
    """A Wwise object id: FNV-1 (32 bits) of the lower-case name."""
    h = 2166136261
    for c in name.lower().encode('latin1'):
        h = ((h * 16777619) & 0xFFFFFFFF) ^ c
    return h


def _riff_chunks(d, pos):
    out = {}
    while pos + 8 <= len(d):
        tag, size = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
        out.setdefault(tag, (pos + 8, size))
        pos += 8 + size + (size & 1)
    return out


class Bank:
    def __init__(self, d):
        self.d = d
        pos, self.ch = 0, {}
        while pos + 8 <= len(d):
            tag, size = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
            self.ch[tag] = (pos + 8, size)
            pos += 8 + size
        if b'BKHD' not in self.ch:
            raise ValueError('not a Wwise sound bank')
        self.objects = {}
        if b'HIRC' in self.ch:
            p, _ = self.ch[b'HIRC']
            n, = struct.unpack_from('<I', d, p)
            q = p + 4
            for _ in range(n):
                kind, size, oid = struct.unpack_from('<BII', d, q)
                self.objects[oid] = (kind, d[q + 9:q + 5 + size])
                q += 5 + size
        self.media = {}
        if b'DIDX' in self.ch and b'DATA' in self.ch:
            p, size = self.ch[b'DIDX']
            data = self.ch[b'DATA'][0]
            for i in range(size // 12):
                mid, off, n = struct.unpack_from('<III', d, p + 12 * i)
                self.media[mid] = d[data + off:data + off + n]

    def has_event(self, name):
        o = self.objects.get(fnv(name))
        return o is not None and o[0] == 4

    def event_media(self, name):
        """The ids of the embedded media an event plays (its Play actions' sounds, through containers), in
        order. Other actions (Stop: the battle bank's name calls also stop a shared announcer line) are left out."""
        kind, body = self.objects.get(fnv(name), (None, None))
        if kind != 4:
            raise KeyError(name)
        n, = struct.unpack_from('<I', body, 0)
        out = []
        for aid in struct.unpack_from('<%dI' % n, body, 4):
            akind, abody = self.objects.get(aid, (None, b''))
            if akind != 3:
                continue
            atype, target = struct.unpack_from('<HI', abody, 0)
            if atype & 0xFF00 not in (0x0400, 0x0500):    # Play, Play and continue
                continue
            for media in self._media_of(target):
                if media not in out:
                    out.append(media)
        return out

    def _media_of(self, oid, depth=0):
        """The embedded media of a sound, or of a container's children: a switch container (type 6) by its
        "Overseas" state where it has one (Tekken 7's language switch: Akuma's name is "Gouki" in Japanese), a
        random or sequence container (type 5) all of them."""
        kind, body = self.objects.get(oid, (None, b''))
        if kind == 2:
            _, stream, media, _ = struct.unpack_from('<IBII', body, 0)
            return [media] if media in self.media else []
        if kind not in (5, 6) or depth > 4:
            return []
        children = []
        if kind == 6:
            at = body.find(struct.pack('<I', fnv('Overseas')))
            if at >= 0 and at + 8 <= len(body):
                count, = struct.unpack_from('<I', body, at + 4)
                if count < 64 and at + 8 + 4 * count <= len(body):
                    children = list(struct.unpack_from('<%dI' % count, body, at + 8))
        if not children:                              # the children: the ids of objects in the body, in order
            for off in range(len(body) - 3):
                v, = struct.unpack_from('<I', body, off)
                if v != oid and v not in children and self.objects.get(v, (None,))[0] in (2, 5, 6):
                    children.append(v)
        out = []
        for c in children:
            out += self._media_of(c, depth + 1)
        return out


def decode_wem(w):
    """A WEM's audio -> (sample rate, int16 array [channels, samples])."""
    if w[:4] != b'RIFF' or w[8:12] != b'WAVE':
        raise ValueError('not a WEM file')
    c = _riff_chunks(w, 12)
    fp, _ = c[b'fmt ']
    tag, channels, rate, _, align, _ = struct.unpack_from('<HHIIHH', w, fp)
    dp, dsize = c[b'data']
    if tag != 2:
        raise ValueError('WEM codec %#x is not Wwise IMA ADPCM' % tag)
    block = align // channels
    per_block = (block - 4) * 2
    blocks = dsize // align
    out = np.zeros((channels, blocks * per_block), np.int16)
    steps, index_step = _IMA_STEPS, _IMA_INDEX
    for ch in range(channels):
        dst = out[ch]
        k = 0
        for b in range(blocks):
            o = dp + b * align + ch * block
            sample, index = struct.unpack_from('<hB', w, o)
            index = min(max(index, 0), 88)
            for byte in w[o + 4:o + block]:
                for code in (byte & 15, byte >> 4):
                    step = steps[index]
                    diff = step >> 3
                    if code & 1:
                        diff += step >> 2
                    if code & 2:
                        diff += step >> 1
                    if code & 4:
                        diff += step
                    sample = sample - diff if code & 8 else sample + diff
                    sample = -32768 if sample < -32768 else (32767 if sample > 32767 else sample)
                    index += index_step[code]
                    index = 0 if index < 0 else (88 if index > 88 else index)
                    dst[k] = sample
                    k += 1
    return rate, out
