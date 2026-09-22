#include "SMotionGetStarted.h"

#include "MotionForgeEditorStyle.h"

#include "IMotionProvider.h"
#include "MotionCharacter.h"
#include "MotionDef.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "IContentBrowserSingleton.h"
#include "IStructureDetailsView.h"
#include "Misc/MessageDialog.h"
#include "ObjectTools.h"
#include "PropertyEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"

#define LOCTEXT_NAMESPACE "MotionForgeGetStarted"

// Qualified rather than pulled in with a using-directive: a file-scope `using namespace` leaks into
// every file after this one in a unity build (see SMotionLibrary.cpp).
namespace MFS = MotionForgeStyle;

namespace MotionGetStartedPrivate
{
	FLinearColor StateColour(EMotionSetupState State)
	{
		switch (State)
		{
		case EMotionSetupState::Done:    return MFS::Good();
		case EMotionSetupState::Todo:    return MFS::Warn();
		case EMotionSetupState::Waiting: return MFS::Info();
		default:                         return MFS::Quiet();
		}
	}

	/** Can generate now, will get itself ready when asked, or needs a person first. */
	enum class EReadiness : uint8 { Ready, StartsItself, NeedsSetup };

	EReadiness ReadinessOf(const FMotionProviderCaps& Caps)
	{
		if (Caps.SetupHint.IsEmpty())
		{
			return EReadiness::Ready;
		}
		return Caps.PrepareLabel.IsEmpty() ? EReadiness::NeedsSetup : EReadiness::StartsItself;
	}

	/** "A tired person sits down on a chair" -> "MD_TiredPersonSitsDown". Words that say nothing are skipped. */
	FString NameFromPrompt(const FString& Prompt)
	{
		static const TSet<FString> Skip =
		{
			TEXT("a"), TEXT("an"), TEXT("the"), TEXT("person"), TEXT("and"), TEXT("then"), TEXT("with"),
			TEXT("their"), TEXT("his"), TEXT("her"), TEXT("of"), TEXT("on"), TEXT("to"), TEXT("in"), TEXT("at")
		};

		TArray<FString> Words;
		Prompt.ParseIntoArrayWS(Words);

		FString Name;
		int32 Used = 0;
		for (FString Word : Words)
		{
			Word = Word.TrimStartAndEnd();
			FString Clean;
			for (const TCHAR Char : Word)
			{
				if (FChar::IsAlnum(Char))
				{
					Clean.AppendChar(Char);
				}
			}

			if (Clean.IsEmpty() || Skip.Contains(Clean.ToLower()))
			{
				continue;
			}

			Clean[0] = FChar::ToUpper(Clean[0]);
			Name += Clean;

			if (++Used == 4)
			{
				break;
			}
		}

		return Name.IsEmpty() ? FString(TEXT("MD_FirstMotion")) : TEXT("MD_") + Name;
	}
}

// -------------------------------------------------------------------------------------------------

