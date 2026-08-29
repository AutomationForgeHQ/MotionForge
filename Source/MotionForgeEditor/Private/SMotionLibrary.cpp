// Copyright Blackcode SA. All rights reserved.

#include "SMotionLibrary.h"

#include "MotionDef.h"
#include "MotionForgeFactories.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

/**
 * Named rather than anonymous, deliberately.
 *
 * Unity builds fold neighbouring translation units together, and this file sits beside two others
 * with their own GoodColour and their own Dot. Anonymous namespaces do not save you from that -
 * they collide with a redefinition error that names a file you did not touch.
 */
namespace MotionLibraryUI
{
	const FLinearColor GoodColour(0.30f, 0.78f, 0.45f);
	const FLinearColor WarnColour(0.95f, 0.65f, 0.20f);
	const FLinearColor BadColour(0.90f, 0.36f, 0.36f);
	const FLinearColor QuietColour(0.55f, 0.55f, 0.58f);

	const FName ColumnName("Name");
	const FName ColumnStatus("Status");
	const FName ColumnProvider("Provider");
	const FName ColumnTakes("Takes");
	const FName ColumnAnimation("Animation");
	const FName ColumnPrompt("Prompt");

	/** The shared vocabulary from PANEL_RULES.md, copied rather than shared - see SMotionDefTakes. */
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

	/** One word per state, in the same vocabulary the definition window uses. */
	void DescribeStatus(EMotionDefStatus Status, FText& OutText, FLinearColor& OutColour)
	{
		switch (Status)
		{
		case EMotionDefStatus::Draft:
			OutText = LOCTEXT("LibDraft", "Draft");
			OutColour = QuietColour;
			break;

		case EMotionDefStatus::Generating:
			OutText = LOCTEXT("LibGenerating", "Generating");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::AwaitingReview:
			OutText = LOCTEXT("LibReview", "Choose a take");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::Downloading:
			OutText = LOCTEXT("LibDownloading", "Downloading");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::Processing:
			OutText = LOCTEXT("LibProcessing", "Importing");
			OutColour = WarnColour;
			break;

		case EMotionDefStatus::Ready:
			OutText = LOCTEXT("LibReady", "Ready");
			OutColour = GoodColour;
			break;

		case EMotionDefStatus::Failed:
			OutText = LOCTEXT("LibFailed", "Failed");
			OutColour = BadColour;
			break;

		default:
			OutText = FText::GetEmpty();
			OutColour = QuietColour;
			break;
		}
	}

	bool MatchesFilter(EMotionLibraryFilter Filter, EMotionDefStatus Status)
	{
		switch (Filter)
		{
		case EMotionLibraryFilter::All:    return true;
		case EMotionLibraryFilter::Draft:  return Status == EMotionDefStatus::Draft;
		case EMotionLibraryFilter::Review: return Status == EMotionDefStatus::AwaitingReview;
		case EMotionLibraryFilter::Ready:  return Status == EMotionDefStatus::Ready;
		case EMotionLibraryFilter::Failed: return Status == EMotionDefStatus::Failed;

		// Everything the pipeline is in the middle of. Three states that mean "wait", and a person
		// filtering for them wants all three.
		case EMotionLibraryFilter::Working:
			return Status == EMotionDefStatus::Generating
				|| Status == EMotionDefStatus::Downloading
				|| Status == EMotionDefStatus::Processing;

		default: return true;
		}
	}

	int32 CountMatching(
		const TArray<TSharedPtr<FMotionLibraryEntry>>& Entries, EMotionLibraryFilter Filter)
	{
		int32 Count = 0;
		for (const TSharedPtr<FMotionLibraryEntry>& Entry : Entries)
		{
			if (Entry.IsValid() && MatchesFilter(Filter, Entry->Status.Status))
			{
				++Count;
			}
		}
		return Count;
	}

	/** A prompt on one line, so a row stays a row. The full text is on the tooltip. */
	FString FirstLine(const FString& Text, int32 MaxLength = 140)
	{
		FString Line = Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
		Line.TrimStartAndEndInline();

		if (Line.Len() > MaxLength)
		{
			Line = Line.Left(MaxLength - 1) + TEXT("…");
		}
		return Line;
	}

	// ---------------------------------------------------------------------------------------------

	/** One definition across six columns. */
	class SMotionLibraryRow : public SMultiColumnTableRow<TSharedPtr<FMotionLibraryEntry>>
	{
	public:

		SLATE_BEGIN_ARGS(SMotionLibraryRow) {}
			SLATE_ARGUMENT(TSharedPtr<FMotionLibraryEntry>, Entry)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Entry = InArgs._Entry;
			SMultiColumnTableRow::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			if (!Entry.IsValid())
			{
				return SNullWidget::NullWidget;
			}

