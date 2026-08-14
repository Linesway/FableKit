// Copyright © 2026 Linesway All rights reserved.

#include "FableAsset.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Animation/Skeleton.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Editor.h"

#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

#include "FableJson.h"

using namespace FableKitPrivate;

// Everything file-private lives here with an `Fa` prefix. ☠ UE unity builds concatenate several
// .cpp files into ONE translation unit, so a helper named the same as one in FableBP.cpp becomes an
// ambiguous call even though each file compiles alone — see FableJson.h's header comment.
namespace FableAssetPrivate
{

static void FaSetStrArray(const FJObj& Obj, const FString& Key, const TArray<FString>& List)
{
	TArray<FJVal> Vals;
	for (const FString& S : List) { Vals.Add(MakeShared<FJsonValueString>(S)); }
	Obj->SetArrayField(Key, Vals);
}

static FString FaNormalizePath(const FString& InPath)
{
	FString P = InPath;
	P.TrimStartAndEndInline();
	if (!P.Contains(TEXT(".")))
	{
		P = P + TEXT(".") + FPackageName::GetShortName(P);
	}
	return P;
}

static UBlueprint* FaLoadBP(const FString& Path, FString& OutErr)
{
	if (Path.IsEmpty()) { OutErr = TEXT("BlueprintPath is empty"); return nullptr; }
	const FString Norm = FaNormalizePath(Path);
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Norm);
	if (!BP) { OutErr = FString::Printf(TEXT("Blueprint not found: %s"), *Norm); }
	return BP;
}

/** "1,2,3" or UE's own "X=1 Y=2 Z=3". Empty string = "leave it alone", reported via bOutSet. */
static bool FaParseVector(const FString& In, FVector& Out, bool& bOutSet)
{
	bOutSet = false;
	FString S = In; S.TrimStartAndEndInline();
	if (S.IsEmpty()) { return true; }
	if (S.Contains(TEXT("=")))
	{
		if (!Out.InitFromString(S)) { return false; }
		bOutSet = true;
		return true;
	}
	TArray<FString> Parts;
	S.ParseIntoArray(Parts, TEXT(","), true);
	if (Parts.Num() != 3) { return false; }
	Out = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
	bOutSet = true;
	return true;
}

/** ☠ The bare 3-number form is PITCH,YAW,ROLL — FRotator's own text order. Python's
 *  unreal.Rotator(a,b,c) is (roll,pitch,yaw), and mixing the two pitched a test pawn -39°. */
static bool FaParseRotator(const FString& In, FRotator& Out, bool& bOutSet)
{
	bOutSet = false;
	FString S = In; S.TrimStartAndEndInline();
	if (S.IsEmpty()) { return true; }
	if (S.Contains(TEXT("=")))
	{
		if (!Out.InitFromString(S)) { return false; }
		bOutSet = true;
		return true;
	}
	TArray<FString> Parts;
	S.ParseIntoArray(Parts, TEXT(","), true);
	if (Parts.Num() != 3) { return false; }
	Out = FRotator(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
	bOutSet = true;
	return true;
}

// ------------------------------------------------------------------ SCS lookup

/** Walk this Blueprint AND its Blueprint ancestors, calling Fn for each SCS + node pair. A child BP
 *  can legally attach to a component declared by any ancestor, so a lookup that only searches the
 *  local SCS reports "no such parent" for names the editor shows in the same tree. */
template <typename FuncT>
static void FaForEachInheritedNode(UBlueprint* BP, FuncT Fn)
{
	TSet<UBlueprint*> Seen;
	UBlueprint* Cur = BP;
	while (Cur && !Seen.Contains(Cur))
	{
		Seen.Add(Cur);
		if (Cur->SimpleConstructionScript)
		{
			for (USCS_Node* N : Cur->SimpleConstructionScript->GetAllNodes())
			{
				if (N) { Fn(Cur, N); }
			}
		}
		Cur = UBlueprint::GetBlueprintFromClass(Cur->ParentClass);
	}
}

static USCS_Node* FaFindNodeAnywhere(UBlueprint* BP, const FString& Name, UBlueprint** OutOwner)
{
	USCS_Node* Found = nullptr;
	UBlueprint* Owner = nullptr;
	FaForEachInheritedNode(BP, [&](UBlueprint* OwnerBP, USCS_Node* N)
	{
		if (Found) { return; }
		if (N->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			Found = N;
			Owner = OwnerBP;
		}
	});
	if (OutOwner) { *OutOwner = Owner; }
	return Found;
}

/** Native components live on the CDO — SCS ones do NOT, which is the trap that makes "list the
 *  components" look like it only ever finds half of them. */
static USceneComponent* FaFindNativeComponent(UBlueprint* BP, const FString& Name)
{
	if (!BP->GeneratedClass) { return nullptr; }
	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) { return nullptr; }
	for (UActorComponent* C : CDO->GetComponents())
	{
		USceneComponent* SC = Cast<USceneComponent>(C);
		if (SC && SC->GetName().Equals(Name, ESearchCase::IgnoreCase)) { return SC; }
	}
	return nullptr;
}

static void FaCollectNativeComponentNames(UBlueprint* BP, TArray<FString>& Out)
{
	if (!BP->GeneratedClass) { return; }
	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) { return; }
	for (UActorComponent* C : CDO->GetComponents())
	{
		if (Cast<USceneComponent>(C)) { Out.Add(C->GetName()); }
	}
}

static void FaCollectAttachTargets(UBlueprint* BP, TArray<FString>& Out)
{
	FaForEachInheritedNode(BP, [&](UBlueprint* OwnerBP, USCS_Node* N)
	{
		if (N->ComponentTemplate && N->ComponentTemplate->IsA<USceneComponent>())
		{
			Out.AddUnique(N->GetVariableName().ToString() + (OwnerBP == BP ? TEXT("") : TEXT(" (inherited)")));
		}
	});
	TArray<FString> Native;
	FaCollectNativeComponentNames(BP, Native);
	for (const FString& S : Native) { Out.AddUnique(S + TEXT(" (native)")); }
}

