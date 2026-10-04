#include "SMotionGeneratePanel.h"

#include "MotionForgeEditorStyle.h"
#include "SMotionSection.h"

#include "IMotionProvider.h"
#include "MotionCharacter.h"
#include "MotionDef.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"
#include "MotionPipeline.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "IContentBrowserSingleton.h"
#include "IDetailsView.h"
#include "IStructureDetailsView.h"
#include "LevelSequence.h"
#include "Misc/MessageDialog.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"

#define LOCTEXT_NAMESPACE "MotionForgeGenerate"

// Qualified rather than pulled in with a using-directive: a file-scope `using namespace` leaks into
// every file after this one in a unity build (see SMotionLibrary.cpp).
namespace MFS = MotionForgeStyle;

namespace MotionGeneratePrivate
{
	TSharedPtr<IMotionProvider> Provider(FName Id)
	{
		UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
		return Forge ? Forge->FindProvider(Id) : nullptr;
	}

	/** Where a provider's rate comes from when the person did not enter it; empty when it is theirs. */
	FString RateNote(FName Id)
	{
		const TSharedPtr<IMotionProvider> P = Provider(Id);
		return P.IsValid() ? P->GetBilling().RateNote : FString();
	}

	/** A blocker the Character card is responsible for explaining and fixing. */
	bool IsCharacterBlocker(EMotionBlocker Blocker)
	{
		return Blocker == EMotionBlocker::NoCharacter
			|| Blocker == EMotionBlocker::CharacterUnusable
			|| Blocker == EMotionBlocker::CharacterForOtherProvider
			|| Blocker == EMotionBlocker::RetargetIncomplete;
	}

	/** A small read-only window for a long answer - a constraint payload, a diagnosis. */
	void ShowText(const FText& Title, const FString& Body)
	{
		TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(Title)
			.ClientSize(FVector2D(760.f, 520.f))
			.SupportsMaximize(true)
			.SupportsMinimize(false);

		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text(FText::FromString(Body))
				.IsReadOnly(true)
				.AutoWrapText(true)
			]);

		FSlateApplication::Get().AddWindow(Window);
	}

}

// -------------------------------------------------------------------------------------------------

void SMotionGeneratePanel::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));

	FDetailsViewArgs Args;
	Args.bAllowSearch = false;
	Args.bHideSelectionTip = true;
	Args.bShowOptions = false;
	Args.bShowPropertyMatrixButton = false;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Args.ColumnWidth = 0.45f;

	PipelineDetails = PropertyModule.CreateDetailView(Args);
	PipelineDetails->OnFinishedChangingProperties().AddLambda([this](const FPropertyChangedEvent&)
	{
		Resolve();
	});

	ImportDetails = PropertyModule.CreateDetailView(Args);
	ImportDetails->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& Property)
	{
		return Property.Property.GetMetaData(TEXT("Category")) == TEXT("4 Import");
	}));
	ImportDetails->SetObject(Definition.Get());

	Resolve();

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(10.f, 10.f, 10.f, 20.f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(ActivityBox, SBox)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				BuildPromptCard()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				BuildCharacterCard()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				BuildGenerateCard()
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				BuildDirectCard()
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildImportCard()
			]
		]
	];

	RebuildActivity();
	RebuildBeats();
	RebuildWarnings();
	RebuildCharacter();
	RebuildProvider();
	RebuildDirect();

	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SMotionGeneratePanel::Poll));

	if (UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get())
	{
		ProviderStateHandle = Forge->OnProviderStateChanged().AddSP(this, &SMotionGeneratePanel::OnProviderStateChanged);

		// Asked once on opening, so the window does not start from whatever was cached a while ago.
		Forge->RefreshProviderState(Resolved.ProviderId);
	}
}

SMotionGeneratePanel::~SMotionGeneratePanel()
{
	if (UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get())
	{
		Forge->OnProviderStateChanged().Remove(ProviderStateHandle);
	}
}

// -------------------------------------------------------------------------------------------------
// Keeping current
// -------------------------------------------------------------------------------------------------

void SMotionGeneratePanel::Resolve()
{
	const UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (Def && Forge)
	{
		Resolved = Forge->ResolveRequest(Def->GetPathName());
	}
	LastFingerprint = Fingerprint();
}

uint32 SMotionGeneratePanel::Fingerprint() const
{
	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return 0;
	}

	uint32 Hash = GetTypeHash(Def->Prompt);
	Hash = HashCombine(Hash, GetTypeHash(Def->Length));
	Hash = HashCombine(Hash, GetTypeHash(Def->Variants));
	Hash = HashCombine(Hash, GetTypeHash(Def->ProviderId));
	Hash = HashCombine(Hash, GetTypeHash(Def->Character.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Def->Status)));
	Hash = HashCombine(Hash, GetTypeHash(Def->Control.ConstraintSequence.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Def->Control.CountAuthoredKeys()));
	Hash = HashCombine(Hash, GetTypeHash(Def->Candidates.Num()));

	for (const float Seconds : Def->Control.BeatSeconds)
	{
		Hash = HashCombine(Hash, GetTypeHash(Seconds));
	}

	if (const UMotionPipeline* Pipeline = Def->FindPipeline(Def->GetResolvedProviderId()))
	{
		Hash = HashCombine(Hash, GetTypeHash(Pipeline->Signature()));
	}

	// The character itself can change under us - a rig built, an upload finished.
	if (const UMotionCharacter* Character = Def->Character.Get())
	{
		Hash = HashCombine(Hash, GetTypeHash(Character->ProviderCharacterId));
		Hash = HashCombine(Hash, GetTypeHash(Character->ProviderMesh.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Character->Retargeter.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Character->ProviderId));
	}

	return Hash;
}

EActiveTimerReturnType SMotionGeneratePanel::Poll(double, float DeltaTime)
{
	if (Fingerprint() != LastFingerprint)
	{
		Resolve();
	}

	RebuildActivity();
	RebuildBeats();
	RebuildWarnings();
	RebuildCharacter();

	if (Resolved.ProviderId != LastProvider)
	{
		RebuildProvider();
		RebuildDirect();
	}

	// And the provider itself, every ten seconds, for what a broadcast cannot see: a container stopped
	// from a terminal, a pod released from a dashboard.
	SinceProviderPoll += DeltaTime;
	if (SinceProviderPoll >= 10.f)
	{
		SinceProviderPoll = 0.f;
		if (UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get())
		{
			Forge->RefreshProviderState(Resolved.ProviderId);
		}
	}

	if (!Message.IsEmpty() && FPlatformTime::Seconds() > MessageUntil)
	{
		Message.Reset();
	}

	return EActiveTimerReturnType::Continue;
}

void SMotionGeneratePanel::OnProviderStateChanged(FName ProviderId)
{
	// Re-resolve and let the card's own stamp decide. Forcing a redraw here redrew the character card
	// every ten seconds, because this window asks the provider for its state that often.
	if (ProviderId == Resolved.ProviderId)
	{
		Resolve();
		RebuildCharacter();
	}
}

void SMotionGeneratePanel::SetMessage(const FString& Text, bool bProblem)
{
	Message = Text;
	bMessageIsProblem = bProblem;
	MessageUntil = FPlatformTime::Seconds() + (bProblem ? 30.0 : 12.0);
}