			const FMotionDefinitionStatus& Status = Entry->Status;

			if (Column == ColumnName)
			{
				return Pad(SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					.Text(FText::FromString(Status.Name))
					.ToolTipText(FText::FromString(Status.AssetPath)));
			}

			if (Column == ColumnStatus)
			{
				FText Text;
				FLinearColor Colour;
				DescribeStatus(Status.Status, Text, Colour);

				return Pad(SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
					[
						Dot(Colour)
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Text)
						.ColorAndOpacity(FSlateColor(Colour))
						.ToolTipText(Status.LastError.IsEmpty()
							? Text
							: FText::FromString(Status.LastError))
					]);
			}

			if (Column == ColumnProvider)
			{
				// Chosen and inherited are different facts. A library that draws them alike is how
				// changing the project default quietly moves every inherited definition with it.
				return Pad(SNew(STextBlock)
					.Text(FText::FromString(Entry->ProviderName))
					.ColorAndOpacity(FSlateColor(Status.bProviderInherited ? QuietColour : FLinearColor::White))
					.ToolTipText(Status.bProviderInherited
						? LOCTEXT("ProviderInheritedTip",
							"Inherited from the project default. Changing that setting moves this "
							"definition to whatever it becomes.")
						: LOCTEXT("ProviderChosenTip", "Chosen on this definition.")));
			}

			if (Column == ColumnTakes)
			{
				const int32 Total = Status.Takes.Num();

				FText Text = Total == 0
					? LOCTEXT("NoTakesDash", "—")
					: FText::AsNumber(Total);

				if (Total > 0 && !Status.SelectedMotionId.IsEmpty())
				{
					Text = FText::Format(LOCTEXT("TakesChosenFmt", "{0} · chosen"), Total);
				}

				return Pad(SNew(STextBlock)
					.Text(Text)
					.ColorAndOpacity(FSlateColor(Total == 0 ? QuietColour : FLinearColor::White))
					.ToolTipText(FText::Format(
						LOCTEXT("TakesTipFmt", "{0} generated, {1} finished."),
						Total, Entry->UsableTakes)));
			}

			if (Column == ColumnAnimation)
			{
				if (Status.bImportedSequenceMissing)
				{
					// The status says Ready and there is nothing to show for it. Nothing else catches
					// this: the status records what the pipeline did, truthfully, and stays that way
					// after somebody deletes the clip.
					return Pad(SNew(STextBlock)
						.Text(LOCTEXT("AnimationMissing", "missing"))
						.ColorAndOpacity(FSlateColor(BadColour))
						.ToolTipText(FText::Format(
							LOCTEXT("AnimationMissingTipFmt",
								"This definition points at '{0}', which is not in the project any "
								"more. Importing the chosen take again will replace it."),
							FText::FromString(Status.ImportedSequencePath))));
				}

				return Pad(SNew(STextBlock)
					.Text(Entry->AnimationName.IsEmpty()
						? LOCTEXT("NoAnimationDash", "—")
						: FText::FromString(Entry->AnimationName))
					.ColorAndOpacity(FSlateColor(Entry->AnimationName.IsEmpty() ? QuietColour : GoodColour))
					.ToolTipText(Entry->AnimationName.IsEmpty()
						? LOCTEXT("NoAnimationTip", "Nothing imported yet.")
						: FText::FromString(Status.ImportedSequencePath)));
			}

			if (Column == ColumnPrompt)
			{
				return Pad(SNew(STextBlock)
					.Text(FText::FromString(FirstLine(Status.Prompt)))
					.ColorAndOpacity(FSlateColor(QuietColour))
					.ToolTipText(Status.Prompt.IsEmpty()
						? LOCTEXT("NoPromptTip", "No prompt. This definition cannot generate yet.")
						: FText::FromString(Status.Prompt)));
			}

			return SNullWidget::NullWidget;
		}

	private:

		static TSharedRef<SWidget> Pad(TSharedRef<SWidget> Content)
		{
			return SNew(SBox)
				.Padding(FMargin(8.f, 4.f))
				.VAlign(VAlign_Center)
				[
					Content
				];
		}

		TSharedPtr<FMotionLibraryEntry> Entry;
	};
}

// Qualified through a short alias rather than pulled in with a using-directive: a file-scope
// `using namespace` leaks into every file that follows this one in a unity build, where it
// collides with the identically named helpers in SMotionDefTakes.cpp. An alias introduces only
// the name MFL, which nothing else uses.
namespace MFL = MotionLibraryUI;

// -------------------------------------------------------------------------------------------------