// ------------------------------------------------------------------ socket validation

/** The sockets a mesh component's mesh actually offers, or false if there is no mesh to ask.
 *  ☠ An unknown AttachToName is NOT an error at runtime — the component silently lands on the
 *  parent's origin. So this is the only place the mistake is catchable. */
static bool FaGetSocketNamesFor(USceneComponent* Comp, TArray<FString>& Out, FString& OutMeshDesc)
{
	if (USkeletalMeshComponent* SkC = Cast<USkeletalMeshComponent>(Comp))
	{
		USkeletalMesh* Mesh = SkC->GetSkeletalMeshAsset();
		if (!Mesh) { return false; }
		OutMeshDesc = Mesh->GetName();
		for (USkeletalMeshSocket* S : Mesh->GetMeshOnlySocketList())
		{
			if (S) { Out.AddUnique(S->SocketName.ToString()); }
		}
		if (USkeleton* Skel = Mesh->GetSkeleton())
		{
			for (USkeletalMeshSocket* S : Skel->Sockets)
			{
				if (S) { Out.AddUnique(S->SocketName.ToString()); }
			}
			// A bone name is a legal attach target too, and for a rig that ships no sockets it is
			// the ONLY one — refusing bones here would make the check worse than no check.
			const FReferenceSkeleton& Ref = Skel->GetReferenceSkeleton();
			for (int32 i = 0; i < Ref.GetNum(); ++i) { Out.AddUnique(Ref.GetBoneName(i).ToString()); }
		}
		return true;
	}
	if (UStaticMeshComponent* StC = Cast<UStaticMeshComponent>(Comp))
	{
		UStaticMesh* Mesh = StC->GetStaticMesh();
		if (!Mesh) { return false; }
		OutMeshDesc = Mesh->GetName();
		for (UStaticMeshSocket* S : Mesh->Sockets)
		{
			if (S) { Out.AddUnique(S->SocketName.ToString()); }
		}
		return true;
	}
	return false;
}

static bool FaValidateSocket(USceneComponent* ParentComp, const FString& SocketName, FString& OutErr)
{
	if (SocketName.IsEmpty() || !ParentComp) { return true; }
	TArray<FString> Names;
	FString MeshDesc;
	if (!FaGetSocketNamesFor(ParentComp, Names, MeshDesc))
	{
		// No mesh assigned (or not a mesh component) — nothing to check against. Allowing it is
		// correct: the mesh may legitimately be set later or per-instance.
		return true;
	}
	for (const FString& N : Names)
	{
		if (N.Equals(SocketName, ESearchCase::IgnoreCase)) { return true; }
	}
	Names.Sort();
	OutErr = ErrWithList(
		FString::Printf(TEXT("'%s' has no socket or bone named '%s' (mesh %s). Attaching anyway would ")
		                TEXT("silently place the component at the parent's origin."),
		                *ParentComp->GetName(), *SocketName, *MeshDesc),
		TEXT("sockets"), Names);
	return false;
}

// ------------------------------------------------------------------ skeleton loading

/** Accepts a Skeleton, a SkeletalMesh, or a StaticMesh path and resolves the pieces each call needs. */
struct FFaSkelTarget
{
	USkeleton* Skeleton = nullptr;
	USkeletalMesh* SkeletalMesh = nullptr;
	UStaticMesh* StaticMesh = nullptr;
	UObject* Asset = nullptr;
};

static bool FaLoadSkelTarget(const FString& Path, FFaSkelTarget& Out, FString& OutErr)
{
	if (Path.IsEmpty()) { OutErr = TEXT("AssetPath is empty"); return false; }
	const FString Norm = FaNormalizePath(Path);
	UObject* Obj = LoadObject<UObject>(nullptr, *Norm);
	if (!Obj) { OutErr = FString::Printf(TEXT("Asset not found: %s"), *Norm); return false; }
	Out.Asset = Obj;
	if (USkeleton* S = Cast<USkeleton>(Obj)) { Out.Skeleton = S; return true; }
	if (USkeletalMesh* M = Cast<USkeletalMesh>(Obj)) { Out.SkeletalMesh = M; Out.Skeleton = M->GetSkeleton(); return true; }
	if (UStaticMesh* SM = Cast<UStaticMesh>(Obj)) { Out.StaticMesh = SM; return true; }
	OutErr = FString::Printf(TEXT("%s is a %s — expected a Skeleton, SkeletalMesh or StaticMesh"),
	                         *Norm, *Obj->GetClass()->GetName());
	return false;
}

static FJObj FaSocketJson(USkeletalMeshSocket* S, const TCHAR* Scope)
{
	FJObj O = NewObj();
	O->SetStringField(TEXT("name"), S->SocketName.ToString());
	O->SetStringField(TEXT("bone"), S->BoneName.ToString());
	O->SetStringField(TEXT("scope"), Scope);
	O->SetStringField(TEXT("location"), S->RelativeLocation.ToString());
	O->SetStringField(TEXT("rotation"), S->RelativeRotation.ToString());
	O->SetStringField(TEXT("scale"), S->RelativeScale.ToString());
	return O;
}

} // namespace FableAssetPrivate

using namespace FableAssetPrivate;

// ==================================================================== SCS : read

