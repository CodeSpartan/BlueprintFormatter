// Wire shortening for the Blueprint formatter. Runs before layout and rewrites the graph so data wires stay short:
//  1. Function graphs: wires from the function entry's parameter pins become "Get <Parameter>" nodes, one per reader.
//  2. Any graph: a pure node read by several exec nodes is duplicated so each exec node gets its own copy.
//     This does not change behaviour: Blueprint re-evaluates pure nodes for every exec node that reads them.
// It never creates variables: a new variable could hold a stale value on a path that skips its Set node.

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;

namespace BPFormat
{
	struct FWireStats
	{
		int32 ParamGetters = 0;
		int32 PureCopies = 0;

		int32 NodesAdded() const { return ParamGetters + PureCopies; }
	};

	/** Rewrites wires inside Scope (see above). New nodes are appended to Scope. Call inside a transaction. */
	FWireStats ShortenWires(UEdGraph* Graph, TArray<UEdGraphNode*>& Scope);
}
