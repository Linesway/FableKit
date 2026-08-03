"""
fable.py — editor-side helpers for FableKit automation.

Lives in Content/Python so it is importable as `import fable` inside the editor's
embedded Python. Wraps the FableKit C++ library (unreal.FableBP, graph authoring)
plus the stock unreal.* APIs for everything Python can already do natively:
assets, components, CDO defaults, reparenting, levels.

Everything here is driven from outside via Tools/fable/uexec.py.

SEE A WIDGET INSTEAD OF GUESSING (the one-liner worth remembering):

    python Plugins/FableKit/Tools/uexec.py -c "import fable; fable.render_widget('/Game/UI/WBP_Thing', r'C:/tmp/thing.png', 1920, 1080)"

then open/Read C:/tmp/thing.png. Renders the Widget Blueprint offscreen — no editor window, no PIE.
Live game state does NOT appear (the widget is built design-time, so NativeConstruct never runs); you
are reviewing layout, spacing, alignment and colour. See render_widget() and UFableRender's C++ header.
"""
import json
import unreal

BP = unreal.FableBP  # C++ graph-authoring library (FableKit plugin)


# ---------------------------------------------------------------- plumbing

def j(raw):
    """Parse a FableBP JSON return; raise on ok=false so failures are loud."""
    data = json.loads(raw)
    if isinstance(data, dict) and data.get("ok") is False:
        raise RuntimeError("FableBP error: %s" % json.dumps(data, indent=1))
    return data


def show(raw):
    """Pretty-print a FableBP JSON return (no raise)."""
    try:
        print(json.dumps(json.loads(raw), indent=1))
    except Exception:
        print(raw)


def _norm(path):
    path = path.strip()
    if "." not in path.rsplit("/", 1)[-1]:
        path = path + "." + path.rsplit("/", 1)[-1]
    return path


# ---------------------------------------------------------------- assets

def create_bp(package_path, name, parent):
    """Create a new Blueprint asset. parent: unreal.Class, '/Script/M.Class' or '/Game/...BP[_C]'."""
    if isinstance(parent, str):
        parent_cls = unreal.load_object(None, parent) if parent.startswith("/Script/") else None
        if parent_cls is None:
            parent_cls = unreal.EditorAssetLibrary.load_blueprint_class(parent.replace("_C", ""))
        parent = parent_cls
    if parent is None:
        raise RuntimeError("create_bp: parent class not found")
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("ParentClass", parent)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    asset = tools.create_asset(name, package_path, None, factory)
    if asset is None:
        raise RuntimeError("create_bp: create_asset failed (asset may already exist)")
    print("created %s" % asset.get_path_name())
    return asset


def save(path):
    if not unreal.EditorAssetLibrary.save_asset(path.split(".")[0], only_if_is_dirty=False):
        raise RuntimeError("save failed: %s" % path)
    print("saved %s" % path)


def save_all_dirty():
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    print("saved dirty packages")


def move_asset(src, dst):
    """Move/rename an asset; run fixup() afterwards to clean redirectors."""
    if not unreal.EditorAssetLibrary.rename_asset(src, dst):
        raise RuntimeError("rename_asset failed %s -> %s" % (src, dst))
    print("moved %s -> %s" % (src, dst))


def fixup(folder="/Game"):
    show(BP.fixup_redirectors(folder))


def delete_asset(path):
    if not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError("delete_asset failed: %s" % path)
    print("deleted %s" % path)


# ---------------------------------------------------------------- CDO defaults

def get_cdo(bp_path):
    """Class default object of a Blueprint asset — read/write defaults on it."""
    cls = unreal.EditorAssetLibrary.load_blueprint_class(bp_path.split(".")[0])
    if cls is None:
        raise RuntimeError("load_blueprint_class failed: %s" % bp_path)
    return unreal.get_default_object(cls)