FString UFableAsset::ScsList(const FString& BlueprintPath)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	TArray<FJVal> Comps;
	FaForEachInheritedNode(BP, [&](UBlueprint* OwnerBP, USCS_Node* N)
	{
		FJObj C = NewObj();
		C->SetStringField(TEXT("name"), N->GetVariableName().ToString());
		C->SetStringField(TEXT("class"), N->ComponentClass ? N->ComponentClass->GetName() : TEXT("(none)"));
		C->SetStringField(TEXT("owner"), OwnerBP->GetName());
		C->SetBoolField(TEXT("inherited"), OwnerBP != BP);
		C->SetBoolField(TEXT("isRoot"), N->IsRootNode());

		// The four bare-UPROPERTY fields Python cannot see. This readback is half the reason the
		// file exists — you cannot fix an attachment you cannot observe.
		C->SetStringField(TEXT("attachSocket"), N->AttachToName.ToString());
		C->SetStringField(TEXT("parent"), N->ParentComponentOrVariableName.ToString());
		C->SetBoolField(TEXT("parentIsNative"), N->bIsParentComponentNative);

		// The SCS-side parent (a child node's real parent is the node holding it, not the
		// ParentComponentOrVariableName field, which is only filled in for the inherited/native case).
		if (OwnerBP->SimpleConstructionScript)
		{
			for (USCS_Node* Maybe : OwnerBP->SimpleConstructionScript->GetAllNodes())
			{
				if (Maybe && Maybe->GetChildNodes().Contains(N))
				{
					C->SetStringField(TEXT("parent"), Maybe->GetVariableName().ToString());
					C->SetBoolField(TEXT("parentIsNative"), false);
					break;
				}
			}
		}

		if (USceneComponent* SC = Cast<USceneComponent>(N->ComponentTemplate))
		{
			C->SetStringField(TEXT("location"), SC->GetRelativeLocation().ToString());
			C->SetStringField(TEXT("rotation"), SC->GetRelativeRotation().ToString());
			C->SetStringField(TEXT("scale"), SC->GetRelativeScale3D().ToString());
			if (USkeletalMeshComponent* SkC = Cast<USkeletalMeshComponent>(SC))
			{
				USkeletalMesh* M = SkC->GetSkeletalMeshAsset();
				C->SetStringField(TEXT("mesh"), M ? M->GetPathName() : TEXT(""));
			}
			else if (UStaticMeshComponent* StC = Cast<UStaticMeshComponent>(SC))
			{
				UStaticMesh* M = StC->GetStaticMesh();
				C->SetStringField(TEXT("mesh"), M ? M->GetPathName() : TEXT(""));
			}
		}
		Comps.Add(MakeShared<FJsonValueObject>(C));
	});

	// Native components, which are on the CDO and therefore invisible to any SCS walk.
	TArray<FJVal> Natives;
	if (BP->GeneratedClass)
	{
		if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject()))
		{
			for (UActorComponent* Comp : CDO->GetComponents())
			{
				FJObj C = NewObj();
				C->SetStringField(TEXT("name"), Comp->GetName());
				C->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
				C->SetBoolField(TEXT("scene"), Comp->IsA<USceneComponent>());
				if (USkeletalMeshComponent* SkC = Cast<USkeletalMeshComponent>(Comp))
				{
					USkeletalMesh* M = SkC->GetSkeletalMeshAsset();
					C->SetStringField(TEXT("mesh"), M ? M->GetPathName() : TEXT(""));
				}
				Natives.Add(MakeShared<FJsonValueObject>(C));
			}
		}
	}

	TArray<FString> Targets;
	FaCollectAttachTargets(BP, Targets);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("blueprint"), BP->GetName());
	O->SetStringField(TEXT("parentClass"), BP->ParentClass ? BP->ParentClass->GetPathName() : TEXT(""));
	O->SetArrayField(TEXT("components"), Comps);
	O->SetArrayField(TEXT("native"), Natives);
	FaSetStrArray(O, TEXT("parents"), Targets);
	return ToJson(O);
}

// ==================================================================== SCS : attach