void SMotionGetStarted::Construct(const FArguments& InArgs)
{
	OnOpenLibrary = InArgs._OnOpenLibrary;

	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();

	// Start on the provider the project already uses; failing that the first one that is ready.
	if (Forge)
	{
		ChosenProvider = Forge->GetProviderCaps(NAME_None).ProviderId;

		if (ChosenProvider.IsNone()
			|| MotionGetStartedPrivate::ReadinessOf(Forge->GetProviderCaps(ChosenProvider)) == MotionGetStartedPrivate::EReadiness::NeedsSetup)
		{
			for (const FName Id : Forge->GetProviderIds())
			{
				if (MotionGetStartedPrivate::ReadinessOf(Forge->GetProviderCaps(Id)) != MotionGetStartedPrivate::EReadiness::NeedsSetup)
				{
					ChosenProvider = Id;
					break;
				}
			}
		}

		if (ChosenProvider.IsNone() && Forge->GetProviderIds().Num() > 0)
		{
			ChosenProvider = Forge->GetProviderIds()[0];
		}

		ChosenCharacter = PickCharacter(ChosenProvider);

		// A provider saying its state moved is a reason to look now rather than at the next tick - and
		// no more than that. Forcing a redraw on every announcement is what made this page collapse and
		// redraw every few seconds while another window kept asking Kimodo how it was.
		ProviderStateHandle = Forge->OnProviderStateChanged().AddSP(this, &SMotionGetStarted::OnProviderStateChanged);

		// Measured on arrival rather than trusted from whenever it was last asked: Docker may have been
		// started, a key stored, an access request granted since.
		for (const FName Id : Forge->GetProviderIds())
		{
			Forge->RefreshProviderState(Id);
		}
	}

	LastSetupSignature = MeasureSetup();
	LastCharacterSignature = MeasureCharacter();
	UpdateSummaries();

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(16.f, 14.f, 16.f, 24.f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
				.Text(LOCTEXT("Title", "Make your first motion"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("Intro",
					"Three steps: choose where motion is made, who it is for, and what happens. Nothing is "
					"generated or spent on this page. The last step opens the motion's own window, where "
					"Generate shows its price on the button before you press it."))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
			[
				SAssignNew(ProvidersStep, SMotionSection)
				.Number(1)
				.Title(LOCTEXT("Step1", "Where motion is made"))
				.Subtitle(LOCTEXT("Step1Sub",
					"Each provider has its own setup, measured here. Pick one to start with; every motion can "
					"use either later."))
				.Summary_Lambda([this]() { return ProvidersSummary; })
				.State_Lambda([this]() { return ProvidersState; })
				.InitiallyExpanded(ProvidersState != EMotionStepState::Done)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				SAssignNew(CharacterStep, SMotionSection)
				.Number(2)
				.Title(LOCTEXT("Step2", "Who it is for"))
				.Subtitle_Lambda([this]() { return CharacterSubtitle; })
				.Summary_Lambda([this]() { return CharacterSummary; })
				.State_Lambda([this]() { return CharacterState; })
				.InitiallyExpanded(CharacterState != EMotionStepState::Done)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
			[
				SAssignNew(FirstMotionStep, SMotionSection)
				.Number(3)
				.Title(LOCTEXT("Step3", "What happens"))
				.Subtitle(LOCTEXT("Step3Sub",
					"Who moves, what they do, and how it ends. Start with \"A person\"; one or two actions work "
					"best. Everything here can be changed later in the motion's window."))
				.State_Lambda([this]()
				{
					return ProvidersState == EMotionStepState::Done && CharacterState == EMotionStepState::Done
						? EMotionStepState::Current : EMotionStepState::Todo;
				})
				.InitiallyExpanded(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Text(LOCTEXT("LibraryHint", "Every motion you make is listed in the library, with what state it is in."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenLibrary", "Open the library"))
					.OnClicked_Lambda([this]()
					{
						OnOpenLibrary.ExecuteIfBound();
						return FReply::Handled();
					})
				]
			]
		]
	];

	RebuildProviders();
	RebuildCharacter();
	RebuildFirstMotion();

	RegisterActiveTimer(1.f, FWidgetActiveTimerDelegate::CreateSP(this, &SMotionGetStarted::Poll));
}

SMotionGetStarted::~SMotionGetStarted()
{
	if (UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get())
	{
		Forge->OnProviderStateChanged().Remove(ProviderStateHandle);
	}
}

// -------------------------------------------------------------------------------------------------
// Keeping current, without redrawing what has not changed
// -------------------------------------------------------------------------------------------------

uint32 SMotionGetStarted::MeasureSetup()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Forge)
	{
		return 0;
	}

	uint32 Hash = GetTypeHash(ChosenProvider);
	for (const FName Id : Forge->GetProviderIds())
	{
		TSharedPtr<IMotionProvider> Provider = Forge->FindProvider(Id);
		if (!Provider.IsValid())
		{
			continue;
		}

		TArray<FMotionSetupStep> Steps;
		Provider->GetSetupSteps(Steps);

		TArray<FText>& Details = StepDetails.FindOrAdd(Id);
		Details.Reset(Steps.Num());

		for (const FMotionSetupStep& Step : Steps)
		{
			// The detail is read live by its row, so it moves - "Starting the runner (2 min so far)" -
			// without anything being rebuilt. Only what changes the row's shape is in the signature.
			Details.Add(Step.Detail);

			Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Step.State)));
			Hash = HashCombine(Hash, GetTypeHash(Step.Label.ToString()));
			Hash = HashCombine(Hash, GetTypeHash(Step.ActionLabel.ToString()));
			Hash = HashCombine(Hash, GetTypeHash(Step.HelpUrl));
			Hash = HashCombine(Hash, GetTypeHash(Step.bOptional));
		}

		const FMotionProviderCaps Caps = Provider->GetCaps();
		const FMotionBilling Billing = Provider->GetBilling();
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(MotionGetStartedPrivate::ReadinessOf(Caps))));
		Hash = HashCombine(Hash, GetTypeHash(Billing.Summary));
		Hash = HashCombine(Hash, GetTypeHash(Billing.bBillingNow));
	}
	return Hash;
}

uint32 SMotionGetStarted::MeasureCharacter() const
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();

	uint32 Hash = HashCombine(GetTypeHash(ChosenProvider), GetTypeHash(ChosenCharacter));

	// Resolved, not loaded: a character not in memory has not changed since it was drawn.
	const UMotionCharacter* Character = Cast<UMotionCharacter>(FSoftObjectPath(ChosenCharacter).ResolveObject());
	if (Forge && Character)
	{
		FString Why;
		EMotionBlocker Blocker = EMotionBlocker::None;
		Hash = HashCombine(Hash, GetTypeHash(Forge->DoesCharacterSuit(Character, ChosenProvider, &Why, &Blocker)));
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Blocker)));
		Hash = HashCombine(Hash, GetTypeHash(Character->ProviderId));
		Hash = HashCombine(Hash, GetTypeHash(Character->ProviderCharacterId));
		Hash = HashCombine(Hash, GetTypeHash(Character->ProviderMesh.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Character->Retargeter.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Character->TargetSkeleton.ToString()));
	}
	return Hash;
}

