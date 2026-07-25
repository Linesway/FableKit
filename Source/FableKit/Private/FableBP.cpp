#include "FableBP.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/MemberReference.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"

#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_StructOperation.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"

#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Modules/ModuleManager.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "Components/PanelSlot.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "ScopedTransaction.h"
#include "Editor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/TopLevelAssetPath.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"

#define FABLEKIT_VERSION TEXT("1.0")

namespace FableKitPrivate
{

// ------------------------------------------------------------------ JSON

typedef TSharedPtr<FJsonObject> FJObj;
typedef TSharedPtr<FJsonValue> FJVal;

static FJObj NewObj() { return MakeShared<FJsonObject>(); }

static FString ToJson(const FJObj& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Out;
}

static FString Err(const FString& Msg)
{
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), false);
	O->SetStringField(TEXT("error"), Msg);
	return ToJson(O);
}

static FString ErrWithList(const FString& Msg, const FString& ListKey, const TArray<FString>& List)
{
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), false);
	O->SetStringField(TEXT("error"), Msg);
	TArray<FJVal> Vals;
	for (const FString& S : List) { Vals.Add(MakeShared<FJsonValueString>(S)); }
	O->SetArrayField(ListKey, Vals);
	return ToJson(O);
}

static void SetStrArray(const FJObj& Obj, const FString& Key, const TArray<FString>& List)
{
	TArray<FJVal> Vals;
	for (const FString& S : List) { Vals.Add(MakeShared<FJsonValueString>(S)); }
	Obj->SetArrayField(Key, Vals);
}

// ------------------------------------------------------------------ loading / lookup

static FString NormalizeAssetPath(const FString& InPath)
{
	FString P = InPath;
	P.TrimStartAndEndInline();
	if (!P.Contains(TEXT(".")))
	{
		P = P + TEXT(".") + FPackageName::GetShortName(P);
	}
	return P;
}

static UBlueprint* LoadBP(const FString& Path, FString& OutErr)
{
	if (Path.IsEmpty()) { OutErr = TEXT("BlueprintPath is empty"); return nullptr; }
	const FString Norm = NormalizeAssetPath(Path);
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Norm);
	if (!BP)
	{
		OutErr = FString::Printf(TEXT("Blueprint not found: %s"), *Norm);
	}
	return BP;
}

static bool CheckMutate(FString& OutErr)
{
	if (GEditor && GEditor->PlayWorld)
	{
		OutErr = TEXT("Refused: a PIE session is active. Stop play-in-editor first.");
		return false;
	}
	return true;
}

struct FGraphInfo
{
	UEdGraph* Graph = nullptr;
	FString Kind;
};

static void CollectGraphs(UBlueprint* BP, TArray<FGraphInfo>& Out)
{
	for (UEdGraph* G : BP->UbergraphPages)          { if (G) Out.Add({G, TEXT("ubergraph")}); }
	for (UEdGraph* G : BP->FunctionGraphs)          { if (G) Out.Add({G, TEXT("function")}); }
	for (UEdGraph* G : BP->MacroGraphs)             { if (G) Out.Add({G, TEXT("macro")}); }
	for (UEdGraph* G : BP->DelegateSignatureGraphs) { if (G) Out.Add({G, TEXT("delegate")}); }
	for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
	{
		for (UEdGraph* G : Desc.Graphs) { if (G) Out.Add({G, TEXT("interface")}); }
	}
}

static UEdGraph* FindGraph(UBlueprint* BP, const FString& GraphName, FString& OutErr)
{
	TArray<FGraphInfo> Graphs;
	CollectGraphs(BP, Graphs);

	if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
	{
		for (const FGraphInfo& GI : Graphs)
		{
			if (GI.Kind == TEXT("ubergraph")) { return GI.Graph; }
		}
	}
	for (const FGraphInfo& GI : Graphs)
	{
		if (GI.Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) { return GI.Graph; }
	}

	TArray<FString> Names;
	for (const FGraphInfo& GI : Graphs) { Names.Add(GI.Graph->GetName() + TEXT(" (") + GI.Kind + TEXT(")")); }
	OutErr = ErrWithList(FString::Printf(TEXT("Graph '%s' not found"), *GraphName), TEXT("graphs"), Names);
	return nullptr;
}

static UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeId, FString& OutErr)
{
	FGuid Guid;
	if (FGuid::Parse(NodeId, Guid))
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Guid) { return N; }
		}
	}
	TArray<FString> Names;
	int32 Count = 0;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		if (++Count > 60) { Names.Add(TEXT("...")); break; }
		Names.Add(N->NodeGuid.ToString() + TEXT(" = ") + N->GetNodeTitle(ENodeTitleType::ListView).ToString());
	}
	OutErr = ErrWithList(FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *NodeId, *Graph->GetName()), TEXT("nodes"), Names);
	return nullptr;
}

static FString NormalizePinToken(const FString& In)
{
	FString S = In;
	S.ReplaceInline(TEXT(" "), TEXT(""));
	return S.ToLower();
}

static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& InPinName, FString& OutErr)
{
	FString Name = InPinName;
	int32 WantDir = -1; // -1 any, 0 input, 1 output
	if (Name.StartsWith(TEXT("in:"), ESearchCase::IgnoreCase))  { WantDir = 0; Name = Name.Mid(3); }
	if (Name.StartsWith(TEXT("out:"), ESearchCase::IgnoreCase)) { WantDir = 1; Name = Name.Mid(4); }
	const FString Norm = NormalizePinToken(Name);

	TArray<UEdGraphPin*> Matches;
	for (UEdGraphPin* P : Node->Pins)
	{
		if (!P) continue;
		if (WantDir == 0 && P->Direction != EGPD_Input) continue;
		if (WantDir == 1 && P->Direction != EGPD_Output) continue;
		if (NormalizePinToken(P->PinName.ToString()) == Norm) { Matches.Add(P); }
	}
	if (Matches.Num() == 1) { return Matches[0]; }

	TArray<FString> Pins;
	for (UEdGraphPin* P : Node->Pins)
	{
		if (!P) continue;
		Pins.Add(FString::Printf(TEXT("%s:%s"), P->Direction == EGPD_Input ? TEXT("in") : TEXT("out"), *P->PinName.ToString()));
	}
	const FString Reason = Matches.Num() > 1
		? FString::Printf(TEXT("Pin '%s' is ambiguous on node '%s' — prefix with in:/out:"), *InPinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString())
		: FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *InPinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	OutErr = ErrWithList(Reason, TEXT("pins"), Pins);
	return nullptr;
}

// ------------------------------------------------------------------ class / struct / enum resolution

static UClass* ResolveClass(const FString& InName, FString& OutErr)
{
	FString S = InName;
	S.TrimStartAndEndInline();
	if (S.IsEmpty()) { OutErr = TEXT("Empty class name"); return nullptr; }

	if (S.StartsWith(TEXT("/")))
	{
		if (UClass* C = LoadObject<UClass>(nullptr, *S)) { return C; }
		// Maybe a BP asset path: /Game/X/BP_Y[.BP_Y] -> try the generated class
		FString WithC = S;
		if (!WithC.Contains(TEXT(".")))
		{
			WithC = WithC + TEXT(".") + FPackageName::GetShortName(WithC) + TEXT("_C");
		}
		else if (!WithC.EndsWith(TEXT("_C")))
		{
			WithC += TEXT("_C");
		}
		if (UClass* C = LoadObject<UClass>(nullptr, *WithC)) { return C; }
		if (UBlueprint* B = LoadObject<UBlueprint>(nullptr, *NormalizeAssetPath(S)))
		{
			if (B->GeneratedClass) { return B->GeneratedClass; }
		}
		OutErr = FString::Printf(TEXT("Class not found: %s"), *S);
		return nullptr;
	}

	if (UClass* C = FindFirstObject<UClass>(*S, EFindFirstObjectOptions::NativeFirst))
	{
		return C;
	}
	OutErr = FString::Printf(TEXT("Class not found: %s (try a full path like /Script/Engine.Actor or /Game/Path/BP_X.BP_X_C)"), *S);
	return nullptr;
}

static UScriptStruct* ResolveStruct(const FString& InName, FString& OutErr)
{
	FString S = InName;
	S.TrimStartAndEndInline();
	if (S.StartsWith(TEXT("/")))
	{
		if (UScriptStruct* St = LoadObject<UScriptStruct>(nullptr, *S)) { return St; }
	}
	else if (UScriptStruct* St = FindFirstObject<UScriptStruct>(*S, EFindFirstObjectOptions::NativeFirst))
	{
		return St;
	}
	OutErr = FString::Printf(TEXT("Struct not found: %s"), *S);
	return nullptr;
}

static UEnum* ResolveEnum(const FString& InName, FString& OutErr)
{
	FString S = InName;
	S.TrimStartAndEndInline();
	if (S.StartsWith(TEXT("/")))
	{
		if (UEnum* E = LoadObject<UEnum>(nullptr, *S)) { return E; }
	}
	else if (UEnum* E = FindFirstObject<UEnum>(*S, EFindFirstObjectOptions::NativeFirst))
	{
		return E;
	}
	OutErr = FString::Printf(TEXT("Enum not found: %s"), *S);
	return nullptr;
}

// ------------------------------------------------------------------ type strings

