#include "FableRender.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Slate/WidgetRenderer.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "PixelFormat.h" // EPixelFormat / PF_B8G8R8A8 — Core in 5.6, not RHI
// FlushRenderingCommands / BeginCleanup — RenderCore. Both are load-bearing here (see the notes at the
// draw site), so declare the dependency rather than leaning on a transitive include.
#include "RenderingThread.h"
#include "RenderDeferredCleanup.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"
#include "Editor.h"

// RenderMesh: a throwaway preview world + a scene capture, so nothing is ever spawned into the level
// the user has open. Live-mode widget renders reuse the same idea: their throwaway world hosts the
// minimal player chain UUserWidget::Initialize demands before it will run NativeOnInitialized.
#include "PreviewScene.h"
#include "GameFramework/PlayerController.h"
#include "Engine/LocalPlayer.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/StrongObjectPtr.h"

// The JSON reply vocabulary (FJObj/NewObj/ToJson/Err/ErrWithList/CheckMutate) is shared with the
// other bridge files — see FableJson.h for why it must NOT be redeclared here.
#include "FableJson.h"
using namespace FableKitPrivate;

/**
 * Helper names in here are deliberately "Wr"-prefixed and unique across the plugin.
 *
 * Every Fable* bridge .cpp ends with a file-scope `using namespace <its own private namespace>`, and a
 * UE unity build concatenates several of them into ONE translation unit. Two same-named helpers in two
 * such namespaces then both become visible and every call is `error C2668: ambiguous call` — which is
 * exactly why FableJson.h exists. A generic name like Norm() here would collide with
 * FableNiagaraPrivate::Norm the moment the unity chunker put the two files together.
 */
namespace FableRenderPrivate
{
	static FString WrNormAssetPath(const FString& InPath)
	{
		FString P = InPath;
		P.TrimStartAndEndInline();
		if (!P.Contains(TEXT("."))) { P = P + TEXT(".") + FPackageName::GetShortName(P); }
		return P;
	}

	/** Accept a Widget Blueprint asset path (the normal case) or a straight UClass path. */
	static UClass* WrResolveWidgetClass(const FString& Path, FString& OutErr, FString& OutResolvedPath)
	{
		if (Path.IsEmpty()) { OutErr = TEXT("BlueprintPath is empty"); return nullptr; }
		const FString Norm = WrNormAssetPath(Path);

		// Quiet: a miss here is not an error, it just means we fall through to the class lookup.
		if (UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *Norm, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			OutResolvedPath = WBP->GetPathName();
			if (!WBP->GeneratedClass)
			{
				OutErr = FString::Printf(TEXT("%s has no GeneratedClass - compile the Widget Blueprint first."), *Norm);
				return nullptr;
			}
			return WBP->GeneratedClass;
		}

		// "/Game/UI/WBP_X.WBP_X_C" or a native "/Script/Deliria.SSomeWidget". Use the path verbatim:
		// class paths already carry their own suffix and must not be re-normalised.
		if (UClass* Cls = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			OutResolvedPath = Cls->GetPathName();
			return Cls;
		}

		OutErr = FString::Printf(TEXT("No WidgetBlueprint or UClass found at: %s"), *Norm);
		return nullptr;
	}

	/** Background colour from the friendly forms documented on RenderWidget. */
	static bool WrParseColor(const FString& In, FLinearColor& Out, FString& OutErr)
	{
		FString S = In;
		S.TrimStartAndEndInline();
		if (S.IsEmpty()) { return true; } // keep the default

		const FString L = S.ToLower();
		if (L == TEXT("transparent") || L == TEXT("none")) { Out = FLinearColor(0.f, 0.f, 0.f, 0.f);        return true; }
		if (L == TEXT("black"))                           { Out = FLinearColor(0.f, 0.f, 0.f, 1.f);        return true; }
		if (L == TEXT("white"))                           { Out = FLinearColor(1.f, 1.f, 1.f, 1.f);        return true; }
		if (L == TEXT("dark"))                            { Out = FLinearColor(0.02f, 0.02f, 0.025f, 1.f); return true; }
		if (L == TEXT("light"))                           { Out = FLinearColor(0.85f, 0.85f, 0.87f, 1.f);  return true; }
		if (L == TEXT("grey") || L == TEXT("gray"))       { Out = FLinearColor(0.18f, 0.18f, 0.18f, 1.f);  return true; }

		if (S.StartsWith(TEXT("#")))
		{
			// Hex is how a human writes a UI colour, i.e. sRGB — convert, don't paste the bytes into a
			// linear colour or every background comes out visibly too bright.
			const FColor Srgb = FColor::FromHex(S);
			Out = FLinearColor::FromSRGBColor(Srgb);
			Out.A = static_cast<float>(Srgb.A) / 255.0f; // alpha is linear already
			return true;
		}

		if (S.StartsWith(TEXT("(")))
		{
			if (Out.InitFromString(S)) { return true; }
			OutErr = FString::Printf(TEXT("background: could not parse struct literal '%s'"), *S);
			return false;
		}

		TArray<FString> Parts;
		S.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 3 || Parts.Num() == 4)
		{
			Out.R = FCString::Atof(*Parts[0]);
			Out.G = FCString::Atof(*Parts[1]);
			Out.B = FCString::Atof(*Parts[2]);
			Out.A = (Parts.Num() == 4) ? FCString::Atof(*Parts[3]) : 1.0f;
			return true;
		}

