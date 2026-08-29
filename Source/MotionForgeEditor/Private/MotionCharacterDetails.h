// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "MotionForgeTypes.h"

class IDetailCategoryBuilder;
class UMotionCharacter;

/**
 * The pairing, as a sequence rather than as ten fields.
 *
 * A Motion Character is a five-step ritual wearing the clothes of a property list: export the mesh,
 * upload it, paste back the id, import the provider's own copy of the rig, author a retargeter
 * between them. Four of those must happen in order and none of them is implied by anything on the
 * page - the asset showed ten equal fields and no hint that filling in the fourth before the second
 * is meaningless.
 *
 * So this puts a checklist above them: what is done, what is next, and the button for the step you
 * are actually on.
 *
 * **Which steps exist depends on the provider**, and that is the whole reason this could not be a
 * static list. A service that retargets on its own hardware needs a character uploaded and an id
 * pasted back; a model that generates on a fixed rig has neither, and showing those steps to
 * somebody using Kimodo would be inventing work. The provider is asked, never assumed.
 */
class FMotionCharacterDetails : public IDetailCustomization
{
public:

	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:

	/** One line of the checklist: a dot, what it is, what it says, and sometimes a button. */
	void AddStep(
		IDetailCategoryBuilder& Category,
		const FText& Label,
		const FText& Value,
		const FLinearColor& Colour,
		TSharedPtr<SWidget> Action = nullptr);

	FReply OnUpload();
	FReply OnImportProviderMesh();

	bool IsBusy() const { return bBusy; }

	UMotionCharacter* Character() const { return Customised.Get(); }

	/** Re-run the layout, so the checklist moves on when a step completes. */
	void Rebuild();

	TWeakObjectPtr<UMotionCharacter> Customised;
	IDetailLayoutBuilder* Layout = nullptr;

	bool bBusy = false;
};
