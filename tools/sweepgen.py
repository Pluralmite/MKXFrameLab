#!/usr/bin/env python3
"""Build a MKXFrameLab bot script that performs every move of P1's character.

usage: sweepgen.py <MKXFrameLab_movelist.txt> <out_cmd.txt> [close|far|both] [only-hashes-file]

Each move:  mark sw|<pass>|<hash>|<label>  ->  practice reset  ->  (walk in)  ->  inputs  ->  wait until both are free.
The log is read back by sweepreport.py.
"""
import re, sys

BTN = {0x0F: 'X', 0x10: 'Y', 0x11: 'A', 0x12: 'B', 0x13: 'X+Y', 0x14: 'X+A', 0x15: 'X+B', 0x16: 'Y+A', 0x17: 'Y+B', 0x18: 'A+B',
       0x1A: 'X+A', 0x37: 'X+RT', 0x38: 'Y+RT', 0x39: 'A+RT', 0x3A: 'B+RT', 0x3B: 'LT+RT'}
DIR = {0: 5, 1: 6, 2: 4, 3: 8, 4: 2, 0x21: 1, 0x22: 3, 0x23: 7, 0x24: 9}
JUMP = {0x26: 9, 0x27: 7, 0x28: 8}
SKIP_CMD = re.compile(r'FATALITY|Fatality|Brutal|Debug|RunFwd|BlockEscape|ThrowFwdOut|ThrowBwdOut|Taunt', re.I)


def parse(path):
    cmds, moves, props = [], [], {}
    for ln in open(path, errors='replace'):
        ln = ln.rstrip('\n')
        m = re.match(r'^P1 cmd ([0-9A-F]{8}) (\S+)\s.*raw (\S*) flags ([0-9A-F]+) btn ([0-9A-F]+)', ln)
        if m:
            raw = [int(x, 16) for x in m.group(3).split(',') if x]
            cmds.append(dict(hash=m.group(1), name=m.group(2), raw=raw, btn=int(m.group(5), 16) & 0xFFFF))
            continue
        m = re.match(r'^P1 move g(\d) ([0-9A-F]{8}) .*raw (\S+)', ln)
        if m:
            raw = [int(x, 16) for x in m.group(3).split(',')]
            moves.append(dict(g=int(m.group(1)), hash=m.group(2), raw=[x for x in raw if x != 0xFFFFFFFF]))
            continue
        m = re.match(r'^P1 prop ([0-9A-F]{8}) (.*?)\s+start (-?\d+) end (-?\d+) miss (-?\d+)', ln)
        if m:
            s0, s1 = int(m.group(3)), int(m.group(4))
            props[m.group(1)] = dict(name=m.group(2).strip(), startup=s0 + 1 if s1 > 0 else 0, active=max(0, s1 - s0))
    return cmds, moves, props


def move_label(raw):
    out = []
    for mid in raw:
        g, d = mid >> 16, mid & 0xFFFF
        b = {0x0F: '1', 0x10: '2', 0x11: '3', 0x12: '4', 0x13: '1+2', 0x14: '1+3', 0x15: '1+4', 0x16: '2+3', 0x17: '2+4', 0x18: '3+4'}.get(g, '?%X' % g)
        dn = {0: '', 1: 'F', 2: 'B', 3: 'U', 4: 'D', 0x21: 'DB', 0x22: 'DF', 0x23: 'UB', 0x24: 'UF', 0x26: 'JF', 0x27: 'JB', 0x28: 'JU'}.get(d, '?')
        out.append(dn + b)
    return ','.join(out)


