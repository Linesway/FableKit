#include "FableNiagara.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "NiagaraSystem.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraSystemFactoryNew.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Editor.h"
// TFieldIterator<FObjectPropertyBase> + FProperty::Import/ExportText_InContainer. CoreMinimal does not
// pull these in; relying on a transitive include from a Niagara header is how this breaks on someone
// else's unity-build chunking.
#include "UObject/UnrealType.h"
// TObjectIterator, for the PrepareForEdit component sweep below.
#include "UObject/UObjectIterator.h"

// The JSON reply vocabulary (FJObj/NewObj/ToJson/Err/ErrWithList/CheckMutate) is shared with the
// other bridge files — see FableJson.h for why it must NOT be redeclared here.
#include "FableJson.h"
using namespace FableKitPrivate;

namespace FableNiagaraPrivate
{
	static FString Norm(const FString& InPath)
	{
		FString P = InPath;
		P.TrimStartAndEndInline();
		if (!P.Contains(TEXT("."))) { P = P + TEXT(".") + FPackageName::GetShortName(P); }
		return P;
	}

	static UNiagaraSystem* LoadSystem(const FString& Path, FString& OutErr)
	{
		if (Path.IsEmpty()) { OutErr = TEXT("SystemPath is empty"); return nullptr; }
		UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *Norm(Path));
		if (!Sys) { OutErr = FString::Printf(TEXT("NiagaraSystem not found: %s"), *Norm(Path)); }
		return Sys;
	}

	/** Live UNiagaraComponents referencing a system keep pointers into its compiled data; mutating the
	 *  asset underneath them is the classic Niagara editor crash. Always clear them first.
	 *
	 *  This is FNiagaraEditorUtilities::KillSystemInstances reproduced from public API. That helper is
	 *  declared in NiagaraEditorUtilities.h but carries NO NIAGARAEDITOR_API export (the namespace
	 *  exports per-function — AddEmitterToSystem right above it does have the macro), so it is
	 *  module-internal and can never link from a plugin: LNK2019. Its body is exactly the loop below. */
	static void PrepareForEdit(UNiagaraSystem& Sys)
	{
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			UNiagaraComponent* Component = *It;
			if (Component && Component->GetAsset() == &Sys)
			{
				Component->DestroyInstance();
			}
		}
	}

	static FNiagaraEmitterHandle* FindHandle(UNiagaraSystem& Sys, const FString& EmitterName, TArray<FString>& OutNames)
	{
		FNiagaraEmitterHandle* Found = nullptr;
		for (FNiagaraEmitterHandle& H : Sys.GetEmitterHandles())
		{
			const FString N = H.GetName().ToString();
			OutNames.Add(N);
			if (!Found && N.Equals(EmitterName, ESearchCase::IgnoreCase)) { Found = &H; }
		}
		return Found;
	}

	static void RenderersToJson(const FNiagaraEmitterHandle& Handle, TArray<FJVal>& Out)
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data) { return; }
		int32 Index = 0;
		for (const UNiagaraRendererProperties* R : Data->GetRenderers())
		{
			FJObj O = NewObj();
			O->SetNumberField(TEXT("index"), Index++);
			O->SetStringField(TEXT("class"), R ? R->GetClass()->GetName() : TEXT("None"));
			if (R)
			{
				// Surface the asset-shaped properties (material / mesh / etc.) without needing to know
				// each renderer subclass — that's what makes this work for sprite, mesh and ribbon alike.
				for (TFieldIterator<FObjectPropertyBase> It(R->GetClass()); It; ++It)
				{
					const UObject* Value = It->GetObjectPropertyValue_InContainer(R);
					if (Value) { O->SetStringField(It->GetName(), Value->GetPathName()); }
				}
			}
			Out.Add(MakeShared<FJsonValueObject>(O));
		}
	}

	/** Set one FProperty from UE text. Shared by SetRendererProps. */
	static bool SetPropFromText(UObject* Target, const FString& PropName, const FString& ValueText, FString& OutErr)
	{
		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop) { OutErr = PropName + TEXT(" (no such property)"); return false; }
		if (!Prop->ImportText_InContainer(*ValueText, Target, Target, PPF_None))
		{
			OutErr = PropName + TEXT(" (ImportText rejected the value)");
			return false;
		}
		return true;
	}
}

