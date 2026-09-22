#include "MotionDef.h"

#include "IMotionProvider.h"
#include "MotionCharacter.h"
#include "MotionForge.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"
#include "MotionPipeline.h"

#include "Animation/AnimSequence.h"
#include "UObject/AssetRegistryTagsContext.h"

const FName UMotionDef::TagStatus(TEXT("MotionStatus"));
const FName UMotionDef::TagProvider(TEXT("MotionProvider"));
const FName UMotionDef::TagTakeCount(TEXT("MotionTakes"));
const FName UMotionDef::TagClip(TEXT("MotionClip"));
const FName UMotionDef::TagPrompt(TEXT("MotionPrompt"));
const FName UMotionDef::TagCharacter(TEXT("MotionCharacter"));

FString FMotionCandidate::GetLabel() const
{
	if (TakeNumber > 0)
	{
		return FString::Printf(TEXT("Take %d"), TakeNumber);
	}

	// Made before takes were numbered. The variant index restarts every run, so it alone would repeat;
	// the time it was made keeps two of them apart.
	return GeneratedAt == FDateTime()
		? FString::Printf(TEXT("Older take %d"), VariantIndex + 1)
		: FString::Printf(TEXT("Older take %d (%s)"), VariantIndex + 1, *GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M")));
}

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
		if (Candidate.IsUsable())
		{
			++Count;
		}
	}
	return Count;
}

int32 UMotionDef::EstimateDownloadSeconds(bool bSelectedOnly) const
{
	// A take's own recorded length where it has one: providers meter what was asked for, and that is
	// what the take remembers. Older takes fall back to the definition's length.
	auto SecondsOf = [this](const FMotionCandidate& Candidate)
	{
		return Candidate.LengthSeconds > 0.f ? FMath::CeilToInt(Candidate.LengthSeconds) : Length;
	};

	if (bSelectedOnly)
	{
		const FMotionCandidate* Selected = FindSelectedCandidate();
		if (!Selected || Selected->bDownloaded)
		{
			return 0;
		}
		return SecondsOf(*Selected);
	}

	int32 Seconds = 0;
	for (const FMotionCandidate& Candidate : Candidates)
	{
		if (Candidate.IsUsable() && !Candidate.bDownloaded)
		{
			Seconds += SecondsOf(Candidate);
		}
	}
	return Seconds;
}

