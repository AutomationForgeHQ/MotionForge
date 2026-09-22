// Get Started: from a fresh install to a first motion, in the order a person meets it.
//
// Three steps, each measured rather than assumed: where motion is made (each provider's own setup -
// keys, access grants, Docker, a runner - with the button that moves each on), who it is for (a Motion
// Character prepared for that provider), and what happens (a prompt). The last step creates the
// definition and opens it, where Generate says its price before anything is spent.
//
// Each step is a section that stays on screen; only what is inside it is redrawn, and only when a
// step's state actually changes. Text that moves by itself - a runner's progress - is read live.

#pragma once

#include "CoreMinimal.h"
#include "SMotionSection.h"
#include "Widgets/SCompoundWidget.h"

class UMotionCharacter;
struct FMotionCharacterSetupAction;

class SMotionGetStarted : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionGetStarted) {}
		/** Called by "Open the library", so the home can switch pages. */
		SLATE_EVENT(FSimpleDelegate, OnOpenLibrary)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMotionGetStarted() override;

private:

	// ---------------------------------------------------------------------------------------------
	// The three steps. Each replaces only its section's content.
	// ---------------------------------------------------------------------------------------------

	void RebuildProviders();
	void RebuildCharacter();
	void RebuildFirstMotion();

	TSharedRef<SWidget> MakeProviderCard(FName ProviderId);
	TSharedRef<SWidget> MakeActionRow(const FMotionCharacterSetupAction& Action);

	/**
	 * Ask every provider for its setup rows: keep their text for the rows to read live, and return
	 * a signature of what would need the rows redrawn - states, labels, buttons, readiness.
	 */
	uint32 MeasureSetup();

	/** The same for step 2: the chosen character, whether it suits, and what it is prepared with. */
	uint32 MeasureCharacter() const;

	/** Recompute the one-line summaries and badge states the section headers show. */
	void UpdateSummaries();

	EActiveTimerReturnType Poll(double InCurrentTime, float InDeltaTime);

	/** A provider said its state moved: look now, redraw only what changed shape. */
	void OnProviderStateChanged(FName ProviderId);

	// ---------------------------------------------------------------------------------------------

	void ChooseProvider(FName ProviderId);
	void ChooseCharacter(const FString& Path);
	void CreateCharacterFromMesh(const FString& MeshPath);
	void RunAction(const FMotionCharacterSetupAction& Action);

	/** The definition's name, made from the prompt until somebody types their own. */
	FString SuggestName() const;

	/**
	 * Whether a definition of this name already exists. Creating one by an existing name updates it,
	 * which on a page for first motions would quietly rewrite somebody's work.
	 */
	static bool DefinitionExists(const FString& AssetName);

	/** The character to start from for a provider: one that suits, else one made for it that needs a fix. */
	FString PickCharacter(FName ProviderId) const;

	/** What one take of this length costs on the chosen provider, as a sentence. */
	FText PriceLine() const;

	FReply OnCreate();

	void SetMessage(const FString& Text, bool bProblem);

	// ---------------------------------------------------------------------------------------------

	TSharedPtr<SMotionSection> ProvidersStep;
	TSharedPtr<SMotionSection> CharacterStep;
	TSharedPtr<SMotionSection> FirstMotionStep;

	FName ChosenProvider;
	FString ChosenCharacter;

	FString Prompt;
	int32 LengthSeconds = 4;
	FString Name;
	bool bNameEdited = false;

	FString Message;
	bool bMessageIsProblem = false;

	/** The action being run, so its buttons wait for it. Empty when none is. */
	FString RunningAction;

	/** Each provider's setup-row text, as last measured. Rows read it every paint. */
	TMap<FName, TArray<FText>> StepDetails;

	uint32 LastSetupSignature = 0;
	uint32 LastCharacterSignature = 0;

	// What the section headers show, recomputed once a second rather than on every paint.
	FText ProvidersSummary;
	EMotionStepState ProvidersState = EMotionStepState::Todo;
	FText CharacterSummary;
	EMotionStepState CharacterState = EMotionStepState::Todo;
	FText CharacterSubtitle;

	FSimpleDelegate OnOpenLibrary;
	FDelegateHandle ProviderStateHandle;
};
