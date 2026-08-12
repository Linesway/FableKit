#include "FablePlay.h"

#include "AnimNodes/AnimNode_Slot.h"          // FAnimNode_Slot (AnimGraphRuntime)
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EngineUtils.h"                      // TActorIterator
#include "Components/SkeletalMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "UnrealEdGlobals.h"                  // GUnrealEd — StartPIE/StopPIE
#include "Editor/UnrealEdEngine.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"              // TObjectIterator — WidgetRect's live-widget scan
#include "Components/Widget.h"                    // UWidget geometry for WidgetRect
#include "Framework/Application/SlateApplication.h" // pointer injection — the Slate-level click driver
#include "InputCoreTypes.h"                       // EKeys for the injected pointer events
#include "Layout/WidgetPath.h"                    // FWidgetPath — the HitTest probe

namespace FablePlayInternal
{
	FString Fail(const FString& Why)
	{
		return FString::Printf(TEXT("{\"ok\":false,\"error\":\"%s\"}"), *Why.ReplaceCharWithEscapedChar());
	}

	/** The PIE world, or null. Deliberately NOT the editor world: everything here is about a
	 *  RUNNING game, and silently operating on the editor world would "succeed" while doing nothing
	 *  a player could ever see. */
	UWorld* PIEWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				return Ctx.World();
			}
		}
		return nullptr;
	}

	APlayerController* PC(int32 PlayerIndex)
	{
		UWorld* const W = PIEWorld();
		return W ? UGameplayStatics::GetPlayerController(W, PlayerIndex) : nullptr;
	}

	UEnhancedInputLocalPlayerSubsystem* InputSubsystem(int32 PlayerIndex)
	{
		APlayerController* const C = PC(PlayerIndex);
		ULocalPlayer* const LP = C ? C->GetLocalPlayer() : nullptr;
		return LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	}

	/** Accepts "/Game/A/IA_X" or "/Game/A/IA_X.IA_X", and bare "IA_X" by asset-registry lookup —
	 *  guessing the object-name suffix is the single most common way an automation call fails. */
	UInputAction* ResolveAction(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		FString Full = Path;
		if (!Full.Contains(TEXT(".")) && Full.StartsWith(TEXT("/")))
		{
			FString Leaf;
			Full.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			Full = Full + TEXT(".") + Leaf;
		}
		if (UInputAction* Direct = LoadObject<UInputAction>(nullptr, *Full))
		{
			return Direct;
		}
		// Bare name: ask the asset registry rather than making the caller know the folder.
		const FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		ARM.Get().GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, true);
		for (const FAssetData& A : Assets)
		{
			if (A.AssetName.ToString().Equals(Path, ESearchCase::IgnoreCase))
			{
				return Cast<UInputAction>(A.GetAsset());
			}
		}
		return nullptr;
	}

	/** Build a value of the action's OWN type.
	 *
	 * ☠ THIS IS NOT COSMETIC. FInputActionValue carries its value type, and Enhanced Input will not
	 * reconcile a mismatch: feeding an Axis3D vector to a Digital action injects a value the action
	 * cannot read, so the injection "succeeds", every call returns ok, and NOTHING HAPPENS ANYWHERE.
	 * That failure cost a full build cycle to find, because the game was ticking, the action asset
	 * was the right one, and the only symptom was silence. */
	FInputActionValue MakeValueFor(const UInputAction* Action, const FVector& V)
	{
		switch (Action ? Action->ValueType : EInputActionValueType::Boolean)
		{
		case EInputActionValueType::Boolean: return FInputActionValue(!FMath::IsNearlyZero(V.X));
		case EInputActionValueType::Axis1D:  return FInputActionValue(static_cast<float>(V.X));
		case EInputActionValueType::Axis2D:  return FInputActionValue(FVector2D(V.X, V.Y));
		default:                             return FInputActionValue(V);
		}
	}

	const TCHAR* ValueTypeName(const UInputAction* Action)
	{
		switch (Action ? Action->ValueType : EInputActionValueType::Boolean)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
		case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
		default:                             return TEXT("Axis3D");
		}
	}

	/** One running hold: which action, for whom, until when. */
	struct FHold
	{
		TWeakObjectPtr<UInputAction> Action;
		FVector Value = FVector::ZeroVector;
		int32 PlayerIndex = 0;
		double EndTime = 0.0;
		FTSTicker::FDelegateHandle Ticker;
	};

	static TMap<FString, TSharedPtr<FHold>> GHolds;

	void StopHold(const FString& Key)
	{
		if (TSharedPtr<FHold>* Found = GHolds.Find(Key))
		{
			if ((*Found)->Ticker.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker((*Found)->Ticker);
			}
			GHolds.Remove(Key);
		}
	}

	/** One sample row of a watch. */
	struct FWatchSample
	{
		double T = 0.0;
		FString Actor;
		TMap<FString, FString> Values;
	};

	struct FWatch
	{
		TArray<TWeakObjectPtr<AActor>> Actors;
		TArray<FString> Props;
		double EndTime = 0.0;
		double NextSample = 0.0;
		double Interval = 0.05;
		double StartTime = 0.0;
		TArray<FWatchSample> Samples;
		FTSTicker::FDelegateHandle Ticker;
		bool bRunning = false;
	};

	static TSharedPtr<FWatch> GWatch;

	FString EscapeJson(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Out.ReplaceInline(TEXT("\n"), TEXT(" "));
		Out.ReplaceInline(TEXT("\r"), TEXT(" "));
		Out.ReplaceInline(TEXT("\t"), TEXT(" "));
		return Out;
	}

	/** Read one property off an object by name, as a display string. Supports "Comp.Prop" so a
	 *  component's value is reachable without a second call. Returns false when the name resolves
	 *  to nothing, so the caller can say WHICH name was wrong rather than reporting an empty value. */
	bool ReadProp(UObject* Obj, const FString& PropPath, FString& Out)
	{
		if (!IsValid(Obj))
		{
			return false;
		}

		FString Head = PropPath;
		FString Tail;
		if (PropPath.Split(TEXT("."), &Head, &Tail))
		{
			// Component hop: find a component whose name or class matches Head, then recurse.
			if (const AActor* AsActor = Cast<AActor>(Obj))
			{
				for (UActorComponent* Comp : AsActor->GetComponents())
				{
					if (Comp && (Comp->GetName().Equals(Head, ESearchCase::IgnoreCase)
						|| Comp->GetClass()->GetName().Equals(Head, ESearchCase::IgnoreCase)))
					{
						return ReadProp(Comp, Tail, Out);
					}
				}
			}
			return false;
		}

		FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*Head));
		if (!Prop)
		{
			// Blueprint property names keep their authored spacing ("Time of Day"), so a second pass
			// matching on the display name is not a nicety — it is the only way to reach them.
			for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
			{
				if (It->GetName().Replace(TEXT(" "), TEXT("")).Equals(Head.Replace(TEXT(" "), TEXT("")),
					ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}
		if (!Prop)
		{
			return false;
		}

		const void* Value = Prop->ContainerPtrToValuePtr<void>(Obj);
		Prop->ExportTextItem_Direct(Out, Value, nullptr, Obj, PPF_None);
		return true;
	}

	FString VecJson(const FVector& V)
	{
		return FString::Printf(TEXT("{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}"), V.X, V.Y, V.Z);
	}

	/** Does the running anim graph actually contain a slot NODE with this name?
	 *
	 * IsSlotActive answers "is a montage playing there", which is the WRONG question — a missing
	 * slot also answers false, and the two failures need completely different fixes. This walks the
	 * generated class's FAnimNode_Slot properties, the same test the game's own montage relay uses. */
	bool GraphHasSlotNode(const UAnimInstance* Anim, FName SlotName)
	{
		if (!Anim || SlotName == NAME_None)
		{
			return false;
		}
		const IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(Anim->GetClass());
		if (!AnimClass)
		{
			return false;
		}
		const UObject* ClassDefaults = Anim->GetClass()->GetDefaultObject();
		for (const FStructProperty* NodeProp : AnimClass->GetAnimNodeProperties())
		{
			if (NodeProp && NodeProp->Struct && NodeProp->Struct->IsChildOf(FAnimNode_Slot::StaticStruct()))
			{
				const FAnimNode_Slot* SlotNode = NodeProp->ContainerPtrToValuePtr<FAnimNode_Slot>(ClassDefaults);
				if (SlotNode && SlotNode->SlotName == SlotName)
				{
					return true;
				}
			}
		}
		return false;
	}

	AActor* FindActorByName(const FString& Name)
	{
		UWorld* const W = PIEWorld();
		if (!W)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

using namespace FablePlayInternal;

FString UFablePlay::StartPIE()
{
	if (!GUnrealEd)
	{
		return TEXT("{\"ok\":false,\"error\":\"no GUnrealEd — not an editor build\"}");
	}
	if (GEditor && GEditor->PlayWorld)
	{
		return TEXT("{\"ok\":true,\"alreadyRunning\":true}");
	}
	// Default params = the toolbar Play button's request (selected viewport, in-process).
	FRequestPlaySessionParams Params;
	GUnrealEd->RequestPlaySession(Params);
	return TEXT("{\"ok\":true,\"alreadyRunning\":false}");
}

FString UFablePlay::StopPIE()
{
	const bool bWasRunning = GEditor && GEditor->PlayWorld != nullptr;
	if (GUnrealEd)
	{
		GUnrealEd->RequestEndPlayMap();
	}
	return FString::Printf(TEXT("{\"ok\":true,\"wasRunning\":%s}"),
		bWasRunning ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::Status()
{
	UWorld* const W = PIEWorld();
	if (!W)
	{
		return TEXT("{\"ok\":true,\"pie\":false}");
	}
	APlayerController* const C = UGameplayStatics::GetPlayerController(W, 0);
	APawn* const P = C ? C->GetPawn() : nullptr;
	return FString::Printf(
		TEXT("{\"ok\":true,\"pie\":true,\"world\":\"%s\",\"netMode\":%d,\"pawn\":\"%s\",\"holds\":%d,")
		TEXT("\"watching\":%s}"),
		*EscapeJson(W->GetName()), static_cast<int32>(W->GetNetMode()),
		P ? *EscapeJson(P->GetName()) : TEXT(""), GHolds.Num(),
		(GWatch.IsValid() && GWatch->bRunning) ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::ListInputActions(int32 PlayerIndex)
{
	const FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, true);

	FString Actions;
	for (const FAssetData& A : Assets)
	{
		if (!Actions.IsEmpty())
		{
			Actions += TEXT(",");
		}
		Actions += FString::Printf(TEXT("\"%s\""), *EscapeJson(A.GetSoftObjectPath().ToString()));
	}

	/* NOTE: there is no "is this action currently BOUND" field here, and that is a real gap — an
	 * action no active mapping context maps is bound to nothing, so injecting it does exactly
	 * nothing, with no error anywhere. Determine it empirically instead: inject once and check
	 * MontageState / PlayerSnapshot for a reaction. Worth adding properly if it ever costs a
	 * debugging session. */
	return FString::Printf(TEXT("{\"ok\":true,\"count\":%d,\"actions\":[%s],\"pie\":%s}"),
		Assets.Num(), *Actions, PIEWorld() ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::InjectAction(const FString& ActionPath, float X, float Y, float Z, int32 PlayerIndex)
{
	UEnhancedInputLocalPlayerSubsystem* const Sub = InputSubsystem(PlayerIndex);
	if (!Sub)
	{
		return Fail(TEXT("no PIE local player - start PIE first"));
	}
	UInputAction* const Action = ResolveAction(ActionPath);
	if (!Action)
	{
		return Fail(FString::Printf(TEXT("no InputAction '%s' - call ListInputActions"), *ActionPath));
	}

	Sub->InjectInputForAction(Action, MakeValueFor(Action, FVector(X, Y, Z)), {}, {});
	return FString::Printf(
		TEXT("{\"ok\":true,\"action\":\"%s\",\"valueType\":\"%s\",\"value\":%s,\"frames\":1}"),
		*EscapeJson(Action->GetName()), ValueTypeName(Action), *VecJson(FVector(X, Y, Z)));
}

FString UFablePlay::HoldAction(const FString& ActionPath, float Seconds, float X, float Y, float Z,
	int32 PlayerIndex)
{
	UEnhancedInputLocalPlayerSubsystem* const Sub = InputSubsystem(PlayerIndex);
	if (!Sub)
	{
		return Fail(TEXT("no PIE local player - start PIE first"));
	}
	UInputAction* const Action = ResolveAction(ActionPath);
	if (!Action)
	{
		return Fail(FString::Printf(TEXT("no InputAction '%s' - call ListInputActions"), *ActionPath));
	}

	const FString Key = Action->GetPathName();
	StopHold(Key);   // replace rather than stack: two holds on one action fight over the value

	TSharedPtr<FHold> Hold = MakeShared<FHold>();
	Hold->Action = Action;
	Hold->Value = FVector(X, Y, Z);
	Hold->PlayerIndex = PlayerIndex;
	Hold->EndTime = FPlatformTime::Seconds() + FMath::Max(Seconds, 0.0f);

	/* AN INJECTION LASTS ONE FRAME. Enhanced Input treats an action it was not fed this frame as
	 * released, so "hold" genuinely means re-inject every frame — which is also why this has to run
	 * on a ticker rather than a sleep: the game thread must be free to advance between injections. */
	Hold->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Key](float) -> bool
		{
			TSharedPtr<FHold>* Found = GHolds.Find(Key);
			if (!Found || !Found->IsValid())
			{
				return false;
			}
			FHold& H = **Found;
			UEnhancedInputLocalPlayerSubsystem* const S = InputSubsystem(H.PlayerIndex);
			if (!S || !H.Action.IsValid() || FPlatformTime::Seconds() >= H.EndTime)
			{
				GHolds.Remove(Key);   // stop feeding it; the next frame reads as the release
				return false;
			}
			S->InjectInputForAction(H.Action.Get(), MakeValueFor(H.Action.Get(), H.Value), {}, {});
			return true;
		}), 0.0f);

	GHolds.Add(Key, Hold);
	return FString::Printf(
		TEXT("{\"ok\":true,\"action\":\"%s\",\"valueType\":\"%s\",\"seconds\":%.3f,\"value\":%s}"),
		*EscapeJson(Action->GetName()), ValueTypeName(Action), Seconds, *VecJson(FVector(X, Y, Z)));
}

FString UFablePlay::ReleaseAction(const FString& ActionPath)
{
	if (ActionPath.IsEmpty())
	{
		const int32 N = GHolds.Num();
		TArray<FString> Keys;
		GHolds.GetKeys(Keys);
		for (const FString& K : Keys)
		{
			StopHold(K);
		}
		return FString::Printf(TEXT("{\"ok\":true,\"released\":%d}"), N);
	}

	UInputAction* const Action = ResolveAction(ActionPath);
	if (!Action)
	{
		return Fail(FString::Printf(TEXT("no InputAction '%s'"), *ActionPath));
	}
	StopHold(Action->GetPathName());
	return FString::Printf(TEXT("{\"ok\":true,\"released\":\"%s\"}"), *EscapeJson(Action->GetName()));
}

FString UFablePlay::CallFunction(const FString& Target, const FString& Call)
{
	UWorld* const W = PIEWorld();
	if (!W)
	{
		return Fail(TEXT("no PIE world"));
	}

	UObject* Obj = nullptr;
	if (Target.Equals(TEXT("player"), ESearchCase::IgnoreCase))
	{
		APlayerController* const C = PC(0);
		Obj = C ? Cast<UObject>(C->GetPawn()) : nullptr;
	}
	else if (Target.Equals(TEXT("weapon"), ESearchCase::IgnoreCase))
	{
		/* Pawn -> PlayerState -> CurrentWeapon, entirely by reflection. Naming the game's classes
		 * here would make this plugin depend on the game, which is backwards; a property hop costs
		 * nothing and keeps working when those headers move. */
		APlayerController* const C = PC(0);
		APawn* const P = C ? C->GetPawn() : nullptr;
		if (APlayerState* PS = P ? P->GetPlayerState() : nullptr)
		{
			if (FObjectProperty* Prop = CastField<FObjectProperty>(
				PS->GetClass()->FindPropertyByName(TEXT("CurrentWeapon"))))
			{
				Obj = Prop->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(PS));
			}
		}
	}
	else
	{
		Obj = FindActorByName(Target);
	}

	if (!IsValid(Obj))
	{
		return Fail(FString::Printf(TEXT("could not resolve target '%s'"), *Target));
	}

	// Name the function first so a typo is reported as a typo rather than as a silent no-op.
	FString FuncName = Call;
	FString Rest;
	Call.Split(TEXT(" "), &FuncName, &Rest);
	if (!Obj->FindFunction(FName(*FuncName)))
	{
		FString Available;
		int32 Shown = 0;
		for (TFieldIterator<UFunction> It(Obj->GetClass()); It && Shown < 40; ++It, ++Shown)
		{
			if (!Available.IsEmpty())
			{
				Available += TEXT(",");
			}
			Available += FString::Printf(TEXT("\"%s\""), *EscapeJson(It->GetName()));
		}
		return FString::Printf(
			TEXT("{\"ok\":false,\"error\":\"no function '%s' on %s\",\"target\":\"%s\",\"functions\":[%s]}"),
			*EscapeJson(FuncName), *EscapeJson(Obj->GetClass()->GetName()),
			*EscapeJson(Obj->GetName()), *Available);
	}

	/* CallFunctionByNameWithArguments does the console-style argument parsing for us — every
	 * parameter type that a console command can take, without this module owning a parser. */
	FStringOutputDevice Ar;
	const bool bOk = Obj->CallFunctionByNameWithArguments(*Call, Ar, nullptr, /*bForceCallWithNonExec=*/true);
	return FString::Printf(TEXT("{\"ok\":%s,\"target\":\"%s\",\"call\":\"%s\",\"output\":\"%s\"}"),
		bOk ? TEXT("true") : TEXT("false"), *EscapeJson(Obj->GetName()),
		*EscapeJson(Call), *EscapeJson(Ar));
}

FString UFablePlay::PlayerSnapshot(const FString& ExtraProps, int32 PlayerIndex)
{
	APlayerController* const C = PC(PlayerIndex);
	APawn* const P = C ? C->GetPawn() : nullptr;
	if (!P)
	{
		return Fail(TEXT("no player pawn - start PIE first"));
	}

	FString Extra;
	if (!ExtraProps.IsEmpty())
	{
		TArray<FString> Names;
		ExtraProps.ParseIntoArray(Names, TEXT(","), true);
		for (FString& N : Names)
		{
			N.TrimStartAndEndInline();
			FString Val;
			const bool bGot = ReadProp(P, N, Val);
			Extra += FString::Printf(TEXT(",\"%s\":%s"), *EscapeJson(N),
				bGot ? *FString::Printf(TEXT("\"%s\""), *EscapeJson(Val)) : TEXT("null"));
		}
	}

	FString Move = TEXT("null");
	if (const ACharacter* Ch = Cast<ACharacter>(P))
	{
		if (const UCharacterMovementComponent* M = Ch->GetCharacterMovement())
		{
			Move = FString::Printf(TEXT("{\"mode\":%d,\"falling\":%s,\"speed\":%.1f}"),
				static_cast<int32>(M->MovementMode), M->IsFalling() ? TEXT("true") : TEXT("false"),
				M->Velocity.Size());
		}
	}

	return FString::Printf(
		TEXT("{\"ok\":true,\"pawn\":\"%s\",\"loc\":%s,\"rot\":{\"pitch\":%.1f,\"yaw\":%.1f,\"roll\":%.1f},")
		TEXT("\"vel\":%s,\"movement\":%s%s}"),
		*EscapeJson(P->GetName()), *VecJson(P->GetActorLocation()),
		P->GetActorRotation().Pitch, P->GetActorRotation().Yaw, P->GetActorRotation().Roll,
		*VecJson(P->GetVelocity()), *Move, *Extra);
}

FString UFablePlay::MontageState(int32 PlayerIndex)
{
	APlayerController* const C = PC(PlayerIndex);
	APawn* const P = C ? C->GetPawn() : nullptr;
	USkeletalMeshComponent* const Mesh = P ? P->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
	UAnimInstance* const Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!Anim)
	{
		return Fail(TEXT("no anim instance on the player pawn"));
	}

	FString Playing;
	for (const FAnimMontageInstance* Inst : Anim->MontageInstances)
	{
		if (!Inst || !Inst->Montage || !Inst->IsPlaying())
		{
			continue;
		}
		const UAnimMontage* M = Inst->Montage;

		/* THE SLOT IS THE POINT. A montage on a slot the running graph has no node for advances
		 * silently with NO VISIBLE POSE — the animation "plays", every API agrees it is playing, and
		 * nothing moves. It is invisible from every other vantage point, and it has cost this
		 * project real days. Report the slot and whether the graph can actually play it. */
		FString Slots;
		for (const FSlotAnimationTrack& Track : M->SlotAnimTracks)
		{
			if (!Slots.IsEmpty())
			{
				Slots += TEXT(",");
			}
			Slots += FString::Printf(TEXT("{\"slot\":\"%s\",\"graphHasSlot\":%s,\"slotActive\":%s}"),
				*EscapeJson(Track.SlotName.ToString()),
				GraphHasSlotNode(Anim, Track.SlotName) ? TEXT("true") : TEXT("false"),
				Anim->IsSlotActive(Track.SlotName) ? TEXT("true") : TEXT("false"));
		}

		if (!Playing.IsEmpty())
		{
			Playing += TEXT(",");
		}
		Playing += FString::Printf(
			TEXT("{\"montage\":\"%s\",\"position\":%.3f,\"length\":%.3f,\"rate\":%.2f,\"weight\":%.2f,")
			TEXT("\"slots\":[%s]}"),
			*EscapeJson(M->GetName()), Inst->GetPosition(), M->GetPlayLength(),
			Inst->GetPlayRate(), Inst->GetWeight(), *Slots);
	}

	return FString::Printf(TEXT("{\"ok\":true,\"playing\":[%s]}"), *Playing);
}

FString UFablePlay::FindActors(const FString& ClassPath, float X, float Y, float Z, float Radius)
{
	UWorld* const W = PIEWorld();
	if (!W)
	{
		return Fail(TEXT("no PIE world"));
	}
	UClass* const Cls = LoadClass<AActor>(nullptr, *ClassPath);
	if (!Cls)
	{
		return Fail(FString::Printf(TEXT("no class '%s' (try '/Game/..../BP_X.BP_X_C')"), *ClassPath));
	}

	const FVector Origin(X, Y, Z);
	TArray<TPair<double, AActor*>> Hits;
	for (TActorIterator<AActor> It(W, Cls); It; ++It)
	{
		AActor* const A = *It;
		const double D = FVector::Dist(A->GetActorLocation(), Origin);
		if (Radius > 0.0f && D > Radius)
		{
			continue;
		}
		Hits.Add({ D, A });
	}
	Hits.Sort([](const TPair<double, AActor*>& L, const TPair<double, AActor*>& R) { return L.Key < R.Key; });

	FString Rows;
	for (const TPair<double, AActor*>& H : Hits)
	{
		if (!Rows.IsEmpty())
		{
			Rows += TEXT(",");
		}
		Rows += FString::Printf(TEXT("{\"name\":\"%s\",\"loc\":%s,\"dist\":%.1f}"),
			*EscapeJson(H.Value->GetName()), *VecJson(H.Value->GetActorLocation()), H.Key);
	}
	return FString::Printf(TEXT("{\"ok\":true,\"count\":%d,\"actors\":[%s]}"), Hits.Num(), *Rows);
}

FString UFablePlay::GetProps(const FString& ActorName, const FString& Props)
{
	AActor* const A = FindActorByName(ActorName);
	if (!A)
	{
		return Fail(FString::Printf(TEXT("no actor named '%s' in the PIE world"), *ActorName));
	}

	TArray<FString> Names;
	Props.ParseIntoArray(Names, TEXT(","), true);
	FString Rows;
	for (FString& N : Names)
	{
		N.TrimStartAndEndInline();
		FString Val;
		const bool bGot = ReadProp(A, N, Val);
		if (!Rows.IsEmpty())
		{
			Rows += TEXT(",");
		}
		Rows += FString::Printf(TEXT("\"%s\":%s"), *EscapeJson(N),
			bGot ? *FString::Printf(TEXT("\"%s\""), *EscapeJson(Val)) : TEXT("null"));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"actor\":\"%s\",\"props\":{%s}}"),
		*EscapeJson(A->GetName()), *Rows);
}

FString UFablePlay::WatchProps(const FString& ActorNames, const FString& Props, float Seconds,
	float IntervalSeconds)
{
	UWorld* const W = PIEWorld();
	if (!W)
	{
		return Fail(TEXT("no PIE world"));
	}

	if (GWatch.IsValid() && GWatch->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GWatch->Ticker);
	}

	TSharedPtr<FWatch> Watch = MakeShared<FWatch>();
	TArray<FString> Names;
	ActorNames.ParseIntoArray(Names, TEXT(","), true);
	for (FString& N : Names)
	{
		N.TrimStartAndEndInline();
		if (AActor* A = FindActorByName(N))
		{
			Watch->Actors.Add(A);
		}
	}
	if (Watch->Actors.Num() == 0)
	{
		return Fail(FString::Printf(TEXT("none of '%s' found in the PIE world"), *ActorNames));
	}

	Props.ParseIntoArray(Watch->Props, TEXT(","), true);
	for (FString& P : Watch->Props)
	{
		P.TrimStartAndEndInline();
	}

	Watch->StartTime = FPlatformTime::Seconds();
	Watch->EndTime = Watch->StartTime + FMath::Max(Seconds, 0.0f);
	Watch->Interval = FMath::Max(static_cast<double>(IntervalSeconds), 0.005);
	Watch->NextSample = Watch->StartTime;
	Watch->bRunning = true;

	Watch->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			if (!GWatch.IsValid() || !GWatch->bRunning)
			{
				return false;
			}
			FWatch& Wt = *GWatch;
			const double Now = FPlatformTime::Seconds();
			if (Now >= Wt.EndTime)
			{
				Wt.bRunning = false;
				return false;
			}
			if (Now < Wt.NextSample)
			{
				return true;
			}
			Wt.NextSample = Now + Wt.Interval;

			for (const TWeakObjectPtr<AActor>& Weak : Wt.Actors)
			{
				AActor* const A = Weak.Get();
				if (!IsValid(A))
				{
					continue;
				}
				FWatchSample S;
				S.T = Now - Wt.StartTime;
				S.Actor = A->GetName();
				for (const FString& P : Wt.Props)
				{
					FString Val;
					if (ReadProp(A, P, Val))
					{
						S.Values.Add(P, Val);
					}
				}
				Wt.Samples.Add(MoveTemp(S));
			}
			return true;
		}), 0.0f);

	GWatch = Watch;
	return FString::Printf(TEXT("{\"ok\":true,\"actors\":%d,\"props\":%d,\"seconds\":%.2f}"),
		Watch->Actors.Num(), Watch->Props.Num(), Seconds);
}

