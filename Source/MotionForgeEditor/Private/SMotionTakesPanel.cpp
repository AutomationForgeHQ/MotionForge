#include "SMotionTakesPanel.h"

#include "MotionForgeEditorStyle.h"
#include "SMotionStage.h"

#include "IMotionProvider.h"
#include "MotionCharacter.h"
#include "MotionDef.h"
#include "MotionForgeSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformProcess.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "MotionForgeTakes"

namespace MotionTakesPrivate
{
	const FName ColTake(TEXT("Take"));
	const FName ColWhen(TEXT("When"));
	const FName ColMadeBy(TEXT("MadeBy"));
	const FName ColSeed(TEXT("Seed"));
	const FName ColLength(TEXT("Length"));
	const FName ColCost(TEXT("Cost"));
	const FName ColState(TEXT("State"));

	FString KeyOf(const FMotionCandidate& Take)
	{
		return Take.MotionId.IsEmpty() ? FString::Printf(TEXT("#%d"), Take.TakeNumber) : Take.MotionId;
	}

	FString ProviderName(FName ProviderId)
	{
		if (ProviderId.IsNone())
		{
			return FString();
		}
		if (UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get())
		{
			if (TSharedPtr<IMotionProvider> Provider = Forge->FindProvider(ProviderId))
			{
				// "Kimodo (local)" reads long in a table; the provider is enough there, where it runs is
				// in the Generate card.
				FString Name = Provider->GetDisplayName();
				int32 Paren = INDEX_NONE;
				if (Name.FindChar(TEXT('('), Paren))
				{
					Name = Name.Left(Paren).TrimEnd();
				}
				return Name;
			}
		}
		return ProviderId.ToString();
	}

	/** "Kimodo · RP-v1", the model shortened to what tells two apart. */
	FString MadeBy(const FMotionCandidate& Take)
	{
		if (Take.ProviderId.IsNone())
		{
			return TEXT("-");
		}

		FString Model = Take.ModelId;
		Model.RemoveFromStart(TEXT("Kimodo-SOMA-"));
		Model.RemoveFromStart(TEXT("text-to-motion-"));

		return Model.IsEmpty()
			? ProviderName(Take.ProviderId)
			: FString::Printf(TEXT("%s · %s"), *ProviderName(Take.ProviderId), *Model);
	}

	void DescribeState(const FMotionTakeRow& Row, FText& OutText, FLinearColor& OutColour)
	{
		const FMotionCandidate& Take = Row.Take;
		const FTimespan Since = FDateTime::Now() - Take.GeneratedAt;

		switch (Take.Status)
		{
		case EMotionJobStatus::Pending:
			OutText = Take.JobId.IsEmpty()
				? LOCTEXT("StateSubmitting", "submitting")
				: FText::Format(LOCTEXT("StateQueuedFmt", "queued {0}"), FText::FromString(MotionForgeStyle::Clock(Since)));
			OutColour = MotionForgeStyle::Quiet();
			break;

		case EMotionJobStatus::Running:
			OutText = Take.bLate
				? FText::Format(LOCTEXT("StateLateFmt", "late {0}"), FText::FromString(MotionForgeStyle::Clock(Since)))
				: FText::Format(LOCTEXT("StateRunningFmt", "generating {0}"), FText::FromString(MotionForgeStyle::Clock(Since)));
			OutColour = MotionForgeStyle::Warn();
			break;

		case EMotionJobStatus::Finished:
			OutText = Row.bInGame
				? LOCTEXT("StateInGame", "in the game")
				: (!Row.File.IsEmpty() ? LOCTEXT("StateOnDisk", "ready, on disk") : LOCTEXT("StateReady", "ready"));
			OutColour = MotionForgeStyle::Good();
			break;

		case EMotionJobStatus::Failed:
		default:
			OutText = LOCTEXT("StateFailed", "failed");
			OutColour = MotionForgeStyle::Bad();
			break;
		}
	}

	/** Metres the root travels from first frame to last. */
	float Travel(UAnimSequence* Clip)
	{
		if (!Clip)
		{
			return 0.f;
		}

		FTransform Start, End;
		Clip->GetBoneTransform(Start, FSkeletonPoseBoneIndex(0), FAnimExtractContext(0.0), false);
		Clip->GetBoneTransform(End, FSkeletonPoseBoneIndex(0), FAnimExtractContext(static_cast<double>(Clip->GetPlayLength())), false);

		const FVector Delta = End.GetLocation() - Start.GetLocation();
		return FVector(Delta.X, Delta.Y, 0.f).Size() / 100.f;
	}
}

// -------------------------------------------------------------------------------------------------
// A row
// -------------------------------------------------------------------------------------------------

class SMotionTakeTableRow : public SMultiColumnTableRow<TSharedPtr<FMotionTakeRow>>
{
public:

	SLATE_BEGIN_ARGS(SMotionTakeTableRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, const TSharedRef<STableViewBase>& Owner, TSharedPtr<FMotionTakeRow> InRow)
	{
		Row = InRow;
		SMultiColumnTableRow<TSharedPtr<FMotionTakeRow>>::Construct(
			FSuperRowType::FArguments().Padding(FMargin(0.f, 3.f)), Owner);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
	{
		using namespace MotionTakesPrivate;

		const FMotionCandidate& Take = Row->Take;
		TSharedPtr<FMotionTakeRow> Captured = Row;

		auto Cell = [](const FText& Text, const FLinearColor* Colour = nullptr, const FText& Tip = FText::GetEmpty())
		{
			return SNew(SBox).Padding(FMargin(6.f, 0.f)).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Text)
					.ToolTipText(Tip)
					.ColorAndOpacity(Colour ? FSlateColor(*Colour) : FSlateColor::UseForeground())
				];
		};

		if (Column == ColTake)
		{
			const FLinearColor Quiet = MotionForgeStyle::Quiet();
			return SNew(SBox).Padding(FMargin(6.f, 0.f)).VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock)
						.Font(Row->bInGame ? FAppStyle::GetFontStyle("BoldFont") : FAppStyle::GetFontStyle("NormalFont"))
						.Text(FText::FromString(Take.GetLabel()))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Visibility(Row->bStale ? EVisibility::Visible : EVisibility::Collapsed)
						.Text(LOCTEXT("Stale", "older recipe"))
						.ToolTipText(LOCTEXT("StaleTip", "Made from a prompt, character, model or settings the definition no longer asks for."))
						.ColorAndOpacity(FSlateColor(Quiet))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f, 0.f, 0.f)
					[
						SNew(STextBlock)
						.Visibility(Take.bHidden ? EVisibility::Visible : EVisibility::Collapsed)
						.Text(LOCTEXT("HiddenTag", "hidden"))
						.ColorAndOpacity(FSlateColor(Quiet))
					]
				];
		}

		if (Column == ColWhen)
		{
			const bool bToday = Take.GeneratedAt.GetDate() == FDateTime::Now().GetDate();
			return Cell(Take.GeneratedAt == FDateTime()
				? FText::FromString(TEXT("-"))
				: FText::FromString(Take.GeneratedAt.ToString(bToday ? TEXT("%H:%M") : TEXT("%Y-%m-%d %H:%M"))));
		}

		if (Column == ColMadeBy)
		{
			return Cell(FText::FromString(MadeBy(Take)), nullptr,
				Take.ModelId.IsEmpty() ? FText::GetEmpty() : FText::FromString(Take.ModelId));
		}

		if (Column == ColSeed)
		{
			return Cell(Take.Seed >= 0 ? FText::AsNumber(Take.Seed, &FNumberFormattingOptions::DefaultNoGrouping()) : FText::FromString(TEXT("-")),
				nullptr, Take.Seed >= 0
					? LOCTEXT("SeedTip", "The seed that was sent. With the same prompt, model and settings it makes this take again.")
					: LOCTEXT("NoSeedTip", "No seed. This provider cannot make this take again, which is why takes are only ever hidden."));
		}

		if (Column == ColLength)
		{
			return Cell(Take.LengthSeconds > 0.f ? FText::FromString(MotionForgeStyle::Seconds(Take.LengthSeconds)) : FText::FromString(TEXT("-")));
		}

		if (Column == ColCost)
		{
			FText Text = FText::FromString(TEXT("-"));
			if (Take.EstimatedCost > 0.f)
			{
				Text = FText::FromString(TEXT("~") + MotionForgeStyle::Money(Take.EstimatedCost, Take.Currency));
			}
			else if (!Take.ProviderId.IsNone())
			{
				Text = LOCTEXT("CostFree", "free");
			}
			return Cell(Text, nullptr, LOCTEXT("CostTip", "What it was estimated to cost when submitted. An estimate, not an invoice."));
		}

		if (Column == ColState)
		{
			return SNew(SBox).Padding(FMargin(6.f, 0.f)).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([Captured]()
					{
						FText Text; FLinearColor Colour;
						MotionTakesPrivate::DescribeState(*Captured, Text, Colour);
						return Text;
					})
					.ColorAndOpacity_Lambda([Captured]()
					{
						FText Text; FLinearColor Colour;
						MotionTakesPrivate::DescribeState(*Captured, Text, Colour);
						return FSlateColor(Colour);
					})
					.ToolTipText(FText::FromString(Take.Error))
				];
		}

		return SNullWidget::NullWidget;
	}

private:

	TSharedPtr<FMotionTakeRow> Row;
};

// -------------------------------------------------------------------------------------------------
// The panel
// -------------------------------------------------------------------------------------------------