		OutErr = FString::Printf(
			TEXT("background: unrecognised value '%s' - want a name (transparent|black|white|dark|light|grey), ")
			TEXT("'#RRGGBB[AA]', linear 'R,G,B[,A]', or '(R=..,G=..,B=..,A=..)'"), *S);
		return false;
	}

	/**
	 * A UUserWidget needs a UWorld to be outered to — UUserWidget::GetWorld() walks the outer chain.
	 * There is no PIE world in a plain editor session, so use the editor world.
	 *
	 * Deliberately NOT GEditor->GetEditorWorldContext(): that helper ends in check(false) when no
	 * EWorldType::Editor context exists, which would take the editor down on the one configuration we
	 * are trying to report cleanly. Walking the context list degrades instead. The last resort is a bare
	 * transient UWorld, which is what UWidgetBlueprint::DetectSlateWidgetLeaks does for the same reason —
	 * it is inert, but it is a valid outer.
	 */
	static UWorld* WrPickWorld(bool& bOutTransient)
	{
		bOutTransient = false;

		if (GEditor)
		{
			for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
			{
				if (Ctx.WorldType == EWorldType::Editor && Ctx.World() != nullptr)
				{
					return Ctx.World();
				}
			}
		}
		if (GWorld) { return GWorld; }

		bOutTransient = true;
		return NewObject<UWorld>();
	}

	/**
	 * The "live" widget lifecycle needs a PLAYER CONTEXT: UUserWidget::Initialize only calls
	 * NativeOnInitialized when PlayerContext.IsValid() (UserWidget.cpp — the backward-compat gate),
	 * and IsValid demands the whole chain: a ULocalPlayer, a world, a PlayerController resolvable
	 * from the pair, and that controller's ->Player set. Without it, "live" ran NativeConstruct but
	 * silently skipped NativeOnInitialized — which is where this project's screens compose.
	 *
	 * Build the minimum of that chain in a THROWAWAY preview world (never the user's level — same
	 * isolation RenderMesh uses). The controller is a bare engine APlayerController with a default
	 * APlayerState; game code reading GetPS()/GetGS()/GetGameInstance() gets null and must guard,
	 * which the project's screens do. Destroyed with the scene when the call returns.
	 */
	struct FWrLivePlayerChain
	{
		TUniquePtr<FPreviewScene> Scene;
		APlayerController* PC = nullptr;
		ULocalPlayer* LP = nullptr;

		bool Build()
		{
			Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
				.SetCreatePhysicsScene(false)
				.SetTransactional(false));
			UWorld* World = Scene ? Scene->GetWorld() : nullptr;
			if (!World) { return false; }

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			PC = World->SpawnActor<APlayerController>(SpawnParams);
			// The engine base class on purpose, not GEngine->LocalPlayerClass: a game-specific local
			// player may assume a viewport client this world does not have. Widget code that casts to
			// the game's class gets null and guards, same as every other absent-game-state read here.
			LP = NewObject<ULocalPlayer>(GEngine, ULocalPlayer::StaticClass(), NAME_None, RF_Transient);
			if (!PC || !LP) { return false; }
			PC->Player = LP;
			LP->PlayerController = PC;
			return true;
		}
		UWorld* GetWorld() const { return Scene ? Scene->GetWorld() : nullptr; }
	};

	/**
	 * Invoke one scripted call from the "calls" option on the widget: ["FuncName", arg, arg...].
	 * Args fill the UFunction's parameters POSITIONALLY; supported parameter types are the scalar
	 * set a state-driving call actually needs (bool / ints / float / double / FString / FName /
	 * FText / enum bytes). Anything else refuses loudly — a silently skipped call would make a
	 * conformance render lie, which is the exact failure class this option exists to kill.
	 */
	static bool WrInvokeScriptedCall(UUserWidget* Widget, const TArray<TSharedPtr<FJsonValue>>& Call, FString& OutErr)
	{
		if (Call.Num() < 1 || !Call[0].IsValid())
		{
			OutErr = TEXT("calls: each entry must be a non-empty array starting with the function name");
			return false;
		}
		const FString FuncName = Call[0]->AsString();
		UFunction* Func = Widget->FindFunction(FName(*FuncName));
		if (!Func)
		{
			OutErr = FString::Printf(TEXT("calls: %s has no function named '%s'"),
				*Widget->GetClass()->GetName(), *FuncName);
			return false;
		}

		TArray<uint8> Parms;
		Parms.SetNumZeroed(FMath::Max<int32>(Func->ParmsSize, 1));
		// Two passes around the fill: FText/FString parameters have real constructors, and a
		// memzeroed block is not a constructed value.
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Parms.GetData());
		}

		bool bOk = true;
		int32 ArgIndex = 1;
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { continue; }
			if (ArgIndex >= Call.Num()) { break; }   // unsupplied params keep their default-initialised value
			const TSharedPtr<FJsonValue>& Arg = Call[ArgIndex++];
			if (!Arg.IsValid()) { continue; }

			if (FBoolProperty* BP = CastField<FBoolProperty>(*It))        { BP->SetPropertyValue_InContainer(Parms.GetData(), Arg->AsBool()); }
			else if (FFloatProperty* FP = CastField<FFloatProperty>(*It)) { FP->SetPropertyValue_InContainer(Parms.GetData(), static_cast<float>(Arg->AsNumber())); }
			else if (FDoubleProperty* DP = CastField<FDoubleProperty>(*It)) { DP->SetPropertyValue_InContainer(Parms.GetData(), Arg->AsNumber()); }
			else if (FIntProperty* IP = CastField<FIntProperty>(*It))     { IP->SetPropertyValue_InContainer(Parms.GetData(), static_cast<int32>(Arg->AsNumber())); }
			else if (FInt64Property* I64 = CastField<FInt64Property>(*It)) { I64->SetPropertyValue_InContainer(Parms.GetData(), static_cast<int64>(Arg->AsNumber())); }
			else if (FByteProperty* BY = CastField<FByteProperty>(*It))   { BY->SetPropertyValue_InContainer(Parms.GetData(), static_cast<uint8>(Arg->AsNumber())); }
			else if (FEnumProperty* EP = CastField<FEnumProperty>(*It))
			{
				EP->GetUnderlyingProperty()->SetIntPropertyValue(
					EP->ContainerPtrToValuePtr<void>(Parms.GetData()), static_cast<int64>(Arg->AsNumber()));
			}
			else if (FStrProperty* SP = CastField<FStrProperty>(*It))     { SP->SetPropertyValue_InContainer(Parms.GetData(), Arg->AsString()); }
			else if (FNameProperty* NP = CastField<FNameProperty>(*It))   { NP->SetPropertyValue_InContainer(Parms.GetData(), FName(*Arg->AsString())); }
			else if (FTextProperty* TP = CastField<FTextProperty>(*It))   { TP->SetPropertyValue_InContainer(Parms.GetData(), FText::FromString(Arg->AsString())); }
			else
			{
				OutErr = FString::Printf(TEXT("calls: %s parameter '%s' is a %s — unsupported for scripted calls"),
					*FuncName, *It->GetName(), *It->GetClass()->GetName());
				bOk = false;
				break;
			}
		}

		if (bOk)
		{
			Widget->ProcessEvent(Func, Parms.GetData());
		}
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Parms.GetData());
		}
		return bOk;
	}

	/** Shared PNG write-out: delete first, export, report the byte count. Returns the resolved path. */
	static FString WrExportPng(UWorld* World, UTextureRenderTarget2D* RT, const FString& OutPngPath, int64& OutBytes)
	{
		FString FullPath = OutPngPath;
		FPaths::NormalizeFilename(FullPath);
		if (!FPaths::GetExtension(FullPath).Equals(TEXT("png"), ESearchCase::IgnoreCase))
		{
			FullPath += TEXT(".png");
		}
		FullPath = FPaths::ConvertRelativePathToFull(FullPath);

		const FString Dir = FPaths::GetPath(FullPath);
		const FString FileName = FPaths::GetCleanFilename(FullPath);
		if (!Dir.IsEmpty() && !IFileManager::Get().DirectoryExists(*Dir))
		{
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		}

		// ExportRenderTarget returns void, so a stale file left here would read back as success.
		IFileManager::Get().Delete(*FullPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		UKismetRenderingLibrary::ExportRenderTarget(World, RT, Dir, FileName);
		OutBytes = IFileManager::Get().FileSize(*FullPath);
		return FullPath;
	}
}