static bool ParseTypeTerminal(const FString& InType, FName& OutCat, FName& OutSubCat, UObject*& OutSubObj, FString& OutErr)
{
	OutCat = NAME_None; OutSubCat = NAME_None; OutSubObj = nullptr;
	FString S = InType;
	S.TrimStartAndEndInline();
	const FString L = S.ToLower();

	if (L == TEXT("bool") || L == TEXT("boolean")) { OutCat = UEdGraphSchema_K2::PC_Boolean; return true; }
	if (L == TEXT("byte"))                          { OutCat = UEdGraphSchema_K2::PC_Byte; return true; }
	if (L == TEXT("int") || L == TEXT("integer"))   { OutCat = UEdGraphSchema_K2::PC_Int; return true; }
	if (L == TEXT("int64"))                         { OutCat = UEdGraphSchema_K2::PC_Int64; return true; }
	if (L == TEXT("float") || L == TEXT("double") || L == TEXT("real"))
	{
		OutCat = UEdGraphSchema_K2::PC_Real; OutSubCat = UEdGraphSchema_K2::PC_Double; return true;
	}
	if (L == TEXT("string"))   { OutCat = UEdGraphSchema_K2::PC_String; return true; }
	if (L == TEXT("name"))     { OutCat = UEdGraphSchema_K2::PC_Name; return true; }
	if (L == TEXT("text"))     { OutCat = UEdGraphSchema_K2::PC_Text; return true; }
	if (L == TEXT("wildcard")) { OutCat = UEdGraphSchema_K2::PC_Wildcard; return true; }

	if (L == TEXT("vector"))      { OutCat = UEdGraphSchema_K2::PC_Struct; OutSubObj = TBaseStructure<FVector>::Get(); return true; }
	if (L == TEXT("vector2d"))    { OutCat = UEdGraphSchema_K2::PC_Struct; OutSubObj = TBaseStructure<FVector2D>::Get(); return true; }
	if (L == TEXT("rotator"))     { OutCat = UEdGraphSchema_K2::PC_Struct; OutSubObj = TBaseStructure<FRotator>::Get(); return true; }
	if (L == TEXT("transform"))   { OutCat = UEdGraphSchema_K2::PC_Struct; OutSubObj = TBaseStructure<FTransform>::Get(); return true; }
	if (L == TEXT("linearcolor")) { OutCat = UEdGraphSchema_K2::PC_Struct; OutSubObj = TBaseStructure<FLinearColor>::Get(); return true; }
	if (L == TEXT("color"))       { OutCat = UEdGraphSchema_K2::PC_Struct; OutSubObj = TBaseStructure<FColor>::Get(); return true; }

	auto After = [&S](int32 N) { return S.Mid(N); };
	if (L.StartsWith(TEXT("object:")))     { OutCat = UEdGraphSchema_K2::PC_Object;     OutSubObj = ResolveClass(After(7), OutErr);  return OutSubObj != nullptr; }
	if (L.StartsWith(TEXT("softobject:"))) { OutCat = UEdGraphSchema_K2::PC_SoftObject; OutSubObj = ResolveClass(After(11), OutErr); return OutSubObj != nullptr; }
	if (L.StartsWith(TEXT("class:")))      { OutCat = UEdGraphSchema_K2::PC_Class;      OutSubObj = ResolveClass(After(6), OutErr);  return OutSubObj != nullptr; }
	if (L.StartsWith(TEXT("softclass:")))  { OutCat = UEdGraphSchema_K2::PC_SoftClass;  OutSubObj = ResolveClass(After(10), OutErr); return OutSubObj != nullptr; }
	if (L.StartsWith(TEXT("interface:")))  { OutCat = UEdGraphSchema_K2::PC_Interface;  OutSubObj = ResolveClass(After(10), OutErr); return OutSubObj != nullptr; }
	if (L.StartsWith(TEXT("struct:")))     { OutCat = UEdGraphSchema_K2::PC_Struct;     OutSubObj = ResolveStruct(After(7), OutErr); return OutSubObj != nullptr; }
	if (L.StartsWith(TEXT("enum:")))       { OutCat = UEdGraphSchema_K2::PC_Byte;       OutSubObj = ResolveEnum(After(5), OutErr);   return OutSubObj != nullptr; }

	OutErr = FString::Printf(TEXT("Unknown type '%s'"), *S);
	return false;
}

static bool ParseType(const FString& InType, FEdGraphPinType& Out, FString& OutErr)
{
	Out = FEdGraphPinType();
	FString S = InType;
	S.TrimStartAndEndInline();

	if (S.EndsWith(TEXT("&"))) { Out.bIsReference = true; S.LeftChopInline(1); S.TrimStartAndEndInline(); }

	const FString L = S.ToLower();
	if (L.StartsWith(TEXT("array:")))
	{
		Out.ContainerType = EPinContainerType::Array;
		S = S.Mid(6);
	}
	else if (L.StartsWith(TEXT("set:")))
	{
		Out.ContainerType = EPinContainerType::Set;
		S = S.Mid(4);
	}
	else if (L.StartsWith(TEXT("map:")))
	{
		Out.ContainerType = EPinContainerType::Map;
		S = S.Mid(4);
		FString KeyStr, ValStr;
		if (!S.Split(TEXT("|"), &KeyStr, &ValStr))
		{
			OutErr = TEXT("map type must be map:<key>|<value>");
			return false;
		}
		FName VCat, VSub; UObject* VObj = nullptr;
		if (!ParseTypeTerminal(ValStr, VCat, VSub, VObj, OutErr)) { return false; }
		Out.PinValueType.TerminalCategory = VCat;
		Out.PinValueType.TerminalSubCategory = VSub;
		Out.PinValueType.TerminalSubCategoryObject = VObj;
		S = KeyStr;
	}

	FName Cat, Sub; UObject* Obj = nullptr;
	if (!ParseTypeTerminal(S, Cat, Sub, Obj, OutErr)) { return false; }
	Out.PinCategory = Cat;
	Out.PinSubCategory = Sub;
	Out.PinSubCategoryObject = Obj;
	return true;
}

static FString TerminalTypeToStr(const FName Cat, const FName SubCat, const UObject* SubObj)
{
	if (Cat == UEdGraphSchema_K2::PC_Exec)    { return TEXT("exec"); }
	if (Cat == UEdGraphSchema_K2::PC_Boolean) { return TEXT("bool"); }
	if (Cat == UEdGraphSchema_K2::PC_Byte)    { return SubObj ? FString::Printf(TEXT("enum:%s"), *SubObj->GetPathName()) : TEXT("byte"); }
	if (Cat == UEdGraphSchema_K2::PC_Int)     { return TEXT("int"); }
	if (Cat == UEdGraphSchema_K2::PC_Int64)   { return TEXT("int64"); }
	if (Cat == UEdGraphSchema_K2::PC_Real)    { return TEXT("float"); }
	if (Cat == UEdGraphSchema_K2::PC_String)  { return TEXT("string"); }
	if (Cat == UEdGraphSchema_K2::PC_Name)    { return TEXT("name"); }
	if (Cat == UEdGraphSchema_K2::PC_Text)    { return TEXT("text"); }
	if (Cat == UEdGraphSchema_K2::PC_Wildcard){ return TEXT("wildcard"); }
	if (Cat == UEdGraphSchema_K2::PC_Struct)
	{
		if (SubObj == TBaseStructure<FVector>::Get())      { return TEXT("vector"); }
		if (SubObj == TBaseStructure<FVector2D>::Get())    { return TEXT("vector2d"); }
		if (SubObj == TBaseStructure<FRotator>::Get())     { return TEXT("rotator"); }
		if (SubObj == TBaseStructure<FTransform>::Get())   { return TEXT("transform"); }
		if (SubObj == TBaseStructure<FLinearColor>::Get()) { return TEXT("linearcolor"); }
		return FString::Printf(TEXT("struct:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?"));
	}
	if (Cat == UEdGraphSchema_K2::PC_Object)
	{
		if (SubCat == UEdGraphSchema_K2::PSC_Self) { return TEXT("object:self"); }
		return FString::Printf(TEXT("object:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?"));
	}
	if (Cat == UEdGraphSchema_K2::PC_SoftObject) { return FString::Printf(TEXT("softobject:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?")); }
	if (Cat == UEdGraphSchema_K2::PC_Class)      { return FString::Printf(TEXT("class:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?")); }
	if (Cat == UEdGraphSchema_K2::PC_SoftClass)  { return FString::Printf(TEXT("softclass:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?")); }
	if (Cat == UEdGraphSchema_K2::PC_Interface)  { return FString::Printf(TEXT("interface:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?")); }
	if (Cat == UEdGraphSchema_K2::PC_Delegate)   { return TEXT("delegate"); }
	if (Cat == UEdGraphSchema_K2::PC_MCDelegate) { return TEXT("mcdelegate"); }
	return Cat.ToString();
}

static FString TypeToStr(const FEdGraphPinType& T)
{
	FString Inner = TerminalTypeToStr(T.PinCategory, T.PinSubCategory, T.PinSubCategoryObject.Get());
	FString Out;
	switch (T.ContainerType)
	{
	case EPinContainerType::Array: Out = TEXT("array:") + Inner; break;
	case EPinContainerType::Set:   Out = TEXT("set:") + Inner; break;
	case EPinContainerType::Map:
		Out = TEXT("map:") + Inner + TEXT("|") + TerminalTypeToStr(T.PinValueType.TerminalCategory, T.PinValueType.TerminalSubCategory, T.PinValueType.TerminalSubCategoryObject.Get());
		break;
	default: Out = Inner; break;
	}
	if (T.bIsReference) { Out += TEXT("&"); }
	return Out;
}

// ------------------------------------------------------------------ node / pin JSON