void UMotionDef::ApplySpec(const FMotionDefSpec& Spec, TArray<FString>* OutProblems)
{
	// Only authoring fields, and only the ones given - a spec must never rewrite pipeline state and
	// orphan the takes already paid for, and an update naming one field must not reset the others.
	if (!Spec.Prompt.IsEmpty())
	{
		Prompt = Spec.Prompt;
	}

	if (Spec.Length > 0)
	{
		Length = Spec.Length;
	}

	if (Spec.Variants > 0)
	{
		Variants = FMath::Clamp(Spec.Variants, 1, 16);
	}

	if (Spec.bClearTrim)
	{
		TrimWindow = FVector2D::ZeroVector;
	}
	else if (!Spec.TrimWindow.IsNearlyZero())
	{
		TrimWindow = Spec.TrimWindow;
	}

	if (!Spec.ProviderId.IsNone())
	{
		ProviderId = Spec.ProviderId;
	}

	if (!Spec.CharacterAssetPath.IsEmpty())
	{
		Character = TSoftObjectPtr<UMotionCharacter>(FSoftObjectPath(Spec.CharacterAssetPath));
	}

	// The authored half of Control, field by field.
	if (Spec.Control.Constraints.Num() > 0)
	{
		Control.Constraints = Spec.Control.Constraints;
	}
	if (!Spec.Control.ConstraintSequence.IsNull())
	{
		Control.ConstraintSequence = Spec.Control.ConstraintSequence;
	}
	if (Spec.Control.BeatSeconds.Num() > 0)
	{
		Control.BeatSeconds = Spec.Control.BeatSeconds;
	}

	auto Refuse = [OutProblems](const FString& Problem)
	{
		UE_LOG(LogMotionForge, Warning, TEXT("%s"), *Problem);
		if (OutProblems)
		{
			OutProblems->Add(Problem);
		}
	};

	const FName Provider = GetResolvedProviderId();
	UMotionPipeline* Pipeline = GetOrCreatePipeline(Provider);

	const bool bAnySetting = !Spec.ModelId.IsEmpty() || Spec.PipelineOptions.Num() > 0
		|| !Spec.bRewritePrompt || Spec.Control.HasSamplerSettings();

	if (Pipeline == nullptr)
	{
		if (bAnySetting)
		{
			Refuse(FString::Printf(
				TEXT("'%s' has no provider settings to write to - %s is not installed or declares none. "
					 "Model and options were not applied."),
				*GetName(), Provider.IsNone() ? TEXT("no provider") : *Provider.ToString()));
		}
		return;
	}

	// Callers written before providers declared their own settings still get their seed and steps
	// through. The pipeline decides which of them it has any use for.
	//
	// **Only the ones the caller set.** MigrateLegacy writes every legacy field, which is right on load,
	// where all of them are the definition's own. From a spec the fields left alone are defaults, and
	// passing them straight through reset seed, postprocess and beat splitting when an agent set only
	// the steps. So the translation runs twice on scratch copies - from what was sent, and from nothing -
	// and only the settings that come out different are written. A legacy field set to its own default
	// therefore changes nothing; PipelineOptions below is the way to set a value back.
	if (Spec.Control.HasSamplerSettings() || Spec.Control.ConstraintSequenceType != EMotionConstraintType::FullBody
		|| !Spec.bRewritePrompt)
	{
		UMotionPipeline* FromSpec = NewObject<UMotionPipeline>(GetTransientPackage(), Pipeline->GetClass());
		UMotionPipeline* FromNothing = NewObject<UMotionPipeline>(GetTransientPackage(), Pipeline->GetClass());
		FromSpec->MigrateLegacy(FString(), Spec.bRewritePrompt, Spec.Control);
		FromNothing->MigrateLegacy(FString(), true, FMotionControl());

		const TArray<FMotionPipelineOption> Unset = FromNothing->DescribeOptions();
		for (const FMotionPipelineOption& Wanted : FromSpec->DescribeOptions())
		{
			const FMotionPipelineOption* Same = Unset.FindByPredicate(
				[&Wanted](const FMotionPipelineOption& Other) { return Other.Key == Wanted.Key; });
			if (Same != nullptr && Same->Value == Wanted.Value)
			{
				continue;
			}

			FString Error;
			if (!Pipeline->SetOption(Wanted.Key, Wanted.Value, Error))
			{
				Refuse(Error);
			}
		}
	}

	if (!Spec.ModelId.IsEmpty())
	{
		FString Error;
		if (!Pipeline->SetModelId(Spec.ModelId, Error))
		{
			Refuse(Error);
		}
	}

	for (const TPair<FString, FString>& Option : Spec.PipelineOptions)
	{
		FString Error;
		if (!Pipeline->SetOption(Option.Key, Option.Value, Error))
		{
			Refuse(Error);
		}
	}
}