using namespace FableRenderPrivate;

FString UFableRender::RenderWidget(const FString& BlueprintPath, const FString& OutPngPath, int32 Width, int32 Height, const FString& OptionsJson)
{
	/* ---------------- environment ---------------- */

	if (!IsInGameThread())
	{
		return Err(TEXT("RenderWidget must run on the game thread."));
	}
	if (!FSlateApplication::IsInitialized())
	{
		return Err(TEXT("Slate is not initialised - widget rendering needs a live editor (not a commandlet, not -nullrhi)."));
	}
	if (OutPngPath.IsEmpty())
	{
		return Err(TEXT("OutPngPath is empty."));
	}

	/* ---------------- options ---------------- */

	FLinearColor Background(0.02f, 0.02f, 0.025f, 1.0f);
	float Scale = 1.0f;
	bool bPreConstruct = true;
	bool bLive = false;
	TArray<TSharedPtr<FJsonValue>> ScriptedCalls;

	if (!OptionsJson.IsEmpty() && OptionsJson != TEXT("{}"))
	{
		FJObj Opts;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OptionsJson), Opts) || !Opts.IsValid())
		{
			return Err(TEXT("OptionsJson is not a JSON object."));
		}

		FString ColorText;
		if (Opts->TryGetStringField(TEXT("background"), ColorText))
		{
			FString ColorErr;
			if (!WrParseColor(ColorText, Background, ColorErr)) { return Err(ColorErr); }
		}

		double ScaleNum = 0.0;
		if (Opts->TryGetNumberField(TEXT("scale"), ScaleNum)) { Scale = static_cast<float>(ScaleNum); }

		bool bOpt = false;
		if (Opts->TryGetBoolField(TEXT("pre_construct"), bOpt)) { bPreConstruct = bOpt; }
		if (Opts->TryGetBoolField(TEXT("live"), bOpt))          { bLive = bOpt; }
		const TArray<TSharedPtr<FJsonValue>>* CallsArr = nullptr;
		if (Opts->TryGetArrayField(TEXT("calls"), CallsArr) && CallsArr) { ScriptedCalls = *CallsArr; }
	}

	if (Scale <= 0.0f || Scale > 8.0f)
	{
		return Err(FString::Printf(TEXT("scale must be > 0 and <= 8 (got %f)."), Scale));
	}

	/* ---------------- resolve the class ---------------- */

	FString LoadErr, ResolvedPath;
	UClass* WidgetClass = WrResolveWidgetClass(BlueprintPath, LoadErr, ResolvedPath);
	if (!WidgetClass) { return Err(LoadErr); }

	if (!WidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return Err(FString::Printf(TEXT("%s is not a UUserWidget class (it is a %s) - nothing to render."),
			*ResolvedPath, *WidgetClass->GetName()));
	}
	if (WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return Err(FString::Printf(TEXT("%s cannot be instantiated (abstract, deprecated or a stale REINST_ class). ")
			TEXT("Render a concrete subclass, or recompile the Blueprint."), *ResolvedPath));
	}

	/* ---------------- construct the widget ---------------- */

	// Declared before the widget guard on purpose: locals destroy in reverse order, so the preview
	// world (the widget's outer in live mode) outlives the widget's strong ref.
	FWrLivePlayerChain LiveChain;
	bool bLivePlayer = false;

	bool bTransientWorld = false;
	UWorld* World = nullptr;
	if (bLive && LiveChain.Build())
	{
		World = LiveChain.GetWorld();
		bLivePlayer = true;
	}
	if (!World)
	{
		World = WrPickWorld(bTransientWorld);
	}
	if (!World)
	{
		return Err(TEXT("No world available to outer the widget to (no editor world, no GWorld, and a transient world could not be created)."));
	}
	// Only root a world we made ourselves; the editor world is already rooted.
	TStrongObjectPtr<UWorld> TransientWorldGuard(bTransientWorld ? World : nullptr);

	UUserWidget* Widget = NewObject<UUserWidget>(World, WidgetClass, NAME_None, RF_Transient);
	if (!Widget)
	{
		return Err(FString::Printf(TEXT("NewObject failed for %s."), *ResolvedPath));
	}
	// Strong ref for the duration of the call; dropped at return so GC reclaims the widget and nothing
	// accumulates across repeated calls.
	TStrongObjectPtr<UUserWidget> WidgetGuard(Widget);

	if (bLivePlayer)
	{
		// What unlocks NativeOnInitialized inside Initialize() — see FWrLivePlayerChain.
		Widget->SetPlayerContext(FLocalPlayerContext(LiveChain.LP, World));
	}

