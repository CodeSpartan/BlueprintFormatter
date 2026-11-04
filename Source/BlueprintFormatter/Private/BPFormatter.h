#pragma once

#include "CoreMinimal.h"

class SGraphEditor;

namespace BPFormat
{
	/**
	 * Formats the graph shown in the given graph editor.
	 * Scope: no selection = whole graph; one node = its connected group; several = exactly those.
	 * First shortens wires (see BPFormatWires.h): parameter getters in functions, and a copy of each shared
	 * pure node for every exec node that reads it. Then moves nodes and refits comments.
	 * Everything is one undo transaction. It never creates variables or deletes nodes.
	 */
	void FormatGraph(const TSharedPtr<SGraphEditor>& GraphEditor);
}