void SMotionLibrary::Construct(const FArguments&)
{
	SortColumn = MFL::ColumnName;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(FMargin(16.f, 14.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(SummaryBox, SBox)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 8.f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SAssignNew(FilterBox, SBox)
				]

				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(16.f, 0.f, 0.f, 0.f)
				[
					// Built once and kept. Rebuilding it with the filters beside it would take the
					// text and the caret away mid-word.
					SNew(SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search names and prompts"))
					.OnTextChanged_Lambda([this](const FText& Text)
					{
						SearchText = Text.ToString();
						ApplyFilter();
					})
				]
			]

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(1.f)
				[
					SAssignNew(ListView, SListView<TSharedPtr<FMotionLibraryEntry>>)
					.ListItemsSource(&Visible)
					.SelectionMode(ESelectionMode::Multi)
					.OnGenerateRow(this, &SMotionLibrary::OnGenerateRow)
					.OnSelectionChanged(this, &SMotionLibrary::OnSelectionChanged)
					.OnMouseButtonDoubleClick(this, &SMotionLibrary::OnRowDoubleClicked)
					.HeaderRow
					(
						SNew(SHeaderRow)

						+ SHeaderRow::Column(MFL::ColumnName)
						.DefaultLabel(LOCTEXT("ColName", "Definition"))
						.FillWidth(0.20f)
						.SortMode_Lambda([this]()
						{
							return SortColumn == MFL::ColumnName ? SortMode : EColumnSortMode::None;
						})
						.OnSort(this, &SMotionLibrary::OnSortChanged)

						+ SHeaderRow::Column(MFL::ColumnStatus)
						.DefaultLabel(LOCTEXT("ColStatus", "Status"))
						.FillWidth(0.15f)
						.SortMode_Lambda([this]()
						{
							return SortColumn == MFL::ColumnStatus ? SortMode : EColumnSortMode::None;
						})
						.OnSort(this, &SMotionLibrary::OnSortChanged)

						+ SHeaderRow::Column(MFL::ColumnProvider)
						.DefaultLabel(LOCTEXT("ColProvider", "Provider"))
						.FillWidth(0.12f)
						.SortMode_Lambda([this]()
						{
							return SortColumn == MFL::ColumnProvider ? SortMode : EColumnSortMode::None;
						})
						.OnSort(this, &SMotionLibrary::OnSortChanged)

						+ SHeaderRow::Column(MFL::ColumnTakes)
						.DefaultLabel(LOCTEXT("ColTakes", "Takes"))
						.FillWidth(0.09f)

						// Wider than the prompt beside it: an animation's name is a handle somebody
						// copies, and half a handle is no use. The prompt is read, not copied, and
						// its full text is one hover away.
						+ SHeaderRow::Column(MFL::ColumnAnimation)
						.DefaultLabel(LOCTEXT("ColAnimation", "Animation"))
						.FillWidth(0.22f)

						+ SHeaderRow::Column(MFL::ColumnPrompt)
						.DefaultLabel(LOCTEXT("ColPrompt", "Prompt"))
						.FillWidth(0.22f)
					)
				]
			]

			// Only ever on screen when the list is empty, which is the one time a person needs
			// telling why. Two different reasons, and they want two different answers.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]()
				{
					return Visible.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					MFL::Card(SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor(MFL::QuietColour))
						.Text_Lambda([this]()
						{
							if (Entries.Num() > 0)
							{
								return LOCTEXT("NothingMatches",
									"Nothing here matches. Try another filter, or clear the search.");
							}

							return LOCTEXT("NoDefinitions",
								"No motion definitions in this project yet.\n\n"
								"Press New Definition above, or right-click in the Content Browser and "
								"pick Automation Forge › MotionForge › Motion Definition. "
								"An agent can author a whole library in one call.");
						}))
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
			[
				SAssignNew(FooterBox, SBox)
			]
		]
	];

	// Somebody else adds, deletes or renames a definition - an agent, a content browser, a sync.
	// Queued rather than acted on, because a registry scan fires this once per asset.
	FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	AssetAddedHandle   = Registry.Get().OnAssetAdded().AddSP(this, &SMotionLibrary::OnAssetRegistryChanged);
	AssetRemovedHandle = Registry.Get().OnAssetRemoved().AddSP(this, &SMotionLibrary::OnAssetRegistryChanged);
	AssetRenamedHandle = Registry.Get().OnAssetRenamed().AddSP(this, &SMotionLibrary::OnAssetRenamed);

	RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SMotionLibrary::Poll));

	Rescan();
}

