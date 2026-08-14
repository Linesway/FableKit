#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FablePlay.generated.h"

/**
 * DRIVE AND OBSERVE A RUNNING PIE SESSION, from outside, without touching the machine.
 *
 * ============================================================================================
 * WHY THIS EXISTS. Testing gameplay from automation used to mean faking mouse and keyboard at the
 * OS level (SetCursorPos / mouse_event / SendKeys / SetForegroundWindow). That is banned here, and
 * it earned the ban: a click computed off a screenshot missed by the 1.5x DPI factor, landed in the
 * Content Browser instead of the viewport, and raised a "Delete Assets" modal — which blocks the
 * game thread and wedges this very bridge — while the user was at the machine doing something else.
 *
 * The ban was never on driving the GAME. It was on driving the WINDOW.
 *
 * ☠ THE RULE, AND IT IS NOT NEGOTIABLE: NOTHING IN THIS FILE MAY EVER CALL SetCursorPos, mouse_event,
 * SendKeys, SendInput OR SetForegroundWindow. Every entry point here enters INSIDE THE PROCESS, at
 * FSlateApplication (pointer / character events) or APlayerController::InputKey (keys, axes) — the
 * exact two doors the platform layer itself knocks on. Nothing moves the machine's cursor, nothing
 * steals focus, nothing can land in the wrong editor panel. If a later change "improves" any of this
 * into synthetic OS input, it re-buys a bug that has already been paid for once.
 *
 * ============================================================================================
 * ☠ AND THE ONE THAT COST 08-09 THROUGH 08-13: InjectInputForAction IS A SIDE CHANNEL.
 *
 * InjectAction / HoldAction / ReleaseAction below report ok:true and, for this project, do nothing.
 * Enhanced Input stamps an injected value for exactly one frame, consumes it only if a CURRENTLY
 * APPLIED mapping context maps that action, and — the fatal part — it never touches the key-state
 * table (UPlayerInput::KeyStateMap) that every trigger is actually evaluated against. So the value
 * lands somewhere nobody reads, and every symptom of that is silence.
 *
 * PressKey / ReleaseKey / TapKey / HoldKey / MouseAxis / GamepadAxis go in the OTHER door:
 * APlayerController::InputKey -> UPlayerInput::InputKey -> KeyStateMap -> ProcessInputStack. That is
 * the same call the viewport makes when a physical keyboard reports a key, so Enhanced Input cannot
 * tell the harness from hardware — there is no difference left to tell. Prefer them. InjectAction is
 * kept only as a fallback for an action no key maps at all.
 * ============================================================================================
 *
 * Conventions match UFableBP: every function returns a JSON string, mutators answer {"ok":true,...}
 * or {"ok":false,"error":"..."} with enough context to self-correct.
 *
 * DELIBERATELY GAME-AGNOSTIC. This module lives in a plugin, so it cannot reference the game's own
 * classes — and it should not want to. Observation is by REFLECTION (property names resolved at
 * runtime), which means it works on any project and keeps working when the game's headers move.
 *
 * A typical combat test, end to end:
 *      FablePlay.FindActors("/Game/Mobs/Slime/BP_NormalSlime.BP_NormalSlime_C", 0,0,0, 0)
 *      FablePlay.WatchProps("<slime name>", "Health", 3.0, 0.05)      # start sampling
 *      FablePlay.HoldAction("/Game/Blueprints/Input/Character/IA_AutoSwing", 0.1, 1,0,0, 0)
 *      ... wait ...
 *      FablePlay.PollWatch()                                          # health timeline back
 * which proves the hit landed, how much it took off, and when — with no window focus and no clicks.
 */