FString UFablePlay::PollWatch()
{
	if (!GWatch.IsValid())
	{
		return TEXT("{\"ok\":true,\"running\":false,\"samples\":[]}");
	}

	FString Rows;
	for (const FWatchSample& S : GWatch->Samples)
	{
		FString Vals;
		for (const TPair<FString, FString>& KV : S.Values)
		{
			Vals += FString::Printf(TEXT(",\"%s\":\"%s\""), *EscapeJson(KV.Key), *EscapeJson(KV.Value));
		}
		if (!Rows.IsEmpty())
		{
			Rows += TEXT(",");
		}
		Rows += FString::Printf(TEXT("{\"t\":%.3f,\"actor\":\"%s\"%s}"),
			S.T, *EscapeJson(S.Actor), *Vals);
	}
	const bool bRunning = GWatch->bRunning;
	GWatch->Samples.Reset();   // drain, so repeated polls stream rather than repeat

	return FString::Printf(TEXT("{\"ok\":true,\"running\":%s,\"samples\":[%s]}"),
		bRunning ? TEXT("true") : TEXT("false"), *Rows);
}

FString UFablePlay::TeleportPlayer(float X, float Y, float Z, bool bSweep, int32 PlayerIndex)
{
	APlayerController* const C = PC(PlayerIndex);
	APawn* const P = C ? C->GetPawn() : nullptr;
	if (!P)
	{
		return Fail(TEXT("no player pawn"));
	}
	const bool bMoved = P->SetActorLocation(FVector(X, Y, Z), bSweep, nullptr, ETeleportType::TeleportPhysics);
	return FString::Printf(TEXT("{\"ok\":true,\"moved\":%s,\"loc\":%s}"),
		bMoved ? TEXT("true") : TEXT("false"), *VecJson(P->GetActorLocation()));
}