void SMotionTakesPanel::Construct(const FArguments& InArgs)
{
	using namespace MotionTakesPrivate;

	Definition = InArgs._Definition;

	TSharedRef<SHeaderRow> Header = SNew(SHeaderRow)
		+ SHeaderRow::Column(ColTake).DefaultLabel(LOCTEXT("ColTake", "Take")).FillWidth(1.4f)
		+ SHeaderRow::Column(ColWhen).DefaultLabel(LOCTEXT("ColWhen", "When")).FillWidth(0.9f)
		+ SHeaderRow::Column(ColMadeBy).DefaultLabel(LOCTEXT("ColMadeBy", "Made by")).FillWidth(1.2f)
		+ SHeaderRow::Column(ColSeed).DefaultLabel(LOCTEXT("ColSeed", "Seed")).FillWidth(0.8f)
		+ SHeaderRow::Column(ColLength).DefaultLabel(LOCTEXT("ColLength", "Length")).FillWidth(0.6f)
		+ SHeaderRow::Column(ColCost).DefaultLabel(LOCTEXT("ColCost", "Cost")).FillWidth(0.6f)
		+ SHeaderRow::Column(ColState).DefaultLabel(LOCTEXT("ColState", "State")).FillWidth(1.0f);

	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(Orient_Vertical)

		+ SSplitter::Slot().Value(0.6f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(4.f)
			[
				SAssignNew(Stage, SMotionStage)
				.Message(this, &SMotionTakesPanel::StageMessage)
				.Stats(this, &SMotionTakesPanel::StageStats)
			]
		]

		+ SSplitter::Slot().Value(0.4f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 6.f)
			[
				SAssignNew(StatusBox, SBox)
			]

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(2.f)
				[
					SAssignNew(List, SListView<TSharedPtr<FMotionTakeRow>>)
					.ListItemsSource(&Rows)
					.SelectionMode(ESelectionMode::Single)
					.HeaderRow(Header)
					.OnGenerateRow(this, &SMotionTakesPanel::MakeRow)
					.OnSelectionChanged(this, &SMotionTakesPanel::OnSelectionChanged)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
			[
				BuildActions()
			]
		]
	];

	SAssignNew(RecordBox, SBox);

	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SMotionTakesPanel::Poll));
	Refresh();
}

SMotionTakesPanel::~SMotionTakesPanel()
{
}

TSharedRef<ITableRow> SMotionTakesPanel::MakeRow(TSharedPtr<FMotionTakeRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SMotionTakeTableRow, Owner, Row);
}

uint32 SMotionTakesPanel::Fingerprint() const
{
	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return 0;
	}

	uint32 Hash = HashCombine(GetTypeHash(static_cast<uint8>(Def->Status)), GetTypeHash(Def->SelectedMotionId));
	Hash = HashCombine(Hash, GetTypeHash(Def->ImportedMotionId));
	Hash = HashCombine(Hash, GetTypeHash(Def->ImportedSequence.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Def->LastError));
	Hash = HashCombine(Hash, GetTypeHash(bShowHidden));

	for (const FMotionCandidate& Take : Def->Candidates)
	{
		Hash = HashCombine(Hash, GetTypeHash(Take.MotionId));
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Take.Status)));
		Hash = HashCombine(Hash, GetTypeHash(Take.bDownloaded));
		Hash = HashCombine(Hash, GetTypeHash(Take.bHidden));
		Hash = HashCombine(Hash, GetTypeHash(Take.JobId));
	}
	return Hash;
}

EActiveTimerReturnType SMotionTakesPanel::Poll(double, float)
{
	if (Fingerprint() != LastFingerprint)
	{
		Refresh();
	}
	return EActiveTimerReturnType::Continue;
}

void SMotionTakesPanel::Refresh()
{
	LastFingerprint = Fingerprint();

	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Def || !Forge)
	{
		return;
	}

	// Which takes are in the game and which are stale, from the same resolver the rest of the window uses.
	const TArray<FMotionDefinitionStatus> Status = Forge->GetStatus({ Def->GetPathName() });
	TMap<FString, const FMotionTakeInfo*> Infos;
	if (Status.Num() == 1)
	{
		for (const FMotionTakeInfo& Info : Status[0].Takes)
		{
			if (!Info.MotionId.IsEmpty())
			{
				Infos.Add(Info.MotionId, &Info);
			}
		}
	}

	Rows.Reset();
	HiddenCount = 0;

	// Newest first: the take just generated is the one being looked for.
	for (int32 Index = Def->Candidates.Num() - 1; Index >= 0; --Index)
	{
		const FMotionCandidate& Take = Def->Candidates[Index];
		if (Take.bHidden)
		{
			++HiddenCount;
			if (!bShowHidden)
			{
				continue;
			}
		}

		TSharedPtr<FMotionTakeRow> Row = MakeShared<FMotionTakeRow>();
		Row->Key = MotionTakesPrivate::KeyOf(Take);
		Row->Take = Take;
		Row->File = UMotionForgeSubsystem::FindTakeFile(Take);

		if (const FMotionTakeInfo* const* Info = Infos.Find(Take.MotionId))
		{
			Row->bInGame = (*Info)->bInGame;
			Row->bStale = (*Info)->bStale;
		}
		Rows.Add(Row);
	}

	if (List.IsValid())
	{
		List->RequestListRefresh();

		// Selection by take, not by row, so a refresh never drops what somebody was looking at.
		for (const TSharedPtr<FMotionTakeRow>& Row : Rows)
		{
			if (Row->Key == SelectedKey)
			{
				List->SetSelection(Row, ESelectInfo::Direct);
				break;
			}
		}
	}

	if (StatusBox.IsValid())
	{
		StatusBox->SetContent(BuildStatusLine());
	}

	// The first time: open on the clip in the game, or on the newest finished take.
	if (!bOpenedOnce)
	{
		bOpenedOnce = true;

		if (!Def->ImportedSequence.IsNull() && Def->ImportedSequence.LoadSynchronous())
		{
			ShowImportedClip();
		}
		else
		{
			for (const TSharedPtr<FMotionTakeRow>& Row : Rows)
			{
				if (Row->Take.IsUsable())
				{
					List->SetSelection(Row, ESelectInfo::OnMouseClick);
					break;
				}
			}
		}
	}

	RebuildRecord();
}

