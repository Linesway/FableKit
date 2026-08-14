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
#include "EnhancedPlayerInput.h"                  // GetActionValue / FindActionInstanceData — the "did the action fire" half
#include "EnhancedActionKeyMapping.h"             // FEnhancedActionKeyMapping — the BOUND/UNBOUND answer
#include "InputActionValue.h"
#include "InputTriggers.h"                        // ETriggerEvent
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
#include "Components/SceneCaptureComponent2D.h"   // CaptureScreen — render the world, never photograph the window
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/GameViewportClient.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/KismetRenderingLibrary.h"        // ExportRenderTarget -> PNG
#include "Slate/WidgetRenderer.h"                 // the UMG half of the composite
#include "Slate/SGameLayerManager.h"              // the game's LIVE Slate overlay, HUD and all
#include "RenderingThread.h"                      // FlushRenderingCommands
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"                    // ☠ THE REAL DOOR: APlayerController::InputKey's args
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"   // a VALID FInputDeviceId, not a default one
#include "GameFramework/InputSettings.h"
#include "CoreGlobals.h"                          // GFrameCounter — one analog sample per frame, never two

namespace FablePlayInternal
{
	/* ☠☠ THIS ESCAPER USED TO BE `ReplaceCharWithEscapedChar()`, AND IT DESTROYED EVERY ERROR MESSAGE
	 * THE FILE TAKES CARE TO WRITE.
	 *
	 * That function escapes C-style, so a single quote becomes `\'` — which JSON does not permit
	 * (only \" \\ \/ \b \f \n \r \t \uXXXX are legal). Nearly every Fail() here names the offending
	 * token in quotes ("unknown step or key '2'", "no key '%s' - call ListKeys"), so nearly every
	 * error came back as INVALID JSON and every caller reported a generic parse failure instead of
	 * the diagnostic. Measured 08-14: `run_sequence("2 tap")` printed `unknown step or key \'2\'`,
	 * and play.py turned that into "could not start: unparsed reply" — the one message that tells
	 * you nothing, standing in for the one that told you exactly what to fix.
	 *
	 * A tool whose error path is broken is worse than one with no error path, because the failure it
	 * reports is its own.
	 *
	 * The replacement is the ENGINE's own escaper (Serialization/JsonWriter.h), not a hand-rolled
	 * one — a second local copy shadowed it and merely produced an ambiguous-call build error, which
	 * was the compiler pointing out that the correct function was already in scope. */
	FString Fail(const FString& Why)
	{
		// ☠ The engine's EscapeJsonString ADDS THE SURROUNDING QUOTES, so the format string must not
		// supply its own — hence `:%s}` and not `:\"%s\"}`.
		return FString::Printf(TEXT("{\"ok\":false,\"error\":%s}"), *EscapeJsonString(Why));
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

/* ==================================================================================================
 * REAL KEYS — APlayerController::InputKey, the door the viewport uses for a physical keyboard.
 *
 * ☠ NOTHING BELOW TOUCHES THE OS. No SetCursorPos, no SendInput, no SetForegroundWindow. The call
 * chain is entirely in-process: InputKey -> UPlayerInput::InputKey -> KeyStateMap ->
 * UEnhancedPlayerInput::ProcessInputStack. Enhanced Input evaluates every applied mapping context
 * against that key-state table, which is exactly why this works where InjectInputForAction does not:
 * an injection is a per-frame side channel that never writes the table at all.
 * ================================================================================================== */

namespace FablePlayInternal
{
	/** FString -> FKey, accepting the names a human types as well as the engine's own.
	 *  An unknown name comes back as EKeys::Invalid so the caller is told, never silently ignored. */
	FKey ResolveKey(const FString& In)
	{
		FString N = In;
		N.TrimStartAndEndInline();
		if (N.IsEmpty())
		{
			return EKeys::Invalid;
		}

		/* The aliases are not sugar: "Space" and "Esc" and "LMB" are what a test script says, and
		 * every one of them is a MISS against the engine's table ("SpaceBar", "Escape",
		 * "LeftMouseButton"). Without this a perfectly reasonable script fails on the key name. */
		static const TMap<FString, FKey> Aliases =
		{
			{ TEXT("lmb"),   EKeys::LeftMouseButton },
			{ TEXT("rmb"),   EKeys::RightMouseButton },
			{ TEXT("mmb"),   EKeys::MiddleMouseButton },
			{ TEXT("mouse1"), EKeys::LeftMouseButton },
			{ TEXT("mouse2"), EKeys::RightMouseButton },
			{ TEXT("mouse3"), EKeys::MiddleMouseButton },
			{ TEXT("space"), EKeys::SpaceBar },
			{ TEXT("esc"),   EKeys::Escape },
			{ TEXT("ctrl"),  EKeys::LeftControl },
			{ TEXT("control"), EKeys::LeftControl },
			{ TEXT("shift"), EKeys::LeftShift },
			{ TEXT("alt"),   EKeys::LeftAlt },
			{ TEXT("return"), EKeys::Enter },
			{ TEXT("wheelup"),   EKeys::MouseScrollUp },
			{ TEXT("wheeldown"), EKeys::MouseScrollDown },
		};
		if (const FKey* Alias = Aliases.Find(N.ToLower()))
		{
			return *Alias;
		}

		const FKey Direct(*N);
		if (Direct.IsValid())
		{
			return Direct;
		}

		// Last chance: a case-insensitive sweep of the whole key table, so "spacebar" also lands.
		TArray<FKey> All;
		EKeys::GetAllKeys(All);
		for (const FKey& Candidate : All)
		{
			if (Candidate.GetFName().ToString().Equals(N, ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}
		return EKeys::Invalid;
	}

	/** ☠ A DEFAULT-CONSTRUCTED FInputDeviceId IS INPUTDEVICEID_NONE, and APlayerController::InputKey
	 *  DROPS the event outright when UInputSettings::bFilterInputByPlatformUser is on and the device
	 *  does not map to this controller's platform user. Resolve the player's own device, and only
	 *  fall back to the platform default. */
	FInputDeviceId DeviceFor(const APlayerController* C)
	{
		IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
		if (C)
		{
			const FPlatformUserId User = C->GetPlatformUserId();
			if (User.IsValid())
			{
				const FInputDeviceId Primary = Mapper.GetPrimaryInputDeviceForUser(User);
				if (Primary.IsValid())
				{
					return Primary;
				}
			}
		}
		return Mapper.GetDefaultInputDevice();
	}

	/** The one place a key event is manufactured. NumSamples < 0 = the engine's own default
	 *  (1 for analog, 0 for digital) — MouseX/MouseY ensure() on a zero sample count, and
	 *  CreateSimulated's default is what keeps them legal. */
	bool SendKey(int32 PlayerIndex, const FKey& Key, EInputEvent Event, float Amount, int32 NumSamples = -1)
	{
		APlayerController* const C = PC(PlayerIndex);
		if (!C || !Key.IsValid())
		{
			return false;
		}
		const FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(
			Key, Event, Amount, NumSamples, DeviceFor(C));
		return C->InputKey(Args);
	}

	/** One key held by this harness: which key, for whom, until when, and how it ends. */
	struct FKeyHold
	{
		FKey Key;
		int32 PlayerIndex = 0;
		float Amount = 1.0f;
		bool bAnalog = false;
		double EndTime = 0.0;
		uint64 LastFrame = 0;
		FTSTicker::FDelegateHandle Ticker;
	};

	static TMap<FString, TSharedPtr<FKeyHold>> GKeyHolds;

	FString KeyHoldId(int32 PlayerIndex, const FKey& Key)
	{
		return FString::Printf(TEXT("%d:%s"), PlayerIndex, *Key.GetFName().ToString());
	}

	/** End a hold, and — this is the part that matters — end it with a REAL release event.
	 *  Merely stopping the feed is how the old action-injection "release" worked, and a starved
	 *  frame is not a release: the key-state table keeps bDown until something says otherwise. */
	void EndKeyHold(const FString& Id, bool bSendRelease)
	{
		TSharedPtr<FKeyHold>* Found = GKeyHolds.Find(Id);
		if (!Found || !Found->IsValid())
		{
			return;
		}
		const TSharedPtr<FKeyHold> Hold = *Found;
		GKeyHolds.Remove(Id);
		if (Hold->Ticker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Hold->Ticker);
		}
		if (bSendRelease)
		{
			if (Hold->bAnalog)
			{
				SendKey(Hold->PlayerIndex, Hold->Key, IE_Axis, 0.0f, 1);   // a stuck stick is worse than no stick
			}
			else
			{
				SendKey(Hold->PlayerIndex, Hold->Key, IE_Released, 0.0f);
			}
		}
	}

	/** Press (or deflect) now, re-arm every frame, release after Seconds.
	 *  Seconds <= 0 means TAP: pressed this frame, released on the next tick. */
	void StartKeyHold(int32 PlayerIndex, const FKey& Key, float Seconds, float Amount)
	{
		const FString Id = KeyHoldId(PlayerIndex, Key);
		EndKeyHold(Id, /*bSendRelease=*/true);   // replace rather than stack

		TSharedPtr<FKeyHold> Hold = MakeShared<FKeyHold>();
		Hold->Key = Key;
		Hold->PlayerIndex = PlayerIndex;
		Hold->Amount = Amount;
		Hold->bAnalog = Key.IsAnalog();
		Hold->EndTime = FPlatformTime::Seconds() + FMath::Max(Seconds, 0.0f);
		Hold->LastFrame = GFrameCounter;

		SendKey(PlayerIndex, Key, Hold->bAnalog ? IE_Axis : IE_Pressed, Amount, Hold->bAnalog ? 1 : -1);

		Hold->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Id](float) -> bool
			{
				TSharedPtr<FKeyHold>* Found = GKeyHolds.Find(Id);
				if (!Found || !Found->IsValid())
				{
					return false;
				}
				FKeyHold& H = **Found;
				if (!PC(H.PlayerIndex) || FPlatformTime::Seconds() >= H.EndTime)
				{
					EndKeyHold(Id, /*bSendRelease=*/PC(H.PlayerIndex) != nullptr);
					return false;
				}
				/* ☠ ONE SAMPLE PER FRAME. The analog path ACCUMULATES AmountDepressed, so two
				 * injections between two EvaluateKeyMapState passes read as a doubled stick. */
				if (H.LastFrame != GFrameCounter)
				{
					H.LastFrame = GFrameCounter;
					/* A digital key needs no re-arm to stay down (bDown is maintained when the press
					 * delta is zero), but IE_Repeat is what the OS actually sends and it re-asserts
					 * the value if anything flushed it. An analog axis DOES need it: with no samples
					 * and no UpdateAxisWithoutSamples flag its value would simply stick. */
					SendKey(H.PlayerIndex, H.Key, H.bAnalog ? IE_Axis : IE_Repeat, H.Amount,
						H.bAnalog ? 1 : -1);
				}
				return true;
			}), 0.0f);

		GKeyHolds.Add(Id, Hold);
	}

