#include "SMotionSection.h"

#include "MotionForgeEditorStyle.h"

#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SOverlay.h"

namespace MotionSectionPrivate
{
	FLinearColor BadgeColour(EMotionStepState State)
	{
		switch (State)
		{
		case EMotionStepState::Done:      return MotionForgeStyle::Good();
		case EMotionStepState::Current:   return MotionForgeStyle::Info();
		case EMotionStepState::Attention: return MotionForgeStyle::Warn();
		default:                          return FLinearColor(0.32f, 0.32f, 0.34f);
		}
	}
}

void SMotionSection::Construct(const FArguments& InArgs)
{
	const TAttribute<EMotionStepState> State = InArgs._State;
	const TAttribute<FText> Summary = InArgs._Summary;
	const TAttribute<FText> Subtitle = InArgs._Subtitle;

	TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);

	// The badge: the step's number, or a tick once it is done.
	if (InArgs._Number > 0)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
		[
			SNew(SBox)
			.WidthOverride(22.f)
			.HeightOverride(22.f)
			[
				SNew(SBorder)
				.BorderImage(MotionForgeStyle::BadgeBrush())
				.BorderBackgroundColor_Lambda([State]() { return FSlateColor(MotionSectionPrivate::BadgeColour(State.Get())); })
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(0.f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						.Text(FText::AsNumber(InArgs._Number))
						.ColorAndOpacity(FLinearColor::White)
						.Visibility_Lambda([State]() { return State.Get() == EMotionStepState::Done ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Check"))
						.ColorAndOpacity(FLinearColor::White)
						.DesiredSizeOverride(FVector2D(14.f, 14.f))
						.Visibility_Lambda([State]() { return State.Get() == EMotionStepState::Done ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					]
				]
			]
		];
	}

	Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
		.Text(InArgs._Title)
	];

	// What the step holds, so a closed step still says it: "Kimodo (local) - ready - free".
	Header->AddSlot().FillWidth(1.f).VAlign(VAlign_Center).Padding(14.f, 0.f, 8.f, 0.f)
	[
		SNew(STextBlock)
		.Text(Summary)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	];

	if (InArgs._HeaderRight.Widget != SNullWidget::NullWidget)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			InArgs._HeaderRight.Widget
		];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(MotionForgeStyle::SectionBrush())
		.Padding(FMargin(1.f))
		[
			SAssignNew(Area, SExpandableArea)
			.InitiallyCollapsed(!InArgs._InitiallyExpanded)
			.AllowAnimatedTransition(false)
			.BorderImage(FAppStyle::GetNoBrush())
			.BodyBorderImage(FAppStyle::GetNoBrush())
			.HeaderPadding(FMargin(10.f, 9.f))
			.Padding(FMargin(16.f, 0.f, 14.f, 14.f))
			.OnAreaExpansionChanged(InArgs._OnExpansionChanged)
			.HeaderContent()
			[
				Header
			]
			.BodyContent()
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
				[
					SNew(STextBlock)
					.Text(Subtitle)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Visibility_Lambda([Subtitle]() { return Subtitle.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(Body, SBox)
					[
						InArgs._Content.Widget
					]
				]
			]
		]
	];
}

void SMotionSection::SetContent(TSharedRef<SWidget> Content)
{
	if (Body.IsValid())
	{
		Body->SetContent(Content);
	}
}

void SMotionSection::SetExpanded(bool bExpanded)
{
	if (Area.IsValid())
	{
		Area->SetExpanded(bExpanded);
	}
}

bool SMotionSection::IsExpanded() const
{
	return Area.IsValid() && Area->IsExpanded();
}