TSharedRef<SWidget> SMotionTakesPanel::BuildStatusLine()
{
	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return SNullWidget::NullWidget;
	}

	FText Text;
	FLinearColor Colour = MotionForgeStyle::Quiet();

	const bool bHadClip = !Def->ImportedSequence.IsNull();
	const bool bClipMissing = (Def->Status == EMotionDefStatus::Ready && !bHadClip)
		|| (bHadClip && !Def->ImportedSequence.ToSoftObjectPath().TryLoad());

	int32 Usable = 0;
	for (const FMotionCandidate& Take : Def->Candidates)
	{
		Usable += Take.IsUsable() && !Take.bHidden ? 1 : 0;
	}

	switch (Def->Status)
	{
	case EMotionDefStatus::Draft:
		Text = LOCTEXT("StatusDraft", "Nothing generated yet. Write the prompt and press Generate.");
		break;
	case EMotionDefStatus::Generating:
		Text = LOCTEXT("StatusGenerating", "Generating. Takes appear here as they finish.");
		Colour = MotionForgeStyle::Warn();
		break;
	case EMotionDefStatus::AwaitingReview:
	{
		// New takes do not take the old clip out of the game; say which one is still playing there.
		const FMotionCandidate* InGame = Def->ImportedMotionId.IsEmpty() ? nullptr : Def->FindCandidate(Def->ImportedMotionId);
		Text = (InGame && !bClipMissing)
			? FText::Format(LOCTEXT("StatusReviewKeptFmt", "{0} {0}|plural(one=take,other=takes) to choose from. The game plays {1}, from {2}, until you choose and import another."),
				Usable, FText::FromString(Def->ImportedSequence.ToSoftObjectPath().GetAssetName()), FText::FromString(InGame->GetLabel()))
			: FText::Format(LOCTEXT("StatusReviewFmt", "{0} {0}|plural(one=take,other=takes) to choose from. Watch them, then Choose and import the one to keep."), Usable);
		Colour = MotionForgeStyle::Info();
		break;
	}
	case EMotionDefStatus::Downloading:
		Text = LOCTEXT("StatusDownloading", "Fetching the chosen take.");
		Colour = MotionForgeStyle::Warn();
		break;
	case EMotionDefStatus::Processing:
		Text = LOCTEXT("StatusProcessing", "Importing the chosen take.");
		Colour = MotionForgeStyle::Warn();
		break;
	case EMotionDefStatus::Ready:
		// Checked, not believed: Ready records what happened, and the clip can be deleted since.
		if (bClipMissing)
		{
			Text = LOCTEXT("StatusClipGone", "The imported clip was deleted. Choose and import a take to make it again - it costs nothing on a take already fetched.");
			Colour = MotionForgeStyle::Bad();
		}
		else
		{
			const FMotionCandidate* Chosen = Def->ImportedMotionId.IsEmpty() ? nullptr : Def->FindCandidate(Def->ImportedMotionId);
			Text = FText::Format(LOCTEXT("StatusReadyFmt", "In the game: {0}, from {1}."),
				FText::FromString(Def->ImportedSequence.ToSoftObjectPath().GetAssetName()),
				FText::FromString(Chosen ? Chosen->GetLabel() : FString(TEXT("the chosen take"))));
			Colour = MotionForgeStyle::Good();
		}
		break;
	case EMotionDefStatus::Failed:
		Text = FText::FromString(Def->LastError.IsEmpty() ? FString(TEXT("The last attempt failed.")) : Def->LastError);
		Colour = MotionForgeStyle::Bad();
		break;
	}

	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 8.f, 0.f)
		[
			MotionForgeStyle::Dot(Colour)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Text).AutoWrapText(true)
		];

	if (HiddenCount > 0)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return bShowHidden ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowHidden = State == ECheckBoxState::Checked; Refresh(); })
			[
				SNew(STextBlock).Text(FText::Format(LOCTEXT("ShowHiddenFmt", "Show hidden ({0})"), HiddenCount))
			]
		];
	}

	return Line;
}