FString UFableAsset::ScsSetAttach(const FString& BlueprintPath, const FString& ComponentName,
                                  const FString& ParentName, const FString& SocketName)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (!BP->SimpleConstructionScript) { return Err(TEXT("Blueprint has no SimpleConstructionScript")); }

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		TArray<FString> Names;
		for (USCS_Node* N : SCS->GetAllNodes()) { if (N) { Names.Add(N->GetVariableName().ToString()); } }
		return ErrWithList(FString::Printf(TEXT("No SCS component named '%s' on %s (an INHERITED or ")
		                                   TEXT("NATIVE component cannot be re-attached from the child — ")
		                                   TEXT("edit the Blueprint that declares it)"),
		                                   *ComponentName, *BP->GetName()),
		                   TEXT("components"), Names);
	}
	if (!Cast<USceneComponent>(Node->ComponentTemplate))
	{
		return Err(FString::Printf(TEXT("'%s' is a %s, not a SceneComponent — only scene components attach"),
		                           *ComponentName,
		                           Node->ComponentTemplate ? *Node->ComponentTemplate->GetClass()->GetName() : TEXT("(null template)")));
	}

	// ---- resolve the target parent (unchanged when ParentName is empty) ----
	USCS_Node* ParentNode = nullptr;
	USceneComponent* ParentComp = nullptr;
	bool bParentIsNative = false;
	const bool bChangingParent = !ParentName.IsEmpty();

	if (bChangingParent)
	{
		UBlueprint* OwnerBP = nullptr;
		ParentNode = FaFindNodeAnywhere(BP, ParentName, &OwnerBP);
		if (ParentNode)
		{
			if (OwnerBP != BP)
			{
				// Attaching to a component declared by an ANCESTOR is legal, but it is recorded the
				// same way a native parent is (by name + owning class), not as a child-node link.
				ParentComp = Cast<USceneComponent>(ParentNode->ComponentTemplate);
				bParentIsNative = false;
			}
			else
			{
				ParentComp = Cast<USceneComponent>(ParentNode->ComponentTemplate);
			}
			if (!ParentComp)
			{
				return Err(FString::Printf(TEXT("'%s' is not a SceneComponent"), *ParentName));
			}
			if (ParentNode == Node || ParentNode->IsChildOf(Node))
			{
				return Err(FString::Printf(TEXT("Refused: attaching '%s' to '%s' would make a cycle"),
				                           *ComponentName, *ParentName));
			}
		}
		else
		{
			ParentComp = FaFindNativeComponent(BP, ParentName);
			bParentIsNative = true;
			if (!ParentComp)
			{
				TArray<FString> Targets;
				FaCollectAttachTargets(BP, Targets);
				return ErrWithList(FString::Printf(TEXT("No component named '%s' on %s"), *ParentName, *BP->GetName()),
				                   TEXT("parents"), Targets);
			}
		}
	}
	else
	{
		// Socket-only change: validate against whatever it is ALREADY attached to.
		UBlueprint* OwnerBP = nullptr;
		if (USCS_Node* Existing = FaFindNodeAnywhere(BP, Node->ParentComponentOrVariableName.ToString(), &OwnerBP))
		{
			ParentComp = Cast<USceneComponent>(Existing->ComponentTemplate);
		}
		if (!ParentComp) { ParentComp = FaFindNativeComponent(BP, Node->ParentComponentOrVariableName.ToString()); }
		if (!ParentComp)
		{
			for (USCS_Node* Maybe : SCS->GetAllNodes())
			{
				if (Maybe && Maybe->GetChildNodes().Contains(Node))
				{
					ParentComp = Cast<USceneComponent>(Maybe->ComponentTemplate);
					break;
				}
			}
		}
	}

	if (!FaValidateSocket(ParentComp, SocketName, E)) { return E; }

	// ---- commit ----
	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Component Attachment")));
	BP->Modify();
	SCS->Modify();
	Node->Modify();

	const FString OldParent = Node->ParentComponentOrVariableName.ToString();
	const FString OldSocket = Node->AttachToName.ToString();

	if (bChangingParent)
	{
		// RemoveNode drops it from RootNodes+AllNodes (root case) or from its parent's ChildNodes
		// (child case) and clears the three parent fields, so both start states converge here.
		SCS->RemoveNode(Node, false);

		UBlueprint* OwnerBP = nullptr;
		FaFindNodeAnywhere(BP, ParentName, &OwnerBP);
		const bool bLocalScsParent = (ParentNode != nullptr && OwnerBP == BP);

		if (bLocalScsParent)
		{
			ParentNode->AddChildNode(Node, true);
		}
		else
		{
			// Native or inherited parent: the node becomes a ROOT of this SCS and records its parent
			// by name. SetParent fills bIsParentComponentNative / ParentComponentOwnerClassName —
			// getting that trio consistent by hand is exactly what goes wrong when it is done blind.
			SCS->AddNode(Node);
			Node->SetParent(ParentComp);
		}
		(void)bParentIsNative;
	}

	Node->AttachToName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("component"), ComponentName);
	O->SetStringField(TEXT("parentWas"), OldParent);
	O->SetStringField(TEXT("parentNow"), Node->ParentComponentOrVariableName.ToString());
	O->SetStringField(TEXT("socketWas"), OldSocket);
	O->SetStringField(TEXT("socketNow"), Node->AttachToName.ToString());
	O->SetBoolField(TEXT("parentIsNative"), Node->bIsParentComponentNative);
	O->SetBoolField(TEXT("isRoot"), Node->IsRootNode());
	O->SetStringField(TEXT("note"), TEXT("Not saved — call save_asset and verify the mtime moved."));
	return ToJson(O);
}

FString UFableAsset::ScsAddComponent(const FString& BlueprintPath, const FString& ComponentClassPath,
                                     const FString& ComponentName, const FString& ParentName,
                                     const FString& SocketName)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (!BP->SimpleConstructionScript) { return Err(TEXT("Blueprint has no SimpleConstructionScript")); }
	if (ComponentName.IsEmpty()) { return Err(TEXT("ComponentName is empty")); }

	UClass* CompClass = LoadObject<UClass>(nullptr, *ComponentClassPath);
	if (!CompClass)
	{
		CompClass = FindFirstObject<UClass>(*ComponentClassPath, EFindFirstObjectOptions::None);
	}
	if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return Err(FString::Printf(TEXT("'%s' did not resolve to an ActorComponent class"), *ComponentClassPath));
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (SCS->FindSCSNode(FName(*ComponentName)))
	{
		return Err(FString::Printf(TEXT("'%s' already exists on %s"), *ComponentName, *BP->GetName()));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Component")));
	BP->Modify();
	SCS->Modify();

	USCS_Node* Node = SCS->CreateNode(CompClass, FName(*ComponentName));
	if (!Node) { return Err(TEXT("CreateNode returned null")); }
	SCS->AddNode(Node);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Attach as a second step so the (well-tested) parent/socket path is shared, and so a bad
	// parent or socket leaves a created-but-unattached component rather than a half-written one.
	if (!ParentName.IsEmpty() || !SocketName.IsEmpty())
	{
		const FString AttachResult = ScsSetAttach(BlueprintPath, ComponentName, ParentName, SocketName);
		TSharedPtr<FJsonObject> Parsed;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(AttachResult), Parsed) && Parsed.IsValid())
		{
			bool bOk = false;
			if (Parsed->TryGetBoolField(TEXT("ok"), bOk) && !bOk)
			{
				Parsed->SetStringField(TEXT("warning"),
					FString::Printf(TEXT("'%s' WAS created but is unattached — fix the socket/parent and call ScsSetAttach."),
					                *ComponentName));
				return ToJson(Parsed);
			}
		}
		return AttachResult;
	}

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("component"), ComponentName);
	O->SetStringField(TEXT("class"), CompClass->GetName());
	O->SetBoolField(TEXT("isRoot"), Node->IsRootNode());
	return ToJson(O);
}

