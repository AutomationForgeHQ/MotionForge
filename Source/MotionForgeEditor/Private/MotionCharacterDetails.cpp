// Copyright Blackcode SA. All rights reserved.

#include "MotionCharacterDetails.h"

#include "IMotionProvider.h"
#include "MotionCharacter.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IStructureDetailsView.h"
#include "Misc/MessageDialog.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

// Named, not anonymous. Unity builds fold several .cpp files into one translation unit, and this
// file's neighbour has helpers by exactly these names - two identical palettes in one namespace is
// a redefinition, not a coincidence worth debugging twice.
namespace MotionCharacterUI
{
	const FLinearColor GoodColour(0.30f, 0.78f, 0.45f);
	const FLinearColor WarnColour(0.95f, 0.65f, 0.20f);
	const FLinearColor BadColour(0.90f, 0.36f, 0.36f);
	const FLinearColor QuietColour(0.55f, 0.55f, 0.58f);

	TSharedRef<SWidget> Dot(const FLinearColor& Colour)
	{
		return SNew(SBox)
			.WidthOverride(8.f).HeightOverride(8.f).VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
				.ColorAndOpacity(Colour)
			];
	}

	void Tell(const FString& Message, bool bSuccess)
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = bSuccess ? 6.f : 12.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

TSharedRef<IDetailCustomization> FMotionCharacterDetails::MakeInstance()
{
	return MakeShared<FMotionCharacterDetails>();
}

void FMotionCharacterDetails::Rebuild()
{
	if (Layout)
	{
		Layout->ForceRefreshDetails();
	}
}

void FMotionCharacterDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	Layout = &DetailBuilder;
	OptionForms.Reset();

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	Customised = Objects.Num() == 1 ? Cast<UMotionCharacter>(Objects[0].Get()) : nullptr;

	UMotionCharacter* Char = Character();
	if (!Char)
	{
		return;
	}

	// Redraw when anything the checklist reads is edited. Without this the steps described the
	// asset as it was when the window opened, and changing the provider changed nothing on screen
	// until the asset was saved and reopened.
	for (const FName Field : {
			GET_MEMBER_NAME_CHECKED(UMotionCharacter, ProviderId),
			GET_MEMBER_NAME_CHECKED(UMotionCharacter, ProviderCharacterId),
			GET_MEMBER_NAME_CHECKED(UMotionCharacter, TargetSkeleton),
			GET_MEMBER_NAME_CHECKED(UMotionCharacter, PreviewMesh),
			GET_MEMBER_NAME_CHECKED(UMotionCharacter, ProviderMesh),
			GET_MEMBER_NAME_CHECKED(UMotionCharacter, Retargeter) })
	{
		if (TSharedPtr<IPropertyHandle> Handle = DetailBuilder.GetProperty(Field))
		{
			Handle->SetOnPropertyValueChanged(
				FSimpleDelegate::CreateSP(this, &FMotionCharacterDetails::Rebuild));
		}
	}

	// A character that names no provider is not "a Uthana character with the field left blank" - it
	// is a universal one, and whichever provider the definition uses decides what it needs. Judging
	// it against the project default and demanding that provider's ritual was how a character
	// prepared for Kimodo came to be shown an Upload to Uthana button.
	const bool bUniversal = Char->ProviderId.IsNone();

	FMotionProviderCaps Caps;
	if (!bUniversal)
	{
		if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
		{
			Caps = Subsystem->GetProviderCaps(Char->ProviderId);
		}
	}

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
		TEXT("Pairing"), LOCTEXT("PairingCategory", "Pairing"), ECategoryPriority::Important);

	// ---------------------------------------------------------------------------------------------

	if (!bUniversal && Caps.ProviderId.IsNone())
	{
		AddStep(Category, LOCTEXT("StepProvider", "Provider"),
			FText::Format(LOCTEXT("NoSuchProviderFmt", "'{0}' is not installed"),
				FText::FromName(Char->ProviderId)),
			MotionCharacterUI::BadColour);
		return;
	}

	AddStep(Category, LOCTEXT("StepProvider", "Provider"),
		bUniversal
			? LOCTEXT("ProviderUniversal",
				"any - this character names none, so the definition's provider decides")
			: FText::FromString(Caps.DisplayName),
		MotionCharacterUI::GoodColour);

	// 1. The skeleton finished animations land on. Nothing works without it.
	const bool bHasSkeleton = !Char->TargetSkeleton.IsNull();
	AddStep(Category, LOCTEXT("StepSkeleton", "Target skeleton"),
		bHasSkeleton
			? FText::FromString(Char->TargetSkeleton.GetAssetName())
			: LOCTEXT("NoSkeleton", "not set - imported animations would have nothing to bind to"),
		bHasSkeleton ? MotionCharacterUI::GoodColour : MotionCharacterUI::BadColour);

	// 2. The mesh that represents this character. Required before anything can be uploaded.
	const bool bHasPreview = !Char->PreviewMesh.IsNull();
	AddStep(Category, LOCTEXT("StepPreview", "Preview mesh"),
		bHasPreview
			? FText::FromString(Char->PreviewMesh.GetAssetName())
			: (Caps.bSupportsCharacterUpload
				? LOCTEXT("NoPreviewNeeded", "not set - this is the mesh that gets uploaded")
				: LOCTEXT("NoPreviewOptional",
					"not set - needed for previewing, and to retarget onto")),
		bHasPreview
			? MotionCharacterUI::GoodColour
			: (Caps.bSupportsCharacterUpload ? MotionCharacterUI::BadColour : MotionCharacterUI::QuietColour));

	// 3. Pairing, but only where a provider has something to pair with. Kimodo generates on its own
	//    fixed rig, so it has nothing to upload and no id to paste - showing those steps would be
	//    inventing work. A universal character cannot know, so it says so instead of demanding.
	if (bUniversal)
	{
		AddStep(Category, LOCTEXT("StepPaired", "Paired"),
			LOCTEXT("PairedUniversal",
				"not needed here - a provider that generates on its own rig needs no upload. One "
				"that retargets on its own hardware will want a Provider Character Id, so name that "
				"provider above if you intend to use it."),
			MotionCharacterUI::QuietColour);
	}
	else if (Caps.bSupportsCharacterUpload)
	{
		// The fact only. The upload itself is the provider's own action, below, with its options.
		const bool bPaired = !Char->ProviderCharacterId.IsEmpty();

		AddStep(Category, LOCTEXT("StepPaired", "Paired"),
			bPaired
				? FText::FromString(Char->ProviderCharacterId)
				: LOCTEXT("NotPaired", "not uploaded yet - the upload is below"),
			bPaired ? MotionCharacterUI::GoodColour : MotionCharacterUI::WarnColour);
	}

	// 4. Which of the two pipelines this character uses.
	//
	// Not a step that can be missing, which is what the earlier version got wrong. The import path
	// reads `bRetargeting = ProviderMesh != nullptr`: an empty Provider's rig is not an unfinished
	// setup, it is the *direct* pipeline, chosen. Calling it "not set" in amber told somebody with
	// a working character that they had work left.
	const bool bRetargets = !Char->ProviderMesh.IsNull();

	AddStep(Category, LOCTEXT("StepPipeline", "Pipeline"),
		bRetargets
			? FText::Format(LOCTEXT("PipelineRetargetFmt", "retargeted - clips arrive on {0}"),
				FText::FromString(Char->ProviderMesh.GetAssetName()))
			: FText::Format(LOCTEXT("PipelineDirectFmt", "direct - clips import straight onto {0}"),
				Char->TargetSkeleton.IsNull()
					? LOCTEXT("TheSkeleton", "the target skeleton")
					: FText::FromString(Char->TargetSkeleton.GetAssetName())),
		MotionCharacterUI::GoodColour);

	// Direct is only correct where the provider hands back the bone names this skeleton already
	// has. Where it does not, the clip imports without a warning and comes out subtly twisted -
	// which is worth saying here rather than leaving to be discovered in the viewport.
	if (!bUniversal && !bRetargets && Caps.bRequiresRetarget)
	{
		AddStep(Category, FText::GetEmpty(),
			FText::Format(
				LOCTEXT("DirectRiskFmt",
					"{0} generates on its own rig, so direct only works if its bone names match this "
					"skeleton. If clips come out twisted, set a Provider's rig and a retargeter."),
				FText::FromString(Caps.DisplayName)),
			MotionCharacterUI::WarnColour);
	}

	// 5. The retargeter, and only where one is actually used. Authored by hand in the IK Retargeter
	//    editor - there is no button for "make good decisions about bone mapping", and pretending
	//    otherwise would be worse than saying what to do.
	if (bRetargets)
	{
		const bool bHasRetargeter = !Char->Retargeter.IsNull();

		AddStep(Category, LOCTEXT("StepRetargeter", "Retargeter"),
			bHasRetargeter
				? FText::FromString(Char->Retargeter.GetAssetName())
				: LOCTEXT("NoRetargeter",
					"not set - author one from the provider's rig to the preview mesh, or the import "
					"stops on the generator's skeleton"),
			bHasRetargeter ? MotionCharacterUI::GoodColour : MotionCharacterUI::BadColour);
	}

	// What each provider offers this character, in the provider's own terms: build a rig for it,
	// upload it, fetch its copy back. A universal character sees every installed provider, because
	// it could go to any of them; one prepared for a provider sees only that one.
	AddProviderSections(DetailBuilder, Char, bUniversal);

	// The verdict, in the pipeline's own words rather than this page's opinion of them.
	//
	// For a character prepared for a provider, the same question Generate asks - including a provider
	// rig with nothing to move clips off it - so this line and the provider's section below cannot
	// disagree about one character on one page.
	FString Reason;
	bool bUsable = false;
	if (bUniversal)
	{
		bUsable = Char->IsUsable(Reason);
	}
	else if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		bUsable = Subsystem->DoesCharacterSuit(Char, Char->ProviderId, &Reason);
	}
	else
	{
		bUsable = Char->IsUsableForProvider(Caps.bSupportsCharacterUpload, Reason);
	}

	Category.AddCustomRow(LOCTEXT("VerdictRow", "Ready"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			MotionCharacterUI::Dot(bUsable ? MotionCharacterUI::GoodColour : MotionCharacterUI::BadColour)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(bUsable
				? (bUniversal
					? LOCTEXT("UsableAny", "Ready - usable with any provider that needs no upload")
					: FText::Format(LOCTEXT("UsableFmt", "Ready to generate with {0}"),
						FText::FromString(Caps.DisplayName)))
				: FText::FromString(Reason))
			.ColorAndOpacity(FSlateColor(bUsable ? MotionCharacterUI::GoodColour : MotionCharacterUI::BadColour))
			.AutoWrapText(true)
		]
	];
}