using namespace FableNiagaraPrivate;

/* ================================ read ================================ */

FString UFableNiagara::Info(const FString& SystemPath)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("path"), Sys->GetPathName());

	TArray<FJVal> Emitters;
	for (const FNiagaraEmitterHandle& H : Sys->GetEmitterHandles())
	{
		FJObj EO = NewObj();
		EO->SetStringField(TEXT("name"), H.GetName().ToString());
		EO->SetBoolField(TEXT("enabled"), H.GetIsEnabled());
		TArray<FJVal> Renderers;
		RenderersToJson(H, Renderers);
		EO->SetArrayField(TEXT("renderers"), Renderers);
		Emitters.Add(MakeShared<FJsonValueObject>(EO));
	}
	O->SetArrayField(TEXT("emitters"), Emitters);

	TArray<FNiagaraVariable> Params;
	Sys->GetExposedParameters().GetParameters(Params);
	TArray<FJVal> ParamVals;
	for (const FNiagaraVariable& V : Params)
	{
		FJObj PO = NewObj();
		PO->SetStringField(TEXT("name"), V.GetName().ToString());
		PO->SetStringField(TEXT("type"), V.GetType().GetName());
		ParamVals.Add(MakeShared<FJsonValueObject>(PO));
	}
	O->SetArrayField(TEXT("user_params"), ParamVals);
	return ToJson(O);
}

FString UFableNiagara::ListUserParams(const FString& SystemPath)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }

	const FNiagaraUserRedirectionParameterStore& Store = Sys->GetExposedParameters();
	TArray<FNiagaraVariable> Params;
	Store.GetParameters(Params);

	TArray<FJVal> Out;
	for (const FNiagaraVariable& V : Params)
	{
		FJObj PO = NewObj();
		PO->SetStringField(TEXT("name"), V.GetName().ToString());
		PO->SetStringField(TEXT("type"), V.GetType().GetName());
		if (const UScriptStruct* Struct = V.GetType().GetScriptStruct())
		{
			if (const uint8* Data = Store.GetParameterData(V))
			{
				FString Text;
				Struct->ExportText(Text, Data, Data, nullptr, PPF_None, nullptr);
				PO->SetStringField(TEXT("value"), Text);
			}
		}
		Out.Add(MakeShared<FJsonValueObject>(PO));
	}
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetArrayField(TEXT("params"), Out);
	return ToJson(O);
}

FString UFableNiagara::DumpRenderer(const FString& SystemPath, const FString& EmitterName, int32 RendererIndex)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }

	TArray<FString> Names;
	FNiagaraEmitterHandle* H = FindHandle(*Sys, EmitterName, Names);
	if (!H) { return ErrWithList(FString::Printf(TEXT("No emitter '%s'"), *EmitterName), TEXT("emitters"), Names); }

	const FVersionedNiagaraEmitterData* Data = H->GetEmitterData();
	if (!Data) { return Err(TEXT("Emitter has no data for its active version")); }
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (!Renderers.IsValidIndex(RendererIndex))
	{
		return Err(FString::Printf(TEXT("Renderer index %d out of range (%d renderers)"), RendererIndex, Renderers.Num()));
	}
	UNiagaraRendererProperties* R = Renderers[RendererIndex];
	if (!R) { return Err(TEXT("Null renderer")); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("class"), R->GetClass()->GetName());
	FJObj Props = NewObj();
	for (TFieldIterator<FProperty> It(R->GetClass()); It; ++It)
	{
		FString Text;
		It->ExportText_InContainer(0, Text, R, R, nullptr, PPF_None);
		Props->SetStringField(It->GetName(), Text);
	}
	O->SetObjectField(TEXT("properties"), Props);
	return ToJson(O);
}

/* ============================== configure ============================== */

