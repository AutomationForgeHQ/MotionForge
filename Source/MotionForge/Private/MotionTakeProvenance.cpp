#include "MotionTakeProvenance.h"

#include "MotionDef.h"
#include "MotionForge.h"
#include "MotionForgeSubsystem.h"
#include "MotionPromptSequence.h"

#include "Animation/AnimSequence.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

const FName FMotionProvenance::StepNormalise   = TEXT("blender.normalise");
const FName FMotionProvenance::StepRetarget    = TEXT("rig.retarget");
const FName FMotionProvenance::StepDepenetrate = TEXT("physics.depenetrate");

void UMotionTakeProvenance::PostLoad()
{
	Super::PostLoad();

	// Clips stamped before the list existed carry the two booleans and nothing else. Migrated here so
	// that everything downstream can read one place - the alternative is every reader remembering to
	// check both, which is how the two representations drift apart.
	if (Processing.IsEmpty())
	{
		if (bWasNormalized)
		{
			FMotionProcessingRecord Record;
			Record.StepId = FMotionProvenance::StepNormalise;
			Record.Summary = TEXT("Recorded before the processing list existed; only that it ran is known.");
			Record.At = GeneratedAt;
			Processing.Add(MoveTemp(Record));
		}

		if (bWasRetargeted)
		{
			FMotionProcessingRecord Record;
			Record.StepId = FMotionProvenance::StepRetarget;
			Record.Summary = TEXT("Recorded before the processing list existed; only that it ran is known.");
			Record.At = GeneratedAt;
			Processing.Add(MoveTemp(Record));
		}
	}
}

bool FMotionProvenance::RecordProcessing(UAnimSequence* Sequence, const FMotionProcessingRecord& Step)
{
	if (Sequence == nullptr)
	{
		return false;
	}

	UMotionTakeProvenance* Record = Cast<UMotionTakeProvenance>(
		Sequence->GetAssetUserDataOfClass(UMotionTakeProvenance::StaticClass()));

	if (Record == nullptr)
	{
		// A clip nobody stamped - hand-made, imported by another route, or older than this record.
		// It still gets one, because "this was corrected" is worth more than the tidiness of refusing.
		Record = NewObject<UMotionTakeProvenance>(Sequence);
		Record->FrameCount = Sequence->GetNumberOfSampledKeys();
		Record->DurationSeconds = Sequence->GetPlayLength();
		Sequence->AddAssetUserData(Record);

		UE_LOG(LogMotionForge, Warning,
			TEXT("'%s' had no provenance, so only what was just done to it is recorded. Where it came "
			     "from is not known and cannot be recovered from the asset."),
			*Sequence->GetName());
	}

	FMotionProcessingRecord Entry = Step;
	if (Entry.At == FDateTime())
	{
		Entry.At = FDateTime::UtcNow();
	}

	Record->Processing.Add(MoveTemp(Entry));

	// Written from the list and never beside it, so the summary cannot disagree with the record.
	Record->bWasNormalized = Record->WasProcessedBy(StepNormalise);
	Record->bWasRetargeted = Record->WasProcessedBy(StepRetarget);

	Sequence->MarkPackageDirty();

	UE_LOG(LogMotionForge, Log, TEXT("'%s': %s%s"),
		*Sequence->GetName(), *Step.StepId.ToString(),
		Step.Summary.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" - %s"), *Step.Summary));

	return true;
}

