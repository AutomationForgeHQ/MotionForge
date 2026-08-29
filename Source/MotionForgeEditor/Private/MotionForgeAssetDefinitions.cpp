// Copyright Blackcode SA. All rights reserved.

#include "MotionForgeAssetDefinitions.h"

#include "MotionCharacter.h"
#include "MotionDef.h"
#include "MotionDefEditorToolkit.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

namespace
{
	/** One category for the whole family, with a submenu per set, so the types sit together. */
	const TArray<FAssetCategoryPath>& ForgeCategories()
	{
		static const TArray<FAssetCategoryPath> Categories
		{
			FAssetCategoryPath(
				FAssetCategoryPath(LOCTEXT("AutomationForge", "Automation Forge")),
				LOCTEXT("MotionForge", "MotionForge"))
		};
		return Categories;
	}
}

// -------------------------------------------------------------------------------------------------

FText UMotionDefAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("MotionDef", "Motion Definition");
}

FLinearColor UMotionDefAssetDefinition::GetAssetColor() const
{
	return FLinearColor(0.36f, 0.52f, 0.86f);
}

TSoftClassPtr<UObject> UMotionDefAssetDefinition::GetAssetClass() const
{
	return UMotionDef::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UMotionDefAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

EAssetCommandResult UMotionDefAssetDefinition::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UMotionDef* Def : OpenArgs.LoadObjects<UMotionDef>())
	{
		const TSharedRef<FMotionDefEditorToolkit> Toolkit = MakeShared<FMotionDefEditorToolkit>();
		Toolkit->Initialise(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, Def);
	}

	return EAssetCommandResult::Handled;
}

// -------------------------------------------------------------------------------------------------

FText UMotionCharacterAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("MotionCharacter", "Motion Character");
}

FLinearColor UMotionCharacterAssetDefinition::GetAssetColor() const
{
	return FLinearColor(0.30f, 0.68f, 0.62f);
}

TSoftClassPtr<UObject> UMotionCharacterAssetDefinition::GetAssetClass() const
{
	return UMotionCharacter::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UMotionCharacterAssetDefinition::GetAssetCategories() const
{
	return ForgeCategories();
}

#undef LOCTEXT_NAMESPACE