static FJObj PinToJson(const UEdGraphPin* P)
{
	FJObj O = NewObj();
	O->SetStringField(TEXT("name"), P->PinName.ToString());
	O->SetStringField(TEXT("dir"), P->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
	O->SetStringField(TEXT("type"), TypeToStr(P->PinType));
	if (P->bHidden) { O->SetBoolField(TEXT("hidden"), true); }

	FString Default;
	if (P->DefaultObject) { Default = P->DefaultObject->GetPathName(); }
	else if (!P->DefaultTextValue.IsEmpty()) { Default = P->DefaultTextValue.ToString(); }
	else { Default = P->DefaultValue; }
	if (!Default.IsEmpty()) { O->SetStringField(TEXT("default"), Default); }

	if (P->LinkedTo.Num() > 0)
	{
		TArray<FJVal> Links;
		for (const UEdGraphPin* L : P->LinkedTo)
		{
			if (!L) continue;
			FJObj LO = NewObj();
			LO->SetStringField(TEXT("node"), L->GetOwningNode()->NodeGuid.ToString());
			LO->SetStringField(TEXT("pin"), L->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(LO));
		}
		O->SetArrayField(TEXT("links"), Links);
	}
	if (P->SubPins.Num() > 0) { O->SetBoolField(TEXT("split"), true); }
	return O;
}

static FString NodeDetail(const UEdGraphNode* Node)
{
	if (const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(Node))
	{
		const UClass* Parent = CF->FunctionReference.GetMemberParentClass();
		return FString::Printf(TEXT("%s:%s"), Parent ? *Parent->GetPathName() : TEXT("self"), *CF->FunctionReference.GetMemberName().ToString());
	}
	if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
	{
		return FString::Printf(TEXT("custom_event:%s"), *CE->CustomFunctionName.ToString());
	}
	if (const UK2Node_Event* Ev = Cast<UK2Node_Event>(Node))
	{
		const UClass* Parent = Ev->EventReference.GetMemberParentClass();
		return FString::Printf(TEXT("event:%s:%s"), Parent ? *Parent->GetPathName() : TEXT("self"), *Ev->EventReference.GetMemberName().ToString());
	}
	if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
	{
		return FString::Printf(TEXT("var:%s"), *Var->GetVarName().ToString());
	}
	if (const UK2Node_MacroInstance* Mac = Cast<UK2Node_MacroInstance>(Node))
	{
		const UEdGraph* MG = const_cast<UK2Node_MacroInstance*>(Mac)->GetMacroGraph();
		return FString::Printf(TEXT("macro:%s"), MG ? *MG->GetName() : TEXT("?"));
	}
	if (const UK2Node_DynamicCast* Cst = Cast<UK2Node_DynamicCast>(Node))
	{
		return FString::Printf(TEXT("cast:%s"), Cst->TargetType ? *Cst->TargetType->GetPathName() : TEXT("?"));
	}
	if (const UK2Node_StructOperation* St = Cast<UK2Node_StructOperation>(Node))
	{
		return FString::Printf(TEXT("struct:%s"), St->StructType ? *St->StructType->GetPathName() : TEXT("?"));
	}
	return FString();
}

static FJObj NodeToJson(UEdGraphNode* Node, bool bIncludePins)
{
	FJObj O = NewObj();
	O->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
	O->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	O->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	O->SetNumberField(TEXT("x"), Node->NodePosX);
	O->SetNumberField(TEXT("y"), Node->NodePosY);
	// Comment boxes: size + text, so callers can compute containment (EdGraphNode_Comment isn't
	// script-exposed, making this the only python-reachable source for either).
	if (Node->IsA<UEdGraphNode_Comment>())
	{
		O->SetNumberField(TEXT("width"), Node->NodeWidth);
		O->SetNumberField(TEXT("height"), Node->NodeHeight);
	}
	O->SetStringField(TEXT("path"), Node->GetPathName());
	if (!Node->IsNodeEnabled()) { O->SetBoolField(TEXT("disabled"), true); }
	if (!Node->NodeComment.IsEmpty()) { O->SetStringField(TEXT("comment"), Node->NodeComment); }
	const FString Detail = NodeDetail(Node);
	if (!Detail.IsEmpty()) { O->SetStringField(TEXT("detail"), Detail); }

	if (bIncludePins)
	{
		TArray<FJVal> Pins;
		for (const UEdGraphPin* P : Node->Pins)
		{
			if (P) { Pins.Add(MakeShared<FJsonValueObject>(PinToJson(P))); }
		}
		O->SetArrayField(TEXT("pins"), Pins);
	}
	return O;
}

static FString OkNode(UEdGraphNode* Node)
{
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetObjectField(TEXT("node"), NodeToJson(Node, true));
	return ToJson(O);
}

// ------------------------------------------------------------------ node spawning

static void FinishSpawn(UBlueprint* BP, UEdGraph* Graph, UEdGraphNode* Node, float X, float Y)
{
	Graph->Modify();
	Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
	Node->SetFlags(RF_Transactional);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	Node->NodePosX = static_cast<int32>(X);
	Node->NodePosY = static_cast<int32>(Y);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
}

// Shared preamble for graph-level mutators.
struct FGraphCtx
{
	UBlueprint* BP = nullptr;
	UEdGraph* Graph = nullptr;
	FString Error; // already-JSON error when invalid
	bool IsValid() const { return BP && Graph; }
};

static FGraphCtx GetGraphCtx(const FString& BlueprintPath, const FString& GraphName, bool bForMutation)
{
	FGraphCtx Ctx;
	FString E;
	if (bForMutation && !CheckMutate(E)) { Ctx.Error = Err(E); return Ctx; }
	Ctx.BP = LoadBP(BlueprintPath, E);
	if (!Ctx.BP) { Ctx.Error = Err(E); return Ctx; }
	FString GraphErrJson;
	Ctx.Graph = FindGraph(Ctx.BP, GraphName, GraphErrJson);
	if (!Ctx.Graph) { Ctx.BP = nullptr; Ctx.Error = GraphErrJson; }
	return Ctx;
}

// Parse "Name:type;Name2:type2" parameter lists.
struct FParamDecl { FString Name; FEdGraphPinType Type; };

static bool ParseParamList(const FString& In, TArray<FParamDecl>& Out, FString& OutErr)
{
	FString S = In;
	S.TrimStartAndEndInline();
	if (S.IsEmpty()) { return true; }
	TArray<FString> Parts;
	S.ParseIntoArray(Parts, TEXT(";"), true);
	for (const FString& Part : Parts)
	{
		int32 ColonIdx;
		if (!Part.FindChar(TEXT(':'), ColonIdx))
		{
			OutErr = FString::Printf(TEXT("Parameter '%s' must be Name:type"), *Part);
			return false;
		}
		FParamDecl Decl;
		Decl.Name = Part.Left(ColonIdx).TrimStartAndEnd();
		if (!ParseType(Part.Mid(ColonIdx + 1), Decl.Type, OutErr)) { return false; }
		Out.Add(Decl);
	}
	return true;
}

static int32 FindBPVarIndex(UBlueprint* BP, const FName VarName)
{
	for (int32 i = 0; i < BP->NewVariables.Num(); ++i)
	{
		if (BP->NewVariables[i].VarName == VarName) { return i; }
	}
	return INDEX_NONE;
}

static bool ApplyVarFlags(UBlueprint* BP, const FName VarName, const FString& Category, bool bInstanceEditable, bool bBlueprintReadOnly, bool bExposeOnSpawn, const FString& Replication, FString& OutErr)
{
	const int32 Idx = FindBPVarIndex(BP, VarName);
	if (Idx == INDEX_NONE)
	{
		OutErr = FString::Printf(TEXT("Variable '%s' is not a Blueprint-declared variable on this BP"), *VarName.ToString());
		return false;
	}
	FBPVariableDescription& V = BP->NewVariables[Idx];

	if (bInstanceEditable) { V.PropertyFlags &= ~CPF_DisableEditOnInstance; }
	else                   { V.PropertyFlags |= CPF_DisableEditOnInstance; }

	if (bBlueprintReadOnly) { V.PropertyFlags |= CPF_BlueprintReadOnly; }
	else                    { V.PropertyFlags &= ~CPF_BlueprintReadOnly; }

	if (bExposeOnSpawn)
	{
		V.PropertyFlags |= CPF_ExposeOnSpawn;
		V.SetMetaData(TEXT("ExposeOnSpawn"), TEXT("true"));
	}
	else
	{
		V.PropertyFlags &= ~CPF_ExposeOnSpawn;
		if (V.FindMetaDataEntryIndexForKey(TEXT("ExposeOnSpawn")) != INDEX_NONE)
		{
			V.RemoveMetaData(TEXT("ExposeOnSpawn"));
		}
	}

	const FString Rep = Replication.ToLower();
	if (Rep == TEXT("none"))
	{
		V.PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
		V.RepNotifyFunc = NAME_None;
	}
	else if (Rep == TEXT("replicated"))
	{
		V.PropertyFlags |= CPF_Net;
		V.PropertyFlags &= ~CPF_RepNotify;
		V.RepNotifyFunc = NAME_None;
	}
	else if (Rep == TEXT("repnotify"))
	{
		V.PropertyFlags |= CPF_Net | CPF_RepNotify;
		const FString FuncName = FString::Printf(TEXT("OnRep_%s"), *VarName.ToString());
		V.RepNotifyFunc = FName(*FuncName);
		// Mirrors FBlueprintVarActionDetails::OnChangeReplication — create the OnRep graph if missing.
		UEdGraph* FuncGraph = FindObject<UEdGraph>(BP, *FuncName);
		if (!FuncGraph)
		{
			FuncGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, false, static_cast<UClass*>(nullptr));
		}
	}
	else if (!Rep.IsEmpty())
	{
		OutErr = FString::Printf(TEXT("Unknown Replication '%s' (use none/replicated/repnotify or empty)"), *Replication);
		return false;
	}

	if (!Category.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, VarName, nullptr, FText::FromString(Category), /*bDontRecompile*/ true);
	}
	return true;
}

static FJObj VarToJson(const FBPVariableDescription& V)
{
	FJObj O = NewObj();
	O->SetStringField(TEXT("name"), V.VarName.ToString());
	O->SetStringField(TEXT("type"), TypeToStr(V.VarType));
	O->SetStringField(TEXT("category"), V.Category.ToString());
	if (!V.DefaultValue.IsEmpty()) { O->SetStringField(TEXT("default"), V.DefaultValue); }
	O->SetBoolField(TEXT("instance_editable"), !(V.PropertyFlags & CPF_DisableEditOnInstance));
	if (V.PropertyFlags & CPF_BlueprintReadOnly) { O->SetBoolField(TEXT("read_only"), true); }
	if (V.PropertyFlags & CPF_Net)
	{
		O->SetStringField(TEXT("replication"), (V.PropertyFlags & CPF_RepNotify) ? TEXT("repnotify") : TEXT("replicated"));
		if (V.RepNotifyFunc != NAME_None) { O->SetStringField(TEXT("rep_notify_func"), V.RepNotifyFunc.ToString()); }
	}
	return O;
}

static bool FuncHasOutputs(const UFunction* F)
{
	for (TFieldIterator<FProperty> It(F); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { return true; }
		if (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm)) { return true; }
	}
	return false;
}

