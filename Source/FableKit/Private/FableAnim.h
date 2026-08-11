// Copyright © 2026 Linesway All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FableAnim.generated.h"

/**
 * Animation authoring surface for external automation (unreal.FableAnim.* over the same
 * Remote-Control -> Python bridge as UFableBP / UFableNiagara).
 *
 * WHY THIS EXISTS — the two things Python fundamentally cannot do:
 *
 *  1. ☠ `UAnimMontage::SlotAnimTracks` is a BARE `UPROPERTY()` (AnimMontage.h:688) with no
 *     EditAnywhere/BlueprintReadWrite, so it is invisible to `set_editor_property`. That is why
 *     `AnimMontageFactory` montages are permanently stuck on `DefaultSlot` from Python — and the
 *     SLOT is what decides whether a montage animates the full body, only the upper body above
 *     spine_03, or nothing at all in the air. A montage on the wrong slot plays silently with no
 *     visible pose. Authoring montages without being able to choose the slot is authoring blind.
 *
 *  2. A slot name that the SKELETON has never heard of silently does nothing. `RegisterSlotNode`
 *     is C++-only too, so SetMontageSlot registers the name when it is new.
 *
 * `bEnableRootMotion` on the other hand IS `EditAnywhere` (AnimSequence.h:319) and Python can
 * already set it — it is wrapped here anyway so root motion and its slot can be changed in one
 * call, and because SetRootMotion refuses to touch a sequence that other assets reference unless
 * you say so (flipping root motion on a shared clip silently changes every weapon using it).
 *
 * Conventions (same as UFableBP / UFableNiagara):
 *  - Every function returns JSON; failures are {"ok":false,"error":...} and list valid options
 *    where that helps a caller self-correct.
 *  - Paths accept "/Game/Anim/Foo" or "/Game/Anim/Foo.Foo".
 *  - Mutators refuse during PIE, are transactional (Ctrl+Z), and mark the package dirty. They
 *    never save — call save_asset yourself once the result reads right.
 */
UCLASS()
class UFableAnim : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/* ---------- read ---------- */

	/** Type, skeleton, play length, rate scale, root motion, and (montages) every slot track with
	 *  its segments. This is the discovery call — run it before assigning a montage anywhere. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString Info(const FString& AnimPath);

	/** Every slot name the skeleton knows, grouped. These are the only names SetMontageSlot should
	 *  be given without deliberately minting a new one. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString ListSkeletonSlots(const FString& SkeletonPath);

	/** Survey montages/sequences under a content path: name, slot, root motion, length. The bulk
	 *  question "which of these 910 anims are upper-body swings with no root motion" is one call. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString Survey(const FString& PackagePath, const FString& NameContains);

	/** Who references this asset. The safety check before flipping root motion IN PLACE: an unused
	 *  clip can be edited directly, a shared one must be duplicated first. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString ListReferencers(const FString& AssetPath);

	/* ---------- author ---------- */

	/** Point a montage's slot track(s) at SlotName, registering it on the skeleton if new.
	 *  TrackIndex -1 = every track. THE call Python cannot make. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString SetMontageSlot(const FString& MontagePath, const FString& SlotName, int32 TrackIndex = -1);

	/** Create a montage from one sequence ON A CHOSEN SLOT, with an optional rate scale.
	 *  Equivalent to AnimMontageFactory + SetMontageSlot, minus the trip through DefaultSlot. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString CreateMontage(const FString& SequencePath, const FString& DestPath,
	                             const FString& SlotName, float RateScale = 1.0f);

	/** Enable/disable root motion on a sequence. Refuses when other assets reference it unless
	 *  bAllowShared — see the class comment. RootLock: "" leaves it, else RefPose/AnimFirstFrame/Zero. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString SetRootMotion(const FString& SequencePath, bool bEnable, bool bAllowShared = false,
	                             const FString& RootLock = TEXT(""));

	/** Duplicate an animation asset (the "copy it, then flip root motion on the copy" workflow). */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString DuplicateAnim(const FString& SrcPath, const FString& DestPath);

	/** Montage playback dials in one call: rate scale, blend in/out seconds, auto blend out. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Anim")
	static FString SetMontageTiming(const FString& MontagePath, float RateScale,
	                                float BlendInSeconds = -1.0f, float BlendOutSeconds = -1.0f);
};