void FMotionCharacterDetails::AddStep(
	IDetailCategoryBuilder& Category,
	const FText& Label,
	const FText& Value,
	const FLinearColor& Colour,
	TSharedPtr<SWidget> Action)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			MotionCharacterUI::Dot(Colour)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(120.f)
			[
				SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor(MotionCharacterUI::QuietColour))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Value).ColorAndOpacity(FSlateColor(Colour)).AutoWrapText(true)
		];

	if (Action.IsValid())
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(12.f, 0.f, 0.f, 0.f)
		[
			Action.ToSharedRef()
		];
	}

	Category.AddCustomRow(Label).WholeRowContent()[ Row ];
}

// -------------------------------------------------------------------------------------------------

void FMotionCharacterDetails::AddProviderSections(IDetailLayoutBuilder& DetailBuilder, UMotionCharacter* Char, bool bUniversal)
{
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();
	if (!Subsystem || !Char)
	{
		return;
	}

	TArray<FName> Ids;
	if (bUniversal)
	{
		Ids = Subsystem->GetProviderIds();
	}
	else
	{
		Ids.Add(Char->ProviderId);
	}

	const FString CharPath = Char->GetPathName();

	for (const FName Id : Ids)
	{
		TSharedPtr<IMotionProvider> Provider = Subsystem->FindProvider(Id);
		if (!Provider.IsValid())
		{
			continue;
		}

		TArray<FMotionCharacterSetupAction> Actions;
		Provider->GetCharacterSetupActions(Char, Actions);
		const FString Route = Provider->DescribeCharacterRoute(Char);

		FString Why;
		EMotionBlocker Blocker = EMotionBlocker::None;
		const bool bSuits = Subsystem->DoesCharacterSuit(Char, Id, &Why, &Blocker);

		IDetailCategoryBuilder& Section = DetailBuilder.EditCategory(
			FName(*FString::Printf(TEXT("With%s"), *Id.ToString())),
			FText::Format(LOCTEXT("WithProviderFmt", "With {0}"), FText::FromString(Provider->GetDisplayName())),
			ECategoryPriority::Important);

		// For a character prepared for this provider the verdict at the end of Pairing already says
		// this; a universal one has no verdict per provider, so each section says its own.
		if (bUniversal)
		{
			AddStep(Section, LOCTEXT("StepSuits", "Ready"),
				bSuits
					? FText::Format(LOCTEXT("SuitsFmt", "yes - {0} can generate for this character"), FText::FromString(Provider->GetDisplayName()))
					: FText::FromString(Why),
				bSuits ? MotionCharacterUI::GoodColour : MotionCharacterUI::WarnColour);
		}

		if (!Route.IsEmpty())
		{
			AddStep(Section, LOCTEXT("StepRoute", "Clips are"), FText::FromString(Route), MotionCharacterUI::QuietColour);
		}

		// The fix the core can make for any provider: a provider rig with no retargeter imports
		// directly once the rig is forgotten. The same button the definition window and Get Started have.
		if (!bSuits && Blocker == EMotionBlocker::RetargetIncomplete && !Char->ProviderMesh.IsNull() && Char->Retargeter.IsNull())
		{
			TWeakPtr<FMotionCharacterDetails> Weak = SharedThis(this);
			AddStep(Section, FText::GetEmpty(),
				LOCTEXT("DirectFixDetail", "Or import straight onto the target skeleton, which needs no retargeter."),
				MotionCharacterUI::QuietColour,
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("ImportDirectlyBtn", "Import directly"))
				.IsEnabled_Lambda([this]() { return !IsBusy(); })
				.OnClicked_Lambda([Weak, CharPath]()
				{
					if (UMotionForgeSubsystem* F = UMotionForgeSubsystem::Get())
					{
						FString Said;
						const bool bOk = F->ClearCharacterProviderRig(CharPath, Said);
						MotionCharacterUI::Tell(Said, bOk);
						if (TSharedPtr<FMotionCharacterDetails> Self = Weak.Pin())
						{
							Self->Rebuild();
						}
					}
					return FReply::Handled();
				}));
		}

		for (const FMotionCharacterSetupAction& Action : Actions)
		{
			AddAction(Section, Action);
		}
	}
}

