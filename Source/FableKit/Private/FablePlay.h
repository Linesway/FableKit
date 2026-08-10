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
 * The ban was never on driving the GAME. It was on driving the WINDOW. Enhanced Input already ships
 * the right primitive: InjectInputForAction feeds a specific player's input subsystem directly, so
 * it cannot steal focus, cannot land in the wrong panel, and still runs the real bindings, the real
 * triggers and the real gameplay code. Everything here is built on that.
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

	// ---------- input ----------

	/** Every UInputAction asset in the project, plus which mapping contexts the given player has
	 *  active. Use this instead of guessing an action path. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString ListInputActions(int32 PlayerIndex = 0);

	/** Inject ONE FRAME of an action. Axis actions take X/Y/Z; a button wants X=1.
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

	/** The full Slate hit-test path under desktop coords, TOP-MOST FIRST: widget type, debug name
	 *  and per-widget visibility. When clicks at a point vanish, the widget eating them is on this
	 *  list — no theory required. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Play")
	static FString HitTest(float X, float Y);
};