TSharedRef<SWidget> SMotionTakesPanel::BuildActions()
{
	auto Selected = [this]() { return GetSelected(); };

	auto Button = [](const FText& Label, const FText& Tip, TFunction<bool()> Enabled, FOnClicked OnClicked)
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText(Tip)
			.IsEnabled_Lambda([Enabled]() { return Enabled(); })
			.OnClicked(OnClicked);
	};

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			Button(LOCTEXT("ShowAsB", "Show as B"),
				LOCTEXT("ShowAsBTip", "Put this take beside the one on stage, on the same clock, to compare them at the same moment. Press again with nothing selected to clear B."),
				[Selected]() { TSharedPtr<FMotionTakeRow> Row = Selected(); return !Row.IsValid() || Row->Take.IsUsable(); },
				FOnClicked::CreateSP(this, &SMotionTakesPanel::OnShowAsB))
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SButton)
			.Text_Lambda([Selected]()
			{
				TSharedPtr<FMotionTakeRow> Row = Selected();
				FString Name = Row.IsValid() ? MotionTakesPrivate::ProviderName(Row->Take.ProviderId) : FString();
				return Name.IsEmpty() ? LOCTEXT("Watch", "Watch online") : FText::Format(LOCTEXT("WatchFmt", "Watch on {0}"), FText::FromString(Name));
			})
			.ToolTipText(LOCTEXT("WatchTip", "Open this take in the provider's own viewer. Free."))
			.Visibility_Lambda([Selected]()
			{
				TSharedPtr<FMotionTakeRow> Row = Selected();
				return Row.IsValid() && !Row->Take.ViewerUrl.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnClicked(this, &SMotionTakesPanel::OnWatch)
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			Button(LOCTEXT("ShowFiles", "Show file"),
				LOCTEXT("ShowFilesTip", "Open the folder holding this take's fetched file."),
				[Selected]() { TSharedPtr<FMotionTakeRow> Row = Selected(); return Row.IsValid() && !Row->File.IsEmpty(); },
				FOnClicked::CreateSP(this, &SMotionTakesPanel::OnShowFiles))
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SButton)
			.Text_Lambda([Selected]()
			{
				TSharedPtr<FMotionTakeRow> Row = Selected();
				return Row.IsValid() && Row->Take.bHidden ? LOCTEXT("Unhide", "Unhide") : LOCTEXT("Hide", "Hide");
			})
			.ToolTipText(LOCTEXT("HideTip", "Take it out of the list. Nothing is deleted - some takes cannot be made again - and Show hidden brings it back."))
			.IsEnabled_Lambda([Selected]() { TSharedPtr<FMotionTakeRow> Row = Selected(); return Row.IsValid() && !Row->Take.MotionId.IsEmpty(); })
			.OnClicked(this, &SMotionTakesPanel::OnHide)
		]

		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SSpacer)
		]

		// The one verb that changes the project, on the right, where it cannot be hit on the way to Hide.
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.Text(LOCTEXT("ChooseImport", "Choose and import"))
			.ToolTipText(LOCTEXT("ChooseImportTip",
				"Make this take the definition's animation: fetch it if it is not on disk, and import it onto the "
				"character. Asks first when something already uses the animation."))
			.IsEnabled_Lambda([this, Selected]()
			{
				const UMotionDef* Def = Definition.Get();
				TSharedPtr<FMotionTakeRow> Row = Selected();
				return Def && !Def->IsBusy() && Row.IsValid() && Row->Take.IsUsable();
			})
			.OnClicked(this, &SMotionTakesPanel::OnChooseAndImport)
		];
}

TSharedPtr<FMotionTakeRow> SMotionTakesPanel::GetSelected() const
{
	if (!List.IsValid())
	{
		return nullptr;
	}
	TArray<TSharedPtr<FMotionTakeRow>> Selected = List->GetSelectedItems();
	return Selected.Num() > 0 ? Selected[0] : nullptr;
}

const FMotionCandidate* SMotionTakesPanel::FindLiveTake(const FString& Key) const
{
	const UMotionDef* Def = Definition.Get();
	if (!Def)
	{
		return nullptr;
	}
	return Def->Candidates.FindByPredicate([&Key](const FMotionCandidate& Take) { return MotionTakesPrivate::KeyOf(Take) == Key; });
}

void SMotionTakesPanel::OnSelectionChanged(TSharedPtr<FMotionTakeRow> Row, ESelectInfo::Type Info)
{
	if (!Row.IsValid())
	{
		return;
	}

	const bool bMoved = Row->Key != SelectedKey;
	SelectedKey = Row->Key;
	RebuildRecord();

	// A refresh restoring the same selection does not reload the stage.
	if (bMoved || Info != ESelectInfo::Direct)
	{
		if (Row->Take.IsUsable())
		{
			ShowTake(0, Row->Take.MotionId);
		}
	}
}

// -------------------------------------------------------------------------------------------------
// The stage
// -------------------------------------------------------------------------------------------------

