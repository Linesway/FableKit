// Copyright © 2026 Linesway All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FableAsset.generated.h"

/**
 * Component-tree (SCS) and skeleton/socket authoring for external automation
 * (unreal.FableAsset.* over the same Remote-Control -> Python bridge as UFableBP / UFableAnim).
 *
 * WHY THIS EXISTS — the PROTECTED-PROPERTY WALL.
 *
 * Python reaches UPROPERTYs through PropertyAccessUtil::CanSetPropertyValue, which refuses anything
 * without CPF_Edit | CPF_BlueprintVisible. Four of the properties that decide how a Blueprint's
 * components are ASSEMBLED are bare `UPROPERTY()`, so they are unreadable AND unwritable from
 * Python — you can see a component exists and never learn, or change, where it hangs:
 *
 *   USCS_Node::AttachToName                  (SCS_Node.h:44)  the SOCKET a component sits on
 *   USCS_Node::ParentComponentOrVariableName (SCS_Node.h:48)  what it hangs off
 *   USCS_Node::bIsParentComponentNative      (SCS_Node.h:57)  whether that parent is native or SCS
 *   UBlueprint::ParentClass                  (Blueprint.h:411) the reparent target
 *
 * The concrete failure that motivated this: a banner mesh on a standard bearer could be created from
 * Python but never attached to `hand_l`, so it floated at the actor origin. The morale system it
 * belongs to keys off the CLASS NAME and was already live — only the visual was unreachable.
 *
 * Sockets are the same story from the other side. USkeleton::Sockets is a bare UPROPERTY holding
 * USkeletalMeshSocket objects; the socket's own fields are EditAnywhere (so Python can edit one that
 * already exists) but CREATING one and getting it into the array cannot be done from outside C++.
 * A socket that does not exist makes every attach to it silently fall back to the component origin —
 * ☠ AttachToName naming an unknown socket does NOT error, it attaches to the bone-less root.
 *
 * Conventions (same as UFableBP / UFableAnim):
 *  - Every function returns JSON; failures are {"ok":false,"error":...} and list the valid options
 *    where that helps a caller self-correct.
 *  - Paths accept "/Game/A/Foo" or "/Game/A/Foo.Foo".
 *  - Vectors/rotators accept "1,2,3" OR UE's own "X=1 Y=2 Z=3" / "P=0 Y=90 R=0" text. ☠ the bare
 *    3-number form for a ROTATOR is PITCH,YAW,ROLL — matching FRotator's own text order, NOT the
 *    (roll,pitch,yaw) that unreal.Rotator's positional constructor takes in Python.
 *  - Mutators refuse during PIE, are transactional (Ctrl+Z), and mark the package dirty. They never
 *    save — call save_asset yourself once the result reads right, and verify the mtime moved.
 */
