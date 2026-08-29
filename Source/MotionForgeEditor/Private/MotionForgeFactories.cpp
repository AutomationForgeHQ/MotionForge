// Copyright Blackcode SA. All rights reserved.

#include "MotionForgeFactories.h"

#include "MotionCharacter.h"
#include "MotionDef.h"

#define LOCTEXT_NAMESPACE "MotionForgeEditor"

// -------------------------------------------------------------------------------------------------

UMotionDefFactory::UMotionDefFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UMotionDef::StaticClass();
}

UObject* UMotionDefFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	UMotionDef* Def = NewObject<UMotionDef>(InParent, Class, Name, Flags);

	// The same defaults CreateMotionDef applies, through the same call. A definition made by hand
	// that skipped this looked identical in the Content Browser and generated against nothing.
	if (Def)
	{
		Def->ApplyProjectDefaults();
	}

	return Def;
}

FText UMotionDefFactory::GetDisplayName() const
{
	return LOCTEXT("NewMotionDef", "Motion Definition");
}

FString UMotionDefFactory::GetDefaultNewAssetName() const
{
	// The prefix the rest of the library uses, so a new one sorts with its neighbours rather than
	// under N for NewDataAsset.
	return TEXT("MD_NewMotion");
}

// -------------------------------------------------------------------------------------------------

UMotionCharacterFactory::UMotionCharacterFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UMotionCharacter::StaticClass();
}

UObject* UMotionCharacterFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	return NewObject<UMotionCharacter>(InParent, Class, Name, Flags);
}

FText UMotionCharacterFactory::GetDisplayName() const
{
	return LOCTEXT("NewMotionCharacter", "Motion Character");
}

FString UMotionCharacterFactory::GetDefaultNewAssetName() const
{
	return TEXT("MC_NewCharacter");
}

#undef LOCTEXT_NAMESPACE