FString UFableNiagara::SetUserParam(const FString& SystemPath, const FString& ParamName, const FString& Value)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	FNiagaraUserRedirectionParameterStore& Store = Sys->GetExposedParameters();
	TArray<FNiagaraVariable> Params;
	Store.GetParameters(Params);

	const FNiagaraVariable* Target = nullptr;
	TArray<FString> Names;
	for (const FNiagaraVariable& V : Params)
	{
		const FString N = V.GetName().ToString();
		Names.Add(N);
		// Accept "Colour" for "User.Colour" — the redirection store hides the prefix in the UI too.
		if (!Target && (N.Equals(ParamName, ESearchCase::IgnoreCase) ||
		                N.Equals(TEXT("User.") + ParamName, ESearchCase::IgnoreCase)))
		{
			Target = &V;
		}
	}
	if (!Target) { return ErrWithList(FString::Printf(TEXT("No exposed parameter '%s'"), *ParamName), TEXT("params"), Names); }

	const UScriptStruct* Struct = Target->GetType().GetScriptStruct();
	if (!Struct) { return Err(FString::Printf(TEXT("Parameter '%s' is type %s, which has no struct form (data interfaces/objects aren't settable here)"),
		*Target->GetName().ToString(), *Target->GetType().GetName())); }

	// Niagara wraps scalars in single-field structs (FNiagaraFloat{Value}), so "2.5" would fail
	// ImportText. Wrap a bare scalar into that struct's one property automatically — the caller
	// shouldn't have to know Niagara's internal boxing.
	FString Text = Value;
	Text.TrimStartAndEndInline();
	if (!Text.StartsWith(TEXT("(")))
	{
		int32 NumFields = 0;
		FString SoleField;
		for (TFieldIterator<FProperty> It(Struct); It; ++It) { ++NumFields; SoleField = It->GetName(); }
		if (NumFields == 1) { Text = FString::Printf(TEXT("(%s=%s)"), *SoleField, *Value); }
	}

	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(Struct->GetStructureSize());
	Struct->InitializeStruct(Buffer.GetData());
	// ImportText returns null on a parse failure — check that rather than plumbing an output device,
	// so a typo'd value is reported back instead of silently writing a zeroed parameter.
	if (Struct->ImportText(*Text, Buffer.GetData(), nullptr, PPF_None, nullptr, Struct->GetName()) == nullptr)
	{
		Struct->DestroyStruct(Buffer.GetData());
		return Err(FString::Printf(TEXT("Could not parse '%s' as %s (expected e.g. \"(R=1,G=0,B=0,A=1)\" for a colour, or a bare number for a scalar)"),
			*Value, *Struct->GetName()));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Niagara User Parameter")));
	PrepareForEdit(*Sys);
	Sys->Modify();
	Store.SetParameterData(Buffer.GetData(), *Target, /*bAdd*/ false);
	Struct->DestroyStruct(Buffer.GetData());
	Sys->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("param"), Target->GetName().ToString());
	O->SetStringField(TEXT("applied"), Text);
	return ToJson(O);
}