def normal_steps(mv, by_raw, props):
    raw = mv['raw']
    steps = []
    air = False
    for i, mid in enumerate(raw):
        g, d = mid >> 16, mid & 0xFFFF
        btn = BTN.get(g)
        if not btn:
            return None
        if i == 0 and d in JUMP:
            steps += ['pad 3 %d' % JUMP[d], 'wait 9']
            air = True
            d = 0
        elif i > 0:
            # press the next button while the previous hit is in its last active frames (strings buffer this)
            pre = by_raw.get(tuple(raw[:i]))
            p = props.get(pre) if pre else None
            gap = (p['startup'] + p['active'] - 2) if p and p['startup'] > 0 else 12
            if air:
                gap = max(gap, 8)
            steps.append('wait %d' % max(4, gap - 1))
            if d in JUMP:
                d = 0
        dd = DIR.get(d)
        if dd is None:
            return None
        steps.append('pad 1 %s' % (btn if dd == 5 else '%d+%s' % (dd, btn)))
    return steps


def special_steps(c):
    # mPadInfo may hold alternatives separated by SW_X (FFFFFFFF); the first one is the move-list input
    seq = []
    for v in c['raw']:
        if v == 0xFFFFFFFF:
            break
        seq.append(v & 0xFFFF)
    btn = BTN.get(c['btn'])
    if not btn:
        return None
    steps = []
    dirs = []
    for v in seq:
        if v in (0x40, 0x41, 0x42, 0x43):   # direction released
            dirs.append('rel')
        elif v in DIR:
            dirs.append(DIR[v])
        elif v == 0:
            continue
        else:
            return None
    pre = []
    if 'Air' in c['name']:
        pre = ['pad 3 8', 'wait 10']
    if not dirs:
        return pre + ['pad 2 %s' % btn]
    for d in dirs[:-1]:
        steps.append('wait 1' if d == 'rel' else 'pad 2 %d' % d)
    last = dirs[-1]
    if last == 'rel':
        steps.append('pad 2 %s' % btn)
    else:
        steps += ['pad 2 %d' % last, 'pad 2 %d+%s' % (last, btn)]
    return pre + steps


def build(path, mode='both', only=None):
    cmds, moves, props = parse(path)
    by_raw = {tuple(m['raw']): m['hash'] for m in moves}
    tests = []   # (hash, label, steps)
    seen = set()
    for m in moves:
        if m['g'] >= 4 or m['hash'] in seen or m['hash'] == '00000000':
            continue
        st = normal_steps(m, by_raw, props)
        if st is None:
            continue
        seen.add(m['hash'])
        tests.append((m['hash'], move_label(m['raw']), st))
    # specials: prefer the ground form, then the jump-up form of air specials
    cmds_sorted = sorted(cmds, key=lambda c: ('From' in c['name'] and 'FromJU' not in c['name'], 'Air' in c['name']))
    for c in cmds_sorted:
        if c['hash'] in seen or c['hash'] == '00000000' or SKIP_CMD.search(c['name']):
            continue
        if 'From' in c['name'] and 'FromJU' not in c['name']:
            continue
        st = special_steps(c)
        if st is None:
            continue
        seen.add(c['hash'])
        tests.append((c['hash'], c['name'], st))
    # throws (1+3), forward and back
    tests.append(('THROWF', 'Throw', ['pad 2 X+A']))
    tests.append(('THROWB', 'Back Throw', ['pad 2 4+X+A']))
    if only:
        tests = [t for t in tests if t[0] in only]
    out = ['rpad 150 START', 'rwait 1200', 'unfreeze', 'pdump', 'pgame 1 2', 'wait 2']
    passes = ['close', 'far'] if mode == 'both' else [mode]
    for ps in passes:
        for h, label, st in tests:
            out.append('mark sw|%s|%s|%s' % (ps, h, label))
            out += ['preset', 'wait 75']
            if ps == 'close':
                out.append('pad 26 6')
            out += st
            out += ['waitfree 420', 'wait 12', 'mark sw-end']
    out.append('mark sw-done')
    return out, tests


if __name__ == '__main__':
    only = None
    if len(sys.argv) > 4:
        only = set(open(sys.argv[4]).read().split())
    lines, tests = build(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 else 'both', only)
    open(sys.argv[2], 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('%d moves, %d script lines' % (len(tests), len(lines)), file=sys.stderr)
