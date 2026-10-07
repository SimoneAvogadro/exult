# Memory-budget statistics for hi-res (S x) override frames.
# Usage: python3 memstat.py  (paths hard-coded below)
import struct, sys, os, json, math, collections

S = 6
S2 = S * S
GAMES = {
    'BG': '/mnt/e/Games/RolePlayingGames/ultima7',
    'SI': '/mnt/e/Games/RolePlayingGames/Serpent',
}


def flex_entries(d):
    n = struct.unpack_from('<I', d, 0x54)[0]
    return [struct.unpack_from('<II', d, 0x80 + 8 * i) for i in range(n)]


class FrameStat:
    __slots__ = ('kind', 'w', 'h', 'opaque', 'hspans', 'vspans', 'datalen', 'nn_rle')

    def __init__(self, kind, w, h, opaque, hspans, vspans, datalen, nn_rle):
        self.kind, self.w, self.h, self.opaque = kind, w, h, opaque
        self.hspans, self.vspans, self.datalen, self.nn_rle = hspans, vspans, datalen, nn_rle

    # 8-bit, whole bounding box at S x (an uncompressed indexed bitmap).
    def bbox_bytes(self):
        return self.w * self.h * S2

    # Exult RLE of noisy (AI) art at S x: every opaque pixel stored raw, one 6-byte
    # scanline header per S x row-span; lower bound (count bytes ignored).
    def rle_noisy(self, reflected=False):
        if self.kind == 'flat':
            return 64 * S2
        spans = self.vspans if reflected else self.hspans
        return self.opaque * S2 + spans * S * 6 + 2

    # RLE of a nearest-neighbour S x upscale (what a cached fallback costs).
    def rle_nn(self):
        if self.kind == 'flat':
            return 64 * S2
        return self.nn_rle

    def orig_bytes(self):
        return self.datalen


