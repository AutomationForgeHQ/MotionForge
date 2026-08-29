// Copyright Blackcode SA. All rights reserved.

#include "SMotionDefTakes.h"

#include "MotionDef.h"
#include "MotionForgeSubsystem.h"

#include "HAL/PlatformProcess.h"
#include "Framework/Docking/TabManager.h"
#include "IMotionProvider.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

namespace
{
	const FLinearColor GoodColour(0.30f, 0.78f, 0.45f);
	const FLinearColor WarnColour(0.95f, 0.65f, 0.20f);
	const FLinearColor BadColour(0.90f, 0.36f, 0.36f);
	const FLinearColor QuietColour(0.55f, 0.55f, 0.58f);

	// The shared vocabulary from PANEL_RULES.md. Copied rather than shared, deliberately: a plugin
	// installed on its own carries its own presentation, and three small functions are a far
	// cheaper duplication than a dependency between plugins.
	TSharedRef<SBorder> Card(TSharedRef<SWidget> Content)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(16.f, 14.f))
			[
				Content
			];
	}

	TSharedRef<SWidget> Heading(const FText& Text)
	{
		return SNew(STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
			.Text(Text)
			.ColorAndOpacity(FSlateColor(QuietColour))
			.TransformPolicy(ETextTransformPolicy::ToUpper);
	}

	/** Money, always to two places. "about $3.5" reads like a typo, because it is one. */
	FText Money(double Amount)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = 2;
		Options.MaximumFractionalDigits = 2;
		return FText::AsNumber(Amount, &Options);
	}

	/** The status line: what this definition is doing, in words rather than an enum name. */
	void DescribeStatus(const UMotionDef& Def, FText& OutText, FLinearColor& OutColour)
	{
		switch (Def.Status)
		{
		case EMotionDefStatus::Draft:
			OutText = LOCTEXT("StatusDraft", "Draft - nothing generated yet");
			OutColour = QuietColour;
			break;

		case EMotionDefStatus::Generating:
			OutText = LOCTEXT("StatusGenerating", "Generating - takes appear here as they finish");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::AwaitingReview:
			OutText = LOCTEXT("StatusReview", "Awaiting review - choose a take");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::Downloading:
			OutText = LOCTEXT("StatusDownloading", "Downloading the chosen take");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::Processing:
			OutText = LOCTEXT("StatusProcessing", "Normalising and importing");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::Ready:
			OutText = LOCTEXT("StatusReady", "Ready - the animation is in the project");
			OutColour = GoodColour;
			break;

		case EMotionDefStatus::Failed:
			OutText = LOCTEXT("StatusFailed", "Failed");
			OutColour = BadColour;
			break;

		default:
			OutText = FText::GetEmpty();
			OutColour = QuietColour;
			break;
		}
	}

	void DescribeTake(const FMotionCandidate& Candidate, FText& OutText, FLinearColor& OutColour)
	{
		switch (Candidate.Status)
		{
		case EMotionJobStatus::Pending:
			OutText = LOCTEXT("TakeQueued", "Queued");
			OutColour = QuietColour;
			break;

		case EMotionJobStatus::Running:
			OutText = LOCTEXT("TakeRunning", "Generating");
			OutColour = WarnColour;
			break;

		case EMotionJobStatus::Finished:
			OutText = LOCTEXT("TakeFinished", "Ready to use");
			OutColour = GoodColour;
			break;

		case EMotionJobStatus::Failed:
			OutText = LOCTEXT("TakeFailed", "Failed");
			OutColour = BadColour;
			break;

		default:
			OutText = FText::GetEmpty();
			OutColour = QuietColour;
			break;
		}
	}

	/** A coloured dot, the same vocabulary the Keys page uses. */
	TSharedRef<SWidget> Dot(const FLinearColor& Colour)
	{
		return SNew(SBox)
			.WidthOverride(8.f)
			.HeightOverride(8.f)
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
				.ColorAndOpacity(Colour)
			];
	}

	FText WhenText(const FDateTime& When)
	{
		return When == FDateTime()
			? FText::GetEmpty()
			: FText::AsDateTime(When, EDateTimeStyle::Short, EDateTimeStyle::Short);
	}
}

// -------------------------------------------------------------------------------------------------

