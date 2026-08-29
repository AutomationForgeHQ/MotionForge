// The whole pipeline, as a scriptable API.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MotionForgeTypes.h"
#include "MotionPromptSequence.h"
#include "IMotionProvider.h"
#include "Containers/Ticker.h"
#include "MotionForgeSubsystem.generated.h"

class UMotionDef;
class UMotionCharacter;
class UAnimSequence;

/** One in-flight job, so a poll response can be matched back to the definition that wants it. */
struct FMotionJobTracking
{
	FString JobId;
	FString DefinitionPath;
	FName   ProviderId;
	int32   VariantIndex = 0;
	double  SubmittedAt = 0.0;

	/** Guards against stacking a second poll on a request that has not answered yet. */
	bool    bPollInFlight = false;

	/** Set once the job reaches a terminal state, so it stops being polled. */
	bool    bSettled = false;
};

/** A group of definitions moving through the pipeline together. */
struct FMotionBatch
{
	FString BatchId;
	EMotionPipelineMode Mode = EMotionPipelineMode::HumanInTheLoop;
	TArray<FString> DefinitionPaths;
	TArray<FMotionJobTracking> Jobs;
	double StartedAt = 0.0;
	bool bCancelled = false;
};

/**
 * Everything MotionForge can do, callable from C++, Blueprint, Python and therefore MCP.
 *
 * The editor UI is a thin layer over this and holds no logic of its own. That is deliberate: if a
 * capability only exists behind a button, an agent cannot use it, and a pipeline that needs a human
 * to click things is not a pipeline.
 *
 * Four rules every function here keeps:
 *
 *   Idempotent      Re-running an operation that is already underway does nothing. An agent that
 *                   retries after a timeout must not be able to pay for the same generation twice.
 *   Non-blocking    Long operations return a batch id immediately; progress is polled. The editor
 *                   never stalls and a caller can check back whenever it likes.
 *   Structured      Status and errors come back as USTRUCTs, not log lines. JSON forms exist too,
 *                   as thin wrappers, for Python and the review widget.
 *   Costed first    EstimateCost exists so nothing spends money without the caller being able
 *                   to find out first.
 */