// -------------------------------------------------------------------------------------------------
// Work in progress
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionGeneratePanel::BuildActivityCard()
{
	return SNullWidget::NullWidget;
}

void SMotionGeneratePanel::RebuildActivity()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!ActivityBox.IsValid() || !Forge)
	{
		return;
	}

	const TArray<FMotionActivity> Activities = Forge->GetActivities();

	uint32 Stamp = GetTypeHash(RunningAction);
	for (const FMotionActivity& Activity : Activities)
	{
		Stamp = HashCombine(Stamp, GetTypeHash(Activity.AssetPath));
		Stamp = HashCombine(Stamp, GetTypeHash(Activity.Doing));
		Stamp = HashCombine(Stamp, GetTypeHash(Activity.bLate));
	}

	if (Stamp == LastActivityStamp)
	{
		return;
	}
	LastActivityStamp = Stamp;

	if (Activities.Num() == 0 && RunningAction.IsEmpty())
	{
		ActivityBox->SetContent(SNullWidget::NullWidget);
		return;
	}

	const FString Own = Definition.IsValid() ? Definition->GetPathName() : FString();

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

	// One row per job, across every definition: two jobs on one GPU explain each other's speed.
	for (const FMotionActivity& Activity : Activities)
	{
		const FString Path = Activity.AssetPath;
		const FDateTime Started = Activity.StartedAt;
		const FName ProviderId = Resolved.ProviderId;
		const bool bOwn = Path == Own;

		Rows->AddSlot().AutoHeight().Padding(0.f, 3.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				Activity.bLate
					? StaticCastSharedRef<SWidget>(MFS::Dot(MFS::Warn()))
					: StaticCastSharedRef<SWidget>(SNew(SCircularThrobber).Radius(7.f))
			]

			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(bOwn ? FAppStyle::GetFontStyle("BoldFont") : FAppStyle::GetFontStyle("NormalFont"))
					.Text(FText::Format(LOCTEXT("ActivityFmt", "{0}: {1}{2}"),
						FText::FromString(Activity.DefinitionName), FText::FromString(Activity.Doing),
						Activity.bLate ? LOCTEXT("ActivityLate", " (past the timeout, still waiting)") : FText::GetEmpty()))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					// What a provider getting ready is doing now, when it says: "Loading the text encoder (3 min)".
					SNew(STextBlock)
					.Text_Lambda([ProviderId]()
					{
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(ProviderId);
						return P.IsValid() ? FText::FromString(P->GetPreparationProgress()) : FText::GetEmpty();
					})
					.Visibility_Lambda([ProviderId, bOwn]()
					{
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(ProviderId);
						return bOwn && P.IsValid() && !P->GetPreparationProgress().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f)
			[
				SNew(STextBlock)
				.Text_Lambda([Started]() { return FText::FromString(MFS::Clock(FDateTime::Now() - Started)); })
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.ToolTipText(LOCTEXT("CancelTip", "Stop waiting. Takes that finished stay; on a paid provider a submitted take may still finish and bill."))
				.Visibility(Activity.bCanCancel ? EVisibility::Visible : EVisibility::Collapsed)
				.OnClicked_Lambda([Path]()
				{
					if (UMotionForgeSubsystem* F = UMotionForgeSubsystem::Get())
					{
						F->CancelDefinition(Path);
					}
					return FReply::Handled();
				})
			]
		];
	}

	if (!RunningAction.IsEmpty())
	{
		const FDateTime Started = RunningSince;
		Rows->AddSlot().AutoHeight().Padding(0.f, 3.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SCircularThrobber).Radius(7.f)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(RunningAction))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([Started]() { return FText::FromString(MFS::Clock(FDateTime::Now() - Started)); })
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
	}

	ActivityBox->SetContent(
		SNew(SBox).Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[
			SNew(SMotionSection)
			.Title(LOCTEXT("ActivityTitle", "Work in progress"))
			[
				Rows
			]
		]);
}

// -------------------------------------------------------------------------------------------------
// Prompt
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionGeneratePanel::BuildPromptCard()
{
	const UMotionDef* Def = Definition.Get();

	TSharedRef<SWrapBox> Examples = SNew(SWrapBox).UseAllottedSize(true);
	Examples->AddSlot().Padding(0.f, 0.f, 6.f, 0.f)
	[
		SNew(STextBlock).Text(LOCTEXT("Try", "Try:")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];
	for (const FString& Example : MFS::ExamplePrompts())
	{
		const FString Text = Example;
		Examples->AddSlot().Padding(0.f, 0.f, 10.f, 2.f)
		[
			SNew(SHyperlink)
			.Text(FText::FromString(Text))
			.OnNavigate_Lambda([this, Text]()
			{
				if (PromptBox.IsValid())
				{
					PromptBox->SetText(FText::FromString(Text));
					OnPromptChanged(FText::FromString(Text));
				}
			})
		];
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(84.f)
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
				.Text(Def ? FText::FromString(Def->Prompt) : FText::GetEmpty())
				.HintText(LOCTEXT("PromptHint", "A person walks forward, stops, and waves with the right hand."))
				.AutoWrapText(true)
				.OnTextChanged(this, &SMotionGeneratePanel::OnPromptChanged)
				.IsReadOnly_Lambda([this]() { return Resolved.bPromptFromTimeline; })
			]
		]

		// Examples for an empty prompt - a first motion should be one click from here.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return PromptBox.IsValid() && PromptBox->GetText().IsEmpty() && !Resolved.bPromptFromTimeline
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				Examples
			]
		]

		// The timeline owns the prompt while it is linked.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]() { return Resolved.bPromptFromTimeline ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				MFS::Note(LOCTEXT("TimelineOwns", "The prompt timeline is the prompt: its beats are sent in order. Edit them there."), MFS::Info())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("OpenTimeline", "Open prompt timeline"))
				.OnClicked_Lambda([this]() { OpenPromptTimeline(); return FReply::Handled(); })
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			MFS::Fact(LOCTEXT("Length", "Length"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(90.f)
					[
						SNew(SSpinBox<int32>)
						.MinValue(1).MaxValue(60)
						.Delta(1)
						.Value_Lambda([this]() { return Definition.IsValid() ? Definition->Length : 5; })
						.IsEnabled_Lambda([this]() { return !Resolved.bPromptFromTimeline; })
						.OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type)
						{
							if (UMotionDef* D = Definition.Get())
							{
								FScopedTransaction Transaction(LOCTEXT("SetLength", "Set Motion Length"));
								D->Modify();
								D->Length = FMath::Clamp(Value, 1, 60);
								D->MarkPackageDirty();
								Resolve();
							}
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f)
				[
					SNew(STextBlock).Text(LOCTEXT("Seconds", "seconds"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						if (!Resolved.LengthNote.IsEmpty())
						{
							return FText::FromString(Resolved.LengthNote);
						}
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId);
						if (!P.IsValid())
						{
							return FText::GetEmpty();
						}
						const FMotionPromptSplitting Split = P->GetPromptSplitting();
						return Split.bSplitsAtFullStops
							? FText::Format(LOCTEXT("SplitRangeFmt", "Shared by the beats. Up to {0} a beat, {1} in all."),
								FText::FromString(MFS::Seconds(Split.MaxBeatSeconds)), FText::FromString(MFS::Seconds(Split.MaxTotalSeconds)))
							: FText::Format(LOCTEXT("RangeFmt", "{0} makes {1} to {2}."),
								FText::FromString(P->GetDisplayName()),
								FText::FromString(MFS::Seconds(P->GetModels().Num() > 0 ? P->GetModels()[0].MinSeconds : 1.f)),
								FText::FromString(MFS::Seconds(P->GetModels().Num() > 0 ? P->GetModels()[0].MaxSeconds : 10.f)));
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				])
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SAssignNew(BeatsBox, SBox)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SAssignNew(WarningsBox, SBox)
		];

	return SNew(SMotionSection)
		.Number(1)
		.Title(LOCTEXT("PromptTitle", "Prompt"))
		.Subtitle(LOCTEXT("PromptSub", "Who moves, what they do, and how it ends. Start with \"A person\"; one or two actions work best."))
		.Summary_Lambda([this]()
		{
			return Resolved.Prompt.IsEmpty()
				? LOCTEXT("PromptSummaryEmpty", "no prompt yet")
				: FText::Format(LOCTEXT("PromptSummaryFmt", "{0} s, {1} {1}|plural(one=beat,other=beats)"),
					FMath::RoundToInt(Resolved.LengthSeconds), FMath::Max(1, Resolved.BeatCount));
		})
		.State_Lambda([this]()
		{
			return Resolved.Prompt.TrimStartAndEnd().IsEmpty() ? EMotionStepState::Current : EMotionStepState::Done;
		})
		[
			Body
		];
}