def set_cdo(bp_path, **props):
    """Set CDO defaults by python property name, then compile+save yourself when done."""
    cdo = get_cdo(bp_path)
    for k, v in props.items():
        cdo.set_editor_property(k, v)
    print("set %d CDO props on %s" % (len(props), bp_path))


# ---------------------------------------------------------------- components (stock SubobjectDataSubsystem)

def _sds():
    return unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)


def list_components(bp_path):
    bp = unreal.load_object(None, _norm(bp_path))
    sds = _sds()
    handles = sds.k2_gather_subobject_data_for_blueprint(bp)
    out = []
    for h in handles:
        data = sds.k2_find_subobject_data_from_handle(h)
        name = unreal.SubobjectDataBlueprintFunctionLibrary.get_variable_name(data)
        obj = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
        out.append((str(name), obj.get_class().get_name() if obj else "?"))
    print(out)
    return out


def add_component(bp_path, component_class, name, parent_name=None):
    """Add an SCS component to a Blueprint. component_class: unreal.Class or '/Script/...' path."""
    if isinstance(component_class, str):
        component_class = unreal.load_object(None, component_class)
    bp = unreal.load_object(None, _norm(bp_path))
    sds = _sds()
    handles = sds.k2_gather_subobject_data_for_blueprint(bp)
    parent_handle = handles[0]  # root actor
    if parent_name:
        for h in handles:
            data = sds.k2_find_subobject_data_from_handle(h)
            if str(unreal.SubobjectDataBlueprintFunctionLibrary.get_variable_name(data)) == parent_name:
                parent_handle = h
                break
    params = unreal.AddNewSubobjectParams(
        parent_handle=parent_handle,
        new_class=component_class,
        blueprint_context=bp)
    handle, fail_reason = sds.add_new_subobject(params)
    # SubobjectDataHandle does not expose is_valid() to Python — probe via the data lookup instead.
    data = sds.k2_find_subobject_data_from_handle(handle)
    if data is None or unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data) is None:
        raise RuntimeError("add_new_subobject failed: %s" % fail_reason)
    sds.rename_subobject(handle, unreal.Text(name))
    print("added component %s (%s)" % (name, component_class.get_name()))
    return handle


# ---------------------------------------------------------------- blueprint lifecycle

# ---------------------------------------------------------------- Niagara
# Compose-and-configure, not module-stack editing: assemble effects from emitters that already look
# right, then retune them. See UFableNiagara's header for why the module stack is out of scope.

def fx_info(system_path):
    """Emitters (name/enabled/renderers) + exposed User parameters."""
    return j(unreal.FableNiagara.info(_norm(system_path)))


def fx_params(system_path):
    """Exposed User.* parameters with their current values."""
    return j(unreal.FableNiagara.list_user_params(_norm(system_path)))


def fx_dump_renderer(system_path, emitter, index=0):
    """Every property on one renderer — the discovery step before fx_set_renderer."""
    return j(unreal.FableNiagara.dump_renderer(_norm(system_path), emitter, index))


def fx_set(system_path, param, value):
    """Set an exposed User parameter. Bare scalars are fine ('2.5'); structs take UE text
    ('(R=1,G=0,B=0,A=1)'). The 'User.' prefix is optional."""
    return j(unreal.FableNiagara.set_user_param(_norm(system_path), param, str(value)))


def fx_set_renderer(system_path, emitter, index=0, **props):
    """Set renderer properties (Material, Meshes, SubImageSize...) by UE text, like wt_set."""
    import json as _json
    return j(unreal.FableNiagara.set_renderer_props(_norm(system_path), emitter, index, _json.dumps(props)))


def fx_enable_emitter(system_path, emitter, enabled=True):
    return j(unreal.FableNiagara.set_emitter_enabled(_norm(system_path), emitter, enabled))


def fx_create(package_path, name):
    """New empty NiagaraSystem asset."""
    return j(unreal.FableNiagara.create_system(package_path, name))