FString UFableAsset::ScsRemoveComponent(const FString& BlueprintPath, const FString& ComponentName)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (!BP->SimpleConstructionScript) { return Err(TEXT("Blueprint has no SimpleConstructionScript")); }

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		TArray<FString> Names;
		for (USCS_Node* N : SCS->GetAllNodes()) { if (N) { Names.Add(N->GetVariableName().ToString()); } }
		return ErrWithList(FString::Printf(TEXT("No SCS component named '%s'"), *ComponentName), TEXT("components"), Names);
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Remove Component")));
	BP->Modify();
	SCS->Modify();
	const int32 Children = Node->GetChildNodes().Num();
	SCS->RemoveNodeAndPromoteChildren(Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("removed"), ComponentName);
	O->SetNumberField(TEXT("childrenPromoted"), Children);
	return ToJson(O);
}

FString UFableAsset::ScsSetTransform(const FString& BlueprintPath, const FString& ComponentName,
                                     const FString& Location, const FString& Rotation, const FString& Scale)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (!BP->SimpleConstructionScript) { return Err(TEXT("Blueprint has no SimpleConstructionScript")); }

	USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
	if (!Node) { return Err(FString::Printf(TEXT("No SCS component named '%s'"), *ComponentName)); }
	USceneComponent* SC = Cast<USceneComponent>(Node->ComponentTemplate);
	if (!SC) { return Err(FString::Printf(TEXT("'%s' is not a SceneComponent"), *ComponentName)); }

	FVector Loc = SC->GetRelativeLocation(), Scl = SC->GetRelativeScale3D();
	FRotator Rot = SC->GetRelativeRotation();
	bool bL = false, bR = false, bS = false;
	if (!FaParseVector(Location, Loc, bL)) { return Err(TEXT("Location: expected '1,2,3' or 'X=1 Y=2 Z=3'")); }
	if (!FaParseRotator(Rotation, Rot, bR)) { return Err(TEXT("Rotation: expected 'Pitch,Yaw,Roll' or 'P=0 Y=90 R=0'")); }
	if (!FaParseVector(Scale, Scl, bS))     { return Err(TEXT("Scale: expected '1,2,3' or 'X=1 Y=1 Z=1'")); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Component Transform")));
	SC->Modify();
	if (bL) { SC->SetRelativeLocation(Loc); }
	if (bR) { SC->SetRelativeRotation(Rot); }
	if (bS) { SC->SetRelativeScale3D(Scl); }
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("component"), ComponentName);
	O->SetStringField(TEXT("location"), SC->GetRelativeLocation().ToString());
	O->SetStringField(TEXT("rotation"), SC->GetRelativeRotation().ToString());
	O->SetStringField(TEXT("scale"), SC->GetRelativeScale3D().ToString());
	return ToJson(O);
}

FString UFableAsset::ScsSetProps(const FString& BlueprintPath, const FString& ComponentName, const FString& PropsJson)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (!BP->SimpleConstructionScript) { return Err(TEXT("Blueprint has no SimpleConstructionScript")); }

	USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
	if (!Node || !Node->ComponentTemplate) { return Err(FString::Printf(TEXT("No SCS component named '%s'"), *ComponentName)); }
	UActorComponent* Target = Node->ComponentTemplate;

	TSharedPtr<FJsonObject> Props;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PropsJson), Props) || !Props.IsValid())
	{
		return Err(TEXT("PropsJson is not a JSON object"));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Component Props")));
	Target->Modify();

	TArray<FString> Set, Failed;
	for (const auto& Pair : Props->Values)
	{
		FString ValueText;
		if (Pair.Value->Type == EJson::String)       { ValueText = Pair.Value->AsString(); }
		else if (Pair.Value->Type == EJson::Boolean) { ValueText = Pair.Value->AsBool() ? TEXT("True") : TEXT("False"); }
		else if (Pair.Value->Type == EJson::Number)  { ValueText = LexToString(Pair.Value->AsNumber()); }
		else { Failed.Add(Pair.Key + TEXT(" (unsupported JSON type)")); continue; }

		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop) { Failed.Add(Pair.Key + TEXT(" (no such property)")); continue; }
		const TCHAR* Result = Prop->ImportText_InContainer(*ValueText, Target, Target, PPF_None);
		if (Result) { Set.Add(Pair.Key); } else { Failed.Add(Pair.Key + TEXT(" (ImportText rejected the value)")); }
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), Failed.Num() == 0);
	FaSetStrArray(O, TEXT("set"), Set);
	FaSetStrArray(O, TEXT("failed"), Failed);
	return ToJson(O);
}

FString UFableAsset::ScsGetProps(const FString& BlueprintPath, const FString& ComponentName, const FString& PropsCsv)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!BP->SimpleConstructionScript) { return Err(TEXT("Blueprint has no SimpleConstructionScript")); }

	UBlueprint* OwnerBP = nullptr;
	USCS_Node* Node = FaFindNodeAnywhere(BP, ComponentName, &OwnerBP);
	if (!Node || !Node->ComponentTemplate) { return Err(FString::Printf(TEXT("No component named '%s'"), *ComponentName)); }
	UActorComponent* Target = Node->ComponentTemplate;

	TSet<FString> Wanted;
	if (!PropsCsv.IsEmpty())
	{
		TArray<FString> Parts;
		PropsCsv.ParseIntoArray(Parts, TEXT(","), true);
		for (FString& P : Parts) { Wanted.Add(P.TrimStartAndEnd()); }
	}

	FJObj Props = NewObj();
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
	{
		FProperty* P = *It;
		const FString Internal = P->GetName();
		const FString Authored = P->GetAuthoredName();
		if (Wanted.Num() > 0 && !Wanted.Contains(Internal) && !Wanted.Contains(Authored)) { continue; }
		FString Value;
		P->ExportText_InContainer(0, Value, Target, Target, Target, PPF_None);
		Props->SetStringField(Authored.IsEmpty() ? Internal : Authored, Value);
		++Count;
	}

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("component"), ComponentName);
	O->SetStringField(TEXT("class"), Target->GetClass()->GetName());
	O->SetStringField(TEXT("owner"), OwnerBP ? OwnerBP->GetName() : TEXT(""));
	O->SetNumberField(TEXT("count"), Count);
	O->SetObjectField(TEXT("props"), Props);
	return ToJson(O);
}