#if WITH_EDITOR
	/* THE safety mechanism, and the reason a widget that null-derefs its PlayerController in
	 * NativeConstruct cannot take the editor down here: with Designing set, IsDesignTime() is true, so
	 * UWidget::OnWidgetRebuilt takes its design-time branch and never calls NativeConstruct — and
	 * UUserWidget::Initialize likewise never calls NativeOnInitialized. Same flag pairing as the
	 * engine's own UWidgetBlueprintThumbnailRenderer.
	 *
	 * "live" OPTS OUT of that safety, deliberately: this project's screens COMPOSE THEIR LAYOUT in
	 * NativeOnInitialized (the chest, the NPC menu, the achievements chrome, the bestiary rebuild),
	 * so a Designing render shows only the raw authored asset — useless for conformance. With no
	 * designer flags the widget takes the real runtime path (NativeOnInitialized at Initialize;
	 * NativePreConstruct + NativeConstruct from OnWidgetRebuilt inside TakeWidget). The trade is
	 * real: game code runs with NO owning player, so a widget that hard-derefs GetOwningPlayer()/
	 * PlayerState CAN take the editor down. Per-call opt-in; null-guarded widgets only. */
	if (!bLive)
	{
		EWidgetDesignFlags DesignFlags = EWidgetDesignFlags::Designing;
		if (bPreConstruct) { DesignFlags |= EWidgetDesignFlags::ExecutePreConstruct; }
		Widget->SetDesignerFlags(DesignFlags);
	}