SMotionLibrary::~SMotionLibrary()
{
	// Guarded: during editor shutdown the registry may already be gone, and asking for it back would
	// load a module on its way out.
	if (FAssetRegistryModule* Registry =
			FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		Registry->Get().OnAssetAdded().Remove(AssetAddedHandle);
		Registry->Get().OnAssetRemoved().Remove(AssetRemovedHandle);
		Registry->Get().OnAssetRenamed().Remove(AssetRenamedHandle);
	}
}

// -------------------------------------------------------------------------------------------------
// Reading the project
// -------------------------------------------------------------------------------------------------

void SMotionLibrary::OnAssetRegistryChanged(const FAssetData& Asset)
{
	if (Asset.AssetClassPath == UMotionDef::StaticClass()->GetClassPathName())
	{
		bRescanQueued = true;
	}
}

void SMotionLibrary::OnAssetRenamed(const FAssetData& Asset, const FString&)
{
	OnAssetRegistryChanged(Asset);
}

EActiveTimerReturnType SMotionLibrary::Poll(double, float)
{
	if (bRescanQueued)
	{
		bRescanQueued = false;
		Rescan();
		return EActiveTimerReturnType::Continue;
	}

	if (Fingerprint() != LastFingerprint)
	{
		Rescan();
	}

	return EActiveTimerReturnType::Continue;
}

uint32 SMotionLibrary::Fingerprint() const
{
	uint32 Hash = GetTypeHash(Entries.Num());

	for (const TSharedPtr<FMotionLibraryEntry>& Entry : Entries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		// Resolve rather than load. Rescan loaded these already, so this is a map lookup; a
		// definition somebody unloaded simply stops contributing until the next rescan.
		const UMotionDef* Def =
			Cast<UMotionDef>(FSoftObjectPath(Entry->Status.AssetPath).ResolveObject());

		if (!Def)
		{
			continue;
		}

		Hash = HashCombine(Hash, GetTypeHash((uint8)Def->Status));
		Hash = HashCombine(Hash, GetTypeHash(Def->Candidates.Num()));
		Hash = HashCombine(Hash, GetTypeHash(Def->SelectedMotionId));
		Hash = HashCombine(Hash, GetTypeHash(Def->ImportedSequence.ToString()));
		Hash = HashCombine(Hash, GetTypeHash(Def->ProviderId));
		Hash = HashCombine(Hash, GetTypeHash(Def->Variants));
	}

	return Hash;
}

void SMotionLibrary::Rescan()
{
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	if (!Subsystem)
	{
		return;
	}

	// The same call an agent makes. The panel adds nothing but the drawing.
	const TArray<FMotionDefinitionStatus> Statuses = Subsystem->GetStatus({});

	// Selection survives a rescan: it is what the buttons below act on, and losing it every time a
	// take finishes somewhere else in the library would be its own small cruelty.
	TSet<FString> SelectedPaths;
	if (ListView.IsValid())
	{
		for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
		{
			if (Entry.IsValid())
			{
				SelectedPaths.Add(Entry->Status.AssetPath);
			}
		}
	}

	Entries.Reset(Statuses.Num());

	for (const FMotionDefinitionStatus& Status : Statuses)
	{
		TSharedRef<FMotionLibraryEntry> Entry = MakeShared<FMotionLibraryEntry>();
		Entry->Status = Status;

		for (const FMotionTakeInfo& Take : Status.Takes)
		{
			if (Take.Status == EMotionJobStatus::Finished)
			{
				++Entry->UsableTakes;
			}
		}

		const FMotionProviderCaps Caps = Subsystem->GetProviderCaps(Status.ProviderId);
		Entry->ProviderName = !Caps.DisplayName.IsEmpty()
			? Caps.DisplayName
			: (Status.ProviderId.IsNone() ? TEXT("None") : Status.ProviderId.ToString());

		if (!Status.ImportedSequencePath.IsEmpty() && !Status.bImportedSequenceMissing)
		{
			Entry->AnimationName = FPackageName::ObjectPathToObjectName(Status.ImportedSequencePath);
		}

		Entries.Add(Entry);
	}

	LastFingerprint = Fingerprint();

	ApplyFilter();

	if (ListView.IsValid() && SelectedPaths.Num() > 0)
	{
		TArray<TSharedPtr<FMotionLibraryEntry>> Restore;
		for (const TSharedPtr<FMotionLibraryEntry>& Entry : Visible)
		{
			if (Entry.IsValid() && SelectedPaths.Contains(Entry->Status.AssetPath))
			{
				Restore.Add(Entry);
			}
		}

		ListView->SetItemSelection(Restore, true, ESelectInfo::Direct);
	}

	// After the selection is back, so the footer prices what is actually selected.
	if (FooterBox.IsValid())
	{
		FooterBox->SetContent(BuildFooter());
	}
}

