"""Lists the part sets (robots) in the game data and the skeleton each one
carries: for every body / legs / head / arm / fist model of a set, the bone
names and positions. Used to build the `based_on` choice for mod robots.

  skeletons.py <game folder> <user data folder>       (text table)
  skeletons.py <game folder> <user data folder> --json out.json
"""
import glob, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'pac'))
import jboy, paclib

# part group -> (archive folder, file prefix) ; groups 1..3 live in the newest DLC package
GROUPS = {0: ('base', 'ed'), 1: ('dlc', 'DLC01_ed'), 2: ('dlc', 'DLC03_ed'), 3: ('dlc', 'DLC04_ed')}
SLOT_NAMES = ['body', 'legs', 'head', 'arm_l', 'arm_r', 'fist_l', 'fist_r']


def string_table(update_dir):
    text = paclib.load(os.path.join(update_dir, 'pac', 'string.pac'))
    for _, node in text.walk():
        if node.children is None and len(node.data) > 24 and struct.unpack_from('<I', node.data, 16)[0] == 10000:
            d = node.data
            count, = struct.unpack_from('<I', d, 4)
            out = {}
            for i in range(count):
                off, length, ident, _ = struct.unpack_from('<4I', d, 8 + i * 16)
                out[ident] = d[off:off + length].rstrip(b'\0').decode('latin1')
            return out
    raise SystemExit('no string table')


def sets(game, root):
    """Yields (group, part id, name, {slot: model path}) for every part set."""
    update = os.path.join(game, 'extracted', 'tu') if os.path.isdir(os.path.join(game, 'extracted', 'tu')) else os.path.join(game, 'update')
    base = os.path.join(game, 'extracted', 'game') if os.path.isdir(os.path.join(game, 'extracted', 'game')) else os.path.join(game, 'game')
    package = glob.glob(os.path.join(root, '0000000000000000', '584111E0', '00000002', '947F*'))
    strings = string_table(update)
    misc = paclib.load(os.path.join(update, 'pac', 'misc.pac'))
    for group, (where, prefix) in GROUPS.items():
        folder = os.path.join(base, 'pac', 'edit') if where == 'base' else (os.path.join(package[0], 'pac', 'edit') if package else None)
        if folder is None:
            continue
        tables = {slot: misc.find('/BATS/INIT/001D/%04X/%04X' % (group, slot)) for slot in range(1, 8)}
        body = tables[1].data
        count, = struct.unpack_from('<I', body, 0)
        for i in range(count):
            rec = body[8 + i * 60:8 + (i + 1) * 60]
            part_id = struct.unpack_from('<H', rec, 12)[0]
            name_id = struct.unpack_from('<I', rec, 52)[0]
            name = strings.get(name_id, '?')
            # the set is complete when every slot has a record with this id
            if not all(any(struct.unpack_from('<H', t.data, 8 + k * 60 + 12)[0] == part_id
                           for k in range(struct.unpack_from('<I', t.data, 0)[0])) for t in tables.values()):
                continue
            files = {SLOT_NAMES[n - 1]: os.path.join(folder, '%s%02d.pac' % (prefix, n)) for n in range(1, 8)}
            yield group, part_id, name, files


_ARCHIVES = {}


def skeleton_of(path, part_id, slot):
    """Bone names and positions of one part model, read without decoding
    the rest of the archive (epac directory + one PACH item)."""
    import epac, pach
    if path not in _ARCHIVES:
        _ARCHIVES[path] = epac.read(path)
    d, groups, _ = _ARCHIVES[path]
    for gname, entries in groups:
        if gname.strip() != 'ED%02d' % slot:
            continue
        for name, off, size in entries:
            if name.strip() == 'M%03X' % part_id:
                blob = d[off:off + size]
                first = next(pach.items(blob))[1] if blob[:4] == b'PACH' else blob
                model = jboy.Model(pach.decode(first))
                return [(bname, tuple(round(v, 3) for v in pos[:3])) for bname, pos, rot, rest in model.bones]
    return None


def main():
    game, root = sys.argv[1], sys.argv[2]
    out_json = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--json' else None
    rows = []
    for group, part_id, name, files in sets(game, root):
        robot = name.split(' ')[0] if ' ' in name else name
        bones = {}
        for slot, path in files.items():
            sk = skeleton_of(path, part_id, SLOT_NAMES.index(slot) + 1)
            if sk is None:
                bones = None
                break
            bones[slot] = sk
        if bones is None:
            continue
        legs = dict(bones['legs'])
        height = legs.get('bottom', (0, 0, 0))[0]
        thigh = legs.get('l_sune', (0, 0, 0))[0]
        shin = legs.get('l_ashi', (0, 0, 0))[0]
        body = dict(bones['body'])
        shoulder = body.get('l_sakotsu', (0, 0, 0))
        arm = dict(bones['arm_l'])
        forearm = arm.get('l_kote', (0, 0, 0))[0]
        rows.append({'group': group, 'id': part_id, 'name': name, 'robot': robot, 'legs': height, 'thigh': thigh,
                     'shin': shin, 'shoulder_out': shoulder[1], 'forearm': forearm, 'bones': bones})
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(rows, f, indent=1)
    print('%-5s %-5s %-26s %6s %6s %6s %8s %7s' % ('group', 'id', 'body part name', 'legs', 'thigh', 'shin', 'shoulder', 'forearm'))
    for r in rows:
        print('%-5d %-5X %-26s %6.2f %6.2f %6.2f %8.2f %7.2f' % (r['group'], r['id'], r['name'], r['legs'], r['thigh'], r['shin'], r['shoulder_out'], r['forearm']))
    print(len(rows), 'complete sets')


if __name__ == '__main__':
    main()
