#pragma once

#include "CoreMinimal.h"

class SGraphEditor;

namespace BPFormat
{
	/**
	 * Formats the graph shown in the given graph editor.
	 * Scope: no selection = whole graph; one node = its connected group; several = exactly those.
	 * Moves nodes (and refits comments) in one undo transaction; never adds, removes or relinks nodes.
	 */
	void FormatGraph(const TSharedPtr<SGraphEditor>& GraphEditor);
}
