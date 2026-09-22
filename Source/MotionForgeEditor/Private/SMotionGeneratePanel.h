// The right half of the definition window: what to make, for whom, on what, and what it costs.

#pragma once

#include "CoreMinimal.h"
#include "IMotionProvider.h"
#include "MotionForgeTypes.h"
#include "Widgets/SCompoundWidget.h"

class UMotionDef;
class IDetailsView;
class SBox;
class SMultiLineEditableTextBox;
class SMotionSection;

/**
 * The Generate tab, in cards from top to bottom: work in progress, prompt, character, generate,
 * direct (for a provider that takes poses) and import.
 *
 * Every number on it comes from one place, the subsystem's resolver, refreshed when the definition or
 * the provider's state moves. So the price on the button is the price of the request the button sends.
 *
 * The prompt box is built once and never rebuilt while it is being typed in; everything that changes
 * shape - the beat strip, the character's setup buttons, the provider's settings - sits in a box of
 * its own that is refilled when its own inputs change.
 */
class SMotionGeneratePanel : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionGeneratePanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UMotionDef>, Definition)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMotionGeneratePanel() override;

	/** Press Generate as the button would: commit the prompt, confirm any spend, submit. For the toolbar too. */
	FReply OnGenerate();

	/** Whether the button would do something now. */
	bool CanGenerate() const;

	const FMotionResolvedRequest& GetResolved() const { return Resolved; }

private:

	// Cards
	TSharedRef<SWidget> BuildActivityCard();
	TSharedRef<SWidget> BuildPromptCard();
	TSharedRef<SWidget> BuildCharacterCard();
	TSharedRef<SWidget> BuildGenerateCard();
	TSharedRef<SWidget> BuildDirectCard();
	TSharedRef<SWidget> BuildImportCard();

	// The parts that are refilled
	void RebuildActivity();
	void RebuildBeats();
	void RebuildCharacter();
	void RebuildProvider();
	void RebuildDirect();
	void RebuildWarnings();

	TSharedRef<SWidget> MakeActionButton(const FMotionCharacterSetupAction& Action, bool bPrimary);
	void RunAction(const FMotionCharacterSetupAction& Action);

	/** The primary button's label and whether it fixes something rather than generating. */
	FText GenerateLabel() const;
	FText GenerateTooltip() const;
	FText CostLine() const;
	FSlateColor CostColour() const;

	/** Write the prompt box into the asset. Before every action, so Generate never sends the previous text. */
	void CommitPrompt();
	void OnPromptChanged(const FText& Text);

	void SetProvider(FName ProviderId);
	void SetCharacter(const FString& Path);
	void CreateCharacterFromMesh(const FString& MeshPath);
	void OpenPromptTimeline();

	/** Show a sentence under the Generate button for a while: what a switch did, what an action said. */
	void SetMessage(const FString& Text, bool bProblem);

	void Resolve();
	uint32 Fingerprint() const;
	EActiveTimerReturnType Poll(double, float);
	void OnProviderStateChanged(FName ProviderId);

	TWeakObjectPtr<UMotionDef> Definition;
	FMotionResolvedRequest Resolved;

	TSharedPtr<SMultiLineEditableTextBox> PromptBox;
	TSharedPtr<SBox> ActivityBox;
	TSharedPtr<SBox> BeatsBox;
	TSharedPtr<SBox> WarningsBox;
	TSharedPtr<SBox> CharacterBox;
	TSharedPtr<SBox> ProviderSettingsBox;
	TSharedPtr<SBox> DirectBox;
	TSharedPtr<SMotionSection> DirectSection;
	TSharedPtr<IDetailsView> PipelineDetails;
	TSharedPtr<IDetailsView> ImportDetails;

	FString Message;
	bool bMessageIsProblem = false;
	double MessageUntil = 0.0;

	/** Something a provider action is doing right now, with when it started, for the clock. */
	FString RunningAction;
	FDateTime RunningSince;

	uint32 LastFingerprint = 0;
	uint32 LastBeatsStamp = 0;
	uint32 LastCharacterStamp = 0;
	uint32 LastActivityStamp = 0;
	uint32 LastWarningsStamp = 0;
	FName LastProvider;
	float SinceProviderPoll = 0.f;

	FDelegateHandle ProviderStateHandle;
};
