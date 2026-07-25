"""One-time (per running editor session) unlock of Remote Control python execution.
Run from the editor console:  py "<project>/Plugins/FableKit/Tools/open_gate.py"
Persisted for future sessions via Config/DefaultRemoteControl.ini; this only fixes
the CURRENT session, whose settings CDO was loaded before that ini edit existed.
"""
import unreal

cdo = unreal.load_object(None, "/Script/RemoteControlCommon.Default__RemoteControlSettings")
if cdo is None:
    raise RuntimeError("RemoteControlSettings CDO not found — is the RemoteControl plugin enabled?")

applied = None
last_err = None
for name in ("bEnableRemotePythonExecution", "enable_remote_python_execution"):
    try:
        cdo.set_editor_property(name, True)
        applied = name
        break
    except Exception as e:  # property-name resolution differs by wrapper state
        last_err = e
if applied is None:
    raise RuntimeError("could not set the property: %s" % last_err)

print("FableKit: remote python execution ENABLED this session (via '%s')" % applied)