UCLASS()
class UFablePlay : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---------- session ----------

	/** Is a PIE session live, and what is in it? {ok, pie, world, players, pawn, netMode}. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString Status();

	/** Start a PIE session on the currently loaded level — the same request the toolbar Play
	 *  button issues, no OS input, no window focus. The bridge's answer to "test it yourself"
	 *  (user's standing order, 08-10). Async: PIE spins up over the next frames; poll Status().
	 *  {ok, alreadyRunning}. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString StartPIE();

	/** End the running PIE session (no-op when none). ALWAYS call when a test lane finishes —
	 *  an abandoned PIE wedges every later editor-scripting call. {ok, wasRunning}. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString StopPIE();

	// ---------- input: KEYS (the real door — prefer these over InjectAction) ----------
	/* Everything in this block ends up at APlayerController::InputKey, which is where the viewport
	 * delivers a physical keystroke. Key names are FKey names ("W", "SpaceBar", "LeftMouseButton",
	 * "Gamepad_FaceButton_Bottom"), with the obvious aliases accepted: lmb/rmb/mmb, space, esc,
	 * ctrl/shift/alt (left variants), enter. ListKeys() greps the whole table if you are unsure. */

	/** One IE_Pressed. The key STAYS DOWN — UPlayerInput maintains bDown until a release — so this is
	 *  a real hold, not a one-frame stamp. ☠ Always pair it with ReleaseKey (or use HoldKey/TapKey);
	 *  a key left down survives PIE restarts within the session and poisons every later test. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PressKey(const FString& Key, int32 PlayerIndex = 0);

	/** One IE_Released. Safe to call on a key that is not down. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ReleaseKey(const FString& Key, int32 PlayerIndex = 0);

	/** Press now, release on the NEXT tick. A Pressed trigger needs the transition, not the level, so
	 *  this — not a one-frame value — is what "tap Jump" actually means. Returns immediately. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString TapKey(const FString& Key, int32 PlayerIndex = 0);

	/** Hold for Seconds on the game-thread ticker, re-arming every frame (IE_Repeat for a digital key,
	 *  the analog value for an axis), and END WITH A REAL IE_Released — not a starved frame. Returns
	 *  immediately; the game ticks throughout. Holding the same key again replaces the running hold. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString HoldKey(const FString& Key, float Seconds = 1.0f, float Amount = 1.0f, int32 PlayerIndex = 0);

	/** End every hold this harness started with a real release (and zero every analog axis it moved).
	 *  bFlushAll additionally calls APlayerController::FlushPressedKeys, which releases keys nobody
	 *  here pressed. Call this at the end of a lane the way you close PIE. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ReleaseAllKeys(bool bFlushAll = false, int32 PlayerIndex = 0);

	/** Mouse LOOK: a delta on EKeys::MouseX / MouseY, which is what a mouse actually reports. No
	 *  current call can turn the camera. These axes carry UpdateAxisWithoutSamples, so the delta
	 *  self-clears next frame exactly as a real mouse's does — call it repeatedly to keep turning. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString MouseAxis(float DX, float DY, int32 PlayerIndex = 0);

	/** Analog stick / trigger via AmountDepressed — the only way the Steam Deck layout is testable at
	 *  all (the pad is five bindings today and nothing could drive it).
	 *
	 *  ☠ A GAMEPAD AXIS DOES NOT SELF-CLEAR. Unlike MouseX/Y it has no UpdateAxisWithoutSamples flag,
	 *  so a value written once stays deflected FOREVER — a stuck stick that looks like an AI bug. This
	 *  therefore always cleans up after itself: Seconds <= 0 zeroes the axis on the next tick, and
	 *  Seconds > 0 holds it on the ticker and then zeroes it. Pass Seconds for anything real. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString GamepadAxis(const FString& Key, float Amount = 1.0f, float Seconds = 0.0f,
		int32 PlayerIndex = 0);

	/** Every FKey whose name or display name contains Pattern (empty = the lot, capped at 200).
	 *  Reports analog/gamepad/mouse flags, so "which name does the Deck's left stick have" is a call
	 *  rather than a guess. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ListKeys(const FString& Pattern = TEXT(""));

	/** What the ENGINE believes about a key right now: down, time held, analog value. This is the
	 *  mutation test for everything above — press, read it down, release, read it up. If PressKey
	 *  ever silently stops working the way InjectAction did, this is the call that says so. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString KeyState(const FString& Key, int32 PlayerIndex = 0);

	/** What Enhanced Input believes about an ACTION right now: bound keys, current trigger event,
	 *  value, elapsed/triggered time. The other half of the proof — KeyState says the key arrived,
	 *  this says the action fired. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ActionState(const FString& ActionPath, int32 PlayerIndex = 0);

	// ---------- input: actions (fallback) ----------

	/** Every UInputAction asset in the project — and, for the given player, WHETHER EACH ONE IS
	 *  ACTUALLY BOUND by a currently applied mapping context, plus the keys that bind it and the list
	 *  of applied contexts. An unbound action is one nothing can drive: injecting it does nothing and
	 *  reports nothing, which is precisely the failure that went undiagnosed from 08-09. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ListInputActions(int32 PlayerIndex = 0);

	/** ⛔ FALLBACK ONLY — see the header block. Inject ONE FRAME of an action. Axis actions take X/Y/Z; a button wants X=1.
	 *  A single frame is a TAP for a Pressed trigger — for anything that measures how long you held
	 *  the button (a charge, a launcher follow), use HoldAction: an injection lasts exactly one
	 *  frame and Enhanced Input sees a release on the next one. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString InjectAction(const FString& ActionPath, float X = 1.0f, float Y = 0.0f, float Z = 0.0f,
		int32 PlayerIndex = 0);

	/** Hold an action for Seconds by re-injecting every frame, then stop. Returns immediately; the
	 *  hold runs on the game thread's ticker. Holding the same action again replaces the running
	 *  hold rather than stacking a second one. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString HoldAction(const FString& ActionPath, float Seconds, float X = 1.0f, float Y = 0.0f,
		float Z = 0.0f, int32 PlayerIndex = 0);

	/** End a running hold now (or all of them with an empty path). The action simply stops being
	 *  injected, which Enhanced Input reads as the release. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ReleaseAction(const FString& ActionPath = TEXT(""));

	/** Call a UFUNCTION on a named actor (or on the player's pawn / its CurrentWeapon) by reflection,
	 *  console-command style: "FunctionName Arg1 Arg2".
	 *
	 *  THIS IS THE RELIABLE DRIVER. Input injection depends on the project's mapping contexts being
	 *  applied and its actions being the ones actually bound — when any of that is off, an injection
	 *  reports success and does nothing. Calling the gameplay function is one hop further in and has
	 *  no such failure mode: if the function exists it runs, and if it does not you get told.
	 *
	 *  Target: an actor name, "player" for the possessed pawn, or "weapon" for the pawn's
	 *  PlayerState->CurrentWeapon (resolved by reflection, so it costs this plugin no game headers). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString CallFunction(const FString& Target, const FString& Call);

	// ---------- observation ----------

	/** The local player's pawn: transform, velocity, movement mode, and any extra properties named
	 *  in ExtraProps (comma-separated, resolved by reflection on the pawn). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PlayerSnapshot(const FString& ExtraProps = TEXT(""), int32 PlayerIndex = 0);

	/** What is playing on the player's skeletal mesh right now: montage name, SLOT, position, weight.
	 *  The slot is the point — a montage on a slot the running graph lacks advances silently with no
	 *  visible pose, and that failure is invisible from anywhere else. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString MontageState(int32 PlayerIndex = 0);

	/** Actors of a class, optionally within Radius of a point (Radius <= 0 = anywhere).
	 *  Returns name, location and distance, nearest first. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString FindActors(const FString& ClassPath, float X = 0.0f, float Y = 0.0f, float Z = 0.0f,
		float Radius = 0.0f);

	/** Read properties off a named actor by reflection. Props is comma-separated; components are
	 *  reachable as "ComponentName.PropertyName". */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString GetProps(const FString& ActorName, const FString& Props);

	/** Sample properties on one or more actors every IntervalSeconds for Seconds, into a buffer that
	 *  PollWatch drains. THIS is how a hit is proven: watch an enemy's health across a swing and the
	 *  timeline shows exactly how many times it dropped and by how much — which is the only way to
	 *  catch a flurry whose extra hits are being silently eaten by invincibility frames. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString WatchProps(const FString& ActorNames, const FString& Props, float Seconds = 3.0f,
		float IntervalSeconds = 0.05f);

	/** Drain the watch buffer: {ok, running, samples:[{t, actor, <prop>:<value>, ...}]}. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PollWatch();

	// ---------- scenario ----------

	/** Put the player somewhere. Sweeps by default so it cannot post you inside terrain. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString TeleportPlayer(float X, float Y, float Z, bool bSweep = true, int32 PlayerIndex = 0);

	/** Spawn an actor. Offsets are RELATIVE TO THE PLAYER when bRelativeToPlayer, which is what a
	 *  repeatable test wants — "a dummy 350uu in front of me" survives spawning anywhere in a world. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString SpawnActor(const FString& ClassPath, float X, float Y, float Z,
		bool bRelativeToPlayer = true, int32 PlayerIndex = 0);

	// ---------- pointer (Slate-level — real UI clicks without touching the machine) ----------
	/* CallFunction can drive a slot's HANDLER, but it cannot drive the PIPELINE: Slate's press
	 * routing, the platform's double-click synthesis, the press-claim/release pairing and the
	 * viewport → Enhanced Input fallback all sit ABOVE the handler — and the 08-10 fast-click bug
	 * lived entirely in that interplay, invisible to a synthetic handler call. These inject
	 * pointer events into FSlateApplication itself: everything below the OS runs exactly as for a
	 * physical mouse, and nothing here moves the machine's cursor or steals focus (the ban on
	 * driving the WINDOW stands untouched — this drives the APPLICATION). */

	/** Live widgets (PIE world only) whose NAME or CLASS contains Pattern: name, class, and the
	 *  absolute desktop-space rect PointerClick wants. Index >= 0 narrows to one match;
	 *  -1 lists up to 40. Zero-size rects are widgets not currently on screen. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString WidgetRect(const FString& Pattern, int32 Index = -1);

	/** One full mouse CLICK (move + press + release) at desktop-space coords. bDouble delivers the
	 *  press as the platform DOUBLE-CLICK event — what the OS turns the second press of a fast
	 *  pair into, and therefore the half of a rapid click a handler call can never reproduce. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PointerClick(float X, float Y, bool bRight = false, bool bDouble = false);

	/** Hammer: Count clicks at ClicksPerSecond on the game-thread ticker, with the platform's
	 *  double-click synthesis reproduced faithfully — any press within the double-click time of
	 *  the previous press at the same spot goes out as a DoubleClick event, exactly like a
	 *  hammering human. Returns immediately; assert state between PollWatch-style reads. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PointerHammer(float X, float Y, int32 Count = 6, float ClicksPerSecond = 8.0f,
		bool bRight = false);

	/** Move the Slate pointer (drives hover enter/leave chains — tooltips, hover cues). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PointerMove(float X, float Y);

	/* REST the pointer somewhere for Seconds, re-injecting every frame. Returns immediately.
	 *
	 * PointerMove sets hover once and the next frame can take it straight back, because the
	 * platform re-derives the cursor from the real mouse. Anything gated on the cursor having
	 * STAYED — a hold-to-reveal, a tooltip delay, a hover-cue gate — cannot arm from a single
	 * move, and the caller cannot hold it by waiting either: a bridge call owns the game thread
	 * for its duration, so nothing ticks while you sleep inside one.
	 *
	 * Drive a hold like this: PointerRest(x, y, 2.0), return, sleep in the SHELL, then read the
	 * state in a second call while the rest is still running. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PointerRest(float X, float Y, float Seconds = 2.0f);

	/** The full Slate hit-test path under desktop coords, TOP-MOST FIRST: widget type, debug name
	 *  and per-widget visibility. When clicks at a point vanish, the widget eating them is on this
	 *  list — no theory required. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString HitTest(float X, float Y);

	/** Press at (X1,Y1), travel to (X2,Y2) over Seconds in Steps moves with the button HELD, release.
	 *  The intermediate moves are the point: Slate only raises OnDragDetected once the pointer has
	 *  moved the drag-trigger distance WHILE down, so a press+release pair — or a single teleporting
	 *  move — can never start a drag. INVENTORY DRAG-DROP HAS NEVER BEEN MACHINE-TESTED; this is the
	 *  call that tests it. Runs on the ticker, returns immediately, PollSequence-free (poll widgets). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString Drag(float X1, float Y1, float X2, float Y2, float Seconds = 0.35f,
		int32 Steps = 12, bool bRight = false);

	/** Mouse wheel at desktop coords (negative X/Y = wherever the Slate pointer already is). Drives
	 *  hotbar cycling, map zoom and the weapon-mode wheel. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ScrollWheel(float Delta, float X = -1.0f, float Y = -1.0f);

	/** Type a string into whatever Slate widget holds keyboard focus: per character, a key down, the
	 *  character event, and a key up — the three events a real keystroke produces, in that order.
	 *  CharsPerSecond <= 0 sends the whole string this frame; otherwise it types on the ticker and
	 *  returns immediately (a text box that debounces per keystroke needs the real cadence). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString TypeText(const FString& Text, float CharsPerSecond = 0.0f);

	// ---------- sequences ----------

	/** ☠☠ A BRIDGE CALL OWNS THE GAME THREAD, so a multi-step playthrough CANNOT be one script with
	 *  sleeps in it — nothing ticks while you wait. This runs the whole script on the game-thread
	 *  ticker and returns at once; PollSequence() reads the transcript afterwards.
	 *
	 *  Steps are separated by '|'. Everything is case-insensitive.
	 *      W 2.0              hold key W for 2 s        (a bare number after a key = hold seconds)
	 *      W                  tap W                     (equivalently "W TAP")
	 *      LMB DOWN / LMB UP  press / release, unpaired
	 *      LOOK 90 0          mouse delta dx dy         (also "MOUSE")
	 *      AXIS Gamepad_LeftX -1.0 1.5   analog axis, amount then seconds
	 *      WAIT 0.5           do nothing for 0.5 s
	 *      CLICK 640 360      Slate click  (CLICK R x y for right, DCLICK for double)
	 *      MOVE 640 360       Slate pointer move
	 *      SCROLL -2          mouse wheel
	 *      TYPE hello there   type the rest of the step as text
	 *      CALL player Func A B    CallFunction on a target — the reliable driver, inline
	 *      SAY anything       write a marker into the transcript
	 *
	 *  "W 2.0 | LOOK 90 0 | LMB TAP | WAIT 0.5 | E TAP" is one bridge call and one playthrough beat.
	 *  ☠ The runner releases every key it pressed when it ends, aborts, or is replaced. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString RunSequence(const FString& Script, int32 PlayerIndex = 0);

	/** {ok, running, step, total, elapsed, transcript:[{t, step, result}]}. Drains the transcript, so
	 *  repeated polls stream rather than repeat — the same contract as PollWatch. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString PollSequence();

	/** Abort the running sequence NOW, releasing anything it was holding. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString StopSequence();

	// ---------- capture ----------

	/** A PNG of what the player is looking at, WITH THE EDITOR BACKGROUNDED OR MINIMISED.
	 *
	 *  ☠ take_high_res_screenshot cannot do this twice over: it needs the editor window foregrounded,
	 *  which is the one thing the input ban forbids, AND it photographs the scene without compositing
	 *  UMG — so no widget can ever be judged from one. This renders instead of photographing: a
	 *  USceneCaptureComponent2D placed at the player camera draws the world into a render target (no
	 *  window, no focus, no swap chain involved), and then — when bIncludeUI — the game's live Slate
	 *  overlay is drawn ON TOP of that same target with the clear disabled, so the HUD composites in
	 *  for free with no pixel readback.
	 *
	 *  FOV <= 0 takes the player camera's own. OutPngPath may be absolute or relative to the project.
	 *  Returns {ok, png, width, height, bytes, ui} — ☠ judge it by BYTES, not by ok: an unwritable
	 *  path leaves a successful capture with nothing on disk. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString CaptureScreen(const FString& OutPngPath, int32 Width = 1280, int32 Height = 720,
		bool bIncludeUI = true, float FOV = 0.0f, int32 PlayerIndex = 0);
};