void SMotionLibrary::ApplyFilter()
{
	Visible.Reset(Entries.Num());

	const FString Search = SearchText.TrimStartAndEnd();

	for (const TSharedPtr<FMotionLibraryEntry>& Entry : Entries)
	{
		if (!Entry.IsValid() || !MFL::MatchesFilter(Filter, Entry->Status.Status))
		{
			continue;
		}

		if (!Search.IsEmpty()
			&& !Entry->Status.Name.Contains(Search)
			&& !Entry->Status.Prompt.Contains(Search))
		{
			continue;
		}

		Visible.Add(Entry);
	}

	const bool bAscending = SortMode == EColumnSortMode::Ascending;
	const FName Column = SortColumn;

	Visible.Sort([Column, bAscending](
		const TSharedPtr<FMotionLibraryEntry>& A, const TSharedPtr<FMotionLibraryEntry>& B)
	{
		int32 Order = 0;

		if (Column == MFL::ColumnStatus)
		{
			// Enum order is workflow order, which is the useful one: drafts, then everything in
			// flight, then what is waiting on a person, then what is done.
			Order = (int32)A->Status.Status - (int32)B->Status.Status;
		}
		else if (Column == MFL::ColumnProvider)
		{
			Order = A->ProviderName.Compare(B->ProviderName, ESearchCase::IgnoreCase);
		}

		// Name breaks every other tie, so a re-sort never shuffles equal rows about.
		if (Order == 0)
		{
			Order = A->Status.Name.Compare(B->Status.Name, ESearchCase::IgnoreCase);
		}

		return bAscending ? Order < 0 : Order > 0;
	});

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	if (SummaryBox.IsValid())
	{
		SummaryBox->SetContent(BuildSummary());
	}

	if (FilterBox.IsValid())
	{
		FilterBox->SetContent(BuildFilters());
	}
}

// -------------------------------------------------------------------------------------------------
// Drawing
// -------------------------------------------------------------------------------------------------

TSharedRef<SWidget> SMotionLibrary::BuildSummary()
{
	// What the library is, in one line. The per-status counts live on the filters below rather than
	// being said twice - two places counting the same thing eventually disagree.
	const int32 Total = Entries.Num();

	int32 Missing = 0;
	for (const TSharedPtr<FMotionLibraryEntry>& Entry : Entries)
	{
		if (Entry.IsValid() && Entry->Status.bImportedSequenceMissing)
		{
			++Missing;
		}
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	TSharedRef<SHorizontalBox> TopRow = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(Total == 0
				? LOCTEXT("SummaryEmpty", "No motion definitions")
				: FText::Format(
					LOCTEXT("SummaryFmt", "{0} motion {0}|plural(one=definition,other=definitions)"),
					Total))
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("RefreshTip",
				"Read every definition again. The list keeps itself current, so this is for when you "
				"would rather not wait for it to notice."))
			.OnClicked(this, &SMotionLibrary::OnRefresh)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Refresh"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("NewDefinition", "New Definition"))
			.ToolTipText(LOCTEXT("NewDefinitionTip",
				"Create a motion definition, with the project's default character and provider "
				"already filled in. You choose the name and where it goes."))
			.OnClicked(this, &SMotionLibrary::OnNewDefinition)
		];

	Body->AddSlot().AutoHeight()[ TopRow ];

	// A definition reporting Ready whose animation is gone. Said once, here, because it is a fact
	// about the library rather than about any one row - the rows say it too, in their own column.
	if (Missing > 0)
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Warning"))
				.ColorAndOpacity(FSlateColor(MFL::WarnColour))
			]

			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(MFL::WarnColour))
				.Text(FText::Format(
					LOCTEXT("MissingFmt",
						"{0} {0}|plural(one=definition,other=definitions) point at an animation that "
						"is no longer in the project. Importing the chosen take again replaces it."),
					Missing))
			]
		];
	}

	return MFL::Card(Body);
}