FString UFablePlay::SpawnActor(const FString& ClassPath, float X, float Y, float Z,
	bool bRelativeToPlayer, int32 PlayerIndex)
{
	UWorld* const W = PIEWorld();
	if (!W)
	{
		return Fail(TEXT("no PIE world"));
	}
	UClass* const Cls = LoadClass<AActor>(nullptr, *ClassPath);
	if (!Cls)
	{
		return Fail(FString::Printf(TEXT("no class '%s'"), *ClassPath));
	}

	FVector Where(X, Y, Z);
	FRotator Facing = FRotator::ZeroRotator;
	if (bRelativeToPlayer)
	{
		APlayerController* const C = PC(PlayerIndex);
		APawn* const P = C ? C->GetPawn() : nullptr;
		if (!P)
		{
			return Fail(TEXT("no player pawn to place relative to"));
		}
		/* X forward, Y right, Z up IN THE PLAYER'S FRAME. A test that says "350 in front of me"
		 * then reads the same wherever in the world it is run, which is what makes a repro
		 * repeatable across sessions and across worlds. */
		const FRotator Yaw(0.0, P->GetActorRotation().Yaw, 0.0);
		Where = P->GetActorLocation() + Yaw.RotateVector(FVector(X, Y, Z));
		Facing = FRotator(0.0, Yaw.Yaw + 180.0, 0.0);   // face the player
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AActor* const Spawned = W->SpawnActor<AActor>(Cls, Where, Facing, Params);
	if (!Spawned)
	{
		return Fail(TEXT("spawn failed"));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"name\":\"%s\",\"loc\":%s}"),
		*EscapeJson(Spawned->GetName()), *VecJson(Spawned->GetActorLocation()));
}