void SMotionGeneratePanel::OnPromptChanged(const FText& Text)
{
	// Written as it is typed, so Generate never sends the text from before the last keystroke. No
	// transaction per keystroke - the asset is marked dirty and saved with everything else.
	UMotionDef* Def = Definition.Get();
	if (Def && !Resolved.bPromptFromTimeline && Def->Prompt != Text.ToString())
	{
		Def->Prompt = Text.ToString();
		Def->MarkPackageDirty();
	}
}

void SMotionGeneratePanel::CommitPrompt()
{
	if (PromptBox.IsValid())
	{
		OnPromptChanged(PromptBox->GetText());
	}
}

void SMotionGeneratePanel::RebuildBeats()
{
	if (!BeatsBox.IsValid())
	{
		return;
	}

	uint32 Stamp = GetTypeHash(Resolved.bSplitIntoBeats);
	Stamp = HashCombine(Stamp, GetTypeHash(Resolved.bPromptFromTimeline));
	for (const FMotionPromptBeat& Beat : Resolved.Beats)
	{
		Stamp = HashCombine(Stamp, GetTypeHash(Beat.Text));
		Stamp = HashCombine(Stamp, GetTypeHash(Beat.Seconds));
	}
	if (Stamp == LastBeatsStamp)
	{
		return;
	}
	LastBeatsStamp = Stamp;

	UMotionDef* Def = Definition.Get();
	TSharedPtr<IMotionProvider> Provider = MotionGeneratePrivate::Provider(Resolved.ProviderId);

	// Beats mean something only where the provider cuts the prompt into them. Elsewhere the whole
	// prompt is one generation and the Length line says everything.
	if (!Def || !Provider.IsValid() || !Resolved.bSplitIntoBeats || Resolved.Beats.Num() == 0)
	{
		BeatsBox->SetContent(SNullWidget::NullWidget);
		return;
	}

	const float MaxBeat = Provider->GetPromptSplitting().MaxBeatSeconds;
	const bool bExplicit = Def->Control.BeatSeconds.Num() > 0 || Resolved.bPromptFromTimeline;

	TSharedRef<SWrapBox> Chips = SNew(SWrapBox).UseAllottedSize(true);

	for (int32 Index = 0; Index < Resolved.Beats.Num(); ++Index)
	{
		const FMotionPromptBeat& Beat = Resolved.Beats[Index];
		const bool bTooLong = Beat.Seconds > MaxBeat + 0.01f;

		Chips->AddSlot().Padding(0.f, 0.f, 6.f, 6.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(8.f, 4.f))
			.ToolTipText(FText::FromString(Beat.Text.IsEmpty() ? FString(TEXT("(empty beat)")) : Beat.Text))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text(FText::Format(LOCTEXT("BeatFmt", "Beat {0}"), Index + 1))
					.ColorAndOpacity(bTooLong ? FSlateColor(MFS::Bad()) : FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(64.f)
					[
						SNew(SSpinBox<float>)
						.MinValue(0.1f).MaxValue(FMath::Max(MaxBeat, 1.f) * 2.f)
						.Delta(0.1f)
						.MinFractionalDigits(1).MaxFractionalDigits(1)
						.Value(Beat.Seconds)
						.IsEnabled(!Resolved.bPromptFromTimeline)
						.ToolTipText(LOCTEXT("BeatSecondsTip", "Seconds for this beat. Setting one gives every beat its own duration; the clip is their sum."))
						.OnValueCommitted_Lambda([this, Index](float Value, ETextCommit::Type)
						{
							UMotionDef* D = Definition.Get();
							if (!D)
							{
								return;
							}

							// Every beat gets an explicit duration the moment one does, from what the strip
							// shows now - that is what the provider would have used anyway.
							TArray<float> Seconds;
							for (const FMotionPromptBeat& B : Resolved.Beats)
							{
								Seconds.Add(B.Seconds);
							}
							if (!Seconds.IsValidIndex(Index))
							{
								return;
							}
							Seconds[Index] = FMath::Max(0.1f, Value);

							FScopedTransaction Transaction(LOCTEXT("SetBeat", "Set Beat Seconds"));
							D->Modify();
							D->Control.BeatSeconds = Seconds;
							D->MarkPackageDirty();
							Resolve();
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock).Text(LOCTEXT("S", "s")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox).MaxDesiredWidth(220.f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Beat.Text))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
			]
		];
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ Chips ];

	if (Def->Control.BeatSeconds.Num() > 0 && !Resolved.bPromptFromTimeline)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ExplicitBeats", "Each beat has its own seconds; the clip is their sum."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("ShareEvenly", "Share the length evenly"))
				.OnClicked_Lambda([this]()
				{
					if (UMotionDef* D = Definition.Get())
					{
						FScopedTransaction Transaction(LOCTEXT("ClearBeats", "Share Beats Evenly"));
						D->Modify();
						D->Control.BeatSeconds.Reset();
						D->MarkPackageDirty();
						Resolve();
					}
					return FReply::Handled();
				})
			]
		];
	}
	else if (!bExplicit && Resolved.Beats.Num() > 1)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("EvenBeats", "The length is shared evenly. Change a beat's seconds to give it more or less time."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
	}

	BeatsBox->SetContent(Body);
}

