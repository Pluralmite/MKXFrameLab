#!/usr/bin/env python3
"""Read MKXFrameLab.log after a sweep and summarise every move.

usage: sweepreport.py <MKXFrameLab.log> [out.json]

Per move segment (mark sw|pass|hash|label ... mark sw-end):
  * box checks: every strike / collision-object hit test the game ran, with the gap between the outline we draw
    and the defender's hurt spheres; disagreement = the game says HIT but our shapes are apart (or the reverse)
  * frame data: measured vs the move list (MISMATCH flag from the mod), and the advantage result
"""
import json, re, sys

TOL = 1.0   # world units of slack for float rounding between our math and the game's

def main(path, out=None):
    segs, cur = [], None
    for ln in open(path, errors='replace'):
        body = ln[15:].rstrip('\n') if ln.startswith('[') else ln.rstrip('\n')
        m = re.match(r'mark: ?sw\|(\w+)\|(\w+)\|(.*)$', body)
        if m:
            cur = dict(pass_=m.group(1), hash=m.group(2), label=m.group(3), moves=[], strike=[], shape=[], adv=[], ok_reset=True)
            segs.append(cur)
            continue
        if cur is None:
            continue
        if body.startswith('mark: sw-end') or body.startswith('mark:sw-end'):
            cur = None
            continue
        m = re.match(r'move: P1 id ([0-9A-F]+) startup (-?\d+) active (-?\d+) recovery (-?\d+) total (-?\d+)(.*)$', body)
        if m:
            rest = m.group(6)
            lm = re.search(r'\| list (.*?) (-?\d+)/(-?\d+)/(-?\d+) blk (-?\d+) hit (-?\d+) cancel (-?\d+)', rest)
            cur['moves'].append(dict(id=m.group(1).zfill(8), startup=int(m.group(2)), active=int(m.group(3)), recovery=int(m.group(4)),
                                     hit='(hit)' in rest, chained='(chained)' in rest, mismatch='MISMATCH' in rest,
                                     list=dict(name=lm.group(1), startup=int(lm.group(2)), active=int(lm.group(3)), recovery=int(lm.group(4)),
                                               blk=int(lm.group(5)), hit=int(lm.group(6))) if lm else None))
            continue
        m = re.match(r'strikechk: P(\d) f(\d+) gap (-?[\d.]+) .*game (HIT|miss)', body)
        if m:
            cur['strike'].append(dict(p=int(m.group(1)), f=int(m.group(2)), gap=float(m.group(3)), hit=m.group(4) == 'HIT'))
            continue
        m = re.match(r'shapechk: col (\w+) type (\d+) gap (-?[\d.e+]+) +game (HIT|miss)', body)
        if m:
            cur['shape'].append(dict(col=m.group(1), kind=int(m.group(2)), gap=float(m.group(3)), hit=m.group(4) == 'HIT'))
            continue
        m = re.match(r'adv: P1 (BLOCK|HIT)(.*?) ([+-]\d+) ', body)
        if m:
            cur['adv'].append(dict(kind=m.group(1), extra=m.group(2).strip(), adv=int(m.group(3))))
            continue
        if 'practice: reset unavailable' in body or 'bot: @preset failed' in body:
            cur['ok_reset'] = False
    res = []
    for s in segs:
        p1s = [c for c in s['strike'] if c['p'] == 1]
        bad = [c for c in p1s if (c['hit'] and c['gap'] > TOL) or (not c['hit'] and c['gap'] < -TOL)]
        sbad = [c for c in s['shape'] if c['gap'] < 1e8 and ((c['hit'] and c['gap'] > TOL) or (not c['hit'] and c['gap'] < -TOL))]
        own = [m for m in s['moves'] if m['id'] == s['hash']]
        performed = bool(own) or (s['hash'].startswith('THROW') and bool(s['moves']))
        r = dict(pass_=s['pass_'], hash=s['hash'], label=s['label'], performed=performed,
                 ids=[m['id'] for m in s['moves']],
                 strike_tests=len(p1s), strike_hits=sum(c['hit'] for c in p1s), strike_bad=len(bad),
                 strike_bad_detail=[(c['f'], c['gap'], c['hit']) for c in bad][:6],
                 shape_tests=len(s['shape']), shape_hits=sum(c['hit'] for c in s['shape']), shape_bad=len(sbad),
                 shape_bad_detail=[(c['kind'], c['gap'], c['hit']) for c in sbad][:6],
                 moves=s['moves'], adv=s['adv'], reset=s['ok_reset'])
        res.append(r)
    if out:
        json.dump(res, open(out, 'w'), indent=1)
    # console summary
    tot = dict(n=len(res), perf=sum(r['performed'] for r in res), st=sum(r['strike_tests'] for r in res), sb=sum(r['strike_bad'] for r in res),
               sh=sum(r['shape_tests'] for r in res), shb=sum(r['shape_bad'] for r in res))
    print('segments %(n)d performed %(perf)d | strike tests %(st)d disagree %(sb)d | shape tests %(sh)d disagree %(shb)d' % tot)
    for r in res:
        flag = []
        if not r['performed']: flag.append('NOT PERFORMED')
        if r['strike_bad']: flag.append('STRIKE x%d %s' % (r['strike_bad'], r['strike_bad_detail']))
        if r['shape_bad']: flag.append('SHAPE x%d %s' % (r['shape_bad'], r['shape_bad_detail']))
        mm = [m for m in r['moves'] if m['mismatch']]
        if mm: flag.append('FRAMES ' + '; '.join('%s %d/%d/%d vs %s %d/%d/%d' % (m['id'], m['startup'] - 1, m['active'], m['recovery'] + 1,
                                                                                m['list']['name'], m['list']['startup'], m['list']['active'], m['list']['recovery']) for m in mm))
        print('%-5s %-8s %-28s tests %3d/%-3d hits %3d %s' % (r['pass_'], r['hash'], r['label'][:28], r['strike_tests'], r['shape_tests'],
                                                        r['strike_hits'] + r['shape_hits'], ' | '.join(flag)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
