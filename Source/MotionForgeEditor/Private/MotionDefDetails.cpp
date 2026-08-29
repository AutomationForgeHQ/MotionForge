// Copyright Blackcode SA. All rights reserved.

#include "MotionDefDetails.h"

#include "MotionDef.h"
#include "MotionForgeSubsystem.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

TSharedRef<IDetailCustomization> FMotionDefDetails::MakeInstance()
{
	return MakeShared<FMotionDefDetails>();
}

void FMotionDefDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	PromptHandle   = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMotionDef, Prompt));
	ProviderHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMotionDef, ProviderId));

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	CustomisedDefinition = Objects.Num() == 1
		? Cast<UMotionDef>(Objects[0].Get())
		: nullptr;

	AddPromptCategory(DetailBuilder);
	AddProviderRow(DetailBuilder);
}

UMotionDef* FMotionDefDetails::Definition() const
{
	return CustomisedDefinition.Get();
}

// -------------------------------------------------------------------------------------------------

void FMotionDefDetails::AddPromptCategory(IDetailLayoutBuilder& DetailBuilder)
{
	if (!PromptHandle.IsValid())
	{
		return;
	}

	DetailBuilder.HideProperty(PromptHandle);

	// Its own category at the top, because the prompt is the work and everything else is a dial on it.
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
		TEXT("Prompt"), LOCTEXT("PromptCategory", "Prompt"), ECategoryPriority::Important);

	Category.AddCustomRow(LOCTEXT("PromptRow", "Prompt"))
	.WholeRowContent()
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(140.f)
			[
				SNew(SMultiLineEditableTextBox)
				.AutoWrapText(true)
				.HintText(LOCTEXT("PromptHint",
					"Describe beats, give each its own tempo, and say how the motion ends."))
				.Text(this, &FMotionDefDetails::PromptText)
				.OnTextCommitted(this, &FMotionDefDetails::OnPromptCommitted)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PromptAdvice",
				"A model with a minimum clip length spends the whole duration whether or not you say "
				"how, so an underspecified prompt comes back padded and lifeless."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]
	];
}

FText FMotionDefDetails::PromptText() const
{
	const UMotionDef* Def = Definition();
	return Def ? FText::FromString(Def->Prompt) : FText::GetEmpty();
}

void FMotionDefDetails::OnPromptCommitted(const FText& NewText, ETextCommit::Type)
{
	const UMotionDef* Def = Definition();

	// Committing an unchanged box would push a transaction onto the undo stack every time focus
	// leaves the field, which is most times somebody clicks anything.
	if (!Def || !PromptHandle.IsValid() || Def->Prompt == NewText.ToString())
	{
		return;
	}

	PromptHandle->SetValue(NewText.ToString());
}

// -------------------------------------------------------------------------------------------------

void FMotionDefDetails::AddProviderRow(IDetailLayoutBuilder& DetailBuilder)
{
	if (!ProviderHandle.IsValid())
	{
		return;
	}

	DetailBuilder.HideProperty(ProviderHandle);

	// What is actually registered, rather than what someone remembers the id being.
	ProviderChoices.Reset();
	ProviderChoices.Add(MakeShared<FMotionProviderChoice>(
		FMotionProviderChoice{ NAME_None, LOCTEXT("ProjectDefault", "Project default").ToString() }));

	if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
	{
		for (const FName Id : Subsystem->GetProviderIds())
		{
			const FMotionProviderCaps Caps = Subsystem->GetProviderCaps(Id);

			FString Label = Caps.DisplayName.IsEmpty() ? Id.ToString() : Caps.DisplayName;
			if (Caps.bIsLocal)
			{
				Label += TEXT("  (on this machine)");
			}
			else if (Caps.bIsMetered)
			{
				Label += TEXT("  (billed)");
			}

			ProviderChoices.Add(MakeShared<FMotionProviderChoice>(FMotionProviderChoice{ Id, Label }));
		}
	}

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Motion"));

	Category.AddCustomRow(LOCTEXT("ProviderRow", "Provider"))
	.NameContent()
	[
		ProviderHandle->CreatePropertyNameWidget(
			LOCTEXT("ProviderName", "Provider"),
			LOCTEXT("ProviderTip",
				"Which service or local runner generates this motion. Project default follows the "
				"one set in Editor Preferences, which is what most definitions should use."))
	]
	.ValueContent()
	.MinDesiredWidth(240.f)
	[
		SNew(SComboBox<TSharedPtr<FMotionProviderChoice>>)
		.OptionsSource(&ProviderChoices)
		.OnGenerateWidget_Lambda([](TSharedPtr<FMotionProviderChoice> Choice)
		{
			return SNew(STextBlock).Text(FText::FromString(Choice->Label));
		})
		// Bound to a member rather than a captured handle: the handle is reassigned whenever the
		// details view rebuilds its layout, and a captured copy would then write nowhere.
		.OnSelectionChanged(this, &FMotionDefDetails::OnProviderChosen)
		[
			SNew(STextBlock).Text(this, &FMotionDefDetails::CurrentProviderLabel)
		]
	];
}

void FMotionDefDetails::OnProviderChosen(TSharedPtr<FMotionProviderChoice> Choice, ESelectInfo::Type Info)
{
	// Direct means the list set itself from the current value, which is not somebody choosing.
	if (Choice.IsValid() && Info != ESelectInfo::Direct && ProviderHandle.IsValid())
	{
		ProviderHandle->SetValue(Choice->Id);
	}
}

FName FMotionDefDetails::CurrentProviderId() const
{
	const UMotionDef* Def = Definition();
	return Def ? Def->ProviderId : NAME_None;
}

FText FMotionDefDetails::CurrentProviderLabel() const
{
	const FName Current = CurrentProviderId();

	for (const TSharedPtr<FMotionProviderChoice>& Choice : ProviderChoices)
	{
		if (Choice.IsValid() && Choice->Id == Current)
		{
			return FText::FromString(Choice->Label);
		}
	}

	// A definition pointing at a provider that is no longer installed. Say so rather than showing
	// an empty box, which reads as "default" and is the opposite of the truth.
	return FText::Format(
		LOCTEXT("ProviderMissingFmt", "{0} - not installed"), FText::FromName(Current));
}

#undef LOCTEXT_NAMESPACE