void SMotionDefTakes::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			.Padding(16.f, 14.f, 16.f, 20.f)
			[
				SAssignNew(Body, SVerticalBox)
			]
		]
	];

	// One tick a second, all the while this editor is open. It compares a fingerprint and usually
	// does nothing, which is what lets generation appear here without anyone pressing refresh.
	RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SMotionDefTakes::Poll));

	// Somebody else moved the provider: a container started from the runner panel, a pod released,
	// a key set. Without this the window kept drawing whatever was true when it opened, and the
	// only way to see a runner that had come up was to close the asset and open it again.
	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		ProviderStateHandle = Subsystem->OnProviderStateChanged().AddSP(
			this, &SMotionDefTakes::OnProviderStateChanged);
	}

	Refresh();
}

SMotionDefTakes::~SMotionDefTakes()
{
	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		Subsystem->OnProviderStateChanged().Remove(ProviderStateHandle);
	}
}

void SMotionDefTakes::OnProviderStateChanged(FName ProviderId)
{
	// Only when it is the provider this definition draws. Two definitions on different providers
	// should not redraw each other.
	if (ProviderId == ResolveCaps().ProviderId)
	{
		Refresh();
	}
}

EActiveTimerReturnType SMotionDefTakes::Poll(double, float InDeltaTime)
{
	if (Fingerprint() != LastFingerprint)
	{
		Refresh();
	}

	// And every ten seconds, ask the provider itself. The broadcast covers anything that happens
	// inside the editor; this covers everything that does not - a container stopped from a
	// terminal, a pod released from the hub, a machine that went to sleep.
	SinceProviderPoll += InDeltaTime;
	if (SinceProviderPoll >= 10.f)
	{
		SinceProviderPoll = 0.f;

		if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
		{
			Subsystem->RefreshProviderState(ResolveCaps().ProviderId);
		}
	}

	return EActiveTimerReturnType::Continue;
}

FReply SMotionDefTakes::OnRefreshProvider()
{
	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		SinceProviderPoll = 0.f;
		Subsystem->RefreshProviderState(ResolveCaps().ProviderId);
	}
	return FReply::Handled();
}

uint32 SMotionDefTakes::Fingerprint() const
{
	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return 0;
	}

	uint32 Hash = HashCombine(GetTypeHash((uint8)Def->Status), GetTypeHash(Def->SelectedMotionId));
	Hash = HashCombine(Hash, GetTypeHash(Def->Candidates.Num()));
	Hash = HashCombine(Hash, GetTypeHash(Def->LastError));
	Hash = HashCombine(Hash, GetTypeHash(Def->Variants));
	Hash = HashCombine(Hash, GetTypeHash(Def->Length));
	Hash = HashCombine(Hash, GetTypeHash(Def->ProviderId));

	for (const FMotionCandidate& Candidate : Def->Candidates)
	{
		Hash = HashCombine(Hash, GetTypeHash(Candidate.MotionId));
		Hash = HashCombine(Hash, GetTypeHash((uint8)Candidate.Status));
		Hash = HashCombine(Hash, GetTypeHash((uint8)Candidate.bDownloaded));
	}

	// The provider's readiness is drawn here too, so it belongs in what decides a redraw. Leaving
	// it out was why a runner coming up changed nothing on screen: every field this compared
	// belonged to the asset, and none of them had moved.
	const FMotionProviderCaps Caps = ResolveCaps();
	Hash = HashCombine(Hash, GetTypeHash(Caps.SetupHint));
	Hash = HashCombine(Hash, GetTypeHash(Caps.ProviderId));
	Hash = HashCombine(Hash, GetTypeHash((uint8)Caps.bIsMetered));

	return Hash;
}

FMotionProviderCaps SMotionDefTakes::ResolveCaps() const
{
	const UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	if (!Def || !Subsystem)
	{
		return FMotionProviderCaps();
	}

	// None resolves to the project's default provider, which is exactly what an unset field means.
	return Subsystem->GetProviderCaps(Def->ProviderId);
}

// -------------------------------------------------------------------------------------------------