UCLASS()
class MOTIONFORGE_API UMotionForgeSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	static UMotionForgeSubsystem* Get();

	// ---------------------------------------------------------------------------------------------
	// Authoring
	// ---------------------------------------------------------------------------------------------

	/**
	 * Create a motion definition asset from a spec.
	 * @return content path of the new asset, or empty on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	FString CreateMotionDef(const FMotionDefSpec& Spec);

	/** Overwrite a definition's authoring fields. Pipeline state and candidates are left alone. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	bool UpdateMotionDef(const FString& AssetPath, const FMotionDefSpec& Spec);

	/**
	 * Motion definitions in the project.
	 * @param StatusFilter empty for everything, or the statuses to keep.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	TArray<FString> FindMotionDefs(const TArray<EMotionDefStatus>& StatusFilter) const;

	/**
	 * What every imported clip says about itself, and which of them contradict their own frame rate.
	 *
	 * The check is `frames / duration == the provider's native rate`. It costs one division and it is
	 * the only thing that catches a clip holding twice the motion its length allows - which passes
	 * bone counts, curve counts, a clean log, and a duration exactly as requested. Three shipped that
	 * way once; run this after any change to a provider, an image, or the machine doing the importing.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring",
		meta = (AFNode = "Report/Motion", AFShape = "map", AFGrain = "run", AFIdempotent, AFReadOnly, AFCostPerItem = "0"))
	TArray<FMotionProvenanceRow> ReportProvenance() const;


	/**
	 * FindMotionDefs by status name rather than enum, for Python and the review widget.
	 * @param StatusFilter empty for everything, or a status name such as "AwaitingReview".
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	TArray<FString> ListMotionDefs(const FString& StatusFilter) const;

	// ---------------------------------------------------------------------------------------------
	// The prompt on a timeline
	// ---------------------------------------------------------------------------------------------

	/**
	 * Lay a definition's prompt out in a Level Sequence, one section per beat, and point the
	 * definition at it.
	 *
	 * From then on **the sequence is the prompt.** Beats can be dragged, retimed, split and rewritten
	 * on a timeline where their durations are visible, instead of hiding in a sentence whose full
	 * stops silently decide the structure. Generation reads the track; the definition's own `Prompt`
	 * and `BeatSeconds` are ignored while it is set, and left untouched underneath.
	 *
	 * It is the same sequence as `Control.ConstraintSequence`, on purpose. Constraint keys index the
	 * whole timeline and prompt beats divide it, so an existing constraint track sits alongside this
	 * one and needs no translation - one sequence carries the poses and the words.
	 *
	 * Re-running it on a definition that already has one replaces the beats and keeps everything else
	 * on the sequence, so a prompt can be re-laid-out without losing authored poses.
	 *
	 * @param SequenceAssetPath Where to put it. Empty puts it beside the other generated sequences.
	 * @return content path of the sequence, or empty with OutError set.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Prompt")
	FString CreatePromptSequence(
		const FString& DefinitionPath,
		const FString& SequenceAssetPath,
		FString& OutError);

	/**
	 * What a definition will actually be generated from - its beats, their durations, and what is
	 * wrong with them.
	 *
	 * Answers for a definition with a prompt sequence and one without, so a caller never has to know
	 * which. `bFromSequence` says which it was.
	 *
	 * **Read `Problems` before generating.** Nothing in it stops a generation; every entry is
	 * something that will change what comes back in a way the timeline does not show.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Prompt")
	FMotionPromptRead ReadPromptSequence(const FString& DefinitionPath, FString& OutError) const;

	/**
	 * Copy the sequence's beats back onto the definition, as its `Prompt`, `BeatSeconds` and `Length`.
	 *
	 * Insurance rather than a step in the workflow. The sequence keeps winning afterwards - this does
	 * not clear the reference, because the same sequence usually carries the constraint poses too and
	 * dropping those to bake a prompt would be a poor trade. What it buys is that **deleting the
	 * sequence later costs nothing**: the definition already says the same thing, in the form it
	 * understood before the timeline existed.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Prompt")
	bool BakePromptSequence(const FString& DefinitionPath, FString& OutError);

	/**
	 * Export a motion character's mesh to FBX, ready to upload to a provider.
	 *
	 * Providers retarget server-side against a character you have given them, so this is the first
	 * step of pairing one: export, upload by hand, paste the returned id into ProviderCharacterId.
	 *
	 * The character's SourceFbxPath is stamped with the file written, so the pairing stays traceable
	 * after the fact.
	 *
	 * @param CharacterAssetPath Content path of the UMotionCharacter.
	 * @param AbsoluteOutputPath Where to write. Empty writes into the staging directory.
	 * @param OutError Why it failed, when it did.
	 * @return absolute path of the file written, or empty on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	FString ExportCharacterFbx(
		const FString& CharacterAssetPath,
		const FString& AbsoluteOutputPath,
		FString& OutError);

	/**
	 * Export a motion character and upload it to its provider, in one call.
	 *
	 * This is the whole pairing: the character's Preview Mesh is exported to FBX, sent to the
	 * provider, and the id that comes back is written into ProviderCharacterId and saved. After this
	 * the character is ready to generate against, with nothing to paste by hand.
	 *
	 * Not idempotent, and deliberately so - calling it twice creates two characters on the provider.
	 * Characters that already have a ProviderCharacterId are refused unless bForce, because silently
	 * repointing a character would orphan every take already generated against the old id.
	 *
	 * @param OnComplete Runs on the game thread. bSuccess false means OutError explains why.
	 */
	void UploadCharacter(
		const FString& CharacterAssetPath,
		const FMotionCharacterUploadOptions& Options,
		bool bForce,
		TFunction<void(bool /*bSuccess*/, const FMotionCharacterUpload& /*Result*/, const FString& /*Error*/)> OnComplete);

	/**
	 * Fetch a paired character back from its provider and import it, filling in ProviderMesh.
	 *
	 * This is the rig clips actually arrive on. Providers normalise a character on ingest - Uthana
	 * drops this project's root and root1 - and then generate against the normalised version, so the
	 * file we uploaded is not the skeleton their animation fits. Importing their copy gives an exact
	 * match and removes every reason to reconstruct bones by hand.
	 *
	 * Run once per character, after Upload Character To Provider. Then author an IK Retargeter from
	 * ProviderMesh to PreviewMesh and set it on the character; imports retarget from then on.
	 */
	void ImportProviderCharacter(
		const FString& CharacterAssetPath,
		TFunction<void(bool /*bSuccess*/, const FString& /*MeshPath*/, const FString& /*Error*/)> OnComplete);

	/** Characters the provider already holds, so an upload can be skipped. */
	void ListProviderCharacters(
		FName ProviderId,
		TFunction<void(bool /*bSuccess*/, const TArray<FMotionRemoteCharacter>&, const FString& /*Error*/)> OnComplete);

	/**
	 * Create and queue a whole library in one call.
	 *
	 * The entry point for scripts and agents that want to define twenty motions without twenty round
	 * trips. Definitions whose asset already exists are updated rather than duplicated.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	FMotionBatchSubmission SubmitBatch(const TArray<FMotionDefSpec>& Specs, EMotionPipelineMode Mode);

	/**
	 * SubmitBatch driven by {"mode": "...", "definitions": [ {spec}, ... ]}.
	 *
	 * Kept for Python and the review widget, which have a JSON payload to hand and no convenient way
	 * to build an FMotionDefSpec array. C++, Blueprint and MCP callers should use SubmitBatch.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	FString SubmitBatchFromJson(const FString& Json);

	// ---------------------------------------------------------------------------------------------
	// Pipeline
	// ---------------------------------------------------------------------------------------------

	/**
	 * Submit generation jobs. Stops at AwaitingReview.
	 *
	 * Definitions already generating are skipped rather than resubmitted.
	 * @return batch id, or empty when nothing was eligible.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	FString Generate(const TArray<FString>& AssetPaths);

	/** Generate, auto-pick the first usable take, download, normalise and import without stopping. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	FString RunFullPipeline(const TArray<FString>& AssetPaths);

	/** Download the chosen take for each definition, then normalise and import it. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	FString DownloadSelected(const TArray<FString>& AssetPaths);

	/** Choose which take a definition should use. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool SelectCandidate(const FString& AssetPath, const FString& MotionId);

	/**
	 * Stop polling a batch.
	 *
	 * Jobs already submitted keep running on the provider and their motion ids are still recorded, so
	 * nothing paid for is thrown away - only the waiting stops.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool CancelBatch(const FString& BatchId);

	// ---------------------------------------------------------------------------------------------
	// Observation
	// ---------------------------------------------------------------------------------------------

	/** Per-definition status, takes and errors. Empty array means every definition. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	TArray<FMotionDefinitionStatus> GetStatus(const TArray<FString>& AssetPaths) const;

	/** How far a batch has got. Check bTracked before reading the counts. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FMotionBatchStatus GetBatchStatus(const FString& BatchId) const;

	/**
	 * What downloading these definitions would cost, in seconds of motion.
	 *
	 * Providers meter download seconds, generated seconds, or both. This reports the quantity; what
	 * a second costs depends on the plan, which the plugin deliberately does not try to model.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FMotionCostEstimate EstimateCost(const TArray<FString>& AssetPaths, bool bSelectedOnly) const;

	/**
	 * What generating a set of specs would cost, before any of them exist.
	 *
	 * The one estimate that matters on pay-as-you-go, where generation is what bills: it has to be
	 * answerable *before* submitting, and every other estimate here needs the assets to exist first.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FMotionCostEstimate EstimateGenerationCost(const TArray<FMotionDefSpec>& Specs) const;

	/**
	 * Whether a provider has a usable credential, without revealing it.
	 * @return false when there is no such provider, leaving OutInfo untouched.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	bool GetCredentialInfo(FName ProviderId, FMotionCredentialInfo& OutInfo) const;

	// The JSON forms below exist for Python and the review widget. They are thin wrappers over the
	// typed calls above - new callers should use those, so schema changes stay in one place.

	/** GetStatus as {"ok": true, "definitions": [...]}. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FString GetStatusJson(const TArray<FString>& AssetPaths) const;

	/** GetBatchStatus as JSON. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FString GetBatchStatusJson(const FString& BatchId) const;

	/** EstimateCost as JSON. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FString EstimateCostJson(const TArray<FString>& AssetPaths, bool bSelectedOnly) const;

	/** GetCredentialInfo as JSON, with an error envelope when the provider is unknown. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FString DescribeCredential(FName ProviderId) const;

	/** Make one cheap authenticated call and report what happened. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	void TestConnection(FName ProviderId);

	/** Store a provider secret in the OS credential vault. Never persisted in project files. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	bool SetCredential(FName ProviderId, const FString& Secret);

	// ---------------------------------------------------------------------------------------------
	// Providers
	// ---------------------------------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "MotionForge|Providers")
	TArray<FName> GetProviderIds() const;

	/**
	 * What a provider can do - frame rate, length limits, whether it bills, whether it seeds.
	 *
	 * Read this rather than assuming. Every field on it was once a global setting that happened to
	 * be right for one provider.
	 *
	 * @param ProviderId Leave as None for the default provider. Unknown ids come back with an empty
	 *        ProviderId, which is how a caller tells "no such provider" from a real answer.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Providers")
	FMotionProviderCaps GetProviderCaps(FName ProviderId) const;

	TSharedPtr<IMotionProvider> FindProvider(FName ProviderId) const;

private:

	/** Remove the provider-rig intermediate once the retargeted clip is saved. */
	void DeleteSourceClip(class UAnimSequence* SourceSequence);


	/**
	 * Un-stick definitions a previous session left claiming to be generating.
	 *
	 * Batches are in-memory only, so nothing can legitimately be in flight at startup - and a
	 * definition stuck at Generating is refused by `IsBusy` forever, with the Generate button
	 * cheerfully reporting a batch that cannot exist.
	 */
	void ReleaseStrandedDefinitions();

	/** Load a definition by content path. */
	UMotionDef* LoadDef(const FString& AssetPath) const;

	/** Resolve provider and character for a definition, filling in settings defaults. */
	bool ResolveDefinition(
		UMotionDef* Def,
		TSharedPtr<IMotionProvider>& OutProvider,
		UMotionCharacter*& OutCharacter,
		FString& OutError) const;

	/** Shared implementation behind Generate and RunFullPipeline. */
	FString StartGeneration(const TArray<FString>& AssetPaths, EMotionPipelineMode Mode);

	/** Poll every in-flight job. Driven by the ticker. */
	bool Tick(float DeltaTime);

	void OnJobFinished(FMotionBatch& Batch, const FMotionJobTracking& Job, const FMotionJobResult& JobResult);

	/** Called once every job for a definition has settled. */
	void OnDefinitionGenerated(FMotionBatch& Batch, UMotionDef* Def);

	/** Fetch, normalise and import the selected candidate. */
	void ProcessSelected(UMotionDef* Def);

	/** Normalise and import a file already on disk. */
	void NormalizeAndImport(UMotionDef* Def, const FString& RawPath);

	/** Write an asset's package to disk, if anything dirtied it. */
	static void SaveAsset(UObject* Asset);