void SMotionGeneratePanel::RebuildWarnings()
{
	if (!WarningsBox.IsValid())
	{
		return;
	}

	uint32 Stamp = 0;
	for (const FString& Warning : Resolved.Readiness.Warnings)
	{
		Stamp = HashCombine(Stamp, GetTypeHash(Warning));
	}
	if (Stamp == LastWarningsStamp)
	{
		return;
	}
	LastWarningsStamp = Stamp;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const FString& Warning : Resolved.Readiness.Warnings)
	{
		Box->AddSlot().AutoHeight().Padding(0.f, 2.f)[ MFS::Note(FText::FromString(Warning), MFS::Warn(), "Icons.Warning") ];
	}
	WarningsBox->SetContent(Box);
}

// -------------------------------------------------------------------------------------------------
// Character
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionGeneratePanel::BuildCharacterCard()
{
	return SNew(SMotionSection)
		.Number(2)
		.Title(LOCTEXT("CharacterTitle", "Character"))
		.Subtitle(LOCTEXT("CharacterSub", "Who the motion is for. Each provider needs a character prepared for it."))
		.Summary_Lambda([this]()
		{
			return Resolved.CharacterName.IsEmpty()
				? LOCTEXT("CharacterSummaryNone", "no character yet")
				: FText::FromString(Resolved.CharacterName);
		})
		.State_Lambda([this]()
		{
			if (Resolved.CharacterName.IsEmpty())
			{
				return EMotionStepState::Current;
			}
			return MotionGeneratePrivate::IsCharacterBlocker(Resolved.Readiness.Blocker) ? EMotionStepState::Attention : EMotionStepState::Done;
		})
		[
			SAssignNew(CharacterBox, SBox)
		];
}

void SMotionGeneratePanel::RebuildCharacter()
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!CharacterBox.IsValid() || !Def || !Forge)
	{
		return;
	}

	UMotionCharacter* Character = Def->Character.LoadSynchronous();

	uint32 Stamp = GetTypeHash(Def->Character.ToString());
	Stamp = HashCombine(Stamp, GetTypeHash(Resolved.ProviderId));
	Stamp = HashCombine(Stamp, GetTypeHash(static_cast<uint8>(Resolved.Readiness.Blocker)));
	Stamp = HashCombine(Stamp, GetTypeHash(Resolved.Readiness.Problem));
	Stamp = HashCombine(Stamp, GetTypeHash(RunningAction));
	if (Character)
	{
		Stamp = HashCombine(Stamp, GetTypeHash(Character->ProviderCharacterId));
		Stamp = HashCombine(Stamp, GetTypeHash(Character->ProviderMesh.ToString()));
		Stamp = HashCombine(Stamp, GetTypeHash(Character->Retargeter.ToString()));
	}
	if (Stamp == LastCharacterStamp)
	{
		return;
	}
	LastCharacterStamp = Stamp;

	TSharedPtr<IMotionProvider> Provider = MotionGeneratePrivate::Provider(Resolved.ProviderId);
	const FString ProviderName = Provider.IsValid() ? Provider->GetDisplayName() : Resolved.ProviderId.ToString();

	// The picker: characters that suit this provider first, then the rest, which say why they do not.
	TSharedRef<TArray<TSharedPtr<FString>>> Options = MakeShared<TArray<TSharedPtr<FString>>>();
	TSharedPtr<FString> Current;
	{
		TSet<FString> Seen;
		for (const FString& Path : Forge->FindCharactersFor(Resolved.ProviderId))
		{
			Options->Add(MakeShared<FString>(Path));
			Seen.Add(Path);
		}

		const FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByClass(UMotionCharacter::StaticClass()->GetClassPathName(), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			const FString Path = Asset.GetSoftObjectPath().ToString();
			if (!Seen.Contains(Path))
			{
				Options->Add(MakeShared<FString>(Path));
			}
		}

		for (const TSharedPtr<FString>& Option : *Options)
		{
			if (*Option == Def->Character.ToString())
			{
				Current = Option;
			}
		}
	}

	auto Describe = [Forge, ProviderId = Resolved.ProviderId](const FString& Path)
	{
		const UMotionCharacter* C = Cast<UMotionCharacter>(FSoftObjectPath(Path).TryLoad());
		if (!C)
		{
			return FText::FromString(FSoftObjectPath(Path).GetAssetName());
		}

		FString Why;
		const bool bSuits = Forge->DoesCharacterSuit(C, ProviderId, &Why);
		const FString Kind = C->ProviderId.IsNone() ? FString(TEXT("universal")) : FString::Printf(TEXT("for %s"), *C->ProviderId.ToString());
		return FText::FromString(bSuits
			? FString::Printf(TEXT("%s  (%s)"), *C->GetDisplayName(), *Kind)
			: FString::Printf(TEXT("%s  (%s, needs attention)"), *C->GetDisplayName(), *Kind));
	};

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	TWeakPtr<SMotionGeneratePanel> WeakThis = SharedThis(this);

	// New from a mesh: the quickest route from nothing to a character.
	TSharedRef<SWidget> NewFromMesh = SNew(SComboButton)
		.ButtonContent()
		[
			SNew(STextBlock).Text(LOCTEXT("NewCharacter", "New from a mesh"))
		]
		.ToolTipText(LOCTEXT("NewCharacterTip", "Make a Motion Character from any skeletal mesh: its skeleton becomes the target, the mesh the preview."))
		.OnGetMenuContent_Lambda([WeakThis]()
		{
			FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

			FAssetPickerConfig Picker;
			Picker.Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
			Picker.Filter.bRecursiveClasses = true;
			Picker.InitialAssetViewType = EAssetViewType::List;
			Picker.bAllowNullSelection = false;
			Picker.OnAssetSelected = FOnAssetSelected::CreateLambda([WeakThis](const FAssetData& Asset)
			{
				FSlateApplication::Get().DismissAllMenus();
				if (TSharedPtr<SMotionGeneratePanel> Self = WeakThis.Pin())
				{
					Self->CreateCharacterFromMesh(Asset.GetSoftObjectPath().ToString());
				}
			});

			return StaticCastSharedRef<SWidget>(
				SNew(SBox).WidthOverride(360.f).HeightOverride(420.f)
				[
					ContentBrowser.Get().CreateAssetPicker(Picker)
				]);
		});

	Body->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&Options.Get())
			.InitiallySelectedItem(Current)
			.OnGenerateWidget_Lambda([Describe, Options](TSharedPtr<FString> Item)
			{
				return SNew(STextBlock).Text(Describe(*Item));
			})
			.OnSelectionChanged_Lambda([WeakThis, Options](TSharedPtr<FString> Item, ESelectInfo::Type Info)
			{
				TSharedPtr<SMotionGeneratePanel> Self = WeakThis.Pin();
				if (Self.IsValid() && Item.IsValid() && Info != ESelectInfo::Direct)
				{
					Self->SetCharacter(*Item);
				}
			})
			[
				SNew(STextBlock)
				.Text(Character ? Describe(Character->GetPathName()) : LOCTEXT("NoCharacter", "Choose a character"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenCharacter", "Open"))
			.ToolTipText(LOCTEXT("OpenCharacterTip", "Open the character: its skeleton, mesh, provider and setup steps."))
			.IsEnabled(Character != nullptr)
			.OnClicked_Lambda([Character]()
			{
				if (Character && GEditor)
				{
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Character);
				}
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
		[
			NewFromMesh
		]
	];

	if (Character && !Resolved.CharacterRoute.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			MFS::Fact(LOCTEXT("Route", "Clips are"), FText::FromString(Resolved.CharacterRoute))
		];
	}

	const bool bCharacterBlocked = MotionGeneratePrivate::IsCharacterBlocker(Resolved.Readiness.Blocker);
	if (bCharacterBlocked)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			MFS::Note(FText::FromString(Resolved.Readiness.Problem), MFS::Warn(), "Icons.Warning")
		];
	}

	// The fix the core can make whatever the provider: a provider rig with no retargeter imports
	// directly once the rig is forgotten.
	if (Character && Resolved.Readiness.Blocker == EMotionBlocker::RetargetIncomplete
		&& !Character->ProviderMesh.IsNull() && Character->Retargeter.IsNull())
	{
		const FString Path = Character->GetPathName();
		Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DirectFixText", "Import straight onto the character's skeleton instead. Right for a character the provider generates on."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("ImportDirectly", "Import directly"))
				.OnClicked_Lambda([WeakThis, Path]()
				{
					if (UMotionForgeSubsystem* F = UMotionForgeSubsystem::Get())
					{
						FString Said;
						const bool bOk = F->ClearCharacterProviderRig(Path, Said);
						if (TSharedPtr<SMotionGeneratePanel> Self = WeakThis.Pin())
						{
							Self->SetMessage(Said, !bOk);
							Self->Resolve();
						}
					}
					return FReply::Handled();
				})
			]
		];
	}

	// The provider's own steps for this character: upload it, build a rig for it.
	if (Character && Provider.IsValid())
	{
		TArray<FMotionCharacterSetupAction> Actions;
		Provider->GetCharacterSetupActions(Character, Actions);

		for (const FMotionCharacterSetupAction& Action : Actions)
		{
			Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
			[
				MakeActionButton(Action, Action.bRequired && !Action.bDone)
			];
		}
	}

	if (!Character)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			MFS::Note(FText::Format(LOCTEXT("NoCharacterHelp", "No character yet. Pick one above, or make one with New from a mesh - any skeletal mesh will do. {0}"),
				Provider.IsValid() && Provider->GetCaps().bSupportsCharacterUpload
					? FText::Format(LOCTEXT("NeedsUpload", "{0} then needs it uploaded, which is free."), FText::FromString(ProviderName))
					: LOCTEXT("DirectWorks", "It works straight away.")),
				MFS::Info())
		];
	}

	CharacterBox->SetContent(Body);
}

