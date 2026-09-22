// The family's visual vocabulary, for every MotionForge window. PANEL_RULES 21-22 and the house style
// of 2026-09-02: a bordered card with a bold header per group, a 112 px label column for facts, colour
// only where it carries a state, safe buttons left and the spending one right.

#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace MotionForgeStyle
{
	inline const FLinearColor Good()  { return FLinearColor(0.30f, 0.78f, 0.45f); }
	inline const FLinearColor Warn()  { return FLinearColor(0.95f, 0.65f, 0.20f); }
	inline const FLinearColor Bad()   { return FLinearColor(0.90f, 0.36f, 0.36f); }
	inline const FLinearColor Quiet() { return FLinearColor(0.55f, 0.55f, 0.58f); }
	inline const FLinearColor Info()  { return FLinearColor(0.36f, 0.62f, 0.95f); }

	/** The width of the label column in a list of facts. One grid, so facts line up. */
	inline constexpr float LabelWidth = 112.f;

	/**
	 * The box every section sits in: lifted off the panel, with an edge.
	 *
	 * `ToolPanel.GroupBorder` is within a shade of the panel behind it in the current editor theme, so
	 * a page built from it read as one long column with no steps in it. Theme colours rather than
	 * fixed ones, so it follows a changed theme.
	 */
	inline const FSlateBrush* SectionBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FStyleColors::Header, 6.f, FStyleColors::Hover, 1.f);
		return &Brush;
	}

	/**
	 * One of several things to choose between, inside a section: sunk into it, with an edge that says
	 * which is chosen - the accent colour for the chosen one, a quiet one for the rest.
	 */
	inline const FSlateBrush* ChoiceBrush(bool bChosen)
	{
		static const FSlateRoundedBoxBrush Chosen(FStyleColors::Recessed, 5.f, FStyleColors::Primary, 1.5f);
		static const FSlateRoundedBoxBrush Other(FStyleColors::Recessed, 5.f, FStyleColors::Hover, 1.f);
		return bChosen ? &Chosen : &Other;
	}

	/** A round badge, tinted by whoever draws it. */
	inline const FSlateBrush* BadgeBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 11.f);
		return &Brush;
	}

	/** A group: boxed, padded, with a bold header and an optional quiet line under it. */
	inline TSharedRef<SWidget> Card(const FText& Title, TSharedRef<SWidget> Content, const FText& Subtitle = FText::GetEmpty(), TSharedPtr<SWidget> HeaderRight = nullptr)
	{
		TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(Title)
			];

		if (HeaderRight.IsValid())
		{
			Header->AddSlot().AutoWidth().VAlign(VAlign_Center)[ HeaderRight.ToSharedRef() ];
		}

		TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Header ];

		if (!Subtitle.IsEmpty())
		{
			Body->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Subtitle)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			];
		}

		Body->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)[ Content ];

		return SNew(SBorder)
			.BorderImage(SectionBrush())
			.Padding(FMargin(14.f, 12.f))
			[
				Body
			];
	}

	/** A filled dot in a state colour. Colour works by being rare. */
	inline TSharedRef<SWidget> Dot(const FLinearColor& Colour)
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

	/** One label-and-value row on the shared grid. */
	inline TSharedRef<SWidget> Fact(const FText& Label, TSharedRef<SWidget> Value)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 1.f, 8.f, 1.f)
			[
				SNew(SBox)
				.WidthOverride(LabelWidth)
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top).Padding(0.f, 1.f)
			[
				Value
			];
	}

	inline TSharedRef<SWidget> Fact(const FText& Label, const FText& Value, const FLinearColor* Colour = nullptr)
	{
		return Fact(Label, SNew(STextBlock)
			.Text(Value)
			.ColorAndOpacity(Colour ? FSlateColor(*Colour) : FSlateColor::UseForeground())
			.AutoWrapText(true));
	}

	/** A sentence with a small icon, for a reason, a warning or a note. */
	inline TSharedRef<SWidget> Note(const FText& Text, const FLinearColor& Colour, const FName Icon = "Icons.Info")
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.f, 1.f, 6.f, 0.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(Icon))
				.ColorAndOpacity(Colour)
				.DesiredSizeOverride(FVector2D(14.f, 14.f))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Text(Text)
				.ColorAndOpacity(FSlateColor(Colour))
				.AutoWrapText(true)
			];
	}

	/** Money to two places with the symbol people expect. "about $3.5" reads like a typo. */
	inline FString Money(double Amount, const FString& Currency)
	{
		const FString Number = FString::Printf(TEXT("%.2f"), Amount);
		if (Currency == TEXT("USD")) { return TEXT("$") + Number; }
		if (Currency == TEXT("EUR")) { return FString(TEXT("EUR ")) + Number; }
		if (Currency == TEXT("GBP")) { return FString(TEXT("GBP ")) + Number; }
		return Number + TEXT(" ") + Currency;
	}

	/** A duration a person reads at a glance: "0:14", "3:05". */
	inline FString Clock(const FTimespan& Span)
	{
		const int32 Seconds = FMath::Max(0, FMath::FloorToInt(Span.GetTotalSeconds()));
		return Seconds >= 3600
			? FString::Printf(TEXT("%d:%02d:%02d"), Seconds / 3600, (Seconds / 60) % 60, Seconds % 60)
			: FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60);
	}

	inline FString Seconds(float Value)
	{
		return FMath::IsNearlyEqual(Value, FMath::RoundToFloat(Value), 0.01f)
			? FString::Printf(TEXT("%d s"), FMath::RoundToInt(Value))
			: FString::Printf(TEXT("%.1f s"), Value);
	}

	/**
	 * Prompts that make a good first motion on any provider: one person, one or two actions, an end.
	 * One list, so Get Started and a definition's window offer the same.
	 */
	inline const TArray<FString>& ExamplePrompts()
	{
		static const TArray<FString> Examples =
		{
			TEXT("A person walks forward, stops, and waves with the right hand."),
			TEXT("A person kneels down on one knee and inspects something on the floor, then stands up."),
			TEXT("A tired person sits down on a chair, rubs their neck, and leans back."),
		};
		return Examples;
	}

	/** Open a tab by id, when something registered it. The family's panels are optional to each other. */
	inline bool TryOpenTab(const FName TabId)
	{
		if (FGlobalTabmanager::Get()->HasTabSpawner(TabId))
		{
			FGlobalTabmanager::Get()->TryInvokeTab(TabId);
			return true;
		}
		return false;
	}
}