USkeletalMesh* SMotionTakesPanel::MeshFor(UAnimSequence* Clip) const
{
	if (!Clip || !Clip->GetSkeleton())
	{
		return nullptr;
	}

	const USkeleton* Skeleton = Clip->GetSkeleton();

	if (const UMotionDef* Def = Definition.Get())
	{
		if (const UMotionCharacter* Character = Def->Character.LoadSynchronous())
		{
			USkeletalMesh* Preview = Character->PreviewMesh.LoadSynchronous();
			if (Preview && Preview->GetSkeleton() && Preview->GetSkeleton()->IsCompatibleForEditor(Skeleton))
			{
				return Preview;
			}

			USkeletalMesh* ProviderMesh = Character->ProviderMesh.LoadSynchronous();
			if (ProviderMesh && ProviderMesh->GetSkeleton() && ProviderMesh->GetSkeleton()->IsCompatibleForEditor(Skeleton))
			{
				return ProviderMesh;
			}
		}
	}

	return const_cast<USkeleton*>(Skeleton)->GetPreviewMesh(/*bFindIfNotSet=*/true);
}

void SMotionTakesPanel::PlaceOnStage(int32 Slot, UAnimSequence* Clip, const FString& Label)
{
	if (!Stage.IsValid())
	{
		return;
	}

	USkeletalMesh* Mesh = MeshFor(Clip);
	if (!Mesh)
	{
		Slots[Slot].Problem = TEXT("There is no mesh on this clip's skeleton to show it on. Set a Preview Mesh on the Motion Character.");
		return;
	}

	Stage->GetViewport()->SetSlot(Slot, Mesh, Clip);
	Slots[Slot].bFilled = true;
	Slots[Slot].Label = Label;
	Slots[Slot].PlayingLabel = Label;
	Stage->GetViewport()->Play();
}

void SMotionTakesPanel::ShowImportedClip()
{
	const UMotionDef* Def = Definition.Get();
	UAnimSequence* Clip = Def ? Def->ImportedSequence.LoadSynchronous() : nullptr;
	if (!Clip)
	{
		return;
	}

	Slots[0] = FSlot();
	Slots[0].MotionId = Def->ImportedMotionId;
	PlaceOnStage(0, Clip, FString::Printf(TEXT("%s (in the game)"), *Clip->GetName()));
}

void SMotionTakesPanel::ShowTake(int32 Slot, const FString& MotionId)
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	if (!Def || !Forge || MotionId.IsEmpty())
	{
		return;
	}

	const FMotionCandidate* Take = Def->FindCandidate(MotionId);
	if (!Take)
	{
		return;
	}

	const FString Label = Take->GetLabel();

	// The take in the game is its own imported clip: show that rather than build a second copy.
	if (Def->ImportedMotionId == MotionId && !Def->ImportedSequence.IsNull())
	{
		if (UAnimSequence* Imported = Def->ImportedSequence.LoadSynchronous())
		{
			Slots[Slot] = FSlot();
			Slots[Slot].MotionId = MotionId;
			PlaceOnStage(Slot, Imported, Label + TEXT(" (in the game)"));
			return;
		}
	}

	// The clip already on the slot keeps playing, under its own name, until this one arrives.
	const FString Playing = Slots[Slot].PlayingLabel;
	const bool bWasFilled = Slots[Slot].bFilled;
	Slots[Slot] = FSlot();
	Slots[Slot].MotionId = MotionId;
	Slots[Slot].Label = Label;
	Slots[Slot].PlayingLabel = Playing;
	Slots[Slot].bFilled = bWasFilled;

	FString WhyNot;
	if (!Forge->CanPreviewTake(Def->GetPathName(), MotionId, WhyNot))
	{
		Slots[Slot].Problem = FString::Printf(TEXT("%s: %s"), *Label, *WhyNot);
		return;
	}

	Slots[Slot].bLoading = true;

	TWeakPtr<SMotionTakesPanel> WeakThis = SharedThis(this);
	Forge->PreviewTake(Def->GetPathName(), MotionId,
		[WeakThis, Slot, MotionId, Label](UAnimSequence* Clip, const FString& Error)
		{
			TSharedPtr<SMotionTakesPanel> Self = WeakThis.Pin();
			if (!Self.IsValid() || Self->Slots[Slot].MotionId != MotionId)
			{
				return;   // something else was put on this slot meanwhile
			}

			Self->Slots[Slot].bLoading = false;

			if (!Clip)
			{
				Self->Slots[Slot].Problem = FString::Printf(TEXT("%s could not be shown: %s"), *Label, *Error);
				return;
			}

			Self->PlaceOnStage(Slot, Clip, Label);
		});
}

void SMotionTakesPanel::ClearSlot(int32 Slot)
{
	Slots[Slot] = FSlot();
	if (Stage.IsValid())
	{
		Stage->GetViewport()->SetSlot(Slot, nullptr, nullptr);
	}
}

FText SMotionTakesPanel::StageMessage() const
{
	TArray<FString> Lines;

	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		if (Slots[Slot].bLoading)
		{
			Lines.Add(FString::Printf(TEXT("Fetching %s..."), *Slots[Slot].Label));
		}
		else if (!Slots[Slot].Problem.IsEmpty())
		{
			Lines.Add(Slots[Slot].Problem);
		}
	}

	if (Lines.Num() == 0 && !Slots[0].bFilled && !Slots[1].bFilled)
	{
		const UMotionDef* Def = Definition.Get();
		return Def && Def->Candidates.Num() == 0
			? LOCTEXT("StageEmpty", "Takes play here on the character, before you choose one.\nWrite a prompt on the right and press Generate.")
			: LOCTEXT("StagePick", "Select a take to watch it on the character.");
	}

	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

