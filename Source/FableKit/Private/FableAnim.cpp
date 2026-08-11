// Copyright © 2026 Linesway All rights reserved.

#include "FableAnim.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/Skeleton.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Factories/AnimMontageFactory.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "UObject/UnrealType.h"

// Shared JSON reply vocabulary — see FableJson.h for why it must NOT be redeclared here.
#include "FableJson.h"
using namespace FableKitPrivate;

namespace FableAnimPrivate
{
	static FString Norm(const FString& InPath)
	{
		FString P = InPath;
		P.TrimStartAndEndInline();
		if (!P.Contains(TEXT("."))) { P = P + TEXT(".") + FPackageName::GetShortName(P); }
		return P;
	}

	static UAnimationAsset* LoadAnim(const FString& Path, FString& OutErr)
	{
		if (Path.IsEmpty()) { OutErr = TEXT("AnimPath is empty"); return nullptr; }
		UAnimationAsset* A = LoadObject<UAnimationAsset>(nullptr, *Norm(Path));
		if (!A) { OutErr = FString::Printf(TEXT("Animation asset not found: %s"), *Norm(Path)); }
		return A;
	}

	static IAssetRegistry& Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/** Package-name (no .Object) form, which is what the AssetRegistry's referencer graph speaks. */
	static FName PackageNameOf(const UObject* Obj)
	{
		return Obj && Obj->GetOutermost() ? Obj->GetOutermost()->GetFName() : NAME_None;
	}

	static TArray<FString> ReferencersOf(const UObject* Obj)
	{
		TArray<FString> Out;
		const FName Pkg = PackageNameOf(Obj);
		if (Pkg.IsNone()) { return Out; }
		TArray<FName> Refs;
		Registry().GetReferencers(Pkg, Refs);
		for (const FName& R : Refs)
		{
			// A montage referencing its own source sequence is not "someone else using it".
			if (R != Pkg) { Out.Add(R.ToString()); }
		}
		return Out;
	}

	static TArray<FString> SkeletonSlotNames(const USkeleton* Skel)
	{
		TArray<FString> Out;
		if (!Skel) { return Out; }
		for (const FAnimSlotGroup& Group : Skel->GetSlotGroups())
		{
			for (const FName& S : Group.SlotNames) { Out.Add(S.ToString()); }
		}
		return Out;
	}

	/** The one fact a caller usually wants about a montage: which slot it plays on. */
	static FString FirstSlotName(const UAnimMontage* M)
	{
		if (M && M->SlotAnimTracks.Num() > 0) { return M->SlotAnimTracks[0].SlotName.ToString(); }
		return FString();
	}
}
// ☠ NO file-scope `using namespace FableAnimPrivate;` — a unity build concatenates this
// file with FableNiagara.cpp, whose private namespace also declares Norm(), and every
// unqualified call in BOTH files then becomes C2668 ambiguous. See FableJson.h.


