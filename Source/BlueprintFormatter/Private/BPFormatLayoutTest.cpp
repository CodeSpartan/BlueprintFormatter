#include "BPFormatLayout.h"

#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace BPFormatTest
{
	using namespace BPFormat;

	int32 AddNode(TArray<BPFormat::FNode>& Nodes, FVector2f Pos, FVector2f Size, TArray<float> In, TArray<float> Out, bool bExec)
	{
		BPFormat::FNode& N = Nodes.AddDefaulted_GetRef();
		N.OrigPos = Pos;
		N.Size = Size;
		N.InPinY = MoveTemp(In);
		N.OutPinY = MoveTemp(Out);
		N.bExec = bExec;
		return Nodes.Num() - 1;
	}

	void AddEdge(TArray<BPFormat::FEdge>& Edges, int32 From, int32 FromPin, int32 To, int32 ToPin, bool bExec)
	{
		Edges.Add({ From, FromPin, To, ToPin, bExec });
	}

	FBox2f Rect(const BPFormat::FNode& N)
	{
		return FBox2f(N.NewPos, N.NewPos + N.Size);
	}

	bool CheckNoOverlap(FAutomationTestBase& Test, const TArray<BPFormat::FNode>& Nodes)
	{
		for (int32 a = 0; a < Nodes.Num(); ++a)
		{
			for (int32 b = a + 1; b < Nodes.Num(); ++b)
			{
				const FBox2f A = Rect(Nodes[a]);
				const FBox2f B = Rect(Nodes[b]);
				if (A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y)
				{
					Test.AddError(FString::Printf(TEXT("Nodes %d and %d overlap"), a, b));
					return false;
				}
			}
		}
		return true;
	}

	/** Runs the layout a second time from its own output; nothing should move. */
	void CheckIdempotent(FAutomationTestBase& Test, TArray<BPFormat::FNode> Nodes, const TArray<BPFormat::FEdge>& Edges)
	{
		for (BPFormat::FNode& N : Nodes)
		{
			N.OrigPos = FVector2f(FMath::RoundToFloat(N.NewPos.X), FMath::RoundToFloat(N.NewPos.Y));
		}
		TArray<BPFormat::FNode> Again = Nodes;
		Layout(Again, Edges, FSettings());
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			if (!Again[i].NewPos.Equals(Nodes[i].OrigPos, 1.01f))
			{
				Test.AddError(FString::Printf(TEXT("Second format moved node %d from (%.0f,%.0f) to (%.0f,%.0f)"), i,
					Nodes[i].OrigPos.X, Nodes[i].OrigPos.Y, Again[i].NewPos.X, Again[i].NewPos.Y));
				return;
			}
		}
	}

	float OutPinAbs(const BPFormat::FNode& N, int32 Pin) { return N.NewPos.Y + N.OutPinY[Pin]; }
	float InPinAbs(const BPFormat::FNode& N, int32 Pin) { return N.NewPos.Y + N.InPinY[Pin]; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPFormatLayoutBasicTest, "BlueprintFormatter.Layout.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBPFormatLayoutBasicTest::RunTest(const FString& Parameters)
{
	using namespace BPFormatTest;
	TArray<BPFormat::FNode> Nodes;
	TArray<BPFormat::FEdge> Edges;

	// Event -> Branch(cond = GetVar) -> True: PrintA(Add(GetX, GetY)) -> Delay ; False: PrintB -> Delay (merge)
	const int32 Event = AddNode(Nodes, { 900, 700 }, { 150, 60 }, {}, { 30 }, true);
	const int32 Branch = AddNode(Nodes, { 10, -400 }, { 140, 80 }, { 30, 55 }, { 30, 55 }, true);
	const int32 PrintA = AddNode(Nodes, { -300, 50 }, { 180, 90 }, { 30, 60 }, { 30 }, true);
	const int32 PrintB = AddNode(Nodes, { 600, 20 }, { 180, 90 }, { 30, 60 }, { 30 }, true);
	const int32 GetVar = AddNode(Nodes, { 0, 0 }, { 120, 40 }, {}, { 20 }, false);
	const int32 Add = AddNode(Nodes, { 50, 900 }, { 100, 60 }, { 20, 40 }, { 30 }, false);
	const int32 GetX = AddNode(Nodes, { 70, 300 }, { 90, 40 }, {}, { 20 }, false);
	const int32 GetY = AddNode(Nodes, { 70, 350 }, { 90, 40 }, {}, { 20 }, false);
	const int32 Delay = AddNode(Nodes, { 0, 100 }, { 160, 100 }, { 30, 60 }, { 30 }, true);
	const int32 Event2 = AddNode(Nodes, { 900, 1500 }, { 150, 60 }, {}, { 30 }, true);
	const int32 PrintC = AddNode(Nodes, { 0, 0 }, { 180, 90 }, { 30, 60 }, { 30 }, true);
	const int32 Shared = AddNode(Nodes, { 0, 0 }, { 120, 40 }, {}, { 20 }, false); // feeds PrintB and PrintC

	AddEdge(Edges, Event, 0, Branch, 0, true);
	AddEdge(Edges, Branch, 0, PrintA, 0, true);
	AddEdge(Edges, Branch, 1, PrintB, 0, true);
	AddEdge(Edges, GetVar, 0, Branch, 1, false);
	AddEdge(Edges, Add, 0, PrintA, 1, false);
	AddEdge(Edges, GetX, 0, Add, 0, false);
	AddEdge(Edges, GetY, 0, Add, 1, false);
	AddEdge(Edges, PrintA, 0, Delay, 0, true);
	AddEdge(Edges, PrintB, 0, Delay, 0, true);
	AddEdge(Edges, Event2, 0, PrintC, 0, true);
	AddEdge(Edges, Shared, 0, PrintB, 1, false);
	AddEdge(Edges, Shared, 0, PrintC, 1, false);

	BPFormat::Layout(Nodes, Edges, BPFormat::FSettings());

	CheckNoOverlap(*this, Nodes);
	TestTrue(TEXT("First event keeps its position"), Nodes[Event].NewPos.Equals(Nodes[Event].OrigPos));
	TestEqual(TEXT("Event->Branch wire is straight"), OutPinAbs(Nodes[Event], 0), InPinAbs(Nodes[Branch], 0));
	TestEqual(TEXT("Branch True->PrintA wire is straight"), OutPinAbs(Nodes[Branch], 0), InPinAbs(Nodes[PrintA], 0));
	TestEqual(TEXT("PrintA->Delay wire is straight"), OutPinAbs(Nodes[PrintA], 0), InPinAbs(Nodes[Delay], 0));
	TestTrue(TEXT("False branch is below True branch"), Nodes[PrintB].NewPos.Y > Nodes[PrintA].NewPos.Y + Nodes[PrintA].Size.Y);
	TestTrue(TEXT("Flow goes left to right"), Nodes[Branch].NewPos.X > Nodes[Event].NewPos.X + Nodes[Event].Size.X
		&& Nodes[PrintA].NewPos.X > Nodes[Branch].NewPos.X + Nodes[Branch].Size.X);
	TestTrue(TEXT("GetVar sits left of Branch"), Nodes[GetVar].NewPos.X + Nodes[GetVar].Size.X < Nodes[Branch].NewPos.X);
	TestTrue(TEXT("GetVar is between Event and Branch"), Nodes[GetVar].NewPos.X > Nodes[Event].NewPos.X + Nodes[Event].Size.X);
	TestTrue(TEXT("Add sits left of PrintA"), Nodes[Add].NewPos.X + Nodes[Add].Size.X < Nodes[PrintA].NewPos.X);
	TestTrue(TEXT("GetX sits left of Add"), Nodes[GetX].NewPos.X + Nodes[GetX].Size.X < Nodes[Add].NewPos.X);
	TestTrue(TEXT("GetY is below GetX"), Nodes[GetY].NewPos.Y > Nodes[GetX].NewPos.Y);
	TestTrue(TEXT("Shared pure node is placed with its first consumer (PrintB)"), Nodes[Shared].NewPos.X + Nodes[Shared].Size.X < Nodes[PrintB].NewPos.X
		&& Nodes[Shared].NewPos.Y < Nodes[Event2].NewPos.Y);

	float Tree1Bottom = 0.f;
	for (int32 i : { Event, Branch, PrintA, PrintB, GetVar, Add, GetX, GetY, Delay, Shared })
	{
		Tree1Bottom = FMath::Max(Tree1Bottom, Nodes[i].NewPos.Y + Nodes[i].Size.Y);
	}
	TestTrue(TEXT("Second event tree is stacked below the first"), Nodes[Event2].NewPos.Y > Tree1Bottom && Nodes[PrintC].NewPos.Y > Tree1Bottom);

	CheckIdempotent(*this, Nodes, Edges);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBPFormatLayoutRandomTest, "BlueprintFormatter.Layout.Random",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBPFormatLayoutRandomTest::RunTest(const FString& Parameters)
{
	using namespace BPFormatTest;
	for (int32 Seed = 1; Seed <= 20; ++Seed)
	{
		FRandomStream Rng(Seed);
		TArray<BPFormat::FNode> Nodes;
		TArray<BPFormat::FEdge> Edges;
		const int32 Num = 400;
		for (int32 i = 0; i < Num; ++i)
		{
			const bool bExec = Rng.FRand() < 0.55f;
			const int32 NumIn = Rng.RandRange(bExec ? 1 : 0, 4);
			const int32 NumOut = Rng.RandRange(1, bExec ? 3 : 2);
			TArray<float> In, Out;
			for (int32 p = 0; p < NumIn; ++p) In.Add(30.f + 22.f * p);
			for (int32 p = 0; p < NumOut; ++p) Out.Add(30.f + 22.f * p);
			const float H = 40.f + 22.f * FMath::Max(NumIn, NumOut);
			AddNode(Nodes, { Rng.FRandRange(-3000, 3000), Rng.FRandRange(-3000, 3000) }, { Rng.FRandRange(60, 320), H }, In, Out, bExec);
		}
		TArray<TArray<bool>> InUsed;
		InUsed.SetNum(Num);
		for (int32 i = 0; i < Num; ++i) InUsed[i].Init(false, Nodes[i].InPinY.Num());
		for (int32 i = 0; i < Num * 2; ++i)
		{
			const int32 A = Rng.RandRange(0, Num - 1);
			const int32 B = Rng.RandRange(0, Num - 1);
			if (A == B || Nodes[B].InPinY.Num() == 0)
			{
				continue;
			}
			// Exec edges: exec pin 0 to exec pin 0 (allows merges and cycles); data otherwise.
			const bool bExec = Nodes[A].bExec && Nodes[B].bExec && Rng.FRand() < 0.6f;
			const int32 ToPin = bExec ? 0 : Rng.RandRange(0, Nodes[B].InPinY.Num() - 1);
			if (!bExec && InUsed[B][ToPin])
			{
				continue; // a data input has at most one link
			}
			InUsed[B][ToPin] = InUsed[B][ToPin] || !bExec;
			AddEdge(Edges, A, Rng.RandRange(0, Nodes[A].OutPinY.Num() - 1), B, ToPin, bExec);
		}

		const double Start = FPlatformTime::Seconds();
		BPFormat::Layout(Nodes, Edges, BPFormat::FSettings());
		const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
		AddInfo(FString::Printf(TEXT("Seed %d: %d nodes, %d edges, %.1f ms"), Seed, Num, Edges.Num(), Ms));

		for (const BPFormat::FNode& N : Nodes)
		{
			if (!FMath::IsFinite(N.NewPos.X) || !FMath::IsFinite(N.NewPos.Y))
			{
				AddError(TEXT("Non-finite position"));
				return false;
			}
		}
		if (!CheckNoOverlap(*this, Nodes))
		{
			return false;
		}
		CheckIdempotent(*this, Nodes, Edges);
	}
	return true;
}

#endif