def fx_add_emitter(system_path, source_path, source_emitter="", new_name=""):
    """COPY an emitter into a system. source_path is a NiagaraEmitter asset, or a NiagaraSystem
    (then source_emitter picks which one). The donor is never modified. This is the authoring verb."""
    return j(unreal.FableNiagara.add_emitter(_norm(system_path), _norm(source_path), source_emitter, new_name))


def fx_remove_emitter(system_path, emitter):
    return j(unreal.FableNiagara.remove_emitter(_norm(system_path), emitter))


def fx_duplicate(system_path, dest_package, dest_name):
    """Duplicate a whole system — the safe way to iterate on a shipped effect."""
    return j(unreal.FableNiagara.duplicate_system(_norm(system_path), dest_package, dest_name))


def fx_compile(system_path):
    """Request a compile. `ready` will be FALSE in this same call — Niagara queues compile work onto the
    editor tick, and a bridge call holds the game thread so no tick can happen inside it. Check
    fx_ready() in a LATER call (a separate uexec invocation) before saving. Not a failure."""
    return j(unreal.FableNiagara.compile_system(_norm(system_path)))


def fx_ready(system_path):
    """Read-only readiness probe — poll this after fx_compile, in a separate call."""
    return j(unreal.FableNiagara.is_ready(_norm(system_path)))


def create_widget_bp(package_path, name, parent="/Script/UMG.UserWidget"):
    """Create a real UWidgetBlueprint (designer-capable) — the generic factory can't."""
    return j(unreal.FableBP.create_widget_blueprint(package_path, name, parent))


def wt_list(bp_path):
    return j(unreal.FableBP.wt_list_widgets(_norm(bp_path)))


def wt_add(bp_path, widget_class, name, parent_name=""):
    """Add a widget to the tree. Empty parent_name = root. Panels AddChild; Border/SizeBox SetContent."""
    return j(unreal.FableBP.wt_add_widget(_norm(bp_path), widget_class, name, parent_name))


def wt_remove(bp_path, name):
    return j(unreal.FableBP.wt_remove_widget(_norm(bp_path), name))


def wt_reparent(bp_path, name, new_parent, index=-1):
    """Move a widget (and its subtree) under a different parent. index -1 appends.

    The UWidget OBJECT is reused, so authored properties (fonts, brushes, tints, template variables)
    survive the move. The layout SLOT is rebuilt from scratch — re-apply wt_slot afterwards.
    Refuses on the root, self-parenting, cycles, and an occupied single-content parent.
    """
    return j(unreal.FableBP.wt_reparent_widget(_norm(bp_path), name, new_parent, int(index)))


def wt_set(bp_path, name, **props):
    """Set widget properties. Values are UE ImportText strings — T3D struct literals work verbatim."""
    import json as _json
    return j(unreal.FableBP.wt_set_props(_norm(bp_path), name, _json.dumps(props)))


def wt_slot(bp_path, name, **props):
    """Set layout-slot properties (Padding/HorizontalAlignment/Size...)."""
    import json as _json
    return j(unreal.FableBP.wt_set_slot_props(_norm(bp_path), name, _json.dumps(props)))


def wt_get(bp_path, name, props=""):
    """READ a widget's properties, in the same text form wt_set takes.

    props "" dumps everything; otherwise a comma-separated list of names (internal OR authored, so
    Blueprint variables like "In Font Info" work). Use this to COPY a value off an authored widget
    onto a new one rather than guessing — a fresh template instance does NOT match its siblings.

        v = fable.wt_get(BP, 'SinglePlayer', 'In Font Info')['props']['In Font Info']
        fable.wt_set(BP, 'Achievements', **{'In Font Info': v})
    """
    if isinstance(props, (list, tuple)):
        props = ",".join(props)
    return j(unreal.FableBP.wt_get_props(_norm(bp_path), name, props))


