#!/usr/bin/env python3
"""
world.py — get from "editor is open" to "a player is standing in a named Deliria world", in one call.

    python world.py --new W5Fresh --seed 1337     # create + enter (cheats ON)
    python world.py --enter W5Fresh               # enter an existing one
    python world.py --info                        # what world is actually live right now
    python world.py --stop                        # end PIE

WHY THIS EXISTS
---------------
Every PIE session in this project needs the same five steps, and skipping any one of them produces a
session that LOOKS fine and measures the wrong thing:

  1. ☠ Load `/Game/Maps/TerrariaWorldGen/Deliria` FIRST. `editor_request_begin_play()` plays whatever
     map the editor happens to have open, and a freshly launched editor sits on an empty Untitled
     level. PIE there runs in a void that still renders, still has a pawn, and still answers every
     query.
  2. Begin play.
  3. ☠ CREATE OR ENTER A WORLD EXPLICITLY. Pressing play does NOT create one — you get a menu pawn at
     (0,0,90) and no terrain.
  4. ☠ CHEATS ON AT CREATION. `world_cheats_enabled` is a field on the save; it cannot be turned on
     afterwards, so a world made without it has to be thrown away and remade.
  5. ☠ CHANGE THE SDF => MAKE A NEW WORLD. Saved regions are restored OVER the procedural field, so a
     stale world is a mixture of two worldgens that measures the OLD bug while looking identical.

☠☠ AND THE READBACK TRAP THAT COST A CYCLE HERE: `GlobalGetDefaultWorldSaveObject` returns the
DEFAULT save template, not the running world — it reports world_name='' seed=0 cheats=False no matter
what is actually loaded. The live one is `GlobalGetCurrentWorldSaveObject`. --info uses that one, so
"which world am I in" has a truthful answer.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uexec  # noqa: E402

GAMEPLAY_MAP = "/Game/Maps/TerrariaWorldGen/Deliria"
MENU_MAP = "/Game/Maps/MainMenu/MainMenu"


def py(code, timeout=300):
    resp = uexec.run_python("127.0.0.1", 30010, code, timeout)
    out, errs = [], []
    for e in resp.get("LogOutput", []) or []:
        (errs if e.get("Type") == "Error" else out).append(e.get("Output", "").rstrip("\n"))
    if errs:
        raise RuntimeError("\n".join(errs))
    return "\n".join(out).strip()


PREAMBLE = """
import unreal
def _w():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
"""


def info():
    print(py(PREAMBLE + """
w = _w()
if w is None:
    print(json.dumps({'pie': False}) if False else '{"pie": false}')
else:
    cur = unreal.SGameInstance.global_get_current_world_save_object(w)
    gi  = cur.get_editor_property('general_world_info') if cur else None
    import json as _j
    print(_j.dumps({
        'pie': True,
        'map': w.get_name(),
        'world': str(gi.get_editor_property('world_name')) if gi else None,
        'seed': gi.get_editor_property('world_seed') if gi else None,
        'cheats': bool(gi.get_editor_property('world_cheats_enabled')) if gi else None,
    }))
"""))


def ensure_pie():
    """Load the real map and begin play if PIE is not already up. Returns when a game world exists."""
    state = py(PREAMBLE + "print('UP' if _w() is not None else 'DOWN')")
    if state.endswith("UP"):
        return
    py(PREAMBLE + """
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
les.load_level('%s')
les.editor_request_begin_play()
print('begun')
""" % GAMEPLAY_MAP)
    # The wait belongs HERE: a bridge call owns the game thread, so nothing ticks while one is open.
    for _ in range(60):
        time.sleep(2)
        if py(PREAMBLE + "print('UP' if _w() is not None else 'DOWN')").endswith("UP"):
            return
    raise RuntimeError("PIE did not come up")


def new_world(name, seed):
    ensure_pie()
    py(PREAMBLE + """
w   = _w()
sav = unreal.SGameInstance.global_get_default_world_save_object(w)
gi  = sav.get_editor_property('general_world_info')
gi.set_editor_property('world_name', '%s')
gi.set_editor_property('world_seed', %d)
gi.set_editor_property('world_seed_text', '%d')
gi.set_editor_property('world_cheats_enabled', True)   # cannot be enabled later
sav.set_editor_property('general_world_info', gi)
unreal.SGameInstance.save_world(sav)
unreal.SGameInstance.start_game_on_world(w, unreal.SGameInstance.load_world('%s'), 0)
print('started')
""" % (name, seed, seed, name))
    _settle()


def enter_world(name):
    ensure_pie()
    py(PREAMBLE + """
w = _w()
unreal.SGameInstance.start_game_on_world(w, unreal.SGameInstance.load_world('%s'), 0)
print('started')
""" % name)
    _settle()


def _settle():
    """Terrain streams in after the travel; a snapshot taken too early reads as an empty world."""
    for _ in range(40):
        time.sleep(2)
        try:
            if "SDeliriaChunkWorld" in py(PREAMBLE + """
w = _w()
cw = unreal.GameplayStatics.get_all_actors_of_class(w, unreal.load_class(None,'/Script/SVoxelPlugin.SChunkWorld'))
print(cw[0].get_name() if cw else 'none')
"""):
                return
        except RuntimeError:
            pass  # mid-travel the world pointer flickers; keep waiting
    print("warning: no chunk world after 80s", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--new", metavar="NAME", help="create a fresh world and enter it (cheats ON)")
    ap.add_argument("--seed", type=int, default=1337, help="worldgen seed (1337 matches the VoxelCPU tests)")
    ap.add_argument("--enter", metavar="NAME", help="enter an existing world")
    ap.add_argument("--info", action="store_true", help="report the LIVE world (not the default template)")
    ap.add_argument("--stop", action="store_true", help="end PIE")
    a = ap.parse_args()

    if a.stop:
        py(PREAMBLE + "unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()\nprint('ended')")
    elif a.new:
        new_world(a.new, a.seed)
        info()
    elif a.enter:
        enter_world(a.enter)
        info()
    else:
        info()
    return 0


if __name__ == "__main__":
    sys.exit(main())
