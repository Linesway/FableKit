"""
fable.py — editor-side helpers for FableKit automation.

Lives in Content/Python so it is importable as `import fable` inside the editor's
embedded Python. Wraps the FableKit C++ library (unreal.FableBP, graph authoring)
plus the stock unreal.* APIs for everything Python can already do natively:
assets, components, CDO defaults, reparenting, levels.

Everything here is driven from outside via Tools/fable/uexec.py.
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


def wt_set(bp_path, name, **props):
    """Set widget properties. Values are UE ImportText strings — T3D struct literals work verbatim."""
    import json as _json
    return j(unreal.FableBP.wt_set_props(_norm(bp_path), name, _json.dumps(props)))


def wt_slot(bp_path, name, **props):
    """Set layout-slot properties (Padding/HorizontalAlignment/Size...)."""
    import json as _json
    return j(unreal.FableBP.wt_set_slot_props(_norm(bp_path), name, _json.dumps(props)))


def clear_dead_bindings(bp_path):
    """Widget BPs: drop orphaned FDelegateEditorBinding entries (deleted binding functions). The
    Bindings array is python-protected, so this rides the native FableBP.ClearDeadBindings."""
    return j(unreal.FableBP.clear_dead_bindings(_norm(bp_path)))


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
