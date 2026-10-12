#!/usr/bin/env python3
"""Prints the stats records of a stock part set, the numbers a mod robot
starts from (docs/modding.md, "Stats").

  show_stats.py <SET> [--game <game folder>] [--tu <title update folder>]
  show_stats.py --list

Known fields of the 98-field record: 0 sub_hp, 1 power_charge, 2 attack,
3 defense, 6 durability, 9 energy, 13 weight_capacity, 15 weight,
18 price (with 19). Everything else is printed as field<N> for
experiments.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pac'))
import paclib  # noqa: E402

NAMES = {0: 'sub_hp', 1: 'power_charge', 2: 'attack', 3: 'defense', 6: 'durability', 9: 'energy',
         13: 'weight_capacity', 15: 'weight', 18: 'price'}
SLOTS = {1: 'body', 2: 'legs', 3: 'head', 4: 'arm_l', 5: 'arm_r', 6: 'fist_l', 7: 'fist_r'}


def strings(text):
    out = {}
    for path, node in text.walk():
        if path.startswith('/SDB3/0200/') and node.children is None and len(node.data) > 24:
            d = node.data
            count, = struct.unpack_from('<I', d, 4)
            if count > 100000 or 8 + count * 16 > len(d):
                continue
            for i in range(count):
                off, length, ident, _ = struct.unpack_from('<4I', d, 8 + i * 16)
                out.setdefault(ident, d[off:off + length - 1].decode('utf-8', 'replace'))
    return out


def main():
    args = sys.argv[1:]
    root = os.path.normpath(os.path.join(HERE, '..', '..'))
    game, tu, want, list_sets = os.path.join(root, 'work', 'game'), os.path.join(root, 'work', 'tu'), None, False
    while args:
        a = args.pop(0)
        if a == '--game':
            game = args.pop(0)
        elif a == '--tu':
            tu = args.pop(0)
        elif a == '--list':
            list_sets = True
        else:
            want = a.upper()
    misc_path = os.path.join(tu if os.path.isfile(os.path.join(tu, 'pac', 'misc.pac')) else game, 'pac', 'misc.pac')
    text_path = os.path.join(tu if os.path.isfile(os.path.join(tu, 'pac', 'string.pac')) else game, 'pac', 'string.pac')
    misc, text = paclib.load(misc_path), paclib.load(text_path)
    names = strings(text)
    tables = {p: n for p, n in misc.walk() if n.children is None}
    sets = {}
    for slot in range(1, 8):
        menu = tables.get('/BATS/INIT/001D/0000/%04X' % slot)
        stats = tables.get('/BATS/INIT/001E/0000/%04X' % slot)
        if menu is None or stats is None:
            continue
        count, = struct.unpack_from('<H', stats.data, 4)
        size = (len(stats.data) - 8) // count
        for i in range(count):
            m = menu.data[8 + i * 60:8 + (i + 1) * 60]
            name = names.get(struct.unpack_from('<I', m, 52)[0], '?')
            rec = stats.data[8 + i * size:8 + (i + 1) * size]
            set_name = name.rsplit(' ', 2)[0] if ' Lv.' in name else name
            sets.setdefault(set_name.upper(), []).append((slot, name, rec))
    if list_sets or want is None:
        print('\n'.join(sorted(sets)))
        return 0
    if want not in sets:
        print('no set called %s (try --list)' % want)
        return 1
    for slot, name, rec in sorted(sets[want]):
        fields = struct.unpack_from('<98H', rec, 0)
        known = ['%s=%d' % (NAMES[i], fields[i] if i != 18 else fields[18] | (fields[19] << 16)) for i in sorted(NAMES)]
        others = ['field%d=%d' % (i, v) for i, v in enumerate(fields) if v and i not in NAMES and i != 19]
        print('[%s] %s' % (SLOTS[slot], name))
        print('   ' + '  '.join(known))
        print('   ' + '  '.join(others))
    return 0


if __name__ == '__main__':
    sys.exit(main())
