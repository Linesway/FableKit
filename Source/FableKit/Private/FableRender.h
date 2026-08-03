// Copyright © 2026 Linesway All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FableRender.generated.h"

/**
 * Offscreen rendering surface for external automation (unreal.FableRender.* over the same
 * Remote-Control -> Python bridge as UFableBP / UFableNiagara).
 *
 * WHY THIS EXISTS: every UI change on this project has been made blind. unreal.WidgetRenderer and
 * unreal.WidgetBlueprintLibrary are not exposed to Python, so there was no way to LOOK at a Widget
 * Blueprint without opening the editor by hand — bad layouts shipped and were only caught in playtest
 * screenshots. This renders a WBP straight to a PNG you can open.
 *
 * ONE-LINER (from a shell, editor running):
 *   python Plugins/FableKit/Tools/uexec.py -c "import fable; fable.render_widget('/Game/UI/WBP_Thing', r'C:/tmp/thing.png', 1920, 1080)"
 * then just Read the PNG.
 *
 * WHAT YOU ARE LOOKING AT — read this before filing a bug against the render:
 *  - BY DEFAULT the widget is constructed with EWidgetDesignFlags::Designing, exactly like the
 *    editor's own asset-thumbnail renderer. UWidget::OnWidgetRebuilt takes its IsDesignTime() branch,
 *    so NativeConstruct and NativeOnInitialized NEVER run. A widget that dereferences a
 *    PlayerController or PlayerState in NativeConstruct therefore cannot take the editor down here —
 *    but it also means anything those functions would have filled in is absent.
 *  - So: text/icons/counts driven from live game state render as their DESIGN-TIME defaults (empty
 *    strings, placeholder numbers, default brushes). That is expected. The point of this is LAYOUT AND
 *    STYLING — spacing, alignment, colour, hierarchy, font sizes, overflow — not live data.
 *  - PreConstruct DOES run by default (option "pre_construct", matching the engine thumbnail path)
 *    because it is where designers put layout-affecting logic. It is the one piece of the widget's own
 *    code that executes, so it is also the one place a null-deref can still bite. If a specific widget
 *    kills the editor, re-run it with {"pre_construct": false}.
 *  - {"live": true} OPTS OUT of the design-time safety and runs the REAL lifecycle:
 *    NativeOnInitialized at Initialize, then NativePreConstruct + NativeConstruct from TakeWidget.
 *    This is THE way to render/measure the screens that COMPOSE their layout in code (the chest, the
 *    NPC menu, the achievements chrome, the Index of the Eye) — a Designing render of those shows
 *    only the raw authored asset. Two caveats: (1) game code runs with NO owning player, so only use
 *    it on widgets that null-guard GetOwningPlayer()/GetPS()/GetPC() — a hard deref CAN take the
 *    editor down; (2) child widgets the screen spawns via CreateWidget(GetOwningPlayer(), ...) get a
 *    null owner and are skipped, so item CELLS inside composed grids may be absent — judge the
 *    layout, not the cell contents.
 *
 * Conventions (same as UFableBP / UFableNiagara):
 *  - Returns JSON. Failures are {"ok":false,"error":...} and carry enough context to self-correct.
 *  - Paths accept "/Game/UI/WBP_X" or "/Game/UI/WBP_X.WBP_X"; a class path ("...WBP_X_C", or a native
 *    "/Script/Deliria.SSomeWidget") also works.
 *  - Read-only: it never touches, dirties or saves an asset, so unlike the mutators it does not refuse
 *    to run during PIE.
 */