/* ==================================================================================================
 * Pointer (Slate-level) — see the header block. Everything below the OS runs exactly as for a
 * physical mouse; nothing here moves the machine's cursor.
 * ================================================================================================== */

namespace FablePlayInternal
{
	FPointerEvent MakePointer(const FVector2D& Pos, const FKey& Button, bool bButtonDown)
	{
		TSet<FKey> Pressed;
		if (bButtonDown)
		{
			Pressed.Add(Button);
		}
		return FPointerEvent(/*PointerIndex=*/0, Pos, Pos, Pressed, Button,
			/*WheelDelta=*/0.0f, FModifierKeysState());
	}

	/** Move + press(+double) + release at desktop coords, through FSlateApplication's own entry
	 *  points — the exact calls the platform layer makes for a physical mouse. */
	bool InjectClick(const FVector2D& Pos, bool bRight, bool bDouble)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& App = FSlateApplication::Get();
		const FKey Button = bRight ? EKeys::RightMouseButton : EKeys::LeftMouseButton;

		// Move first so hover/hit-test state matches a real approach (enter chains, tooltips).
		App.ProcessMouseMoveEvent(MakePointer(Pos, EKeys::Invalid, false));
		if (bDouble)
		{
			App.ProcessMouseButtonDoubleClickEvent(nullptr, MakePointer(Pos, Button, true));
		}
		else
		{
			App.ProcessMouseButtonDownEvent(nullptr, MakePointer(Pos, Button, true));
		}
		App.ProcessMouseButtonUpEvent(MakePointer(Pos, Button, false));
		return true;
	}

	/** The pointer-rest's state. One at a time, like the hammer and the input holds. */
	struct FRest
	{
		FVector2D Pos = FVector2D::ZeroVector;
		double Until = 0.0;
		FTSTicker::FDelegateHandle Ticker;
	};
	static TSharedPtr<FRest> GRest;

	/** The hammer's state — one at a time, like the input holds. */
	struct FHammer
	{
		FVector2D Pos = FVector2D::ZeroVector;
		int32 Remaining = 0;
		double Interval = 0.12;
		double NextClick = 0.0;
		double LastPress = -1000.0;
		bool bRight = false;
		FTSTicker::FDelegateHandle Ticker;
	};
	static TSharedPtr<FHammer> GHammer;
}

