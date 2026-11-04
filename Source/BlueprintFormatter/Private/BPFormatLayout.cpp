#include "BPFormatLayout.h"

namespace BPFormat
{
namespace
{
	struct FItem
	{
		int32 Node;
		FVector2f Pos;
	};
	using FShape = TArray<FItem>;

	struct FTreeChild
	{
		int32 Child;
		int32 ParentPin; // output pin on the parent (exec) / input pin on the consumer (pure)
		int32 ChildPin;  // input pin on the child (exec) / output pin on the source (pure)
	};

	// Horizontal overlap allowance: rects closer than this in x count as overlapping columns.
	constexpr float ColumnMargin = 8.f;

	class FLayouter
	{
	public:
		FLayouter(TArray<FNode>& InNodes, const TArray<FEdge>& InEdges, const FSettings& InSettings)
			: Nodes(InNodes), Edges(InEdges), Settings(InSettings)
		{
		}

		void Run()
		{
			const int32 Num = Nodes.Num();
			ExecOut.SetNum(Num);
			DataIn.SetNum(Num);
			ExecInCount.Init(0, Num);
			TreeChildren.SetNum(Num);
			PureChildren.SetNum(Num);
			Visited.Init(false, Num);
			Claimed.Init(false, Num);

			BuildAdjacency();

			// 1. Exec spanning trees.
			TArray<int32> ExecRoots;
			for (int32 i = 0; i < Num; ++i)
			{
				if (Nodes[i].bExec && ExecInCount[i] == 0)
				{
					ExecRoots.Add(i);
				}
			}
			SortByOrigPos(ExecRoots);
			for (int32 Root : ExecRoots)
			{
				Dfs(Root);
			}
			// Exec nodes only reachable through a cycle: start extra trees in original order.
			TArray<int32> Remaining;
			for (int32 i = 0; i < Num; ++i)
			{
				if (Nodes[i].bExec && !Visited[i])
				{
					Remaining.Add(i);
				}
			}
			SortByOrigPos(Remaining);
			for (int32 Node : Remaining)
			{
				if (!Visited[Node])
				{
					ExecRoots.Add(Node);
					Dfs(Node);
				}
			}

			// 2. Pure ownership, in DFS preorder so the earliest consumer wins.
			for (int32 Node : Preorder)
			{
				ClaimPureInputs(Node);
			}
			// Pure nodes that feed no exec node: their sinks become roots of their own blocks.
			TArray<int32> PureRoots;
			for (int32 i = 0; i < Num; ++i)
			{
				if (!Nodes[i].bExec && !Claimed[i] && !FeedsUnclaimedPure(i))
				{
					PureRoots.Add(i);
				}
			}
			SortByOrigPos(PureRoots);
			for (int32 Root : PureRoots)
			{
				Claimed[Root] = true;
				ClaimPureInputs(Root);
			}
			// Anything left sits on a data cycle; give each leftover its own block.
			TArray<int32> Leftover;
			for (int32 i = 0; i < Num; ++i)
			{
				if (!Claimed[i] && !Nodes[i].bExec)
				{
					Leftover.Add(i);
				}
			}
			SortByOrigPos(Leftover);
			for (int32 Node : Leftover)
			{
				if (!Claimed[Node])
				{
					Claimed[Node] = true;
					PureRoots.Add(Node);
					ClaimPureInputs(Node);
				}
			}

			// 3. Lay out each tree, then stack the trees.
			TArray<TPair<int32, FShape>> Trees;
			for (int32 Root : ExecRoots)
			{
				Trees.Emplace(Root, LayoutExec(Root));
			}
			for (int32 Root : PureRoots)
			{
				Trees.Emplace(Root, LayoutPureBlock(Root));
			}
			StackTrees(Trees);
		}

	private:
		TArray<FNode>& Nodes;
		const TArray<FEdge>& Edges;
		const FSettings& Settings;

		TArray<TArray<int32>> ExecOut; // edge indices, sorted
		TArray<TArray<int32>> DataIn;  // edge indices, sorted
		TArray<int32> ExecInCount;
		TArray<TArray<FTreeChild>> TreeChildren;
		TArray<TArray<FTreeChild>> PureChildren;
		TArray<bool> Visited;
		TArray<bool> Claimed;
		TArray<int32> Preorder;

