#include "BPFormatter.h"

#include "BPFormatLayout.h"
#include "BPFormatWires.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphEditor.h"
#include "HAL/PlatformTime.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Containers/Ticker.h"
#include "Layout/Geometry.h"
#include "Layout/ArrangedWidget.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "BlueprintFormatter"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintFormatter, Log, All);

namespace BPFormat
{
namespace
{
	void Notify(const FText& Text, bool bError)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = bError ? 5.f : 2.f;
		Info.bUseSuccessFailIcons = true;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
		}
	}

	bool IsLayoutPin(const UEdGraphPin* Pin)
	{
		// Delegate (event binding) wires are not flow; leave them out of the layout.
		return Pin && !Pin->bHidden && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate;
	}

	FSlateRect NodeRect(const UEdGraphNode* Node, const FVector2f& Size)
	{
		return FSlateRect(Node->NodePosX, Node->NodePosY, Node->NodePosX + Size.X, Node->NodePosY + Size.Y);
	}

	/** Nodes to format, from the current selection. */
	TArray<UEdGraphNode*> GatherScope(UEdGraph* Graph, const FGraphPanelSelectionSet& Selection)
	{
		TArray<UEdGraphNode*> Selected;
		TArray<UEdGraphNode_Comment*> SelectedComments;
		for (UObject* Obj : Selection)
		{
			if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Obj))
			{
				SelectedComments.Add(Comment);
			}
			else if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
			{
				Selected.Add(Node);
			}
		}

		TArray<UEdGraphNode*> Scope;
		auto AddUnique = [&Scope](UEdGraphNode* Node) { Scope.AddUnique(Node); };

		if (Selected.Num() == 0 && SelectedComments.Num() == 0)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !Node->IsA<UEdGraphNode_Comment>())
				{
					AddUnique(Node);
				}
			}
		}
		else if (Selected.Num() == 1 && SelectedComments.Num() == 0)
		{
			// One node: its whole connected group.
			TArray<UEdGraphNode*> Stack = { Selected[0] };
			TSet<UEdGraphNode*> Seen = { Selected[0] };
			while (Stack.Num() > 0)
			{
				UEdGraphNode* Node = Stack.Pop(EAllowShrinking::No);
				AddUnique(Node);
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!IsLayoutPin(Pin))
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
						if (Other && !Seen.Contains(Other))
						{
							Seen.Add(Other);
							Stack.Add(Other);
						}
					}
				}
			}
			// Keep graph order so results do not depend on traversal order.
			Scope.Sort([Graph](const UEdGraphNode& A, const UEdGraphNode& B)
			{
				return Graph->Nodes.IndexOfByKey(&A) < Graph->Nodes.IndexOfByKey(&B);
			});
		}
		else
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Selected.Contains(Node))
				{
					AddUnique(Node);
				}
			}
			// Selected comments pull in the nodes inside them.
			for (UEdGraphNode_Comment* Comment : SelectedComments)
			{
				const FSlateRect Rect(Comment->NodePosX, Comment->NodePosY, Comment->NodePosX + Comment->NodeWidth, Comment->NodePosY + Comment->NodeHeight);
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && !Node->IsA<UEdGraphNode_Comment>() && Rect.ContainsPoint(FVector2f(Node->NodePosX, Node->NodePosY)))
					{
						AddUnique(Node);
					}
				}
			}
		}
		return Scope;
	}

	void TickRecursive(SWidget& Widget, const FGeometry& Geometry)
	{
		Widget.Tick(Geometry, FSlateApplication::Get().GetCurrentTime(), 0.f);
		FArrangedChildren Children(EVisibility::Visible);
		Widget.ArrangeChildren(Geometry, Children);
		for (int32 i = 0; i < Children.Num(); ++i)
		{
			TickRecursive(Children[i].Widget.Get(), Children[i].Geometry);
		}
	}

	/** Arranges Widget's subtree from Geometry and records the geometry of each widget in Find. */
	void FindGeometries(const SWidget& Widget, const FGeometry& Geometry, const TSet<TSharedRef<SWidget>>& Find, TMap<TSharedRef<SWidget>, FArrangedWidget>& Out)
	{
		FArrangedChildren Children(EVisibility::Visible);
		Widget.ArrangeChildren(Geometry, Children);
		for (int32 i = 0; i < Children.Num() && Out.Num() < Find.Num(); ++i)
		{
			const FArrangedWidget& Child = Children[i];
			if (Find.Contains(Child.Widget))
			{
				Out.Add(Child.Widget, Child);
			}
			FindGeometries(Child.Widget.Get(), Child.Geometry, Find, Out);
		}
	}

	struct FMeasured
	{
		FVector2f Size;
		TMap<const UEdGraphPin*, float> PinY; // vertical pin centre relative to node top
	};

	/**
	 * Measures node and pin sizes from the live widgets at 1:1 zoom and full detail.
	 * Pin offsets come from arranging the prepassed widget ourselves, so they do not
	 * depend on whether the node has been painted (culled / off-screen nodes are fine).
	 */
	bool Measure(SGraphEditor& GraphEditor, const TArray<UEdGraphNode*>& Nodes, TMap<const UEdGraphNode*, FMeasured>& Out, FText& OutError, bool& bOutMissingWidget)
	{
		bOutMissingWidget = false;
		SGraphPanel* Panel = GraphEditor.GetGraphPanel();
		if (!Panel)
		{
			OutError = LOCTEXT("NoPanel", "Format: no graph panel found.");
			return false;
		}

		// Zoom level drives the panel LOD; at low LOD pins and titles shrink. Switch to 1:1 for
		// the measurement and restore it right after (all within this call, so nothing repaints).
		FVector2f ViewLocation;
		float ZoomAmount = 1.f;
		GraphEditor.GetViewLocation(ViewLocation, ZoomAmount);
		const bool bChangeZoom = !FMath::IsNearlyEqual(ZoomAmount, 1.f);
		if (bChangeZoom)
		{
			GraphEditor.SetViewLocation(ViewLocation, 1.f);
		}

		bool bOk = true;
		for (UEdGraphNode* Node : Nodes)
		{
			TSharedPtr<SGraphNode> Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid);
			if (!Widget.IsValid())
			{
				OutError = FText::Format(LOCTEXT("NoWidget", "Format: node '{0}' has no widget yet. Try again."), Node->GetNodeTitle(ENodeTitleType::ListView));
				bOutMissingWidget = true;
				bOk = false;
				break;
			}

			// Root the node at its graph position at scale 1, so the pin Tick below caches correct offsets.
			const FVector2f NodePos(Node->NodePosX, Node->NodePosY);
			Widget->SlatePrepass(1.f);
			FMeasured& M = Out.Add(Node);
			M.Size = Widget->GetDesiredSize();
			// Level-of-detail content (pin labels, titles) only swaps on Tick, and culled nodes are not
			// ticked by the panel. Tick the node ourselves at full detail, then measure again.
			TickRecursive(*Widget, FGeometry::MakeRoot(M.Size, FSlateLayoutTransform(NodePos)));
			Widget->SlatePrepass(1.f);
			M.Size = Widget->GetDesiredSize();
			if (M.Size.X < 1.f || M.Size.Y < 1.f)
			{
				OutError = FText::Format(LOCTEXT("ZeroSize", "Format: could not measure node '{0}'. Nothing was moved."), Node->GetNodeTitle(ENodeTitleType::ListView));
				bOk = false;
				break;
			}

			TSet<TSharedRef<SWidget>> PinWidgets;
			TMap<TSharedRef<SWidget>, const UEdGraphPin*> WidgetToPin;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!IsLayoutPin(Pin))
				{
					continue;
				}
				if (TSharedPtr<SGraphPin> PinWidget = Widget->FindWidgetForPin(Pin))
				{
					TSharedRef<SWidget> Ref = PinWidget.ToSharedRef();
					PinWidgets.Add(Ref);
					WidgetToPin.Add(Ref, Pin);
				}
			}
			if (PinWidgets.Num() > 0)
			{
				// Collapsed pins (e.g. hidden advanced pins) are simply not found; they fall back later.
				const FGeometry Root = FGeometry::MakeRoot(M.Size, FSlateLayoutTransform(NodePos));
				TMap<TSharedRef<SWidget>, FArrangedWidget> Arranged;
				FindGeometries(*Widget, Root, PinWidgets, Arranged);
				for (const TPair<TSharedRef<SWidget>, FArrangedWidget>& Pair : Arranged)
				{
					const FGeometry& G = Pair.Value.Geometry;
					const float Top = G.GetAbsolutePosition().Y - NodePos.Y;
					M.PinY.Add(WidgetToPin[Pair.Key], Top + G.GetAbsoluteSize().Y * 0.5f);
				}
			}
		}

		if (bChangeZoom)
		{
			GraphEditor.SetViewLocation(ViewLocation, ZoomAmount);
		}
		return bOk;
	}

	struct FCommentFit
	{
		UEdGraphNode_Comment* Comment = nullptr;
		FSlateRect OrigRect;
		TArray<UEdGraphNode*> Members;
		TArray<int32> ChildComments; // indices into the comment list
	};

	/** Comments whose contents are all being formatted, inner comments first. */
	TArray<FCommentFit> GatherComments(UEdGraph* Graph, const TArray<UEdGraphNode*>& Scope, const TMap<const UEdGraphNode*, FMeasured>& Measured)
	{
		TArray<FCommentFit> Fits;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
			if (!Comment)
			{
				continue;
			}
			FCommentFit Fit;
			Fit.Comment = Comment;
			Fit.OrigRect = FSlateRect(Comment->NodePosX, Comment->NodePosY, Comment->NodePosX + Comment->NodeWidth, Comment->NodePosY + Comment->NodeHeight);
			bool bAllInScope = true;
			for (UEdGraphNode* Other : Graph->Nodes)
			{
				if (!Other || Other->IsA<UEdGraphNode_Comment>())
				{
					continue;
				}
				// Membership by node centre (sizes are only known for nodes in scope).
				const FMeasured* M = Measured.Find(Other);
				const FVector2f Center = FVector2f(Other->NodePosX, Other->NodePosY) + (M ? M->Size * 0.5f : FVector2f::ZeroVector);
				if (Fit.OrigRect.ContainsPoint(Center))
				{
					Fit.Members.Add(Other);
					bAllInScope &= Scope.Contains(Other);
				}
			}
			if (bAllInScope && Fit.Members.Num() > 0)
			{
				Fits.Add(MoveTemp(Fit));
			}
		}

		Fits.Sort([](const FCommentFit& A, const FCommentFit& B)
		{
			return A.OrigRect.GetArea() < B.OrigRect.GetArea();
		});
		for (int32 Outer = 0; Outer < Fits.Num(); ++Outer)
		{
			for (int32 Inner = 0; Inner < Outer; ++Inner)
			{
				const FSlateRect& I = Fits[Inner].OrigRect;
				const FSlateRect& O = Fits[Outer].OrigRect;
				if (I.Left >= O.Left && I.Top >= O.Top && I.Right <= O.Right && I.Bottom <= O.Bottom)
				{
					Fits[Outer].ChildComments.Add(Inner);
				}
			}
		}
		return Fits;
	}

	/** Measures and arranges Scope. Returns false (and touches nothing) when a node widget is not built yet. */
	bool LayoutScope(const TSharedPtr<SGraphEditor>& GraphEditor, UEdGraph* Graph, const TArray<UEdGraphNode*>& Scope, double StartTime, bool bReportMissingWidget);

	/** New getter nodes get their widgets on a later frame, so retry the layout for a short while. */
	void ScheduleLayout(TWeakPtr<SGraphEditor> WeakEditor, TWeakObjectPtr<UEdGraph> WeakGraph, TArray<TWeakObjectPtr<UEdGraphNode>> WeakScope, double StartTime)
	{
		TSharedRef<int32> Attempts = MakeShared<int32>(0);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakEditor, WeakGraph, WeakScope, StartTime, Attempts](float)
		{
			TSharedPtr<SGraphEditor> Editor = WeakEditor.Pin();
			UEdGraph* Graph = WeakGraph.Get();
			if (!Editor.IsValid() || !Graph || Editor->GetCurrentGraph() != Graph)
			{
				return false;
			}
			TArray<UEdGraphNode*> Scope;
			for (const TWeakObjectPtr<UEdGraphNode>& Weak : WeakScope)
			{
				if (UEdGraphNode* Node = Weak.Get())
				{
					Scope.Add(Node);
				}
			}
			const bool bLastTry = ++(*Attempts) >= 30;
			const bool bDone = LayoutScope(Editor, Graph, Scope, StartTime, bLastTry);
			return !bDone && !bLastTry; // true = keep ticking
		}));
	}
}