void SMotionGetStarted::UpdateSummaries()
{
	using namespace MotionGetStartedPrivate;

	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	TSharedPtr<IMotionProvider> Provider = Forge ? Forge->FindProvider(ChosenProvider) : nullptr;
	const FString ProviderName = Provider.IsValid() ? Provider->GetDisplayName() : ChosenProvider.ToString();

	if (!Provider.IsValid())
	{
		ProvidersSummary = LOCTEXT("SummaryNoProvider", "No provider installed");
		ProvidersState = EMotionStepState::Attention;
	}
	else
	{
		const EReadiness Readiness = ReadinessOf(Provider->GetCaps());
		ProvidersSummary = FText::Format(LOCTEXT("ProviderSummaryFmt", "{0}  -  {1}  -  {2}"),
			FText::FromString(ProviderName),
			Readiness == EReadiness::Ready ? LOCTEXT("SummaryReady", "ready")
				: (Readiness == EReadiness::StartsItself ? LOCTEXT("SummaryStarts", "starts when you generate")
					: LOCTEXT("SummaryNeedsSetup", "needs setup")),
			FText::FromString(Provider->GetBilling().Summary));
		ProvidersState = Readiness == EReadiness::NeedsSetup ? EMotionStepState::Attention : EMotionStepState::Done;
	}

	const UMotionCharacter* Character = Cast<UMotionCharacter>(FSoftObjectPath(ChosenCharacter).ResolveObject());
	if (!Character && !ChosenCharacter.IsEmpty())
	{
		Character = Cast<UMotionCharacter>(FSoftObjectPath(ChosenCharacter).TryLoad());
	}

	const bool bSuits = Forge && Character && Forge->DoesCharacterSuit(Character, ChosenProvider);

	CharacterSummary = !Character
		? LOCTEXT("SummaryNoCharacter", "no character yet")
		: FText::Format(bSuits ? LOCTEXT("SummaryCharacterReadyFmt", "{0}  -  ready for {1}")
				: LOCTEXT("SummaryCharacterFixFmt", "{0}  -  needs a fix for {1}"),
			FText::FromString(Character->GetDisplayName()), FText::FromString(ProviderName));

	CharacterState = bSuits ? EMotionStepState::Done
		: (ProvidersState != EMotionStepState::Done ? EMotionStepState::Todo
			: (Character ? EMotionStepState::Attention : EMotionStepState::Current));

	CharacterSubtitle = FText::Format(LOCTEXT("Step2SubFmt",
		"A Motion Character: the skeleton clips are built for and the mesh they play on. {0} needs one "
		"prepared for it."), FText::FromString(ProviderName));
}

EActiveTimerReturnType SMotionGetStarted::Poll(double, float)
{
	// A runner starting, an access request granted: the rows say so without anyone pressing refresh,
	// and only a change in a row's shape redraws it.
	const uint32 Setup = MeasureSetup();
	if (Setup != LastSetupSignature)
	{
		LastSetupSignature = Setup;
		RebuildProviders();
	}

	const uint32 CharacterSignature = MeasureCharacter();
	if (CharacterSignature != LastCharacterSignature)
	{
		LastCharacterSignature = CharacterSignature;
		RebuildCharacter();
	}

	// Not the prompt step: rebuilding it would take the caret away from somebody typing while a
	// runner starts. What it shows that depends on the other steps is read live.
	UpdateSummaries();
	return EActiveTimerReturnType::Continue;
}

void SMotionGetStarted::OnProviderStateChanged(FName)
{
	// Look now, redraw only if something changed shape.
	Poll(0.0, 0.f);
}

// -------------------------------------------------------------------------------------------------
// Step 1: where motion is made
// -------------------------------------------------------------------------------------------------

void SMotionGetStarted::RebuildProviders()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!ProvidersStep.IsValid() || !Forge)
	{
		return;
	}

	const TArray<FName> Ids = Forge->GetProviderIds();

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 Index = 0; Index < Ids.Num(); ++Index)
	{
		Row->AddSlot()
			.FillWidth(1.f)
			.Padding(Index == 0 ? 0.f : 5.f, 0.f, Index == Ids.Num() - 1 ? 0.f : 5.f, 0.f)
			[
				MakeProviderCard(Ids[Index])
			];
	}

	ProvidersStep->SetContent(Ids.Num() > 0
		? StaticCastSharedRef<SWidget>(Row)
		: MFS::Note(LOCTEXT("NoProviders",
			"No motion provider is installed. Enable MotionForgeKimodo (free, runs on your GPU) or "
			"MotionForgeUthana (a paid service) in Edit > Plugins, then restart the editor."), MFS::Warn(), "Icons.Warning"));

	UpdateSummaries();
}

