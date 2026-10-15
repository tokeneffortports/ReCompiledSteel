# Robot mods

The game loads extra robots from a folder called **`mods`** next to the game
executable, one sub-folder per robot. It installs them by itself each time it
starts. You don't run any tools, edit any files or rebuild anything.

```
ReCompiledSteel\
  realsteel.exe        (realsteel on Linux / Steam Deck)
  mods\
    MyRobot\           <- one folder per robot
```

## Installing a robot

1. Unzip the robot into the game's `mods` folder, so that the robot's folder
   is directly inside `mods` (`mods\MyRobot\robot.txt` must exist).
2. Start the game. The first start after a change takes a few seconds
   longer while the game builds and installs the robot.
3. **Assembly Mode → your robot → Customize**: the new parts are in each list
   (Head, Body, Arms, Fists, Legs), named after the robot. Buy them in the
   shop if they are not yours yet, equip them and save.

- The official add-ons must be installed (mods are added on top of the newest
  add-on pack). A robot that cannot be loaded - no add-ons, a base set
  (`based_on`) the game does not know or whose add-on files are missing, a
  model with a problem - is left out, and the game says which and why in a
  message at the bottom of the screen after it starts (and in its log).
- To remove a robot, delete its folder. A saved robot still wearing its parts
  shows plain white stand-in parts until you equip something else. When no
  saved robot uses a removed mod any more, the game forgets it.
- Online, players only see each other's matches when they have the same mods
  installed. The MULTIPLAYER panel lists which mods are missing on which side.
- To update a mod, overwrite its files. The game notices the change and
  reinstalls it. Keep the `name` in `robot.txt` the same: a renamed robot is a
  new robot.

## Making a robot

You need the game installed from your own copy, this repository, and
Python 3 with numpy (`pip install numpy`).

```bash
python tools/mods/make_mod.py --new mods/MyRobot --name "MY ROBOT" --based-on KOGA
```

This starts a robot: a `robot.txt` and a simple blocky robot fitted to the
chosen skeleton (seven OBJ models with PNG textures). Reshape and repaint it
in any 3D tool and paint program, then check it:

```bash
python tools/mods/make_mod.py mods/MyRobot
```

The check goes through every rule below and tells you exactly what is wrong.
There is nothing to build: copy the folder into the game's `mods` folder and
start the game, which makes the models into game files itself, from the
player's own copy of the base set. To share a robot, zip its folder as it is.
It holds only your own files, no data from the game, and players don't need
Python. (Older robots came with a `built` folder; the game still loads those,
but a folder with its models uses them and ignores `built`.)

`make_mod.py` reads the base set from the game folder and the add-ons from your
user data folder (`Documents\realsteel` on Windows,
`~/.local/share/realsteel` on Linux). Use `--game <folder>` and `--root <folder>`
if yours are elsewhere. To look at a robot without the game, run
`python tools/mods/render_folder.py mods/MyRobot preview.png hero front back`.

### The folder

```
MyRobot\
  robot.txt                 name, based_on, id, stats (below)
  body.obj  legs.obj  head.obj  arm_l.obj  arm_r.obj  fist_l.obj  fist_r.obj
  <part>_diffuse.png        colour, required for every part
  <part>_glow.png           glow mask (white glows), optional
  <part>_normal.png, _gloss.png, _ao.png    optional
  skeleton_scale.txt        one number, the robot's size, optional
```

**Coordinates.** Each part has its own space. Reshape a part in place; don't
move, rotate or scale it as a whole, because the game attaches it to the
skeleton where it is. The axes are **+X = the robot's left, +Y = down, and the
robot faces −Z**. Most tools assume +Y up, so import and export without any
axis conversion or unit scaling. `body` and `legs` share one space, with the
hips at the origin and the floor at about Y = 11.3. A robot is roughly 24
units tall.

**Groups.** Every face must be in one of its part's named groups (OBJ `g`
lines). A group moves with one bone:

| Part | Groups |
| --- | --- |
| `body` | `chest`, `abdomen`, `hips` |
| `legs` | `thigh_l`, `shin_l`, `foot_l`, `toe_l`, `thigh_r`, `shin_r`, `foot_r`, `toe_r` |
| `head` | `head` |
| `arm_l`, `arm_r` | `upper` (with the shoulder armour), `forearm` (with the elbow) |
| `fist_l`, `fist_r` | `fist` |

`_l` is the robot's left. A suffix added by a tool (`chest.001`) is fine.

**Faces.** Use triangles and quads. Wind them counter-clockwise seen from
outside; back faces are not drawn. Every corner needs a texture coordinate.
Normals are used if present. Keep each group under about 20,000 faces. Stock
parts use a few thousand per part, and the game has to stay at 60 fps.

**Textures.** Use OBJ UVs in the 0..1 square of the part's own texture, with
no tiling. Sizes: `body` and `legs` 1024 × 512, `head` and arms 512 × 512,
fists 512 × 256. Other sizes are resampled. Use PNG, 8 bits per channel, not
interlaced. Alpha is ignored. The colour texture is reduced to 256 colours,
so flat colours survive best.

### robot.txt

```
name = MY ROBOT        # what the parts are called (letters and spaces, up to 14)
based_on = KOGA        # skeleton and starting stats, see Skeletons (default BASENJA)
id = 512               # optional, see Part numbers
```

### Stats

Parts fight with the numbers of the `based_on` set. To change them, add
sections. Any value you leave out keeps the base set's:

```
[head]
sub_hp = 1100
defense = 950

[arms]            # both arms; [arm_l] / [arm_r] for one
attack = 1050

[fists]
attack = 1200

[all]
price = 2000
```