FString UFableAsset::SetParentClass(const FString& BlueprintPath, const FString& ParentClassPath)
{
	FString E;
	UBlueprint* BP = FaLoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	UClass* NewParent = LoadObject<UClass>(nullptr, *ParentClassPath);
	if (!NewParent && !ParentClassPath.Contains(TEXT(".")))
	{
		NewParent = FindFirstObject<UClass>(*ParentClassPath, EFindFirstObjectOptions::None);
	}
	if (!NewParent)
	{
		// A /Game Blueprint parent is named by its GENERATED class ("....BP_X_C"), which is the
		// spelling people forget and which fails identically to a typo.
		UBlueprint* ParentBP = LoadObject<UBlueprint>(nullptr, *FaNormalizePath(ParentClassPath));
		if (ParentBP) { NewParent = ParentBP->GeneratedClass; }
	}
	if (!NewParent) { return Err(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassPath)); }
	if (NewParent == BP->ParentClass) { return Err(FString::Printf(TEXT("%s is already parented to %s"), *BP->GetName(), *NewParent->GetName())); }
	if (BP->GeneratedClass && NewParent->IsChildOf(BP->GeneratedClass))
	{
		return Err(TEXT("Refused: that would make the Blueprint its own ancestor"));
	}

	const FString OldParent = BP->ParentClass ? BP->ParentClass->GetPathName() : TEXT("");

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Reparent Blueprint")));
	BP->Modify();
	BP->ParentClass = NewParent;
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("blueprint"), BP->GetName());
	O->SetStringField(TEXT("parentWas"), OldParent);
	O->SetStringField(TEXT("parentNow"), NewParent->GetPathName());
	O->SetStringField(TEXT("warning"),
		TEXT("A reparent DELTA-WIPES leaf values that match the new inherited default. Value-diff "
		     "every property and every SCS archetype before trusting this."));
	return ToJson(O);
}

// ==================================================================== sockets

FString UFableAsset::SkelInfo(const FString& AssetPath)
{
	FString E;
	FFaSkelTarget T;
	if (!FaLoadSkelTarget(AssetPath, T, E)) { return Err(E); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("asset"), T.Asset->GetPathName());
	O->SetStringField(TEXT("type"), T.Asset->GetClass()->GetName());

	TArray<FJVal> Sockets;
	if (T.StaticMesh)
	{
		for (UStaticMeshSocket* S : T.StaticMesh->Sockets)
		{
			if (!S) continue;
			FJObj C = NewObj();
			C->SetStringField(TEXT("name"), S->SocketName.ToString());
			C->SetStringField(TEXT("scope"), TEXT("staticmesh"));
			C->SetStringField(TEXT("location"), S->RelativeLocation.ToString());
			C->SetStringField(TEXT("rotation"), S->RelativeRotation.ToString());
			C->SetStringField(TEXT("scale"), S->RelativeScale.ToString());
			Sockets.Add(MakeShared<FJsonValueObject>(C));
		}
		O->SetArrayField(TEXT("sockets"), Sockets);
		return ToJson(O);
	}

	if (T.Skeleton)
	{
		O->SetStringField(TEXT("skeleton"), T.Skeleton->GetPathName());
		O->SetNumberField(TEXT("boneCount"), T.Skeleton->GetReferenceSkeleton().GetNum());
		for (USkeletalMeshSocket* S : T.Skeleton->Sockets)
		{
			if (S) { Sockets.Add(MakeShared<FJsonValueObject>(FaSocketJson(S, TEXT("skeleton")))); }
		}
	}
	if (T.SkeletalMesh)
	{
		O->SetStringField(TEXT("mesh"), T.SkeletalMesh->GetPathName());
		for (USkeletalMeshSocket* S : T.SkeletalMesh->GetMeshOnlySocketList())
		{
			if (S) { Sockets.Add(MakeShared<FJsonValueObject>(FaSocketJson(S, TEXT("mesh")))); }
		}
	}
	O->SetArrayField(TEXT("sockets"), Sockets);
	return ToJson(O);
}

FString UFableAsset::SkelListBones(const FString& AssetPath, const FString& Contains)
{
	FString E;
	FFaSkelTarget T;
	if (!FaLoadSkelTarget(AssetPath, T, E)) { return Err(E); }
	if (!T.Skeleton) { return Err(TEXT("That asset has no skeleton (a StaticMesh has bones only as sockets)")); }

	const FReferenceSkeleton& Ref = T.Skeleton->GetReferenceSkeleton();
	TArray<FString> Names;
	for (int32 i = 0; i < Ref.GetNum(); ++i)
	{
		const FString N = Ref.GetBoneName(i).ToString();
		if (Contains.IsEmpty() || N.Contains(Contains, ESearchCase::IgnoreCase)) { Names.Add(N); }
	}

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("skeleton"), T.Skeleton->GetPathName());
	O->SetNumberField(TEXT("total"), Ref.GetNum());
	O->SetNumberField(TEXT("matched"), Names.Num());
	FaSetStrArray(O, TEXT("bones"), Names);
	return ToJson(O);
}