void SMotionDefTakes::Refresh()
{
	LastFingerprint = Fingerprint();

	if (!Body.IsValid())
	{
		return;
	}

	Body->ClearChildren();

	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return;
	}

	const FMotionProviderCaps Caps = ResolveCaps();

	// Same shape as the Kimodo panel, for the reasons in PANEL_RULES.md: state in a card, then the
	// cost of the next action, then a quiet heading over the list.
	Body->AddSlot().AutoHeight()[ Card(BuildHeader(Caps)) ];
	Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[ BuildCostLine(Caps) ];

	Body->AddSlot().AutoHeight().Padding(0.f, 22.f, 0.f, 8.f)
	[
		Heading(Def->Candidates.Num() == 0
			? LOCTEXT("TakesHeading", "Takes")
			: FText::Format(LOCTEXT("TakesHeadingCountFmt", "Takes ({0})"),
				FText::AsNumber(Def->Candidates.Num())))
	];

	if (Def->Candidates.Num() == 0)
	{
		Body->AddSlot().AutoHeight()[ Card(BuildNothingYet(Caps)) ];
		return;
	}

	for (const FMotionCandidate& Candidate : Def->Candidates)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
		[
			BuildCard(Candidate)
		];
	}
}

TSharedRef<SWidget> SMotionDefTakes::BuildHeader(const FMotionProviderCaps& Caps)
{
	const UMotionDef* Def = Definition.Get();
	check(Def);

	FText StatusText;
	FLinearColor StatusColour;
	DescribeStatus(*Def, StatusText, StatusColour);

	TSharedRef<SVerticalBox> Header = SNew(SVerticalBox);

	// Status, as one sentence with a dot - the thing to read first.
	Header->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			Dot(StatusColour)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(StatusText)
		]
	];

	// Which provider, and whether it charges. Both decide what the buttons below should say.
	const FText ProviderLine = Caps.ProviderId.IsNone()
		? LOCTEXT("NoProvider", "No motion provider is available.")
		: FText::Format(
			LOCTEXT("ProviderLineFmt", "{0}{1} - {2}"),
			FText::FromString(Caps.DisplayName),
			Caps.bIsLocal ? LOCTEXT("LocalSuffix", " (on this machine)") : FText::GetEmpty(),
			Caps.bIsMetered
				? LOCTEXT("Metered", "every generated second is billed")
				: LOCTEXT("Unmetered", "generation is free"));

	// Where this provider is set up, when it has somewhere beyond a key - a container to start, a
	// GPU to rent. Asked of the provider rather than looked up by plugin name, so a provider added
	// later gets the same button and MotionForge never learns what a container is.
	FText SetupLabel;
	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		if (TSharedPtr<IMotionProvider> Provider = Subsystem->FindProvider(Caps.ProviderId))
		{
			SetupLabel = Provider->GetSetupSurfaceLabel();
		}
	}

	TSharedRef<SHorizontalBox> ProviderRow = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(ProviderLine)
			.ColorAndOpacity(FSlateColor(QuietColour))
			.AutoWrapText(true)
		]

		// Ask the machine now. The window listens for changes and polls every ten seconds, so this
		// is rarely needed - but "rarely needed" is not "never", and waiting ten seconds while
		// staring at a stale line is its own small misery.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("RefreshProviderTip",
				"Ask the provider whether it is ready now, instead of waiting for the next check."))
			.OnClicked(this, &SMotionDefTakes::OnRefreshProvider)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Refresh"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];

	if (!SetupLabel.IsEmpty())
	{
		const FName ProviderId = Caps.ProviderId;

		ProviderRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			// Leads only when the provider is saying it cannot work yet - which is exactly when
			// somebody wants to go and start a container or rent a card.
			.ButtonStyle(FAppStyle::Get(), Caps.SetupHint.IsEmpty() ? "SimpleButton" : "PrimaryButton")
			.Text(FText::Format(LOCTEXT("OpenSetupFmt", "Open {0}"), SetupLabel))
			.ToolTipText(LOCTEXT("OpenSetupTip",
				"Where this provider is started, stopped and paid for. Generating needs it running."))
			.OnClicked_Lambda([ProviderId]()
			{
				if (UMotionForgeSubsystem* S = UMotionForgeSubsystem::Get())
				{
					if (TSharedPtr<IMotionProvider> P = S->FindProvider(ProviderId))
					{
						P->OpenSetupSurface();
					}
				}
				return FReply::Handled();
			})
		];
	}

	Header->AddSlot().AutoHeight().Padding(16.f, 4.f, 0.f, 0.f)[ ProviderRow ];

	// The one thing standing between this definition and a generation, asked of the same code that
	// submission asks. Offering Generate while this is set would be a button that cannot work.
	//
	// The provider's own setup hint is not repeated here: when the provider is what is missing this
	// sentence already contains it, and the button beside the provider line is the way out.
	FMotionReadiness Readiness;
	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		Readiness = Subsystem->CheckReadiness(Def->GetPathName());
	}
	LastReadiness = Readiness;

	if (!Readiness.bCanGenerate && !Readiness.Problem.IsEmpty())
	{
		TSharedRef<SHorizontalBox> BlockedRow = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 2.f, 8.f, 0.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Warning"))
				.ColorAndOpacity(FSlateColor(WarnColour))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Readiness.Problem))
				.ColorAndOpacity(FSlateColor(WarnColour))
				.AutoWrapText(true)
			];

		// A missing key has one obvious next step, and it is not in this window.
		if (Readiness.Blocker == EMotionBlocker::NoCredential
			&& FGlobalTabmanager::Get()->HasTabSpawner(FName("ForgeKeys")))
		{
			BlockedRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("OpenKeys", "Open Keys"))
				.OnClicked_Lambda([]()
				{
					FGlobalTabmanager::Get()->TryInvokeTab(FName("ForgeKeys"));
					return FReply::Handled();
				})
			];
		}

		Header->AddSlot().AutoHeight().Padding(16.f, 8.f, 0.f, 0.f)[ BlockedRow ];
	}

	if (!Def->LastError.IsEmpty())
	{
		Header->AddSlot().AutoHeight().Padding(16.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Def->LastError))
			.ColorAndOpacity(FSlateColor(BadColour))
			.AutoWrapText(true)
		];
	}

	return Header;
}

