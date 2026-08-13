# look2.py — render an asset through a SCENE CAPTURE, not the editor viewport.
#
# ☠☠ WHY THE VIEWPORT ROUTE HAD TO GO. take_high_res_screenshot photographs the active editor
# VIEWPORT, and the editor does not render a viewport it considers occluded or unfocused — so the
# same script returned a perfect image in one session and pure black in the next, with no signal
# about which. It cost a whole verification pass and nearly convinced me a good material was broken.
# A SceneCapture2D renders on demand into a render target and does not care what the window is
# doing, which makes the result a FACT instead of a coin flip.
#
#   1. write look.json  (same shape as before)
#   2. call this once per subject — it does the first whose png is missing
#   3. read Saved/Screenshots/WindowsEditor/<shot>.png
#
# Still true from the viewport version, and still load-bearing:
#   ☠ python Rotator is (roll, pitch, yaw); a +Z Plane needs PITCH 90 to face the camera
#   ☠ a scratch level from the DEFAULT TEMPLATE, so there is a light and a sky (a bare new_level
#     renders lit materials pure black)
#   ☠ ONE subject per call — a bridge call owns the game thread, so nothing async finishes while
#     you sleep inside one
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_PY = os.path.normpath(os.path.join(_HERE, "..", "..",
                                          "Plugins", "FableKit", "Content", "Python"))
if os.path.isdir(PLUGIN_PY) and PLUGIN_PY not in sys.path:
    sys.path.insert(0, PLUGIN_PY)

import unreal  # noqa: E402

EAL = unreal.EditorAssetLibrary
CFG = os.path.join(_HERE, "look.json")
OUT = os.path.join(_HERE, "look2.result.txt")
SHOTS = os.path.normpath(unreal.Paths.project_saved_dir() + "Screenshots/WindowsEditor")

SHAPES = {
    "plane":  "/Engine/BasicShapes/Plane.Plane",
    "sphere": "/Engine/BasicShapes/Sphere.Sphere",
    "cube":   "/Engine/BasicShapes/Cube.Cube",
}

lines = []


def note(s):
    unreal.log_warning("LOOK2: " + str(s))
    lines.append(str(s))


def finish():
    open(OUT, "w", encoding="utf-8").write("\n".join(lines))


def main():
    subjects = json.load(open(CFG, encoding="utf-8"))
    les = unreal.LevelEditorSubsystem()
    eas = unreal.EditorActorSubsystem()
    ues = unreal.UnrealEditorSubsystem()

    marker = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label() == "LOOK_HARNESS_MARKER":
            marker = a
            break
    if marker is None:
        les.new_level_from_template("/Temp/LookHarness2", "/Engine/Maps/Templates/Template_Default")
        mk = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, -8000),
                                        unreal.Rotator(0, 0, 0))
        mk.set_actor_label("LOOK_HARNESS_MARKER")
        note("scratch level built — call again to shoot")
        return finish()

    todo = None
    for s in subjects:
        if not os.path.exists(os.path.join(SHOTS, s["shot"] + ".png")):
            todo = s
            break
    if todo is None:
        note("ALL DONE")
        return finish()

    mat = EAL.load_asset(todo["mat"])
    mesh = EAL.load_asset(SHAPES.get(todo.get("mesh", "sphere"), todo.get("mesh")))
    if mat is None or mesh is None:
        note("MISSING mat/mesh for %s" % todo["shot"])
        return finish()

    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        if isinstance(a, unreal.StaticMeshActor) and "Floor" not in lbl and lbl != "LOOK_HARNESS_MARKER":
            eas.destroy_actor(a)

    z = float(todo.get("z", 260))
    act = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, z),
                                     unreal.Rotator(0, 0, 0))
    c = act.static_mesh_component
    c.set_editor_property("static_mesh", mesh)
    sc = float(todo.get("scale", 3.0))
    is_plane = todo.get("mesh") == "plane"
    act.set_actor_scale3d(unreal.Vector(sc, sc, 1.0 if is_plane else sc))
    if is_plane:
        act.set_actor_rotation(unreal.Rotator(0.0, 90.0, 0.0), False)
    for i in range(max(1, c.get_num_materials())):
        c.set_material(i, mat)

    world = ues.get_editor_world()
    w, h = int(todo.get("w", 1280)), int(todo.get("h", 720))
    rt = unreal.RenderingLibrary.create_render_target2d(world, w, h)
    # ☠ export_render_target writes by the TARGET'S FORMAT, not by the file extension you pass.
    # A default float target produces an .HDR with a .png name — unreadable as an image. RGBA8
    # gives an actual PNG.
    rt.set_editor_property("render_target_format",
                           unreal.TextureRenderTargetFormat.RTF_RGBA8)

    dist = float(todo.get("dist", 620))
    cap = eas.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(-dist, 0.0, z),
                                     unreal.Rotator(0, 0, 0))
    comp = cap.capture_component2d
    comp.set_editor_property("texture_target", rt)
    # FINAL COLOR, or you capture scene colour with no tonemapping and everything reads wrong.
    comp.set_editor_property("capture_source",
                             unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 50.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.capture_scene()

    unreal.RenderingLibrary.export_render_target(world, rt, SHOTS, todo["shot"] + ".png")
    eas.destroy_actor(cap)

    ok = os.path.exists(os.path.join(SHOTS, todo["shot"] + ".png"))
    note("captured %-18s <- %-34s FILE=%s" % (todo["shot"], todo["mat"].split("/")[-1], ok))
    finish()


main()
