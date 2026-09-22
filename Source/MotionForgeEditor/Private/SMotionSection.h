// A section of a MotionForge page: its own box, a header that opens and closes it, and - for a step
// in a sequence - a numbered badge that says where that step stands.
//
// Built on the editor's own expandable area, so it opens and closes the way a Details category does.
// The body is replaced in place while the header, and whether it is open, stay put: rebuilding the
// whole section on every change is what made pages collapse and redraw while a runner started.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SExpandableArea;

/** Where a step stands, drawn as its badge. */
enum class EMotionStepState : uint8
{
	/** Not a numbered step, or nothing to say. */
	None,
	/** Waits on an earlier step. */
	Todo,
	/** The step to do now. */
	Current,
	/** Needs a person before anything else can happen. */
	Attention,
	Done
};

class SMotionSection : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionSection)
		: _Number(0)
		, _State(EMotionStepState::None)
		, _InitiallyExpanded(true)
	{}
		/** The step's number in its sequence. Zero draws no badge. */
		SLATE_ARGUMENT(int32, Number)
		SLATE_ATTRIBUTE(FText, Title)
		/** One line in the header, beside the title: what the step holds, readable while it is closed. */
		SLATE_ATTRIBUTE(FText, Summary)
		/** The quiet first line of the body. */
		SLATE_ATTRIBUTE(FText, Subtitle)
		SLATE_ATTRIBUTE(EMotionStepState, State)
		SLATE_ARGUMENT(bool, InitiallyExpanded)
		/** Controls at the right end of the header. */
		SLATE_NAMED_SLOT(FArguments, HeaderRight)
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_EVENT(FOnBooleanValueChanged, OnExpansionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Replace what the section holds, keeping its header and whether it is open. */
	void SetContent(TSharedRef<SWidget> Content);

	void SetExpanded(bool bExpanded);
	bool IsExpanded() const;

private:

	TSharedPtr<SExpandableArea> Area;
	TSharedPtr<SBox> Body;
};