FString UFablePlay::WidgetRect(const FString& Pattern, int32 Index)
{
	using namespace FablePlayInternal;
	UWorld* const W = PIEWorld();
	if (!W)
	{
		return Fail(TEXT("no PIE session"));
	}
	FString Rows;
	int32 Matches = 0;
	/* ☠ `It`, NOT `*It`. TObjectIterator's operator* INDEXES the global object array, so using it as
	 * the loop condition dereferences one past the end on the final step and takes the whole editor
	 * down with `Array index out of bounds: 9280 into an array of size 9280`. The bool conversion is
	 * the only safe test. This crashed the editor on the first call that reached the end of the
	 * iteration — i.e. every call whose Pattern did not match something early. */
	for (TObjectIterator<UWidget> It; It; ++It)
	{
		UWidget* const Widget = *It;
		if (!IsValid(Widget) || Widget->GetWorld() != W)
		{
			continue;
		}
		const FString Name = Widget->GetName();
		const FString ClassName = Widget->GetClass()->GetName();
		if (!Pattern.IsEmpty() && !Name.Contains(Pattern) && !ClassName.Contains(Pattern))
		{
			continue;
		}
		const FGeometry Geo = Widget->GetCachedGeometry();
		const FVector2D Size = Geo.GetAbsoluteSize();
		if (Index >= 0 && Matches != Index)
		{
			++Matches;
			continue;
		}
		const FVector2D TopLeft = Geo.GetAbsolutePosition();
		Rows += FString::Printf(TEXT("%s{\"name\":\"%s\",\"class\":\"%s\",\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f,\"cx\":%.0f,\"cy\":%.0f}"),
			Rows.IsEmpty() ? TEXT("") : TEXT(","),
			*EscapeJson(Name), *EscapeJson(ClassName),
			TopLeft.X, TopLeft.Y, Size.X, Size.Y,
			TopLeft.X + Size.X * 0.5f, TopLeft.Y + Size.Y * 0.5f);
		++Matches;
		if (Index >= 0 || Matches >= 40)
		{
			break;
		}
	}
	return FString::Printf(TEXT("{\"ok\":true,\"matches\":%d,\"widgets\":[%s]}"), Matches, *Rows);
}