	int32 ReleaseAllHeldKeys()
	{
		TArray<FString> Ids;
		GKeyHolds.GetKeys(Ids);
		for (const FString& Id : Ids)
		{
			EndKeyHold(Id, /*bSendRelease=*/true);
		}
		return Ids.Num();
	}

	const TCHAR* TriggerEventName(ETriggerEvent E)
	{
		switch (E)
		{
		case ETriggerEvent::Triggered: return TEXT("Triggered");
		case ETriggerEvent::Started:   return TEXT("Started");
		case ETriggerEvent::Ongoing:   return TEXT("Ongoing");
		case ETriggerEvent::Canceled:  return TEXT("Canceled");
		case ETriggerEvent::Completed: return TEXT("Completed");
		default:                       return TEXT("None");
		}
	}

	FString KeyJson(const FKey& K)
	{
		return FString::Printf(TEXT("\"%s\""), *EscapeJson(K.GetFName().ToString()));
	}

	/** The player's flattened, currently-applied mappings.
	 *
	 * UEnhancedPlayerInput::GetEnhancedActionMappings() is PROTECTED, so this reads the UPROPERTY by
	 * reflection — the same trick the rest of this file uses to stay free of the game's headers, and
	 * the only route to the one fact that matters: is this action reachable AT ALL right now. */
	void GatherBoundMappings(int32 PlayerIndex, TMap<FString, TArray<FKey>>& OutByActionPath,
		TArray<FString>& OutContexts)
	{
		UEnhancedInputLocalPlayerSubsystem* const Sub = InputSubsystem(PlayerIndex);
		UEnhancedPlayerInput* const EPI = Sub ? Sub->GetPlayerInput() : nullptr;
		if (!EPI)
		{
			return;
		}

		if (const FArrayProperty* Arr = CastField<FArrayProperty>(
			EPI->GetClass()->FindPropertyByName(TEXT("EnhancedActionMappings"))))
		{
			FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(EPI));
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				const FEnhancedActionKeyMapping* Mapping =
					reinterpret_cast<const FEnhancedActionKeyMapping*>(Helper.GetRawPtr(i));
				if (Mapping && Mapping->Action)
				{
					OutByActionPath.FindOrAdd(Mapping->Action->GetPathName()).AddUnique(Mapping->Key);
				}
			}
		}

		if (const FMapProperty* MapProp = CastField<FMapProperty>(
			EPI->GetClass()->FindPropertyByName(TEXT("AppliedInputContextData"))))
		{
			FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(EPI));
			const FObjectPropertyBase* KeyProp = CastField<FObjectPropertyBase>(MapProp->KeyProp);
			for (int32 i = 0; KeyProp && i < Helper.GetMaxIndex(); ++i)
			{
				if (!Helper.IsValidIndex(i))
				{
					continue;
				}
				if (const UObject* Ctx = KeyProp->GetObjectPropertyValue(Helper.GetKeyPtr(i)))
				{
					OutContexts.Add(Ctx->GetPathName());
				}
			}
		}
	}
}