| Key | In the game | Parts |
| --- | --- | --- |
| `sub_hp` | SUB HP | head, body, arms |
| `attack` | ATK | arms, fists |
| `defense` | DFN | head, body, arms |
| `durability` | DRB | all |
| `energy` | ENG | all |
| `weight` | WGT (lbs) | all |
| `power_charge` | POWER CHARGE | body |
| `weight_capacity` | WEIGHT CAPACITY | legs |
| `price` | shop price | all |

Stock Lv.5 parts sit around sub HP 900-1300, attack 900-1100, defense
780-1070 and durability 5-9. The other fields of a part's record can be set
as `field<N> = value` (N = 0..97). `python tools/mods/show_stats.py <SET>`
prints a stock set's records to start from.

### Skeletons

`based_on` picks a stock set's skeleton: limb lengths, shoulder width and
where the parts plug together. The animations are shared, so every skeleton
works with every move. The choice also sets the starting stats, weight and
price. Pick it first, since the models are shaped around its joints.
`skeleton_scale.txt` scales the whole robot on top.

Lengths are in game units (legs = hip to floor; shoulder = how far the
shoulder joint sits out from the chest bone). The pictures show each set's
own robot on its skeleton, front and side.

| `based_on` | Legs | Thigh | Shin | Shoulder out | Forearm | Picture |
| --- | --- | --- | --- | --- | --- | --- |
| BASENJA | 11.3 | 4.3 | 4.6 | 4.3 | 4.2 | [view](skeletons/BASENJA.png) |
| BIOTECH | 12.8 | 5.5 | 5.7 | 0.4 | 3.7 | [view](skeletons/BIOTECH.png) |
| BOULDER | 11.9 | 4.6 | 4.9 | 1.2 | 4.0 | [view](skeletons/BOULDER.png) |
| CARAPACE | 12.9 | 5.6 | 6.6 | 1.6 | 3.9 | [view](skeletons/CARAPACE.png) |
| CHROMIUM | 13.0 | 5.6 | 5.7 | 1.7 | 3.9 | [view](skeletons/CHROMIUM.png) |
| FANTOM | 12.2 | 5.1 | 5.4 | 0.9 | 3.8 | [view](skeletons/FANTOM.png) |
| GEMINI | 13.1 | 5.3 | 6.4 | 2.7 | 3.7 | [view](skeletons/GEMINI.png) |
| GIMLET | 12.9 | 4.9 | 6.0 | 1.0 | 3.5 | [view](skeletons/GIMLET.png) |
| GORDIAS | 13.7 | 5.8 | 6.3 | 1.4 | 4.0 | [view](skeletons/GORDIAS.png) |
| JUPITER | 13.1 | 5.8 | 5.9 | 1.5 | 3.7 | [view](skeletons/JUPITER.png) |
| KOGA | 13.7 | 4.6 | 6.6 | 1.9 | 4.9 | [view](skeletons/KOGA.png) |
| MING | 13.6 | 4.7 | 7.1 | 0.5 | 3.6 | [view](skeletons/MING.png) |
| NECTRON | 12.7 | 5.2 | 5.3 | 2.1 | 3.9 | [view](skeletons/NECTRON.png) |
| PLATED | 12.3 | 5.5 | 4.9 | 0.9 | 4.5 | [view](skeletons/PLATED.png) |
| PROTON | 12.0 | 5.5 | 4.6 | 0.9 | 4.5 | [view](skeletons/PROTON.png) |
| QUADRITECH | 13.1 | 4.8 | 6.2 | 0.9 | 3.6 | [view](skeletons/QUADRITECH.png) |
| QUANTUM | 12.0 | 4.5 | 6.6 | 3.0 | 3.1 | [view](skeletons/QUANTUM.png) |
| RAMPAGE | 12.5 | 5.2 | 6.1 | 1.1 | 4.0 | [view](skeletons/RAMPAGE.png) |
| REPTILAX | 13.6 | 5.6 | 7.3 | 2.0 | 3.9 | [view](skeletons/REPTILAX.png) |
| ROCKET | 12.0 | 5.2 | 5.8 | 2.3 | 3.9 | [view](skeletons/ROCKET.png) |
| SCRAPHEAP | 11.7 | 3.2 | 5.9 | 1.4 | 4.5 | [view](skeletons/SCRAPHEAP.png) |
| SCYPHON | 13.8 | 5.4 | 6.8 | 1.4 | 2.8 | [view](skeletons/SCYPHON.png) |
| SHOGUN | 12.2 | 5.3 | 5.3 | 1.0 | 4.3 | [view](skeletons/SHOGUN.png) |
| ZELEZO | 13.1 | 5.1 | 5.5 | 3.5 | 4.5 | [view](skeletons/ZELEZO.png) |

### Part numbers

Each robot owns a number from 0 to 959 that fixes its parts' numbers in the
game. By default it comes from the robot's name, so the same mod gets the same
numbers on every machine. To rule out a clash with another robot whose name
happens to hash alike, claim a number with `id = <number>` in `robot.txt` and
add it to the list below with a pull request. Never reuse a published number.
If two installed robots claim the same number, the first one (alphabetically)
keeps it, and the game logs the clash.

| ID | Robot | Based on | Where to get it |
| --- | --- | --- | --- |
| 606 | ANGLER | BOULDER | the ReCompiled Steel mods download (release page) |
| 773 | QUILLBACK | KOGA | the ReCompiled Steel mods download (release page) |

### Limits

A mod can't change the skeleton or the animations, or add moves or opponents.
Each group moves rigidly with its bone, so there is no smooth bending at the
joints.