static FJObj FuncToJson(const UFunction* F)
{
	FJObj O = NewObj();
	O->SetStringField(TEXT("name"), F->GetName());
	O->SetStringField(TEXT("class"), F->GetOwnerClass() ? F->GetOwnerClass()->GetPathName() : TEXT("?"));
	if (F->HasAnyFunctionFlags(FUNC_BlueprintPure)) { O->SetBoolField(TEXT("pure"), true); }
	if (F->HasAnyFunctionFlags(FUNC_Static))        { O->SetBoolField(TEXT("static"), true); }
	if (F->HasAnyFunctionFlags(FUNC_BlueprintEvent)){ O->SetBoolField(TEXT("event"), true); }

	const UEdGraphSchema_K2* K2 = GetDefault<UEdGraphSchema_K2>();
	TArray<FString> Params;
	for (TFieldIterator<FProperty> It(F); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FEdGraphPinType T;
		FString TypeStr = TEXT("?");
		if (K2->ConvertPropertyToPinType(*It, T)) { TypeStr = TypeToStr(T); }
		const bool bOut = It->HasAnyPropertyFlags(CPF_ReturnParm) || (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm));
		Params.Add(FString::Printf(TEXT("%s:%s:%s"), *It->GetName(), *TypeStr, bOut ? TEXT("out") : TEXT("in")));
	}
	SetStrArray(O, TEXT("params"), Params);
	return O;
}

} // namespace FableKitPrivate

using namespace FableKitPrivate;

// ================================================================== info / reading

FString UFableBP::Ping()
{
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("plugin"), TEXT("FableKit"));
	O->SetStringField(TEXT("version"), FABLEKIT_VERSION);
	O->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
	O->SetBoolField(TEXT("pie_active"), GEditor && GEditor->PlayWorld != nullptr);
	return ToJson(O);
}

FString UFableBP::ListGraphs(const FString& BlueprintPath)
{
	FString E;
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	TArray<FGraphInfo> Graphs;
	CollectGraphs(BP, Graphs);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	TArray<FJVal> Arr;
	for (const FGraphInfo& GI : Graphs)
	{
		FJObj G = NewObj();
		G->SetStringField(TEXT("name"), GI.Graph->GetName());
		G->SetStringField(TEXT("kind"), GI.Kind);
		G->SetNumberField(TEXT("nodes"), GI.Graph->Nodes.Num());
		Arr.Add(MakeShared<FJsonValueObject>(G));
	}
	O->SetArrayField(TEXT("graphs"), Arr);
	return ToJson(O);
}

FString UFableBP::DumpBlueprint(const FString& BlueprintPath)
{
	FString E;
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("name"), BP->GetName());
	O->SetStringField(TEXT("parent"), BP->ParentClass ? BP->ParentClass->GetPathName() : TEXT("None"));
	O->SetStringField(TEXT("generated_class"), BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : TEXT("None"));

	switch (BP->Status)
	{
	case BS_UpToDate:             O->SetStringField(TEXT("status"), TEXT("UpToDate")); break;
	case BS_UpToDateWithWarnings: O->SetStringField(TEXT("status"), TEXT("UpToDateWithWarnings")); break;
	case BS_Error:                O->SetStringField(TEXT("status"), TEXT("Error")); break;
	case BS_Dirty:                O->SetStringField(TEXT("status"), TEXT("Dirty")); break;
	default:                      O->SetStringField(TEXT("status"), TEXT("Unknown")); break;
	}

	TArray<FString> Ifaces;
	for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
	{
		if (Desc.Interface) { Ifaces.Add(Desc.Interface->GetPathName()); }
	}
	SetStrArray(O, TEXT("interfaces"), Ifaces);

	TArray<FJVal> Vars;
	TArray<FString> Dispatchers;
	for (const FBPVariableDescription& V : BP->NewVariables)
	{
		if (V.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
		{
			Dispatchers.Add(V.VarName.ToString());
			continue;
		}
		Vars.Add(MakeShared<FJsonValueObject>(VarToJson(V)));
	}
	O->SetArrayField(TEXT("variables"), Vars);
	SetStrArray(O, TEXT("dispatchers"), Dispatchers);

	TArray<FGraphInfo> Graphs;
	CollectGraphs(BP, Graphs);
	TArray<FJVal> GraphArr;
	for (const FGraphInfo& GI : Graphs)
	{
		FJObj G = NewObj();
		G->SetStringField(TEXT("name"), GI.Graph->GetName());
		G->SetStringField(TEXT("kind"), GI.Kind);
		G->SetNumberField(TEXT("nodes"), GI.Graph->Nodes.Num());
		GraphArr.Add(MakeShared<FJsonValueObject>(G));
	}
	O->SetArrayField(TEXT("graphs"), GraphArr);

	TArray<FJVal> Comps;
	if (BP->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& AllNodes = BP->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* N : AllNodes)
		{
			if (!N) continue;
			FJObj C = NewObj();
			C->SetStringField(TEXT("name"), N->GetVariableName().ToString());
			C->SetStringField(TEXT("class"), N->ComponentClass ? N->ComponentClass->GetPathName() : TEXT("?"));
			for (USCS_Node* Other : AllNodes)
			{
				if (Other && Other->GetChildNodes().Contains(N))
				{
					C->SetStringField(TEXT("parent"), Other->GetVariableName().ToString());
					break;
				}
			}
			Comps.Add(MakeShared<FJsonValueObject>(C));
		}
	}
	O->SetArrayField(TEXT("components"), Comps);
	return ToJson(O);
}

FString UFableBP::DumpGraph(const FString& BlueprintPath, const FString& GraphName, const FString& Filter)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, false);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("graph"), Ctx.Graph->GetName());
	TArray<FJVal> Nodes;
	for (UEdGraphNode* N : Ctx.Graph->Nodes)
	{
		if (!N) continue;
		if (!Filter.IsEmpty())
		{
			const FString Title = N->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (!Title.Contains(Filter) && !N->GetClass()->GetName().Contains(Filter))
			{
				continue;
			}
		}
		Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(N, true)));
	}
	O->SetArrayField(TEXT("nodes"), Nodes);
	O->SetNumberField(TEXT("total_nodes"), Ctx.Graph->Nodes.Num());
	return ToJson(O);
}

FString UFableBP::GetNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, false);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E);
	if (!N) { return E; }
	return OkNode(N);
}

FString UFableBP::ListFunctions(const FString& ClassPath, const FString& Contains)
{
	FString E;
	UClass* C = ResolveClass(ClassPath, E);
	if (!C) { return Err(E); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("class"), C->GetPathName());
	TArray<FJVal> Funcs;
	int32 Count = 0;
	for (TFieldIterator<UFunction> It(C, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		UFunction* F = *It;
		if (!F->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent)) { continue; }
		if (!Contains.IsEmpty() && !F->GetName().Contains(Contains)) { continue; }
		Funcs.Add(MakeShared<FJsonValueObject>(FuncToJson(F)));
		if (++Count >= 400) { O->SetBoolField(TEXT("truncated"), true); break; }
	}
	O->SetArrayField(TEXT("functions"), Funcs);
	return ToJson(O);
}

