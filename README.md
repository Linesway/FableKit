# FableKit — Blueprint authoring bridge for AI agents (UE 5.6)

Author Unreal Engine **Blueprints from the command line** while the editor runs:
create/read/edit graphs, spawn + wire + configure nodes, add variables (incl.
RepNotify), functions, event dispatchers, interfaces, compile with error feedback,
manage assets/components/CDO defaults, and fix up redirectors after moves.

Built for driving the editor from an AI coding agent (Claude Code), but it's just
HTTP + Python — anything can drive it. Verified end-to-end on UE 5.6.1 (source build).

## Architecture

```
agent / CLI
  └─ Tools/uexec.py                  (pure-stdlib client, no deps)
       └─ HTTP PUT 127.0.0.1:30010/remote/object/call     [RemoteControl plugin — stock]
            └─ ExecutePythonCommandEx                     [PythonScriptPlugin — stock]
                 ├─ stock unreal.*    assets, components, CDO defaults, reparent, levels
                 └─ unreal.FableBP.*  graphs/nodes/pins/vars/functions/compile
                                      [this plugin — C++, ~45 functions]
```

Why a C++ plugin at all: node/pin-level graph editing is **not** reflected into
stock Python (`UEdGraphPin` isn't a `UObject`), so a native library is mandatory.
Everything else rides on stock engine plugins.

## Install (any project)

1. Clone into `<Project>/Plugins/FableKit`.
2. Enable plugins in your `.uproject`: `FableKit` and `RemoteControl`
   (both fine with `"TargetAllowList": ["Editor"]`).
3. Project config:
   - `Config/DefaultRemoteControl.ini`
     ```ini
     [/Script/RemoteControlCommon.RemoteControlSettings]
     bAutoStartWebServer=True
     RemoteControlHttpServerPort=30010
     bEnableRemotePythonExecution=True
     ```
   - `Config/DefaultEngine.ini` (keep the server loopback-only)
     ```ini
     [HTTPServer.Listeners]
     DefaultBindAddress=127.0.0.1
     ```
4. Compile the editor target (UBT auto-discovers the plugin), start the editor.
5. Verify: `python Plugins/FableKit/Tools/uexec.py --ping`
   → `{"ok":true,"plugin":"FableKit",...}`
6. Full test: `python Plugins/FableKit/Tools/uexec.py -f "<abs>/Plugins/FableKit/Tools/smoke_test.py"`
   → `FABLE SMOKE: PASS`

If the editor was already running before `bEnableRemotePythonExecution` existed in
config, run `py "<abs>/Plugins/FableKit/Tools/open_gate.py"` in the editor console
once (or restart).

## Usage

```bash
# liveness
python Plugins/FableKit/Tools/uexec.py --ping

# read a Blueprint
python Plugins/FableKit/Tools/uexec.py -c "import fable; fable.info('/Game/Blueprints/BP_Thing')"

# dump a graph to a file (preferred for big graphs)
python Plugins/FableKit/Tools/uexec.py -c "import fable; fable.dump('/Game/BP_X','EventGraph',out=r'C:/temp/g.json')"

# author: add an event + wired PrintString
python Plugins/FableKit/Tools/uexec.py -c "
import unreal, json
B = unreal.FableBP
bp = '/Game/Dev/BP_Test'
ev = json.loads(B.add_event(bp, '', 'ReceiveBeginPlay', '', 0, 0))['node']['id']
ps = json.loads(B.add_call_function(bp, '', '/Script/Engine.KismetSystemLibrary:PrintString', 400, 0))['node']['id']
print(B.connect_pins(bp, '', ev, 'then', ps, 'execute'))
print(B.compile_bp(bp))
"

# run a whole script file (the editor reads the path directly — same machine)
python Plugins/FableKit/Tools/uexec.py -f "C:/path/to/job.py"
```

`import fable` (Content/Python/fable.py, auto-injected onto sys.path by uexec)
adds helpers: `create_bp`, `add_component`, `list_components`, `get_cdo`/`set_cdo`,
`save`, `move_asset`, `fixup`, `reparent`, `compile_bp`, `dump`, `info`.

## unreal.FableBP quick reference (python names)

- read: `ping`, `list_graphs`, `dump_blueprint`, `dump_graph`, `get_node`,
  `list_functions`, `list_overridable_events`
- nodes: `add_call_function`, `add_event`, `add_custom_event`, `add_var_get`,
  `add_var_set`, `add_branch`, `add_sequence`, `add_macro` (ForEachLoop etc.),
  `add_cast`, `add_spawn_actor`, `add_make_struct`, `add_break_struct`,
  `add_dispatcher_call`, `add_dispatcher_bind`, `add_node_by_class` (escape hatch),
  `add_comment`, `delete_node`, `move_node`, `reconstruct_node`, `add_input_pin`,
  `add_user_pin` (params on custom events / function entry/result)
- pins: `connect_pins` (auto-conversion nodes work), `break_pin_link`,
  `break_all_pin_links`, `set_pin_default`, `split_pin`, `recombine_pin`
- members: `add_variable` (flags + replicated/repnotify — creates the OnRep graph),
  `remove_variable`, `set_variable_flags`, `add_function`, `add_function_override`,
  `add_local_variable`, `add_event_dispatcher`, `add_interface`,
  `remove_graph_by_name`
- lifecycle: `compile_bp` (returns compiler errors/warnings), `fixup_redirectors`

Conventions: JSON in/out everywhere; failed lookups return the valid options
(pins/nodes/graphs/vars) so a caller can self-correct; node adds return the full
pin list so wiring needs no re-dump; every mutator is transactional (**Ctrl+Z works**
in the editor) and refuses to run during PIE.

Type strings: `bool int int64 float string name text vector vector2d rotator
transform linearcolor byte wildcard`, `object:/Script/Engine.Actor`, `class:...`,
`softobject:...`, `softclass:...`, `interface:...`,
`struct:/Script/CoreUObject.Vector`, `enum:/Game/...`, `array:<t>`, `set:<t>`,
`map:<k>|<v>`, suffix `&` = by-ref.

## Limits

- The editor must be running (headless alternative:
  `UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script=job.py`).
- Out of scope v1: timelines, AnimBP graphs, material graphs, UMG widget trees.
  Enhanced-input event nodes: `add_node_by_class`
  `/Script/BlueprintGraph.K2Node_EnhancedInputAction`, then set `input_action` via
  `unreal.find_object(<node path>)` + `reconstruct_node`.
- Engine APIs used are core Kismet/BlueprintGraph/UnrealEd — stable across 5.x,
  but only 5.6.1 is verified.

## Security

Remote python execution is arbitrary code execution by design. Keep the HTTP
server loopback-bound (`DefaultBindAddress=127.0.0.1`) and never expose the port.