void FMotionCharacterDetails::AddAction(IDetailCategoryBuilder& Section, const FMotionCharacterSetupAction& Action)
{
	TWeakPtr<FMotionCharacterDetails> Weak = SharedThis(this);
	const FMotionCharacterSetupAction Copy = Action;
	const bool bPrimary = Action.bRequired && !Action.bDone;

	AddStep(Section, Action.Label,
		Action.Status.IsEmpty() ? Action.Tooltip : Action.Status,
		Action.bDone ? MotionCharacterUI::GoodColour : (Action.bRequired ? MotionCharacterUI::WarnColour : MotionCharacterUI::QuietColour),
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
		.Text(Action.Label)
		.ToolTipText(Action.Tooltip)
		.IsEnabled_Lambda([this]() { return !IsBusy(); })
		.OnClicked_Lambda([Weak, Copy]()
		{
			if (TSharedPtr<FMotionCharacterDetails> Self = Weak.Pin())
			{
				Self->RunAction(Copy);
			}
			return FReply::Handled();
		}));

	// The provider's own options for the step - upload options, say - folded under it.
	if (Action.Options.IsValid())
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));

		FDetailsViewArgs Args;
		Args.bAllowSearch = false;
		Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		Args.bHideSelectionTip = true;

		FStructureDetailsViewArgs StructArgs;
		TSharedRef<IStructureDetailsView> Form = PropertyModule.CreateStructureDetailView(Args, StructArgs, Action.Options);
		OptionForms.Add(Form);

		Section.AddCustomRow(FText::Format(LOCTEXT("OptionsRowFmt", "{0} options"), Action.Label))
		.WholeRowContent()
		[
			SNew(SBox)
			.Padding(FMargin(20.f, 0.f, 0.f, 0.f))
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.AreaTitle(FText::Format(LOCTEXT("ActionOptionsFmt", "{0}: options"), Action.Label))
				.BodyContent()
				[
					Form->GetWidget().ToSharedRef()
				]
			]
		];
	}
}

void FMotionCharacterDetails::RunAction(const FMotionCharacterSetupAction& Action)
{
	if (!Action.Run || bBusy)
	{
		return;
	}

	if (!Action.Confirmation.IsEmpty()
		&& FMessageDialog::Open(EAppMsgType::OkCancel, Action.Confirmation, Action.Label) != EAppReturnType::Ok)
	{
		return;
	}

	bBusy = true;
	MotionCharacterUI::Tell(FString::Printf(TEXT("%s..."), *Action.Label.ToString()), true);

	TWeakPtr<FMotionCharacterDetails> Weak = SharedThis(this);
	Action.Run([Weak](bool bOk, const FString& Said)
	{
		if (TSharedPtr<FMotionCharacterDetails> Self = Weak.Pin())
		{
			Self->bBusy = false;
			MotionCharacterUI::Tell(Said, bOk);
			Self->Rebuild();
		}
	});
}

#undef LOCTEXT_NAMESPACE
