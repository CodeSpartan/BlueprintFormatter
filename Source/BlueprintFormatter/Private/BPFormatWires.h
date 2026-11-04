// Wire shortening for the Blueprint formatter. Runs before layout and rewrites the graph so data wires stay short:
//  1. Function graphs: wires from the function entry's parameter pins become "Get <Parameter>" nodes, one per reader.
//  2. Any graph: a pure node read by several exec nodes is duplicated so each exec node gets its own copy.
//     This does not change behaviour: Blueprint re-evaluates pure nodes for every exec node that reads them.
//  3. Function graphs: an exec node output read two or more exec steps later is stored in a new local variable
//     (a Set node right after the producing exec pin) and read back with "Get" nodes beside each far reader.
// Event graphs have no local variables, so step 3 is skipped there.

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
		int32 LocalVariables = 0;
		int32 LocalGetters = 0;

		int32 NodesAdded() const { return ParamGetters + PureCopies + LocalVariables + LocalGetters; }
	};

	/** Rewrites wires inside Scope (see above). New nodes are appended to Scope. Call inside a transaction. */
	FWireStats ShortenWires(UEdGraph* Graph, TArray<UEdGraphNode*>& Scope);
}