FString UFableAnim::Info(const FString& AnimPath)
{
	FString ErrMsg;
	UAnimationAsset* Anim = FableAnimPrivate::LoadAnim(AnimPath, ErrMsg);
	if (!Anim) { return Err(ErrMsg); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("path"), Anim->GetPathName());
	O->SetStringField(TEXT("class"), Anim->GetClass()->GetName());

	const USkeleton* Skel = Anim->GetSkeleton();
	O->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT(""));

	if (const UAnimSequenceBase* Base = Cast<UAnimSequenceBase>(Anim))
	{
		O->SetNumberField(TEXT("play_length"), Base->GetPlayLength());
		O->SetNumberField(TEXT("num_notifies"), Base->Notifies.Num());
	}
	if (const UAnimSequence* Seq = Cast<UAnimSequence>(Anim))
	{
		O->SetBoolField(TEXT("root_motion"), Seq->bEnableRootMotion);
		O->SetBoolField(TEXT("force_root_lock"), Seq->bForceRootLock);
		O->SetNumberField(TEXT("num_frames"), Seq->GetNumberOfSampledKeys());
	}
	if (const UAnimMontage* M = Cast<UAnimMontage>(Anim))
	{
		O->SetNumberField(TEXT("rate_scale"), M->RateScale);
		O->SetBoolField(TEXT("has_root_motion"), M->HasRootMotion());
		O->SetNumberField(TEXT("blend_in"), M->BlendIn.GetBlendTime());
		O->SetNumberField(TEXT("blend_out"), M->BlendOut.GetBlendTime());
		O->SetStringField(TEXT("slot"), FableAnimPrivate::FirstSlotName(M));

		TArray<FJVal> Tracks;
		for (const FSlotAnimationTrack& T : M->SlotAnimTracks)
		{
			FJObj TO = NewObj();
			TO->SetStringField(TEXT("slot"), T.SlotName.ToString());
			TArray<FJVal> Segs;
			for (const FAnimSegment& Seg : T.AnimTrack.AnimSegments)
			{
				FJObj SO = NewObj();
				const UAnimSequenceBase* Ref = Seg.GetAnimReference();
				SO->SetStringField(TEXT("anim"), Ref ? Ref->GetPathName() : TEXT(""));
				// The segment's own root motion is what actually moves the character.
				if (const UAnimSequence* RefSeq = Cast<UAnimSequence>(Ref))
				{
					SO->SetBoolField(TEXT("anim_root_motion"), RefSeq->bEnableRootMotion);
				}
				SO->SetNumberField(TEXT("start"), Seg.StartPos);
				SO->SetNumberField(TEXT("play_rate"), Seg.AnimPlayRate);
				Segs.Add(MakeShared<FJsonValueObject>(SO));
			}
			TO->SetArrayField(TEXT("segments"), Segs);
			Tracks.Add(MakeShared<FJsonValueObject>(TO));
		}
		O->SetArrayField(TEXT("slot_tracks"), Tracks);

		// Whether that slot even exists on the skeleton — a montage on an unregistered slot plays
		// silently with no visible pose, which reads exactly like "the montage is broken".
		O->SetBoolField(TEXT("slot_registered_on_skeleton"),
			Skel && M->SlotAnimTracks.Num() > 0 && Skel->ContainsSlotName(M->SlotAnimTracks[0].SlotName));
	}

	TArray<FString> Refs = FableAnimPrivate::ReferencersOf(Anim);
	TArray<FJVal> RefVals;
	for (const FString& R : Refs) { RefVals.Add(MakeShared<FJsonValueString>(R)); }
	O->SetArrayField(TEXT("referencers"), RefVals);
	O->SetNumberField(TEXT("num_referencers"), Refs.Num());
	return ToJson(O);
}


FString UFableAnim::ListSkeletonSlots(const FString& SkeletonPath)
{
	USkeleton* Skel = LoadObject<USkeleton>(nullptr, *FableAnimPrivate::Norm(SkeletonPath));
	if (!Skel) { return Err(FString::Printf(TEXT("Skeleton not found: %s"), *FableAnimPrivate::Norm(SkeletonPath))); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("skeleton"), Skel->GetPathName());
	TArray<FJVal> Groups;
	for (const FAnimSlotGroup& Group : Skel->GetSlotGroups())
	{
		FJObj G = NewObj();
		G->SetStringField(TEXT("group"), Group.GroupName.ToString());
		TArray<FJVal> Slots;
		for (const FName& S : Group.SlotNames) { Slots.Add(MakeShared<FJsonValueString>(S.ToString())); }
		G->SetArrayField(TEXT("slots"), Slots);
		Groups.Add(MakeShared<FJsonValueObject>(G));
	}
	O->SetArrayField(TEXT("groups"), Groups);
	return ToJson(O);
}


FString UFableAnim::Survey(const FString& PackagePath, const FString& NameContains)
{
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UAnimMontage::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());

	TArray<FAssetData> Assets;
	FableAnimPrivate::Registry().GetAssets(Filter, Assets);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	TArray<FJVal> Rows;
	for (const FAssetData& AD : Assets)
	{
		const FString Name = AD.AssetName.ToString();
		if (!NameContains.IsEmpty() && !Name.Contains(NameContains)) { continue; }

		UAnimationAsset* Anim = Cast<UAnimationAsset>(AD.GetAsset());
		if (!Anim) { continue; }

		FJObj R = NewObj();
		R->SetStringField(TEXT("name"), Name);
		R->SetStringField(TEXT("path"), Anim->GetPathName());
		R->SetStringField(TEXT("class"), Anim->GetClass()->GetName());
		if (const UAnimSequenceBase* Base = Cast<UAnimSequenceBase>(Anim))
		{
			R->SetNumberField(TEXT("play_length"), Base->GetPlayLength());
		}
		if (const UAnimSequence* Seq = Cast<UAnimSequence>(Anim))
		{
			R->SetBoolField(TEXT("root_motion"), Seq->bEnableRootMotion);
		}
		if (const UAnimMontage* M = Cast<UAnimMontage>(Anim))
		{
			R->SetStringField(TEXT("slot"), FableAnimPrivate::FirstSlotName(M));
			R->SetNumberField(TEXT("rate_scale"), M->RateScale);
			R->SetBoolField(TEXT("root_motion"), M->HasRootMotion());
		}
		Rows.Add(MakeShared<FJsonValueObject>(R));
	}
	O->SetNumberField(TEXT("count"), Rows.Num());
	O->SetArrayField(TEXT("anims"), Rows);
	return ToJson(O);
}


