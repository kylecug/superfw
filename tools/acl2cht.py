#!/usr/bin/env python3
"""Convert open_agb_firm (oaf-boost) gba.acl into SuperFW .cht files (CodeBreaker).

usage: acl2cht.py gba.acl outdir
"""
import os, struct, sys, collections

def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]

def parse(d):
    assert d[:3] == b'ACL'
    n, maxl = struct.unpack_from('<HH', d, 4)
    sers = [d[8+3*i:11+3*i].decode('ascii') for i in range(n)]
    idx_base = 8 + 3*n
    idx = struct.unpack_from('<%dH' % (n+1), d, idx_base)
    cfg_base = idx_base + 2*(n+1)
    out = []
    for i, s in enumerate(sers):
        for c in range(idx[i], idx[i+1]):
            typeloc, cfgid = struct.unpack_from('<II', d, cfg_base + 8*c)
            region = chr(typeloc & 0xff)
            off = (typeloc & ~0xff) >> 3
            out.append((s, region, cfgid, off))
    return out

def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('utf-8', 'replace')

def parse_set(d, base):
    entc = u16(d, base)
    ents = []
    for i in range(entc):
        ents.append(struct.unpack_from('<III', d, base + 2 + 12*i))   # entid, offset, datasz
    strbase = base + 2 + 12*entc
    emap = {e[0]: e for e in ents}
    def s(off): return cstr(d, strbase + off)
    root = emap[0]
    holes = []
    for k in range(root[2]):
        hname = s(u16(d, base + root[1] + 2*k))
        he = emap.get((k+1) | 0)
        keys = []
        def blk(j):
            ke = emap.get((k+1) | (j << 16))
            if not ke: return []
            vals = struct.unpack_from('<%dI' % ke[2], d, base + ke[1])
            return [(vals[a], vals[a+1] >> 8, vals[a+1] & 0xff) for a in range(0, len(vals), 2)]
        if he and he[2] > 0:
            for j in range(he[2]):
                keys.append((s(u16(d, base + he[1] + 2*j)), blk(j+1)))
        else:
            keys.append((None, blk(1)))     # single unnamed option == ON
        holes.append((hname, keys))
    return holes

def blocks_to_cb(blocks):
    """(addr,count,val) byte-fill blocks -> list of CodeBreaker 'AAAAAAAA+VVVV' strings."""
    # expand to a byte map, then re-pack into 16-bit writes where aligned/adjacent.
    mem = {}
    for a, cnt, v in blocks:
        for i in range(cnt):
            mem[a+i] = v
    codes = []
    for a in sorted(mem):
        pass
    done = set()
    for a in sorted(mem):
        if a in done: continue
        if a % 2 == 0 and (a+1) in mem:
            codes.append('8%07X+%04X' % (a & 0xFFFFFFF, mem[a] | (mem[a+1] << 8)))
            done.update((a, a+1))
        else:
            codes.append('3%07X+%04X' % (a & 0xFFFFFFF, mem[a]))
            done.add(a)
    return codes

MAXLINE = 36   # 14 chars per code -> 504 bytes, under SuperFW 512 byte line limit

def main():
    acl, outdir = sys.argv[1], sys.argv[2]
    d = open(acl, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    cfgs = parse(d)
    stats = collections.Counter()
    seen = collections.defaultdict(list)
    for s, region, cfgid, off in cfgs:
        code4 = s + region
        seen[code4].append((cfgid, off))
    for code4, lst in sorted(seen.items()):
        for n, (cfgid, off) in enumerate(lst):
            holes = parse_set(d, off)
            lines = []
            for hname, keys in holes:
                for kname, blocks in keys:
                    if not blocks: continue
                    codes = blocks_to_cb(blocks)
                    if len(codes) > MAXLINE:
                        stats['oversize'] += 1
                        stats['split_lines'] += (len(codes) + MAXLINE - 1)//MAXLINE
                    for p in range(0, len(codes), MAXLINE):
                        title = hname if kname is None else '%s: %s' % (hname, kname)
                        if len(codes) > MAXLINE:
                            title += ' (%d)' % (p//MAXLINE + 1)
                        lines.append((title, ' '.join(codes[p:p+MAXLINE])))
            if not lines: continue
            fn = code4 + ('.cht' if n == 0 else '~%d.cht' % (n+1))
            with open(os.path.join(outdir, fn), 'w', encoding='utf-8', newline='\n') as f:
                for t, c in lines:
                    f.write(t + '\n' + c + '\n')
            stats['files'] += 1
            stats['entries'] += len(lines)
            if n > 0: stats['extra_sets'] += 1
    print(dict(stats), 'game codes:', len(seen))

main()
