"""
FableKit end-to-end smoke test. Run from outside the editor with:
    python Plugins/FableKit/Tools/uexec.py -f "<abs>/Plugins/FableKit/Tools/smoke_test.py"

Creates /Game/FableKitSmoke/BP_FableSmoke, authors a small graph (event, function,
repnotify var, wired PrintString incl. an auto-conversion), compiles, saves,
then deletes the folder. Prints "FABLE SMOKE: PASS" on success.
Set KEEP = True to inspect the asset in the editor afterwards.
"""
import json
import unreal

KEEP = False
FOLDER = "/Game/FableKitSmoke"
BP_PATH = FOLDER + "/BP_FableSmoke"

B = unreal.FableBP


def ok(raw, what):
    data = json.loads(raw)
    if not data.get("ok"):
        raise RuntimeError("%s FAILED: %s" % (what, json.dumps(data, indent=1)))
    print("  ok: %s" % what)
    return data


print("== FableKit smoke ==")
print(B.ping())

# clean slate
if unreal.EditorAssetLibrary.does_directory_exist(FOLDER):
    unreal.EditorAssetLibrary.delete_directory(FOLDER)

# 1. create asset (stock python path)
factory = unreal.BlueprintFactory()
factory.set_editor_property("ParentClass", unreal.Actor)
asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
    "BP_FableSmoke", FOLDER, None, factory)
assert asset is not None, "create_asset failed"
print("  ok: created %s" % BP_PATH)

# 2. member variable with repnotify
ok(B.add_variable(BP_PATH, "Health", "float", "100.0", "Stats",
                  True, False, False, "repnotify"), "add_variable Health")
graphs = ok(B.list_graphs(BP_PATH), "list_graphs")
names = [g["name"] for g in graphs["graphs"]]
assert "OnRep_Health" in names, "OnRep_Health graph missing: %s" % names
print("  ok: OnRep_Health graph exists")

# 3. function with params
ok(B.add_function(BP_PATH, "TakeHit", "Amount:float", "NewHealth:float", False),
   "add_function TakeHit")

# 4. BeginPlay -> PrintString, with Health -> InString auto-conversion
ev = ok(B.add_event(BP_PATH, "", "ReceiveBeginPlay", "", 0, 0), "add_event BeginPlay")
ev_id = ev["node"]["id"]
ps = ok(B.add_call_function(BP_PATH, "", "/Script/Engine.KismetSystemLibrary:PrintString", 400, 0),
        "add_call_function PrintString")
ps_id = ps["node"]["id"]
ok(B.connect_pins(BP_PATH, "", ev_id, "then", ps_id, "execute"), "connect exec")

vg = ok(B.add_var_get(BP_PATH, "", "Health", 150, 200), "add_var_get Health")
vg_id = vg["node"]["id"]
conn = ok(B.connect_pins(BP_PATH, "", vg_id, "Health", ps_id, "InString"),
          "connect Health->InString (conversion)")
print("    response: %s" % conn.get("response", ""))

ok(B.set_pin_default(BP_PATH, "", ps_id, "Duration", "5.0"), "set_pin_default Duration")

# 5. custom event + dispatcher round trip
ok(B.add_custom_event(BP_PATH, "", "OnSmokeSignal", 0, 500), "add_custom_event")
ok(B.add_event_dispatcher(BP_PATH, "OnSmokeBroadcast", "Value:int"), "add_event_dispatcher")
ok(B.add_dispatcher_call(BP_PATH, "", "OnSmokeBroadcast", 400, 500), "add_dispatcher_call")

# 6. compile + verify
res = ok(B.compile_bp(BP_PATH), "compile")
assert res["num_errors"] == 0, "compile errors: %s" % res["errors"]
print("  ok: compiled status=%s warnings=%d" % (res["status"], res["num_warnings"]))

# 7. dump graph sanity
dump = ok(B.dump_graph(BP_PATH, "", ""), "dump_graph")
assert dump["total_nodes"] >= 4, "expected >=4 nodes, got %d" % dump["total_nodes"]
print("  ok: dump shows %d nodes" % dump["total_nodes"])

# 8. save + cleanup
assert unreal.EditorAssetLibrary.save_asset(BP_PATH, only_if_is_dirty=False), "save failed"
print("  ok: saved")
if not KEEP:
    unreal.EditorAssetLibrary.delete_directory(FOLDER)
    print("  ok: cleaned up %s" % FOLDER)

print("FABLE SMOKE: PASS")