FString UFableAnim::ListReferencers(const FString& AssetPath)
{
	FString ErrMsg;
	UAnimationAsset* Anim = FableAnimPrivate::LoadAnim(AssetPath, ErrMsg);
	if (!Anim) { return Err(ErrMsg); }

	const TArray<FString> Refs = FableAnimPrivate::ReferencersOf(Anim);
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("asset"), Anim->GetPathName());
	O->SetNumberField(TEXT("num_referencers"), Refs.Num());
	TArray<FJVal> Vals;
	for (const FString& R : Refs) { Vals.Add(MakeShared<FJsonValueString>(R)); }
	O->SetArrayField(TEXT("referencers"), Vals);
	return ToJson(O);
}


FString UFableAnim::SetMontageSlot(const FString& MontagePath, const FString& SlotName, int32 TrackIndex)
{
	FString ErrMsg;
	if (!CheckMutate(ErrMsg)) { return Err(ErrMsg); }
	if (SlotName.IsEmpty()) { return Err(TEXT("SlotName is empty")); }

	UAnimMontage* M = LoadObject<UAnimMontage>(nullptr, *FableAnimPrivate::Norm(MontagePath));
	if (!M) { return Err(FString::Printf(TEXT("AnimMontage not found: %s"), *FableAnimPrivate::Norm(MontagePath))); }
	if (M->SlotAnimTracks.Num() == 0) { return Err(TEXT("Montage has no slot tracks")); }
	if (TrackIndex >= M->SlotAnimTracks.Num())
	{
		return Err(FString::Printf(TEXT("TrackIndex %d out of range (%d tracks)"),
			TrackIndex, M->SlotAnimTracks.Num()));
	}

	USkeleton* Skel = M->GetSkeleton();
	const FName NewSlot(*SlotName);

	/* ☠ A slot the skeleton has never heard of is not an error anywhere — the montage just plays
	 * with no visible pose. Register it, and tell the caller we had to, so a typo is visible as a
	 * brand-new one-slot group rather than as a silent no-op. */
	bool bRegistered = false;
	if (Skel && !Skel->ContainsSlotName(NewSlot))
	{
		Skel->Modify();
		bRegistered = Skel->RegisterSlotNode(NewSlot);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("FableAnim", "SetMontageSlot", "Set Montage Slot"));
	M->Modify();

	TArray<FString> Changed;
	for (int32 i = 0; i < M->SlotAnimTracks.Num(); ++i)
	{
		if (TrackIndex >= 0 && i != TrackIndex) { continue; }
		Changed.Add(FString::Printf(TEXT("%d:%s->%s"), i,
			*M->SlotAnimTracks[i].SlotName.ToString(), *SlotName));
		M->SlotAnimTracks[i].SlotName = NewSlot;
	}
	M->PostEditChange();
	M->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("montage"), M->GetPathName());
	O->SetStringField(TEXT("slot"), SlotName);
	O->SetBoolField(TEXT("registered_new_slot_on_skeleton"), bRegistered);
	TArray<FJVal> Vals;
	for (const FString& C : Changed) { Vals.Add(MakeShared<FJsonValueString>(C)); }
	O->SetArrayField(TEXT("changed"), Vals);
	if (Skel)
	{
		TArray<FString> All = FableAnimPrivate::SkeletonSlotNames(Skel);
		TArray<FJVal> AllVals;
		for (const FString& S : All) { AllVals.Add(MakeShared<FJsonValueString>(S)); }
		O->SetArrayField(TEXT("skeleton_slots"), AllVals);
	}
	return ToJson(O);
}