		bool OrigLess(int32 A, int32 B) const
		{
			const FVector2f& PA = Nodes[A].OrigPos;
			const FVector2f& PB = Nodes[B].OrigPos;
			if (PA.Y != PB.Y) return PA.Y < PB.Y;
			if (PA.X != PB.X) return PA.X < PB.X;
			return A < B;
		}

		void SortByOrigPos(TArray<int32>& List) const
		{
			List.Sort([this](int32 A, int32 B) { return OrigLess(A, B); });
		}

		void BuildAdjacency()
		{
			for (int32 e = 0; e < Edges.Num(); ++e)
			{
				const FEdge& Edge = Edges[e];
				if (Edge.From == Edge.To)
				{
					continue;
				}
				if (Edge.bExec)
				{
					ExecOut[Edge.From].Add(e);
					++ExecInCount[Edge.To];
				}
				else
				{
					DataIn[Edge.To].Add(e);
				}
			}
			for (TArray<int32>& List : ExecOut)
			{
				List.Sort([this](int32 A, int32 B)
				{
					const FEdge& EA = Edges[A];
					const FEdge& EB = Edges[B];
					if (EA.FromPin != EB.FromPin) return EA.FromPin < EB.FromPin;
					if (EA.To != EB.To) return OrigLess(EA.To, EB.To);
					return EA.ToPin < EB.ToPin;
				});
			}
			for (TArray<int32>& List : DataIn)
			{
				List.Sort([this](int32 A, int32 B)
				{
					const FEdge& EA = Edges[A];
					const FEdge& EB = Edges[B];
					if (EA.ToPin != EB.ToPin) return EA.ToPin < EB.ToPin;
					if (EA.From != EB.From) return OrigLess(EA.From, EB.From);
					return EA.FromPin < EB.FromPin;
				});
			}
		}

		void Dfs(int32 Node)
		{
			Visited[Node] = true;
			Preorder.Add(Node);
			for (int32 e : ExecOut[Node])
			{
				const FEdge& Edge = Edges[e];
				if (!Visited[Edge.To])
				{
					TreeChildren[Node].Add({ Edge.To, Edge.FromPin, Edge.ToPin });
					Dfs(Edge.To);
				}
			}
		}

		void ClaimPureInputs(int32 Consumer)
		{
			for (int32 e : DataIn[Consumer])
			{
				const FEdge& Edge = Edges[e];
				const int32 Source = Edge.From;
				if (!Nodes[Source].bExec && !Claimed[Source])
				{
					Claimed[Source] = true;
					PureChildren[Consumer].Add({ Source, Edge.ToPin, Edge.FromPin });
					ClaimPureInputs(Source);
				}
			}
		}

		bool FeedsUnclaimedPure(int32 Node) const
		{
			for (const FEdge& Edge : Edges)
			{
				if (Edge.From == Node && Edge.To != Node && !Edge.bExec && !Nodes[Edge.To].bExec && !Claimed[Edge.To])
				{
					return true;
				}
			}
			return false;
		}

		static float PinY(const TArray<float>& Pins, int32 Index, float Fallback)
		{
			return Pins.IsValidIndex(Index) ? Pins[Index] : Fallback;
		}

		/** Smallest dy >= StartDy so that Sub (offset by Dx, dy) overlaps nothing in Placed. Only moves down. */
		float PushDown(const FShape& Sub, float Dx, float StartDy, const FShape& Placed) const
		{
			float Dy = StartDy;
			for (int32 Guard = 0; Guard < 10000; ++Guard)
			{
				float Needed = 0.f;
				for (const FItem& A : Sub)
				{
					const FVector2f& SizeA = Nodes[A.Node].Size;
					const float AX0 = A.Pos.X + Dx;
					const float AX1 = AX0 + SizeA.X;
					const float AY0 = A.Pos.Y + Dy;
					const float AY1 = AY0 + SizeA.Y;
					for (const FItem& B : Placed)
					{
						const FVector2f& SizeB = Nodes[B.Node].Size;
						const float BX0 = B.Pos.X;
						const float BX1 = BX0 + SizeB.X;
						if (AX0 >= BX1 + ColumnMargin || BX0 >= AX1 + ColumnMargin)
						{
							continue;
						}
						const float BY0 = B.Pos.Y;
						const float BY1 = BY0 + SizeB.Y;
						if (AY0 < BY1 + Settings.GapY && BY0 < AY1 + Settings.GapY)
						{
							Needed = FMath::Max(Needed, BY1 + Settings.GapY - AY0);
						}
					}
				}
				if (Needed <= 0.f)
				{
					break;
				}
				Dy += Needed;
			}
			return Dy;
		}