public:

	/**
	 * Put the finished clip on an animation track in the definition's prompt sequence.
	 *
	 * So that the thing you asked for and the thing you got are on adjacent rows: the beats above, the
	 * animation below, and any beat that came back wrong is one glance rather than one export.
	 *
	 * Only ever touches a sequence the definition already points at, and only its own animation track,
	 * which it replaces. Silent when there is no prompt sequence, no binding, or no clip - none of
	 * those is a fault, and an import must not fail over where its result was displayed.
	 */
	static void PlaceTakeOnPromptSequence(UMotionDef* Def, UAnimSequence* Sequence);

private:

	/**
	 * Move a clip from the provider's rig onto the character's own, via its IK Retargeter.
	 * @return the retargeted sequence, or null with OutError explaining what is not set up.
	 */
	UAnimSequence* RetargetToCharacterRig(
		UMotionDef* Def,
		UMotionCharacter* Character,
		UAnimSequence* SourceSequence,
		FString& OutError);

	/**
	 * Fill in which of the two second-counts actually bills, and what it comes to.
	 *
	 * @param bAnyMetered False when nothing in the estimate uses a provider that charges, which
	 *        zeroes the money rather than reporting a plan that does not apply.
	 */
	static void ApplyBilling(FMotionCostEstimate& Estimate, bool bAnyMetered);

	/** Batch ids are readable rather than GUIDs, because a human reads them in logs. */
	static FString MakeBatchId();

	/**
	 * The rate a provider generates at, which is the only rate a clip should be handled at.
	 *
	 * Falls back to the settings value only when there is no provider to ask.
	 */
	static int32 ResolveFrameRate(const TSharedPtr<IMotionProvider>& Provider);

	TMap<FString, FMotionBatch> Batches;

	/** The stranded-definition sweep runs once, on the first tick. See Tick for why not in Initialize. */
	bool bSweptStrandedDefinitions = false;

	FTSTicker::FDelegateHandle TickHandle;
	double LastPollTime = 0.0;
};
