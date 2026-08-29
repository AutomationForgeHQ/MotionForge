#include "MotionCharacter.h"

FString UMotionCharacter::GetDisplayName() const
{
	return DisplayName.IsEmpty() ? GetName() : DisplayName;
}

bool UMotionCharacter::IsUsable(FString& OutReason) const
{
	// Deliberately not checking ProviderCharacterId. Whether one is needed is a fact about the
	// provider, not about the character - a generator with its own fixed rig has nothing to pair
	// with and no id to paste. See IsUsableForProvider, which is what the pipeline actually calls.
	if (TargetSkeleton.IsNull())
	{
		OutReason = TEXT("No TargetSkeleton - imported animations would have nothing to bind to.");
		return false;
	}

	OutReason.Reset();
	return true;
}

bool UMotionCharacter::IsUsableForProvider(bool bProviderNeedsCharacterId, FString& OutReason) const
{
	if (!IsUsable(OutReason))
	{
		return false;
	}

	if (bProviderNeedsCharacterId && ProviderCharacterId.IsEmpty())
	{
		OutReason = TEXT(
			"No ProviderCharacterId - upload the character to the provider and paste its id here.");
		return false;
	}

	OutReason.Reset();
	return true;
}
