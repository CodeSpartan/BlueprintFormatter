#include "BPFormatWires.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace BPFormat
{
namespace
{
	constexpr int32 MaxPureCopies = 400;  // safety cap per Format click

	bool IsExecNode(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && UEdGraphSchema_K2::IsExecPin(*Pin))
			{
				return true;
			}
		}
		return false;
	}

	bool IsDataPin(const UEdGraphPin* Pin)
	{
		return Pin && !Pin->bHidden && !UEdGraphSchema_K2::IsExecPin(*Pin)
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate;
	}

	/** The exec node a node's value ends up feeding. bMulti when it feeds more than one. */
	struct FOwner
	{
		UEdGraphNode* Node = nullptr;
		bool bMulti = false;
	};

	FOwner OwnerOf(UEdGraphNode* Node, const TSet<UEdGraphNode*>& ScopeSet, TMap<UEdGraphNode*, FOwner>& Memo)
	{
		if (IsExecNode(Node))
		{
			return { Node, false };
		}
		if (const FOwner* Found = Memo.Find(Node))
		{
			return *Found;
		}
		Memo.Add(Node, FOwner()); // cycle guard

		FOwner Result;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsDataPin(Pin) || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* Reader = Linked ? Linked->GetOwningNode() : nullptr;
				if (!Reader || !ScopeSet.Contains(Reader))
				{
					continue;
				}
				const FOwner Sub = OwnerOf(Reader, ScopeSet, Memo);
				if (Sub.bMulti || (Sub.Node && Result.Node && Sub.Node != Result.Node))
				{
					Result = { nullptr, true };
					Memo[Node] = Result;
					return Result;
				}
				if (Sub.Node)
				{
					Result.Node = Sub.Node;
				}
			}
		}
		Memo[Node] = Result;
		return Result;
	}

	UEdGraphPin* MatchingPin(UEdGraphNode* Copy, const UEdGraphNode* Source, const UEdGraphPin* SourcePin)
	{
		if (UEdGraphPin* ByName = Copy->FindPin(SourcePin->PinName, SourcePin->Direction))
		{
			return ByName;
		}
		const int32 Index = Source->Pins.IndexOfByKey(SourcePin);
		return (Copy->Pins.Num() == Source->Pins.Num() && Copy->Pins.IsValidIndex(Index)) ? Copy->Pins[Index] : nullptr;
	}

	/** Copies a node the way Copy/Paste does. The copy has no links. */
	UEdGraphNode* DuplicateNode(UEdGraph* Graph, UEdGraphNode* Source)
	{
		if (!Source->CanDuplicateNode())
		{
			return nullptr;
		}
		Source->PrepareForCopying();
		TSet<UObject*> ToExport;
		ToExport.Add(Source);
		FString Text;
		FEdGraphUtilities::ExportNodesToText(ToExport, Text);

		TSet<UEdGraphNode*> Imported;
		FEdGraphUtilities::ImportNodesFromText(Graph, Text, Imported);
		if (Imported.Num() != 1)
		{
			for (UEdGraphNode* Node : Imported)
			{
				Graph->RemoveNode(Node);
			}
			return nullptr;
		}
		UEdGraphNode* Copy = *Imported.CreateConstIterator();
		Copy->CreateNewGuid();
		Copy->NodePosX = Source->NodePosX;
		Copy->NodePosY = Source->NodePosY + 80;
		return Copy;
	}

	UK2Node_VariableGet* MakeLocalGetter(UEdGraph* Graph, FName Name, const FString& ScopeName, const FGuid& Guid, const UEdGraphNode* NearNode)
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Getter = Creator.CreateNode(/*bSelectNewNode*/ false);
		Getter->VariableReference.SetLocalMember(Name, ScopeName, Guid);
		Getter->NodePosX = NearNode->NodePosX - 250;
		Getter->NodePosY = NearNode->NodePosY;
		Creator.Finalize();
		return Getter;
	}

	void RemoveCreatedNode(UEdGraph* Graph, UEdGraphNode* Node)
	{
		FBlueprintEditorUtils::RemoveNode(FBlueprintEditorUtils::FindBlueprintForGraph(Graph), Node, /*bDontRecompile*/ true);
	}

	// ---------------------------------------------------------------------------------------------
	// 1. Function parameters -> Get nodes

	void ReplaceParameterWires(UEdGraph* Graph, UEdGraph* FunctionGraph, TArray<UEdGraphNode*>& Scope, TSet<UEdGraphNode*>& ScopeSet, FWireStats& Stats)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Scope)
		{
			if ((Entry = Cast<UK2Node_FunctionEntry>(Node)) != nullptr)
			{
				break;
			}
		}
		if (!Entry)
		{
			return;
		}

		for (UEdGraphPin* ParamPin : Entry->Pins)
		{
			// By-reference parameters keep their wire: a getter would read a copy.
			if (!IsDataPin(ParamPin) || ParamPin->Direction != EGPD_Output || ParamPin->PinType.bIsReference || ParamPin->LinkedTo.Num() == 0)
			{
				continue;
			}
			TMap<UEdGraphNode*, UK2Node_VariableGet*> GetterFor;
			const TArray<UEdGraphPin*> Readers = ParamPin->LinkedTo;
			for (UEdGraphPin* ReaderPin : Readers)
			{
				UEdGraphNode* Reader = ReaderPin ? ReaderPin->GetOwningNode() : nullptr;
				if (!Reader || !ScopeSet.Contains(Reader))
				{
					continue;
				}
				UK2Node_VariableGet* Getter = GetterFor.FindRef(Reader);
				if (!Getter)
				{
					// Function parameters resolve as local members of the function, like local variables.
					Getter = MakeLocalGetter(Graph, ParamPin->PinName, FunctionGraph->GetName(), FGuid(), Reader);
					if (!Getter->GetValuePin() || !Schema->ArePinsCompatible(Getter->GetValuePin(), ReaderPin))
					{
						RemoveCreatedNode(Graph, Getter); // not resolvable (e.g. not compiled since added): keep the wire
						continue;
					}
					GetterFor.Add(Reader, Getter);
					Scope.Add(Getter);
					ScopeSet.Add(Getter);
					++Stats.ParamGetters;
				}
				Entry->Modify();
				Reader->Modify();
				ParamPin->BreakLinkTo(ReaderPin);
				Getter->GetValuePin()->MakeLinkTo(ReaderPin);
			}
		}
	}

	// ---------------------------------------------------------------------------------------------
	// 2. Shared pure nodes -> one copy per exec node that reads them

	void DuplicateSharedPureNodes(UEdGraph* Graph, TArray<UEdGraphNode*>& Scope, TSet<UEdGraphNode*>& ScopeSet, FWireStats& Stats)
	{
		for (int32 Pass = 0; Pass < 32 && Stats.PureCopies < MaxPureCopies; ++Pass)
		{
			bool bChanged = false;
			TMap<UEdGraphNode*, FOwner> Memo;
			const TArray<UEdGraphNode*> Snapshot = Scope;
			for (UEdGraphNode* Pure : Snapshot)
			{
				if (!Pure || IsExecNode(Pure) || Pure->IsA<UK2Node_Knot>() || Pure->IsA<UEdGraphNode_Comment>())
				{
					continue;
				}

				// Group this node's reader links by the exec node each reader feeds.
				TArray<UEdGraphNode*> Keys;
				TMap<UEdGraphNode*, TArray<TPair<UEdGraphPin*, UEdGraphPin*>>> Groups;
				bool bUnresolved = false;
				for (UEdGraphPin* Out : Pure->Pins)
				{
					if (!IsDataPin(Out) || Out->Direction != EGPD_Output)
					{
						continue;
					}
					for (UEdGraphPin* ReaderPin : Out->LinkedTo)
					{
						UEdGraphNode* Reader = ReaderPin ? ReaderPin->GetOwningNode() : nullptr;
						if (!Reader || !ScopeSet.Contains(Reader))
						{
							continue;
						}
						const FOwner Owner = OwnerOf(Reader, ScopeSet, Memo);
						if (Owner.bMulti)
						{
							bUnresolved = true; // split the reader first (a later pass)
							break;
						}
						UEdGraphNode* Key = Owner.Node ? Owner.Node : Reader;
						if (!Groups.Contains(Key))
						{
							Keys.Add(Key);
						}
						Groups.FindOrAdd(Key).Add({ Out, ReaderPin });
					}
					if (bUnresolved)
					{
						break;
					}
				}
				if (bUnresolved || Keys.Num() <= 1)
				{
					continue;
				}

				// The first group keeps the original node; every other group gets its own copy.
				for (int32 k = 1; k < Keys.Num() && Stats.PureCopies < MaxPureCopies; ++k)
				{
					UEdGraphNode* Copy = DuplicateNode(Graph, Pure);
					if (!Copy)
					{
						break;
					}
					Pure->Modify();
					for (UEdGraphPin* In : Pure->Pins)
					{
						if (In && In->Direction == EGPD_Input && In->LinkedTo.Num() > 0)
						{
							if (UEdGraphPin* CopyIn = MatchingPin(Copy, Pure, In))
							{
								for (UEdGraphPin* Source : In->LinkedTo)
								{
									Source->GetOwningNode()->Modify();
									CopyIn->MakeLinkTo(Source);
								}
							}
						}
					}
					for (const TPair<UEdGraphPin*, UEdGraphPin*>& Link : Groups[Keys[k]])
					{
						if (UEdGraphPin* CopyOut = MatchingPin(Copy, Pure, Link.Key))
						{
							Link.Value->GetOwningNode()->Modify();
							Link.Key->BreakLinkTo(Link.Value);
							CopyOut->MakeLinkTo(Link.Value);
						}
					}
					Scope.Add(Copy);
					ScopeSet.Add(Copy);
					++Stats.PureCopies;
					bChanged = true;
				}
				Memo.Reset(); // owners changed
			}
			if (!bChanged)
			{
				break;
			}
		}
	}
}

FWireStats ShortenWires(UEdGraph* Graph, TArray<UEdGraphNode*>& Scope)
{
	FWireStats Stats;
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		return Stats;
	}
	const bool bFunction = Schema->GetGraphType(Graph) == GT_Function;
	UEdGraph* FunctionGraph = FBlueprintEditorUtils::GetTopLevelGraph(Graph);
	TSet<UEdGraphNode*> ScopeSet(Scope);

	Graph->Modify();
	if (bFunction)
	{
		ReplaceParameterWires(Graph, FunctionGraph, Scope, ScopeSet, Stats);
	}
	DuplicateSharedPureNodes(Graph, Scope, ScopeSet, Stats);

	if (Stats.NodesAdded() > 0)
	{
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
	}
	return Stats;
}
}