FText SMotionTakesPanel::StageStats() const
{
	if (!Stage.IsValid())
	{
		return FText::GetEmpty();
	}

	TArray<FString> Parts;
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		UAnimSequence* Clip = Stage->GetViewport()->GetClip(Slot);
		if (!Clip)
		{
			continue;
		}

		const float Travel = MotionTakesPrivate::Travel(Clip);
		Parts.Add(FString::Printf(TEXT("%s  %s · %d frames · %.2f s%s"),
			Slot == 0 ? TEXT("A") : TEXT("B"), *Slots[Slot].PlayingLabel,
			Clip->GetNumberOfSampledKeys(), Clip->GetPlayLength(),
			Travel > 0.05f ? *FString::Printf(TEXT(" · travels %.1f m"), Travel) : TEXT(" · on the spot")));
	}

	return FText::FromString(FString::Join(Parts, TEXT("        ")));
}

// -------------------------------------------------------------------------------------------------
// Actions
// -------------------------------------------------------------------------------------------------

FReply SMotionTakesPanel::OnShowAsB()
{
	TSharedPtr<FMotionTakeRow> Row = GetSelected();

	// With nothing new selected, the button clears B - one control for putting a comparison up and
	// taking it down, rather than a second one to find.
	if (!Row.IsValid() || Row->Take.MotionId == Slots[1].MotionId)
	{
		ClearSlot(1);
		return FReply::Handled();
	}

	ShowTake(1, Row->Take.MotionId);
	return FReply::Handled();
}

FReply SMotionTakesPanel::OnWatch()
{
	if (TSharedPtr<FMotionTakeRow> Row = GetSelected())
	{
		if (!Row->Take.ViewerUrl.IsEmpty())
		{
			FPlatformProcess::LaunchURL(*Row->Take.ViewerUrl, nullptr, nullptr);
		}
	}
	return FReply::Handled();
}

FReply SMotionTakesPanel::OnShowFiles()
{
	if (TSharedPtr<FMotionTakeRow> Row = GetSelected())
	{
		if (!Row->File.IsEmpty())
		{
			// A file path opens its folder with the file selected.
			FPlatformProcess::ExploreFolder(*Row->File);
		}
	}
	return FReply::Handled();
}

FReply SMotionTakesPanel::OnHide()
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	TSharedPtr<FMotionTakeRow> Row = GetSelected();

	if (Def && Forge && Row.IsValid() && !Row->Take.MotionId.IsEmpty())
	{
		Forge->HideTake(Def->GetPathName(), Row->Take.MotionId, !Row->Take.bHidden);
		Refresh();
	}
	return FReply::Handled();
}

FReply SMotionTakesPanel::OnChooseAndImport()
{
	UMotionDef* Def = Definition.Get();
	UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
	TSharedPtr<FMotionTakeRow> Row = GetSelected();

	if (!Def || !Forge || !Row.IsValid() || !Row->Take.IsUsable())
	{
		return FReply::Handled();
	}

	const FString Label = Row->Take.GetLabel();

	// Replacing a clip changes everything that plays it. Say what, by name, before doing it.
	if (!Def->ImportedSequence.IsNull() && Def->ImportedMotionId != Row->Take.MotionId)
	{
		const TArray<FString> Users = Forge->GetClipUsers(Def->GetPathName());
		const FString Clip = Def->ImportedSequence.ToSoftObjectPath().GetAssetName();

		const FText Question = Users.Num() > 0
			? FText::Format(LOCTEXT("ReplaceUsedFmt",
				"Replace {0} with {1}?\n\n{2} {2}|plural(one=asset uses,other=assets use) it and will play the new take:\n{3}"),
				FText::FromString(Clip), FText::FromString(Label), Users.Num(), FText::FromString(FString::Join(Users, TEXT("\n"))))
			: FText::Format(LOCTEXT("ReplaceFmt", "Replace {0} with {1}? Nothing else in the project uses it yet."),
				FText::FromString(Clip), FText::FromString(Label));

		if (FMessageDialog::Open(EAppMsgType::OkCancel, Question, LOCTEXT("ReplaceTitle", "Replace the animation")) != EAppReturnType::Ok)
		{
			return FReply::Handled();
		}
	}

	// On a plan that bills downloads, fetching a take not yet on disk spends quota. Priced, and asked.
	if (Row->File.IsEmpty())
	{
		// Priced by the provider that made the take, which is the one that will be asked for it.
		TSharedPtr<IMotionProvider> Maker = Forge->FindProvider(Row->Take.ProviderId.IsNone() ? Def->ProviderId : Row->Take.ProviderId);
		const FMotionBilling Billing = Maker.IsValid() ? Maker->GetBilling() : FMotionBilling();

		if (Billing.Unit == EMotionBillingUnit::PerDownloadedSecond)
		{
			const float Seconds = Row->Take.LengthSeconds > 0.f ? FMath::CeilToFloat(Row->Take.LengthSeconds) : Def->Length;
			const FString Money = Billing.Rate > 0.f
				? FString::Printf(TEXT(", about %s"), *MotionForgeStyle::Money(Seconds * Billing.Rate, Billing.Currency))
				: FString();

			const FText Question = FText::Format(LOCTEXT("DownloadCostFmt",
				"Fetch {0}?\n\nIt uses {1} of your {2} download quota{3}. Watching it on {2}'s site first is free."),
				FText::FromString(Label), FText::FromString(MotionForgeStyle::Seconds(Seconds)),
				FText::FromString(Maker->GetDisplayName()), FText::FromString(Money));

			if (FMessageDialog::Open(EAppMsgType::OkCancel, Question, LOCTEXT("DownloadTitle", "Fetching uses quota")) != EAppReturnType::Ok)
			{
				return FReply::Handled();
			}
		}
	}

	Forge->ChooseAndImport(Def->GetPathName(), Row->Take.MotionId);
	Refresh();
	return FReply::Handled();
}