FString UFableAsset::SkelAddSocket(const FString& AssetPath, const FString& SocketName, const FString& BoneName,
                                   const FString& Location, const FString& Rotation, const FString& Scale,
                                   bool bMeshOnly)
{
	FString E;
	FFaSkelTarget T;
	if (!FaLoadSkelTarget(AssetPath, T, E)) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (SocketName.IsEmpty()) { return Err(TEXT("SocketName is empty")); }
	if (!T.Skeleton) { return Err(TEXT("SkelAddSocket needs a Skeleton or SkeletalMesh")); }
	if (bMeshOnly && !T.SkeletalMesh) { return Err(TEXT("bMeshOnly needs a SkeletalMesh path, not a Skeleton")); }

	// A socket on a bone that does not exist is created without complaint and then never moves —
	// it is pinned to the component origin forever. Refuse instead, and say what the options are.
	const FReferenceSkeleton& Ref = T.Skeleton->GetReferenceSkeleton();
	if (Ref.FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
	{
		TArray<FString> Bones;
		for (int32 i = 0; i < Ref.GetNum(); ++i) { Bones.Add(Ref.GetBoneName(i).ToString()); }
		return ErrWithList(FString::Printf(TEXT("Skeleton %s has no bone '%s'"), *T.Skeleton->GetName(), *BoneName),
		                   TEXT("bones"), Bones);
	}
	if (T.Skeleton->FindSocket(FName(*SocketName)))
	{
		return Err(FString::Printf(TEXT("Socket '%s' already exists — use SkelSetSocket to move it"), *SocketName));
	}
	if (T.SkeletalMesh && T.SkeletalMesh->FindSocket(FName(*SocketName)))
	{
		return Err(FString::Printf(TEXT("Socket '%s' already exists on the mesh"), *SocketName));
	}

	FVector Loc = FVector::ZeroVector, Scl = FVector::OneVector;
	FRotator Rot = FRotator::ZeroRotator;
	bool bL = false, bR = false, bS = false;
	if (!FaParseVector(Location, Loc, bL)) { return Err(TEXT("Location: expected '1,2,3' or 'X=1 Y=2 Z=3'")); }
	if (!FaParseRotator(Rotation, Rot, bR)) { return Err(TEXT("Rotation: expected 'Pitch,Yaw,Roll' or 'P=0 Y=90 R=0'")); }
	if (!FaParseVector(Scale, Scl, bS))     { return Err(TEXT("Scale: expected '1,2,3' or 'X=1 Y=1 Z=1'")); }

	UObject* Owner = bMeshOnly ? static_cast<UObject*>(T.SkeletalMesh) : static_cast<UObject*>(T.Skeleton);

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Socket")));
	Owner->Modify();

	USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Owner);
	Socket->SocketName = FName(*SocketName);
	Socket->BoneName = FName(*BoneName);
	Socket->RelativeLocation = Loc;
	Socket->RelativeRotation = Rot;
	Socket->RelativeScale = Scl;

	if (bMeshOnly) { T.SkeletalMesh->GetMeshOnlySocketList().Add(Socket); }
	else           { T.Skeleton->Sockets.Add(Socket); }

	Owner->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("socket"), SocketName);
	O->SetStringField(TEXT("bone"), BoneName);
	O->SetStringField(TEXT("scope"), bMeshOnly ? TEXT("mesh") : TEXT("skeleton"));
	O->SetStringField(TEXT("owner"), Owner->GetPathName());
	O->SetStringField(TEXT("location"), Loc.ToString());
	O->SetStringField(TEXT("rotation"), Rot.ToString());
	O->SetStringField(TEXT("scale"), Scl.ToString());
	O->SetStringField(TEXT("note"), TEXT("Not saved — call save_asset and verify the mtime moved."));
	return ToJson(O);
}