FString UFableBP::ListOverridableEvents(const FString& BlueprintPath)
{
	FString E;
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	TArray<FJVal> Events;

	auto Consider = [&](UFunction* F, const FString& Source)
	{
		if (!F->HasAnyFunctionFlags(FUNC_BlueprintEvent)) { return; }
		if (FBlueprintEditorUtils::FindOverrideForFunction(BP, F->GetOwnerClass(), F->GetFName())) { return; }
		// Skip if a function graph with this name already exists (function-style override).
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetFName() == F->GetFName()) { return; }
		}
		FJObj EO = FuncToJson(F);
		EO->SetStringField(TEXT("source"), Source);
		EO->SetStringField(TEXT("kind"), FuncHasOutputs(F) ? TEXT("function_override") : TEXT("event"));
		Events.Add(MakeShared<FJsonValueObject>(EO));
	};

	if (BP->ParentClass)
	{
		for (TFieldIterator<UFunction> It(BP->ParentClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			Consider(*It, TEXT("parent"));
		}
	}
	for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
	{
		if (!Desc.Interface) { continue; }
		for (TFieldIterator<UFunction> It(*Desc.Interface, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			bool bHasGraph = false;
			for (UEdGraph* G : Desc.Graphs)
			{
				if (G && G->GetFName() == It->GetFName()) { bHasGraph = true; break; }
			}
			if (!bHasGraph) { Consider(*It, TEXT("interface")); }
		}
	}
	O->SetArrayField(TEXT("events"), Events);
	return ToJson(O);
}

// ================================================================== node creation

FString UFableBP::AddCallFunction(const FString& BlueprintPath, const FString& GraphName, const FString& FunctionRef, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	FString ClassPart, FuncPart;
	int32 ColonIdx;
	if (FunctionRef.FindLastChar(TEXT(':'), ColonIdx))
	{
		ClassPart = FunctionRef.Left(ColonIdx);
		FuncPart = FunctionRef.Mid(ColonIdx + 1);
	}
	else
	{
		FuncPart = FunctionRef;
	}
	FuncPart.TrimStartAndEndInline();
	if (FuncPart.IsEmpty()) { return Err(TEXT("FunctionRef must be '<class>:<Func>' or ':<Func>'")); }

	UClass* SearchClass = nullptr;
	FString E;
	if (ClassPart.IsEmpty())
	{
		SearchClass = Ctx.BP->SkeletonGeneratedClass ? Ctx.BP->SkeletonGeneratedClass.Get() : Ctx.BP->GeneratedClass.Get();
		if (!SearchClass) { SearchClass = Ctx.BP->ParentClass; }
	}
	else
	{
		SearchClass = ResolveClass(ClassPart, E);
		if (!SearchClass) { return Err(E); }
	}

	UFunction* Func = SearchClass ? SearchClass->FindFunctionByName(FName(*FuncPart)) : nullptr;
	if (!Func)
	{
		return Err(FString::Printf(TEXT("Function '%s' not found on %s (use ListFunctions to browse)"), *FuncPart, SearchClass ? *SearchClass->GetPathName() : TEXT("?")));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add CallFunction")));
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Ctx.Graph);
	Node->SetFromFunction(Func);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddEvent(const FString& BlueprintPath, const FString& GraphName, const FString& EventName, const FString& ClassHint, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	UFunction* Func = nullptr;
	FString E;
	if (!ClassHint.IsEmpty())
	{
		UClass* C = ResolveClass(ClassHint, E);
		if (!C) { return Err(E); }
		Func = C->FindFunctionByName(FName(*EventName));
	}
	else
	{
		if (Ctx.BP->ParentClass) { Func = Ctx.BP->ParentClass->FindFunctionByName(FName(*EventName)); }
		if (!Func)
		{
			for (const FBPInterfaceDescription& Desc : Ctx.BP->ImplementedInterfaces)
			{
				if (Desc.Interface)
				{
					Func = Desc.Interface->FindFunctionByName(FName(*EventName));
					if (Func) { break; }
				}
			}
		}
	}
	if (!Func)
	{
		return Err(FString::Printf(TEXT("Event '%s' not found on the parent hierarchy or implemented interfaces (use ListOverridableEvents)"), *EventName));
	}
	if (!Func->HasAnyFunctionFlags(FUNC_BlueprintEvent))
	{
		return Err(FString::Printf(TEXT("'%s' is not a BlueprintImplementableEvent/BlueprintNativeEvent"), *EventName));
	}
	if (FuncHasOutputs(Func))
	{
		return Err(FString::Printf(TEXT("'%s' has outputs — implement it with AddFunctionOverride instead of an event node"), *EventName));
	}

	UClass* SignatureClass = Func->GetOwnerClass();
	if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Ctx.BP, SignatureClass, Func->GetFName()))
	{
		FJObj O = NewObj();
		O->SetBoolField(TEXT("ok"), true);
		O->SetBoolField(TEXT("existing"), true);
		O->SetObjectField(TEXT("node"), NodeToJson(Existing, true));
		return ToJson(O);
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Event")));
	UK2Node_Event* Node = NewObject<UK2Node_Event>(Ctx.Graph);
	Node->EventReference.SetExternalMember(Func->GetFName(), SignatureClass);
	Node->bOverrideFunction = true;
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddCustomEvent(const FString& BlueprintPath, const FString& GraphName, const FString& EventName, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	for (UEdGraphNode* N : Ctx.Graph->Nodes)
	{
		if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
		{
			if (CE->CustomFunctionName == FName(*EventName))
			{
				FJObj O = NewObj();
				O->SetBoolField(TEXT("ok"), true);
				O->SetBoolField(TEXT("existing"), true);
				O->SetObjectField(TEXT("node"), NodeToJson(CE, true));
				return ToJson(O);
			}
		}
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Custom Event")));
	UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Ctx.Graph);
	Node->CustomFunctionName = FName(*EventName);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);
	return OkNode(Node);
}

static FString AddVariableNodeImpl(const FString& BlueprintPath, const FString& GraphName, const FString& VarName, float X, float Y, bool bSetter)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	const FName VarFName(*VarName);
	UClass* Skel = Ctx.BP->SkeletonGeneratedClass ? Ctx.BP->SkeletonGeneratedClass.Get() : Ctx.BP->GeneratedClass.Get();
	const bool bExists =
		(Skel && FindFProperty<FProperty>(Skel, VarFName) != nullptr) ||
		(FindBPVarIndex(Ctx.BP, VarFName) != INDEX_NONE);
	if (!bExists)
	{
		TArray<FString> Names;
		for (const FBPVariableDescription& V : Ctx.BP->NewVariables) { Names.Add(V.VarName.ToString()); }
		if (Ctx.BP->SimpleConstructionScript)
		{
			for (USCS_Node* N : Ctx.BP->SimpleConstructionScript->GetAllNodes())
			{
				if (N) { Names.Add(N->GetVariableName().ToString() + TEXT(" (component)")); }
			}
		}
		return ErrWithList(FString::Printf(TEXT("Variable '%s' not found on this Blueprint"), *VarName), TEXT("variables"), Names);
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Variable Node")));
	UK2Node_Variable* Node;
	if (bSetter) { Node = NewObject<UK2Node_VariableSet>(Ctx.Graph); }
	else         { Node = NewObject<UK2Node_VariableGet>(Ctx.Graph); }
	Node->VariableReference.SetSelfMember(VarFName);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddVarGet(const FString& BlueprintPath, const FString& GraphName, const FString& VarName, float X, float Y)
{
	return AddVariableNodeImpl(BlueprintPath, GraphName, VarName, X, Y, false);
}

FString UFableBP::AddVarSet(const FString& BlueprintPath, const FString& GraphName, const FString& VarName, float X, float Y)
{
	return AddVariableNodeImpl(BlueprintPath, GraphName, VarName, X, Y, true);
}

FString UFableBP::AddBranch(const FString& BlueprintPath, const FString& GraphName, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Branch")));
	UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Ctx.Graph);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddSequence(const FString& BlueprintPath, const FString& GraphName, int32 NumOutputs, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Sequence")));
	UK2Node_ExecutionSequence* Node = NewObject<UK2Node_ExecutionSequence>(Ctx.Graph);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	if (IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(Node))
	{
		for (int32 i = 2; i < NumOutputs && AddPin->CanAddPin(); ++i)
		{
			AddPin->AddInputPin();
		}
	}
	return OkNode(Node);
}

FString UFableBP::AddMacro(const FString& BlueprintPath, const FString& GraphName, const FString& MacroName, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	FString LibPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
	FString GraphPart = MacroName;
	int32 ColonIdx;
	if (MacroName.FindLastChar(TEXT(':'), ColonIdx))
	{
		LibPath = NormalizeAssetPath(MacroName.Left(ColonIdx));
		GraphPart = MacroName.Mid(ColonIdx + 1);
	}

	UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, *LibPath);
	if (!MacroLib) { return Err(FString::Printf(TEXT("Macro library not found: %s"), *LibPath)); }

	UEdGraph* MacroGraph = nullptr;
	TArray<FString> Available;
	for (UEdGraph* G : MacroLib->MacroGraphs)
	{
		if (!G) continue;
		Available.Add(G->GetName());
		if (G->GetName().Equals(GraphPart, ESearchCase::IgnoreCase)) { MacroGraph = G; }
	}
	if (!MacroGraph)
	{
		return ErrWithList(FString::Printf(TEXT("Macro '%s' not found in %s"), *GraphPart, *LibPath), TEXT("macros"), Available);
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Macro")));
	UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Ctx.Graph);
	Node->SetMacroGraph(MacroGraph);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddCast(const FString& BlueprintPath, const FString& GraphName, const FString& TargetClass, bool bPure, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UClass* C = ResolveClass(TargetClass, E);
	if (!C) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Cast")));
	UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Ctx.Graph);
	Node->TargetType = C;
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	if (bPure) { Node->SetPurity(true); }
	return OkNode(Node);
}

FString UFableBP::AddSpawnActor(const FString& BlueprintPath, const FString& GraphName, const FString& ActorClass, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UClass* C = ResolveClass(ActorClass, E);
	if (!C) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add SpawnActor")));
	UK2Node_SpawnActorFromClass* Node = NewObject<UK2Node_SpawnActorFromClass>(Ctx.Graph);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	if (UEdGraphPin* ClassPin = Node->FindPin(TEXT("Class")))
	{
		Ctx.Graph->GetSchema()->TrySetDefaultObject(*ClassPin, C);
	}
	return OkNode(Node);
}

FString UFableBP::AddMakeStruct(const FString& BlueprintPath, const FString& GraphName, const FString& StructPath, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UScriptStruct* St = ResolveStruct(StructPath, E);
	if (!St) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add MakeStruct")));
	UK2Node_MakeStruct* Node = NewObject<UK2Node_MakeStruct>(Ctx.Graph);
	Node->StructType = St;
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddBreakStruct(const FString& BlueprintPath, const FString& GraphName, const FString& StructPath, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UScriptStruct* St = ResolveStruct(StructPath, E);
	if (!St) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add BreakStruct")));
	UK2Node_BreakStruct* Node = NewObject<UK2Node_BreakStruct>(Ctx.Graph);
	Node->StructType = St;
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

static FString AddDispatcherNodeImpl(const FString& BlueprintPath, const FString& GraphName, const FString& DispatcherName, float X, float Y, bool bBind)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	UClass* Skel = Ctx.BP->SkeletonGeneratedClass ? Ctx.BP->SkeletonGeneratedClass.Get() : Ctx.BP->GeneratedClass.Get();
	FMulticastDelegateProperty* Prop = Skel ? FindFProperty<FMulticastDelegateProperty>(Skel, FName(*DispatcherName)) : nullptr;
	if (!Prop)
	{
		TArray<FString> Names;
		for (const FBPVariableDescription& V : Ctx.BP->NewVariables)
		{
			if (V.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) { Names.Add(V.VarName.ToString()); }
		}
		return ErrWithList(FString::Printf(TEXT("Event dispatcher '%s' not found"), *DispatcherName), TEXT("dispatchers"), Names);
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Dispatcher Node")));
	UK2Node_BaseMCDelegate* Node;
	if (bBind) { Node = NewObject<UK2Node_AddDelegate>(Ctx.Graph); }
	else       { Node = NewObject<UK2Node_CallDelegate>(Ctx.Graph); }
	Node->SetFromProperty(Prop, /*bSelfContext*/ true, Prop->GetOwnerClass());
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddDispatcherCall(const FString& BlueprintPath, const FString& GraphName, const FString& DispatcherName, float X, float Y)
{
	return AddDispatcherNodeImpl(BlueprintPath, GraphName, DispatcherName, X, Y, false);
}

FString UFableBP::AddDispatcherBind(const FString& BlueprintPath, const FString& GraphName, const FString& DispatcherName, float X, float Y)
{
	return AddDispatcherNodeImpl(BlueprintPath, GraphName, DispatcherName, X, Y, true);
}

FString UFableBP::AddNodeByClass(const FString& BlueprintPath, const FString& GraphName, const FString& NodeClassPath, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UClass* C = ResolveClass(NodeClassPath, E);
	if (!C) { return Err(E); }
	if (!C->IsChildOf(UEdGraphNode::StaticClass())) { return Err(FString::Printf(TEXT("%s is not a UEdGraphNode subclass"), *C->GetPathName())); }
	if (C->HasAnyClassFlags(CLASS_Abstract)) { return Err(FString::Printf(TEXT("%s is abstract"), *C->GetPathName())); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Node")));
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Ctx.Graph, C);
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	return OkNode(Node);
}

FString UFableBP::AddComment(const FString& BlueprintPath, const FString& GraphName, const FString& Text, float X, float Y, float W, float H)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Comment")));
	UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Ctx.Graph);
	Node->NodeComment = Text;
	FinishSpawn(Ctx.BP, Ctx.Graph, Node, X, Y);
	Node->NodeWidth = static_cast<int32>(W > 0.f ? W : 400.f);
	Node->NodeHeight = static_cast<int32>(H > 0.f ? H : 200.f);
	return OkNode(Node);
}

// ================================================================== node edits

FString UFableBP::DeleteNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E);
	if (!N) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Delete Node")));
	N->Modify();
	Ctx.Graph->GetSchema()->BreakNodeLinks(*N);
	N->DestroyNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

FString UFableBP::MoveNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, float X, float Y)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E);
	if (!N) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Move Node")));
	N->Modify();
	N->NodePosX = static_cast<int32>(X);
	N->NodePosY = static_cast<int32>(Y);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);
	return OkNode(N);
}

