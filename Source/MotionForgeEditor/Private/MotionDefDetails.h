// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Types/SlateEnums.h"

class IPropertyHandle;

/** One entry in the provider picker: what the user reads, and what gets stored. */
struct FMotionProviderChoice
{
	FName Id;
	FString Label;
};

/**
 * The authoring half of the definition window.
 *
 * Two changes, both of which exist because the default panel gave a wrong impression rather than
 * because it was ugly:
 *
 *   The prompt is the work, and it was a one-line-tall box two thirds of the way down a list. It is
 *   now the first thing in the window and large enough to write beats in.
 *
 *   The provider was a raw FName typed by hand, so the only way to discover what was installed was
 *   to guess and read the log afterwards. It is a list of what is actually registered.
 */
class FMotionDefDetails : public IDetailCustomization
{
public:

	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:

	void AddPromptCategory(IDetailLayoutBuilder& DetailBuilder);
	void AddProviderRow(IDetailLayoutBuilder& DetailBuilder);

	/**
	 * Read from the object, write through the handle.
	 *
	 * A handle from the layout builder dies with the layout, and a details view rebuilds itself for
	 * reasons a customization does not see - which showed up as a prompt box that was correct when
	 * the window opened and empty afterwards. Reading the object cannot go stale; writing through the
	 * handle is still what puts the change on the undo stack and marks the package.
	 */
	FText PromptText() const;
	void OnPromptCommitted(const FText& NewText, ETextCommit::Type Commit);

	void OnProviderChosen(TSharedPtr<FMotionProviderChoice> Choice, ESelectInfo::Type Info);
	FText CurrentProviderLabel() const;
	FName CurrentProviderId() const;

	class UMotionDef* Definition() const;

	TSharedPtr<IPropertyHandle> PromptHandle;
	TSharedPtr<IPropertyHandle> ProviderHandle;

	TWeakObjectPtr<UMotionDef> CustomisedDefinition;

	TArray<TSharedPtr<FMotionProviderChoice>> ProviderChoices;
};