		static void Append(FShape& Into, const FShape& Sub, float Dx, float Dy)
		{
			Into.Reserve(Into.Num() + Sub.Num());
			for (const FItem& It : Sub)
			{
				Into.Add({ It.Node, It.Pos + FVector2f(Dx, Dy) });
			}
		}

		/** Consumer at (0,0), its owned pure inputs packed to the left. */
		FShape LayoutPureBlock(int32 Consumer)
		{
			const FNode& C = Nodes[Consumer];
			FShape Shape;
			Shape.Add({ Consumer, FVector2f::ZeroVector });
			for (const FTreeChild& PC : PureChildren[Consumer])
			{
				const FNode& P = Nodes[PC.Child];
				const FShape Sub = LayoutPureBlock(PC.Child);
				const float Dx = -Settings.PureGapX - P.Size.X;
				const float WantDy = PinY(C.InPinY, PC.ParentPin, C.Size.Y * 0.5f) - PinY(P.OutPinY, PC.ChildPin, P.Size.Y * 0.5f);
				const float Dy = PushDown(Sub, Dx, WantDy, Shape);
				Append(Shape, Sub, Dx, Dy);
			}
			return Shape;
		}

		/** Exec node at (0,0) with its pure block and its whole exec subtree. */
		FShape LayoutExec(int32 Node)
		{
			const FNode& U = Nodes[Node];
			FShape Shape = LayoutPureBlock(Node);
			for (const FTreeChild& TC : TreeChildren[Node])
			{
				const FNode& V = Nodes[TC.Child];
				const FShape Sub = LayoutExec(TC.Child);
				float MinX = 0.f;
				for (const FItem& It : Sub)
				{
					MinX = FMath::Min(MinX, It.Pos.X);
				}
				const float Dx = U.Size.X + Settings.ExecGapX - MinX;
				const float WantDy = PinY(U.OutPinY, TC.ParentPin, U.Size.Y * 0.5f) - PinY(V.InPinY, TC.ChildPin, V.Size.Y * 0.5f);
				const float Dy = PushDown(Sub, Dx, WantDy, Shape);
				Append(Shape, Sub, Dx, Dy);
			}
			return Shape;
		}

		void StackTrees(const TArray<TPair<int32, FShape>>& Trees)
		{
			if (Trees.Num() == 0)
			{
				return;
			}

			// Left-align every tree's bounding box; stack downward in order.
			float CursorY = 0.f;
			float FirstRootDx = 0.f, FirstRootDy = 0.f;
			for (int32 t = 0; t < Trees.Num(); ++t)
			{
				const FShape& Shape = Trees[t].Value;
				FVector2f Min(TNumericLimits<float>::Max()), Max(-TNumericLimits<float>::Max());
				for (const FItem& It : Shape)
				{
					Min = FVector2f::Min(Min, It.Pos);
					Max = FVector2f::Max(Max, It.Pos + Nodes[It.Node].Size);
				}
				const float Dx = -Min.X;
				const float Dy = CursorY - Min.Y;
				for (const FItem& It : Shape)
				{
					Nodes[It.Node].NewPos = It.Pos + FVector2f(Dx, Dy);
				}
				if (t == 0)
				{
					FirstRootDx = Dx;
					FirstRootDy = Dy;
				}
				CursorY += (Max.Y - Min.Y) + Settings.TreeGapY;
			}

			// The first root node keeps its original position.
			const int32 FirstRoot = Trees[0].Key;
			const FVector2f Offset = Nodes[FirstRoot].OrigPos - Nodes[FirstRoot].NewPos;
			for (FNode& N : Nodes)
			{
				N.NewPos += Offset;
			}
		}
	};
}

void Layout(TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const FSettings& Settings)
{
	for (FNode& N : Nodes)
	{
		N.NewPos = N.OrigPos;
	}
	if (Nodes.Num() == 0)
	{
		return;
	}
	FLayouter(Nodes, Edges, Settings).Run();
}
}