TSharedRef<SWidget> SMotionGeneratePanel::MakeActionButton(const FMotionCharacterSetupAction& Action, bool bPrimary)
{
	TWeakPtr<SMotionGeneratePanel> WeakThis = SharedThis(this);
	const FMotionCharacterSetupAction Copy = Action;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			MFS::Dot(Action.bDone ? MFS::Good() : (Action.bRequired ? MFS::Warn() : MFS::Quiet()))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Action.Status)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
			.Text(Action.Label)
			.ToolTipText(Action.Tooltip)
			.IsEnabled_Lambda([WeakThis]() { TSharedPtr<SMotionGeneratePanel> Self = WeakThis.Pin(); return Self.IsValid() && Self->RunningAction.IsEmpty(); })
			.OnClicked_Lambda([WeakThis, Copy]()
			{
				if (TSharedPtr<SMotionGeneratePanel> Self = WeakThis.Pin())
				{
					Self->RunAction(Copy);
				}
				return FReply::Handled();
			})
		]
	];

	// The provider's own options for this step - upload options, a pose to pin - drawn beside it.
	if (Action.Options.IsValid())
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));

		FDetailsViewArgs Args;
		Args.bAllowSearch = false;
		Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		Args.bHideSelectionTip = true;

		FStructureDetailsViewArgs StructArgs;
		TSharedRef<IStructureDetailsView> Form = PropertyModule.CreateStructureDetailView(Args, StructArgs, Action.Options);

		Box->AddSlot().AutoHeight().Padding(16.f, 4.f, 0.f, 0.f)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(true)
			.AreaTitle(FText::Format(LOCTEXT("OptionsFmt", "{0}: options"), Action.Label))
			.BodyContent()
			[
				Form->GetWidget().ToSharedRef()
			]
		];
	}

	return Box;
}

void SMotionGeneratePanel::RunAction(const FMotionCharacterSetupAction& Action)
{
	if (!Action.Run)
	{
		return;
	}

	if (!Action.Confirmation.IsEmpty()
		&& FMessageDialog::Open(EAppMsgType::OkCancel, Action.Confirmation, Action.Label) != EAppReturnType::Ok)
	{
		return;
	}

	RunningAction = Action.Label.ToString();
	RunningSince = FDateTime::Now();
	LastActivityStamp = 0;

	TWeakPtr<SMotionGeneratePanel> WeakThis = SharedThis(this);
	const FText Label = Action.Label;

	Action.Run([WeakThis, Label](bool bOk, const FString& Said)
	{
		TSharedPtr<SMotionGeneratePanel> Self = WeakThis.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		Self->RunningAction.Reset();
		Self->LastActivityStamp = 0;
		Self->LastCharacterStamp = 0;

		// A long answer - a payload, a diagnosis - gets a window; a sentence goes under the button.
		if (Said.Len() > 400 || Said.Contains(TEXT("\n")))
		{
			MotionGeneratePrivate::ShowText(Label, Said);
			Self->SetMessage(FString::Printf(TEXT("%s: done. The answer opened in its own window."), *Label.ToString()), !bOk);
		}
		else
		{
			Self->SetMessage(Said, !bOk);
		}

		Self->Resolve();
	});
}

void SMotionGeneratePanel::SetCharacter(const FString& Path)
{
	UMotionDef* Def = Definition.Get();
	if (!Def || Def->Character.ToString() == Path)
	{
		return;
	}

	FScopedTransaction Transaction(LOCTEXT("SetCharacter", "Set Motion Character"));
	Def->Modify();
	Def->Character = TSoftObjectPtr<UMotionCharacter>(FSoftObjectPath(Path));
	Def->CharacterByProvider.Add(Def->GetResolvedProviderId(), Def->Character);
	Def->MarkPackageDirty();

	Resolve();
	LastCharacterStamp = 0;
}

void SMotionGeneratePanel::CreateCharacterFromMesh(const FString& MeshPath)
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Forge)
	{
		return;
	}

	FString Error;
	const FString Created = Forge->CreateCharacterFromMesh(MeshPath, Error);
	if (Created.IsEmpty())
	{
		SetMessage(Error, true);
		return;
	}

	SetCharacter(Created);
	SetMessage(FString::Printf(TEXT("Made %s from %s."), *FSoftObjectPath(Created).GetAssetName(), *FSoftObjectPath(MeshPath).GetAssetName()), false);
}

