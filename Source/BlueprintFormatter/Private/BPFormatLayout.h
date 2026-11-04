// Layout core for the Blueprint formatter. Works on a plain copy of the graph
// (no UObjects, no widgets), so it is deterministic and safe to run in one call.
//
// Algorithm (flow-following tidy tree):
//  1. Exec nodes form a spanning tree by DFS from the roots, outputs in pin order.
//     Merges and loop back-edges are not tree edges and do not affect placement.
//  2. Every pure node gets one owner: the first exec node (in DFS order) that reads it.
//     It is packed to the left of the node it feeds, with its output pin aligned to
//     the input pin when there is room.
//  3. Each exec node's first exec output continues on the same row (straight wire).
//     Later outputs are pushed down below everything already placed under the node.
//  4. Separate trees (events) are stacked vertically in their original order.
//  5. The result is translated so the first root keeps its original position.

#pragma once

#include "CoreMinimal.h"

namespace BPFormat
{
	struct FNode
	{
		FVector2f OrigPos = FVector2f::ZeroVector;
		FVector2f Size = FVector2f::ZeroVector;
		// Vertical centre of each input / output pin, relative to the node top, in pin order.
		TArray<float> InPinY;
		TArray<float> OutPinY;
		bool bExec = false; // has exec pins: part of the flow, not a pure/data node

		// Output
		FVector2f NewPos = FVector2f::ZeroVector;
	};

	struct FEdge
	{
		int32 From = INDEX_NONE;
		int32 FromPin = 0; // index into From's OutPinY
		int32 To = INDEX_NONE;
		int32 ToPin = 0;   // index into To's InPinY
		bool bExec = false;
	};

	struct FSettings
	{
		float ExecGapX = 80.f;   // between an exec node and the next one
		float PureGapX = 40.f;   // between a pure node and the node it feeds
		float GapY = 24.f;       // minimum vertical gap between nodes
		float TreeGapY = 96.f;   // between separate event trees
	};

	/** Fills NewPos for every node. */
	void Layout(TArray<FNode>& Nodes, const TArray<FEdge>& Edges, const FSettings& Settings);
}