FString UFablePlay::PointerClick(float X, float Y, bool bRight, bool bDouble)
{
	using namespace FablePlayInternal;
	if (!PIEWorld())
	{
		return Fail(TEXT("no PIE session"));
	}
	if (!InjectClick(FVector2D(X, Y), bRight, bDouble))
	{
		return Fail(TEXT("Slate not initialized"));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"x\":%.0f,\"y\":%.0f,\"right\":%s,\"double\":%s}"),
		X, Y, bRight ? TEXT("true") : TEXT("false"), bDouble ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::PointerHammer(float X, float Y, int32 Count, float ClicksPerSecond, bool bRight)
{
	using namespace FablePlayInternal;
	if (!PIEWorld())
	{
		return Fail(TEXT("no PIE session"));
	}
	if (Count <= 0 || ClicksPerSecond <= 0.0f)
	{
		return Fail(TEXT("Count and ClicksPerSecond must be positive"));
	}
	if (GHammer.IsValid() && GHammer->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GHammer->Ticker);   // one hammer at a time
	}

	TSharedPtr<FHammer> Hammer = MakeShared<FHammer>();
	Hammer->Pos = FVector2D(X, Y);
	Hammer->Remaining = Count;
	Hammer->Interval = 1.0 / static_cast<double>(ClicksPerSecond);
	Hammer->NextClick = FPlatformTime::Seconds();
	Hammer->bRight = bRight;

	Hammer->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			if (!GHammer.IsValid() || GHammer->Remaining <= 0 || !PIEWorld())
			{
				return false;
			}
			FHammer& H = *GHammer;
			const double Now = FPlatformTime::Seconds();
			if (Now < H.NextClick)
			{
				return true;
			}
			H.NextClick = Now + H.Interval;

			/* The platform's own synthesis, reproduced: a press inside the OS double-click window
			 * of the previous press at the same spot goes out as the DOUBLE-CLICK event — that is
			 * the event stream a hammering human actually produces, and the half of it a handler
			 * call can never exercise. 0.5s = the Windows default (Slate itself never sees the OS
			 * value — the platform layer synthesizes doubles before Slate, which is why there is
			 * no FSlateApplication getter to ask). */
			const double DoubleWindow = 0.5;
			const bool bDouble = (Now - H.LastPress) < DoubleWindow;
			H.LastPress = Now;
			InjectClick(H.Pos, H.bRight, bDouble);
			--H.Remaining;
			return H.Remaining > 0;
		}), 0.0f);

	GHammer = Hammer;
	return FString::Printf(TEXT("{\"ok\":true,\"clicks\":%d,\"cps\":%.1f}"), Count, ClicksPerSecond);
}