FString UFableNiagara::SetRendererProps(const FString& SystemPath, const FString& EmitterName, int32 RendererIndex, const FString& PropsJson)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	TArray<FString> Names;
	FNiagaraEmitterHandle* H = FindHandle(*Sys, EmitterName, Names);
	if (!H) { return ErrWithList(FString::Printf(TEXT("No emitter '%s'"), *EmitterName), TEXT("emitters"), Names); }
	FVersionedNiagaraEmitterData* Data = H->GetEmitterData();
	if (!Data) { return Err(TEXT("Emitter has no data for its active version")); }
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (!Renderers.IsValidIndex(RendererIndex))
	{
		return Err(FString::Printf(TEXT("Renderer index %d out of range (%d renderers)"), RendererIndex, Renderers.Num()));
	}
	UNiagaraRendererProperties* R = Renderers[RendererIndex];
	if (!R) { return Err(TEXT("Null renderer")); }

	TSharedPtr<FJsonObject> Props;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PropsJson), Props) || !Props.IsValid())
	{
		return Err(TEXT("PropsJson is not a JSON object"));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Niagara Renderer Properties")));
	PrepareForEdit(*Sys);
	R->Modify();

	TArray<FString> Set, Failed;
	for (const auto& Pair : Props->Values)
	{
		FString ValueText;
		if (Pair.Value->Type == EJson::String) { ValueText = Pair.Value->AsString(); }
		else if (Pair.Value->Type == EJson::Boolean) { ValueText = Pair.Value->AsBool() ? TEXT("True") : TEXT("False"); }
		else if (Pair.Value->Type == EJson::Number) { ValueText = LexToString(Pair.Value->AsNumber()); }
		else { Failed.Add(Pair.Key + TEXT(" (unsupported JSON type)")); continue; }

		FString PropErr;
		if (SetPropFromText(R, Pair.Key, ValueText, PropErr)) { Set.Add(Pair.Key); }
		else { Failed.Add(PropErr); }
	}

	// Renderers cache derived state (bindings, material lists) — let it rebuild.
	R->PostEditChange();
	Sys->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), Failed.Num() == 0);
	TArray<FJVal> SetVals, FailVals;
	for (const FString& S : Set) { SetVals.Add(MakeShared<FJsonValueString>(S)); }
	for (const FString& S : Failed) { FailVals.Add(MakeShared<FJsonValueString>(S)); }
	O->SetArrayField(TEXT("set"), SetVals);
	O->SetArrayField(TEXT("failed"), FailVals);
	return ToJson(O);
}

FString UFableNiagara::SetEmitterEnabled(const FString& SystemPath, const FString& EmitterName, bool bEnabled)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	TArray<FString> Names;
	FNiagaraEmitterHandle* H = FindHandle(*Sys, EmitterName, Names);
	if (!H) { return ErrWithList(FString::Printf(TEXT("No emitter '%s'"), *EmitterName), TEXT("emitters"), Names); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Niagara Emitter Enabled")));
	PrepareForEdit(*Sys);
	Sys->Modify();
	const bool bChanged = H->SetIsEnabled(bEnabled, *Sys, /*bRecompileIfChanged*/ true);
	Sys->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetBoolField(TEXT("changed"), bChanged);
	O->SetBoolField(TEXT("enabled"), H->GetIsEnabled());
	return ToJson(O);
}

/* =============================== compose =============================== */

FString UFableNiagara::CreateSystem(const FString& PackagePath, const FString& AssetName)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }

	UNiagaraSystemFactoryNew* Factory = NewObject<UNiagaraSystemFactoryNew>();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* Asset = AssetTools.CreateAsset(AssetName, PackagePath, UNiagaraSystem::StaticClass(), Factory);
	if (!Asset) { return Err(TEXT("CreateAsset failed (does the asset already exist?)")); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("path"), Asset->GetPathName());
	return ToJson(O);
}