FString UFableAnim::CreateMontage(const FString& SequencePath, const FString& DestPath,
                                  const FString& SlotName, float RateScale)
{
	FString ErrMsg;
	if (!CheckMutate(ErrMsg)) { return Err(ErrMsg); }

	UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *FableAnimPrivate::Norm(SequencePath));
	if (!Seq) { return Err(FString::Printf(TEXT("AnimSequence not found: %s"), *FableAnimPrivate::Norm(SequencePath))); }

	const FString PackagePath = FPackageName::GetLongPackagePath(DestPath);
	const FString AssetName = FPackageName::GetShortName(DestPath);
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return Err(FString::Printf(TEXT("DestPath must be a full asset path like /Game/Anim/M_Thing: %s"), *DestPath));
	}
	// CREATE-ONLY: overwriting a live asset is how the editor gets wedged (see build-command memory).
	if (LoadObject<UObject>(nullptr, *FableAnimPrivate::Norm(DestPath)))
	{
		return Err(FString::Printf(TEXT("Refused: %s already exists. Delete it by hand or pick another name."), *DestPath));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	Factory->TargetSkeleton = Seq->GetSkeleton();
	Factory->SourceAnimation = Seq;

	UObject* Created = AssetTools.CreateAsset(AssetName, PackagePath, UAnimMontage::StaticClass(), Factory);
	UAnimMontage* M = Cast<UAnimMontage>(Created);
	if (!M) { return Err(FString::Printf(TEXT("CreateAsset failed for %s"), *DestPath)); }

	M->Modify();
	if (RateScale > 0.0f) { M->RateScale = RateScale; }

	// The whole point: land it on the requested slot instead of the factory's DefaultSlot.
	bool bRegistered = false;
	if (!SlotName.IsEmpty())
	{
		USkeleton* Skel = M->GetSkeleton();
		const FName NewSlot(*SlotName);
		if (Skel && !Skel->ContainsSlotName(NewSlot))
		{
			Skel->Modify();
			bRegistered = Skel->RegisterSlotNode(NewSlot);
		}
		for (FSlotAnimationTrack& T : M->SlotAnimTracks) { T.SlotName = NewSlot; }
	}
	M->PostEditChange();
	M->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("montage"), M->GetPathName());
	O->SetStringField(TEXT("slot"), FableAnimPrivate::FirstSlotName(M));
	O->SetNumberField(TEXT("play_length"), M->GetPlayLength());
	O->SetNumberField(TEXT("rate_scale"), M->RateScale);
	O->SetNumberField(TEXT("effective_seconds"), M->RateScale > 0.0f ? M->GetPlayLength() / M->RateScale : 0.0f);
	O->SetBoolField(TEXT("has_root_motion"), M->HasRootMotion());
	O->SetBoolField(TEXT("registered_new_slot_on_skeleton"), bRegistered);
	return ToJson(O);
}


FString UFableAnim::SetRootMotion(const FString& SequencePath, bool bEnable, bool bAllowShared,
                                  const FString& RootLock)
{
	FString ErrMsg;
	if (!CheckMutate(ErrMsg)) { return Err(ErrMsg); }

	UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *FableAnimPrivate::Norm(SequencePath));
	if (!Seq) { return Err(FString::Printf(TEXT("AnimSequence not found: %s"), *FableAnimPrivate::Norm(SequencePath))); }

	/* ☠ Root motion is a property of the CLIP, so flipping it changes every montage and every
	 * weapon that plays this sequence. Refuse by default when anything else references it and hand
	 * back the list — the caller almost always wants to duplicate first. */
	const TArray<FString> Refs = FableAnimPrivate::ReferencersOf(Seq);
	if (!bAllowShared && Refs.Num() > 0)
	{
		return ErrWithList(
			FString::Printf(TEXT("Refused: %s is referenced by %d other asset(s); flipping root motion would change all of them. Duplicate it first, or pass bAllowShared=True."),
				*Seq->GetName(), Refs.Num()),
			TEXT("referencers"), Refs);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("FableAnim", "SetRootMotion", "Set Root Motion"));
	Seq->Modify();
	const bool bWas = Seq->bEnableRootMotion;
	Seq->bEnableRootMotion = bEnable;

	FString LockApplied;
	if (!RootLock.IsEmpty())
	{
		static const TMap<FString, ERootMotionRootLock::Type> Locks = {
			{ TEXT("RefPose"),        ERootMotionRootLock::RefPose },
			{ TEXT("AnimFirstFrame"), ERootMotionRootLock::AnimFirstFrame },
			{ TEXT("Zero"),           ERootMotionRootLock::Zero },
		};
		if (const ERootMotionRootLock::Type* Found = Locks.Find(RootLock))
		{
			Seq->RootMotionRootLock = *Found;
			LockApplied = RootLock;
		}
		else
		{
			TArray<FString> Valid;
			Locks.GenerateKeyArray(Valid);
			return ErrWithList(FString::Printf(TEXT("Unknown RootLock '%s'"), *RootLock), TEXT("valid"), Valid);
		}
	}
	Seq->PostEditChange();
	Seq->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("sequence"), Seq->GetPathName());
	O->SetBoolField(TEXT("was"), bWas);
	O->SetBoolField(TEXT("root_motion"), Seq->bEnableRootMotion);
	O->SetStringField(TEXT("root_lock"), LockApplied);
	O->SetNumberField(TEXT("num_referencers"), Refs.Num());
	return ToJson(O);
}