TSharedRef<SWidget> SMotionLibrary::BuildFilters()
{
	// Each filter says how much is behind it, which makes it the summary as well as the control.
	//
	// All is deliberately bare: its count is the total, the card above already says the total, and
	// a panel that says one number in two places has started arguing with itself the moment they
	// are computed differently.
	const auto Label = [this](const FText& Name, EMotionLibraryFilter Which)
	{
		const int32 Count = MFL::CountMatching(Entries, Which);
		return Count == 0
			? Name
			: FText::Format(LOCTEXT("FilterCountFmt", "{0}  {1}"), Name, Count);
	};

	return SNew(SSegmentedControl<EMotionLibraryFilter>)
		.Value(Filter)
		.OnValueChanged_Lambda([this](EMotionLibraryFilter NewFilter)
		{
			Filter = NewFilter;
			ApplyFilter();
		})

		+ SSegmentedControl<EMotionLibraryFilter>::Slot(EMotionLibraryFilter::All)
		.Text(LOCTEXT("FilterAll", "All"))
		.ToolTip(LOCTEXT("FilterAllTip", "Every definition in the project. The count is on the card above."))

		+ SSegmentedControl<EMotionLibraryFilter>::Slot(EMotionLibraryFilter::Draft)
		.Text(Label(LOCTEXT("FilterDraft", "Draft"), EMotionLibraryFilter::Draft))
		.ToolTip(LOCTEXT("FilterDraftTip", "Written but never generated. Nothing has been spent on these."))

		+ SSegmentedControl<EMotionLibraryFilter>::Slot(EMotionLibraryFilter::Working)
		.Text(Label(LOCTEXT("FilterWorking", "Working"), EMotionLibraryFilter::Working))
		.ToolTip(LOCTEXT("FilterWorkingTip", "Generating, downloading or importing. Nothing to do but wait."))

		+ SSegmentedControl<EMotionLibraryFilter>::Slot(EMotionLibraryFilter::Review)
		.Text(Label(LOCTEXT("FilterReview", "Review"), EMotionLibraryFilter::Review))
		.ToolTip(LOCTEXT("FilterReviewTip", "Takes are waiting for somebody to choose one."))

		+ SSegmentedControl<EMotionLibraryFilter>::Slot(EMotionLibraryFilter::Ready)
		.Text(Label(LOCTEXT("FilterReady", "Ready"), EMotionLibraryFilter::Ready))
		.ToolTip(LOCTEXT("FilterReadyTip", "An animation exists in the project."))

		+ SSegmentedControl<EMotionLibraryFilter>::Slot(EMotionLibraryFilter::Failed)
		.Text(Label(LOCTEXT("FilterFailed", "Failed"), EMotionLibraryFilter::Failed))
		.ToolTip(LOCTEXT("FilterFailedTip", "Something went wrong. Generating again is safe."));
}