TSharedRef<SWidget> SMotionDefTakes::BuildCostLine(const FMotionProviderCaps& Caps)
{
	const UMotionDef* Def = Definition.Get();
	check(Def);

	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	// What pressing Generate would cost, before it is pressed. The whole reason Variants defaults
	// to one: this line has to be able to say "twice the price" out loud.
	FText GenerateCost = LOCTEXT("CostUnknown", "");
	if (Subsystem)
	{
		FMotionDefSpec Spec;
		Spec.Length     = Def->Length;
		Spec.Variants   = Def->Variants;
		Spec.ProviderId = Def->ProviderId;
		Spec.ModelId    = Def->ModelId;

		const FMotionCostEstimate Estimate = Subsystem->EstimateGenerationCost({ Spec });

		// Numbers rather than FText::AsNumber, so the plural forms below can inflect on them.
		const int32 Takes = FMath::Max(1, Def->Variants);

		if (!Caps.bIsMetered)
		{
			GenerateCost = FText::Format(
				LOCTEXT("CostFreeFmt",
					"{0} {0}|plural(one=take,other=takes) of {1}s - free on this provider, so ask for more"),
				Takes, Def->Length);
		}
		else if (Estimate.EstimatedCost > 0.f)
		{
			GenerateCost = FText::Format(
				LOCTEXT("CostMoneyFmt",
					"{0} {0}|plural(one=take,other=takes) x {1}s = {2}s billed, about {3} {4}"),
				Takes, Def->Length, Estimate.BilledSeconds,
				Money(Estimate.EstimatedCost), FText::FromString(Estimate.Currency));
		}
		else
		{
			GenerateCost = FText::Format(
				LOCTEXT("CostSecondsFmt",
					"{0} {0}|plural(one=take,other=takes) x {1}s = {2}s billed"),
				Takes, Def->Length, Estimate.BilledSeconds);
		}
	}

	// The cost of pressing Generate, spelled out before it is pressed. Verbs live on the toolbar;
	// this line is the context that decides whether to use one.
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(12.f, 8.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(Caps.bIsMetered ? "Icons.Warning" : "Icons.Info"))
				.ColorAndOpacity(FSlateColor(Caps.bIsMetered ? WarnColour : QuietColour))
			]

			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(GenerateCost)
				.ColorAndOpacity(FSlateColor(Caps.bIsMetered ? WarnColour : QuietColour))
				.AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SMotionDefTakes::BuildNothingYet(const FMotionProviderCaps& Caps)
{
	return SNew(STextBlock)
		.Text(Caps.ProviderId.IsNone()
			? LOCTEXT("NoTakesNoProvider",
				"No takes. Set a provider in Editor Preferences before generating.")
			: LOCTEXT("NoTakes",
				"No takes yet. Write the prompt, then press Generate."))
		.ColorAndOpacity(FSlateColor(QuietColour))
		.AutoWrapText(true);
}

