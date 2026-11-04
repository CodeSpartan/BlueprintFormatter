#pragma once

#include "CoreMinimal.h"

class SGraphEditor;

namespace BPFormat
{
	/**
	 * Formats the graph shown in the given graph editor.
	 * Scope: no selection = whole graph; one node = its connected group; several = exactly those.
	 * Moves nodes (and refits comments) in one undo transaction.
	 * First shortens wires (a separate undo step, see BPFormatWires.h): parameter getters and local variables
	 * in functions, and a copy of each shared pure node for every exec node that reads it.
	 */
	void FormatGraph(const TSharedPtr<SGraphEditor>& GraphEditor);
}