// -------------------------------------------------------------------------------------------------
// Generate
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionGeneratePanel::BuildGenerateCard()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();

	TSharedRef<TArray<TSharedPtr<FName>>> Providers = MakeShared<TArray<TSharedPtr<FName>>>();
	if (Forge)
	{
		for (const FName Id : Forge->GetProviderIds())
		{
			Providers->Add(MakeShared<FName>(Id));
		}
	}

	auto ProviderLabel = [](FName Id)
	{
		TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Id);
		if (!P.IsValid())
		{
			return FText::FromName(Id);
		}
		return FText::Format(LOCTEXT("ProviderOptionFmt", "{0}  -  {1}"),
			FText::FromString(P->GetDisplayName()), FText::FromString(P->GetBilling().Summary));
	};

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			MFS::Fact(LOCTEXT("Provider", "Provider"),
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SComboBox<TSharedPtr<FName>>)
					.OptionsSource(&Providers.Get())
					.OnGenerateWidget_Lambda([ProviderLabel, Providers](TSharedPtr<FName> Item)
					{
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(*Item);
						return SNew(STextBlock)
							.Text(ProviderLabel(*Item))
							.ToolTipText(P.IsValid() ? P->GetTagline() : FText::GetEmpty());
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FName> Item, ESelectInfo::Type Info)
					{
						if (Item.IsValid() && Info != ESelectInfo::Direct)
						{
							SetProvider(*Item);
						}
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this, ProviderLabel]()
						{
							return Resolved.ProviderId.IsNone()
								? LOCTEXT("ChooseProvider", "Choose a provider")
								: ProviderLabel(Resolved.ProviderId);
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId);
						FText Tagline = P.IsValid() ? P->GetTagline() : FText::GetEmpty();
						return Resolved.bProviderInherited
							? FText::Format(LOCTEXT("InheritedFmt", "The project default. {0}"), Tagline)
							: Tagline;
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f).HAlign(HAlign_Left)
				[
					SNew(SButton)
					.Text_Lambda([this]()
					{
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId);
						return P.IsValid() ? FText::Format(LOCTEXT("OpenSurfaceFmt", "Open {0}"), P->GetSetupSurfaceLabel()) : FText::GetEmpty();
					})
					.ToolTipText(LOCTEXT("OpenSurfaceTip", "Where this provider is started, stopped and paid for."))
					.Visibility_Lambda([this]()
					{
						TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId);
						return P.IsValid() && !P->GetSetupSurfaceLabel().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]()
					{
						if (TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId))
						{
							P->OpenSetupSurface();
						}
						return FReply::Handled();
					})
				])
		]

		// The provider's own settings, by its own names, with Advanced collapsed.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SAssignNew(ProviderSettingsBox, SBox)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			MFS::Fact(LOCTEXT("Takes", "Takes"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(90.f)
					[
						SNew(SSpinBox<int32>)
						.MinValue(1).MaxValue(16).Delta(1)
						.Value_Lambda([this]() { return Definition.IsValid() ? Definition->Variants : 1; })
						.OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type)
						{
							if (UMotionDef* D = Definition.Get())
							{
								FScopedTransaction Transaction(LOCTEXT("SetVariants", "Set Takes"));
								D->Modify();
								D->Variants = FMath::Clamp(Value, 1, 16);
								D->MarkPackageDirty();
								Resolve();
							}
						})
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						switch (Resolved.Cost.Unit)
						{
						case EMotionBillingUnit::Free:               return LOCTEXT("TakesFree", "Free here, so ask for several and pick the best.");
						case EMotionBillingUnit::PerGeneratedSecond: return LOCTEXT("TakesPaid", "Each take is billed when submitted, kept or not.");
						case EMotionBillingUnit::PerDownloadedSecond:return LOCTEXT("TakesQuota", "Generating is free on your plan; importing uses quota.");
						case EMotionBillingUnit::PerHour:            return LOCTEXT("TakesHourly", "No charge per take; the rented GPU bills by the hour.");
						}
						return FText::GetEmpty();
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				])
		]

		// Exactly what goes out, from the resolver the submission uses.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(true)
			.AreaTitle(LOCTEXT("WillBeSent", "What will be sent"))
			.BodyContent()
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					TArray<FString> Lines;
					Lines.Add(FString::Printf(TEXT("provider: %s, model %s"), *Resolved.ProviderDisplayName, *Resolved.ModelId));
					Lines.Add(FString::Printf(TEXT("prompt: %s"), *Resolved.Prompt));
					Lines.Add(FString::Printf(TEXT("length: %s%s"), *MFS::Seconds(Resolved.LengthSeconds),
						Resolved.bSplitIntoBeats ? *FString::Printf(TEXT(" in %d beat(s)"), Resolved.BeatCount) : TEXT("")));
					Lines.Add(FString::Printf(TEXT("takes: %d"), Resolved.Variants));
					for (const FString& Setting : Resolved.Settings)
					{
						Lines.Add(Setting);
					}
					if (!Resolved.CharacterRoute.IsEmpty())
					{
						Lines.Add(FString::Printf(TEXT("character: %s, %s"), *Resolved.CharacterName, *Resolved.CharacterRoute));
					}
					if (!Resolved.Constraints.IsEmpty())
					{
						Lines.Add(FString::Printf(TEXT("poses: %s"), *Resolved.Constraints));
					}
					return FText::FromString(FString::Join(Lines, TEXT("\n")));
				})
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		// The action bar: the reason or the price on the left, the one button on the right.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(STextBlock)
					.Text(this, &SMotionGeneratePanel::CostLine)
					.ColorAndOpacity(this, &SMotionGeneratePanel::CostColour)
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(this, &SMotionGeneratePanel::GenerateLabel)
					.ToolTipText(this, &SMotionGeneratePanel::GenerateTooltip)
					.IsEnabled(this, &SMotionGeneratePanel::CanGenerate)
					.OnClicked(this, &SMotionGeneratePanel::OnGenerate)
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return FText::FromString(Message); })
			.ColorAndOpacity_Lambda([this]() { return FSlateColor(bMessageIsProblem ? MFS::Bad() : MFS::Good()); })
			.Visibility_Lambda([this]() { return Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.AutoWrapText(true)
		];

	return SNew(SMotionSection)
		.Number(3)
		.Title(LOCTEXT("GenerateTitle", "Generate"))
		.Subtitle(LOCTEXT("GenerateSub", "The provider, its own settings, and how many takes. The button says what it will cost."))
		.Summary_Lambda([this]()
		{
			return FText::Format(LOCTEXT("GenerateSummaryFmt", "{0}  -  {1} {1}|plural(one=take,other=takes)"),
				FText::FromString(Resolved.ProviderDisplayName), FMath::Max(1, Resolved.Variants));
		})
		.State_Lambda([this]()
		{
			if (Resolved.bCanSubmit)
			{
				return EMotionStepState::Current;
			}
			return MotionGeneratePrivate::IsCharacterBlocker(Resolved.Readiness.Blocker) || Resolved.Prompt.TrimStartAndEnd().IsEmpty()
				? EMotionStepState::Todo : EMotionStepState::Attention;
		})
		[
			Body
		];
}

