"""Tile PNG screenshots into one contact sheet (stdlib only).
Usage: sheet.py out.png in1.png in2.png ...  (cols via SHEET_COLS, default 4)"""
import os, struct, sys, zlib


def readpng(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8]); c = d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        if t == b'IDAT':
            idat += c
        i += 12 + n
    bpp = {2: 3, 6: 4}[ct]; raw = zlib.decompress(idat); s = w * bpp
    rows = []; prev = bytearray(s); p = 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + s]); p += 1 + s
        for x in range(s):
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[i] for x in range(w) for i in range(x * bpp, x * bpp + 3)))
        prev = line
    return w, h, rows


def sheet(files, out, cols):
    ims = [readpng(f) for f in files]; w, h = ims[0][0], ims[0][1]
    r = (len(ims) + cols - 1) // cols; W, H = w * cols, h * r
    blank = b'\x40' * (w * 3); raw = bytearray()
    for y in range(H):
        raw += b'\x00'
        for c in range(cols):
            k = (y // h) * cols + c
            raw += ims[k][2][y % h] if k < len(ims) else blank
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                          + ch(b'IDAT', zlib.compress(bytes(raw))) + ch(b'IEND', b''))


if __name__ == '__main__':
    sheet(sys.argv[2:], sys.argv[1], int(os.environ.get('SHEET_COLS', 4)))