TSharedRef<SWidget> SMotionGetStarted::MakeProviderCard(FName ProviderId)
{
	using namespace MotionGetStartedPrivate;

	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	TSharedPtr<IMotionProvider> Provider = Forge ? Forge->FindProvider(ProviderId) : nullptr;
	if (!Provider.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	const FMotionProviderCaps Caps = Provider->GetCaps();
	const FMotionBilling Billing = Provider->GetBilling();
	const EReadiness Readiness = ReadinessOf(Caps);
	const bool bChosen = ChosenProvider == ProviderId;

	FText ReadyText;
	FLinearColor ReadyColour;
	switch (Readiness)
	{
	case EReadiness::Ready:
		ReadyText = LOCTEXT("ProviderReady", "Ready");
		ReadyColour = MFS::Good();
		break;
	case EReadiness::StartsItself:
		ReadyText = LOCTEXT("ProviderStarts", "Starts when you generate");
		ReadyColour = MFS::Info();
		break;
	default:
		ReadyText = LOCTEXT("ProviderNeedsSetup", "Needs setup");
		ReadyColour = MFS::Warn();
		break;
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	// Header: choose it, its name, and whether it can work.
	Body->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked(bChosen ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.ToolTipText(LOCTEXT("UseProviderTip", "Make the first motion with this provider."))
			.OnCheckStateChanged_Lambda([this, ProviderId](ECheckBoxState)
			{
				ChooseProvider(ProviderId);
			})
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(FText::FromString(Provider->GetDisplayName()))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 6.f, 0.f)
		[
			MFS::Dot(ReadyColour)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(ReadyText)
			.ColorAndOpacity(FSlateColor(ReadyColour))
		]
	];

	if (!Provider->GetTagline().IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(Provider->GetTagline())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
	}

	// What it costs, in its own words. Money is the first thing a person asks of a service, and a
	// machine billing right now is the one cost nothing else on screen shows.
	const FLinearColor PaidColour = MFS::Warn();
	Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
	[
		MFS::Fact(LOCTEXT("Costs", "Costs"),
			FText::FromString(Billing.Summary.IsEmpty() ? FString(TEXT("-")) : Billing.Summary),
			Billing.Unit == EMotionBillingUnit::Free && !Billing.bBillingNow ? nullptr : &PaidColour)
	];

	// The setup, row by row: what is done, what is not, and the button that moves it on.
	TArray<FMotionSetupStep> Steps;
	Provider->GetSetupSteps(Steps);

	TSharedRef<SVerticalBox> StepList = SNew(SVerticalBox);
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		const FMotionSetupStep& Step = Steps[Index];
		const FLinearColor Colour = StateColour(Step.State);
		const TFunction<void()> Action = Step.Action;
		const FString HelpUrl = Step.HelpUrl;

		TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
		if (!HelpUrl.IsEmpty() && Step.State != EMotionSetupState::Done)
		{
			Buttons->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SHyperlink)
				.Text(Step.HelpLabel.IsEmpty() ? LOCTEXT("OpenPage", "Open page") : Step.HelpLabel)
				.ToolTipText(FText::FromString(HelpUrl))
				.OnNavigate_Lambda([HelpUrl]() { FPlatformProcess::LaunchURL(*HelpUrl, nullptr, nullptr); })
			];
		}
		if (!Step.ActionLabel.IsEmpty() && Action)
		{
			// The step that is holding things up gets the primary colour; the rest stay quiet.
			const bool bPrimary = Step.State == EMotionSetupState::Todo && !Step.bOptional;
			Buttons->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
				.Text(Step.ActionLabel)
				.OnClicked_Lambda([Action, ProviderId]()
				{
					Action();
					// Whatever it started, measure again rather than wait for the provider to say.
					if (UMotionForgeSubsystem* F = UMotionForgeSubsystem::Get())
					{
						F->RefreshProviderState(ProviderId);
					}
					return FReply::Handled();
				})
			];
		}

		StepList->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 4.f, 8.f, 0.f)
			[
				MFS::Dot(Colour)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(Step.bOptional
						? FText::Format(LOCTEXT("OptionalFmt", "{0}  (optional)"), Step.Label)
						: Step.Label)
					.ColorAndOpacity(Step.bOptional ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f, 0.f, 0.f)
				[
					// Live: a runner's progress changes this every few seconds, and redrawing the page
					// for it is what made everything collapse and reappear.
					SNew(STextBlock)
					.Text_Lambda([this, ProviderId, Index]()
					{
						const TArray<FText>* Details = StepDetails.Find(ProviderId);
						return Details && Details->IsValidIndex(Index) ? (*Details)[Index] : FText::GetEmpty();
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				Buttons
			]
		];
	}

	Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)[ StepList ];

	// The provider's own window, for everything past a first motion.
	if (!Provider->GetSetupSurfaceLabel().IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)[ SNew(SSpacer) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(Provider->GetSetupSurfaceLabel())
				.OnClicked_Lambda([Provider]()
				{
					Provider->OpenSetupSurface();
					return FReply::Handled();
				})
			]
		];
	}

	// Which one is chosen reads before any text does: an accent edge on it, a quiet one on the rest.
	return SNew(SBorder)
		.BorderImage(MFS::ChoiceBrush(bChosen))
		.Padding(FMargin(12.f, 10.f))
		[
			Body
		];
}

void SMotionGetStarted::ChooseProvider(FName ProviderId)
{
	if (ChosenProvider == ProviderId)
	{
		RebuildProviders();   // the radio button unticked itself; put it back
		return;
	}

	ChosenProvider = ProviderId;

	// A character prepared for the old provider does not suit the new one. Keep it only if it does.
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	const UMotionCharacter* Current = Cast<UMotionCharacter>(FSoftObjectPath(ChosenCharacter).TryLoad());
	if (Forge && (!Current || !Forge->DoesCharacterSuit(Current, ProviderId)))
	{
		ChosenCharacter = PickCharacter(ProviderId);
	}

	SetMessage(FString(), false);
	LastSetupSignature = MeasureSetup();
	LastCharacterSignature = MeasureCharacter();
	RebuildProviders();
	RebuildCharacter();
	RebuildFirstMotion();
}

// -------------------------------------------------------------------------------------------------
// Step 2: who it is for
// -------------------------------------------------------------------------------------------------

