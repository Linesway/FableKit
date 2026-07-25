#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FableBP.generated.h"

/**
 * Blueprint-graph authoring surface for external automation (driven from Python
 * via Remote Control HTTP -> ExecutePythonCommandEx -> unreal.FableBP.*).
 *
 * Conventions:
 *  - Every function returns a JSON string. Mutators return {"ok":true,...} or
 *    {"ok":false,"error":"...","<context>":[...]} with enough context to self-correct
 *    (e.g. a bad pin name lists the node's actual pins).
 *  - BlueprintPath accepts "/Game/A/BP_X" or "/Game/A/BP_X.BP_X".
 *  - GraphName "" or "EventGraph" means the first ubergraph page.
 *  - Nodes are addressed by their NodeGuid (as returned in dumps / add results).
 *  - Pins are addressed by name, case/space-insensitive; prefix "in:"/"out:" to
 *    disambiguate direction.
 *  - Type strings: bool,byte,int,int64,float,string,name,text,vector,vector2d,
 *    rotator,transform,linearcolor,wildcard, object:<class>, softobject:<class>,
 *    class:<class>, softclass:<class>, interface:<class>, struct:<path>,
 *    enum:<path>, array:<inner>, set:<inner>, map:<key>|<value>.
 *    Class refs accept "/Script/Module.Class", "/Game/...BP.BP_C" or short native names.
 *  - Function refs: "<class>:<Func>" or ":<Func>" for self-context.
 *  - Mutators are transactional (Ctrl+Z works in the editor) and refuse to run during PIE.
 */
