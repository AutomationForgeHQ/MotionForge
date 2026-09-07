#include "MotionDef.h"

#include "MotionForge.h"
#include "MotionForgeSettings.h"

const FMotionCandidate* UMotionDef::FindSelectedCandidate() const
{
	return SelectedMotionId.IsEmpty() ? nullptr : FindCandidate(SelectedMotionId);
}

const FMotionCandidate* UMotionDef::FindCandidate(const FString& MotionId) const
{
	return Candidates.FindByPredicate(
		[&MotionId](const FMotionCandidate& Candidate) { return Candidate.MotionId == MotionId; });
}

FMotionCandidate* UMotionDef::FindCandidateMutable(const FString& MotionId)
{
	return Candidates.FindByPredicate(
		[&MotionId](const FMotionCandidate& Candidate) { return Candidate.MotionId == MotionId; });
}

bool UMotionDef::IsBusy() const
{
	return Status == EMotionDefStatus::Generating
		|| Status == EMotionDefStatus::Downloading
		|| Status == EMotionDefStatus::Processing;
}

int32 UMotionDef::CountUsableCandidates() const
{
	int32 Count = 0;
	for (const FMotionCandidate& Candidate : Candidates)
	{
		if (Candidate.Status == EMotionJobStatus::Finished && Candidate.IsValidCandidate())
		{
			++Count;
		}
	}
	return Count;
}

int32 UMotionDef::EstimateDownloadSeconds(bool bSelectedOnly) const
{
	// Providers meter the generated length, not whatever survives trimming - the trim happens here,
	// long after the money is spent. Estimating against Length keeps the number honest.
	if (bSelectedOnly)
	{
		const FMotionCandidate* Selected = FindSelectedCandidate();
		if (!Selected || Selected->bDownloaded)
		{
			return 0;
		}
		return Length;
	}

	int32 Seconds = 0;
	for (const FMotionCandidate& Candidate : Candidates)
	{
		if (Candidate.Status == EMotionJobStatus::Finished && Candidate.IsValidCandidate() && !Candidate.bDownloaded)
		{
			Seconds += Length;
		}
	}
	return Seconds;
}

void UMotionDef::ApplySpec(const FMotionDefSpec& Spec)
{
	// Only authoring fields - a spec must never be able to rewrite pipeline state and orphan the
	// candidates already paid for.
	if (!Spec.Prompt.IsEmpty())
	{
		Prompt = Spec.Prompt;
	}

	Length = Spec.Length > 0 ? Spec.Length : Length;
	Variants = Spec.Variants > 0 ? Spec.Variants : Variants;
	bRewritePrompt = Spec.bRewritePrompt;
	TrimWindow = Spec.TrimWindow;

	if (!Spec.ProviderId.IsNone())
	{
		ProviderId = Spec.ProviderId;
	}

	if (!Spec.ModelId.IsEmpty())
	{
		ModelId = Spec.ModelId;
	}

	// Assigned wholesale rather than merged field by field. Control is one coherent recipe, and a
	// half-applied one - this spec's seed with the last spec's constraints - is not reproducible,
	// which defeats the only reason the struct exists.
	if (!Spec.Control.IsDefault())
	{
		Control = Spec.Control;
	}

	if (!Spec.CharacterAssetPath.IsEmpty())
	{
		Character = TSoftObjectPtr<UMotionCharacter>(FSoftObjectPath(Spec.CharacterAssetPath));
	}
}

void UMotionDef::ApplyProjectDefaults()
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	if (Character.IsNull())
	{
		Character = Settings->DefaultCharacter;
	}

	if (ProviderId.IsNone())
	{
		FMotionForgeModule* Module = FMotionForgeModule::GetPtr();
		ProviderId = Module ? Module->ResolveDefaultProviderId() : NAME_None;
	}

	// Only inherit the configured model when this definition is actually going to the provider that
	// setting belongs to. A model id is a provider's private vocabulary - stamping Uthana's
	// "text-to-motion-3.0" onto a Kimodo definition produces a generation that fails at the far end
	// with a name the local model has never heard of.
	//
	// Left empty, the provider's own default applies at submit time, which is always right.
	if (ModelId.IsEmpty() && !Settings->DefaultProviderId.IsNone() && ProviderId == Settings->DefaultProviderId)
	{
		ModelId = Settings->DefaultModelId;
	}
}

void UMotionDef::SetStatus(EMotionDefStatus NewStatus, const FString& Error)
{
	Status = NewStatus;

	if (NewStatus == EMotionDefStatus::Failed)
	{
		LastError = Error;
		UE_LOG(LogMotionForge, Warning, TEXT("[%s] failed: %s"), *GetName(), *Error);
	}
	else
	{
		LastError.Reset();
	}

	if (NewStatus == EMotionDefStatus::Ready || NewStatus == EMotionDefStatus::Failed)
	{
		ActiveBatchId.Reset();
	}

	MarkPackageDirty();
}