#endif

	Widget->Initialize();

	// TakeWidget builds the Slate tree (RebuildWidget + SynchronizeProperties + OnWidgetRebuilt).
	TSharedRef<SWidget> SlateWidget = Widget->TakeWidget();

	/* ---------------- scripted state ("calls") ----------------
	 * Drive the widget into the STATE being verified — a search filter, a selected category, a
	 * service page — after the full lifecycle, before the draw. A failed call fails the render:
	 * a conformance image of the wrong state is worse than no image. */
	int32 CallsRun = 0;
	for (const TSharedPtr<FJsonValue>& CallValue : ScriptedCalls)
	{
		const TArray<TSharedPtr<FJsonValue>>* CallArr = nullptr;
		if (!CallValue.IsValid() || !CallValue->TryGetArray(CallArr) || !CallArr)
		{
			return Err(TEXT("calls: every entry must be an array like [\"SetSearchFilter\", \"fps\"]."));
		}
		FString CallErr;
		if (!WrInvokeScriptedCall(Widget, *CallArr, CallErr))
		{
			return Err(CallErr);
		}
		++CallsRun;
	}

	/* ---------------- size ---------------- */

	SlateWidget->SlatePrepass(Scale);
	const FVector2D Desired = SlateWidget->GetDesiredSize();

	FString SizeSource = TEXT("explicit");
	FVector2D DrawSize(static_cast<double>(Width), static_cast<double>(Height));

	if (Width <= 0 || Height <= 0)
	{
		FVector2D Fallback(0.0, 0.0);
#if WITH_EDITORONLY_DATA
		Fallback = Widget->DesignTimeSize;   // what the designer canvas was actually set to
		SizeSource = TEXT("design_time");
#endif
		if (Fallback.X < 1.0 || Fallback.Y < 1.0)
		{
			Fallback = Desired;
			SizeSource = TEXT("desired");
		}
		if (Fallback.X < 1.0 || Fallback.Y < 1.0)
		{
			return Err(TEXT("Auto-size failed: the widget has no design-time size and measures 0x0 (a root Canvas Panel ")
			           TEXT("always does). Pass explicit Width and Height, e.g. 1920 1080."));
		}
		DrawSize = Fallback;
	}

	DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 1.0, 8192.0);
	DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 1.0, 8192.0);

	/* ---------------- render target ---------------- */

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	TStrongObjectPtr<UTextureRenderTarget2D> RenderTargetGuard(RenderTarget);

	RenderTarget->Filter = TF_Bilinear;
	// ClearColor becomes the RHI fast-clear binding, which is what FWidgetRenderer's EClear load action
	// resolves to — so this IS the background the widget is composited over.
	RenderTarget->ClearColor = Background;
	RenderTarget->SRGB = true;
	// MUST NOT be RTF_RGBA16f: UKismetRenderingLibrary::ExportRenderTarget branches on exactly that value
	// and writes EXR/HDR instead of PNG for it.
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->bAutoGenerateMips = false;

	FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer();
	const EPixelFormat PixelFormat = SlateRenderer ? SlateRenderer->GetSlateRecommendedColorFormat() : PF_B8G8R8A8;

	// bForceLinearGamma=false + SRGB=true is the pairing the editor's own widget thumbnail uses: the
	// target stores display-ready sRGB bytes, so the PNG readback needs no further correction.
	RenderTarget->InitCustomFormat(static_cast<uint32>(DrawSize.X), static_cast<uint32>(DrawSize.Y), PixelFormat, /*bForceLinearGamma*/ false);
	RenderTarget->UpdateResourceImmediate(/*bClearRenderTarget*/ true);

	/* ---------------- draw ---------------- */

	// FWidgetRenderer owns render-thread resources and is an FDeferredCleanupInterface: heap-allocate and
	// hand it to BeginCleanup, never destroy it inline on the game thread.
	FWidgetRenderer* Renderer = new FWidgetRenderer(/*bUseGammaCorrection*/ false, /*bInClearTarget*/ true);
	Renderer->SetIsPrepassNeeded(true);
	Renderer->DrawWidget(RenderTarget, SlateWidget, Scale, DrawSize, /*DeltaTime*/ 0.0f, /*bDeferRenderTargetUpdate*/ false);

	// Without this the export below reads back a target the render thread has not finished writing —
	// i.e. a blank or half-drawn PNG. This is the single easiest way to get a "why is it empty" bug.
	FlushRenderingCommands();

	/* ---------------- export ---------------- */

	int64 Bytes = 0;
	const FString FullPath = WrExportPng(World, RenderTarget, OutPngPath, Bytes);

	/* ---------------- teardown ---------------- */

	BeginCleanup(Renderer);
	Renderer = nullptr;
	Widget->ReleaseSlateResources(/*bReleaseChildren*/ true);
	RenderTarget->ReleaseResource();
	// SlateWidget / WidgetGuard / RenderTargetGuard / TransientWorldGuard all release at return, in
	// reverse declaration order (Slate ref first), leaving nothing rooted.

	/* ---------------- reply ---------------- */

	FJObj Out = NewObj();
	Out->SetBoolField(TEXT("ok"), Bytes > 0);
	if (Bytes <= 0)
	{
		Out->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Rendered, but nothing was written to %s. Check the Output Log for a 'Blueprint' message-log warning; ")
			TEXT("the usual cause is an unwritable or invalid output path."), *FullPath));
	}
	Out->SetStringField(TEXT("widget"), ResolvedPath);
	Out->SetStringField(TEXT("class"), WidgetClass->GetPathName());
	Out->SetStringField(TEXT("png"), FullPath);
	Out->SetNumberField(TEXT("width"), DrawSize.X);
	Out->SetNumberField(TEXT("height"), DrawSize.Y);
	Out->SetNumberField(TEXT("scale"), Scale);
	Out->SetStringField(TEXT("size_source"), SizeSource);
	Out->SetNumberField(TEXT("desired_w"), Desired.X);
	Out->SetNumberField(TEXT("desired_h"), Desired.Y);
	Out->SetStringField(TEXT("background"), Background.ToString());
	Out->SetBoolField(TEXT("pre_construct"), bPreConstruct);
	Out->SetBoolField(TEXT("live"), bLive);
	Out->SetBoolField(TEXT("live_player"), bLivePlayer);
	Out->SetNumberField(TEXT("calls_run"), CallsRun);
	Out->SetBoolField(TEXT("transient_world"), bTransientWorld);
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	return ToJson(Out);
}