def nn_span_cost(pixels):
    # pixels: list of palette indices of one S=1 opaque span.
    # After NN S x upscale every run of equal colour r becomes a repeat run of S*r,
    # encoded in chunks of <=127 as 2 bytes each. Plus a 6-byte header per row.
    cost = 6
    i = 0
    n = len(pixels)
    while i < n:
        j = i + 1
        while j < n and pixels[j] == pixels[i]:
            j += 1
        r = (j - i) * S
        cost += 2 * ((r + 126) // 127)
        i = j
    return cost * S  # S rows


def parse_vga(path):
    d = open(path, 'rb').read()
    ents = flex_entries(d)
    frames = {}
    nframes = {}
    for s, (off, ln) in enumerate(ents):
        if ln == 0 or off == 0:
            continue
        dlen = struct.unpack_from('<I', d, off)[0]
        if not (dlen == ln or (ln % 2 == 0 and dlen == ln - 1)):
            n = ln // 64
            nframes[s] = n
            for f in range(n):
                frames[(s, f)] = FrameStat('flat', 8, 8, 64, 8, 8, 64, 64 * S2)
            continue
        hdr = struct.unpack_from('<I', d, off + 4)[0]
        nf = (hdr - 4) // 4
        nframes[s] = nf
        offs = [struct.unpack_from('<I', d, off + 4 + 4 * f)[0] for f in range(nf)]
        for f in range(nf):
            fo = off + offs[f]
            flen = (offs[f + 1] if f + 1 < nf else dlen) - offs[f]
            xr, xl, ya, yb = struct.unpack_from('<hhhh', d, fo)
            w = xl + xr + 1
            h = ya + yb + 1
            p = fo + 8
            opaque = 0
            hspans = 0
            nn = 2
            cols = collections.defaultdict(list)  # x -> list of y (for vertical spans)
            while True:
                sl = struct.unpack_from('<H', d, p)[0]
                p += 2
                if sl == 0:
                    break
                enc = sl & 1
                L = sl >> 1
                sx, sy = struct.unpack_from('<hh', d, p)
                p += 4
                hspans += 1
                if not enc:
                    px = list(d[p:p + L])
                    p += L
                else:
                    px = []
                    rem = L
                    while rem > 0:
                        b = d[p]
                        p += 1
                        c = b >> 1
                        if b & 1:
                            px.extend([d[p]] * c)
                            p += 1
                        else:
                            px.extend(d[p:p + c])
                            p += c
                        rem -= c
                opaque += len(px)
                nn += nn_span_cost(px)
                for k in range(len(px)):
                    cols[sx + k].append(sy)
            vspans = 0
            for x, ys in cols.items():
                ys.sort()
                prev = None
                for y in ys:
                    if prev is None or y != prev + 1:
                        vspans += 1
                    prev = y
            frames[(s, f)] = FrameStat('rle', w, h, opaque, hspans, vspans, flen - 8, nn)
    return frames, nframes


def summarize(name, frames, nframes):
    rle = [fs for fs in frames.values() if fs.kind == 'rle']
    flats = [fs for fs in frames.values() if fs.kind == 'flat']
    out = collections.OrderedDict()
    out['file'] = name
    out['shapes'] = len(nframes)
    out['rle_frames'] = len(rle)
    out['flat_frames'] = len(flats)
    out['orig_mem_bytes'] = sum(fs.orig_bytes() for fs in frames.values())
    out['bbox_px'] = sum(fs.w * fs.h for fs in rle)
    out['opaque_px'] = sum(fs.opaque for fs in rle)
    out['hspans'] = sum(fs.hspans for fs in rle)
    out['S6_bbox8_MB'] = (sum(fs.bbox_bytes() for fs in rle) + len(flats) * 64 * S2) / 1e6
    out['S6_rle_noisy_MB'] = sum(fs.rle_noisy() for fs in frames.values()) / 1e6
    out['S6_rle_nn_MB'] = sum(fs.rle_nn() for fs in frames.values()) / 1e6
    out['S6_rgba_bbox_MB'] = out['S6_bbox8_MB'] * 4
    if rle:
        big = sorted(((fs.w * fs.h, k, fs) for k, fs in frames.items() if fs.kind == 'rle'), key=lambda t: -t[0])
        out['largest'] = [(k[0], k[1], fs.w, fs.h, fs.w * S, fs.h * S, round(fs.rle_noisy() / 1e6, 2)) for _, k, fs in big[:8]]
        hist = collections.Counter()
        for fs in rle:
            b = fs.rle_noisy()
            if b < 64e3:
                hist['<64KB'] += 1
            elif b < 256e3:
                hist['64-256KB'] += 1
            elif b < 1e6:
                hist['256KB-1MB'] += 1
            elif b < 2e6:
                hist['1-2MB'] += 1
            else:
                hist['>=2MB'] += 1
        out['size_hist_S6_noisy'] = dict(hist)
        out['max_w'] = max(fs.w for fs in rle)
        out['max_h'] = max(fs.h for fs in rle)
        out['frames_wider_than_33px'] = sum(1 for fs in rle if fs.w * S > 200)
    # frame-0 floor: Shape_manager::read_shape_info() forces frame 0 of every shape
    f0 = sum(frames[(s, 0)].rle_noisy() for s in nframes if (s, 0) in frames)
    out['S6_frame0_all_shapes_MB'] = f0 / 1e6
    # reflection worst case (every frame of every RLE shape with <=32 frames reflected)
    refl = sum(fs.rle_noisy(reflected=True) for (s, f), fs in frames.items() if fs.kind == 'rle' and nframes.get(s, 99) <= 32)
    out['S6_reflections_worstcase_MB'] = refl / 1e6
    return out


def read_tfa(path, n):
    try:
        d = open(path, 'rb').read()
    except OSError:
        return {}
    return {s: d[3 * s + 1] & 15 for s in range(min(n, len(d) // 3))}


def ireg_keys(path, game):
    """Top-level (map) objects of a u7iregXX file -> {(cx,cy): set((shape,frame))}"""
    res = collections.defaultdict(set)
    try:
        d = open(path, 'rb').read()
    except OSError:
        return res
    p = 0
    depth = 0
    n = len(d)
    while p < n:
        entlen = d[p]
        p += 1
        if entlen in (0, 1):
            if depth > 0:
                depth -= 1
            continue
        if entlen == 2:
            p += 2
            continue
        if entlen == 255:
            p += 1
            ln = struct.unpack_from('<H', d, p)[0]
            p += 2 + ln
            continue
        extended = 0
        if entlen in (254, 253):
            extended = 1 if entlen == 254 else 0
            entlen = d[p]
            p += 1
        testlen = entlen - extended
        e = d[p:p + entlen]
        p += entlen
        if testlen not in (6, 10, 12, 13, 14, 18) or len(e) < 5:
            continue
        if extended:
            shnum = e[2] + 256 * e[3]
            frnum = e[4]
        else:
            shnum = e[2] + 256 * (e[3] & 3)
            frnum = e[3] >> 2
        if depth == 0:
            cx = e[0] >> 4
            cy = e[1] >> 4
            res[(cx, cy)].add((shnum, frnum))
        if testlen in (12, 13) and (e[4] | e[5]) != 0 and not (game == 'BG' and shnum == 330):
            depth += 1
    return res


def world_working_set(game, root, frames, nframes):
    st = root + '/static/'
    m = open(st + 'u7map', 'rb').read()
    ch = open(st + 'u7chunks', 'rb').read()
    v2 = ch[:4] == b'exlt'
    ntempl = len(ch) // 512
    # template -> set of keys
    templ = []
    for t in range(ntempl):
        ks = set()
        b = ch[512 * t:512 * (t + 1)]
        for i in range(256):
            shn = b[2 * i] + 256 * (b[2 * i + 1] & 3)
            frn = (b[2 * i + 1] >> 2) & 0x1f
            ks.add((shn, frn))
        templ.append(frozenset(ks))
    world = [[None] * 192 for _ in range(192)]
    for sy in range(12):
        for sx in range(12):
            for cy in range(16):
                for cx in range(16):
                    idx = struct.unpack_from('<H', m, 2 * (((sy * 12 + sx) * 16 + cy) * 16 + cx))[0]
                    world[sy * 16 + cy][sx * 16 + cx] = set(templ[idx] if idx < ntempl else ())
    # ifix
    for sc in range(144):
        fn = st + 'u7ifix%02x' % sc
        try:
            d = open(fn, 'rb').read()
        except OSError:
            continue
        ents = flex_entries(d)
        sy, sx = sc // 12, sc % 12
        for ci, (off, ln) in enumerate(ents[:256]):
            if not ln:
                continue
            cy, cx = ci // 16, ci % 16
            tgt = world[sy * 16 + cy][sx * 16 + cx]
            for i in range(ln // 4):
                e = d[off + 4 * i:off + 4 * i + 4]
                tgt.add((e[2] + 256 * (e[3] & 3), e[3] >> 2))
    # ireg (initial/saved state in gamedat)
    gd = root + '/gamedat/'
    nireg = 0
    for sc in range(144):
        r = ireg_keys(gd + 'u7ireg%02x' % sc, game)
        if r:
            nireg += 1
        sy, sx = sc // 12, sc % 12
        for (cx, cy), ks in r.items():
            world[sy * 16 + cy][sx * 16 + cx] |= ks

    cost_cache = {}

    def cost(key, model):
        ck = (key, model)
        if ck in cost_cache:
            return cost_cache[ck]
        s, f = key
        fs = frames.get(key)
        c = 0
        if fs is None and (f & 32) and nframes.get(s, 0) <= 32:
            base = frames.get((s, f & 31))
            if base is not None:
                # base frame is loaded too (Shape::reflect calls get()).
                c = (base.rle_noisy() + base.rle_noisy(True)) if model == 'noisy' else 2 * base.bbox_bytes()
        elif fs is None and nframes.get(s) and frames.get((s, f & 31)) and frames[(s, f & 31)].kind == 'flat':
            c = 64 * S2
        elif fs is not None:
            c = fs.rle_noisy() if model == 'noisy' else (fs.bbox_bytes() if fs.kind == 'rle' else 64 * S2)
        cost_cache[ck] = c
        return c

    def window_stats(wc, hc, step):
        vals = []
        nkeys = []
        for y in range(0, 192 - hc + 1, step):
            for x in range(0, 192 - wc + 1, step):
                u = set()
                for yy in range(y, y + hc):
                    row = world[yy]
                    for xx in range(x, x + wc):
                        u |= row[xx]
                vals.append(sum(cost(k, 'noisy') for k in u))
                nkeys.append(len(u))
        vals.sort()
        nkeys.sort()
        q = lambda a, p: a[min(len(a) - 1, int(p * len(a)))]
        return {'windows': len(vals), 'p50_MB': q(vals, .5) / 1e6, 'p95_MB': q(vals, .95) / 1e6, 'max_MB': vals[-1] / 1e6,
                'keys_p50': q(nkeys, .5), 'keys_max': nkeys[-1]}

    allkeys = set()
    for row in world:
        for c in row:
            allkeys |= c
    res = collections.OrderedDict()
    res['u7chunks_v2'] = v2
    res['ireg_files_read'] = nireg
    res['distinct_frames_on_map'] = len(allkeys)
    res['distinct_frames_on_map_S6_noisy_MB'] = sum(cost(k, 'noisy') for k in allkeys) / 1e6
    res['reflected_keys_on_map'] = sum(1 for (s, f) in allkeys if (f & 32) and nframes.get(s, 0) <= 32)
    res['win_5x4_chunks'] = window_stats(5, 4, 1)
    res['win_10x10_chunks'] = window_stats(10, 10, 2)
    res['win_16x16_chunks'] = window_stats(16, 16, 4)
    return res


def actor_costs(game, root, frames, nframes):
    tfa = read_tfa(root + '/static/tfa.dat', max(s for s, _ in frames) + 1)
    per = []
    for s, cls in tfa.items():
        if cls in (12, 13) and s in nframes:
            c = 0
            for f in range(nframes[s]):
                fs = frames.get((s, f))
                if fs is None:
                    continue
                c += fs.rle_noisy()
                if nframes[s] <= 32 and fs.kind == 'rle':
                    c += fs.rle_noisy(True)  # actors face E/W via reflection
            per.append((c, s))
    per.sort()
    if not per:
        return {}
    return {'actor_shapes': len(per), 'median_MB': per[len(per) // 2][0] / 1e6, 'max_MB': per[-1][0] / 1e6, 'max_shape': per[-1][1],
            'all_actor_shapes_MB': sum(c for c, _ in per) / 1e6}


if __name__ == '__main__':
    report = collections.OrderedDict()
    for game, root in GAMES.items():
        st = root + '/static/'
        g = collections.OrderedDict()
        for fn in ['shapes.vga', 'faces.vga', 'gumps.vga', 'sprites.vga', 'paperdol.vga', 'fonts.vga']:
            if not os.path.exists(st + fn):
                continue
            fr, nf = parse_vga(st + fn)
            g[fn] = summarize(fn, fr, nf)
            if fn == 'shapes.vga':
                g['actors'] = actor_costs(game, root, fr, nf)
                g['world'] = world_working_set(game, root, fr, nf)
            print(game, fn, 'done', file=sys.stderr)
        report[game] = g
    json.dump(report, open(os.path.join(os.path.dirname(__file__), 'memstat.json'), 'w'), indent=1)
    print(json.dumps(report, indent=1))
