// Copyright © 2026 Linesway All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Editor.h"

/**
 * The JSON reply vocabulary every Fable* bridge file answers the Python side with.
 *
 * WHY THIS HEADER EXISTS: FableBP.cpp and FableNiagara.cpp each used to define their own private
 * copies of these five helpers (byte-identical apart from one parameter name) inside their own
 * `FableKitPrivate` / `FableNiagaraPrivate` namespaces, and each ended with a file-scope
 * `using namespace ...;`. That compiles alone but NOT under a UE unity build, which concatenates
 * several .cpp files into one translation unit — both using-directives then apply to both files and
 * every call to Err/ToJson/NewObj becomes `error C2668: ambiguous call`.
 *
 * One definition in one namespace fixes it in both build modes: the name is now reachable through
 * two using-directives but resolves to the SAME entity, which is not ambiguous.
 *
 * So: any new Fable* bridge file should include this and must NOT redeclare these names. Anything
 * genuinely specific to one bridge still belongs in that file's own private namespace.
 */
namespace FableKitPrivate
{
	typedef TSharedPtr<FJsonObject> FJObj;
	typedef TSharedPtr<FJsonValue> FJVal;

	inline FJObj NewObj() { return MakeShared<FJsonObject>(); }

	inline FString ToJson(const FJObj& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	inline FString Err(const FString& Msg)
	{
		FJObj O = NewObj();
		O->SetBoolField(TEXT("ok"), false);
		O->SetStringField(TEXT("error"), Msg);
		return ToJson(O);
	}

	/** An error that also hands back the valid choices — what the caller actually needed to know. */
	inline FString ErrWithList(const FString& Msg, const FString& ListKey, const TArray<FString>& List)
	{
		FJObj O = NewObj();
		O->SetBoolField(TEXT("ok"), false);
		O->SetStringField(TEXT("error"), Msg);
		TArray<FJVal> Vals;
		for (const FString& S : List) { Vals.Add(MakeShared<FJsonValueString>(S)); }
		O->SetArrayField(ListKey, Vals);
		return ToJson(O);
	}

	/** Editing assets underneath a live PIE session is how the editor gets corrupted. Refuse instead. */
	inline bool CheckMutate(FString& OutErr)
	{
		if (GEditor && GEditor->PlayWorld)
		{
			OutErr = TEXT("Refused: a PIE session is active. Stop play-in-editor first.");
			return false;
		}
		return true;
	}
}