def measure_widget(bp_path, width=0, height=0, scale=None, pre_construct=None):
    """Desired size of the root AND of every widget in the tree.

    A render tells you a panel is too tall; this tells you WHICH CHILD is forcing it. Sort by
    desired_h and read the top. Same design-time construction as render_widget.
    """
    import json as _json
    opts = {}
    if scale is not None:
        opts["scale"] = float(scale)
    if pre_construct is not None:
        opts["pre_construct"] = bool(pre_construct)
    return j(unreal.FableRender.measure_widget(_norm(bp_path), int(width), int(height),
                                               _json.dumps(opts) if opts else ""))


def tallest(bp_path, top=12):
    """measure_widget, sorted by height — the one-liner for 'why is this thing so tall'."""
    r = measure_widget(bp_path)
    rows = sorted(r["widgets"], key=lambda w: -w["desired_h"])[:top]
    print("root %.0f x %.0f" % (r["root_w"], r["root_h"]))
    for w in rows:
        print("  %7.1f x %7.1f  %-30s %-26s <- %s"
              % (w["desired_w"], w["desired_h"], w["name"], w["class"], w["parent"]))
    return rows


# ---------------------------------------------------------------- offscreen render

def render_widget(bp_path, out_png, width=0, height=0,
                  background=None, scale=None, pre_construct=None):
    """Render a Widget Blueprint offscreen to a PNG. Returns the reply dict; raises on failure.

        fable.render_widget('/Game/UI/WBP_Thing', r'C:/tmp/thing.png', 1920, 1080)

    width/height 0 auto-sizes (designer canvas size, then the measured desired size). Pass explicit
    numbers for anything with a root Canvas Panel — those measure 0x0 and auto-size will refuse.

    background     'transparent'|'black'|'white'|'dark'|'light'|'grey', '#RRGGBB[AA]', linear
                   'R,G,B[,A]', or '(R=..,G=..,B=..,A=..)'. Default 'dark'. This is what a
                   transparent panel gets composited over, so flip it to 'light' to check contrast.
    scale          DPI / layout scale, 0..8 (default 1.0). The image stays width x height; the widget
                   lays out as if the application DPI scale were this.
    pre_construct  False skips NativePreConstruct. The escape hatch: PreConstruct is the only widget
                   code that runs here, so it is the only thing that can still crash on null state.

    WHAT YOU SEE: layout and styling only. The widget is constructed design-time (same as the editor's
    own asset thumbnails), so NativeConstruct and NativeOnInitialized never run and anything driven
    from a PlayerController / PlayerState / inventory shows its design-time default instead.
    """
    import json as _json
    opts = {}
    if background is not None:
        opts["background"] = str(background)
    if scale is not None:
        opts["scale"] = float(scale)
    if pre_construct is not None:
        opts["pre_construct"] = bool(pre_construct)
    res = j(unreal.FableRender.render_widget(
        _norm(bp_path), out_png, int(width), int(height),
        _json.dumps(opts) if opts else ""))
    print("rendered %s -> %s (%dx%d)" % (bp_path, res["png"], res["width"], res["height"]))
    return res


def render_live(bp_path, out_png, width=1920, height=1080, calls=None, background=None, scale=None):
    """Render with the REAL widget lifecycle (NativeOnInitialized + NativeConstruct) — the way to
    see RUNTIME-COMPOSED screens (chest, NPC menu, achievements, Index of the Eye) as they ship.

        fable.render_live('/Game/UI/...WGBP_Options', r'C:/tmp/opt_search.png',
                          calls=[["SetSearchFilter", "fps"]])

    calls  list of ["FunctionName", arg, ...] invoked on the widget after construction, before the
           draw — puts the widget into the STATE being verified. A failed call fails the render.

    CAVEATS: game code runs with no game state — null-guarded widgets only; counts/data read as
    empty; icon textures may render as checkers in the preview world. Judge LAYOUT.
    """
    import json as _json
    opts = {"live": True}
    if background is not None:
        opts["background"] = str(background)
    if scale is not None:
        opts["scale"] = float(scale)
    if calls:
        opts["calls"] = list(calls)
    res = j(unreal.FableRender.render_widget(
        _norm(bp_path), out_png, int(width), int(height), _json.dumps(opts)))
    print("live-rendered %s -> %s (calls_run=%s)" % (bp_path, res["png"], res.get("calls_run", 0)))
    return res