void SMotionGetStarted::RebuildCharacter()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!CharacterStep.IsValid() || !Forge)
	{
		return;
	}

	TSharedPtr<IMotionProvider> Provider = Forge->FindProvider(ChosenProvider);
	const FString ProviderName = Provider.IsValid() ? Provider->GetDisplayName() : ChosenProvider.ToString();

	UMotionCharacter* Character = Cast<UMotionCharacter>(FSoftObjectPath(ChosenCharacter).TryLoad());

	// Suitable first, then the rest, so a character somebody made for the other provider is still
	// findable - and says why it will not do.
	TSharedRef<TArray<TSharedPtr<FString>>> Options = MakeShared<TArray<TSharedPtr<FString>>>();
	TSharedPtr<FString> Current;
	{
		TSet<FString> Seen;
		for (const FString& Path : Forge->FindCharactersFor(ChosenProvider))
		{
			Options->Add(MakeShared<FString>(Path));
			Seen.Add(Path);
		}

		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> All;
		Registry.Get().GetAssetsByClass(UMotionCharacter::StaticClass()->GetClassPathName(), All);
		for (const FAssetData& Asset : All)
		{
			const FString Path = Asset.GetSoftObjectPath().ToString();
			if (!Seen.Contains(Path))
			{
				Options->Add(MakeShared<FString>(Path));
			}
		}

		for (const TSharedPtr<FString>& Option : *Options)
		{
			if (*Option == ChosenCharacter)
			{
				Current = Option;
			}
		}
	}

	const FName ProviderId = ChosenProvider;
	auto Describe = [Forge, ProviderId](const FString& Path)
	{
		const UMotionCharacter* C = Cast<UMotionCharacter>(FSoftObjectPath(Path).TryLoad());
		if (!C)
		{
			return FText::FromString(FSoftObjectPath(Path).GetAssetName());
		}

		const bool bSuits = Forge->DoesCharacterSuit(C, ProviderId);
		const FString Kind = C->ProviderId.IsNone() ? FString(TEXT("universal")) : FString::Printf(TEXT("for %s"), *C->ProviderId.ToString());
		return FText::FromString(bSuits
			? FString::Printf(TEXT("%s  (%s)"), *C->GetDisplayName(), *Kind)
			: FString::Printf(TEXT("%s  (%s, does not suit)"), *C->GetDisplayName(), *Kind));
	};

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	TWeakPtr<SMotionGetStarted> WeakThis = SharedThis(this);

	TSharedRef<SWidget> NewFromMesh = SNew(SComboButton)
		.ButtonContent()
		[
			SNew(STextBlock).Text(LOCTEXT("NewFromMesh", "New from a mesh"))
		]
		.ToolTipText(LOCTEXT("NewFromMeshTip",
			"Make a Motion Character from any skeletal mesh: its skeleton becomes the target, the mesh the "
			"one clips play on. SKM_Quinn and SKM_Manny work as they are."))
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
				if (TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin())
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
			.IsEnabled(Options->Num() > 0)
			.OnGenerateWidget_Lambda([Describe, Options](TSharedPtr<FString> Item)
			{
				return SNew(STextBlock).Text(Describe(*Item));
			})
			.OnSelectionChanged_Lambda([WeakThis, Options](TSharedPtr<FString> Item, ESelectInfo::Type Info)
			{
				TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
				if (Self.IsValid() && Item.IsValid() && Info != ESelectInfo::Direct)
				{
					Self->ChooseCharacter(*Item);
				}
			})
			[
				SNew(STextBlock)
				.Text(Character ? Describe(Character->GetPathName())
					: (Options->Num() > 0 ? LOCTEXT("PickCharacter", "Choose a character") : LOCTEXT("NoCharacters", "No characters in this project yet")))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenCharacter", "Open"))
			.ToolTipText(LOCTEXT("OpenCharacterTip", "Open the character: its skeleton, mesh, provider and setup."))
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

	if (!Character)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			MFS::Note(FText::Format(
				LOCTEXT("NoCharacterHelpFmt",
					"No character for {0} yet. New from a mesh makes one from any skeletal mesh in a moment. {1}"),
				FText::FromString(ProviderName),
				Provider.IsValid() && Provider->GetCaps().bSupportsCharacterUpload
					? FText::Format(LOCTEXT("NeedsUploadFmt", "{0} then needs it uploaded, which is free and takes a minute."), FText::FromString(ProviderName))
					: LOCTEXT("WorksStraightAway", "It can be used straight away.")),
				MFS::Info())
		];
	}
	else
	{
		if (Provider.IsValid())
		{
			const FString Route = Provider->DescribeCharacterRoute(Character);
			if (!Route.IsEmpty())
			{
				Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					MFS::Fact(LOCTEXT("Route", "Clips are"), FText::FromString(Route))
				];
			}
		}

		FString Why;
		EMotionBlocker Blocker = EMotionBlocker::None;
		if (!Forge->DoesCharacterSuit(Character, ChosenProvider, &Why, &Blocker))
		{
			Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
			[
				MFS::Note(FText::FromString(Why), MFS::Warn(), "Icons.Warning")
			];

			// The fix the core can make whatever the provider: a provider rig with no retargeter
			// imports directly once the rig is forgotten. The same button the definition window has.
			if (Blocker == EMotionBlocker::RetargetIncomplete && !Character->ProviderMesh.IsNull() && Character->Retargeter.IsNull())
			{
				const FString Path = Character->GetPathName();
				TWeakPtr<SMotionGetStarted> WeakFix = SharedThis(this);
				Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("DirectFix", "Or import straight onto the character's own skeleton, which needs no retargeter."))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text(LOCTEXT("ImportDirectly", "Import directly"))
						.OnClicked_Lambda([WeakFix, Path]()
						{
							if (UMotionForgeSubsystem* F = UMotionForgeSubsystem::Get())
							{
								FString Said;
								const bool bOk = F->ClearCharacterProviderRig(Path, Said);
								if (TSharedPtr<SMotionGetStarted> Self = WeakFix.Pin())
								{
									Self->SetMessage(Said, !bOk);
									Self->RebuildCharacter();
								}
							}
							return FReply::Handled();
						})
					]
				];
			}
		}
		else
		{
			Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
			[
				MFS::Note(FText::Format(LOCTEXT("SuitsFmt", "{0} is ready for {1}."),
					FText::FromString(Character->GetDisplayName()), FText::FromString(ProviderName)), MFS::Good(), "Icons.SuccessWithColor")
			];
		}

		// The provider's own steps for this character: upload it, build a rig for it.
		if (Provider.IsValid())
		{
			TArray<FMotionCharacterSetupAction> Actions;
			Provider->GetCharacterSetupActions(Character, Actions);
			for (const FMotionCharacterSetupAction& Action : Actions)
			{
				Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					MakeActionRow(Action)
				];
			}
		}
	}

	CharacterStep->SetContent(Body);
	LastCharacterSignature = MeasureCharacter();
	UpdateSummaries();
}

