// The left half of the definition window: the stage, the takes, and what can be done with one.

#pragma once

#include "CoreMinimal.h"
#include "MotionForgeTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class UMotionDef;
class UMotionCharacter;
class UAnimSequence;
class USkeletalMesh;
class SMotionStage;
class SBox;

/** One row of the take table: a copy of the take as it was at the last refresh. */
struct FMotionTakeRow
{
	/** The take's identity across refreshes: its motion id, or its number while it has none. */
	FString Key;

	FMotionCandidate Take;
	bool bInGame = false;
	bool bStale = false;

	/** The fetched file as it is on disk now, or empty. */
	FString File;
};

/**
 * The takes, and the stage they are judged on.
 *
 * Chosen while watched, so the list and the stage share the tab (0.6 over 0.4) rather than sitting
 * in two - the one place this departs from MeshForge, for a reason that is specific to motion.
 * Selecting a take puts it on the stage as A; Show as B puts it beside A on the same clock.
 */
class SMotionTakesPanel : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionTakesPanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMotionDef>, Definition)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMotionTakesPanel() override;

	/** The full record of the selected take, for the Record tab. Rebuilt when the selection moves. */
	TSharedRef<SWidget> GetRecordWidget();

	/** Rebuild from the asset. Selection is kept by take, not by row. */
	void Refresh();

	/** Put the imported clip on stage as A - what the window opens on when there is one. */
	void ShowImportedClip();

private:

	struct FSlot
	{
		/** The take asked for on this slot, or empty for the imported clip. */
		FString MotionId;
		FString Label;
		bool bFilled = false;
		bool bLoading = false;
		FString Problem;

		/**
		 * What is playing on this slot now. Not the same as Label while a take is being fetched: the
		 * clip before it keeps playing, and the stats line must not put the new name on the old clip.
		 */
		FString PlayingLabel;
	};

	TSharedRef<ITableRow> MakeRow(TSharedPtr<FMotionTakeRow> Row, const TSharedRef<STableViewBase>& Owner);
	void OnSelectionChanged(TSharedPtr<FMotionTakeRow> Row, ESelectInfo::Type Info);

	/** Fetch (where free) and build a take into a transient clip, then put it on a slot. */
	void ShowTake(int32 Slot, const FString& MotionId);
	void ClearSlot(int32 Slot);
	void PlaceOnStage(int32 Slot, UAnimSequence* Clip, const FString& Label);

	/** The mesh a clip should play on: the character's, when the skeletons agree. */
	USkeletalMesh* MeshFor(UAnimSequence* Clip) const;

	FText StageMessage() const;
	FText StageStats() const;

	TSharedRef<SWidget> BuildActions();
	TSharedRef<SWidget> BuildStatusLine();
	void RebuildRecord();

	FReply OnChooseAndImport();
	FReply OnShowAsB();
	FReply OnWatch();
	FReply OnHide();
	FReply OnShowFiles();

	TSharedPtr<FMotionTakeRow> GetSelected() const;
	const FMotionCandidate* FindLiveTake(const FString& Key) const;

	uint32 Fingerprint() const;
	EActiveTimerReturnType Poll(double, float);

	TWeakObjectPtr<UMotionDef> Definition;

	TSharedPtr<SMotionStage> Stage;
	TSharedPtr<SListView<TSharedPtr<FMotionTakeRow>>> List;
	TArray<TSharedPtr<FMotionTakeRow>> Rows;
	TSharedPtr<SBox> StatusBox;
	TSharedPtr<SBox> RecordBox;

	FSlot Slots[2];

	bool bShowHidden = false;
	int32 HiddenCount = 0;
	uint32 LastFingerprint = 0;
	FString SelectedKey;
	bool bOpenedOnce = false;
};
