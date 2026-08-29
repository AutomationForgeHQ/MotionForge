// Copyright Blackcode SA. All rights reserved.

#include "MotionCharacterDetails.h"

#include "MotionCharacter.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
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
		const bool bPaired = !Char->ProviderCharacterId.IsEmpty();

		TSharedPtr<SWidget> UploadButton;
		if (!bPaired)
		{
			UploadButton = SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(FText::Format(LOCTEXT("UploadFmt", "Upload to {0}"),
					FText::FromString(Caps.DisplayName)))
				.ToolTipText(LOCTEXT("UploadTip",
					"Exports the preview mesh and sends it, then writes the id it comes back with "
					"into this asset.\n\n"
					"Not repeatable by accident: a character that already has an id is refused, "
					"because repointing it would orphan every take generated against the old one."))
				.IsEnabled_Lambda([this, bHasPreview]() { return !IsBusy() && bHasPreview; })
				.OnClicked(this, &FMotionCharacterDetails::OnUpload);
		}

		AddStep(Category, LOCTEXT("StepPaired", "Paired"),
			bPaired
				? FText::FromString(Char->ProviderCharacterId)
				: LOCTEXT("NotPaired", "not uploaded yet"),
			bPaired ? MotionCharacterUI::GoodColour : MotionCharacterUI::WarnColour,
			UploadButton);
	}

	// 4. Which of the two pipelines this character uses.
	//
	// Not a step that can be missing, which is what the earlier version got wrong. The import path
	// reads `bRetargeting = ProviderMesh != nullptr`: an empty Provider's rig is not an unfinished
	// setup, it is the *direct* pipeline, chosen. Calling it "not set" in amber told somebody with
	// a working character that they had work left.
	const bool bRetargets = !Char->ProviderMesh.IsNull();

	TSharedPtr<SWidget> ImportButton;
	if (!bRetargets && Caps.bSupportsCharacterUpload && !Char->ProviderCharacterId.IsEmpty())
	{
		ImportButton = SNew(SButton)
			.Text(FText::Format(LOCTEXT("ImportRigFmt", "Import rig from {0}"),
				FText::FromString(Caps.DisplayName)))
			.ToolTipText(LOCTEXT("ImportRigTip",
				"Fetches the character back as the provider actually stores it, and switches this "
				"character to the retargeted pipeline.\n\n"
				"Providers normalise a rig on ingest, so the file we sent is not the skeleton their "
				"animation fits. Importing their copy removes any guessing about stripped bones."))
			.IsEnabled_Lambda([this]() { return !IsBusy(); })
			.OnClicked(this, &FMotionCharacterDetails::OnImportProviderMesh);
	}

	AddStep(Category, LOCTEXT("StepPipeline", "Pipeline"),
		bRetargets
			? FText::Format(LOCTEXT("PipelineRetargetFmt", "retargeted - clips arrive on {0}"),
				FText::FromString(Char->ProviderMesh.GetAssetName()))
			: FText::Format(LOCTEXT("PipelineDirectFmt", "direct - clips import straight onto {0}"),
				Char->TargetSkeleton.IsNull()
					? LOCTEXT("TheSkeleton", "the target skeleton")
					: FText::FromString(Char->TargetSkeleton.GetAssetName())),
		MotionCharacterUI::GoodColour,
		ImportButton);

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

	// The verdict, in the pipeline's own words rather than this page's opinion of them.
	FString Reason;
	const bool bUsable = bUniversal
		? Char->IsUsable(Reason)
		: Char->IsUsableForProvider(Caps.bSupportsCharacterUpload, Reason);

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

FReply FMotionCharacterDetails::OnUpload()
{
	UMotionCharacter* Char = Character();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	if (!Char || !Subsystem || bBusy)
	{
		return FReply::Handled();
	}

	bBusy = true;
	MotionCharacterUI::Tell(TEXT("Exporting and uploading the character. This takes a moment."), true);

	TWeakPtr<FMotionCharacterDetails> Weak = SharedThis(this);

	Subsystem->UploadCharacter(Char->GetPathName(), FMotionCharacterUploadOptions(), /*bForce*/ false,
		[Weak](bool bSuccess, const FMotionCharacterUpload& Result, const FString& Error)
		{
			if (TSharedPtr<FMotionCharacterDetails> Self = Weak.Pin())
			{
				Self->bBusy = false;

				MotionCharacterUI::Tell(bSuccess
					? FString::Printf(TEXT("Paired. The provider's id is %s."), *Result.ProviderCharacterId)
					: Error, bSuccess);

				Self->Rebuild();
			}
		});

	return FReply::Handled();
}

FReply FMotionCharacterDetails::OnImportProviderMesh()
{
	UMotionCharacter* Char = Character();
	UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get();

	if (!Char || !Subsystem || bBusy)
	{
		return FReply::Handled();
	}

	bBusy = true;
	MotionCharacterUI::Tell(TEXT("Fetching the character back from the provider."), true);

	TWeakPtr<FMotionCharacterDetails> Weak = SharedThis(this);

	Subsystem->ImportProviderCharacter(Char->GetPathName(),
		[Weak](bool bSuccess, const FString& MeshPath, const FString& Error)
		{
			if (TSharedPtr<FMotionCharacterDetails> Self = Weak.Pin())
			{
				Self->bBusy = false;

				MotionCharacterUI::Tell(bSuccess
					? FString::Printf(TEXT("Imported %s. Author a retargeter from it to the preview "
										   "mesh next."), *MeshPath)
					: Error, bSuccess);

				Self->Rebuild();
			}
		});

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