FString SMotionGetStarted::PickCharacter(FName ProviderId) const
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Forge || ProviderId.IsNone())
	{
		return FString();
	}

	const TArray<FString> Suitable = Forge->FindCharactersFor(ProviderId);
	if (Suitable.Num() > 0)
	{
		return Suitable[0];
	}

	// None suits yet, but one made for this provider is the one to fix, not a reason to start over -
	// the page then shows what it needs.
	FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> All;
	Registry.Get().GetAssetsByClass(UMotionCharacter::StaticClass()->GetClassPathName(), All);
	for (const FAssetData& Asset : All)
	{
		if (const UMotionCharacter* Character = Cast<UMotionCharacter>(Asset.GetAsset()))
		{
			if (Character->ProviderId == ProviderId)
			{
				return Asset.GetSoftObjectPath().ToString();
			}
		}
	}

	return FString();
}

TSharedRef<SWidget> SMotionGetStarted::MakeActionRow(const FMotionCharacterSetupAction& Action)
{
	TWeakPtr<SMotionGetStarted> WeakThis = SharedThis(this);
	const FMotionCharacterSetupAction Copy = Action;
	const bool bPrimary = Action.bRequired && !Action.bDone;

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
			.Text(Action.Status.IsEmpty() ? Action.Tooltip : Action.Status)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
			.Text(Action.Label)
			.ToolTipText(Action.Tooltip)
			.IsEnabled_Lambda([WeakThis]()
			{
				TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
				return Self.IsValid() && Self->RunningAction.IsEmpty();
			})
			.OnClicked_Lambda([WeakThis, Copy]()
			{
				if (TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin())
				{
					Self->RunAction(Copy);
				}
				return FReply::Handled();
			})
		]
	];

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
			.AreaTitle(FText::Format(LOCTEXT("ActionOptionsFmt", "{0}: options"), Action.Label))
			.BodyContent()
			[
				Form->GetWidget().ToSharedRef()
			]
		];
	}

	return Box;
}

void SMotionGetStarted::RunAction(const FMotionCharacterSetupAction& Action)
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
	SetMessage(FString::Printf(TEXT("%s..."), *RunningAction), false);

	TWeakPtr<SMotionGetStarted> WeakThis = SharedThis(this);
	Action.Run([WeakThis](bool bOk, const FString& Said)
	{
		TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		Self->RunningAction.Reset();
		Self->SetMessage(Said, !bOk);
		Self->RebuildCharacter();
	});
}

void SMotionGetStarted::ChooseCharacter(const FString& Path)
{
	ChosenCharacter = Path;
	SetMessage(FString(), false);
	RebuildCharacter();
}

void SMotionGetStarted::CreateCharacterFromMesh(const FString& MeshPath)
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

	ChosenCharacter = Created;
	SetMessage(FString::Printf(TEXT("Made %s from %s."), *FSoftObjectPath(Created).GetAssetName(), *FSoftObjectPath(MeshPath).GetAssetName()), false);
	RebuildCharacter();
}

// -------------------------------------------------------------------------------------------------
// Step 3: what happens
// -------------------------------------------------------------------------------------------------

FString SMotionGetStarted::SuggestName() const
{
	const FString Base = MotionGetStartedPrivate::NameFromPrompt(Prompt);

	// Never an existing name: creating by an existing name updates that definition.
	FString Candidate = Base;
	for (int32 Suffix = 2; DefinitionExists(Candidate) && Suffix < 100; ++Suffix)
	{
		Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
	}
	return Candidate;
}

bool SMotionGetStarted::DefinitionExists(const FString& AssetName)
{
	const FString Package = UMotionForgeSettings::Get()->GetDefinitionsPath() / AssetName;
	FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	return Registry.Get().GetAssetByObjectPath(FSoftObjectPath(Package + TEXT(".") + AssetName)).IsValid();
}