def conform(bp_path, target_png, out_png, width=0, height=0, calls=None, diff_png=None):
    """Live-render bp_path and DIFF it against a target mock. Returns (mismatch_pct, reply_dict).

        pct, res = fable.conform('/Game/UI/...WGBP_Achievements',
                                 r'...TARGET_menu_achievements.png', r'...AFTER_ach.png')

    Renders at the TARGET image's size unless width/height are given, computes mean absolute
    per-pixel difference as a percentage, and (optionally) writes a heat overlay to diff_png.
    "Matches the design" becomes a NUMBER — claims without one are opinions.
    Requires PIL on the editor's python (falls back to size-only comparison without it).
    """
    from PIL import Image  # noqa: available in the editor python env used for mockups
    tgt = Image.open(target_png).convert("RGB")
    w = int(width) or tgt.width
    h = int(height) or tgt.height
    res = render_live(bp_path, out_png, w, h, calls=calls)
    got = Image.open(res["png"]).convert("RGB").resize(tgt.size)
    try:
        from PIL import ImageChops
        diff = ImageChops.difference(got, tgt)
        stat_sum = 0
        hist = diff.histogram()
        # mean abs diff over 3 channels
        total = 0
        for band in range(3):
            for value in range(256):
                count = hist[band * 256 + value]
                total += count
                stat_sum += count * value
        pct = 100.0 * stat_sum / max(total * 255.0, 1.0)
        if diff_png:
            diff.point(lambda v: min(255, v * 4)).save(diff_png)
    except Exception as e:  # pragma: no cover
        print("conform: diff failed (%s) — size-only check" % e)
        pct = -1.0
    print("conform %s vs %s -> %.2f%% mean pixel mismatch" % (bp_path, target_png, pct))
    return pct, res


def assert_tree(bp_path, spec, live=True, calls=None):
    """Assert named widgets EXIST (and optionally their class) in a widget's built tree.

        fable.assert_tree('/Game/UI/...WGBP_Achievements',
                          {"DetailStrip": None, "RailHost": "VerticalBox"})

    spec  {name_substring: class_substring_or_None}. Uses measure_widget (live by default, so
    runtime-composed trees are the ones asserted). Raises AssertionError listing every miss —
    structural regressions fail LOUDLY instead of shipping as a quietly wrong screen.
    """
    import json as _json
    opts = {"live": bool(live)}
    if calls:
        opts["calls"] = list(calls)
    res = j(unreal.FableRender.measure_widget(_norm(bp_path), 1920, 1080, _json.dumps(opts)))
    widgets = res.get("widgets", [])
    misses = []
    for name_part, class_part in dict(spec).items():
        found = [w for w in widgets if name_part.lower() in str(w.get("name", "")).lower()]
        if class_part is not None:
            found = [w for w in found if class_part.lower() in str(w.get("class", "")).lower()]
        if not found:
            misses.append("%s (%s)" % (name_part, class_part or "any class"))
    if misses:
        raise AssertionError("assert_tree(%s): MISSING %s" % (bp_path, ", ".join(misses)))
    print("assert_tree %s: all %d present" % (bp_path, len(spec)))
    return True