FString UFablePlay::PointerMove(float X, float Y)
{
	using namespace FablePlayInternal;
	if (!PIEWorld())
	{
		return Fail(TEXT("no PIE session"));
	}
	if (!FSlateApplication::IsInitialized())
	{
		return Fail(TEXT("Slate not initialized"));
	}
	FSlateApplication::Get().ProcessMouseMoveEvent(MakePointer(FVector2D(X, Y), EKeys::Invalid, false));
	return FString::Printf(TEXT("{\"ok\":true,\"x\":%.0f,\"y\":%.0f}"), X, Y);
}

FString UFablePlay::PointerRest(float X, float Y, float Seconds)
{
	using namespace FablePlayInternal;
	if (!PIEWorld())
	{
		return Fail(TEXT("no PIE session"));
	}
	if (!FSlateApplication::IsInitialized())
	{
		return Fail(TEXT("Slate not initialized"));
	}
	if (Seconds <= 0.0f)
	{
		return Fail(TEXT("Seconds must be positive"));
	}
	if (GRest.IsValid() && GRest->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GRest->Ticker);   // one rest at a time
	}

	TSharedPtr<FRest> Rest = MakeShared<FRest>();
	Rest->Pos = FVector2D(X, Y);
	Rest->Until = FPlatformTime::Seconds() + static_cast<double>(Seconds);

	/* RE-INJECT EVERY FRAME, and that is the whole point of this over PointerMove.
	 *
	 * A single move sets Slate's hover and then the next frame can take it straight back: the
	 * platform layer re-derives the cursor from the REAL mouse, which is wherever the human left
	 * it. Anything that asks "has the cursor RESTED here" — a hold-to-reveal, a tooltip delay, a
	 * hover cue gate — therefore never arms from one move, and the caller cannot hold it either,
	 * because a call through the Remote Control bridge OWNS THE GAME THREAD for its duration and
	 * the game cannot tick while you wait inside one.
	 *
	 * So the rest lives on the ticker instead: the pointer is put back every frame for Seconds,
	 * the game ticks normally throughout, and the caller returns immediately and reads the result
	 * in a LATER call. */
	Rest->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			if (!GRest.IsValid() || !PIEWorld() || !FSlateApplication::IsInitialized())
			{
				return false;
			}
			if (FPlatformTime::Seconds() >= GRest->Until)
			{
				return false;   // let go; the next real mouse move owns the cursor again
			}
			FSlateApplication::Get().ProcessMouseMoveEvent(
				MakePointer(GRest->Pos, EKeys::Invalid, false));
			return true;
		}), 0.0f);

	GRest = Rest;
	return FString::Printf(TEXT("{\"ok\":true,\"x\":%.0f,\"y\":%.0f,\"seconds\":%.2f}"),
		X, Y, Seconds);
}