FText SMotionGetStarted::PriceLine() const
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	TSharedPtr<IMotionProvider> Provider = Forge ? Forge->FindProvider(ChosenProvider) : nullptr;
	if (!Provider.IsValid())
	{
		return FText::GetEmpty();
	}

	const FMotionBilling Billing = Provider->GetBilling();
	switch (Billing.Unit)
	{
	case EMotionBillingUnit::PerGeneratedSecond:
		return Billing.Rate > 0.f
			? FText::Format(LOCTEXT("PricePerSecondFmt",
				"One take of {0} s on {1} costs about {2}{3}, billed when it is submitted. The definition's window asks before spending it."),
				LengthSeconds, FText::FromString(Provider->GetDisplayName()),
				FText::FromString(MFS::Money(Billing.Rate * LengthSeconds, Billing.Currency)),
				FText::FromString(Billing.RateNote.IsEmpty() ? FString() : FString::Printf(TEXT(" (%s)"), *Billing.RateNote)))
			: FText::Format(LOCTEXT("PricePerSecondUnknownFmt", "{0} bills generated seconds. The definition's window prices it before you press Generate."),
				FText::FromString(Provider->GetDisplayName()));

	case EMotionBillingUnit::PerDownloadedSecond:
		return FText::Format(LOCTEXT("PriceDownloadFmt",
			"Generating on {0} is included in your plan; importing a take uses {1} s of your download quota."),
			FText::FromString(Provider->GetDisplayName()), LengthSeconds);

	case EMotionBillingUnit::PerHour:
		return FText::Format(LOCTEXT("PriceHourFmt",
			"{0} runs on a rented GPU, billed {1} an hour while it is up, generating or not."),
			FText::FromString(Provider->GetDisplayName()), FText::FromString(MFS::Money(Billing.Rate, Billing.Currency)));

	default:
		return FText::Format(LOCTEXT("PriceFreeFmt", "Free on {0}: nothing is billed, so ask for several takes and keep the best."),
			FText::FromString(Provider->GetDisplayName()));
	}
}