TSharedRef<SWidget> SMotionDefTakes::BuildCard(const FMotionCandidate& Candidate)
{
	const UMotionDef* Def = Definition.Get();
	check(Def);

	const bool bChosen   = !Def->SelectedMotionId.IsEmpty() && Def->SelectedMotionId == Candidate.MotionId;
	const bool bUsable   = Candidate.Status == EMotionJobStatus::Finished;

	FText TakeStatus;
	FLinearColor TakeColour;
	DescribeTake(Candidate, TakeStatus, TakeColour);

	TSharedRef<SVerticalBox> Card = SNew(SVerticalBox);

	// Row one: which take, what state, when.
	TSharedRef<SHorizontalBox> TopRow = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			Dot(TakeColour)
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(FText::Format(LOCTEXT("TakeNFmt", "Take {0}"), FText::AsNumber(Candidate.VariantIndex + 1)))
		]

		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(TakeStatus)
			.ColorAndOpacity(FSlateColor(TakeColour))
		];

	if (bChosen)
	{
		TopRow->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(LOCTEXT("Chosen", "Chosen"))
			.ColorAndOpacity(FSlateColor(GoodColour))
		];
	}

	Card->AddSlot().AutoHeight()[ TopRow ];

	// Row two: the durable handle, and when it was made. The motion id is the only route back to a
	// take on a provider that cannot reproduce one, so it is on the card rather than in a tooltip.
	TSharedRef<SHorizontalBox> IdRow = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Candidate.MotionId))
			.ColorAndOpacity(FSlateColor(QuietColour))
			.ToolTipText(LOCTEXT("MotionIdTip",
				"The provider's own id for this take. On a provider with no seed this is the only "
				"way back to it, which is why takes are never pruned."))
		];

	if (Candidate.GeneratedAt != FDateTime())
	{
		IdRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(WhenText(Candidate.GeneratedAt))
			.ColorAndOpacity(FSlateColor(QuietColour))
		];
	}

	Card->AddSlot().AutoHeight().Padding(16.f, 2.f, 0.f, 0.f)[ IdRow ];

	if (!Candidate.Error.IsEmpty())
	{
		Card->AddSlot().AutoHeight().Padding(16.f, 4.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Candidate.Error))
			.ColorAndOpacity(FSlateColor(BadColour))
			.AutoWrapText(true)
		];
	}

	// Row three: what can be done with this take.
	TSharedRef<SHorizontalBox> Actions = SNew(SHorizontalBox);

	// Watching costs nothing and is the right first look wherever a provider offers it. A local
	// runner returns nothing here on purpose - there is no cheaper place than the imported clip.
	if (!Candidate.ViewerUrl.IsEmpty())
	{
		const FString Url = Candidate.ViewerUrl;
		Actions->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SHyperlink)
			.Text(LOCTEXT("Watch", "Watch"))
			.ToolTipText(LOCTEXT("WatchTip", "Open this take in the provider's viewer. Free."))
			.OnNavigate_Lambda([Url]() { FPlatformProcess::LaunchURL(*Url, nullptr, nullptr); })
		];
	}

	if (Candidate.bDownloaded)
	{
		Actions->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("OnDisk", "already fetched"))
			.ToolTipText(LOCTEXT("OnDiskTip", "The raw file is in the staging directory. Fetching it again costs nothing."))
			.ColorAndOpacity(FSlateColor(QuietColour))
		];
	}

	Actions->AddSlot().FillWidth(1.f)[ SNew(SSpacer) ];

	if (!bChosen)
	{
		const FString MotionId = Candidate.MotionId;
		Actions->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("Choose", "Choose"))
			.ToolTipText(bUsable
				? LOCTEXT("ChooseTip", "Use this take. Choosing costs nothing; importing is the next button up.")
				: LOCTEXT("ChooseTipUnusable", "This take has not finished generating."))
			.IsEnabled(bUsable)
			.OnClicked(this, &SMotionDefTakes::OnChoose, MotionId)
		];
	}

	Card->AddSlot().AutoHeight().Padding(16.f, 8.f, 0.f, 0.f)[ Actions ];

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush(bChosen ? "Brushes.Header" : "Brushes.Recessed"))
		.Padding(12.f, 10.f)
		[
			Card
		];
}

// -------------------------------------------------------------------------------------------------

FReply SMotionDefTakes::OnChoose(FString MotionId)
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	if (Def && Subsystem)
	{
		Subsystem->SelectCandidate(Def->GetPathName(), MotionId);
		Refresh();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