def render_mesh(mesh_path, out_png, width=1024, height=1024, yaw=None, pitch=None, roll=None,
                distance=None, fov=None, material=None, light_yaw=None, light_pitch=None,
                exposure=None):
    """Render a StaticMesh offscreen to a PNG from any camera angle. Returns the reply dict.

        fable.render_mesh('/Game/Effects/SwordSlash/SM_SlashArc', r'C:/tmp/arc.png',
                          yaw=35, pitch=-20)

    Runs in a throwaway preview world, so the level you have open is never spawned into or dirtied.

    yaw/pitch/roll  camera orbit in degrees. yaw 0 looks down +X; negative pitch looks DOWN.
    distance        uu from the bounds centre; None/0 auto-frames the bounding sphere.
    material        override every material slot. Render TWICE — once as authored to judge the
                    EFFECT, once with '/Engine/EngineMaterials/DefaultMaterial' to judge the
                    GEOMETRY. An unlit additive material hides curvature that a lit grey pass shows.
    exposure        fixed EV bias (default 1.0). Raise it to tame a blown-out emissive.
    """
    import json as _json
    opts = {}
    for key, val in (("yaw", yaw), ("pitch", pitch), ("roll", roll), ("distance", distance),
                     ("fov", fov), ("light_yaw", light_yaw), ("light_pitch", light_pitch),
                     ("exposure", exposure)):
        if val is not None:
            opts[key] = float(val)
    if material is not None:
        opts["material"] = _norm(material)
    res = j(unreal.FableRender.render_mesh(
        _norm(mesh_path), out_png, int(width), int(height),
        _json.dumps(opts) if opts else ""))
    print("rendered %s -> %s (%dx%d)" % (mesh_path, res["png"], res["width"], res["height"]))
    return res


def clear_dead_bindings(bp_path):
    """Widget BPs: drop orphaned FDelegateEditorBinding entries (deleted binding functions). The
    Bindings array is python-protected, so this rides the native FableBP.ClearDeadBindings."""
    return j(unreal.FableBP.clear_dead_bindings(_norm(bp_path)))


def bound_event(bp_path, graph, component, delegate, x=0.0, y=0.0):
    """The red 'On Clicked (MyButton)' node — bind a delegate declared on a CHILD widget/component.

    This is what wires an authored-but-inert button. `add_dispatcher_bind` cannot do it: that one is
    self-context only, so it reaches dispatchers on THIS blueprint and nothing else. And it cannot be
    done from Python at all — UK2Node_ComponentBoundEvent's DelegatePropertyName /
    ComponentPropertyName are bare UPROPERTY() with no CPF_Edit|CPF_BlueprintVisible, which
    set_editor_property refuses outright. Hence the native call.

    Idempotent: an existing bound event for the same (component, delegate) comes back with
    existing=True instead of a duplicate.

        n = fable.bound_event(BP, 'EventGraph', 'NewCharacterButton', 'OnClicked', -900, 2600)
        fable.BP.connect_pins(BP, 'EventGraph', n['node']['id'], 'then', callnode, 'execute')
    """
    return j(BP.add_component_bound_event(_norm(bp_path), graph, component, delegate,
                                          float(x), float(y)))


def compile_bp(bp_path):
    res = json.loads(BP.compile_bp(bp_path))
    print(json.dumps(res, indent=1))
    return res


def reparent(bp_path, new_parent):
    """Reparent a Blueprint (the ~555-weapon-BP migration tool). new_parent: class path or unreal.Class."""
    bp = unreal.load_object(None, _norm(bp_path))
    if isinstance(new_parent, str):
        new_parent = unreal.load_object(None, new_parent)
    unreal.BlueprintEditorLibrary.reparent_blueprint(bp, new_parent)
    print("reparented %s -> %s" % (bp_path, new_parent.get_name()))


def dump(bp_path, graph="", out=None, filter=""):
    """Dump a graph as JSON; write to file for big graphs instead of flooding HTTP."""
    raw = BP.dump_graph(bp_path, graph, filter)
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(json.dumps(json.loads(raw), indent=1))
        print("wrote %s" % out)
    else:
        show(raw)


def info(bp_path):
    show(BP.dump_blueprint(bp_path))