FString UFableAsset::SkelSetSocket(const FString& AssetPath, const FString& SocketName, const FString& BoneName,
                                   const FString& Location, const FString& Rotation, const FString& Scale)
{
	FString E;
	FFaSkelTarget T;
	if (!FaLoadSkelTarget(AssetPath, T, E)) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	USkeletalMeshSocket* Socket = nullptr;
	UObject* Owner = nullptr;
	if (T.SkeletalMesh)
	{
		for (USkeletalMeshSocket* S : T.SkeletalMesh->GetMeshOnlySocketList())
		{
			if (S && S->SocketName == FName(*SocketName)) { Socket = S; Owner = T.SkeletalMesh; break; }
		}
	}
	if (!Socket && T.Skeleton)
	{
		Socket = T.Skeleton->FindSocket(FName(*SocketName));
		if (Socket) { Owner = T.Skeleton; }
	}
	if (!Socket)
	{
		TArray<FString> Names;
		if (T.Skeleton)     { for (USkeletalMeshSocket* S : T.Skeleton->Sockets) { if (S) Names.Add(S->SocketName.ToString()); } }
		if (T.SkeletalMesh) { for (USkeletalMeshSocket* S : T.SkeletalMesh->GetMeshOnlySocketList()) { if (S) Names.Add(S->SocketName.ToString()); } }
		return ErrWithList(FString::Printf(TEXT("No socket named '%s'"), *SocketName), TEXT("sockets"), Names);
	}

	FVector Loc = Socket->RelativeLocation, Scl = Socket->RelativeScale;
	FRotator Rot = Socket->RelativeRotation;
	bool bL = false, bR = false, bS = false;
	if (!FaParseVector(Location, Loc, bL)) { return Err(TEXT("Location: expected '1,2,3' or 'X=1 Y=2 Z=3'")); }
	if (!FaParseRotator(Rotation, Rot, bR)) { return Err(TEXT("Rotation: expected 'Pitch,Yaw,Roll' or 'P=0 Y=90 R=0'")); }
	if (!FaParseVector(Scale, Scl, bS))     { return Err(TEXT("Scale: expected '1,2,3' or 'X=1 Y=1 Z=1'")); }

	if (!BoneName.IsEmpty() && T.Skeleton)
	{
		const FReferenceSkeleton& Ref = T.Skeleton->GetReferenceSkeleton();
		if (Ref.FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
		{
			TArray<FString> Bones;
			for (int32 i = 0; i < Ref.GetNum(); ++i) { Bones.Add(Ref.GetBoneName(i).ToString()); }
			return ErrWithList(FString::Printf(TEXT("Skeleton has no bone '%s'"), *BoneName), TEXT("bones"), Bones);
		}
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Socket")));
	Owner->Modify();
	Socket->Modify();
	if (!BoneName.IsEmpty()) { Socket->BoneName = FName(*BoneName); }
	if (bL) { Socket->RelativeLocation = Loc; }
	if (bR) { Socket->RelativeRotation = Rot; }
	if (bS) { Socket->RelativeScale = Scl; }
	Owner->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("socket"), Socket->SocketName.ToString());
	O->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
	O->SetStringField(TEXT("location"), Socket->RelativeLocation.ToString());
	O->SetStringField(TEXT("rotation"), Socket->RelativeRotation.ToString());
	O->SetStringField(TEXT("scale"), Socket->RelativeScale.ToString());
	return ToJson(O);
}

FString UFableAsset::SkelRefPoseTransform(const FString& AssetPath, const FString& SocketOrBoneName)
{
	FString E;
	FFaSkelTarget T;
	if (!FaLoadSkelTarget(AssetPath, T, E)) { return Err(E); }
	if (!T.Skeleton) { return Err(TEXT("SkelRefPoseTransform needs a Skeleton or SkeletalMesh")); }

	const FReferenceSkeleton& Ref = T.Skeleton->GetReferenceSkeleton();

	// A socket resolves to (bone, relative offset); a bare bone name is the identity offset case.
	FName BoneName = FName(*SocketOrBoneName);
	FTransform SocketRelative = FTransform::Identity;
	FString Kind = TEXT("bone");
	if (const USkeletalMeshSocket* S = T.Skeleton->FindSocket(FName(*SocketOrBoneName)))
	{
		BoneName = S->BoneName;
		SocketRelative = FTransform(S->RelativeRotation, S->RelativeLocation, S->RelativeScale);
		Kind = TEXT("socket");
	}
	else if (T.SkeletalMesh)
	{
		for (USkeletalMeshSocket* MS : T.SkeletalMesh->GetMeshOnlySocketList())
		{
			if (MS && MS->SocketName == FName(*SocketOrBoneName))
			{
				BoneName = MS->BoneName;
				SocketRelative = FTransform(MS->RelativeRotation, MS->RelativeLocation, MS->RelativeScale);
				Kind = TEXT("socket");
				break;
			}
		}
	}

	int32 BoneIdx = Ref.FindBoneIndex(BoneName);
	if (BoneIdx == INDEX_NONE)
	{
		return Err(FString::Printf(TEXT("No socket or bone named '%s' on %s"),
		                           *SocketOrBoneName, *T.Skeleton->GetName()));
	}

	// Accumulate the ref-pose chain to component space. GetRefBonePose() is PARENT-relative, so a
	// single bone's entry is meaningless on its own — this walk is the whole point of the function.
	const TArray<FTransform>& Pose = Ref.GetRefBonePose();
	FTransform Component = FTransform::Identity;
	for (int32 i = BoneIdx; i != INDEX_NONE; i = Ref.GetParentIndex(i))
	{
		Component = Component * Pose[i];
	}
	const FTransform Result = SocketRelative * Component;

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("name"), SocketOrBoneName);
	O->SetStringField(TEXT("kind"), Kind);
	O->SetStringField(TEXT("bone"), BoneName.ToString());
	O->SetStringField(TEXT("space"), TEXT("component (reference pose)"));
	O->SetStringField(TEXT("location"), Result.GetLocation().ToString());
	O->SetStringField(TEXT("rotation"), Result.Rotator().ToString());
	O->SetStringField(TEXT("scale"), Result.GetScale3D().ToString());
	// The rotation that makes a child of this socket axis-aligned with the component — i.e. the
	// relative rotation an attachment needs in order to stand upright in the reference pose.
	O->SetStringField(TEXT("uprightRelativeRotation"), Result.Rotator().GetInverse().ToString());
	return ToJson(O);
}

FString UFableAsset::SkelRemoveSocket(const FString& AssetPath, const FString& SocketName)
{
	FString E;
	FFaSkelTarget T;
	if (!FaLoadSkelTarget(AssetPath, T, E)) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	int32 Removed = 0;
	FString Scope;
	if (T.SkeletalMesh)
	{
		TArray<TObjectPtr<USkeletalMeshSocket>>& List = T.SkeletalMesh->GetMeshOnlySocketList();
		for (int32 i = List.Num() - 1; i >= 0; --i)
		{
			if (List[i] && List[i]->SocketName == FName(*SocketName))
			{
				T.SkeletalMesh->Modify();
				List.RemoveAt(i); ++Removed; Scope = TEXT("mesh");
			}
		}
	}
	if (T.Skeleton)
	{
		for (int32 i = T.Skeleton->Sockets.Num() - 1; i >= 0; --i)
		{
			if (T.Skeleton->Sockets[i] && T.Skeleton->Sockets[i]->SocketName == FName(*SocketName))
			{
				T.Skeleton->Modify();
				T.Skeleton->Sockets.RemoveAt(i); ++Removed; Scope = TEXT("skeleton");
			}
		}
	}
	if (Removed == 0) { return Err(FString::Printf(TEXT("No socket named '%s'"), *SocketName)); }

	if (T.SkeletalMesh) { T.SkeletalMesh->MarkPackageDirty(); }
	if (T.Skeleton)     { T.Skeleton->MarkPackageDirty(); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("socket"), SocketName);
	O->SetNumberField(TEXT("removed"), Removed);
	O->SetStringField(TEXT("scope"), Scope);
	return ToJson(O);
}