FString UFablePlay::PressKey(const FString& Key, int32 PlayerIndex)
{
	const FKey K = ResolveKey(Key);
	if (!K.IsValid())
	{
		return Fail(FString::Printf(TEXT("no key '%s' - call ListKeys"), *Key));
	}
	if (!PC(PlayerIndex))
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	/* ☠ REFUSED FOR A TRUE ANALOG AXIS, ON PURPOSE. A stick has no "pressed": deflecting one here
	 * would leave it deflected for the rest of the session, because a gamepad axis carries no
	 * UpdateAxisWithoutSamples flag and so never decays. Route those through GamepadAxis, which is
	 * time-boxed and zeroes itself. (Button-axis keys like MouseScrollUp are NOT analog by this
	 * test and go the normal Pressed/Released way, which is exactly what a wheel notch is.) */
	if (K.IsAnalog())
	{
		return Fail(FString::Printf(
			TEXT("'%s' is an analog axis - use GamepadAxis/MouseAxis, which cannot get stuck"),
			*K.GetFName().ToString()));
	}
	const bool bHandled = SendKey(PlayerIndex, K, IE_Pressed, 1.0f);
	return FString::Printf(TEXT("{\"ok\":true,\"key\":%s,\"event\":\"Pressed\",\"handled\":%s}"),
		*KeyJson(K), bHandled ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::ReleaseKey(const FString& Key, int32 PlayerIndex)
{
	const FKey K = ResolveKey(Key);
	if (!K.IsValid())
	{
		return Fail(FString::Printf(TEXT("no key '%s' - call ListKeys"), *Key));
	}
	if (!PC(PlayerIndex))
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	EndKeyHold(KeyHoldId(PlayerIndex, K), /*bSendRelease=*/false);   // stop any hold, then release once
	const bool bHandled = K.IsAnalog()
		? SendKey(PlayerIndex, K, IE_Axis, 0.0f, 1)
		: SendKey(PlayerIndex, K, IE_Released, 0.0f);
	return FString::Printf(TEXT("{\"ok\":true,\"key\":%s,\"event\":\"Released\",\"handled\":%s}"),
		*KeyJson(K), bHandled ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::TapKey(const FString& Key, int32 PlayerIndex)
{
	const FKey K = ResolveKey(Key);
	if (!K.IsValid())
	{
		return Fail(FString::Printf(TEXT("no key '%s' - call ListKeys"), *Key));
	}
	if (!PC(PlayerIndex))
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	/* Pressed this frame, released on the NEXT tick. A Pressed trigger fires on the TRANSITION, so a
	 * press and release inside one frame can be collapsed by the key-state pass and never seen. */
	StartKeyHold(PlayerIndex, K, 0.0f, 1.0f);
	return FString::Printf(TEXT("{\"ok\":true,\"key\":%s,\"event\":\"Tap\",\"releaseOn\":\"next tick\"}"),
		*KeyJson(K));
}

FString UFablePlay::HoldKey(const FString& Key, float Seconds, float Amount, int32 PlayerIndex)
{
	const FKey K = ResolveKey(Key);
	if (!K.IsValid())
	{
		return Fail(FString::Printf(TEXT("no key '%s' - call ListKeys"), *Key));
	}
	if (!PC(PlayerIndex))
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	StartKeyHold(PlayerIndex, K, Seconds, Amount);
	return FString::Printf(
		TEXT("{\"ok\":true,\"key\":%s,\"seconds\":%.3f,\"amount\":%.3f,\"analog\":%s,\"holds\":%d}"),
		*KeyJson(K), Seconds, Amount, K.IsAnalog() ? TEXT("true") : TEXT("false"), GKeyHolds.Num());
}

FString UFablePlay::ReleaseAllKeys(bool bFlushAll, int32 PlayerIndex)
{
	const int32 N = ReleaseAllHeldKeys();
	bool bFlushed = false;
	if (bFlushAll)
	{
		if (APlayerController* const C = PC(PlayerIndex))
		{
			C->FlushPressedKeys();
			bFlushed = true;
		}
	}
	return FString::Printf(TEXT("{\"ok\":true,\"released\":%d,\"flushed\":%s}"),
		N, bFlushed ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::MouseAxis(float DX, float DY, int32 PlayerIndex)
{
	if (!PC(PlayerIndex))
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	/* MouseX/MouseY carry UpdateAxisWithoutSamples, so a delta with a sample count of at least one is
	 * mandatory (UPlayerInput ensure()s on it) and the value self-clears next frame — which is what a
	 * real mouse does, and why turning the camera means calling this repeatedly. The engine mirrors
	 * both into the paired Mouse2D axis for us. */
	bool bAny = false;
	if (!FMath::IsNearlyZero(DX))
	{
		bAny |= SendKey(PlayerIndex, EKeys::MouseX, IE_Axis, DX, 1);
	}
	if (!FMath::IsNearlyZero(DY))
	{
		bAny |= SendKey(PlayerIndex, EKeys::MouseY, IE_Axis, DY, 1);
	}
	return FString::Printf(TEXT("{\"ok\":true,\"dx\":%.3f,\"dy\":%.3f,\"handled\":%s}"),
		DX, DY, bAny ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::GamepadAxis(const FString& Key, float Amount, float Seconds, int32 PlayerIndex)
{
	const FKey K = ResolveKey(Key);
	if (!K.IsValid())
	{
		return Fail(FString::Printf(TEXT("no key '%s' - call ListKeys Gamepad"), *Key));
	}
	if (!PC(PlayerIndex))
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	if (!K.IsAnalog())
	{
		return Fail(FString::Printf(TEXT("'%s' is a digital key - use TapKey/HoldKey"),
			*K.GetFName().ToString()));
	}
	/* ☠ ALWAYS TIME-BOXED. A gamepad axis has no UpdateAxisWithoutSamples flag, so a value written
	 * once and never cleared stays deflected for the rest of the session — a stick stuck hard over,
	 * which reads downstream as a movement or AI bug rather than as harness residue. Seconds <= 0
	 * still goes through the hold path so that the zeroing happens on the next tick. */
	StartKeyHold(PlayerIndex, K, Seconds, Amount);
	return FString::Printf(
		TEXT("{\"ok\":true,\"key\":%s,\"amount\":%.3f,\"seconds\":%.3f,\"zeroedAfter\":true}"),
		*KeyJson(K), Amount, Seconds);
}

FString UFablePlay::ListKeys(const FString& Pattern)
{
	TArray<FKey> All;
	EKeys::GetAllKeys(All);

	FString Rows;
	int32 Shown = 0;
	for (const FKey& K : All)
	{
		const FString Name = K.GetFName().ToString();
		const FString Display = K.GetDisplayName().ToString();
		if (!Pattern.IsEmpty()
			&& !Name.Contains(Pattern, ESearchCase::IgnoreCase)
			&& !Display.Contains(Pattern, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Shown >= 200)
		{
			break;
		}
		Rows += FString::Printf(
			TEXT("%s{\"key\":\"%s\",\"display\":\"%s\",\"analog\":%s,\"gamepad\":%s,\"mouse\":%s}"),
			Rows.IsEmpty() ? TEXT("") : TEXT(","),
			*EscapeJson(Name), *EscapeJson(Display),
			K.IsAnalog() ? TEXT("true") : TEXT("false"),
			K.IsGamepadKey() ? TEXT("true") : TEXT("false"),
			K.IsMouseButton() ? TEXT("true") : TEXT("false"));
		++Shown;
	}
	return FString::Printf(TEXT("{\"ok\":true,\"count\":%d,\"keys\":[%s]}"), Shown, *Rows);
}

FString UFablePlay::KeyState(const FString& Key, int32 PlayerIndex)
{
	const FKey K = ResolveKey(Key);
	if (!K.IsValid())
	{
		return Fail(FString::Printf(TEXT("no key '%s' - call ListKeys"), *Key));
	}
	APlayerController* const C = PC(PlayerIndex);
	if (!C)
	{
		return Fail(TEXT("no PIE player controller - start PIE first"));
	}
	/* THIS IS THE MUTATION TEST for every call above. Press, read down:true, release, read down:false.
	 * An input route that silently stops working — which is exactly what InjectAction did for four
	 * days — cannot hide from a reading that comes back out of the engine's own key-state table. */
	return FString::Printf(
		TEXT("{\"ok\":true,\"key\":%s,\"down\":%s,\"timeDown\":%.3f,\"justPressed\":%s,")
		TEXT("\"justReleased\":%s,\"analogValue\":%.4f,\"vector\":%s,\"held\":%s}"),
		*KeyJson(K),
		C->IsInputKeyDown(K) ? TEXT("true") : TEXT("false"),
		C->GetInputKeyTimeDown(K),
		C->WasInputKeyJustPressed(K) ? TEXT("true") : TEXT("false"),
		C->WasInputKeyJustReleased(K) ? TEXT("true") : TEXT("false"),
		C->GetInputAnalogKeyState(K),
		*VecJson(C->GetInputVectorKeyState(K)),
		GKeyHolds.Contains(KeyHoldId(PlayerIndex, K)) ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::ActionState(const FString& ActionPath, int32 PlayerIndex)
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

	FString Keys;
	for (const FKey& K : Sub->QueryKeysMappedToAction(Action))
	{
		Keys += FString::Printf(TEXT("%s%s"), Keys.IsEmpty() ? TEXT("") : TEXT(","), *KeyJson(K));
	}

	UEnhancedPlayerInput* const EPI = Sub->GetPlayerInput();
	const FInputActionInstance* const Inst = EPI ? EPI->FindActionInstanceData(Action) : nullptr;
	const FString Value = EPI ? EPI->GetActionValue(Action).ToString() : FString(TEXT("?"));

	return FString::Printf(
		TEXT("{\"ok\":true,\"action\":\"%s\",\"valueType\":\"%s\",\"bound\":%s,\"keys\":[%s],")
		TEXT("\"value\":\"%s\",\"trigger\":\"%s\",\"elapsed\":%.3f,\"triggeredFor\":%.3f}"),
		*EscapeJson(Action->GetName()), ValueTypeName(Action),
		Keys.IsEmpty() ? TEXT("false") : TEXT("true"), *Keys, *EscapeJson(Value),
		Inst ? TriggerEventName(Inst->GetTriggerEvent()) : TEXT("None"),
		Inst ? Inst->GetElapsedTime() : 0.0f,
		Inst ? Inst->GetTriggeredTime() : 0.0f);
}

FString UFablePlay::ListInputActions(int32 PlayerIndex)
{
	const FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, true);

	/* THE GAP THIS CLOSES. An action that no currently applied mapping context maps is bound to
	 * NOTHING: injecting it does nothing, reports nothing and errors nowhere — the exact silence that
	 * went undiagnosed from 08-09. The answer lives in the player's flattened EnhancedActionMappings,
	 * so read it and say so, rather than leaving the caller to infer it from a montage that did not
	 * play. Note this loads no assets: an action that is bound is by definition already in memory. */
	TMap<FString, TArray<FKey>> BoundByPath;
	TArray<FString> Contexts;
	GatherBoundMappings(PlayerIndex, BoundByPath, Contexts);

	FString Rows;
	int32 BoundCount = 0;
	for (const FAssetData& A : Assets)
	{
		const FString Path = A.GetSoftObjectPath().ToString();
		const TArray<FKey>* Keys = BoundByPath.Find(Path);
		FString KeyList;
		if (Keys)
		{
			++BoundCount;
			for (const FKey& K : *Keys)
			{
				KeyList += FString::Printf(TEXT("%s%s"), KeyList.IsEmpty() ? TEXT("") : TEXT(","), *KeyJson(K));
			}
		}
		// FastGetAsset(false) reports the value type only when the asset is ALREADY loaded — asking
		// for it must never be the reason 60 input assets get pulled into memory.
		const UInputAction* const Loaded = Cast<UInputAction>(A.FastGetAsset(false));
		Rows += FString::Printf(
			TEXT("%s{\"path\":\"%s\",\"name\":\"%s\",\"bound\":%s,\"keys\":[%s],\"valueType\":%s}"),
			Rows.IsEmpty() ? TEXT("") : TEXT(","),
			*EscapeJson(Path), *EscapeJson(A.AssetName.ToString()),
			Keys ? TEXT("true") : TEXT("false"), *KeyList,
			Loaded ? *FString::Printf(TEXT("\"%s\""), ValueTypeName(Loaded)) : TEXT("null"));
	}

	FString ContextList;
	for (const FString& Ctx : Contexts)
	{
		ContextList += FString::Printf(TEXT("%s\"%s\""),
			ContextList.IsEmpty() ? TEXT("") : TEXT(","), *EscapeJson(Ctx));
	}

	return FString::Printf(
		TEXT("{\"ok\":true,\"count\":%d,\"bound\":%d,\"pie\":%s,\"appliedContexts\":[%s],\"actions\":[%s]}"),
		Assets.Num(), BoundCount, PIEWorld() ? TEXT("true") : TEXT("false"), *ContextList, *Rows);
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
	/** ☠ FPointerEvent STORES A POINTER TO THE PRESSED-BUTTON SET, NOT A COPY
	 *  (`PressedButtons(&InPressedButtons)`). A local TSet built in a factory and returned by value
	 *  therefore leaves every event holding a dangling pointer into a dead stack frame — it happens
	 *  to survive today only because nothing has re-used that memory before Slate reads it. These
	 *  sets have static storage duration, so the reference is always live. Game thread only. */
	const TSet<FKey>& PressedSetFor(const FKey& Button, bool bButtonDown)
	{
		static const TSet<FKey> Empty;
		// Indirect on purpose: a TMap rehash MOVES its values, which would re-introduce the very
		// dangling reference this exists to remove. The sets themselves never move.
		static TMap<FName, TSharedRef<TSet<FKey>>> Cache;
		if (!bButtonDown || !Button.IsValid())
		{
			return Empty;
		}
		if (TSharedRef<TSet<FKey>>* Found = Cache.Find(Button.GetFName()))
		{
			return **Found;
		}
		TSharedRef<TSet<FKey>> Set = MakeShared<TSet<FKey>>();
		Set->Add(Button);
		Cache.Add(Button.GetFName(), Set);
		return *Set;
	}

	/** Where THIS HARNESS last put the pointer.
	 *  ☠ Deliberately not FSlateApplication::GetCursorPos(): that is the REAL mouse, sitting wherever
	 *  the human left it, and a "scroll where the pointer is" that read it would scroll over whatever
	 *  Peter's cursor happens to be on rather than over the widget the test just moved to. */
	static FVector2D GLastInjectedPointer = FVector2D::ZeroVector;

	/** A pointer event with a real CURSOR DELTA (Pos - LastPos). The delta is not decoration: Slate's
	 *  drag detector only raises OnDragDetected once the pointer has travelled the trigger distance
	 *  while a button is down, so a move whose LastPos equals its Pos can never start a drag. */
	FPointerEvent MakePointerFrom(const FVector2D& LastPos, const FVector2D& Pos, const FKey& Button,
		bool bButtonDown)
	{
		GLastInjectedPointer = Pos;
		return FPointerEvent(/*PointerIndex=*/0, Pos, LastPos, PressedSetFor(Button, bButtonDown),
			Button, /*WheelDelta=*/0.0f, FModifierKeysState());
	}

	FPointerEvent MakePointer(const FVector2D& Pos, const FKey& Button, bool bButtonDown)
	{
		return MakePointerFrom(Pos, Pos, Button, bButtonDown);
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

/* ==================================================================================================
 * DRAG, WHEEL, TEXT — the pointer verbs the click-only API could not express.
 * ================================================================================================== */

namespace FablePlayInternal
{
	/** A drag in flight. Multi-frame BY NECESSITY: Slate arms its drag detector on the press and
	 *  only fires OnDragDetected on a LATER move, so a drag cannot happen inside one bridge call. */
	struct FDragOp
	{
		FVector2D From = FVector2D::ZeroVector;
		FVector2D To = FVector2D::ZeroVector;
		FVector2D Last = FVector2D::ZeroVector;
		int32 Steps = 12;
		int32 Done = 0;
		int32 Phase = 0;              // 0 approach, 1 press, 2 move, 3 release
		double Interval = 0.03;
		double NextAt = 0.0;
		FKey Button = EKeys::LeftMouseButton;
		FTSTicker::FDelegateHandle Ticker;
	};
	static TSharedPtr<FDragOp> GDrag;

	/** A typing run in flight, when the caller asked for a human cadence. */
	struct FTypeOp
	{
		FString Text;
		int32 Index = 0;
		double Interval = 0.0;
		double NextAt = 0.0;
		FTSTicker::FDelegateHandle Ticker;
	};
	static TSharedPtr<FTypeOp> GType;

	/** One character, the way a keyboard delivers it: key down, character, key up.
	 *  The CHARACTER event is what a text box consumes; the key events are what widgets that gate on
	 *  OnKeyDown (commit on Enter, close on Escape) consume. Send both and neither kind is missed. */
	void SendChar(TCHAR Ch)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Mods;

		if (Ch == TEXT('\n') || Ch == TEXT('\r'))
		{
			const FKeyEvent Ev(EKeys::Enter, Mods, /*UserIndex=*/0u, /*bIsRepeat=*/false,
				/*CharCode=*/0u, /*KeyCode=*/0u);
			App.ProcessKeyDownEvent(Ev);
			App.ProcessKeyUpEvent(Ev);
			return;
		}

		// Letters and digits have an FKey; punctuation mostly does not, and does not need one.
		FKey AsKey = EKeys::Invalid;
		if (Ch == TEXT(' '))
		{
			AsKey = EKeys::SpaceBar;
		}
		else if (FChar::IsAlpha(Ch))
		{
			AsKey = FKey(FName(*FString::Chr(FChar::ToUpper(Ch))));
			if (!AsKey.IsValid())
			{
				AsKey = EKeys::Invalid;
			}
		}

		const uint32 CharCode = static_cast<uint32>(Ch);
		if (AsKey.IsValid())
		{
			App.ProcessKeyDownEvent(FKeyEvent(AsKey, Mods, 0u, false, CharCode, 0u));
		}
		App.ProcessKeyCharEvent(FCharacterEvent(Ch, Mods, /*UserIndex=*/0u, /*bIsRepeat=*/false));
		if (AsKey.IsValid())
		{
			App.ProcessKeyUpEvent(FKeyEvent(AsKey, Mods, 0u, false, CharCode, 0u));
		}
	}
}

FString UFablePlay::Drag(float X1, float Y1, float X2, float Y2, float Seconds, int32 Steps, bool bRight)
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
	if (Steps < 2)
	{
		Steps = 2;   // one intermediate move can sit inside the drag trigger distance and detect nothing
	}
	if (GDrag.IsValid() && GDrag->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GDrag->Ticker);   // one drag at a time
	}

	TSharedPtr<FDragOp> Op = MakeShared<FDragOp>();
	Op->From = FVector2D(X1, Y1);
	Op->To = FVector2D(X2, Y2);
	Op->Last = Op->From;
	Op->Steps = Steps;
	Op->Interval = FMath::Max(static_cast<double>(Seconds), 0.0) / static_cast<double>(Steps + 2);
	Op->NextAt = FPlatformTime::Seconds();
	Op->Button = bRight ? EKeys::RightMouseButton : EKeys::LeftMouseButton;

	Op->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			if (!GDrag.IsValid() || !FSlateApplication::IsInitialized())
			{
				return false;
			}
			FDragOp& D = *GDrag;
			const double Now = FPlatformTime::Seconds();
			if (Now < D.NextAt)
			{
				return true;
			}
			D.NextAt = Now + D.Interval;
			FSlateApplication& App = FSlateApplication::Get();

			switch (D.Phase)
			{
			case 0:   // approach: hover the source so the press lands on a hovered widget
				App.ProcessMouseMoveEvent(MakePointer(D.From, EKeys::Invalid, false));
				D.Phase = 1;
				return true;

			case 1:   // press: this is where a drag-capable widget returns FReply::DetectDrag
				App.ProcessMouseButtonDownEvent(nullptr, MakePointer(D.From, D.Button, true));
				D.Phase = 2;
				return true;

			case 2:   // travel, button HELD — each move carries a real cursor delta
			{
				++D.Done;
				const float Alpha = static_cast<float>(D.Done) / static_cast<float>(D.Steps);
				const FVector2D Next = FMath::Lerp(D.From, D.To, FMath::Min(Alpha, 1.0f));
				App.ProcessMouseMoveEvent(MakePointerFrom(D.Last, Next, D.Button, true));
				D.Last = Next;
				if (D.Done >= D.Steps)
				{
					D.Phase = 3;
				}
				return true;
			}

			default:  // release: the drop
				App.ProcessMouseButtonUpEvent(MakePointerFrom(D.Last, D.To, D.Button, false));
				return false;
			}
		}), 0.0f);

	GDrag = Op;
	return FString::Printf(
		TEXT("{\"ok\":true,\"from\":{\"x\":%.0f,\"y\":%.0f},\"to\":{\"x\":%.0f,\"y\":%.0f},")
		TEXT("\"steps\":%d,\"seconds\":%.2f,\"right\":%s}"),
		X1, Y1, X2, Y2, Steps, Seconds, bRight ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::ScrollWheel(float Delta, float X, float Y)
{
	using namespace FablePlayInternal;
	if (!FSlateApplication::IsInitialized())
	{
		return Fail(TEXT("Slate not initialized"));
	}
	FSlateApplication& App = FSlateApplication::Get();
	const FVector2D Pos = (X < 0.0f || Y < 0.0f) ? GLastInjectedPointer : FVector2D(X, Y);

	/* Slate's own wheel path — scroll boxes, list views, the map pad's zoom. It does NOT force the
	 * gameplay route: for a hotbar or a weapon wheel bound to the mouse wheel as an INPUT ACTION,
	 * use TapKey("MouseScrollUp") / TapKey("MouseScrollDown") or MouseWheelAxis, which enter at the
	 * player controller. Doing both here would double-count every notch. */
	const FPointerEvent Wheel(/*PointerIndex=*/0, Pos, Pos,
		PressedSetFor(EKeys::Invalid, false), EKeys::MouseWheelAxis, Delta, FModifierKeysState());
	const bool bHandled = App.ProcessMouseWheelOrGestureEvent(Wheel, nullptr);

	return FString::Printf(TEXT("{\"ok\":true,\"delta\":%.2f,\"x\":%.0f,\"y\":%.0f,\"handled\":%s}"),
		Delta, Pos.X, Pos.Y, bHandled ? TEXT("true") : TEXT("false"));
}

FString UFablePlay::TypeText(const FString& Text, float CharsPerSecond)
{
	using namespace FablePlayInternal;
	if (!FSlateApplication::IsInitialized())
	{
		return Fail(TEXT("Slate not initialized"));
	}
	if (Text.IsEmpty())
	{
		return Fail(TEXT("nothing to type"));
	}

	// Where the characters will land. Empty focus is the usual reason typing "works" and shows nothing.
	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
	const FString FocusName = Focused.IsValid() ? Focused->GetTypeAsString() : FString(TEXT(""));

	if (CharsPerSecond <= 0.0f)
	{
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			SendChar(Text[i]);
		}
		return FString::Printf(TEXT("{\"ok\":true,\"chars\":%d,\"instant\":true,\"focus\":\"%s\"}"),
			Text.Len(), *EscapeJson(FocusName));
	}

	if (GType.IsValid() && GType->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GType->Ticker);
	}
	TSharedPtr<FTypeOp> Op = MakeShared<FTypeOp>();
	Op->Text = Text;
	Op->Interval = 1.0 / static_cast<double>(CharsPerSecond);
	Op->NextAt = FPlatformTime::Seconds();

	Op->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			if (!GType.IsValid() || !FSlateApplication::IsInitialized())
			{
				return false;
			}
			FTypeOp& T = *GType;
			const double Now = FPlatformTime::Seconds();
			if (Now < T.NextAt)
			{
				return true;
			}
			T.NextAt = Now + T.Interval;
			SendChar(T.Text[T.Index]);
			return ++T.Index < T.Text.Len();
		}), 0.0f);

	GType = Op;
	return FString::Printf(TEXT("{\"ok\":true,\"chars\":%d,\"cps\":%.1f,\"focus\":\"%s\"}"),
		Text.Len(), CharsPerSecond, *EscapeJson(FocusName));
}

/* ==================================================================================================
 * SEQUENCES — one bridge call, one whole playthrough beat.
 *
 * ☠☠ THE CONSTRAINT THAT SHAPES ALL OF THIS: a bridge call OWNS THE GAME THREAD for its duration.
 * A script with sleeps in it therefore cannot work — nothing ticks while you wait, so "hold W for
 * two seconds, then swing" degenerates into two things happening on the same starved frame. The
 * whole script runs on the game-thread ticker instead, one step per tick minimum, and the caller
 * returns immediately and reads the transcript later. PointerRest solved this shape first.
 * ================================================================================================== */

namespace FablePlayInternal
{
	enum class ESeqKind : uint8
	{
		Wait, KeyTap, KeyHold, KeyDown, KeyUp, Look, Axis, Click, Move, Scroll, Type, Call, Say, Bad
	};

	struct FSeqStep
	{
		FString Raw;
		ESeqKind Kind = ESeqKind::Bad;
		FKey Key;
		float A = 0.0f;
		float B = 0.0f;
		double Duration = 0.0;
		FString Text;
		FString Text2;
		bool bRight = false;
		bool bDouble = false;
		FString Error;
	};

	struct FSequence
	{
		TArray<FSeqStep> Steps;
		int32 Index = 0;
		bool bStepStarted = false;
		double StepEnd = 0.0;
		double StartTime = 0.0;
		int32 PlayerIndex = 0;
		TArray<TPair<double, FString>> Transcript;
		TArray<FKey> PressedByScript;    // so an abort can put every one of them back up
		FTSTicker::FDelegateHandle Ticker;
		bool bRunning = false;
	};
	static TSharedPtr<FSequence> GSeq;

	bool ParseFloatToken(const FString& Tok, float& Out)
	{
		if (Tok.IsEmpty())
		{
			return false;
		}
		if (!Tok.IsNumeric() && !(Tok.StartsWith(TEXT("-")) && Tok.Mid(1).IsNumeric()))
		{
			return false;
		}
		Out = FCString::Atof(*Tok);
		return true;
	}

	FSeqStep ParseSeqStep(const FString& RawIn)
	{
		FSeqStep S;
		S.Raw = RawIn;
		S.Raw.TrimStartAndEndInline();

		TArray<FString> Tok;
		S.Raw.ParseIntoArrayWS(Tok);
		if (Tok.Num() == 0)
		{
			S.Error = TEXT("empty step");
			return S;
		}
		const FString Head = Tok[0].ToUpper();
		const auto Rest = [&Tok](int32 From) -> FString
		{
			FString Out;
			for (int32 i = From; i < Tok.Num(); ++i)
			{
				Out += (i > From ? TEXT(" ") : TEXT("")) + Tok[i];
			}
			return Out;
		};

		if (Head == TEXT("WAIT"))
		{
			S.Kind = ESeqKind::Wait;
			float Secs = 0.5f;
			if (Tok.Num() > 1)
			{
				ParseFloatToken(Tok[1], Secs);
			}
			S.Duration = FMath::Max(Secs, 0.0f);
			return S;
		}
		if (Head == TEXT("LOOK") || Head == TEXT("MOUSE"))
		{
			S.Kind = ESeqKind::Look;
			if (Tok.Num() > 1) { ParseFloatToken(Tok[1], S.A); }
			if (Tok.Num() > 2) { ParseFloatToken(Tok[2], S.B); }
			return S;
		}
		if (Head == TEXT("AXIS"))
		{
			if (Tok.Num() < 2)
			{
				S.Error = TEXT("AXIS needs a key");
				return S;
			}
			S.Kind = ESeqKind::Axis;
			S.Key = ResolveKey(Tok[1]);
			S.A = 1.0f;
			if (Tok.Num() > 2) { ParseFloatToken(Tok[2], S.A); }
			float Secs = 0.25f;
			if (Tok.Num() > 3) { ParseFloatToken(Tok[3], Secs); }
			S.Duration = FMath::Max(Secs, 0.0f);
			if (!S.Key.IsValid())
			{
				S.Error = FString::Printf(TEXT("unknown key '%s'"), *Tok[1]);
			}
			return S;
		}
		if (Head == TEXT("CLICK") || Head == TEXT("DCLICK"))
		{
			S.Kind = ESeqKind::Click;
			S.bDouble = (Head == TEXT("DCLICK"));
			int32 Base = 1;
			if (Tok.Num() > 1 && (Tok[1].ToUpper() == TEXT("R") || Tok[1].ToUpper() == TEXT("RIGHT")))
			{
				S.bRight = true;
				Base = 2;
			}
			if (Tok.Num() > Base)     { ParseFloatToken(Tok[Base], S.A); }
			if (Tok.Num() > Base + 1) { ParseFloatToken(Tok[Base + 1], S.B); }
			return S;
		}
		if (Head == TEXT("MOVE"))
		{
			S.Kind = ESeqKind::Move;
			if (Tok.Num() > 1) { ParseFloatToken(Tok[1], S.A); }
			if (Tok.Num() > 2) { ParseFloatToken(Tok[2], S.B); }
			return S;
		}
		if (Head == TEXT("SCROLL"))
		{
			S.Kind = ESeqKind::Scroll;
			S.A = 1.0f;
			if (Tok.Num() > 1) { ParseFloatToken(Tok[1], S.A); }
			return S;
		}
		if (Head == TEXT("TYPE"))
		{
			S.Kind = ESeqKind::Type;
			S.Text = Rest(1);
			return S;
		}
		if (Head == TEXT("SAY"))
		{
			S.Kind = ESeqKind::Say;
			S.Text = Rest(1);
			return S;
		}
		if (Head == TEXT("CALL"))
		{
			if (Tok.Num() < 3)
			{
				S.Error = TEXT("CALL needs a target and a function");
				return S;
			}
			S.Kind = ESeqKind::Call;
			S.Text = Tok[1];        // target
			S.Text2 = Rest(2);      // "Func Arg Arg"
			return S;
		}

		// Anything else is a KEY, and the modifier tells us what to do with it.
		S.Key = ResolveKey(Tok[0]);
		if (!S.Key.IsValid())
		{
			S.Error = FString::Printf(TEXT("unknown step or key '%s'"), *Tok[0]);
			return S;
		}
		if (Tok.Num() == 1)
		{
			S.Kind = ESeqKind::KeyTap;
			return S;
		}
		const FString Mod = Tok[1].ToUpper();
		if (Mod == TEXT("TAP"))
		{
			S.Kind = ESeqKind::KeyTap;
			return S;
		}
		if (Mod == TEXT("DOWN") || Mod == TEXT("PRESS"))
		{
			S.Kind = ESeqKind::KeyDown;
			return S;
		}
		if (Mod == TEXT("UP") || Mod == TEXT("RELEASE"))
		{
			S.Kind = ESeqKind::KeyUp;
			return S;
		}
		float Secs = 0.0f;
		const FString Num = (Mod == TEXT("HOLD") && Tok.Num() > 2) ? Tok[2] : Tok[1];
		if (ParseFloatToken(Num, Secs))
		{
			S.Kind = ESeqKind::KeyHold;
			S.Duration = FMath::Max(Secs, 0.0f);
			return S;
		}
		S.Error = FString::Printf(TEXT("'%s' is not TAP/DOWN/UP or a duration"), *Tok[1]);
		return S;
	}

	/** Run a step's BEGIN. Returns a one-line result for the transcript. */
	FString BeginSeqStep(FSequence& S, const FSeqStep& Step)
	{
		switch (Step.Kind)
		{
		case ESeqKind::Wait:
			return FString::Printf(TEXT("wait %.2fs"), Step.Duration);

		case ESeqKind::KeyTap:
			StartKeyHold(S.PlayerIndex, Step.Key, 0.0f, 1.0f);
			return FString::Printf(TEXT("tap %s"), *Step.Key.GetFName().ToString());

		case ESeqKind::KeyHold:
			StartKeyHold(S.PlayerIndex, Step.Key, static_cast<float>(Step.Duration), 1.0f);
			return FString::Printf(TEXT("hold %s %.2fs"), *Step.Key.GetFName().ToString(), Step.Duration);

		case ESeqKind::KeyDown:
			SendKey(S.PlayerIndex, Step.Key, Step.Key.IsAnalog() ? IE_Axis : IE_Pressed, 1.0f,
				Step.Key.IsAnalog() ? 1 : -1);
			S.PressedByScript.AddUnique(Step.Key);
			return FString::Printf(TEXT("down %s"), *Step.Key.GetFName().ToString());

		case ESeqKind::KeyUp:
			SendKey(S.PlayerIndex, Step.Key, Step.Key.IsAnalog() ? IE_Axis : IE_Released, 0.0f,
				Step.Key.IsAnalog() ? 1 : -1);
			S.PressedByScript.Remove(Step.Key);
			return FString::Printf(TEXT("up %s"), *Step.Key.GetFName().ToString());

		case ESeqKind::Look:
			UFablePlay::MouseAxis(Step.A, Step.B, S.PlayerIndex);
			return FString::Printf(TEXT("look %.1f %.1f"), Step.A, Step.B);

		case ESeqKind::Axis:
			StartKeyHold(S.PlayerIndex, Step.Key, static_cast<float>(Step.Duration), Step.A);
			return FString::Printf(TEXT("axis %s %.2f for %.2fs"),
				*Step.Key.GetFName().ToString(), Step.A, Step.Duration);

		case ESeqKind::Click:
			InjectClick(FVector2D(Step.A, Step.B), Step.bRight, Step.bDouble);
			return FString::Printf(TEXT("click %.0f %.0f%s"), Step.A, Step.B,
				Step.bDouble ? TEXT(" double") : TEXT(""));

		case ESeqKind::Move:
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().ProcessMouseMoveEvent(
					MakePointer(FVector2D(Step.A, Step.B), EKeys::Invalid, false));
			}
			return FString::Printf(TEXT("move %.0f %.0f"), Step.A, Step.B);

		case ESeqKind::Scroll:
			UFablePlay::ScrollWheel(Step.A, -1.0f, -1.0f);
			return FString::Printf(TEXT("scroll %.1f"), Step.A);

		case ESeqKind::Type:
			UFablePlay::TypeText(Step.Text, 0.0f);
			return FString::Printf(TEXT("type '%s'"), *Step.Text);

		case ESeqKind::Call:
			return FString::Printf(TEXT("call %s -> %s"), *Step.Text,
				*UFablePlay::CallFunction(Step.Text, Step.Text2));

		case ESeqKind::Say:
			return Step.Text;

		default:
			return FString::Printf(TEXT("SKIPPED: %s"), *Step.Error);
		}
	}

	void FinishSequence(const TCHAR* Why)
	{
		if (!GSeq.IsValid())
		{
			return;
		}
		FSequence& S = *GSeq;
		// ☠ Never leave a key down. A stuck W walks the pawn off the map for the rest of the session.
		for (const FKey& K : S.PressedByScript)
		{
			SendKey(S.PlayerIndex, K, K.IsAnalog() ? IE_Axis : IE_Released, 0.0f, K.IsAnalog() ? 1 : -1);
		}
		S.PressedByScript.Reset();
		ReleaseAllHeldKeys();
		S.bRunning = false;
		S.Transcript.Add({ FPlatformTime::Seconds() - S.StartTime, FString(Why) });
	}
}

FString UFablePlay::RunSequence(const FString& Script, int32 PlayerIndex)
{
	using namespace FablePlayInternal;
	if (!PIEWorld())
	{
		return Fail(TEXT("no PIE session"));
	}
	if (Script.IsEmpty())
	{
		return Fail(TEXT("empty script"));
	}
	if (GSeq.IsValid())
	{
		if (GSeq->Ticker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GSeq->Ticker);
		}
		FinishSequence(TEXT("replaced by a new sequence"));
	}

	TArray<FString> Raw;
	Script.ParseIntoArray(Raw, TEXT("|"), true);

	TSharedPtr<FSequence> Seq = MakeShared<FSequence>();
	Seq->PlayerIndex = PlayerIndex;
	Seq->StartTime = FPlatformTime::Seconds();
	Seq->bRunning = true;

	FString Errors;
	for (const FString& R : Raw)
	{
		FSeqStep Step = ParseSeqStep(R);
		if (!Step.Error.IsEmpty())
		{
			Errors += FString::Printf(TEXT("%s%s: %s"), Errors.IsEmpty() ? TEXT("") : TEXT("; "),
				*Step.Raw, *Step.Error);
		}
		Seq->Steps.Add(MoveTemp(Step));
	}
	if (Seq->Steps.Num() == 0)
	{
		return Fail(TEXT("no steps parsed"));
	}
	/* PARSE THE WHOLE SCRIPT BEFORE RUNNING ANY OF IT. Half-executing a script and then failing on
	 * step four leaves the game in a state nobody wrote down — worse than not running at all. */
	if (!Errors.IsEmpty())
	{
		return Fail(FString::Printf(TEXT("script not run - %s"), *Errors));
	}

	GSeq = Seq;
	GSeq->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[](float) -> bool
		{
			if (!GSeq.IsValid() || !GSeq->bRunning)
			{
				return false;
			}
			FSequence& S = *GSeq;
			if (!PIEWorld())
			{
				FinishSequence(TEXT("aborted: PIE ended"));
				return false;
			}
			if (S.Index >= S.Steps.Num())
			{
				FinishSequence(TEXT("done"));
				return false;
			}

			const double Now = FPlatformTime::Seconds();
			if (!S.bStepStarted)
			{
				const FSeqStep& Step = S.Steps[S.Index];
				const FString Result = BeginSeqStep(S, Step);
				S.Transcript.Add({ Now - S.StartTime, Result });
				S.bStepStarted = true;
				S.StepEnd = Now + Step.Duration;
				/* ONE STEP PER TICK MINIMUM, even for a zero-duration step. Two verbs on one frame is
				 * not a playthrough — a click and the keypress that answers it must be separated by
				 * at least one game tick or the second lands before the first has been processed. */
				return true;
			}
			if (Now >= S.StepEnd)
			{
				++S.Index;
				S.bStepStarted = false;
			}
			return true;
		}), 0.0f);

	return FString::Printf(TEXT("{\"ok\":true,\"steps\":%d,\"script\":\"%s\"}"),
		Seq->Steps.Num(), *EscapeJson(Script));
}