FString UFableBP::ReconstructNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E);
	if (!N) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Reconstruct Node")));
	N->Modify();
	N->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);
	return OkNode(N);
}

FString UFableBP::AddInputPin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E);
	if (!N) { return E; }

	IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(N);
	if (!AddPin) { return Err(TEXT("Node does not support adding pins")); }
	if (!AddPin->CanAddPin()) { return Err(TEXT("Node refuses more pins")); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Input Pin")));
	N->Modify();
	AddPin->AddInputPin();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);
	return OkNode(N);
}

FString UFableBP::AddUserPin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName, const FString& Type)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E);
	if (!N) { return E; }

	UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(N);
	if (!Editable) { return Err(TEXT("Node is not a custom event / function entry / function result")); }

	FEdGraphPinType T;
	if (!ParseType(Type, T, E)) { return Err(E); }

	const EEdGraphPinDirection Dir = N->IsA<UK2Node_FunctionResult>() ? EGPD_Input : EGPD_Output;

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add User Pin")));
	N->Modify();
	UEdGraphPin* NewPin = Editable->CreateUserDefinedPin(FName(*PinName), T, Dir, /*bUseUniqueName*/ true);
	if (!NewPin) { return Err(FString::Printf(TEXT("Failed to create pin '%s' (type may be invalid for this node)"), *PinName)); }
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);
	return OkNode(N);
}

// ================================================================== pins

FString UFableBP::ConnectPins(const FString& BlueprintPath, const FString& GraphName, const FString& NodeA, const FString& PinA, const FString& NodeB, const FString& PinB)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* NA = FindNode(Ctx.Graph, NodeA, E); if (!NA) { return E; }
	UEdGraphNode* NB = FindNode(Ctx.Graph, NodeB, E); if (!NB) { return E; }
	UEdGraphPin* PA = FindPin(NA, PinA, E); if (!PA) { return E; }
	UEdGraphPin* PB = FindPin(NB, PinB, E); if (!PB) { return E; }

	const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
	const FPinConnectionResponse Response = Schema->CanCreateConnection(PA, PB);

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Connect Pins")));
	NA->Modify();
	NB->Modify();
	const bool bOk = Schema->TryCreateConnection(PA, PB);
	if (bOk) { FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), bOk);
	O->SetStringField(TEXT("response"), Response.Message.ToString());
	if (!bOk && Response.Message.IsEmpty())
	{
		O->SetStringField(TEXT("error"), TEXT("Connection refused by schema"));
	}
	else if (!bOk)
	{
		O->SetStringField(TEXT("error"), Response.Message.ToString());
	}
	return ToJson(O);
}

FString UFableBP::BreakPinLink(const FString& BlueprintPath, const FString& GraphName, const FString& NodeA, const FString& PinA, const FString& NodeB, const FString& PinB)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* NA = FindNode(Ctx.Graph, NodeA, E); if (!NA) { return E; }
	UEdGraphNode* NB = FindNode(Ctx.Graph, NodeB, E); if (!NB) { return E; }
	UEdGraphPin* PA = FindPin(NA, PinA, E); if (!PA) { return E; }
	UEdGraphPin* PB = FindPin(NB, PinB, E); if (!PB) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Break Pin Link")));
	NA->Modify();
	NB->Modify();
	Ctx.Graph->GetSchema()->BreakSinglePinLink(PA, PB);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

FString UFableBP::BreakAllPinLinks(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E); if (!N) { return E; }
	UEdGraphPin* P = FindPin(N, PinName, E); if (!P) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Break All Pin Links")));
	N->Modify();
	Ctx.Graph->GetSchema()->BreakPinLinks(*P, /*bSendsNodeNotification*/ true);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

FString UFableBP::ClearDeadBindings(const FString& BlueprintPath)
{
	FString E;
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
	if (!WBP) { return Err(FString::Printf(TEXT("Not a WidgetBlueprint: %s"), *BlueprintPath)); }
	if (!CheckMutate(E)) { return Err(E); }

	// Deliberately NOT consulting GeneratedClass — it still carries functions/properties from the last
	// successful compile, so a binding whose function GRAPH was deleted looks "alive" through it
	// (observed: removed:0 on a binding the UMG compiler then hard-errored on). Truth = current
	// authoring state (function graphs + BP variables) plus never-stale NATIVE parents.
	UClass* NativeParent = WBP->ParentClass;
	while (NativeParent && !NativeParent->HasAnyClassFlags(CLASS_Native))
	{
		NativeParent = NativeParent->GetSuperClass();
	}

	auto BindingIsAlive = [&](const FDelegateEditorBinding& Binding) -> bool
	{
		if (!Binding.FunctionName.IsNone())
		{
			for (UEdGraph* G : WBP->FunctionGraphs)
			{
				if (G && G->GetFName() == Binding.FunctionName) { return true; }
			}
			return NativeParent && NativeParent->FindFunctionByName(Binding.FunctionName) != nullptr;
		}
		if (!Binding.SourceProperty.IsNone())
		{
			for (const FBPVariableDescription& Var : WBP->NewVariables)
			{
				if (Var.VarName == Binding.SourceProperty) { return true; }
			}
			return NativeParent && NativeParent->FindPropertyByName(Binding.SourceProperty) != nullptr;
		}
		if (!Binding.SourcePath.IsEmpty()) { return true; }  // path-style bindings resolve elsewhere
		return false;                                        // nothing to call = dead entry
	};

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Clear Dead Bindings")));
	WBP->Modify();
	int32 Removed = 0;
	for (int32 i = WBP->Bindings.Num() - 1; i >= 0; --i)
	{
		if (!BindingIsAlive(WBP->Bindings[i]))
		{
			WBP->Bindings.RemoveAt(i);
			++Removed;
		}
	}
	if (Removed > 0) { FBlueprintEditorUtils::MarkBlueprintAsModified(WBP); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetNumberField(TEXT("removed"), Removed);
	O->SetNumberField(TEXT("kept"), WBP->Bindings.Num());
	return ToJson(O);
}

/* ---- Widget-tree authoring ---- */

static UWidgetBlueprint* LoadWBP(const FString& Path, FString& OutErr)
{
	UBlueprint* BP = LoadBP(Path, OutErr);
	if (!BP) { return nullptr; }
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
	if (!WBP) { OutErr = FString::Printf(TEXT("Not a WidgetBlueprint: %s"), *Path); }
	return WBP;
}

static UWidget* FindTreeWidget(UWidgetBlueprint* WBP, const FString& Name, FString& OutErr)
{
	UWidget* W = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*Name)) : nullptr;
	if (!W)
	{
		TArray<FString> Names;
		if (WBP->WidgetTree)
		{
			WBP->WidgetTree->ForEachWidget([&Names](UWidget* Each) { if (Each) { Names.Add(Each->GetName()); } });
		}
		OutErr = ErrWithList(FString::Printf(TEXT("No tree widget named '%s'"), *Name), TEXT("widgets"), Names);
	}
	return W;
}

FString UFableBP::CreateWidgetBlueprint(const FString& PackagePath, const FString& AssetName, const FString& ParentClassPath)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }

	UClass* Parent = UUserWidget::StaticClass();
	if (!ParentClassPath.IsEmpty())
	{
		Parent = ResolveClass(ParentClassPath, E);
		if (!Parent) { return Err(E); }
		if (!Parent->IsChildOf(UUserWidget::StaticClass()))
		{
			return Err(FString::Printf(TEXT("Parent is not a UUserWidget: %s"), *ParentClassPath));
		}
	}

	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = Parent;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* Asset = AssetTools.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
	if (!Asset) { return Err(TEXT("CreateAsset failed (does the asset already exist?)")); }

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("path"), Asset->GetPathName());
	return ToJson(O);
}

FString UFableBP::WtListWidgets(const FString& BlueprintPath)
{
	FString E;
	UWidgetBlueprint* WBP = LoadWBP(BlueprintPath, E);
	if (!WBP) { return Err(E); }

	TArray<FJVal> Rows;
	if (WBP->WidgetTree)
	{
		WBP->WidgetTree->ForEachWidget([&Rows](UWidget* W)
		{
			if (!W) { return; }
			FJObj Row = NewObj();
			Row->SetStringField(TEXT("name"), W->GetName());
			Row->SetStringField(TEXT("class"), W->GetClass()->GetName());
			Row->SetStringField(TEXT("parent"), W->GetParent() ? W->GetParent()->GetName() : TEXT(""));
			Row->SetStringField(TEXT("slot"), W->Slot ? W->Slot->GetClass()->GetName() : TEXT(""));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		});
	}
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("root"), (WBP->WidgetTree && WBP->WidgetTree->RootWidget) ? WBP->WidgetTree->RootWidget->GetName() : TEXT(""));
	O->SetArrayField(TEXT("widgets"), Rows);
	return ToJson(O);
}

