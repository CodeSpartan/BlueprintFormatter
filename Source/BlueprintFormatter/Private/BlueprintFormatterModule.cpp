#include "BPFormatter.h"

#include "BlueprintEditor.h"
#include "BlueprintEditorContext.h"
#include "Editor.h"
#include "Framework/Commands/Commands.h"
#include "GraphEditor.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "BlueprintFormatter"

namespace
{
	class FBlueprintFormatterCommands : public TCommands<FBlueprintFormatterCommands>
	{
	public:
		FBlueprintFormatterCommands()
			: TCommands<FBlueprintFormatterCommands>(TEXT("BlueprintFormatter"), LOCTEXT("Context", "Blueprint Formatter"), NAME_None, FAppStyle::GetAppStyleSetName())
		{
		}

		virtual void RegisterCommands() override
		{
			// No default chord: bind one in Editor Preferences > Keyboard Shortcuts > Blueprint Formatter.
			UI_COMMAND(Format, "Format", "Arrange the selected nodes, or the whole graph if nothing is selected", EUserInterfaceActionType::Button, FInputChord());
		}

		TSharedPtr<FUICommandInfo> Format;
	};

	void FormatInEditor(const TWeakPtr<FBlueprintEditor>& WeakEditor)
	{
		TSharedPtr<FBlueprintEditor> Editor = WeakEditor.Pin();
		if (!Editor.IsValid())
		{
			return;
		}
		UEdGraph* Graph = Editor->GetFocusedGraph();
		if (!Graph)
		{
			return;
		}
		// This editor's own graph tab (FindGraphEditorForGraph could return another window's).
		BPFormat::FormatGraph(Editor->OpenGraphAndBringToFront(Graph, /*bSetFocus*/ false));
	}
}

class FBlueprintFormatterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FBlueprintFormatterCommands::Register();

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintFormatterModule::RegisterMenus));

		if (GEditor)
		{
			if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				OpenedHandle = Subsystem->OnAssetOpenedInEditor().AddRaw(this, &FBlueprintFormatterModule::OnAssetOpened);
			}
		}
	}

	virtual void ShutdownModule() override
	{
		if (GEditor && OpenedHandle.IsValid())
		{
			if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				Subsystem->OnAssetOpenedInEditor().Remove(OpenedHandle);
			}
		}
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		FBlueprintFormatterCommands::Unregister();
	}

private:
	FDelegateHandle OpenedHandle;

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		// Mode toolbars (AssetEditor.BlueprintEditor.ToolBar.<Mode>) inherit from this one.
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("AssetEditor.BlueprintEditor.ToolBar");
		FToolMenuSection& Section = Menu->FindOrAddSection("BlueprintFormatter");

		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
		{
			if (const UBlueprintEditorToolMenuContext* BPContext = Context.FindContext<UBlueprintEditorToolMenuContext>())
			{
				FormatInEditor(BPContext->BlueprintEditor);
			}
		});

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"BlueprintFormatter_Format",
			Action,
			LOCTEXT("FormatLabel", "Format"),
			LOCTEXT("FormatTooltip", "Arrange the selected nodes, or the whole graph if nothing is selected.\nOne node selected: its whole connected group."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Clean")));
	}

	/** Maps the Format command into each Blueprint editor so a user-bound shortcut works. */
	void OnAssetOpened(UObject* Asset, IAssetEditorInstance* Instance)
	{
		if (!Instance || Instance->GetEditorName() != FName("BlueprintEditor"))
		{
			return;
		}
		FBlueprintEditor* Editor = static_cast<FBlueprintEditor*>(Instance);
		TWeakPtr<FBlueprintEditor> WeakEditor = StaticCastSharedRef<FBlueprintEditor>(Editor->AsShared());
		const TSharedRef<FUICommandList> Commands = Editor->GetToolkitCommands();
		if (!Commands->IsActionMapped(FBlueprintFormatterCommands::Get().Format))
		{
			Commands->MapAction(FBlueprintFormatterCommands::Get().Format, FExecuteAction::CreateLambda([WeakEditor]()
			{
				FormatInEditor(WeakEditor);
			}));
		}
	}
};

IMPLEMENT_MODULE(FBlueprintFormatterModule, BlueprintFormatter)

#undef LOCTEXT_NAMESPACE