FString UFablePlay::PollSequence()
{
	using namespace FablePlayInternal;
	if (!GSeq.IsValid())
	{
		return TEXT("{\"ok\":true,\"running\":false,\"step\":0,\"total\":0,\"transcript\":[]}");
	}
	FSequence& S = *GSeq;

	FString Rows;
	for (const TPair<double, FString>& Line : S.Transcript)
	{
		Rows += FString::Printf(TEXT("%s{\"t\":%.3f,\"step\":\"%s\"}"),
			Rows.IsEmpty() ? TEXT("") : TEXT(","), Line.Key, *EscapeJson(Line.Value));
	}
	S.Transcript.Reset();   // drain, like PollWatch — repeated polls stream rather than repeat

	return FString::Printf(
		TEXT("{\"ok\":true,\"running\":%s,\"step\":%d,\"total\":%d,\"elapsed\":%.3f,\"transcript\":[%s]}"),
		S.bRunning ? TEXT("true") : TEXT("false"), S.Index, S.Steps.Num(),
		FPlatformTime::Seconds() - S.StartTime, *Rows);
}

FString UFablePlay::StopSequence()
{
	using namespace FablePlayInternal;
	if (!GSeq.IsValid() || !GSeq->bRunning)
	{
		return TEXT("{\"ok\":true,\"wasRunning\":false}");
	}
	if (GSeq->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GSeq->Ticker);
	}
	const int32 At = GSeq->Index;
	FinishSequence(TEXT("stopped by caller"));
	return FString::Printf(TEXT("{\"ok\":true,\"wasRunning\":true,\"stoppedAt\":%d}"), At);
}

