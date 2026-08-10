using UnrealBuildTool;

public class FableKit : ModuleRules
{
	public FableKit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"BlueprintGraph",
			"AssetTools",
			"AssetRegistry",
			"UMG",
			"UMGEditor",
			// Offscreen widget rendering (UFableRender): FWidgetRenderer itself lives in UMG (above), but
			// the draw + readback needs FlushRenderingCommands and BeginCleanup, which are RenderCore.
			// Both arrive transitively through Engine today — declared explicitly so a future engine
			// header shuffle can't quietly break this module. (No RHI needed: EPixelFormat is in Core.)
			"RenderCore",
			// Niagara authoring (UFableNiagara): runtime module for the asset types, editor module for
			// AddEmitterToSystem / KillSystemInstances / the system factory.
			"Niagara",
			"NiagaraEditor",
			// Anim graph authoring (LinkCachedPose): UAnimGraphNode_UseCachedPose /
			// UAnimGraphNode_SaveCachedPose live here. Needed because the Use node's link to its Save
			// node is a bare UPROPERTY with no editor binding, so Python cannot set it — see
			// UFableBP::LinkCachedPose.
			"AnimGraph",
			// Gameplay driving (UFablePlay): InjectInputForAction on the player's
			// UEnhancedInputLocalPlayerSubsystem. This is the whole reason testing does not need
			// synthetic OS mouse/keyboard — it feeds one player's input subsystem directly, so it
			// cannot steal window focus or land a click in the wrong editor panel.
			"EnhancedInput",
			// FAnimNode_Slot::StaticStruct, for the "does the anim graph actually HAVE this slot"
			// test in MontageState. A montage on a slot the graph lacks plays silently with no
			// visible pose, and that is invisible from every other vantage point.
			"AnimGraphRuntime",
			// EKeys for the Slate-level pointer injection (UFablePlay::PointerClick/Hammer) —
			// arrives transitively today, declared so a header shuffle can't quietly break it.
			"InputCore",
		});
	}
}
