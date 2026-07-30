#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FableNiagara.generated.h"

/**
 * Niagara authoring surface for external automation (unreal.FableNiagara.* over the same
 * Remote-Control -> Python bridge as UFableBP).
 *
 * SCOPE — deliberately "compose and configure", not "write module stacks".
 *   Niagara's per-module stack (Particle Spawn/Update rows and their inputs) lives behind
 *   editor-UI view models (FNiagaraSystemViewModel / UNiagaraStackEntry) that are unstable across
 *   engine versions and assume a live editor window. Everything here instead uses the *asset* API,
 *   which is stable: enumerate/copy whole EMITTERS between systems, drive exposed User parameters,
 *   and set renderer properties. That covers the practical workflow — build an effect by assembling
 *   emitters that already look right, then retune them — without pretending to a fragile surface.
 *   If you need a brand-new module stack, author one emitter by hand and clone it from here forever.
 *
 * Conventions (same as UFableBP):
 *  - Every function returns JSON. Failures are {"ok":false,"error":...} and, where useful, list the
 *    valid options (emitter names, parameter names) so a caller can self-correct.
 *  - SystemPath accepts "/Game/FX/NS_Thing" or "/Game/FX/NS_Thing.NS_Thing".
 *  - Emitters are addressed by their handle name (as returned by Info).
 *  - Mutators: refuse during PIE, kill live system instances first (editing a system that has
 *    spawned components in a world is the classic Niagara editor crash), are transactional, and
 *    mark the package dirty. They never save — call unreal.EditorAssetLibrary.save_asset yourself
 *    once the result looks right.
 */
UCLASS()
class UFableNiagara : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/* ---------- read ---------- */

	/** Full structure: emitters (name/enabled/renderers) + exposed User parameters with values. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString Info(const FString& SystemPath);

	/** Exposed User.* parameters only: name, type, and current value as text. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString ListUserParams(const FString& SystemPath);

	/** Every property on one renderer, with current values — the discovery step before SetRendererProps. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString DumpRenderer(const FString& SystemPath, const FString& EmitterName, int32 RendererIndex);

	/* ---------- configure (non-structural) ---------- */

	/** Set an exposed User parameter's default. Value is UE text for the parameter's type; for
	 *  single-field Niagara scalars you may pass the bare value ("2.5") instead of "(Value=2.5)". */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString SetUserParam(const FString& SystemPath, const FString& ParamName, const FString& Value);

	/** Set properties on a renderer from JSON {"Prop":"UE text value"} — same ImportText contract as
	 *  the widget-tree setters, so struct literals (materials, mesh arrays, colours) work verbatim. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString SetRendererProps(const FString& SystemPath, const FString& EmitterName, int32 RendererIndex, const FString& PropsJson);

	/** Enable/disable one emitter in the system (the cheap way to A/B a layer). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString SetEmitterEnabled(const FString& SystemPath, const FString& EmitterName, bool bEnabled);

	/* ---------- compose (structural) ---------- */

	/** Create an empty NiagaraSystem asset. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString CreateSystem(const FString& PackagePath, const FString& AssetName);

	/** Copy an emitter INTO a system. Source may be a standalone UNiagaraEmitter asset, or another
	 *  system (then SourceEmitterName picks which of its emitters). Always copies — the donor is
	 *  never referenced or modified. This is the primary "authoring" verb. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString AddEmitter(const FString& SystemPath, const FString& SourcePath, const FString& SourceEmitterName, const FString& NewEmitterName);

	/** Remove an emitter from a system by handle name. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString RemoveEmitter(const FString& SystemPath, const FString& EmitterName);

	/** Duplicate a whole system to a new asset (the safe way to iterate on a shipped effect). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString DuplicateSystem(const FString& SystemPath, const FString& DestPackagePath, const FString& DestAssetName);

	/* ---------- lifecycle ---------- */

	/** Request a compile. Returns {"ok","ready","outstanding"}.
	 *
	 *  IT CANNOT WAIT, AND NEITHER CAN YOU — not from inside one bridge call. Niagara queues compile
	 *  work onto the editor TICK (UNiagaraSystem::IsReadyToRunInternal bails while
	 *  EmitterHandles.Num() != EmitterCompiledData.Num(), and the engine's own comment says the new
	 *  compile isn't registered "until the next tick"). A bridge call OWNS the game thread for its whole
	 *  duration, so no tick can happen inside it — looping, sleeping or re-polling here all stay false.
	 *  Measured: ready=false in the call that edits, ready=true in the very next call.
	 *  => Call this once, let the call return, then check IsReady in a SEPARATE call before saving. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString CompileSystem(const FString& SystemPath);

	/** Read-only readiness probe — cheap, no compile request, no PIE guard. Use this to poll after
	 *  CompileSystem (in a later call) instead of re-requesting a compile each time. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Niagara")
	static FString IsReady(const FString& SystemPath);
};