void SMotionGetStarted::RebuildFirstMotion()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!FirstMotionStep.IsValid() || !Forge)
	{
		return;
	}

	TSharedPtr<IMotionProvider> Provider = Forge->FindProvider(ChosenProvider);
	const FMotionProviderCaps Caps = Provider.IsValid() ? Provider->GetCaps() : FMotionProviderCaps();
	const FMotionPromptSplitting Splitting = Provider.IsValid() ? Provider->GetPromptSplitting() : FMotionPromptSplitting();

	const int32 MinLength = FMath::Max(1, Caps.MinLengthSeconds);
	const int32 MaxLength = FMath::Max(MinLength, Splitting.bSplitsAtFullStops
		? FMath::RoundToInt(Splitting.MaxTotalSeconds)
		: Caps.MaxLengthSeconds);
	LengthSeconds = FMath::Clamp(LengthSeconds, MinLength, MaxLength);

	if (!bNameEdited)
	{
		Name = SuggestName();
	}

	TWeakPtr<SMotionGetStarted> WeakThis = SharedThis(this);

	TSharedRef<SWrapBox> Examples = SNew(SWrapBox).UseAllottedSize(true);
	Examples->AddSlot().Padding(0.f, 0.f, 6.f, 0.f)
	[
		SNew(STextBlock).Text(LOCTEXT("Try", "Try:")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];
	for (const FString& Example : MFS::ExamplePrompts())
	{
		Examples->AddSlot().Padding(0.f, 0.f, 10.f, 2.f)
		[
			SNew(SHyperlink)
			.Text(FText::FromString(Example))
			.OnNavigate_Lambda([WeakThis, Example]()
			{
				if (TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin())
				{
					Self->Prompt = Example;
					Self->RebuildFirstMotion();
				}
			})
		];
	}

	const FText LengthNote = Splitting.bSplitsAtFullStops
		? FText::Format(LOCTEXT("LengthBeatsFmt", "Each full stop starts a beat of up to {0} s; {1} s in all."),
			FMath::RoundToInt(Splitting.MaxBeatSeconds), MaxLength)
		: FText::Format(LOCTEXT("LengthRangeFmt", "{0} to {1} s on this provider."), MinLength, MaxLength);

	// Whether steps 1 and 2 are done is read live, so this step does not have to be rebuilt - and
	// lose somebody's caret - when a runner comes up or a character is fixed.
	auto StepsDone = [WeakThis]()
	{
		TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
		return Self.IsValid() && Self->ProvidersState == EMotionStepState::Done && Self->CharacterState == EMotionStepState::Done;
	};

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(64.f)
			[
				SNew(SMultiLineEditableTextBox)
				.Text(FText::FromString(Prompt))
				.HintText(LOCTEXT("PromptHint", "A person walks forward, stops, and looks around."))
				.AutoWrapText(true)
				.OnTextChanged_Lambda([WeakThis](const FText& Text)
				{
					if (TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin())
					{
						Self->Prompt = Text.ToString();
						if (!Self->bNameEdited)
						{
							Self->Name = Self->SuggestName();
						}
					}
				})
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			Examples
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
		[
			MFS::Fact(LOCTEXT("Length", "Length"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(80.f)
					[
						SNew(SSpinBox<int32>)
						.MinValue(MinLength)
						.MaxValue(MaxLength)
						.Value_Lambda([WeakThis]() { TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin(); return Self.IsValid() ? Self->LengthSeconds : 4; })
						.OnValueChanged_Lambda([WeakThis](int32 Value)
						{
							if (TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin())
							{
								Self->LengthSeconds = Value;
							}
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 12.f, 0.f)
				[
					SNew(STextBlock).Text(LOCTEXT("SecondsUnit", "seconds"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LengthNote)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				])
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			MFS::Fact(LOCTEXT("Name", "Name"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SEditableTextBox)
					.Text_Lambda([WeakThis]() { TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin(); return Self.IsValid() ? FText::FromString(Self->Name) : FText::GetEmpty(); })
					.OnTextChanged_Lambda([WeakThis](const FText& Text)
					{
						if (TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin())
						{
							Self->Name = Text.ToString();
							Self->bNameEdited = !Self->Name.IsEmpty();
						}
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("SavedInFmt", "in {0}"), FText::FromString(UMotionForgeSettings::Get()->GetDefinitionsPath())))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				])
		];

	// What stands in the way, said beside the button rather than as a disabled button with no reason.
	Body->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([WeakThis]()
			{
				TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
				if (!Self.IsValid())
				{
					return FText::GetEmpty();
				}
				if (Self->ProvidersState != EMotionStepState::Done)
				{
					return LOCTEXT("NeedProvider", "Finish setting up where motion is made, in step 1.");
				}
				if (Self->CharacterState != EMotionStepState::Done)
				{
					return LOCTEXT("NeedCharacter", "Choose or make a character that suits the provider, in step 2.");
				}
				if (Self->Prompt.TrimStartAndEnd().IsEmpty())
				{
					return LOCTEXT("NeedPrompt", "Say what happens: write a prompt, or pick one of the examples.");
				}
				if (DefinitionExists(ObjectTools::SanitizeObjectName(Self->Name)))
				{
					return LOCTEXT("NameTaken", "A definition of that name already exists. Choose another name.");
				}
				return Self->PriceLine();
			})
			.ColorAndOpacity_Lambda([WeakThis, StepsDone]()
			{
				TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
				const bool bReady = Self.IsValid() && StepsDone() && !Self->Prompt.TrimStartAndEnd().IsEmpty();
				return bReady ? FSlateColor::UseSubduedForeground() : FSlateColor(MFS::Warn());
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.Text(LOCTEXT("Create", "Create and open"))
			.ToolTipText(LOCTEXT("CreateTip",
				"Create the motion definition with this prompt, character and provider, and open its window. "
				"Nothing is generated yet: Generate is in that window, with its price on the button."))
			.IsEnabled_Lambda([WeakThis, StepsDone]()
			{
				TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
				return Self.IsValid() && StepsDone()
					&& !Self->Prompt.TrimStartAndEnd().IsEmpty()
					&& !Self->Name.TrimStartAndEnd().IsEmpty()
					&& !DefinitionExists(ObjectTools::SanitizeObjectName(Self->Name));
			})
			.OnClicked(this, &SMotionGetStarted::OnCreate)
		]
	];

	Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Visibility_Lambda([WeakThis]()
		{
			TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
			return Self.IsValid() && !Self->Message.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		.Text_Lambda([WeakThis]()
		{
			TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
			return Self.IsValid() ? FText::FromString(Self->Message) : FText::GetEmpty();
		})
		.ColorAndOpacity_Lambda([WeakThis]()
		{
			TSharedPtr<SMotionGetStarted> Self = WeakThis.Pin();
			return FSlateColor(Self.IsValid() && Self->bMessageIsProblem ? MFS::Bad() : MFS::Good());
		})
	];

	FirstMotionStep->SetContent(Body);
}

FReply SMotionGetStarted::OnCreate()
{
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Forge)
	{
		return FReply::Handled();
	}

	const FString AssetName = ObjectTools::SanitizeObjectName(Name.TrimStartAndEnd());
	if (AssetName.IsEmpty() || DefinitionExists(AssetName))
	{
		SetMessage(TEXT("Choose a name no other definition uses."), true);
		return FReply::Handled();
	}

	FMotionDefSpec Spec;
	Spec.AssetName = AssetName;
	Spec.Prompt = Prompt.TrimStartAndEnd();
	Spec.Length = LengthSeconds;
	Spec.Variants = 1;
	Spec.ProviderId = ChosenProvider;
	Spec.CharacterAssetPath = ChosenCharacter;

	TArray<FString> Problems;
	const FString Created = Forge->CreateMotionDefChecked(Spec, Problems);
	if (Created.IsEmpty())
	{
		SetMessage(Problems.Num() > 0 ? FString::Join(Problems, TEXT(" ")) : FString(TEXT("The definition could not be created.")), true);
		return FReply::Handled();
	}

	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Created);
	}

	SetMessage(FString::Printf(TEXT("Created %s and opened it. Press Generate there when you are ready."), *AssetName), false);

	// Ready for the next one, so a second motion does not start from the first one's name.
	Prompt.Reset();
	bNameEdited = false;
	RebuildFirstMotion();

	return FReply::Handled();
}

void SMotionGetStarted::SetMessage(const FString& Text, bool bProblem)
{
	Message = Text;
	bMessageIsProblem = bProblem;
}

#undef LOCTEXT_NAMESPACE
