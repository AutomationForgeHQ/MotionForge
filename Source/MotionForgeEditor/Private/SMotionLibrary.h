// Copyright Blackcode SA. All rights reserved.
//
// The library: every motion definition in the project, what state each is in, and what it produced.
//
// The Content Browser lists the same assets and can tell you none of that - a folder of MD_ icons is
// twenty definitions with no way to see which are waiting for a decision, which failed, and which
// have an animation to show for themselves. This is also the only place a spend across several
// definitions is priced before it happens.

#pragma once

#include "CoreMinimal.h"
#include "MotionForgeTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

class SBox;
class SSearchBox;

/** Which slice of the library is on screen. */
enum class EMotionLibraryFilter : uint8
{
	All,
	Draft,
	Working,
	Review,
	Ready,
	Failed
};

/** One definition, as the library draws it. */
struct FMotionLibraryEntry
{
	FMotionDefinitionStatus Status;

	/** Takes that finished and could be imported. */
	int32 UsableTakes = 0;

	/** What the provider calls itself, rather than its id. Falls back to the id when unknown. */
	FString ProviderName;

	/** The imported animation's asset name, without its path. Empty when there is none. */
	FString AnimationName;

	/**
	 * Whether this definition could generate right now.
	 *
	 * Only filled in for the current selection: the check loads the definition and reads the
	 * credential vault, which is fine for the handful somebody has selected and not for a library.
	 */
	FMotionReadiness Readiness;
	bool bReadinessKnown = false;
};

class SMotionLibrary : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionLibrary) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMotionLibrary() override;

private:

	// ---------------------------------------------------------------------------------------------
	// Reading the project
	// ---------------------------------------------------------------------------------------------

	/** Re-read every definition and redraw. The expensive one; everything else works off its result. */
	void Rescan();

	/** Re-apply the filter, the search and the sort to what Rescan already read. */
	void ApplyFilter();

	/**
	 * A cheap hash of what the list draws, over the definitions already in memory.
	 *
	 * Rescan loads assets; this reads fields off objects that are loaded and costs nothing, which is
	 * what lets the library notice a generation finishing without a person pressing anything.
	 */
	uint32 Fingerprint() const;

	EActiveTimerReturnType Poll(double InCurrentTime, float InDeltaTime);

	/** The registry moved: a definition was added, removed or renamed somewhere else. */
	void OnAssetRegistryChanged(const struct FAssetData& Asset);
	void OnAssetRenamed(const struct FAssetData& Asset, const FString& OldPath);

	// ---------------------------------------------------------------------------------------------
	// Drawing
	// ---------------------------------------------------------------------------------------------

	TSharedRef<SWidget> BuildSummary();
	TSharedRef<SWidget> BuildFilters();
	TSharedRef<SWidget> BuildFooter();

	TSharedRef<class ITableRow> OnGenerateRow(
		TSharedPtr<FMotionLibraryEntry> Entry, const TSharedRef<class STableViewBase>& Owner);

	void OnSelectionChanged(TSharedPtr<FMotionLibraryEntry> Entry, ESelectInfo::Type SelectInfo);
	void OnRowDoubleClicked(TSharedPtr<FMotionLibraryEntry> Entry);

	void OnSortChanged(EColumnSortPriority::Type Priority, const FName& Column, EColumnSortMode::Type Mode);

	// ---------------------------------------------------------------------------------------------
	// The verbs. Each is one subsystem call.
	// ---------------------------------------------------------------------------------------------

	FReply OnNewDefinition();
	FReply OnRefresh();
	FReply OnOpenSelected();
	FReply OnShowAnimations();
	FReply OnGenerateSelected();
	FReply OnImportSelected();

	/** Definitions in the selection that could generate right now. */
	TArray<TSharedPtr<FMotionLibraryEntry>> GenerateableSelection() const;

	/** Definitions in the selection with a chosen take that has finished. */
	TArray<TSharedPtr<FMotionLibraryEntry>> ImportableSelection() const;

	// ---------------------------------------------------------------------------------------------

	/** Everything in the project. */
	TArray<TSharedPtr<FMotionLibraryEntry>> Entries;

	/** What the list actually shows, after the filter, the search and the sort. */
	TArray<TSharedPtr<FMotionLibraryEntry>> Visible;

	TSharedPtr<SListView<TSharedPtr<FMotionLibraryEntry>>> ListView;

	/** Rebuilt in place, so the search box beside them keeps its text and its focus. */
	TSharedPtr<SBox> SummaryBox;
	TSharedPtr<SBox> FilterBox;
	TSharedPtr<SBox> FooterBox;

	EMotionLibraryFilter Filter = EMotionLibraryFilter::All;
	FString SearchText;

	FName SortColumn;
	EColumnSortMode::Type SortMode = EColumnSortMode::Ascending;

	uint32 LastFingerprint = 0;

	/**
	 * Set by a registry callback, acted on by the timer.
	 *
	 * Rescanning inside the callback would fire once per asset during a scan, and each one loads
	 * every definition in the project.
	 */
	bool bRescanQueued = false;

	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	FDelegateHandle AssetRenamedHandle;
};