/* ==================================================================================================
 * CAPTURE — see it without a human watching, and without the window.
 *
 * ☠ THE TWO REASONS take_high_res_screenshot COULD NEVER SERVE THIS LANE:
 *   1. it photographs the WINDOW, so it needs the editor foregrounded — the exact act the input ban
 *      exists to prevent, with Peter sitting at the machine;
 *   2. it does not composite UMG, so a widget cannot be judged from one at all.
 * Rendering is not photographing. A scene capture draws through the renderer into a texture with no
 * window, no focus and no swap chain in the path, and Slate will draw the live HUD straight onto the
 * same texture afterwards. Both work with the editor minimised.
 * ================================================================================================== */

namespace FablePlayInternal
{
	/** ExportRenderTarget wants a directory and a file name, and returns void — so the ONLY way to
	 *  know it worked is to stat the file afterwards. A capture that reports ok with zero bytes is
	 *  the failure mode this exists to make impossible. */
	FString ExportRTToPng(UWorld* World, UTextureRenderTarget2D* RT, const FString& OutPngPath,
		int64& OutBytes)
	{
		OutBytes = 0;
		FString Full = OutPngPath;
		if (FPaths::IsRelative(Full))
		{
			Full = FPaths::Combine(FPaths::ProjectDir(), Full);
		}
		Full = FPaths::ConvertRelativePathToFull(Full);
		if (!Full.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
		{
			Full += TEXT(".png");
		}
		const FString Dir = FPaths::GetPath(Full);
		const FString File = FPaths::GetCleanFilename(Full);

		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*Dir, /*Tree=*/true);
		FM.Delete(*Full, /*RequireExists=*/false, /*EvenReadOnly=*/true);   // a stale file reads as success

		UKismetRenderingLibrary::ExportRenderTarget(World, RT, Dir, File);
		OutBytes = FM.FileSize(*Full);
		return Full;
	}
}