// -------------------------------------------------------------------------------------------------
// The record
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionTakesPanel::GetRecordWidget()
{
	if (!RecordBox.IsValid())
	{
		SAssignNew(RecordBox, SBox);
	}
	RebuildRecord();

	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(12.f)
		[
			RecordBox.ToSharedRef()
		];
}

void SMotionTakesPanel::RebuildRecord()
{
	if (!RecordBox.IsValid())
	{
		return;
	}

	const FMotionCandidate* Take = FindLiveTake(SelectedKey);
	if (!Take)
	{
		RecordBox->SetContent(
			SNew(STextBlock)
			.Text(LOCTEXT("RecordNone", "Select a take to see everything that made it."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground()));
		return;
	}

	using namespace MotionForgeStyle;

	TSharedRef<SVerticalBox> Facts = SNew(SVerticalBox);
	auto Add = [&Facts](const FText& Label, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Facts->AddSlot().AutoHeight().Padding(0.f, 2.f)[ Fact(Label, FText::FromString(Value)) ];
		}
	};

	FText StateText; FLinearColor StateColour;
	FMotionTakeRow Probe; Probe.Take = *Take;
	for (const TSharedPtr<FMotionTakeRow>& Row : Rows)
	{
		if (Row->Key == SelectedKey) { Probe = *Row; }
	}
	MotionTakesPrivate::DescribeState(Probe, StateText, StateColour);

	Add(LOCTEXT("RecState", "State"), StateText.ToString());
	Add(LOCTEXT("RecMadeBy", "Made by"), Take->ProviderId.IsNone() ? FString() : FString::Printf(TEXT("%s, model %s"),
		*MotionTakesPrivate::ProviderName(Take->ProviderId), *Take->ModelId));
	Add(LOCTEXT("RecCharacter", "Character"), Take->Character.GetAssetName());
	Add(LOCTEXT("RecSeed", "Seed"), Take->Seed >= 0 ? FString::FromInt(Take->Seed) : FString(TEXT("none - this take cannot be made again")));
	Add(LOCTEXT("RecLength", "Length asked"), Take->LengthSeconds > 0.f ? Seconds(Take->LengthSeconds) : FString());
	Add(LOCTEXT("RecCost", "Estimated cost"), Take->EstimatedCost > 0.f ? Money(Take->EstimatedCost, Take->Currency) : (Take->ProviderId.IsNone() ? FString() : FString(TEXT("free"))));
	Add(LOCTEXT("RecWhen", "Generated"), Take->GeneratedAt == FDateTime() ? FString() : Take->GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	Add(LOCTEXT("RecPrompt", "Prompt sent"), Take->PromptSent);
	Add(LOCTEXT("RecSettings", "Settings sent"), Take->SettingsSent);
	Add(LOCTEXT("RecRecipe", "Recipe"), Probe.bStale ? FString(TEXT("older: the definition has changed since this take was made")) : (Take->RecipeHash.IsEmpty() ? FString() : FString(TEXT("matches the definition now"))));
	Add(LOCTEXT("RecMotionId", "Provider's id"), Take->MotionId);
	Add(LOCTEXT("RecJobId", "Job"), Take->JobId);
	Add(LOCTEXT("RecFile", "File on disk"), Probe.File);
	Add(LOCTEXT("RecError", "Error"), Take->Error);

	if (Take->ProviderId.IsNone())
	{
		Facts->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			Note(LOCTEXT("RecOlder", "Made before takes recorded their provider, settings and seed, so those are not known."), Quiet())
		];
	}

	RecordBox->SetContent(Card(FText::FromString(Take->GetLabel()), Facts,
		LOCTEXT("RecordSub", "Everything that made this take, as it was sent.")));
}

#undef LOCTEXT_NAMESPACE