FString UFableRender::MeasureWidget(const FString& BlueprintPath, int32 Width, int32 Height, const FString& OptionsJson)
{
	if (!IsInGameThread())
	{
		return Err(TEXT("MeasureWidget must run on the game thread."));
	}
	if (!FSlateApplication::IsInitialized())
	{
		return Err(TEXT("Slate is not initialised - measuring needs a live editor (not a commandlet, not -nullrhi)."));
	}

	float Scale = 1.0f;
	bool bPreConstruct = true;
	bool bLive = false;
	if (!OptionsJson.IsEmpty() && OptionsJson != TEXT("{}"))
	{
		FJObj Opts;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OptionsJson), Opts) || !Opts.IsValid())
		{
			return Err(TEXT("OptionsJson is not a JSON object."));
		}
		double ScaleNum = 0.0;
		if (Opts->TryGetNumberField(TEXT("scale"), ScaleNum)) { Scale = static_cast<float>(ScaleNum); }
		bool bOpt = false;
		if (Opts->TryGetBoolField(TEXT("pre_construct"), bOpt)) { bPreConstruct = bOpt; }
		if (Opts->TryGetBoolField(TEXT("live"), bOpt))          { bLive = bOpt; }
	}
	if (Scale <= 0.0f || Scale > 8.0f)
	{
		return Err(FString::Printf(TEXT("scale must be > 0 and <= 8 (got %f)."), Scale));
	}

	FString LoadErr, ResolvedPath;
	UClass* WidgetClass = WrResolveWidgetClass(BlueprintPath, LoadErr, ResolvedPath);
	if (!WidgetClass) { return Err(LoadErr); }
	if (!WidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return Err(FString::Printf(TEXT("%s is not a UUserWidget class."), *ResolvedPath));
	}
	if (WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return Err(FString::Printf(TEXT("%s cannot be instantiated (abstract/deprecated/stale REINST_)."), *ResolvedPath));
	}

	FWrLivePlayerChain LiveChain;
	bool bLivePlayer = false;
	bool bTransientWorld = false;
	UWorld* World = nullptr;
	if (bLive && LiveChain.Build())
	{
		World = LiveChain.GetWorld();
		bLivePlayer = true;
	}
	if (!World) { World = WrPickWorld(bTransientWorld); }
	if (!World) { return Err(TEXT("No world available to outer the widget to.")); }
	TStrongObjectPtr<UWorld> TransientWorldGuard(bTransientWorld ? World : nullptr);

	UUserWidget* Widget = NewObject<UUserWidget>(World, WidgetClass, NAME_None, RF_Transient);
	if (!Widget) { return Err(FString::Printf(TEXT("NewObject failed for %s."), *ResolvedPath)); }
	TStrongObjectPtr<UUserWidget> WidgetGuard(Widget);
	if (bLivePlayer)
	{
		Widget->SetPlayerContext(FLocalPlayerContext(LiveChain.LP, World));
	}