# ============================ UI TOOLSET (companion-hosting era) ============================
def _widget_slot_info(w):
    out = {}
    sl = w.slot if hasattr(w, "slot") else None
    if sl:
        out["slot_class"] = sl.get_class().get_name()
        for prop in ("padding", "size", "horizontal_alignment", "vertical_alignment", "z_order", "layout_data"):
            try: out[prop] = str(sl.get_editor_property(prop))
            except Exception: pass
    return out

def dump_tree(asset_path_or_class, live=False, out_path=None):
    """Recursive widget tree: class, visibility, slot props and (live) absolute geometry."""
    import unreal, json
    lines = []
    def walk(w, depth):
        if not w: return
        info = {"name": w.get_name(), "class": w.get_class().get_name()}
        try: info["visible"] = str(w.get_editor_property("visibility"))
        except Exception: pass
        info.update(_widget_slot_info(w))
        try:
            geo = w.get_cached_geometry()
            ap = unreal.SlateBlueprintLibrary.get_absolute_size(geo)
            tl = unreal.SlateBlueprintLibrary.local_to_absolute(geo, unreal.Vector2D(0, 0))
            info["abs_pos"] = (round(tl.x, 1), round(tl.y, 1)); info["abs_size"] = (round(ap.x, 1), round(ap.y, 1))
        except Exception: pass
        lines.append("  " * depth + json.dumps(info))
        try:
            if isinstance(w, unreal.PanelWidget):
                for i in range(w.get_children_count()):
                    walk(w.get_child_at(i), depth + 1)
        except Exception: pass
    if live:
        world = unreal.UnrealEditorSubsystem().get_game_world() or unreal.UnrealEditorSubsystem().get_editor_world()
        for w in unreal.WidgetBlueprintLibrary.get_all_widgets_of_class(world, unreal.UserWidget, False):
            if str(asset_path_or_class).lower() in w.get_class().get_name().lower():
                try: root = w.get_editor_property("widget_tree").get_editor_property("root_widget")
                except Exception: root = None
                walk(root or w, 0)
    else:
        bp = unreal.load_asset(asset_path_or_class)
        wt = bp.get_editor_property("widget_tree") if bp else None
        root = wt.get_editor_property("root_widget") if wt else None
        walk(root, 0)
    text = chr(10).join(lines) if lines else "(no widgets matched)"
    if out_path:
        open(out_path, "w", encoding="utf-8").write(text)
    print(text)
    return text

def find_widget(pattern, live=True):
    """Live viewport widgets whose class/name matches pattern, with absolute rects."""
    import unreal
    world = unreal.UnrealEditorSubsystem().get_game_world() or unreal.UnrealEditorSubsystem().get_editor_world()
    hits = []
    for w in unreal.WidgetBlueprintLibrary.get_all_widgets_of_class(world, unreal.UserWidget, False):
        n, c = w.get_name(), w.get_class().get_name()
        if pattern.lower() in n.lower() or pattern.lower() in c.lower():
            try:
                geo = w.get_cached_geometry()
                sz = unreal.SlateBlueprintLibrary.get_absolute_size(geo)
                tl = unreal.SlateBlueprintLibrary.local_to_absolute(geo, unreal.Vector2D(0, 0))
                hits.append((n, c, (round(tl.x, 1), round(tl.y, 1)), (round(sz.x, 1), round(sz.y, 1))))
            except Exception:
                hits.append((n, c, None, None))
    for h in hits: print(h)
    return hits

def ui_screenshot(out_path, annotate=True):
    """PIE screenshot; annotate writes a sibling .rects.txt with every live widget rect."""
    import unreal
    unreal.AutomationLibrary.take_high_res_screenshot(1920, 1080, out_path)
    if annotate:
        try: hits = find_widget("", live=True)
        except Exception: hits = []
        open(out_path + ".rects.txt", "w", encoding="utf-8").write(repr(hits))
    print("screenshot queued -> " + out_path)