TSharedRef<SWidget> SMotionLibrary::BuildFooter()
{
	const TArray<TSharedPtr<FMotionLibraryEntry>> Selected =
		ListView.IsValid() ? ListView->GetSelectedItems() : TArray<TSharedPtr<FMotionLibraryEntry>>();

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	Body->AddSlot().AutoHeight()
	[
		SNew(STextBlock)
		.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		.Text(Selected.Num() == 0
			? LOCTEXT("NothingSelected", "Nothing selected")
			: FText::Format(
				LOCTEXT("SelectedFmt", "{0} selected"), Selected.Num()))
	];

	const TArray<TSharedPtr<FMotionLibraryEntry>> Generateable = GenerateableSelection();
	const TArray<TSharedPtr<FMotionLibraryEntry>> Importable   = ImportableSelection();

	// What Generate would cost, before it is pressed, for exactly the definitions it would run on.
	// The whole point of a library view over a folder of icons: a spend across many definitions is
	// the one nobody prices in their head.
	FText CostLine;
	FLinearColor CostColour = MFL::QuietColour;
	bool bMetered = false;

	if (Generateable.Num() > 0)
	{
		if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
		{
			TArray<FMotionDefSpec> Specs;
			Specs.Reserve(Generateable.Num());

			for (const TSharedPtr<FMotionLibraryEntry>& Entry : Generateable)
			{
				FMotionDefSpec Spec;
				Spec.Length     = Entry->Status.Length;
				Spec.Variants   = Entry->Status.Variants;
				Spec.ProviderId = Entry->Status.ProviderId;
				Specs.Add(MoveTemp(Spec));

				bMetered |= Subsystem->GetProviderCaps(Entry->Status.ProviderId).bIsMetered;
			}

			const FMotionCostEstimate Estimate = Subsystem->EstimateGenerationCost(Specs);

			if (!bMetered)
			{
				CostLine = FText::Format(
					LOCTEXT("LibCostFreeFmt",
						"Generating {0} {0}|plural(one=definition,other=definitions) - free on this "
						"provider, so ask for more takes"),
					Generateable.Num());
			}
			else if (Estimate.EstimatedCost > 0.f)
			{
				CostLine = FText::Format(
					LOCTEXT("LibCostMoneyFmt",
						"Generating {0} {0}|plural(one=definition,other=definitions) = {1}s billed, "
						"about {2} {3}"),
					Generateable.Num(), Estimate.BilledSeconds,
					MFL::Money(Estimate.EstimatedCost), FText::FromString(Estimate.Currency));
				CostColour = MFL::WarnColour;
			}
			else
			{
				CostLine = FText::Format(
					LOCTEXT("LibCostSecondsFmt",
						"Generating {0} {0}|plural(one=definition,other=definitions) = {1}s billed"),
					Generateable.Num(), Estimate.BilledSeconds);
				CostColour = MFL::WarnColour;
			}
		}
	}
	else if (Selected.Num() > 0)
	{
		// Say which one and why, rather than a disabled button with no explanation. With one
		// selected that is the definition's own reason; with several it is a count.
		int32 Blocked = 0;
		FString FirstReason;

		for (const TSharedPtr<FMotionLibraryEntry>& Entry : Selected)
		{
			if (Entry.IsValid() && Entry->bReadinessKnown && !Entry->Readiness.bCanGenerate)
			{
				++Blocked;
				if (FirstReason.IsEmpty())
				{
					FirstReason = Entry->Readiness.Problem;
				}
			}
		}

		if (Blocked == 1 && !FirstReason.IsEmpty())
		{
			CostLine = FText::FromString(FirstReason);
			CostColour = MFL::WarnColour;
		}
		else if (Blocked > 1)
		{
			CostLine = FText::Format(
				LOCTEXT("LibBlockedManyFmt",
					"None of the {0} selected can generate yet. Open one to see what it is waiting for."),
				Blocked);
			CostColour = MFL::WarnColour;
		}
	}

	if (!CostLine.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(CostColour.Equals(MFL::WarnColour) ? "Icons.Warning" : "Icons.Info"))
				.ColorAndOpacity(FSlateColor(CostColour))
			]

			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(CostLine)
				.ColorAndOpacity(FSlateColor(CostColour))
				.AutoWrapText(true)
			]
		];
	}

	// Safe on the left, the one that spends money pushed right and coloured, so it cannot be hit on
	// the way to Open.
	Body->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenSelected", "Open"))
			.ToolTipText(LOCTEXT("OpenSelectedTip",
				"Open each selected definition in its own window. Double-clicking a row does the same."))
			.IsEnabled(Selected.Num() > 0)
			.OnClicked(this, &SMotionLibrary::OnOpenSelected)
		]

		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("ShowAnimations", "Show Animations"))
			.ToolTipText(LOCTEXT("ShowAnimationsTip",
				"Find what these definitions produced in the Content Browser."))
			.IsEnabled_Lambda([this]()
			{
				if (!ListView.IsValid())
				{
					return false;
				}
				for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
				{
					if (Entry.IsValid() && !Entry->AnimationName.IsEmpty())
					{
						return true;
					}
				}
				return false;
			})
			.OnClicked(this, &SMotionLibrary::OnShowAnimations)
		]

		+ SHorizontalBox::Slot().FillWidth(1.f)[ SNew(SSpacer) ]

		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(SButton)
			.Text(Importable.Num() > 1
				? FText::Format(LOCTEXT("ImportNFmt", "Import {0} Takes"), Importable.Num())
				: LOCTEXT("ImportChosen", "Import Chosen Take"))
			.ToolTipText(LOCTEXT("ImportChosenTip",
				"Fetch each chosen take, normalise it and import it onto its character's skeleton.\n\n"
				"On a subscription this is what bills; on pay-as-you-go the money went at generation."))
			.IsEnabled(Importable.Num() > 0)
			.OnClicked(this, &SMotionLibrary::OnImportSelected)
		]

		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.Text(Generateable.Num() > 1
				? FText::Format(LOCTEXT("GenerateNFmt", "Generate {0}"), Generateable.Num())
				: LOCTEXT("GenerateOne", "Generate"))
			.ToolTipText(LOCTEXT("GenerateSelectedTip",
				"Submit these definitions to their providers. Definitions already generating are "
				"skipped rather than charged twice.\n\n"
				"What it costs is on the line above, before you press it."))
			.IsEnabled(Generateable.Num() > 0)
			.OnClicked(this, &SMotionLibrary::OnGenerateSelected)
		]
	];

	return MFL::Card(Body);
}

TSharedRef<ITableRow> SMotionLibrary::OnGenerateRow(
	TSharedPtr<FMotionLibraryEntry> Entry, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(MFL::SMotionLibraryRow, Owner).Entry(Entry);
}

void SMotionLibrary::OnSelectionChanged(TSharedPtr<FMotionLibraryEntry>, ESelectInfo::Type)
{
	// Readiness is asked here and nowhere else: it loads the definition and reads the credential
	// vault, which is right for the handful somebody selected and wrong for a whole library.
	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		if (ListView.IsValid())
		{
			for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
			{
				if (Entry.IsValid())
				{
					Entry->Readiness = Subsystem->CheckReadiness(Entry->Status.AssetPath);
					Entry->bReadinessKnown = true;
				}
			}
		}
	}

	if (FooterBox.IsValid())
	{
		FooterBox->SetContent(BuildFooter());
	}
}

