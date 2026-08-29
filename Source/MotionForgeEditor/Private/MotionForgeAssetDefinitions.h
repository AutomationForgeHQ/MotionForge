// Copyright Blackcode SA. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"

#include "MotionForgeAssetDefinitions.generated.h"

/**
 * Where a Motion Definition lives in the Content Browser, and what opens it.
 *
 * The category matters more than it looks: a project that installs the whole family gets a dozen
 * asset types, and dropping them into Miscellaneous alongside everything else is how somebody fails
 * to find the thing they made this morning.
 */
UCLASS()
class UMotionDefAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;

	/** Opens the definition window rather than a details panel. */
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};

/**
 * A Motion Character keeps the default editor - it is ten fields with no pipeline state, so a
 * details panel is the right surface and a window would be ceremony.
 *
 * It is here for the category and the colour, so it sits beside the definitions it serves.
 */
UCLASS()
class UMotionCharacterAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:

	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
};
