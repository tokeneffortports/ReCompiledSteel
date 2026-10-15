"""Checks a robot folder (OBJ models + PNG textures, see docs/modding.md):
the game itself makes the folder's models into game files when it starts,
from the player's own copy of the base set, so a robot is shared as the
folder - nothing to build, no game data inside.

  make_mod.py <robot folder> [--root <user data folder>] [--game <game folder>] [--built]
  make_mod.py --new <robot folder> [--name NAME] [--based-on SET] [--root ...] [--game ...]

--built also writes the game files to <robot folder>/built/ the way the
game makes them (for testing the game's builder against this reference;
the game ignores built/ when the folder has its models, and built/ holds
data from the game, so never share it).

--new starts a robot: a folder with a robot.txt and a simple blocky robot
(seven OBJ models, PNG textures) fitted to the chosen set's skeleton, ready
to reshape and repaint (the rules are in docs/modding.md).

The skeleton, shader settings and stats come from the stock part set named
by `based_on` in robot.txt (default BASENJA; the choices are in
docs/modding.md). Its files are read from the game folder (base-game sets) or
the official DLC in the user data folder (Documents\\realsteel on Windows,
~/.local/share/realsteel on Linux / Steam Deck) and cached in
<robot folder>/../.templates/<set>/. Needs Python 3 with numpy.
"""
import glob, os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'pac'))
import paclib, skeletons


def default_root():
    if sys.platform.startswith('win'):
        return os.path.join(os.path.expanduser('~'), 'Documents', 'realsteel')
    return os.path.join(os.path.expanduser('~'), '.local', 'share', 'realsteel')


def extract_templates(game, root, base, out_dir):
    """The base set's part files (models, joint records, textures) as
    ed<slot>_<kind>_<item>.bin in out_dir."""
    if os.path.isfile(os.path.join(out_dir, 'ed07_T_0646.bin')):
        return out_dir
    chosen = None
    for group, part_id, name, files in skeletons.sets(game, root):
        if name.split(' ')[0].upper() == base:
            chosen = (part_id, files)
            break
    if chosen is None:
        raise SystemExit('no part set called %s - see docs/modding.md for the choices (and check --root / --game)' % base)
    part_id, files = chosen
    os.makedirs(out_dir, exist_ok=True)
    for n, slot in enumerate(skeletons.SLOT_NAMES, 1):
        arc = paclib.load(files[slot])
        for kind in 'MT':
            node = next(x for x in arc.group('ED%02d' % n) if x.name == '%s%03X' % (kind, part_id))
            for child in node.children:
                with open(os.path.join(out_dir, 'ed%02d_%s_%s.bin' % (n, kind, child.name)), 'wb') as f:
                    f.write(child.data)
    return out_dir


def read_robot_txt(path):
    values = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if '=' in line and not line.strip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip().lower()] = value.strip()
    return values


def main():
    args = sys.argv[1:]
    root = default_root()
    game = os.path.normpath(os.path.join(HERE, '..', '..'))
    folder = None
    new = False
    write_built = False
    name, based_on = None, 'BASENJA'
    while args:
        a = args.pop(0)
        if a == '--root' and args:
            root = args.pop(0)
        elif a == '--game' and args:
            game = args.pop(0)
        elif a == '--new':
            new = True
        elif a == '--built':
            write_built = True
        elif a == '--name' and args:
            name = args.pop(0)
        elif a == '--based-on' and args:
            based_on = args.pop(0).upper()
        else:
            folder = a
    if new and folder:
        try:
            import numpy  # noqa: F401
        except ImportError:
            raise SystemExit('numpy is needed:  pip install numpy')
        import robot_folder
        if os.path.exists(os.path.join(folder, 'robot.txt')):
            raise SystemExit('%s already has a robot.txt' % folder)
        name = (name or os.path.basename(os.path.normpath(folder))).upper()[:14]
        templates = extract_templates(game, root, based_on, os.path.join(os.path.dirname(os.path.abspath(folder)), '.templates', based_on))
        robot_folder.export(templates, folder)
        with open(os.path.join(folder, 'robot.txt'), 'w', newline='\n') as f:
            f.write('name = %s\nbased_on = %s\n' % (name, based_on))
        print('started %s (%s, based on %s): reshape and repaint it, then run make_mod.py on it' % (folder, name, based_on))
        return
    if not folder or not os.path.isfile(os.path.join(folder, 'robot.txt')):
        raise SystemExit(__doc__ + '\n(the folder needs a robot.txt with  name = YOURROBOT)')
    try:
        import numpy  # noqa: F401
    except ImportError:
        raise SystemExit('numpy is needed:  pip install numpy')
    import robot_folder
    base = read_robot_txt(os.path.join(folder, 'robot.txt')).get('based_on', 'BASENJA').upper() or 'BASENJA'
    templates = extract_templates(game, root, base, os.path.join(os.path.dirname(os.path.abspath(folder)), '.templates', base))
    print('based on the %s set' % base)
    _, problems = robot_folder.load_folder(templates, folder)
    if problems:
        raise SystemExit('fix these first:\n  ' + '\n  '.join(problems))
    built = os.path.join(folder, 'built')
    if os.path.isdir(built):
        shutil.rmtree(built)
    if write_built:
        robot_folder.build(templates, folder, built)
        print('wrote %s (for testing only - never share it, it holds data from the game)' % built)
    print('done: %s is ready - copy the folder into the game\'s mods folder; the game builds the robot when it starts'
          % folder)


if __name__ == '__main__':
    main()
