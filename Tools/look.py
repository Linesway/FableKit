# look.py — LOOK AT AN ASSET. A reusable harness, because judging a material by reading its node
# graph does not work: the portal shipped three wrong versions that were all "correct" on paper.
#
#   1. write Saved/FableKitJobs/look.json   (see SHAPE below)
#   2. python Plugins/FableKit/Tools/uexec.py -f "<abs>/Saved/FableKitJobs/look.py" --timeout 900
#   3. read Saved/Screenshots/WindowsEditor/<shot>.png
#
# SHAPE of look.json — a list of subjects:
#   [{"mat": "/Game/.../M_Thing",         # material or material instance
#     "shot": "look_thing",               # output png name
#     "mesh": "plane" | "sphere" | "cube" | "/Game/full/path.Asset",
#     "dist": 620,                        # camera distance, cm
#     "scale": 4.5}]                      # uniform-ish scale
#
# ============================ FOUR TRAPS THIS EXISTS TO ABSORB ============================
# Every one of these cost an editor cycle on 08-12 and is silent when you get it wrong:
#
#   1. A BARE new_level() HAS NO SKY AND NO LIGHT. Lit materials render PURE BLACK and an unlit one
#      floats on a black void. Use the Default TEMPLATE, which brings a light, a sky and a floor.
#   2. PYTHON'S Rotator IS (roll, pitch, yaw). /Engine/BasicShapes/Plane faces +Z and needs PITCH 90
#      to face the camera; roll 90 turns it EDGE-ON and you photograph an empty room.
#   3. ☠☠ HIGH-RES CAPTURE IS ASYNC AND time.sleep() CANNOT HELP. A bridge call OWNS THE GAME
#      THREAD, so sleeping inside one stops the very ticking the capture needs to finish: N shots in
#      one call always yields exactly ONE file, the last. Sleeping 9 s between them changes nothing.
#      THE ONLY FIX IS TO RETURN. So this script does ONE subject per invocation — it takes the
#      first whose png does not exist yet — and the shell calls it once per subject.
#   4. take_high_res_screenshot GRABS THE EDITOR VIEWPORT, not PIE and not UMG. Useless for widgets
#      (read their properties instead); exactly right for a material on a mesh.
#
# ☠ It builds a scratch level and NEVER saves it. Nothing here can touch a real map.
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_PY = os.path.normpath(os.path.join(_HERE, "..", "..",
                                          "Plugins", "FableKit", "Content", "Python"))
if os.path.isdir(PLUGIN_PY) and PLUGIN_PY not in sys.path:
    sys.path.insert(0, PLUGIN_PY)

import unreal  # noqa: E402

EAL = unreal.EditorAssetLibrary
CFG = os.path.join(_HERE, "look.json")
OUT = os.path.join(_HERE, "look.result.txt")

SHAPES = {
    "plane":  "/Engine/BasicShapes/Plane.Plane",
    "sphere": "/Engine/BasicShapes/Sphere.Sphere",
    "cube":   "/Engine/BasicShapes/Cube.Cube",
    "cyl":    "/Engine/BasicShapes/Cylinder.Cylinder",
}

lines = []


def note(s):
    unreal.log_warning("LOOK: " + str(s))
    lines.append(str(s))


def main():
    if not os.path.exists(CFG):
        note("no look.json next to this script")
        return
    subjects = json.load(open(CFG, encoding="utf-8"))

    les = unreal.LevelEditorSubsystem()
    eas = unreal.EditorActorSubsystem()
    ues = unreal.UnrealEditorSubsystem()

    # TRAP 1 — the template brings light + sky + floor. new_level() alone renders black.
    # ☠☠ TRAP 5, and it invalidated a whole verification pass: DO NOT RECREATE THE LEVEL EVERY CALL.
    # A shot taken in the same call that built the level comes back EMPTY (sky and floor, no
    # subject) or pure black — the viewport has not settled. Because this script runs once per
    # subject, rebuilding each time meant every shot was a first-shot. Build it once, then reuse.
    # ☠ DO NOT identify the level BY NAME — an unsaved /Temp/ level reports as "Untitled", so a
    # name test never matches and the level is rebuilt on every call, which is the very bug this
    # guard exists to prevent. Ask a FACT instead: is our marker actor in the world?
    marker = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label() == "LOOK_HARNESS_MARKER":
            marker = a
            break
    if marker is None:
        les.new_level_from_template("/Temp/LookHarness", "/Engine/Maps/Templates/Template_Default")
        mk = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, -5000),
                                        unreal.Rotator(0, 0, 0))
        mk.set_actor_label("LOOK_HARNESS_MARKER")
        note("scratch level built — CALL AGAIN to shoot (this one will not have settled)")
        open(OUT, "w", encoding="utf-8").write("\n".join(lines))
        return
    note("reusing the warm scratch level")

    # ONE subject per call — see TRAP 3. Pick the first whose shot has not been taken.
    shots_dir = unreal.Paths.project_saved_dir() + "Screenshots/WindowsEditor/"
    todo = None
    for s in subjects:
        if not os.path.exists(os.path.normpath(shots_dir + s["shot"] + ".png")):
            todo = s
            break
    if todo is None:
        note("ALL DONE — every subject already has a png")
        open(OUT, "w", encoding="utf-8").write("\n".join(lines))
        return

    for s in [todo]:
        mat = EAL.load_asset(s["mat"])
        if mat is None:
            note("MISSING material %s" % s["mat"])
            continue
        mesh_key = s.get("mesh", "sphere")
        mesh = EAL.load_asset(SHAPES.get(mesh_key, mesh_key))
        if mesh is None:
            note("MISSING mesh %s" % mesh_key)
            continue

        for a in eas.get_all_level_actors():
            lbl = a.get_actor_label()
            if isinstance(a, unreal.StaticMeshActor) and "Floor" not in lbl and lbl != "LOOK_HARNESS_MARKER":
                eas.destroy_actor(a)

        z = float(s.get("z", 260))
        act = eas.spawn_actor_from_class(unreal.StaticMeshActor,
                                         unreal.Vector(0, 0, z), unreal.Rotator(0, 0, 0))
        c = act.static_mesh_component
        c.set_editor_property("static_mesh", mesh)
        sc = float(s.get("scale", 3.0))
        act.set_actor_scale3d(unreal.Vector(sc, sc, sc if mesh_key != "plane" else 1.0))
        if mesh_key == "plane":
            # TRAP 2 — (roll, pitch, yaw). PITCH 90 stands a +Z plane up to face -X.
            act.set_actor_rotation(unreal.Rotator(0.0, 90.0, 0.0), False)
        for i in range(max(1, c.get_num_materials())):
            c.set_material(i, mat)

        ues.set_level_viewport_camera_info(
            unreal.Vector(-float(s.get("dist", 620)), 0.0, z), unreal.Rotator(0.0, 0.0, 0.0))

        # TRAP 3 — one shot, then real time before the next, or this one is clobbered.
        unreal.AutomationLibrary.take_high_res_screenshot(
            int(s.get("w", 1600)), int(s.get("h", 900)), s["shot"])
        note("shot %-22s <- %s   (returning so the game thread can finish it)"
             % (s["shot"], s["mat"].split("/")[-1]))

    note("one subject done; call again for the next")
    open(OUT, "w", encoding="utf-8").write("\n".join(lines))


main()
