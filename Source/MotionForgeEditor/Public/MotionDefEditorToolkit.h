// Copyright Bojan Andrejek / MetaWorx LLC. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"

class FWorkspaceItem;
class IDetailsView;
class SMotionGeneratePanel;
class SMotionTakesPanel;
class UMotionDef;

/**
 * The window a Motion Definition opens into.
 *
 * The work on the left and the controls on the right, MeshForge's arrangement. Left: the stage with
 * the takes under it, and each take's full record. Right: the cards that say what to make, for whom,
 * on which provider and at what cost - and the full settings for somebody who wants every field.
 *
 * It owns no pipeline logic. Every button calls UMotionForgeSubsystem, so nothing is reachable from
 * this window that an agent cannot reach from a tool call.
 */
class MOTIONFORGEEDITOR_API FMotionDefEditorToolkit : public FAssetEditorToolkit
{
public:

	static const FName ToolkitName;
	static const FName TakesTabId;
	static const FName RecordTabId;
	static const FName GenerateTabId;
	static const FName SettingsTabId;

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

	TSharedRef<SDockTab> SpawnTakesTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnRecordTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnGenerateTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnSettingsTab(const FSpawnTabArgs& Args);

	void ExtendToolbar();
	void FillToolbar(class FToolBarBuilder& Builder);

	// Navigation only; the verbs live in the cards, beside what they act on.
	void OnOpenPromptSequence();
	void OnShowAnimation();
	bool CanShowAnimation() const;
	FText ShowAnimationTooltip() const;
	void OnOpenLibrary();

	UMotionDef* Def() const { return Definition.Get(); }

	TWeakObjectPtr<UMotionDef> Definition;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SMotionTakesPanel> TakesPanel;
	TSharedPtr<SMotionGeneratePanel> GeneratePanel;
	TSharedPtr<FWorkspaceItem> WorkspaceMenuCategory;
};
