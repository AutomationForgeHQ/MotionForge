#include "SMotionHome.h"

#include "MotionForgeEditorModule.h"
#include "MotionForgeSubsystem.h"
#include "MotionDef.h"
#include "SMotionGetStarted.h"
#include "SMotionLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"

#define LOCTEXT_NAMESPACE "MotionForgeHome"

TWeakPtr<SMotionHome> SMotionHome::Instance;

void SMotionHome::Construct(const FArguments&)
{
	Page = DefaultPage();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0.f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(16.f, 10.f, 16.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SSegmentedControl<EMotionHomePage>)
					.Value_Lambda([this]() { return Page; })
					.OnValueChanged_Lambda([this](EMotionHomePage NewPage) { ShowPage(NewPage); })

					+ SSegmentedControl<EMotionHomePage>::Slot(EMotionHomePage::GetStarted)
					.Text(LOCTEXT("GetStarted", "Get started"))
					.ToolTip(LOCTEXT("GetStartedTip", "Set up a provider and a character, and make a first motion, step by step."))

					+ SSegmentedControl<EMotionHomePage>::Slot(EMotionHomePage::Library)
					.Text(LOCTEXT("Library", "Library"))
					.ToolTip(LOCTEXT("LibraryTip", "Every motion definition in the project, what state it is in, and what it produced."))
				]
			]

			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(Switcher, SWidgetSwitcher)
				.WidgetIndex_Lambda([this]() { return Page == EMotionHomePage::GetStarted ? 0 : 1; })

				+ SWidgetSwitcher::Slot()
				[
					SNew(SMotionGetStarted)
					.OnOpenLibrary_Lambda([this]() { ShowPage(EMotionHomePage::Library); })
				]

				+ SWidgetSwitcher::Slot()
				[
					SNew(SMotionLibrary)
				]
			]
		]
	];

	Instance = SharedThis(this);
}

void SMotionHome::ShowPage(EMotionHomePage NewPage)
{
	Page = NewPage;
}

void SMotionHome::Open(EMotionHomePage Page)
{
	FGlobalTabmanager::Get()->TryInvokeTab(FMotionForgeEditorModule::LibraryTabName);

	if (TSharedPtr<SMotionHome> Home = Instance.Pin())
	{
		Home->ShowPage(Page);
	}
}

EMotionHomePage SMotionHome::DefaultPage()
{
	// Read from the registry, not by loading every definition: a tab opening should not load a library.
	FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Definitions;
	Registry.Get().GetAssetsByClass(UMotionDef::StaticClass()->GetClassPathName(), Definitions);

	bool bAnyAnimation = false;
	for (const FAssetData& Asset : Definitions)
	{
		FString Clip;
		if (!Asset.GetTagValue(UMotionDef::TagClip, Clip))
		{
			// Saved before definitions carried registry tags. Read the one asset; the loop stops at the
			// first with a clip, and resaving them (Migrate Motion Definitions) ends this path.
			if (const UMotionDef* Def = Cast<UMotionDef>(Asset.GetAsset()))
			{
				Clip = Def->ImportedSequence.ToString();
			}
		}

		if (!Clip.IsEmpty() && Clip != TEXT("None"))
		{
			bAnyAnimation = true;
			break;
		}
	}

	// A project that has made motions before but has lost every provider since still needs setup.
	bool bAnyProviderWorks = false;
	if (UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get())
	{
		for (const FName Id : Forge->GetProviderIds())
		{
			const FMotionProviderCaps Caps = Forge->GetProviderCaps(Id);
			if (Caps.SetupHint.IsEmpty() || !Caps.PrepareLabel.IsEmpty())
			{
				bAnyProviderWorks = true;
				break;
			}
		}
	}

	return bAnyAnimation && bAnyProviderWorks ? EMotionHomePage::Library : EMotionHomePage::GetStarted;
}

#undef LOCTEXT_NAMESPACE