FString UFableNiagara::AddEmitter(const FString& SystemPath, const FString& SourcePath, const FString& SourceEmitterName, const FString& NewEmitterName)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	// Donor may be a standalone emitter asset or another system we lift one emitter out of.
	UNiagaraEmitter* Source = LoadObject<UNiagaraEmitter>(nullptr, *Norm(SourcePath));
	FGuid SourceVersion;
	if (Source)
	{
		SourceVersion = Source->GetExposedVersion().VersionGuid;
	}
	else
	{
		UNiagaraSystem* SourceSys = LoadObject<UNiagaraSystem>(nullptr, *Norm(SourcePath));
		if (!SourceSys) { return Err(FString::Printf(TEXT("Source is neither a NiagaraEmitter nor a NiagaraSystem: %s"), *Norm(SourcePath))); }

		TArray<FString> Names;
		FNiagaraEmitterHandle* SrcHandle = FindHandle(*SourceSys, SourceEmitterName, Names);
		if (!SrcHandle)
		{
			return ErrWithList(FString::Printf(TEXT("Source system has no emitter '%s' (pass SourceEmitterName)"), *SourceEmitterName),
				TEXT("source_emitters"), Names);
		}
		const FVersionedNiagaraEmitter Versioned = SrcHandle->GetInstance();
		Source = Versioned.Emitter;
		SourceVersion = Versioned.Version;
		if (!Source) { return Err(TEXT("Source emitter handle has no emitter instance")); }
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Niagara Emitter")));
	PrepareForEdit(*Sys);
	Sys->Modify();

	// bCreateCopy=true: the new emitter is owned by this system, so editing it never touches the donor.
	const FGuid NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*Sys, *Source, SourceVersion, /*bCreateCopy*/ true);
	if (!NewHandleId.IsValid()) { return Err(TEXT("AddEmitterToSystem failed")); }

	FString FinalName;
	for (FNiagaraEmitterHandle& H : Sys->GetEmitterHandles())
	{
		if (H.GetId() == NewHandleId)
		{
			/* Deliria local patch (2026-08-02): sever inheritance to the donor. AddEmitterToSystem's
			 * copy keeps Parent = the source emitter, and when the source lives INSIDE another
			 * system's package that is a cross-package private-object reference — the composed system
			 * can then NEVER SAVE ("Illegal reference to private object ...:Spots_5"). The contract
			 * here is copy-and-own (the comment above already promises "owned by this system"), so
			 * drop the parent link on the fresh copy. Never triggered by the original smoke test
			 * because that test discarded its scratch system without saving. */
			if (FVersionedNiagaraEmitterData* NewData = H.GetEmitterData())
			{
				NewData->RemoveParent();
			}
			if (!NewEmitterName.IsEmpty()) { H.SetName(FName(*NewEmitterName), *Sys); }
			FinalName = H.GetName().ToString();
		}
	}
	Sys->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("emitter"), FinalName);
	O->SetNumberField(TEXT("emitter_count"), Sys->GetEmitterHandles().Num());
	return ToJson(O);
}

FString UFableNiagara::RemoveEmitter(const FString& SystemPath, const FString& EmitterName)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	TArray<FString> Names;
	FNiagaraEmitterHandle* H = FindHandle(*Sys, EmitterName, Names);
	if (!H) { return ErrWithList(FString::Printf(TEXT("No emitter '%s'"), *EmitterName), TEXT("emitters"), Names); }
	const FGuid Id = H->GetId();

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Remove Niagara Emitter")));
	PrepareForEdit(*Sys);
	Sys->Modify();
	TSet<FGuid> ToRemove;
	ToRemove.Add(Id);
	Sys->RemoveEmitterHandlesById(ToRemove);
	Sys->MarkPackageDirty();

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetNumberField(TEXT("emitter_count"), Sys->GetEmitterHandles().Num());
	return ToJson(O);
}

FString UFableNiagara::DuplicateSystem(const FString& SystemPath, const FString& DestPackagePath, const FString& DestAssetName)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* Dup = AssetTools.DuplicateAsset(DestAssetName, DestPackagePath, Sys);
	if (!Dup) { return Err(TEXT("DuplicateAsset failed (name already taken?)")); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("path"), Dup->GetPathName());
	return ToJson(O);
}

/* ============================== lifecycle ============================== */

FString UFableNiagara::CompileSystem(const FString& SystemPath)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }

	Sys->RequestCompile(/*bForce*/ false);
	const bool bDone = Sys->PollForCompilationComplete(/*bFlushRequestCompile*/ true);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetBoolField(TEXT("compiled"), bDone);
	// Expect false here on the call that edited the system — the queueing tick hasn't run yet (see the
	// header). Poll IsReady in a LATER call; that is not a failure state.
	O->SetBoolField(TEXT("ready"), Sys->IsReadyToRun());
	O->SetBoolField(TEXT("outstanding"), Sys->HasOutstandingCompilationRequests());
	return ToJson(O);
}

FString UFableNiagara::IsReady(const FString& SystemPath)
{
	FString E;
	UNiagaraSystem* Sys = LoadSystem(SystemPath, E);
	if (!Sys) { return Err(E); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetBoolField(TEXT("ready"), Sys->IsReadyToRun());
	O->SetBoolField(TEXT("outstanding"), Sys->HasOutstandingCompilationRequests());
	O->SetNumberField(TEXT("emitters"), Sys->GetEmitterHandles().Num());
	return ToJson(O);
}
