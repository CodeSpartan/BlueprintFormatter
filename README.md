# Blueprint Formatter

One-click auto-layout for Unreal Engine Blueprint graphs. Adds a **Format** button to the Blueprint editor toolbar.

![Before and after formatting](Docs/before-after.png)

## What it does

- The main exec flow runs in a straight line from left to right, with wires aligned pin to pin.
- Extra outputs of Branch, Sequence, Switch, FlipFlop and loop nodes stack below the main line.
- Pure nodes (getters, math) are packed just left of the node that uses them.
- Separate events are stacked vertically. The first event stays where it was.
- Comment boxes are refit around the nodes they contain.

Before the layout runs, it shortens long data wires:

- **Function parameters:** wires from the function entry become **Get &lt;Parameter&gt;** nodes beside each node that reads them. By-reference parameters keep their wire.
- **Shared pure nodes:** a pure node that feeds several exec nodes is copied, so each exec node gets its own copy. Behaviour is unchanged, because Blueprint evaluates pure nodes again for every exec node that reads them.
- **Outputs used far away (functions only):** when an exec node's output is read two or more exec steps later, it is stored in a new local variable. A **Set** node is placed right after the producing node, and each far reader gets a **Get** node. Readers right after the producer keep their direct wire.

It never deletes nodes. Undo takes two Ctrl+Z when wires were shortened: one for the layout, one for the added nodes.

## Install

Requires Unreal Engine 5.8 (it may build on earlier 5.x versions, but they are untested). Your project must be able to compile C++.

1. Clone or copy this repo into your project's `Plugins` folder:
   ```
   cd YourProject/Plugins
   git clone https://github.com/CodeSpartan/BlueprintFormatter.git
   ```
2. Build the editor target (or open the project and let it compile the plugin).

The plugin is editor-only and is enabled by default. It adds nothing to packaged games.

## Use

Open any Blueprint and click **Format** in the toolbar:

| Selection | What gets formatted |
|---|---|
| Nothing selected | The whole graph that is currently open |
| One node | Everything connected to that node |
| Several nodes | Exactly those nodes (a selected comment brings in its contents) |

To format with a key, bind one under **Editor Preferences → Keyboard Shortcuts → Blueprint Formatter → Format**. There is no default key.

## Known limitations

- When two branches join back into the same node, it is placed next to its first user, so some exec wires stay long.
- Event graphs have no local variables, so outputs of exec nodes there (for example custom event parameters) can still have long wires.
- When you format only some nodes, they can end up overlapping nodes that were not selected.
- The button appears in the regular Blueprint editor and in the Graph tab of the Widget Blueprint editor. Animation Blueprints are not supported yet.

## Tests

The layout code has automation tests. Run them from the Session Frontend, or headless:

```
UnrealEditor-Cmd YourProject.uproject -ExecCmds="Automation RunTests BlueprintFormatter; Quit" -unattended -NullRHI
```

## License

[Apache License 2.0](LICENSE)