void SMotionGeneratePanel::RebuildProvider()
{
	UMotionDef* Def = Definition.Get();
	LastProvider = Resolved.ProviderId;

	if (!ProviderSettingsBox.IsValid() || !Def)
	{
		return;
	}

	// The definition's own copy of this provider's settings, created with the provider's defaults the
	// first time it is used. The details panel draws whatever class the provider declared.
	UMotionPipeline* Pipeline = Def->GetOrCreatePipeline(Resolved.ProviderId);

	if (!Pipeline)
	{
		ProviderSettingsBox->SetContent(
			SNew(STextBlock)
			.Text(LOCTEXT("NoSettings", "This provider has no settings beyond the prompt, length and takes."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground()));
		return;
	}

	PipelineDetails->SetObject(Pipeline);
	ProviderSettingsBox->SetContent(PipelineDetails.ToSharedRef());
}

void SMotionGeneratePanel::SetProvider(FName ProviderId)
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Def || !Forge || ProviderId == Resolved.ProviderId)
	{
		return;
	}

	CommitPrompt();
	const FString Said = Forge->SetDefinitionProvider(Def->GetPathName(), ProviderId);
	SetMessage(Said, false);

	Resolve();
	RebuildProvider();
	RebuildDirect();
	LastCharacterStamp = 0;
	RebuildCharacter();

	Forge->RefreshProviderState(ProviderId);
}

FText SMotionGeneratePanel::GenerateLabel() const
{
	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return LOCTEXT("Generate", "Generate");
	}

	if (Def->IsBusy())
	{
		return LOCTEXT("Working", "Working...");
	}

	const int32 Takes = Resolved.Variants;
	const EMotionBlocker Blocker = Resolved.Readiness.Blocker;

	// When a step is missing, the button is the fix where one exists.
	if (!Resolved.bCanSubmit)
	{
		return Resolved.Readiness.FixLabel.IsEmpty()
			? FText::Format(LOCTEXT("GenerateN", "Generate {0}"), Takes)
			: FText::FromString(Resolved.Readiness.FixLabel);
	}

	if (Blocker == EMotionBlocker::ProviderStartable)
	{
		return FText::Format(LOCTEXT("StartThenGenerateFmt", "{0}, then generate {1}"), FText::FromString(Resolved.Readiness.FixLabel), Takes);
	}

	if (Resolved.Cost.bSpendsMoney && Resolved.Cost.EstimatedCost > 0.f)
	{
		// A list price nobody entered is marked here too; the tooltip and the confirmation say whose.
		return MotionGeneratePrivate::RateNote(Resolved.ProviderId).IsEmpty()
			? FText::Format(LOCTEXT("GeneratePaidFmt", "Generate {0}  ·  {1}"), Takes,
				FText::FromString(MFS::Money(Resolved.Cost.EstimatedCost, Resolved.Cost.Currency)))
			: FText::Format(LOCTEXT("GeneratePaidListFmt", "Generate {0}  ·  {1} at list price"), Takes,
				FText::FromString(MFS::Money(Resolved.Cost.EstimatedCost, Resolved.Cost.Currency)));
	}

	return FText::Format(LOCTEXT("GenerateN2", "Generate {0}"), Takes);
}

FText SMotionGeneratePanel::GenerateTooltip() const
{
	if (!Resolved.bCanSubmit)
	{
		return FText::FromString(Resolved.Readiness.Problem);
	}

	return Resolved.Readiness.Blocker == EMotionBlocker::ProviderStartable
		? FText::Format(LOCTEXT("StartTipFmt", "{0} It is started for you first, then the takes are submitted. The first start can take several minutes."),
			FText::FromString(Resolved.Readiness.Problem))
		: Resolved.Cost.bSpendsMoney && !MotionGeneratePrivate::RateNote(Resolved.ProviderId).IsEmpty()
		? FText::Format(LOCTEXT("GenerateTipListFmt", "Submit the takes. They appear on the left as they finish, and play on the character before you choose one.\n\nThe price is {0}."),
			FText::FromString(MotionGeneratePrivate::RateNote(Resolved.ProviderId)))
		: LOCTEXT("GenerateTip", "Submit the takes. They appear on the left as they finish, and play on the character before you choose one.");
}

bool SMotionGeneratePanel::CanGenerate() const
{
	const UMotionDef* Def = Definition.Get();
	if (!Def || Def->IsBusy())
	{
		return false;
	}

	if (Resolved.bCanSubmit)
	{
		return true;
	}

	// A fix button is enabled; a reason with nothing to press is not.
	return !Resolved.Readiness.FixLabel.IsEmpty();
}

FText SMotionGeneratePanel::CostLine() const
{
	if (!Resolved.bCanSubmit)
	{
		return FText::FromString(Resolved.Readiness.Problem);
	}

	return FText::FromString(Resolved.Cost.Summary);
}

FSlateColor SMotionGeneratePanel::CostColour() const
{
	if (!Resolved.bCanSubmit)
	{
		return MotionGeneratePrivate::IsCharacterBlocker(Resolved.Readiness.Blocker) || Resolved.Readiness.Blocker == EMotionBlocker::NoPrompt
			? FSlateColor(MFS::Warn())
			: FSlateColor(MFS::Bad());
	}

	return (Resolved.Cost.bSpendsMoney || Resolved.Cost.bHourlyBillingNow)
		? FSlateColor(MFS::Warn())
		: FSlateColor::UseSubduedForeground();
}

FReply SMotionGeneratePanel::OnGenerate()
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Def || !Forge)
	{
		return FReply::Handled();
	}

	CommitPrompt();
	Resolve();

	if (!Resolved.bCanSubmit)
	{
		// The button was the fix.
		const FString& Fix = Resolved.Readiness.FixLabel;
		if (Fix == TEXT("Open Keys"))
		{
			if (!MFS::TryOpenTab(FName("ForgeKeys")))
			{
				if (TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId))
				{
					P->OpenSetupSurface();
				}
			}
		}
		else if (TSharedPtr<IMotionProvider> P = MotionGeneratePrivate::Provider(Resolved.ProviderId))
		{
			P->OpenSetupSurface();
		}
		return FReply::Handled();
	}

	// Money is asked about before it is spent, with the count and the price.
	if (Resolved.Cost.bSpendsMoney)
	{
		const FText Question = FText::Format(
			LOCTEXT("ConfirmSpendFmt", "Generate {0} {0}|plural(one=take,other=takes) of \"{1}\" on {2}?\n\n{3}"),
			Resolved.Variants, FText::FromString(Def->GetName()), FText::FromString(Resolved.ProviderDisplayName),
			FText::FromString(Resolved.Cost.Summary));

		if (FMessageDialog::Open(EAppMsgType::OkCancel, Question, LOCTEXT("ConfirmSpendTitle", "This spends money")) != EAppReturnType::Ok)
		{
			return FReply::Handled();
		}
	}

	const FString BatchId = Forge->Generate({ Def->GetPathName() });

	if (BatchId.IsEmpty())
	{
		SetMessage(Def->LastError.IsEmpty() ? FString(TEXT("Nothing was submitted.")) : Def->LastError, true);
	}
	else
	{
		SetMessage(Resolved.Readiness.Blocker == EMotionBlocker::ProviderStartable
			? FString::Printf(TEXT("Starting %s first. The takes are submitted as soon as it is ready."), *Resolved.ProviderDisplayName)
			: FString::Printf(TEXT("Submitted %d %s."), Resolved.Variants, Resolved.Variants == 1 ? TEXT("take") : TEXT("takes")), false);
	}

	Resolve();
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// Direct
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionGeneratePanel::BuildDirectCard()
{
	// Closed until wanted: a first motion needs none of it, and a person directing a take opens it.
	return SAssignNew(DirectSection, SMotionSection)
		.Title(LOCTEXT("DirectTitle", "Direct"))
		.Subtitle_Lambda([this]()
		{
			return FText::Format(LOCTEXT("DirectSubFmt", "{0} can be guided beyond the words: beats, poses at moments, and how strictly to follow each."),
				FText::FromString(Resolved.ProviderDisplayName));
		})
		.Summary_Lambda([this]()
		{
			const UMotionDef* Def = Definition.Get();
			const int32 Keys = Def ? Def->Control.CountAuthoredKeys() : 0;
			return Keys > 0
				? FText::Format(LOCTEXT("DirectSummaryFmt", "{0} pinned pose {0}|plural(one=key,other=keys)"), Keys)
				: LOCTEXT("DirectSummaryNone", "poses, guidance and seeds");
		})
		.InitiallyExpanded(false)
		.Visibility(EVisibility::Collapsed)
		[
			SAssignNew(DirectBox, SBox)
		];
}