UCLASS()
class UFableBP : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---------- info / reading ----------

	/** Liveness check; returns plugin + engine version info. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString Ping();

	/** List all graphs in a Blueprint with their kind (ubergraph/function/macro/delegate/interface). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ListGraphs(const FString& BlueprintPath);

	/** Summary of a Blueprint: parent, interfaces, variables, graphs, components, dispatchers, status. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString DumpBlueprint(const FString& BlueprintPath);

	/** Full node+pin+link dump of one graph. Filter (optional) substring-matches node class or title. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString DumpGraph(const FString& BlueprintPath, const FString& GraphName, const FString& Filter);

	/** Single node detail (same shape as dump entries). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString GetNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId);

	/** Reflection listing of callable functions on a class (native, or BP via /Game path). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ListFunctions(const FString& ClassPath, const FString& Contains);

	/** Events / functions the Blueprint could override but hasn't (parent hierarchy + interfaces). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ListOverridableEvents(const FString& BlueprintPath);

	// ---------- node creation ----------

	/** Add a CallFunction node. FunctionRef = "<class>:<Func>" or ":<Func>" (self). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddCallFunction(const FString& BlueprintPath, const FString& GraphName, const FString& FunctionRef, float X, float Y);

	/** Add (or return existing) override event node, e.g. "ReceiveBeginPlay". ClassHint optional. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddEvent(const FString& BlueprintPath, const FString& GraphName, const FString& EventName, const FString& ClassHint, float X, float Y);

	/** Add a custom event node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddCustomEvent(const FString& BlueprintPath, const FString& GraphName, const FString& EventName, float X, float Y);

	/** Add a variable getter node (member var or SCS component). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddVarGet(const FString& BlueprintPath, const FString& GraphName, const FString& VarName, float X, float Y);

	/** Add a variable setter node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddVarSet(const FString& BlueprintPath, const FString& GraphName, const FString& VarName, float X, float Y);

	/** Add a Branch (if/else) node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddBranch(const FString& BlueprintPath, const FString& GraphName, float X, float Y);

	/** Add a Sequence node with NumOutputs exec outputs (min 2). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddSequence(const FString& BlueprintPath, const FString& GraphName, int32 NumOutputs, float X, float Y);

	/** Add a standard macro instance (ForEachLoop, ForLoop, WhileLoop, Gate, DoOnce, FlipFlop, IsValid, ...). Also accepts "<MacroLibPath>:<GraphName>". */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddMacro(const FString& BlueprintPath, const FString& GraphName, const FString& MacroName, float X, float Y);

	/** Add a dynamic cast node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddCast(const FString& BlueprintPath, const FString& GraphName, const FString& TargetClass, bool bPure, float X, float Y);

	/** Add a SpawnActorFromClass node with its Class pin preset. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddSpawnActor(const FString& BlueprintPath, const FString& GraphName, const FString& ActorClass, float X, float Y);

	/** Add a MakeStruct node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddMakeStruct(const FString& BlueprintPath, const FString& GraphName, const FString& StructPath, float X, float Y);

	/** Add a BreakStruct node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddBreakStruct(const FString& BlueprintPath, const FString& GraphName, const FString& StructPath, float X, float Y);

	/** Add a call node for an event dispatcher declared on this BP. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddDispatcherCall(const FString& BlueprintPath, const FString& GraphName, const FString& DispatcherName, float X, float Y);

	/** Add a Bind (AddDelegate) node for an event dispatcher. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddDispatcherBind(const FString& BlueprintPath, const FString& GraphName, const FString& DispatcherName, float X, float Y);

	/** Escape hatch: spawn any non-abstract UEdGraphNode subclass by class path. Post-configure its
	 *  reflected properties from Python via unreal.find_object(<returned node path>) + set_editor_property,
	 *  then ReconstructNode. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddNodeByClass(const FString& BlueprintPath, const FString& GraphName, const FString& NodeClassPath, float X, float Y);

	/** Add a comment box. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddComment(const FString& BlueprintPath, const FString& GraphName, const FString& Text, float X, float Y, float W, float H);

	// ---------- node edits ----------

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString DeleteNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString MoveNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, float X, float Y);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ReconstructNode(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId);

	/** Grow a node that supports it (Sequence, Select, commutative math, ...) by one input pin. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddInputPin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId);

	/** Add a user-defined pin (parameter) to a custom event / function entry / function result node. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddUserPin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName, const FString& Type);

	// ---------- pins ----------

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ConnectPins(const FString& BlueprintPath, const FString& GraphName, const FString& NodeA, const FString& PinA, const FString& NodeB, const FString& PinB);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString BreakPinLink(const FString& BlueprintPath, const FString& GraphName, const FString& NodeA, const FString& PinA, const FString& NodeB, const FString& PinB);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString BreakAllPinLinks(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName);

	/** Set a pin literal. Object/class pins take an object path; text pins set localized text; others take the literal string. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString SetPinDefault(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName, const FString& Value);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString SplitPin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString RecombinePin(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName);

	// ---------- members ----------

	/** Add a member variable. Replication: "" | "none" | "replicated" | "repnotify" (repnotify also creates OnRep_<Name> graph). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddVariable(const FString& BlueprintPath, const FString& VarName, const FString& Type, const FString& DefaultValue, const FString& Category, bool bInstanceEditable, bool bBlueprintReadOnly, bool bExposeOnSpawn, const FString& Replication);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString RemoveVariable(const FString& BlueprintPath, const FString& VarName);

	/** Re-apply editability/category/replication flags on an existing BP variable. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString SetVariableFlags(const FString& BlueprintPath, const FString& VarName, const FString& Category, bool bInstanceEditable, bool bBlueprintReadOnly, bool bExposeOnSpawn, const FString& Replication);

	/** Create a function graph. Inputs/Outputs: "Name:type;Name2:type2" ("" for none). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddFunction(const FString& BlueprintPath, const FString& FunctionName, const FString& Inputs, const FString& Outputs, bool bPure);

	/** Create an override graph for a parent BlueprintNativeEvent/BlueprintImplementableEvent function (the with-return-value kind). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddFunctionOverride(const FString& BlueprintPath, const FString& FunctionName);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddLocalVariable(const FString& BlueprintPath, const FString& FunctionGraphName, const FString& VarName, const FString& Type, const FString& DefaultValue);

	/** Create an event dispatcher with an optional "Name:type;..." signature. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddEventDispatcher(const FString& BlueprintPath, const FString& DispatcherName, const FString& Inputs);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddInterface(const FString& BlueprintPath, const FString& InterfacePath);

	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString RemoveGraphByName(const FString& BlueprintPath, const FString& GraphName);

	// ---------- lifecycle ----------

	/** Compile the Blueprint; returns status + compiler errors/warnings as text. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString CompileBP(const FString& BlueprintPath);

	/** Find + fix up all object redirectors under a /Game path (after asset moves/renames). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString FixupRedirectors(const FString& FolderPath);
};