FString UFableBP::WtAddWidget(const FString& BlueprintPath, const FString& WidgetClassPath, const FString& WidgetName, const FString& ParentName)
{
	FString E;
	UWidgetBlueprint* WBP = LoadWBP(BlueprintPath, E);
	if (!WBP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	if (!WBP->WidgetTree) { return Err(TEXT("Widget has no WidgetTree")); }

	UClass* WidgetClass = ResolveClass(WidgetClassPath, E);
	if (!WidgetClass) { return Err(E); }
	if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		return Err(FString::Printf(TEXT("Not a UWidget class: %s"), *WidgetClassPath));
	}
	if (WBP->WidgetTree->FindWidget(FName(*WidgetName)))
	{
		return Err(FString::Printf(TEXT("A tree widget named '%s' already exists"), *WidgetName));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Tree Widget")));
	WBP->WidgetTree->Modify();

	UWidget* NewWidgetObj = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
	if (!NewWidgetObj) { return Err(TEXT("ConstructWidget failed")); }

	if (ParentName.IsEmpty())
	{
		if (WBP->WidgetTree->RootWidget)
		{
			return Err(TEXT("Tree already has a root — pass ParentName"));
		}
		WBP->WidgetTree->RootWidget = NewWidgetObj;
	}
	else
	{
		UWidget* Parent = FindTreeWidget(WBP, ParentName, E);
		if (!Parent) { return E; }
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Parent))
		{
			Panel->Modify();
			Panel->AddChild(NewWidgetObj);
		}
		else if (UContentWidget* Content = Cast<UContentWidget>(Parent))
		{
			Content->Modify();
			Content->SetContent(NewWidgetObj);
		}
		else
		{
			return Err(FString::Printf(TEXT("Parent '%s' (%s) cannot hold children"), *ParentName, *Parent->GetClass()->GetName()));
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("name"), NewWidgetObj->GetName());
	O->SetStringField(TEXT("class"), WidgetClass->GetName());
	O->SetStringField(TEXT("slot"), NewWidgetObj->Slot ? NewWidgetObj->Slot->GetClass()->GetName() : TEXT(""));
	return ToJson(O);
}

FString UFableBP::WtRemoveWidget(const FString& BlueprintPath, const FString& WidgetName)
{
	FString E;
	UWidgetBlueprint* WBP = LoadWBP(BlueprintPath, E);
	if (!WBP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	UWidget* W = FindTreeWidget(WBP, WidgetName, E);
	if (!W) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Remove Tree Widget")));
	WBP->WidgetTree->Modify();
	if (WBP->WidgetTree->RootWidget == W)
	{
		WBP->WidgetTree->RootWidget = nullptr;
	}
	else if (UPanelWidget* Parent = W->GetParent())
	{
		Parent->Modify();
		Parent->RemoveChild(W);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

static FString SetPropsOnObject(UWidgetBlueprint* WBP, UObject* Target, const FString& PropsJson)
{
	TSharedPtr<FJsonObject> Props;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PropsJson), Props) || !Props.IsValid())
	{
		return Err(TEXT("PropsJson is not a JSON object"));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Widget Props")));
	Target->Modify();

	TArray<FString> Set, Failed;
	for (const auto& Pair : Props->Values)
	{
		FString ValueText;
		if (Pair.Value->Type == EJson::String) { ValueText = Pair.Value->AsString(); }
		else if (Pair.Value->Type == EJson::Boolean) { ValueText = Pair.Value->AsBool() ? TEXT("True") : TEXT("False"); }
		else if (Pair.Value->Type == EJson::Number) { ValueText = LexToString(Pair.Value->AsNumber()); }
		else { Failed.Add(Pair.Key + TEXT(" (unsupported JSON type)")); continue; }

		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop) { Failed.Add(Pair.Key + TEXT(" (no such property)")); continue; }
		// T3D-syntax struct literals (brushes, styles, fonts) import verbatim here.
		const TCHAR* Result = Prop->ImportText_InContainer(*ValueText, Target, Target, PPF_None);
		if (Result) { Set.Add(Pair.Key); } else { Failed.Add(Pair.Key + TEXT(" (ImportText rejected the value)")); }
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), Failed.Num() == 0);
	SetStrArray(O, TEXT("set"), Set);
	SetStrArray(O, TEXT("failed"), Failed);
	return ToJson(O);
}

FString UFableBP::WtSetProps(const FString& BlueprintPath, const FString& WidgetName, const FString& PropsJson)
{
	FString E;
	UWidgetBlueprint* WBP = LoadWBP(BlueprintPath, E);
	if (!WBP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	UWidget* W = FindTreeWidget(WBP, WidgetName, E);
	if (!W) { return E; }
	return SetPropsOnObject(WBP, W, PropsJson);
}

FString UFableBP::WtSetSlotProps(const FString& BlueprintPath, const FString& WidgetName, const FString& PropsJson)
{
	FString E;
	UWidgetBlueprint* WBP = LoadWBP(BlueprintPath, E);
	if (!WBP) { return Err(E); }
	if (!CheckMutate(E)) { return Err(E); }
	UWidget* W = FindTreeWidget(WBP, WidgetName, E);
	if (!W) { return E; }
	if (!W->Slot) { return Err(FString::Printf(TEXT("'%s' has no layout slot (is it the root?)"), *WidgetName)); }
	return SetPropsOnObject(WBP, W->Slot, PropsJson);
}

FString UFableBP::SetPinDefault(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName, const FString& Value)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E); if (!N) { return E; }
	UEdGraphPin* P = FindPin(N, PinName, E); if (!P) { return E; }

	const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Pin Default")));
	N->Modify();

	const FName Cat = P->PinType.PinCategory;
	if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class || Cat == UEdGraphSchema_K2::PC_Interface)
	{
		UObject* Obj = nullptr;
		if (!Value.IsEmpty())
		{
			if (Cat == UEdGraphSchema_K2::PC_Class)
			{
				Obj = ResolveClass(Value, E);
			}
			else
			{
				Obj = LoadObject<UObject>(nullptr, *Value);
				if (!Obj) { Obj = LoadObject<UObject>(nullptr, *NormalizeAssetPath(Value)); }
			}
			if (!Obj) { return Err(FString::Printf(TEXT("Object not found: %s"), *Value)); }
		}
		Schema->TrySetDefaultObject(*P, Obj);
	}
	else if (Cat == UEdGraphSchema_K2::PC_Text)
	{
		Schema->TrySetDefaultText(*P, FText::FromString(Value));
	}
	else
	{
		Schema->TrySetDefaultValue(*P, Value);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetObjectField(TEXT("pin"), PinToJson(P));
	return ToJson(O);
}

FString UFableBP::SplitPin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E); if (!N) { return E; }
	UEdGraphPin* P = FindPin(N, PinName, E); if (!P) { return E; }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Split Pin")));
	N->Modify();
	Ctx.Graph->GetSchema()->SplitPin(P, /*bNotify*/ true);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);
	return OkNode(N);
}

FString UFableBP::RecombinePin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	FString E;
	UEdGraphNode* N = FindNode(Ctx.Graph, NodeId, E); if (!N) { return E; }
	UEdGraphPin* P = FindPin(N, PinName, E); if (!P) { return E; }

	// RecombinePin expects one of the generated sub-pins.
	if (P->SubPins.Num() > 0 && P->SubPins[0]) { P = P->SubPins[0]; }
	else if (!P->ParentPin) { return Err(TEXT("Pin is not split")); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Recombine Pin")));
	N->Modify();
	Ctx.Graph->GetSchema()->RecombinePin(P);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.BP);
	return OkNode(N);
}

// ================================================================== members

FString UFableBP::AddVariable(const FString& BlueprintPath, const FString& VarName, const FString& Type, const FString& DefaultValue, const FString& Category, bool bInstanceEditable, bool bBlueprintReadOnly, bool bExposeOnSpawn, const FString& Replication)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	FEdGraphPinType T;
	if (!ParseType(Type, T, E)) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Variable")));
	BP->Modify();
	const FName VarFName(*VarName);
	if (!FBlueprintEditorUtils::AddMemberVariable(BP, VarFName, T, DefaultValue))
	{
		return Err(FString::Printf(TEXT("AddMemberVariable failed for '%s' (name clash or invalid name?)"), *VarName));
	}
	if (!ApplyVarFlags(BP, VarFName, Category, bInstanceEditable, bBlueprintReadOnly, bExposeOnSpawn, Replication, E))
	{
		return Err(E);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	const int32 Idx = FindBPVarIndex(BP, VarFName);
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	if (Idx != INDEX_NONE) { O->SetObjectField(TEXT("variable"), VarToJson(BP->NewVariables[Idx])); }
	return ToJson(O);
}

FString UFableBP::RemoveVariable(const FString& BlueprintPath, const FString& VarName)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	if (FindBPVarIndex(BP, FName(*VarName)) == INDEX_NONE)
	{
		return Err(FString::Printf(TEXT("Variable '%s' is not declared on this BP"), *VarName));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Remove Variable")));
	BP->Modify();
	FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*VarName));

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

FString UFableBP::SetVariableFlags(const FString& BlueprintPath, const FString& VarName, const FString& Category, bool bInstanceEditable, bool bBlueprintReadOnly, bool bExposeOnSpawn, const FString& Replication)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Set Variable Flags")));
	BP->Modify();
	const FName VarFName(*VarName);
	if (!ApplyVarFlags(BP, VarFName, Category, bInstanceEditable, bBlueprintReadOnly, bExposeOnSpawn, Replication, E))
	{
		return Err(E);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	const int32 Idx = FindBPVarIndex(BP, VarFName);
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	if (Idx != INDEX_NONE) { O->SetObjectField(TEXT("variable"), VarToJson(BP->NewVariables[Idx])); }
	return ToJson(O);
}