void SMotionLibrary::OnRowDoubleClicked(TSharedPtr<FMotionLibraryEntry> Entry)
{
	if (Entry.IsValid() && GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Entry->Status.AssetPath);
	}
}

void SMotionLibrary::OnSortChanged(EColumnSortPriority::Type, const FName& Column, EColumnSortMode::Type Mode)
{
	SortColumn = Column;
	SortMode = Mode;
	ApplyFilter();
}

// -------------------------------------------------------------------------------------------------
// The verbs
// -------------------------------------------------------------------------------------------------

TArray<TSharedPtr<FMotionLibraryEntry>> SMotionLibrary::GenerateableSelection() const
{
	TArray<TSharedPtr<FMotionLibraryEntry>> Result;

	if (!ListView.IsValid())
	{
		return Result;
	}

	for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
	{
		// Never offered where it cannot work. Submitting the whole selection and reporting failures
		// afterwards is the same button with the answer arriving too late to act on.
		if (Entry.IsValid() && Entry->bReadinessKnown && Entry->Readiness.bCanGenerate)
		{
			Result.Add(Entry);
		}
	}

	return Result;
}

TArray<TSharedPtr<FMotionLibraryEntry>> SMotionLibrary::ImportableSelection() const
{
	TArray<TSharedPtr<FMotionLibraryEntry>> Result;

	if (!ListView.IsValid())
	{
		return Result;
	}

	for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
	{
		if (!Entry.IsValid() || Entry->Status.SelectedMotionId.IsEmpty())
		{
			continue;
		}

		for (const FMotionTakeInfo& Take : Entry->Status.Takes)
		{
			if (Take.MotionId == Entry->Status.SelectedMotionId
				&& Take.Status == EMotionJobStatus::Finished)
			{
				Result.Add(Entry);
				break;
			}
		}
	}

	return Result;
}

FReply SMotionLibrary::OnRefresh()
{
	Rescan();
	return FReply::Handled();
}

FReply SMotionLibrary::OnNewDefinition()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	UMotionDefFactory* Factory = NewObject<UMotionDefFactory>();

	// Offered where the pipeline writes them, which is where every other definition already is, and
	// still a dialogue - the name is the one thing nobody else can decide.
	UObject* Created = AssetTools.CreateAssetWithDialog(
		Factory->GetDefaultNewAssetName(),
		UMotionForgeSettings::Get()->GetDefinitionsPath(),
		UMotionDef::StaticClass(),
		Factory);

	if (Created && GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Created);
	}

	return FReply::Handled();
}

FReply SMotionLibrary::OnOpenSelected()
{
	if (ListView.IsValid() && GEditor)
	{
		UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();

		for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
		{
			if (Entry.IsValid())
			{
				Editors->OpenEditorForAsset(Entry->Status.AssetPath);
			}
		}
	}

	return FReply::Handled();
}

FReply SMotionLibrary::OnShowAnimations()
{
	if (!ListView.IsValid() || !GEditor)
	{
		return FReply::Handled();
	}

	TArray<UObject*> Objects;

	for (const TSharedPtr<FMotionLibraryEntry>& Entry : ListView->GetSelectedItems())
	{
		if (!Entry.IsValid() || Entry->Status.ImportedSequencePath.IsEmpty())
		{
			continue;
		}

		if (UObject* Sequence = FSoftObjectPath(Entry->Status.ImportedSequencePath).TryLoad())
		{
			Objects.Add(Sequence);
		}
	}

	if (Objects.Num() > 0)
	{
		GEditor->SyncBrowserToObjects(Objects);
	}

	return FReply::Handled();
}

FReply SMotionLibrary::OnGenerateSelected()
{
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	TArray<FString> Paths;
	for (const TSharedPtr<FMotionLibraryEntry>& Entry : GenerateableSelection())
	{
		Paths.Add(Entry->Status.AssetPath);
	}

	if (Paths.Num() > 0)
	{
		Subsystem->Generate(Paths);
		Rescan();
	}

	return FReply::Handled();
}

FReply SMotionLibrary::OnImportSelected()
{
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	if (!Subsystem)
	{
		return FReply::Handled();
	}

	TArray<FString> Paths;
	for (const TSharedPtr<FMotionLibraryEntry>& Entry : ImportableSelection())
	{
		Paths.Add(Entry->Status.AssetPath);
	}

	if (Paths.Num() > 0)
	{
		Subsystem->DownloadSelected(Paths);
		Rescan();
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
