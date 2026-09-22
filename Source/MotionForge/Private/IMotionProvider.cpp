// Defaults for what a provider declares. A provider that overrides nothing is described honestly.

#include "IMotionProvider.h"

#include "MotionCharacter.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

TArray<FMotionModelInfo> IMotionProvider::GetModels() const
{
	FMotionModelInfo Model;
	Model.Id = GetDefaultModelId();
	Model.bDefault = true;

	int32 Min = 1;
	int32 Max = 10;
	GetLengthRange(Model.Id, Min, Max);
	Model.MinSeconds = Min;
	Model.MaxSeconds = Max;

	return { Model };
}

FMotionPromptSplitting IMotionProvider::GetPromptSplitting() const
{
	// One generation of the whole prompt, the whole length. Beat durations mean nothing here.
	FMotionPromptSplitting Splitting;
	Splitting.bSplitsAtFullStops = false;

	int32 Min = 1;
	int32 Max = 10;
	GetLengthRange(GetDefaultModelId(), Min, Max);
	Splitting.MaxBeatSeconds = Max;
	Splitting.MaxTotalSeconds = Max;
	return Splitting;
}

FMotionBilling IMotionProvider::GetBilling() const
{
	const FMotionProviderCaps Caps = GetCaps();

	FMotionBilling Billing;

	if (!Caps.bIsMetered)
	{
		Billing.Unit = EMotionBillingUnit::Free;
		Billing.Summary = Caps.bIsLocal ? TEXT("free, runs on this machine") : TEXT("free");
		Billing.bFetchIsFree = true;
		return Billing;
	}

	// Metered with nothing said about how. Claiming a unit would be a guess with a number attached, so
	// the line says only that it bills - and the confirmation still asks.
	Billing.Unit = EMotionBillingUnit::PerGeneratedSecond;
	Billing.Summary = TEXT("billed by the provider; the rate is not known here");
	Billing.bFetchIsFree = false;
	return Billing;
}

bool IMotionProvider::CheckCharacter(const UMotionCharacter* Character, FString& OutReason, EMotionBlocker& OutBlocker) const
{
	OutBlocker = EMotionBlocker::None;

	if (Character == nullptr)
	{
		OutReason = TEXT("No Motion Character.");
		OutBlocker = EMotionBlocker::NoCharacter;
		return false;
	}

	const FMotionProviderCaps Caps = GetCaps();

	if (!Character->IsUsableForProvider(Caps.bSupportsCharacterUpload, OutReason))
	{
		OutBlocker = EMotionBlocker::CharacterUnusable;
		return false;
	}

	// A set Provider Mesh means clips land on that rig and are retargeted across. Without a retargeter
	// the import would fail after the generation was paid for, so it is refused here instead.
	if (!Character->ProviderMesh.IsNull())
	{
		if (Character->Retargeter.IsNull())
		{
			OutReason = FString::Printf(
				TEXT("'%s' has a provider rig but no Retargeter, so a clip could not be moved onto the "
					 "game's skeleton. Import directly instead, or set a Retargeter on the character."),
				*Character->GetDisplayName());
			OutBlocker = EMotionBlocker::RetargetIncomplete;
			return false;
		}

		if (Character->PreviewMesh.IsNull())
		{
			OutReason = FString::Printf(
				TEXT("'%s' retargets through a provider rig but has no Preview Mesh to retarget onto."),
				*Character->GetDisplayName());
			OutBlocker = EMotionBlocker::RetargetIncomplete;
			return false;
		}
	}

	OutReason.Reset();
	return true;
}

FString IMotionProvider::DescribeCharacterRoute(const UMotionCharacter* Character) const
{
	if (Character == nullptr)
	{
		return FString();
	}

	// Asset names from the soft paths, so describing a route never loads a mesh.
	if (!Character->ProviderMesh.IsNull())
	{
		// What happens, not what was meant to: with no retargeter a clip stops on the provider's rig.
		return Character->Retargeter.IsNull()
			? FString::Printf(TEXT("built on %s and stopped there - no retargeter moves them onto %s"),
				*Character->ProviderMesh.ToSoftObjectPath().GetAssetName(),
				*Character->PreviewMesh.ToSoftObjectPath().GetAssetName())
			: FString::Printf(TEXT("built on %s, then retargeted onto %s"),
				*Character->ProviderMesh.ToSoftObjectPath().GetAssetName(),
				*Character->PreviewMesh.ToSoftObjectPath().GetAssetName());
	}

	return FString::Printf(TEXT("direct onto %s"), *Character->TargetSkeleton.ToSoftObjectPath().GetAssetName());
}
