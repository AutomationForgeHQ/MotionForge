// The Motion Library tab: Get Started for a project with nothing to show yet, the library once it has.
//
// One tab rather than two because they are one journey - a first motion is made on one page and
// found on the other - and a person who has done it once should land on the list, not the lesson.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SWidgetSwitcher;

enum class EMotionHomePage : uint8
{
	GetStarted,
	Library
};

class SMotionHome : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionHome) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void ShowPage(EMotionHomePage Page);

	/**
	 * Open the tab on a page. The tab keeps whichever page it was on otherwise, so this is how a
	 * menu entry or a toolbar button asks for a particular one.
	 */
	static void Open(EMotionHomePage Page);

	/**
	 * The page a person should land on: Get Started until any definition has produced an animation,
	 * or while no provider can work at all.
	 */
	static EMotionHomePage DefaultPage();

private:

	EMotionHomePage Page = EMotionHomePage::Library;
	TSharedPtr<SWidgetSwitcher> Switcher;

	/** The one open instance, so Open can switch its page. The tab is a nomad tab: there is only ever one. */
	static TWeakPtr<SMotionHome> Instance;
};