FString UFableBP::AddFunction(const FString& BlueprintPath, const FString& FunctionName, const FString& Inputs, const FString& Outputs, bool bPure)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	TArray<FGraphInfo> Graphs;
	CollectGraphs(BP, Graphs);
	for (const FGraphInfo& GI : Graphs)
	{
		if (GI.Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			return Err(FString::Printf(TEXT("A graph named '%s' already exists (%s)"), *FunctionName, *GI.Kind));
		}
	}

	TArray<FParamDecl> InDecls, OutDecls;
	if (!ParseParamList(Inputs, InDecls, E)) { return Err(E); }
	if (!ParseParamList(Outputs, OutDecls, E)) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Function")));
	BP->Modify();
	UEdGraph* G = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, G, /*bIsUserCreated*/ true, static_cast<UClass*>(nullptr));

	TArray<UK2Node_FunctionEntry*> Entries;
	G->GetNodesOfClass(Entries);
	if (Entries.Num() == 0) { return Err(TEXT("Function graph has no entry node (unexpected)")); }
	UK2Node_FunctionEntry* Entry = Entries[0];

	for (const FParamDecl& D : InDecls)
	{
		Entry->CreateUserDefinedPin(FName(*D.Name), D.Type, EGPD_Output, true);
	}
	UK2Node_FunctionResult* Result = nullptr;
	if (OutDecls.Num() > 0)
	{
		Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
		for (const FParamDecl& D : OutDecls)
		{
			if (Result) { Result->CreateUserDefinedPin(FName(*D.Name), D.Type, EGPD_Input, true); }
		}
	}
	if (bPure) { Entry->AddExtraFlags(FUNC_BlueprintPure); }
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("graph"), G->GetName());
	O->SetObjectField(TEXT("entry"), NodeToJson(Entry, true));
	if (Result) { O->SetObjectField(TEXT("result"), NodeToJson(Result, true)); }
	return ToJson(O);
}

FString UFableBP::AddFunctionOverride(const FString& BlueprintPath, const FString& FunctionName)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	UFunction* Func = BP->ParentClass ? BP->ParentClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
	if (!Func)
	{
		for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
		{
			if (Desc.Interface)
			{
				Func = Desc.Interface->FindFunctionByName(FName(*FunctionName));
				if (Func) { break; }
			}
		}
	}
	if (!Func) { return Err(FString::Printf(TEXT("Function '%s' not found on parent or interfaces"), *FunctionName)); }
	if (!Func->HasAnyFunctionFlags(FUNC_BlueprintEvent)) { return Err(FString::Printf(TEXT("'%s' is not overridable (needs BlueprintImplementableEvent/BlueprintNativeEvent)"), *FunctionName)); }

	for (UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing->GetFName() == Func->GetFName())
		{
			return Err(FString::Printf(TEXT("Override graph '%s' already exists"), *FunctionName));
		}
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Function Override")));
	BP->Modify();
	UEdGraph* G = FBlueprintEditorUtils::CreateNewGraph(BP, Func->GetFName(), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, G, /*bIsUserCreated*/ false, Func);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("graph"), G->GetName());
	return ToJson(O);
}

FString UFableBP::AddLocalVariable(const FString& BlueprintPath, const FString& FunctionGraphName, const FString& VarName, const FString& Type, const FString& DefaultValue)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, FunctionGraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }

	FEdGraphPinType T;
	FString E;
	if (!ParseType(Type, T, E)) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Local Variable")));
	Ctx.BP->Modify();
	if (!FBlueprintEditorUtils::AddLocalVariable(Ctx.BP, Ctx.Graph, FName(*VarName), T, DefaultValue))
	{
		return Err(FString::Printf(TEXT("AddLocalVariable failed for '%s' (is '%s' a function graph?)"), *VarName, *Ctx.Graph->GetName()));
	}
	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

FString UFableBP::AddEventDispatcher(const FString& BlueprintPath, const FString& DispatcherName, const FString& Inputs)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	TArray<FParamDecl> InDecls;
	if (!ParseParamList(Inputs, InDecls, E)) { return Err(E); }

	// Mirrors FBlueprintEditor::OnAddNewDelegate.
	const UEdGraphSchema_K2* K2 = GetDefault<UEdGraphSchema_K2>();
	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Event Dispatcher")));
	BP->Modify();

	const FName Name(*DispatcherName);
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(BP, Name, DelegateType))
	{
		return Err(FString::Printf(TEXT("Could not add dispatcher variable '%s' (name clash?)"), *DispatcherName));
	}

	UEdGraph* G = FBlueprintEditorUtils::CreateNewGraph(BP, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!G)
	{
		FBlueprintEditorUtils::RemoveMemberVariable(BP, Name);
		return Err(TEXT("Could not create dispatcher signature graph"));
	}
	G->bEditable = false;
	K2->CreateDefaultNodesForGraph(*G);
	K2->CreateFunctionGraphTerminators(*G, static_cast<UClass*>(nullptr));
	K2->AddExtraFunctionFlags(G, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
	K2->MarkFunctionEntryAsEditable(G, true);
	BP->DelegateSignatureGraphs.Add(G);

	if (InDecls.Num() > 0)
	{
		TArray<UK2Node_FunctionEntry*> Entries;
		G->GetNodesOfClass(Entries);
		if (Entries.Num() > 0)
		{
			for (const FParamDecl& D : InDecls)
			{
				Entries[0]->CreateUserDefinedPin(FName(*D.Name), D.Type, EGPD_Output, true);
			}
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetStringField(TEXT("dispatcher"), DispatcherName);
	return ToJson(O);
}

FString UFableBP::AddInterface(const FString& BlueprintPath, const FString& InterfacePath)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }
	UClass* IfaceClass = ResolveClass(InterfacePath, E);
	if (!IfaceClass) { return Err(E); }

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Add Interface")));
	BP->Modify();
	const bool bOk = FBlueprintEditorUtils::ImplementNewInterface(BP, IfaceClass->GetClassPathName());

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), bOk);
	if (!bOk) { O->SetStringField(TEXT("error"), FString::Printf(TEXT("ImplementNewInterface refused %s (already implemented, or not a Blueprint-implementable interface?)"), *IfaceClass->GetPathName())); }
	return ToJson(O);
}

FString UFableBP::RemoveGraphByName(const FString& BlueprintPath, const FString& GraphName)
{
	FGraphCtx Ctx = GetGraphCtx(BlueprintPath, GraphName, true);
	if (!Ctx.IsValid()) { return Ctx.Error; }
	if (Ctx.BP->UbergraphPages.Contains(Ctx.Graph) && Ctx.BP->UbergraphPages.Num() <= 1)
	{
		return Err(TEXT("Refusing to remove the only event graph"));
	}

	const FScopedTransaction Txn(FText::FromString(TEXT("FableKit: Remove Graph")));
	Ctx.BP->Modify();
	FBlueprintEditorUtils::RemoveGraph(Ctx.BP, Ctx.Graph, EGraphRemoveFlags::Default);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.BP);

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	return ToJson(O);
}

// ================================================================== lifecycle

FString UFableBP::CompileBP(const FString& BlueprintPath)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	UBlueprint* BP = LoadBP(BlueprintPath, E);
	if (!BP) { return Err(E); }

	FCompilerResultsLog Results;
	// SkipSave: the compile manager's save-on-compile path regenerates asset THUMBNAILS,
	// which spawns preview actors in a transient world — stale cached preview actors from a
	// pre-reparent class fatally collide with renamed native components (Cascade vs Niagara
	// 'Trail'). Callers save explicitly via EditorAssetLibrary instead.
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipSave, &Results);

	TArray<FString> Errors, Warnings;
	for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
	{
		switch (Msg->GetSeverity())
		{
		case EMessageSeverity::Error:
			Errors.Add(Msg->ToText().ToString());
			break;
		case EMessageSeverity::Warning:
		case EMessageSeverity::PerformanceWarning:
			Warnings.Add(Msg->ToText().ToString());
			break;
		default:
			break;
		}
	}

	FJObj O = NewObj();
	const bool bOk = (Results.NumErrors == 0) && (BP->Status != BS_Error);
	O->SetBoolField(TEXT("ok"), bOk);
	O->SetNumberField(TEXT("num_errors"), Results.NumErrors);
	O->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
	SetStrArray(O, TEXT("errors"), Errors);
	SetStrArray(O, TEXT("warnings"), Warnings);
	switch (BP->Status)
	{
	case BS_UpToDate:             O->SetStringField(TEXT("status"), TEXT("UpToDate")); break;
	case BS_UpToDateWithWarnings: O->SetStringField(TEXT("status"), TEXT("UpToDateWithWarnings")); break;
	case BS_Error:                O->SetStringField(TEXT("status"), TEXT("Error")); break;
	case BS_Dirty:                O->SetStringField(TEXT("status"), TEXT("Dirty")); break;
	default:                      O->SetStringField(TEXT("status"), TEXT("Unknown")); break;
	}
	return ToJson(O);
}

FString UFableBP::FixupRedirectors(const FString& FolderPath)
{
	FString E;
	if (!CheckMutate(E)) { return Err(E); }
	const FString Path = FolderPath.IsEmpty() ? TEXT("/Game") : FolderPath;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByPath(FName(*Path), Assets, /*bRecursive*/ true);

	TArray<UObjectRedirector*> Redirectors;
	for (const FAssetData& A : Assets)
	{
		if (A.AssetClassPath == UObjectRedirector::StaticClass()->GetClassPathName())
		{
			if (UObjectRedirector* R = Cast<UObjectRedirector>(A.GetAsset()))
			{
				Redirectors.Add(R);
			}
		}
	}
	if (Redirectors.Num() > 0)
	{
		FAssetToolsModule::GetModule().Get().FixupReferencers(Redirectors, /*bCheckoutDialogPrompt*/ false, ERedirectFixupMode::DeleteFixedUpRedirectors);
	}

	FJObj O = NewObj();
	O->SetBoolField(TEXT("ok"), true);
	O->SetNumberField(TEXT("fixed"), Redirectors.Num());
	return ToJson(O);
}