UCLASS()
class UFableRender : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Render a Widget Blueprint offscreen and write it to a PNG.
	 *
	 * @param BlueprintPath  Widget Blueprint asset path (or a UUserWidget class path).
	 * @param OutPngPath     Absolute output file. Missing directories are created; a missing ".png"
	 *                       extension is appended (the extension decides the format downstream).
	 * @param Width,Height   Pixel size. Pass 0 for either to auto-size: the widget's designer canvas
	 *                       size first, then its measured desired size. A root Canvas Panel measures
	 *                       0x0, so for most full-screen HUDs you want to pass explicit numbers.
	 * @param OptionsJson    Optional, "" or "{}" for defaults:
	 *                         "background"    – what to fill behind the widget so a transparent panel is
	 *                                           actually visible. Accepts a name
	 *                                           (transparent|black|white|dark|light|grey), "#RRGGBB",
	 *                                           "#RRGGBBAA", linear "R,G,B[,A]" floats, or a UE struct
	 *                                           literal "(R=..,G=..,B=..,A=..)".
	 *                                           Default "dark" = (0.02,0.02,0.025,1).
	 *                         "scale"         – DPI / layout scale, 0..8. Default 1.0. The image stays
	 *                                           Width x Height; the widget is laid out as if the
	 *                                           application DPI scale were this.
	 *                         "pre_construct" – bool, default true. false skips NativePreConstruct (see
	 *                                           the class comment) — the escape hatch for a widget whose
	 *                                           PreConstruct crashes.
	 *                         "live"          – bool, default false. true runs the REAL widget lifecycle
	 *                                           (NativeOnInitialized + NativeConstruct) so runtime-composed
	 *                                           screens render as they ship. Null-guarded widgets only —
	 *                                           see the class comment for the exact trade.
	 *                         "calls"         – array of ["FunctionName", arg, ...] invoked on the widget
	 *                                           AFTER construction, BEFORE the draw — renders a specific
	 *                                           STATE (a live search filter, a selected category, a page).
	 *                                           Args fill parameters positionally; scalar/string/text
	 *                                           parameter types only, and a failed call fails the render.
	 *                                           e.g. {"live":true,"calls":[["SetSearchFilter","fps"]]}
	 *
	 * @return JSON. On success:
	 *   {"ok":true,"widget":"/Game/...WBP_X","class":"/Game/...WBP_X_C","png":"C:/tmp/x.png",
	 *    "width":1920,"height":1080,"scale":1.0,"size_source":"explicit|design_time|desired",
	 *    "desired_w":..,"desired_h":..,"background":"(R=..,G=..,B=..,A=..)",
	 *    "pre_construct":true,"transient_world":false,"bytes":123456}
	 * On failure: {"ok":false,"error":"..."} (plus the same context fields when the failure happened
	 * after the widget was built, e.g. an unwritable output path).
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Render")
	static FString RenderWidget(const FString& BlueprintPath, const FString& OutPngPath, int32 Width, int32 Height, const FString& OptionsJson);

	/**
	 * Render a StaticMesh offscreen from an arbitrary camera angle and write it to a PNG.
	 *
	 * WHY: same blind-authoring problem as RenderWidget, one layer down. Procedural meshes and
	 * emissive materials built over this bridge were being shipped on faith — you cannot judge a
	 * silhouette from a triangle count. This renders the asset into a throwaway FPreviewScene (its
	 * own world: the open level is never spawned into, never dirtied) and captures it.
	 *
	 * TWO PASSES ARE THE POINT. Render once as authored to judge the EFFECT, then again with
	 * {"material":"/Engine/EngineMaterials/DefaultMaterial"} to judge the GEOMETRY — a lit grey pass
	 * shows curvature that an unlit additive material hides completely.
	 *
	 * @param AssetPath    StaticMesh path ("/Game/X/SM_Y" or "/Game/X/SM_Y.SM_Y").
	 * @param OutPngPath   Absolute output file; missing dirs are created, ".png" appended if absent.
	 * @param Width,Height Pixel size; <= 0 falls back to 1024x1024.
	 * @param OptionsJson  Optional, "" or "{}" for defaults:
	 *                       "yaw"      – camera orbit, deg. 0 looks down +X. Default 35.
	 *                       "pitch"    – camera elevation, deg; negative looks DOWN. Default -20.
	 *                       "roll"     – camera roll, deg. Default 0.
	 *                       "distance" – uu from the bounds centre. Default 0 = auto-frame.
	 *                       "fov"      – horizontal FOV, deg. Default 45.
	 *                       "material" – override material/instance on every slot (see above).
	 *                       "light_yaw"/"light_pitch" – key light direction. Default 45 / -35.
	 *                       "exposure" – fixed EV100. Default 1.0; raise to dim a blown-out emissive.
	 *
	 * @return JSON: {"ok":true,"mesh":"...","png":"...","width":..,"height":..,"yaw":..,"pitch":..,
	 *          "distance":..,"fov":..,"bounds_radius":..,"material_override":"..","bytes":..}
	 *          On failure {"ok":false,"error":"..."}.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Render")
	static FString RenderMesh(const FString& AssetPath, const FString& OutPngPath, int32 Width, int32 Height, const FString& OptionsJson);

	/**
	 * MEASURE a Widget Blueprint: build it exactly like RenderWidget does, run a Slate prepass, then
	 * report the DESIRED SIZE of the root and of every widget in its tree.
	 *
	 * WHY THIS EXISTS: a render shows you that a panel is too tall; it does not tell you WHICH child
	 * is forcing the height. Working that out by setting properties and re-rendering costs a round
	 * trip per guess and can easily be wrong several times in a row. This answers it directly — sort
	 * the rows by height and the culprit is at the top.
	 *
	 * Same construction path and therefore the same caveats as RenderWidget: Designing flags by
	 * default, so NativeConstruct never runs and anything filled in at runtime is absent (PreConstruct
	 * DOES run) — and the same {"live": true} opt-out for measuring a runtime-composed tree.
	 *
	 * @param BlueprintPath Widget Blueprint asset path (or a UUserWidget class path).
	 * @param Width,Height  Layout space to measure in. <= 0 measures unconstrained, which is what you
	 *                      want when asking "how big does this WANT to be".
	 * @param OptionsJson   "" for defaults; accepts "scale", "pre_construct" and "live" like RenderWidget.
	 *
	 * @return JSON: {"ok":true,"widget":..,"root_w":..,"root_h":..,"scale":..,
	 *          "widgets":[{"name":..,"class":..,"parent":..,"desired_w":..,"desired_h":..},...]}
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Render")
	static FString MeasureWidget(const FString& BlueprintPath, int32 Width, int32 Height, const FString& OptionsJson);
};