void FormatGraph(const TSharedPtr<SGraphEditor>& GraphEditor)
{
	UEdGraph* Graph = GraphEditor.IsValid() ? GraphEditor->GetCurrentGraph() : nullptr;
	if (!Graph)
	{
		Notify(LOCTEXT("NoEditor", "Format: no graph is open."), true);
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	TArray<UEdGraphNode*> Scope = GatherScope(Graph, GraphEditor->GetSelectedNodes());
	if (Scope.Num() == 0)
	{
		return;
	}

	FWireStats Stats;
	{
		FScopedTransaction Transaction(LOCTEXT("ShortenWiresTransaction", "Format Graph: Shorten Wires"));
		Stats = ShortenWires(Graph, Scope); // appends the nodes it adds to Scope
		if (Stats.NodesAdded() == 0)
		{
			Transaction.Cancel();
		}
	}

	if (Stats.NodesAdded() == 0)
	{
		LayoutScope(GraphEditor, Graph, Scope, StartTime, /*bReportMissingWidget*/ true);
		return;
	}

	UE_LOG(LogBlueprintFormatter, Log, TEXT("Shortened wires: %d parameter getters, %d pure node copies, %d local variables (%d getters)"),
		Stats.ParamGetters, Stats.PureCopies, Stats.LocalVariables, Stats.LocalGetters);

	// The panel builds widgets for the new nodes on its next update; lay out once they exist.
	Graph->NotifyGraphChanged();
	TArray<TWeakObjectPtr<UEdGraphNode>> WeakScope;
	for (UEdGraphNode* Node : Scope)
	{
		WeakScope.Add(Node);
	}
	ScheduleLayout(GraphEditor, Graph, MoveTemp(WeakScope), StartTime);
}

namespace
{
bool LayoutScope(const TSharedPtr<SGraphEditor>& GraphEditor, UEdGraph* Graph, const TArray<UEdGraphNode*>& Scope, double StartTime, bool bReportMissingWidget)
{
	TMap<const UEdGraphNode*, FMeasured> Measured;
	FText Error;
	bool bMissingWidget = false;
	if (!Measure(*GraphEditor, Scope, Measured, Error, bMissingWidget))
	{
		if (!bMissingWidget || bReportMissingWidget)
		{
			Notify(Error, true);
			return true; // give up
		}
		return false; // retry next frame
	}

	// Copy into the plain layout graph.
	TMap<const UEdGraphNode*, int32> IndexOf;
	TArray<FNode> Nodes;
	TMap<const UEdGraphPin*, int32> PinIndex; // index among the node's input or output layout pins
	for (UEdGraphNode* Node : Scope)
	{
		const FMeasured& M = Measured[Node];
		FNode& N = Nodes.AddDefaulted_GetRef();
		IndexOf.Add(Node, Nodes.Num() - 1);
		N.OrigPos = FVector2f(Node->NodePosX, Node->NodePosY);
		N.Size = M.Size;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsLayoutPin(Pin))
			{
				continue;
			}
			N.bExec |= UEdGraphSchema_K2::IsExecPin(*Pin);
			const float* Y = M.PinY.Find(Pin);
			TArray<float>& List = Pin->Direction == EGPD_Input ? N.InPinY : N.OutPinY;
			PinIndex.Add(Pin, List.Num());
			List.Add(Y ? *Y : M.Size.Y * 0.5f);
		}
	}

	TArray<FEdge> Edges;
	for (UEdGraphNode* Node : Scope)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsLayoutPin(Pin) || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!IsLayoutPin(Linked))
				{
					continue;
				}
				const int32* To = IndexOf.Find(Linked->GetOwningNode());
				if (!To)
				{
					continue;
				}
				FEdge& E = Edges.AddDefaulted_GetRef();
				E.From = IndexOf[Node];
				E.FromPin = PinIndex[Pin];
				E.To = *To;
				E.ToPin = PinIndex[Linked];
				E.bExec = UEdGraphSchema_K2::IsExecPin(*Pin) && UEdGraphSchema_K2::IsExecPin(*Linked);
			}
		}
	}

	TArray<FCommentFit> Comments = GatherComments(Graph, Scope, Measured);

	Layout(Nodes, Edges, FSettings());

	// Write back. Positions only: links are never touched.
	FScopedTransaction Transaction(LOCTEXT("FormatTransaction", "Format Graph"));
	Graph->Modify();

	TMap<const UEdGraphNode*, FSlateRect> NewRects;
	int32 Moved = 0;
	for (int32 i = 0; i < Scope.Num(); ++i)
	{
		UEdGraphNode* Node = Scope[i];
		const int32 X = FMath::RoundToInt(Nodes[i].NewPos.X);
		const int32 Y = FMath::RoundToInt(Nodes[i].NewPos.Y);
		if (X != Node->NodePosX || Y != Node->NodePosY)
		{
			Node->Modify();
			Node->NodePosX = X;
			Node->NodePosY = Y;
			++Moved;
		}
		NewRects.Add(Node, NodeRect(Node, Nodes[i].Size));
	}

	// Refit comments around their members, inner comments first.
	TArray<FSlateRect> CommentRects;
	CommentRects.SetNum(Comments.Num());
	bool bResizedComment = false;
	for (int32 c = 0; c < Comments.Num(); ++c)
	{
		FCommentFit& Fit = Comments[c];
		TOptional<FSlateRect> Bounds;
		auto Expand = [&Bounds](const FSlateRect& R) { Bounds = Bounds.IsSet() ? Bounds->Expand(R) : R; };
		for (UEdGraphNode* Member : Fit.Members)
		{
			Expand(NewRects[Member]);
		}
		for (int32 Child : Fit.ChildComments)
		{
			Expand(CommentRects[Child]);
		}
		const float Side = 24.f;
		const float Top = 2.f * Fit.Comment->GetFontSize() + 16.f; // room for the title bar
		const FSlateRect R(Bounds->Left - Side, Bounds->Top - Top, Bounds->Right + Side, Bounds->Bottom + Side);
		CommentRects[c] = R;
		if (!R.ToString().Equals(Fit.OrigRect.ToString()))
		{
			Fit.Comment->Modify();
			Fit.Comment->SetBounds(R);
			bResizedComment = true;
		}
	}

	if (Moved == 0 && !bResizedComment)
	{
		Transaction.Cancel();
	}
	if (bResizedComment)
	{
		// Comment widgets read their size only when constructed.
		Graph->NotifyGraphChanged();
	}

	UE_LOG(LogBlueprintFormatter, Log, TEXT("Formatted %d nodes (%d moved, %d edges, %d comments) in %.1f ms"),
		Scope.Num(), Moved, Edges.Num(), Comments.Num(), (FPlatformTime::Seconds() - StartTime) * 1000.0);
	return true;
}
}
}

#undef LOCTEXT_NAMESPACE