void SMotionGeneratePanel::RebuildDirect()
{
	UMotionDef* Def = Definition.Get();
	TSharedPtr<IMotionProvider> Provider = MotionGeneratePrivate::Provider(Resolved.ProviderId);

	if (!DirectBox.IsValid() || !Def || !Provider.IsValid())
	{
		return;
	}

	TArray<FMotionCharacterSetupAction> Actions;
	Provider->GetDirectActions(Def, Actions);
	const FText Guide = Provider->GetDirectingGuide();

	// A provider directed by its prompt alone gets no card: a section for tools that do not exist
	// would be an advertisement wearing a control's clothes.
	if (Actions.Num() == 0 && Guide.IsEmpty() && Provider->GetCaps().ConstraintTypes.Num() == 0)
	{
		DirectBox->SetContent(SNullWidget::NullWidget);
		DirectSection->SetVisibility(EVisibility::Collapsed);
		return;
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	if (!Guide.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(false)
			.AreaTitle(FText::Format(LOCTEXT("GuideFmt", "How to direct {0}"), FText::FromString(Provider->GetDisplayName())))
			.BodyContent()
			[
				SNew(STextBlock).Text(Guide).AutoWrapText(true)
			]
		];
	}

	Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
	[
		MFS::Fact(LOCTEXT("Poses", "Poses"),
			SNew(STextBlock)
			.Text_Lambda([this]() { return FText::FromString(Resolved.Constraints); })
			.AutoWrapText(true))
	];

	const UMotionCharacter* Character = Def->Character.Get();
	if (Character && Character->ControlRig.IsNull())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
		[
			MFS::Note(LOCTEXT("NoRig", "The character has no Control Rig, so the prompt timeline cannot pose it. Set one on the character to pin poses there."), MFS::Warn(), "Icons.Warning")
		];
	}

	Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f).HAlign(HAlign_Left)
	[
		SNew(SButton)
		.Text_Lambda([this]()
		{
			const UMotionDef* D = Definition.Get();
			return D && !D->Control.ConstraintSequence.IsNull()
				? LOCTEXT("OpenTimeline2", "Open prompt timeline")
				: LOCTEXT("CreateTimeline", "Lay the prompt out on a timeline");
		})
		.ToolTipText(LOCTEXT("TimelineTip",
			"The prompt as beats on a Level Sequence, with the character and its Control Rig. Drag beats to retime them; key "
			"the rig at a moment to pin the body there. The timeline then is the prompt."))
		.OnClicked_Lambda([this]() { OpenPromptTimeline(); return FReply::Handled(); })
	];

	for (const FMotionCharacterSetupAction& Action : Actions)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			MakeActionButton(Action, false)
		];
	}

	DirectBox->SetContent(Body);
	DirectSection->SetVisibility(EVisibility::Visible);
}

void SMotionGeneratePanel::OpenPromptTimeline()
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Def || !Forge)
	{
		return;
	}

	CommitPrompt();

	if (ULevelSequence* Existing = Def->Control.ConstraintSequence.LoadSynchronous())
	{
		FString RefreshError;
		Forge->RefreshPromptSequenceTake(Def->GetPathName(), RefreshError);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Existing);
		return;
	}

	FString Error;
	const FString Created = Forge->CreatePromptSequence(Def->GetPathName(), FString(), Error);
	if (Created.IsEmpty())
	{
		SetMessage(FString::Printf(TEXT("Could not lay the prompt out on a timeline: %s"), *Error), true);
		return;
	}

	if (UObject* Sequence = LoadObject<UObject>(nullptr, *Created))
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
	}
	Resolve();
}

// -------------------------------------------------------------------------------------------------
// Import
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionGeneratePanel::BuildImportCard()
{
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			ImportDetails.ToSharedRef()
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			MFS::Fact(LOCTEXT("RootTravel", "Root travel"),
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					return UMotionForgeSettings::Get()->bZeroRootTranslation
						? LOCTEXT("RootZeroed", "removed: clips play on the spot (project setting)")
						: LOCTEXT("RootKept", "kept: clips move through the world (project setting)");
				})
				.AutoWrapText(true))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const UMotionDef* D = Definition.Get();
						const FMotionCandidate* Chosen = D ? D->FindSelectedCandidate() : nullptr;
						if (!Chosen)
						{
							return LOCTEXT("NoChosen", "Choose a take on the left first.");
						}
						return !UMotionForgeSubsystem::FindTakeFile(*Chosen).IsEmpty()
							? FText::Format(LOCTEXT("ReimportFreeFmt", "Import {0} again with these settings. Free: it is already on disk."), FText::FromString(Chosen->GetLabel()))
							: FText::Format(LOCTEXT("ReimportFmt", "Import {0} again with these settings."), FText::FromString(Chosen->GetLabel()));
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ImportAgain", "Import again"))
					.ToolTipText(LOCTEXT("ImportAgainTip", "Re-import the chosen take after changing the trim or the retargeter. The cheap loop: nothing is generated."))
					.IsEnabled_Lambda([this]()
					{
						const UMotionDef* D = Definition.Get();
						const FMotionCandidate* Chosen = D ? D->FindSelectedCandidate() : nullptr;
						return D && !D->IsBusy() && Chosen && Chosen->IsUsable();
					})
					.OnClicked_Lambda([this]()
					{
						if (UMotionDef* D = Definition.Get())
						{
							if (UMotionForgeSubsystem* F = UMotionForgeSubsystem::Get())
							{
								F->DownloadSelected({ D->GetPathName() });
							}
						}
						return FReply::Handled();
					})
				]
			]
		];

	return SNew(SMotionSection)
		.Title(LOCTEXT("ImportTitle", "Import"))
		.Subtitle(LOCTEXT("ImportSub", "What happens to a take on its way into the project. Changing these needs no new generation."))
		.Summary(LOCTEXT("ImportSummary", "trim, root motion, the clip's name"))
		.InitiallyExpanded(false)
		[
			Body
		];
}

#undef LOCTEXT_NAMESPACE
