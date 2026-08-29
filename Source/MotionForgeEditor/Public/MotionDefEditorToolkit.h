// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"

class FWorkspaceItem;
class IDetailsView;
class SMotionDefTakes;
class UMotionDef;

/**
 * The window a Motion Definition opens into.
 *
 * A definition is sixteen properties across three groups, of which a person authoring one cares
 * about five - and the details panel it used to open in gave equal weight to all of them, put the
 * takes in a collapsed array, and buried the error string in a list. This exists so that the two
 * questions somebody actually has - *what did I ask for* and *which take do I keep* - are the two
 * halves of one window.
 *
 * It owns no pipeline logic. Every command here calls UMotionForgeSubsystem, so nothing is reachable
 * from this window that an agent cannot reach from a tool call.
 */
class MOTIONFORGEEDITOR_API FMotionDefEditorToolkit : public FAssetEditorToolkit
{
public:

	static const FName ToolkitName;
	static const FName DetailsTabId;
	static const FName TakesTabId;

	void Initialise(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host, UMotionDef* Def);

	// FAssetEditorToolkit
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;

	// IToolkit
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;

private:

	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTakesTab(const FSpawnTabArgs& Args);

	void ExtendToolbar();
	void FillToolbar(class FToolBarBuilder& Builder);

	// The four verbs. Each is one subsystem call and a refresh; none of them decides anything.
	void OnGenerate();
	bool CanGenerate() const;

	void OnImportChosen();
	bool CanImportChosen() const;

	void OnOpenPromptSequence();

	void OnShowAnimation();
	bool CanShowAnimation() const;

	/** Says why Generate is greyed out, when it is - a disabled button owes an explanation. */
	FText GenerateTooltip() const;

	/** The import button's label changes with the billing model, because the decision does. */
	FText ImportLabel() const;
	FText ImportTooltip() const;

	UMotionDef* Def() const { return Definition.Get(); }

	TWeakObjectPtr<UMotionDef> Definition;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SMotionDefTakes> TakesPanel;
	TSharedPtr<FWorkspaceItem> WorkspaceMenuCategory;
};