#if WITH_EDITOR
	// Same "live" contract as RenderWidget: measuring a runtime-composed screen needs the runtime
	// lifecycle, or the tree being measured is the raw authored asset rather than what ships.
	if (!bLive)
	{
		EWidgetDesignFlags DesignFlags = EWidgetDesignFlags::Designing;
		if (bPreConstruct) { DesignFlags |= EWidgetDesignFlags::ExecutePreConstruct; }
		Widget->SetDesignerFlags(DesignFlags);
	}
#endif
	Widget->Initialize();

	TSharedRef<SWidget> SlateWidget = Widget->TakeWidget();

	/* A prepass is what fills in every widget's desired size, which is the number that answers "why is
	 * this panel 305 tall". Width/Height are accepted for symmetry with RenderWidget but do not
	 * constrain the measure: desired size is by definition the UNCONSTRAINED ask, and that is exactly
	 * the question being asked when hunting an unexpected height. */
	SlateWidget->SlatePrepass(Scale);
	const FVector2D RootDesired = SlateWidget->GetDesiredSize();

	TArray<FJVal> Rows;
	if (Widget->WidgetTree)
	{
		Widget->WidgetTree->ForEachWidget([&Rows](UWidget* Ch)
		{
			if (!Ch) { return; }
			FJObj Row = NewObj();
			Row->SetStringField(TEXT("name"), Ch->GetName());
			Row->SetStringField(TEXT("class"), Ch->GetClass()->GetName());
			Row->SetStringField(TEXT("parent"), Ch->GetParent() ? Ch->GetParent()->GetName() : TEXT(""));

			// GetCachedWidget is the Slate widget this UWidget built during TakeWidget.
			TSharedPtr<SWidget> S = Ch->GetCachedWidget();
			const FVector2D D = S.IsValid() ? S->GetDesiredSize() : FVector2D::ZeroVector;
			Row->SetNumberField(TEXT("desired_w"), D.X);
			Row->SetNumberField(TEXT("desired_h"), D.Y);
			Row->SetBoolField(TEXT("built"), S.IsValid());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		});
	}

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("widget"), ResolvedPath);
	O->SetNumberField(TEXT("root_w"), RootDesired.X);
	O->SetNumberField(TEXT("root_h"), RootDesired.Y);
	O->SetNumberField(TEXT("scale"), Scale);
	O->SetBoolField(TEXT("live"), bLive);
	O->SetBoolField(TEXT("live_player"), bLivePlayer);
	O->SetArrayField(TEXT("widgets"), Rows);
	return ToJson(O);
}