FString UFablePlay::CaptureScreen(const FString& OutPngPath, int32 Width, int32 Height,
	bool bIncludeUI, float FOV, int32 PlayerIndex)
{
	using namespace FablePlayInternal;
	UWorld* const World = PIEWorld();
	if (!World)
	{
		return Fail(TEXT("no PIE session - nothing to capture"));
	}
	APlayerController* const C = PC(PlayerIndex);
	if (!C)
	{
		return Fail(TEXT("no PIE player controller"));
	}
	if (OutPngPath.IsEmpty())
	{
		return Fail(TEXT("OutPngPath is required"));
	}
	Width = FMath::Clamp(Width, 64, 4096);
	Height = FMath::Clamp(Height, 64, 4096);

	// The player's actual eye, so the capture is what the player SEES rather than a guessed camera.
	FVector CamLoc = FVector::ZeroVector;
	FRotator CamRot = FRotator::ZeroRotator;
	C->GetPlayerViewPoint(CamLoc, CamRot);
	if (FOV <= 0.0f)
	{
		FOV = C->PlayerCameraManager ? C->PlayerCameraManager->GetFOVAngle() : 90.0f;
	}

	UTextureRenderTarget2D* const RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	TStrongObjectPtr<UTextureRenderTarget2D> RTGuard(RT);
	RT->ClearColor = FLinearColor::Black;
	RT->SRGB = true;
	// ☠ NOT RTF_RGBA16f — ExportRenderTarget branches on exactly that value and writes an EXR instead.
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->bAutoGenerateMips = false;
	RT->InitCustomFormat(Width, Height, PF_B8G8R8A8, /*bForceLinearGamma=*/false);
	RT->UpdateResourceImmediate(/*bClearRenderTarget=*/true);

	USceneCaptureComponent2D* const Capture = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	TStrongObjectPtr<USceneCaptureComponent2D> CaptureGuard(Capture);
	Capture->TextureTarget = RT;
	// FinalColorLDR = fully post-processed with an opaque alpha. Any of the HDR/scene-colour sources
	// produces a picture that is technically correct and visually unjudgeable.
	Capture->CaptureSource = SCS_FinalColorLDR;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;   // else temporal history is rebuilt from nothing
	Capture->FOVAngle = FOV;
	Capture->SetRelativeLocationAndRotation(CamLoc, CamRot);   // unattached: relative IS world
	Capture->RegisterComponentWithWorld(World);
	Capture->CaptureScene();
	// Without this the export reads a target the render thread has not finished writing.
	FlushRenderingCommands();

	/* THE UMG HALF. The scene capture cannot see widgets — they are composited by the viewport, above
	 * the renderer. But the game's live Slate overlay is an ordinary SWidget, and FWidgetRenderer with
	 * its CLEAR DISABLED draws it straight onto the target the scene is already in. That is the whole
	 * composite: no pixel readback, no second image, no blend maths. */
	bool bDrewUI = false;
	FString UINote;
	if (bIncludeUI)
	{
		UGameViewportClient* const VC = World->GetGameViewport();
		const TSharedPtr<IGameLayerManager> LayerManager = VC ? VC->GetGameLayerManager() : nullptr;
		if (LayerManager.IsValid())
		{
			const TSharedRef<SWidget> Overlay =
				StaticCastSharedRef<SGameLayerManager>(LayerManager.ToSharedRef());
			// bInClearTarget=false is the entire trick — true would wipe the world back to the
			// clear colour and hand back a HUD floating on black.
			FWidgetRenderer* Renderer = new FWidgetRenderer(/*bUseGammaCorrection=*/false,
				/*bInClearTarget=*/false);
			Renderer->DrawWidget(RT, Overlay, /*Scale=*/1.0f, FVector2D(Width, Height),
				/*DeltaTime=*/0.0f, /*bDeferRenderTargetUpdate=*/false);
			FlushRenderingCommands();
			// Owns render-thread resources: hand it to the deferred cleanup queue, never delete it.
			BeginCleanup(Renderer);
			bDrewUI = true;
		}
		else
		{
			UINote = TEXT("no game layer manager - world only");
		}
	}

	int64 Bytes = 0;
	const FString Full = ExportRTToPng(World, RT, OutPngPath, Bytes);

	Capture->UnregisterComponent();
	Capture->DestroyComponent();
	RT->ReleaseResource();

	return FString::Printf(
		TEXT("{\"ok\":%s,\"png\":\"%s\",\"width\":%d,\"height\":%d,\"bytes\":%lld,\"ui\":%s,")
		TEXT("\"fov\":%.1f,\"loc\":%s,\"note\":\"%s\"}"),
		Bytes > 0 ? TEXT("true") : TEXT("false"), *EscapeJson(Full), Width, Height,
		static_cast<long long>(Bytes), bDrewUI ? TEXT("true") : TEXT("false"),
		FOV, *VecJson(CamLoc),
		*EscapeJson(Bytes > 0 ? UINote
			: FString::Printf(TEXT("captured but NOTHING WRITTEN to %s (usually an unwritable path)"), *Full)));
}