void FMotionProvenance::Stamp(
	UAnimSequence* Sequence,
	const UMotionDef* Definition,
	const FString& ProviderId,
	const FString& ModelId,
	const FString& MotionId,
	const FString& RunnerDescription,
	bool bRunnerWasLocal,
	int32 NativeFrameRate,
	bool bWasNormalized,
	bool bWasRetargeted)
{
	if (Sequence == nullptr)
	{
		return;
	}

	// Replace rather than append. A clip re-imported from the same take is the same clip, and a stack
	// of near-identical records would turn the Details panel into a changelog nobody asked for.
	//
	// This takes the processing history with it, which is right rather than unfortunate: the data has
	// been replaced, so a correction made to what was there before no longer describes what is there
	// now. A clip that needs fixing again is a clip that has not been fixed.
	Sequence->RemoveUserDataOfClass(UMotionTakeProvenance::StaticClass());

	UMotionTakeProvenance* Record = NewObject<UMotionTakeProvenance>(Sequence);

	Record->ProviderId = ProviderId;
	Record->ModelId = ModelId;
	Record->MotionId = MotionId;
	Record->NativeFrameRate = NativeFrameRate;

	// Recorded as events, with the booleans written from them rather than beside them. A clip that has
	// been through Blender has had something done to it, and that belongs in the same list as every
	// later correction - otherwise "what happened to this clip" has two places to look.
	if (bWasNormalized)
	{
		FMotionProcessingRecord Step;
		Step.StepId = StepNormalise;
		Step.Tool = TEXT("Blender");
		Step.Summary = TEXT("Round-tripped through Blender on import, which reorients the rig.");
		Record->Processing.Add(MoveTemp(Step));
	}

	if (bWasRetargeted)
	{
		FMotionProcessingRecord Step;
		Step.StepId = StepRetarget;
		Step.Summary = TEXT("Retargeted from the provider's rig rather than built on ours.");
		Record->Processing.Add(MoveTemp(Step));
	}

	Record->bWasNormalized = Record->WasProcessedBy(StepNormalise);
	Record->bWasRetargeted = Record->WasProcessedBy(StepRetarget);
	Record->GeneratedAt = FDateTime::UtcNow();
	Record->EngineVersion = FEngineVersion::Current().ToString();

	Record->RunnerUrl = RunnerDescription;
	Record->bRunnerWasLocal = bRunnerWasLocal;

	if (Definition)
	{
		Record->Seed = Definition->Control.Seed;
		Record->DiffusionSteps = Definition->Control.DiffusionSteps;
		Record->DefinitionPath = Definition->GetPathName();

		// Resolved, not read off the fields. A definition whose prompt lives on a timeline has a
		// `Prompt` field that is stale by design - the sequence wins - and copying that here would
		// give the clip a confident record of words it was never generated from.
		//
		// Re-resolved rather than passed in: the sequence is referenced and never baked, so the only
		// place the truth lives is the sequence, and reading it is a few strings. An artist who edits
		// the timeline between submitting and importing gets the edited version recorded, which is a
		// real if narrow window and the same one the constraint sequence has always had.
		FMotionPromptRead Beats;
		FString Error;
		FMotionPromptSequence::Resolve(Definition, NativeFrameRate, Beats, Error);

		Record->Prompt = Beats.Prompt.IsEmpty() ? Definition->Prompt : Beats.Prompt;

		// Beats only when they were stated. An even split is reconstructible from the prompt and the
		// length, and recording a guess as though it were a fact is how a record stops being one.
		// Text only when a timeline said which sentence is which - the definition states durations
		// and a prompt, never the pairing.
		for (const FMotionPromptBeat& Beat : Beats.Beats)
		{
			FMotionBeatRecord Record_Beat;
			Record_Beat.Text = Beat.Text;
			Record_Beat.Seconds = Beat.Seconds;
			Record->Beats.Add(Record_Beat);
		}

		// The length that was actually asked for, which is the beats' sum whenever there are beats -
		// `Length` is not used then and recording it would put a 6 next to an eight second clip,
		// which reads as exactly the defect this whole record exists to catch.
		Record->RequestedLengthSeconds = Beats.Beats.Num() > 0
			? FMath::RoundToInt(Beats.TotalSeconds)
			: Definition->Length;
	}

	Record->FrameCount = Sequence->GetNumberOfSampledKeys();
	Record->DurationSeconds = Sequence->GetPlayLength();

	Sequence->AddAssetUserData(Record);
	Sequence->MarkPackageDirty();

	// Say it once, at Log, with the numbers that form the check. A line in the output log is what
	// somebody actually reads on the day something looks wrong.
	UE_LOG(LogMotionForge, Log,
		TEXT("Stamped '%s': %s/%s, %d frames at %dfps (%.2fs)%s%s."),
		*Sequence->GetName(), *ProviderId, *ModelId,
		Record->FrameCount, Record->NativeFrameRate, Record->DurationSeconds,
		bWasNormalized ? TEXT(", normalised") : TEXT(""),
		bWasRetargeted ? TEXT(", retargeted") : TEXT(""));

	if (Record->LooksMisRated())
	{
		// Loud, because this is the failure that passes every other check. See the header.
		UE_LOG(LogMotionForge, Warning,
			TEXT("'%s' has %d frames in %.2fs, which is %.0ffps, but %s runs at %dfps. This clip "
				 "holds %.1fx the motion its length allows and will play at that speed."),
			*Sequence->GetName(), Record->FrameCount, Record->DurationSeconds,
			Record->DerivedFrameRate(), *ProviderId, Record->NativeFrameRate,
			Record->DerivedFrameRate() / FMath::Max(1, Record->NativeFrameRate));
	}
}

TArray<FMotionProvenanceRow> UMotionForgeSubsystem::ReportProvenance() const
{
	TArray<FMotionProvenanceRow> Rows;

	for (const FString& Path : FindMotionDefs({}))
	{
		const UMotionDef* Def = LoadObject<UMotionDef>(nullptr, *Path);
		if (Def == nullptr)
		{
			continue;
		}

		// LoadSynchronous, not Get: a soft pointer to an unloaded asset reads empty and would report
		// every clip in the project as missing. That trap has cost this project a session before.
		UAnimSequence* Sequence = Def->ImportedSequence.LoadSynchronous();
		if (Sequence == nullptr)
		{
			continue;
		}

		FMotionProvenanceRow Row;
		Row.AssetPath = Sequence->GetPathName();
		Row.FrameCount = Sequence->GetNumberOfSampledKeys();
		Row.DurationSeconds = Sequence->GetPlayLength();

		if (const UMotionTakeProvenance* Record = FMotionProvenance::Read(Sequence))
		{
			Row.ProviderId = Record->ProviderId;
			Row.ModelId = Record->ModelId;
			Row.MotionId = Record->MotionId;
			Row.NativeFrameRate = Record->NativeFrameRate;
			Row.bWasNormalized = Record->bWasNormalized;
			Row.bWasRetargeted = Record->bWasRetargeted;

			for (const FMotionProcessingRecord& Step : Record->Processing)
			{
				Row.Processing.Add(Step.StepId);
			}
			Row.GeneratedAt = Record->GeneratedAt.ToString();
			Row.bLooksMisRated = Record->LooksMisRated();
		}

		// Derived from the asset either way, so a clip with no record still gets the useful half - it
		// simply has nothing to compare against, which is what an empty ProviderId means here.
		Row.DerivedFrameRate = Row.DurationSeconds > KINDA_SMALL_NUMBER
			? Row.FrameCount / Row.DurationSeconds
			: 0.f;

		Rows.Add(MoveTemp(Row));
	}

	return Rows;
}

const UMotionTakeProvenance* FMotionProvenance::Read(const UAnimSequence* Sequence)
{
	if (Sequence == nullptr)
	{
		return nullptr;
	}

	// const_cast because GetAssetUserData is non-const on UAnimSequence while being a plain read.
	return Cast<UMotionTakeProvenance>(
		const_cast<UAnimSequence*>(Sequence)->GetAssetUserDataOfClass(UMotionTakeProvenance::StaticClass()));
}