void UMotionDef::ApplyProjectDefaults()
{
	if (ProviderId.IsNone())
	{
		FMotionForgeModule* Module = FMotionForgeModule::GetPtr();
		ProviderId = Module ? Module->ResolveDefaultProviderId() : NAME_None;
	}

	// The default character only when it suits the provider. Stamping an Uthana character onto a
	// Kimodo definition is how MD_WalkCycle failed three different ways on 2026-09-15 while readiness
	// said it could generate.
	if (Character.IsNull())
	{
		if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
		{
			Character = TSoftObjectPtr<UMotionCharacter>(
				FSoftObjectPath(Subsystem->PickCharacterFor(this, GetResolvedProviderId())));
		}
		else
		{
			Character = UMotionForgeSettings::Get()->DefaultCharacter;
		}
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

	if (NewStatus == EMotionDefStatus::Ready || NewStatus == EMotionDefStatus::Failed
		|| NewStatus == EMotionDefStatus::AwaitingReview)
	{
		ActiveBatchId.Reset();
	}

	MarkPackageDirty();
}

// -------------------------------------------------------------------------------------------------
// Pipelines
// -------------------------------------------------------------------------------------------------

FName UMotionDef::GetResolvedProviderId() const
{
	if (!ProviderId.IsNone())
	{
		return ProviderId;
	}

	const FMotionForgeModule* Module = FMotionForgeModule::GetPtrIfLoaded();
	return Module ? Module->ResolveDefaultProviderId() : NAME_None;
}

UMotionPipeline* UMotionDef::FindPipeline(FName InProviderId) const
{
	for (const TObjectPtr<UMotionPipeline>& Pipeline : Pipelines)
	{
		if (Pipeline && Pipeline->GetProviderId() == InProviderId)
		{
			return Pipeline;
		}
	}
	return nullptr;
}

UMotionPipeline* UMotionDef::GetOrCreatePipeline(FName InProviderId)
{
	if (UMotionPipeline* Existing = FindPipeline(InProviderId))
	{
		return Existing;
	}

	FMotionForgeModule* Module = FMotionForgeModule::GetPtrIfLoaded();
	TSharedPtr<IMotionProvider> Provider = Module ? Module->FindProvider(InProviderId) : nullptr;
	UClass* Class = Provider.IsValid() ? Provider->GetPipelineClass() : nullptr;

	if (Class == nullptr || !Class->IsChildOf(UMotionPipeline::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
	{
		return nullptr;
	}

	Modify();

	UMotionPipeline* Created = NewObject<UMotionPipeline>(this, Class, NAME_None, RF_Transactional | RF_Public);
	Created->OnCreated();
	Pipelines.Add(Created);
	MarkPackageDirty();
	return Created;
}

const UMotionPipeline* UMotionDef::GetPipelineForRead(FName InProviderId) const
{
	if (const UMotionPipeline* Existing = FindPipeline(InProviderId))
	{
		return Existing;
	}

	const FMotionForgeModule* Module = FMotionForgeModule::GetPtrIfLoaded();
	TSharedPtr<IMotionProvider> Provider = Module ? Module->FindProvider(InProviderId) : nullptr;
	UClass* Class = Provider.IsValid() ? Provider->GetPipelineClass() : nullptr;

	return Class ? Cast<UMotionPipeline>(Class->GetDefaultObject()) : nullptr;
}

bool UMotionDef::MigrateLegacySettings()
{
	// What a definition saved before 2026-09-18 may carry outside any pipeline: a model id, the rewrite
	// flag, and sampler settings inside Control. Anything still at its old default carries nothing.
	const FMotionControl Defaults;

	const bool bLegacyModel   = !ModelId_DEPRECATED.IsEmpty();
	const bool bLegacyRewrite = !bRewritePrompt_DEPRECATED;
	const bool bLegacyControl = Control.Seed != Defaults.Seed
		|| Control.DiffusionSteps != Defaults.DiffusionSteps
		|| Control.TextGuidance != Defaults.TextGuidance
		|| Control.ConstraintGuidance != Defaults.ConstraintGuidance
		|| Control.bPostProcess != Defaults.bPostProcess
		|| Control.bSplitPromptIntoBeats != Defaults.bSplitPromptIntoBeats
		|| Control.ConstraintSequenceType != Defaults.ConstraintSequenceType;

	if (!bLegacyModel && !bLegacyRewrite && !bLegacyControl)
	{
		return false;
	}

	// The providers are separate plugins. An asset loaded before they registered keeps its old fields
	// until something asks again - nothing is lost by waiting, and guessing would be.
	const FMotionForgeModule* Module = FMotionForgeModule::GetPtrIfLoaded();
	if (Module == nullptr || Module->GetProviderIds().Num() == 0)
	{
		return false;
	}

	// Offered to every installed provider, not only the current one: a definition on Uthana that once
	// carried a Kimodo seed should find it again after switching back. Each pipeline keeps only what it
	// understands, and one that changed nothing is not kept.
	const FName Current = GetResolvedProviderId();

	for (const FName Id : Module->GetProviderIds())
	{
		TSharedPtr<IMotionProvider> Provider = Module->FindProvider(Id);
		UClass* Class = Provider.IsValid() ? Provider->GetPipelineClass() : nullptr;
		if (Class == nullptr)
		{
			continue;
		}

		const bool bExisted = FindPipeline(Id) != nullptr;
		UMotionPipeline* Pipeline = GetOrCreatePipeline(Id);
		if (Pipeline == nullptr)
		{
			continue;
		}

		const FString Before = Pipeline->Signature();

		// The model only for the provider it was chosen for - a model id is one provider's vocabulary.
		Pipeline->MigrateLegacy(Id == Current ? ModelId_DEPRECATED : FString(), bRewritePrompt_DEPRECATED, Control);

		if (!bExisted && Id != Current && Pipeline->Signature() == Before)
		{
			Pipelines.Remove(Pipeline);
			Pipeline->MarkAsGarbage();
		}
	}

	UE_LOG(LogMotionForge, Log,
		TEXT("'%s': moved its model and sampler settings into the provider pipelines. Save it to keep "
			 "the new form."), *GetName());

	ModelId_DEPRECATED.Reset();
	bRewritePrompt_DEPRECATED = true;

	Control.Seed = Defaults.Seed;
	Control.DiffusionSteps = Defaults.DiffusionSteps;
	Control.TextGuidance = Defaults.TextGuidance;
	Control.ConstraintGuidance = Defaults.ConstraintGuidance;
	Control.bPostProcess = Defaults.bPostProcess;
	Control.bSplitPromptIntoBeats = Defaults.bSplitPromptIntoBeats;
	Control.ConstraintSequenceType = Defaults.ConstraintSequenceType;

	return true;
}

float UMotionDef::GetAuthoredLengthSeconds() const
{
	if (Control.BeatSeconds.Num() == 0)
	{
		return Length;
	}

	float Total = 0.f;
	for (const float Seconds : Control.BeatSeconds)
	{
		Total += Seconds;
	}
	return Total;
}

// -------------------------------------------------------------------------------------------------
// UObject
// -------------------------------------------------------------------------------------------------

void UMotionDef::PostLoad()
{
	Super::PostLoad();

	// Takes made before numbering are numbered now, in the order they were made - which is their order in
	// the list, since takes are only ever appended. "Older take 1" three times over told nobody anything.
	if (Candidates.Num() > 0)
	{
		int32 Highest = 0;
		for (const FMotionCandidate& Candidate : Candidates)
		{
			Highest = FMath::Max(Highest, Candidate.TakeNumber);
		}

		int32 Next = 1;
		for (FMotionCandidate& Candidate : Candidates)
		{
			if (Candidate.TakeNumber <= 0)
			{
				while (Candidates.ContainsByPredicate([Next](const FMotionCandidate& C) { return C.TakeNumber == Next; }))
				{
					++Next;
				}
				Candidate.TakeNumber = Next++;
			}
			Highest = FMath::Max(Highest, Candidate.TakeNumber);
		}

		NextTakeNumber = FMath::Max(NextTakeNumber, Highest + 1);
	}

	// Saved before the imported take was recorded. The chosen take is the one imported, unless a later
	// choice failed to import - which the old record cannot tell apart, so this is the best reading.
	if (ImportedMotionId.IsEmpty() && !ImportedSequence.IsNull() && !SelectedMotionId.IsEmpty())
	{
		ImportedMotionId = SelectedMotionId;
	}

	bMigratedOnLoad = MigrateLegacySettings();
}

void UMotionDef::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	Super::GetAssetRegistryTags(Context);

	// What the library lists, so seventy definitions can be drawn without loading one of them.
	const UEnum* StatusEnum = StaticEnum<EMotionDefStatus>();
	Context.AddTag(FAssetRegistryTag(TagStatus,
		StatusEnum ? StatusEnum->GetNameStringByValue(static_cast<int64>(Status)) : FString(),
		FAssetRegistryTag::TT_Alphabetical));

	Context.AddTag(FAssetRegistryTag(TagProvider, ProviderId.ToString(), FAssetRegistryTag::TT_Alphabetical));

	int32 Usable = 0;
	for (const FMotionCandidate& Candidate : Candidates)
	{
		if (Candidate.IsUsable() && !Candidate.bHidden)
		{
			++Usable;
		}
	}
	Context.AddTag(FAssetRegistryTag(TagTakeCount, FString::FromInt(Usable), FAssetRegistryTag::TT_Numerical));

	Context.AddTag(FAssetRegistryTag(TagClip, ImportedSequence.ToString(), FAssetRegistryTag::TT_Alphabetical));
	Context.AddTag(FAssetRegistryTag(TagPrompt, Prompt.Left(200), FAssetRegistryTag::TT_Alphabetical));
	Context.AddTag(FAssetRegistryTag(TagCharacter, Character.ToString(), FAssetRegistryTag::TT_Alphabetical));
}

#if WITH_EDITOR
void UMotionDef::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);

	if (PropertyAboutToChange && PropertyAboutToChange->GetFName() == GET_MEMBER_NAME_CHECKED(UMotionDef, ProviderId))
	{
		ProviderBeforeEdit = GetResolvedProviderId();
	}
}

void UMotionDef::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);

	const FName Changed = Event.GetMemberPropertyName();

	// Switching provider through the details panel does what the window's picker does: keep each
	// provider's settings, and pick a character that suits the new one.
	if (Changed == GET_MEMBER_NAME_CHECKED(UMotionDef, ProviderId))
	{
		if (!ProviderBeforeEdit.IsNone() && !Character.IsNull())
		{
			CharacterByProvider.Add(ProviderBeforeEdit, Character);
		}
		ProviderBeforeEdit = NAME_None;

		if (UMotionForgeSubsystem* Subsystem = UMotionForgeSubsystem::Get())
		{
			Subsystem->OnDefinitionProviderChanged(this);
		}
	}
	else if (Changed == GET_MEMBER_NAME_CHECKED(UMotionDef, Character))
	{
		if (!Character.IsNull())
		{
			CharacterByProvider.Add(GetResolvedProviderId(), Character);
		}
	}
}
#endif