FString UFablePlay::HitTest(float X, float Y)
{
	using namespace FablePlayInternal;
	if (!FSlateApplication::IsInitialized())
	{
		return Fail(TEXT("Slate not initialized"));
	}
	FSlateApplication& App = FSlateApplication::Get();

	// The same lookup a real press performs: window-under-point, then the bubble path within it.
	FWidgetPath Path = App.LocateWindowUnderMouse(FVector2D(X, Y), App.GetInteractiveTopLevelWindows(),
		/*bIgnoreEnabledStatus=*/false);
	if (!Path.IsValid())
	{
		return FString(TEXT("{\"ok\":true,\"widgets\":[]}"));
	}

	FString Rows;
	// Deepest widget = the one a press is offered first on the bubble-up — list it FIRST.
	for (int32 i = Path.Widgets.Num() - 1; i >= 0; --i)
	{
		const TSharedRef<SWidget> W = Path.Widgets[i].Widget;
		Rows += FString::Printf(TEXT("%s{\"type\":\"%s\",\"debug\":\"%s\",\"vis\":\"%s\"}"),
			Rows.IsEmpty() ? TEXT("") : TEXT(","),
			*EscapeJson(W->GetTypeAsString()),
			*EscapeJson(W->ToString()),
			*EscapeJson(W->GetVisibility().ToString()));
	}
	return FString::Printf(TEXT("{\"ok\":true,\"widgets\":[%s]}"), *Rows);
}