FString UFableRender::RenderMesh(const FString& AssetPath, const FString& OutPngPath, int32 Width, int32 Height, const FString& OptionsJson)
{
	/* ---------------- environment ---------------- */

	if (!IsInGameThread())
	{
		return Err(TEXT("RenderMesh must run on the game thread."));
	}
	if (!GEditor || !FApp::CanEverRender())
	{
		return Err(TEXT("RenderMesh needs a live editor with a renderer (not a commandlet, not -nullrhi)."));
	}
	if (OutPngPath.IsEmpty())
	{
		return Err(TEXT("OutPngPath is empty."));
	}

	/* ---------------- options ---------------- */

	float Yaw = 35.0f, Pitch = -20.0f, Roll = 0.0f;
	float Distance = 0.0f, FOV = 45.0f, Exposure = 1.0f;
	float LightYaw = 45.0f, LightPitch = -35.0f;
	FString MaterialPath;

	if (!OptionsJson.IsEmpty() && OptionsJson != TEXT("{}"))
	{
		FJObj Opts;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OptionsJson), Opts) || !Opts.IsValid())
		{
			return Err(TEXT("OptionsJson is not a JSON object."));
		}
		double N = 0.0;
		if (Opts->TryGetNumberField(TEXT("yaw"), N))         { Yaw = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("pitch"), N))       { Pitch = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("roll"), N))        { Roll = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("distance"), N))    { Distance = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("fov"), N))         { FOV = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("exposure"), N))    { Exposure = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("light_yaw"), N))   { LightYaw = static_cast<float>(N); }
		if (Opts->TryGetNumberField(TEXT("light_pitch"), N)) { LightPitch = static_cast<float>(N); }
		Opts->TryGetStringField(TEXT("material"), MaterialPath);
	}

	if (FOV <= 1.0f || FOV >= 179.0f)
	{
		return Err(FString::Printf(TEXT("fov must be between 1 and 179 (got %f)."), FOV));
	}

	const int32 W = (Width  > 0) ? FMath::Min(Width,  8192) : 1024;
	const int32 H = (Height > 0) ? FMath::Min(Height, 8192) : 1024;

	/* ---------------- resolve assets ---------------- */

	const FString Norm = WrNormAssetPath(AssetPath);
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Norm, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Mesh)
	{
		return Err(FString::Printf(TEXT("No StaticMesh found at: %s"), *Norm));
	}

	UMaterialInterface* OverrideMaterial = nullptr;
	if (!MaterialPath.IsEmpty())
	{
		const FString MatNorm = WrNormAssetPath(MaterialPath);
		OverrideMaterial = LoadObject<UMaterialInterface>(nullptr, *MatNorm, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!OverrideMaterial)
		{
			return Err(FString::Printf(TEXT("material override: no MaterialInterface at %s"), *MatNorm));
		}
	}

	/* ---------------- preview scene ---------------- */

	// Its own world. This is the whole reason RenderMesh does not just spawn into the editor world:
	// nothing here can dirty, or even be seen in, the level the user has open.
	FPreviewScene PreviewScene(FPreviewScene::ConstructionValues()
		.SetCreatePhysicsScene(false)
		.SetTransactional(false));
	UWorld* World = PreviewScene.GetWorld();
	if (!World)
	{
		return Err(TEXT("FPreviewScene produced no world."));
	}
	PreviewScene.SetLightDirection(FRotator(LightPitch, LightYaw, 0.0f));

	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	MeshComp->SetStaticMesh(Mesh);
	if (OverrideMaterial)
	{
		for (int32 i = 0; i < FMath::Max(MeshComp->GetNumMaterials(), 1); ++i)
		{
			MeshComp->SetMaterial(i, OverrideMaterial);
		}
	}
	PreviewScene.AddComponent(MeshComp, FTransform::Identity);

	/* ---------------- frame it ---------------- */

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const float Radius = FMath::Max(static_cast<float>(Bounds.SphereRadius), 1.0f);
	if (Distance <= 0.0f)
	{
		// Fit the bounding sphere with a margin, so any yaw/pitch keeps the whole mesh on screen.
		Distance = (Radius / FMath::Tan(FMath::DegreesToRadians(FOV * 0.5f))) * 1.35f;
	}

	const FRotator CamRot(Pitch, Yaw, Roll);
	const FVector Target = Bounds.Origin;
	const FVector CamLoc = Target - CamRot.Vector() * Distance;

	/* ---------------- render target ---------------- */

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	TStrongObjectPtr<UTextureRenderTarget2D> RenderTargetGuard(RenderTarget);
	RenderTarget->ClearColor = FLinearColor(0.02f, 0.02f, 0.025f, 1.0f);
	RenderTarget->SRGB = true;
	// NOT RTF_RGBA16f — ExportRenderTarget branches on it and writes EXR instead of PNG.
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitCustomFormat(W, H, PF_B8G8R8A8, /*bForceLinearGamma*/ false);
	RenderTarget->UpdateResourceImmediate(true);

	/* ---------------- capture ---------------- */

	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Capture->TextureTarget = RenderTarget;
	Capture->CaptureSource = SCS_FinalColorLDR;   // post-processed + opaque alpha, so the PNG is viewable
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->FOVAngle = FOV;
	Capture->ShowFlags.SetAntiAliasing(true);
	// Auto-exposure in a scene containing one emissive object swings wildly between camera angles;
	// pinning it keeps a two-pass geometry/effect comparison actually comparable.
	Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
	Capture->PostProcessSettings.AutoExposureMethod = AEM_Manual;
	Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
	Capture->PostProcessSettings.AutoExposureBias = Exposure;
	PreviewScene.AddComponent(Capture, FTransform(CamRot, CamLoc));

	Capture->CaptureScene();
	// Without this the export reads back a target the render thread has not finished writing.
	FlushRenderingCommands();

	/* ---------------- export ---------------- */

	int64 Bytes = 0;
	const FString FullPath = WrExportPng(World, RenderTarget, OutPngPath, Bytes);

	/* ---------------- teardown ---------------- */

	PreviewScene.RemoveComponent(Capture);
	PreviewScene.RemoveComponent(MeshComp);
	RenderTarget->ReleaseResource();

	/* ---------------- reply ---------------- */

	FJObj Out = NewObj();
	Out->SetBoolField(TEXT("ok"), Bytes > 0);
	if (Bytes <= 0)
	{
		Out->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Captured, but nothing was written to %s (usually an unwritable output path)."), *FullPath));
	}
	Out->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	Out->SetStringField(TEXT("png"), FullPath);
	Out->SetNumberField(TEXT("width"), W);
	Out->SetNumberField(TEXT("height"), H);
	Out->SetNumberField(TEXT("yaw"), Yaw);
	Out->SetNumberField(TEXT("pitch"), Pitch);
	Out->SetNumberField(TEXT("distance"), Distance);
	Out->SetNumberField(TEXT("fov"), FOV);
	Out->SetNumberField(TEXT("bounds_radius"), Radius);
	Out->SetStringField(TEXT("material_override"), OverrideMaterial ? OverrideMaterial->GetPathName() : TEXT(""));
	Out->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	return ToJson(Out);
}