FString UFableAnim::DuplicateAnim(const FString& SrcPath, const FString& DestPath)
{
	FString ErrMsg;
	if (!CheckMutate(ErrMsg)) { return Err(ErrMsg); }

	UAnimationAsset* Src = FableAnimPrivate::LoadAnim(SrcPath, ErrMsg);
	if (!Src) { return Err(ErrMsg); }

	const FString PackagePath = FPackageName::GetLongPackagePath(DestPath);
	const FString AssetName = FPackageName::GetShortName(DestPath);
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return Err(FString::Printf(TEXT("DestPath must be a full asset path like /Game/Anim/Foo_Copy: %s"), *DestPath));
	}
	if (LoadObject<UObject>(nullptr, *FableAnimPrivate::Norm(DestPath)))
	{
		return Err(FString::Printf(TEXT("Refused: %s already exists."), *DestPath));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Dup = AssetTools.DuplicateAsset(AssetName, PackagePath, Src);
	if (!Dup) { return Err(FString::Printf(TEXT("DuplicateAsset failed for %s -> %s"), *SrcPath, *DestPath)); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("source"), Src->GetPathName());
	O->SetStringField(TEXT("duplicate"), Dup->GetPathName());
	if (const UAnimSequence* Seq = Cast<UAnimSequence>(Dup))
	{
		O->SetBoolField(TEXT("root_motion"), Seq->bEnableRootMotion);
	}
	return ToJson(O);
}


FString UFableAnim::SetMontageTiming(const FString& MontagePath, float RateScale,
                                     float BlendInSeconds, float BlendOutSeconds)
{
	FString ErrMsg;
	if (!CheckMutate(ErrMsg)) { return Err(ErrMsg); }

	UAnimMontage* M = LoadObject<UAnimMontage>(nullptr, *FableAnimPrivate::Norm(MontagePath));
	if (!M) { return Err(FString::Printf(TEXT("AnimMontage not found: %s"), *FableAnimPrivate::Norm(MontagePath))); }

	const FScopedTransaction Transaction(NSLOCTEXT("FableAnim", "SetMontageTiming", "Set Montage Timing"));
	M->Modify();
	if (RateScale > 0.0f) { M->RateScale = RateScale; }
	if (BlendInSeconds >= 0.0f) { M->BlendIn.SetBlendTime(BlendInSeconds); }
	if (BlendOutSeconds >= 0.0f) { M->BlendOut.SetBlendTime(BlendOutSeconds); }
	M->PostEditChange();
	M->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("montage"), M->GetPathName());
	O->SetNumberField(TEXT("rate_scale"), M->RateScale);
	O->SetNumberField(TEXT("play_length"), M->GetPlayLength());
	O->SetNumberField(TEXT("effective_seconds"), M->RateScale > 0.0f ? M->GetPlayLength() / M->RateScale : 0.0f);
	O->SetNumberField(TEXT("blend_in"), M->BlendIn.GetBlendTime());
	O->SetNumberField(TEXT("blend_out"), M->BlendOut.GetBlendTime());
	O->SetStringField(TEXT("slot"), FableAnimPrivate::FirstSlotName(M));
	return ToJson(O);
}