UCLASS()
class UFableAsset : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/* ------------------------------------------------ component tree (SCS) : read ------------ */

	/** Every component on a Blueprint — the SCS nodes it declares, the ones it inherits, and the
	 *  NATIVE ones on the CDO — each with the parent + socket it is attached to and its relative
	 *  transform. This is the discovery call: the `parents` list it returns is exactly the set of
	 *  names ScsSetAttach/ScsAddComponent will accept. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsList(const FString& BlueprintPath);

	/* ------------------------------------------------ component tree (SCS) : author ---------- */

	/** Attach an existing component to ParentName, on SocketName.
	 *
	 *  THE call Python cannot make. ParentName "" keeps the current parent and changes only the
	 *  socket; SocketName "" clears the socket back to the parent's origin. The parent may be an SCS
	 *  component of this Blueprint, one inherited from a parent Blueprint, or a native component on
	 *  the CDO (e.g. `CharacterMesh0`) — all three are handled, and they need different bookkeeping,
	 *  which is most of why this is not a one-line property write.
	 *
	 *  ☠ VALIDATES THE SOCKET. If the resolved parent is a skeletal/static mesh component whose mesh
	 *  is set, a SocketName that the mesh does not have is REFUSED with the list of ones it does —
	 *  because the engine would otherwise accept it silently and attach to the origin. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsSetAttach(const FString& BlueprintPath, const FString& ComponentName,
	                            const FString& ParentName, const FString& SocketName);

	/** Create a component on the Blueprint and attach it. ParentName "" makes it a root node.
	 *  Same socket validation as ScsSetAttach. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsAddComponent(const FString& BlueprintPath, const FString& ComponentClassPath,
	                               const FString& ComponentName, const FString& ParentName,
	                               const FString& SocketName);

	/** Delete an SCS component, promoting its children to its parent. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsRemoveComponent(const FString& BlueprintPath, const FString& ComponentName);

	/** Relative location / rotation / scale on an SCS scene component. Any of the three may be ""
	 *  to leave it alone. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsSetTransform(const FString& BlueprintPath, const FString& ComponentName,
	                               const FString& Location, const FString& Rotation, const FString& Scale);

	/** Set properties on an SCS component TEMPLATE from JSON {"Prop":"UE text value",...}. Values go
	 *  through FProperty::ImportText, so T3D struct literals and asset paths work verbatim. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsSetProps(const FString& BlueprintPath, const FString& ComponentName, const FString& PropsJson);

	/** Read them back in the same text form ScsSetProps takes. PropsCsv "" dumps everything. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString ScsGetProps(const FString& BlueprintPath, const FString& ComponentName, const FString& PropsCsv);

	/** Reparent a Blueprint. ☠ Value-diff every leaf afterwards — a reparent DELTA-WIPES any
	 *  property whose new inherited default happens to equal the child's authored value. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SetParentClass(const FString& BlueprintPath, const FString& ParentClassPath);

	/* ------------------------------------------------ skeleton / mesh sockets ---------------- */

	/** Skeleton, bone count, and every socket (skeleton-level and mesh-only) with its bone and
	 *  relative transform. Accepts a Skeleton, a SkeletalMesh, or a StaticMesh. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SkelInfo(const FString& AssetPath);

	/** Bone names, optionally filtered by substring. The check before naming a socket's bone —
	 *  a socket on a bone that does not exist is created happily and never moves. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SkelListBones(const FString& AssetPath, const FString& Contains);

	/** Create a socket on a Skeleton (shared by every mesh using it) or, with bMeshOnly, on one
	 *  SkeletalMesh. Refuses if the bone does not exist, or the socket name is taken. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SkelAddSocket(const FString& AssetPath, const FString& SocketName, const FString& BoneName,
	                             const FString& Location, const FString& Rotation, const FString& Scale,
	                             bool bMeshOnly = false);

	/** Move/retarget an existing socket. Any argument may be "" to leave it alone. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SkelSetSocket(const FString& AssetPath, const FString& SocketName, const FString& BoneName,
	                             const FString& Location, const FString& Rotation, const FString& Scale);

	/** Delete a socket. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SkelRemoveSocket(const FString& AssetPath, const FString& SocketName);

	/** A socket's (or bone's) COMPONENT-SPACE transform in the skeleton's REFERENCE POSE.
	 *
	 *  ☠☠ THE REASON THIS EXISTS, AND IT COST A ROUND TRIP. `GetSocketTransform` on a live component
	 *  reports the CURRENT ANIMATED pose. Deriving an attachment's relative rotation from that bakes
	 *  in whatever frame the character happened to be on — the banner it was derived from stood
	 *  perfectly upright in the idle pose used to measure it, and lay flat on the next spawn because
	 *  that one froze mid-stride.
	 *
	 *  An attachment must be authored against the REFERENCE pose, which is the pose the Blueprint
	 *  viewport shows and the only one that is a property of the asset rather than of a moment.
	 *  Animation then moves the attachment as it should. Nothing else in the Python surface can reach
	 *  this: the reference skeleton is C++-only, and the transform needs the whole parent chain
	 *  accumulated to component space. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Asset")
	static FString SkelRefPoseTransform(const FString& AssetPath, const FString& SocketOrBoneName);
};
