// Copyright Blackcode SA. All rights reserved.
//
// What makes these types appear under right-click > Automation Forge in the Content Browser.
//
// Without a factory a UDataAsset can still be created - through Miscellaneous > Data Asset, then
// picking the class out of a list of every data asset class in the project. That is the path a
// person finds after being told it exists, which is to say not at all. An asset definition decides
// what a double-click opens; a factory is what decides the thing can be made in the first place.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MotionForgeFactories.generated.h"

/**
 * Creates a Motion Definition, configured the way the pipeline creates one.
 *
 * The project defaults - character, provider, model - are applied here as well as in
 * CreateMotionDef, through the same call, so a definition made by hand is not subtly different from
 * one an agent authored.
 */
UCLASS()
class UMotionDefFactory : public UFactory
{
	GENERATED_BODY()

public:

	UMotionDefFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};

/** Creates a Motion Character - the pairing between a provider's rig and one of ours. */
UCLASS()
class UMotionCharacterFactory : public UFactory
{
	GENERATED_BODY()

public:

	UMotionCharacterFactory();

	virtual UObject* FactoryCreateNew(
		UClass* Class,
		UObject* InParent,
		FName Name,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
	virtual FString GetDefaultNewAssetName() const override;
};
