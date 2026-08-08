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

	/** Add the red "On Clicked (MyButton)" event node that binds a delegate declared on a CHILD
	 *  widget/component variable — the thing AddDispatcherBind cannot do, because that one only
	 *  reaches dispatchers on THIS blueprint (bSelfContext).
	 *
	 *  This has to be C++: UK2Node_ComponentBoundEvent's DelegatePropertyName / DelegateOwnerClass /
	 *  ComponentPropertyName are bare UPROPERTY() with no CPF_Edit | CPF_BlueprintVisible, and
	 *  PropertyAccessUtil::CanSetPropertyValue refuses those outright — so AddNodeByClass +
	 *  set_editor_property can never configure one from Python. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString AddComponentBoundEvent(const FString& BlueprintPath, const FString& GraphName, const FString& ComponentName, const FString& DelegateName, float X, float Y);

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

	/** Bind a 'Use cached pose' node to a 'Save cached pose' node.
	 *
	 *  Why this needs to exist: an anim POSE output feeds exactly one input, so forking a pose (which
	 *  is what an upper-body LayeredBoneBlend branch requires) needs a SaveCachedPose/UseCachedPose
	 *  pair. But the link is UAnimGraphNode_UseCachedPose::SaveCachedPoseNode — a bare UPROPERTY with
	 *  no EditAnywhere — so PropertyAccessUtil::CanSetPropertyValue refuses it and Python reports
	 *  "Failed to find property 'save_cached_pose_node'". It IS public C++, so this sets it directly,
	 *  and also stamps the private NameOfCache via reflection because EarlyValidation re-resolves the
	 *  pointer from that name at compile time (AnimGraphNode_UseCachedPose.cpp:47-68).
	 *
	 *  Both ids must be in the same graph. Reconstructs the Use node so its title/pins refresh. */
	UFUNCTION(BlueprintCallable, Category = "FableKit|Graph")
	static FString LinkCachedPose(const FString& BlueprintPath, const FString& GraphName,
	                              const FString& UseNodeId, const FString& SaveNodeId);

	/** Widget Blueprints only: remove FDelegateEditorBinding entries whose bound function/property no
	    longer exists (orphaned property bindings — the UMG compiler hard-errors on them and python can't
	    reach the protected Bindings array). Returns {ok, removed, kept}. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ClearDeadBindings(const FString& BlueprintPath);

	/* ---- Widget-tree authoring (UMG designer surface — v1 excluded it; these close the gap) ---- */

	/** Create a WidgetBlueprint asset (the generic Blueprint factory makes a non-designer Blueprint for
	    UUserWidget parents — this uses the real UWidgetBlueprintFactory). Parent: '/Script/M.Class' or a
	    '/Game/...' widget BP. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString CreateWidgetBlueprint(const FString& PackagePath, const FString& AssetName, const FString& ParentClassPath);

	/** Tree dump: every widget's name/class/parent/slot class. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtListWidgets(const FString& BlueprintPath);

	/** Construct a widget in the tree. Empty ParentName: becomes the root if none exists, else errors.
	    Parents that are panels AddChild; single-content widgets (Border/SizeBox/Button…) SetContent. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtAddWidget(const FString& BlueprintPath, const FString& WidgetClassPath, const FString& WidgetName, const FString& ParentName);

	/** Remove a widget (and its subtree) from the tree by name. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtRemoveWidget(const FString& BlueprintPath, const FString& WidgetName);

	/** Move a widget (with its whole subtree) under a different parent, optionally at a given child
	    index (-1 = append). The UWidget OBJECT is reused, so every authored property on it survives —
	    only the layout SLOT is rebuilt, so re-apply slot padding/alignment afterwards.
	    Refuses on the root, on self-parenting, on a cycle, and on a single-content parent that is
	    already occupied. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtReparentWidget(const FString& BlueprintPath, const FString& WidgetName, const FString& NewParentName, int32 Index = -1);

	/** Set properties on a tree widget from JSON {"Prop": "UE text value", ...}. Values go through
	    FProperty::ImportText, so struct literals in T3D syntax work verbatim (brushes, fonts, styles). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtSetProps(const FString& BlueprintPath, const FString& WidgetName, const FString& PropsJson);

	/** READ a tree widget's properties back out, in the same text form WtSetProps takes.
	    PropsCsv "" dumps everything; otherwise a comma-separated list of property names (either the
	    internal name or the authored/display name — BP variables like "In Font Info" work).
	    This is what lets you COPY a value off an authored widget onto a new one instead of guessing
	    at it, which is the only reliable way to make a fresh template instance match its siblings. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtGetProps(const FString& BlueprintPath, const FString& WidgetName, const FString& PropsCsv);

	/** Same, on the widget's layout SLOT (padding/alignment/size rules). */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtSetSlotProps(const FString& BlueprintPath, const FString& WidgetName, const FString& PropsJson);

	/** List a Widget Blueprint's UMG animations by name. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtListAnimations(const FString& BlueprintPath);

	/** Delete a UMG animation. Empty AnimationName removes ALL of them.
	 *
	 *  WHY THIS IS NATIVE: UWidgetBlueprint::Animations is a PROTECTED UPROPERTY, so Python cannot read
	 *  or write it ("Property 'Animations' ... is protected") — the same blind spot as WidgetTree. An
	 *  animation whose tracks point at deleted widgets makes every compile emit "trying to animate a
	 *  non-existent widget", which is otherwise unfixable from outside the editor UI. */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString WtRemoveAnimation(const FString& BlueprintPath, const FString& AnimationName);

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

	/**
	 * Change an EXISTING member variable's type in place.
	 *
	 * Needed whenever a Blueprint variable is declared against a class you are replacing — retyping
	 * is the only way to keep the variable's identity (and therefore every Get/Set node's links)
	 * while the type underneath it moves. Delegates to FBlueprintEditorUtils::ChangeMemberVariableType,
	 * which retypes the pins on every node that touches the variable; links whose other end is no
	 * longer type-compatible are dropped by the schema, so recompile and check before saving.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString SetVariableType(const FString& BlueprintPath, const FString& VarName, const FString& Type);

	/**
	 * ReconstructNode, but DISCARDING stale pins instead of keeping them as orphans.
	 *
	 * A plain reconstruct preserves any pin the new signature no longer has, as long as it still
	 * holds a link or a non-default value — that is the right default when a node is merely being
	 * refreshed, and the wrong one after a deliberate class or type change, where the leftover pin
	 * becomes a compile error ("In use pin 'X' no longer exists") or a warning ("Input pin 'X'
	 * specifying non-default value no longer exists") describing something you meant to remove.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ReconstructNodePurgeOrphans(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId);

	/**
	 * Repoint every Call Function node that targets OldClass at NewClass instead.
	 *
	 * The companion to SetVariableType when a class is being retired. Retyping the variables makes the
	 * DATA flow to the new class, but each call node still resolves its function against the old one,
	 * so its Target pin keeps the dead class's type and the graph fails with
	 * "X Object Reference is not compatible with Y Object Reference" on a link you did not touch.
	 * Only calls whose function actually exists on NewClass are moved; the rest are reported in
	 * `skipped` rather than being silently broken.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString RetargetFunctionCalls(const FString& BlueprintPath, const FString& OldClassPath, const FString& NewClassPath);

	/**
	 * Force a macro instance (For Each Loop, and friends) back to wildcard.
	 *
	 * UK2Node_MacroInstance caches what its wildcards resolved to in ResolvedWildcardType, and only
	 * clears it via PostFixupAllWildcardPins once EVERY wildcard pin is unlinked. A For Each Loop
	 * whose Array Element output still feeds something therefore keeps a dead class on its Array
	 * input forever, and the schema refuses any new connection with "Array of X is not compatible
	 * with Array of Y" — a link you cannot make and cannot see why.
	 *
	 * Breaks every link on the node, clears the cached type and reconstructs. THE CALLER MUST PUT THE
	 * LINKS BACK, attaching the array input first so the element type propagates before anything
	 * downstream is reattached.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString ResetMacroWildcards(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId);

	/**
	 * Repoint every variable Get/Set node that reads a member OF OldClass at NewClass instead.
	 *
	 * The variable counterpart to RetargetFunctionCalls. A "Get CurrentWorldSaveObject" reading off a
	 * row widget carries the row's class on its Target pin, so retiring that class leaves the node
	 * demanding the dead type from a caller that now supplies the new one. Only variables that exist
	 * on NewClass are moved; the rest are reported in `skipped`.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString RetargetVariableRefs(const FString& BlueprintPath, const FString& OldClassPath, const FString& NewClassPath);

	/**
	 * Change an existing user-defined pin's type on a custom event / function entry / function result.
	 *
	 * The counterpart to SetVariableType for a PARAMETER. Every caller node of the event is
	 * reconstructed afterwards so their argument pins pick the new type up, which is the step that is
	 * easy to forget and leaves the graph looking correct while refusing to compile.
	 */
	UFUNCTION(BlueprintCallable, Category = "FableKit")
	static FString SetUserPinType(const FString& BlueprintPath, const FString& GraphName, const FString& NodeId, const FString& PinName, const FString& Type);

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
