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
class UMotionPipeline;
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

	/** Past the timeout. Still polled, less often, because the provider may yet finish - and bill. */
	bool    bLate = false;
	double  LastPolledAt = 0.0;
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

	/**
	 * Submissions sent and not yet answered, per definition. A definition is not finished while any of
	 * its takes is still being submitted - a fast failure on the first must not settle it before the
	 * second has a job id.
	 */
	TMap<FString, int32> PendingSubmits;

	/** Providers being started before anything can be submitted. The batch is not empty while they run. */
	int32 PendingPrepares = 0;
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

	/** The same, saying which of the spec's settings were refused - an unknown option, a model the provider lacks. */
	FString CreateMotionDefChecked(const FMotionDefSpec& Spec, TArray<FString>& OutProblems);

	/** Update Motion Def, saying which of the spec's settings were refused. */
	bool UpdateMotionDefChecked(const FString& AssetPath, const FMotionDefSpec& Spec, TArray<FString>& OutProblems);

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
	 * Put the definition's imported animation on its prompt sequence's animation row.
	 *
	 * The beats above and the clip they produced below, on adjacent rows - which is the whole reason
	 * to look at a prompt on a timeline rather than in a text field. Creating a sequence does this
	 * already; this is the same thing for a sequence that already exists, whose row is empty because
	 * it was built before the clip was imported, or stale because a different take was chosen since.
	 *
	 * Idempotent: the row is replaced, never appended to, so a stack of every take ever imported
	 * cannot build up. Silent and successful when there is no sequence or no clip yet - neither is a
	 * fault, and refusing would make this awkward to call speculatively, which is how it is used.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Prompt")
	bool RefreshPromptSequenceTake(const FString& DefinitionPath, FString& OutError);

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
	 * Submit generation jobs. Always stops at Awaiting Review, so a person or an agent chooses.
	 *
	 * Definitions already generating are skipped rather than resubmitted. A provider that can start
	 * itself - a stopped local runner - is started first rather than refused.
	 * @return batch id, or empty when nothing was eligible.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	FString Generate(const TArray<FString>& AssetPaths);

	/**
	 * Generate, take the first usable take, download and import it without stopping.
	 *
	 * For pipelines and agents that were asked for an unattended run. No human button calls it: it
	 * spends without anybody looking.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	FString RunFullPipeline(const TArray<FString>& AssetPaths);

	/** Download the chosen take for each definition, then normalise and import it. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	FString DownloadSelected(const TArray<FString>& AssetPaths);

	/**
	 * Choose a take and import it in one step - the one verb every surface uses for it.
	 *
	 * Replaces the definition's clip. Ask first when Get Clip Users names anything.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool ChooseAndImport(const FString& AssetPath, const FString& MotionId);

	/** Choose which take a definition should use. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool SelectCandidate(const FString& AssetPath, const FString& MotionId);

	/**
	 * Stop waiting on a batch, and put every definition in it back where it can generate again.
	 *
	 * A definition with a finished take goes to Awaiting Review; one with none goes to Failed with the
	 * reason. Jobs already submitted may still finish on the provider - and on a paid one, still bill -
	 * but nothing here waits for them.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool CancelBatch(const FString& BatchId);

	/** Cancel whatever one definition is waiting on, in whichever batch it is. Same settling as Cancel Batch. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool CancelDefinition(const FString& AssetPath);

	/** Hide a take from the list, or bring it back. Takes are never deleted: some cannot be made again. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Pipeline")
	bool HideTake(const FString& AssetPath, const FString& MotionId, bool bHidden);

	/**
	 * What is running now, one row per definition, for a job strip. Includes a runner being started
	 * and a clip being imported, not only generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	TArray<FMotionActivity> GetActivities() const;

	// ---------------------------------------------------------------------------------------------
	// The request, resolved once for everyone
	// ---------------------------------------------------------------------------------------------

	/**
	 * Exactly what Generate would send for a definition, what it would cost, and what would stop it.
	 *
	 * The one resolver: readiness, the cost line, the "will be sent" block, the confirmation and the
	 * submission all read this, so the price shown is the price of the request that is sent.
	 * Costs nothing and sends nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FMotionResolvedRequest ResolveRequest(const FString& AssetPath) const;

	// ---------------------------------------------------------------------------------------------
	// Providers on a definition
	// ---------------------------------------------------------------------------------------------

	/**
	 * Move a definition to another provider, safely.
	 *
	 * Keeps each provider's own settings, so switching back gives them back. Keeps the character when
	 * it suits the new provider; otherwise picks the one this definition last used with it, or one
	 * prepared for it, and says which. Prompt, length and takes are left alone.
	 *
	 * @return One sentence saying what changed, for the window to show.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	FString SetDefinitionProvider(const FString& AssetPath, FName ProviderId);

	/** Called by a definition whose provider was edited in a details panel. Same rules as Set Definition Provider. */
	FString OnDefinitionProviderChanged(UMotionDef* Def);

	/**
	 * Motion Characters that suit a provider: prepared for it, or universal and passing its checks.
	 * Prepared ones first.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	TArray<FString> FindCharactersFor(FName ProviderId) const;

	/**
	 * Whether a character suits a provider, and why not when it does not.
	 *
	 * A character naming another provider does not suit, whatever its fields say - its rig, its
	 * uploaded id and its retargeter belong to that provider.
	 */
	bool DoesCharacterSuit(const UMotionCharacter* Character, FName ProviderId, FString* OutReason = nullptr, EMotionBlocker* OutBlocker = nullptr) const;

	/** The character a definition should use with a provider: remembered, current, from its takes, or prepared for it. */
	FString PickCharacterFor(const UMotionDef* Def, FName ProviderId) const;

	/**
	 * Make a character import clips straight onto its own skeleton: forget its provider rig and
	 * retargeter. The fix for a provider rig imported without a retargeter. The rig assets stay.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	bool ClearCharacterProviderRig(const FString& CharacterPath, FString& OutMessage);

	/**
	 * Make a Motion Character from a skeletal mesh: its skeleton becomes the target, the mesh the preview.
	 *
	 * The quickest route from nothing to a character a provider can generate for. Universal until a
	 * provider needs it prepared - uploading it to one makes it that provider's. A character of that
	 * name that already exists is returned rather than duplicated.
	 *
	 * @return Content path of the character, or empty with OutError set.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	FString CreateCharacterFromMesh(const FString& SkeletalMeshPath, FString& OutError);

	/** A definition's settings for a provider, as the flat option list. None means the definition's own provider. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	TArray<FMotionPipelineOption> GetPipelineOptions(const FString& AssetPath, FName ProviderId) const;

	/** Set one of a definition's provider settings by the key Get Pipeline Options reports. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	bool SetPipelineOption(const FString& AssetPath, const FString& Key, const FString& Value, FString& OutError);

	/**
	 * Save every definition still carrying settings from before providers declared their own, in the
	 * new form. Loading migrates in memory; this makes it permanent.
	 *
	 * @return How many definitions were resaved.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Authoring")
	int32 MigrateDefinitions();

	// ---------------------------------------------------------------------------------------------
	// Watching a take before choosing it
	// ---------------------------------------------------------------------------------------------

	/**
	 * Whether a take can be watched in the editor without spending anything, and why not.
	 *
	 * True for a take already imported, a take already on disk, and a take whose provider fetches for
	 * free. False where fetching bills - then the provider's own viewer is the free way to look.
	 */
	bool CanPreviewTake(const FString& AssetPath, const FString& MotionId, FString& OutWhyNot) const;

	/**
	 * Build a take into a transient animation on the definition's character, fetching it first where
	 * that is free. Nothing is saved and nothing enters the project; the clip lives only while watched.
	 *
	 * Completes on the game thread. Previews are cached per take, so watching again is instant.
	 */
	void PreviewTake(
		const FString& AssetPath,
		const FString& MotionId,
		TFunction<void(UAnimSequence* /*Clip*/, const FString& /*Error*/)> OnReady);

	/** The cached preview for a take, or null. */
	UAnimSequence* FindPreview(const FString& AssetPath, const FString& MotionId) const;

	/**
	 * The take's fetched file, if it is still on disk, or empty.
	 *
	 * The recorded path first; failing that, the same file name in the staging directory. A definition
	 * duplicated from another, or a project moved to another folder or machine, keeps its takes' old
	 * absolute paths while the files themselves sit in this project's staging directory.
	 */
	static FString FindTakeFile(const FMotionCandidate& Take);

	/**
	 * What in the project uses a definition's clip - montages, sequences, dialogue - so replacing it can
	 * be asked about by name.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	TArray<FString> GetClipUsers(const FString& AssetPath) const;

	// ---------------------------------------------------------------------------------------------
	// Observation
	// ---------------------------------------------------------------------------------------------

	/**
	 * Could this definition be generated right now, and if not, what is missing?
	 *
	 * The same checks submission makes, asked before anything is spent. A window that offers
	 * Generate to a definition with no character is lying about what the button does, and finding
	 * out afterwards - from an error written into the asset - is the wrong moment to learn it.
	 *
	 * Cheap: no network, no provider call beyond the caps it already caches.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FMotionReadiness CheckReadiness(const FString& AssetPath) const;

	/** Per-definition status, takes and errors. Empty array means every definition. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	TArray<FMotionDefinitionStatus> GetStatus(const TArray<FString>& AssetPaths) const;

	/** How far a batch has got. Check bTracked before reading the counts. */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Status")
	FMotionBatchStatus GetBatchStatus(const FString& BatchId) const;

	/**
	 * What fetching these definitions' takes would cost, priced by the provider that made each one.
	 *
	 * Fetching only. Takes already on disk cost nothing and are not counted. What generating again
	 * would cost is Resolve Request's answer, or Estimate Generation Cost's.
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
	 * Each installed provider's own setup, measured: keys, access grants, the machine it runs on.
	 * None for every provider.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Providers")
	TArray<FMotionSetupStepInfo> GetSetupSteps(FName ProviderId) const;

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

	// ---------------------------------------------------------------------------------------------
	// Provider state, and telling people it moved
	// ---------------------------------------------------------------------------------------------

	/**
	 * A provider's readiness changed - a container started, a pod was released, a key was set.
	 *
	 * Exists because caps are cached and nothing was announcing when the cache went stale. A window
	 * showing "the runner is not up" a minute after somebody started it is not a display bug; it is
	 * a missing signal, and every surface that draws readiness needs this one.
	 *
	 * Broadcast on the game thread. The argument is the provider whose state moved.
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMotionProviderStateChanged, FName /*ProviderId*/);
	FOnMotionProviderStateChanged& OnProviderStateChanged() { return ProviderStateChanged; }

	/**
	 * Say a provider's state moved, so anything drawing it can catch up.
	 *
	 * Called by whatever did the moving - the provider itself after a health check, or an add-on
	 * after starting a container. Cheap, and safe to call when nothing actually changed.
	 */
	void NotifyProviderStateChanged(FName ProviderId);

	/**
	 * Ask a provider to re-read its own readiness, and announce the result.
	 *
	 * The polite form for a panel that wants to be current without knowing what the provider has to
	 * do to find out. Providers with constant caps complete immediately and announce nothing new.
	 */
	UFUNCTION(BlueprintCallable, Category = "MotionForge|Providers")
	void RefreshProviderState(FName ProviderId);

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

	/** None becomes the project default, decided in one place (FMotionForgeModule::ResolveDefaultProviderId). */
	static FName ResolveProviderId(FName ProviderId);

	/**
	 * The resolver. Fills the agent-facing answer and, when asked, the per-take requests that are
	 * actually submitted - so the two cannot describe different things.
	 *
	 * @param bForSubmit Also run the provider's PrepareRequest - the expensive step, such as reading
	 *        poses off a timeline - and pick seeds for takes that asked for a random one.
	 */
	bool ResolveInternal(
		UMotionDef* Def,
		FMotionResolvedRequest& Out,
		TArray<FMotionSubmitRequest>* OutRequests,
		bool bForSubmit,
		TSharedPtr<IMotionProvider>* OutProvider = nullptr,
		UMotionCharacter** OutCharacter = nullptr) const;

	/** Price a resolved request, in the provider's own billing unit. */
	static void PriceRequest(const IMotionProvider& Provider, FMotionResolvedRequest& Resolved);

	/** Shared implementation behind Generate and RunFullPipeline. */
	FString StartGeneration(const TArray<FString>& AssetPaths, EMotionPipelineMode Mode);

	/** Submit one resolved definition into a batch. The provider is ready by the time this runs. */
	void SubmitDefinition(FMotionBatch& Batch, UMotionDef* Def);

	/**
	 * Settle a definition whose waiting stopped - cancelled, or its provider went away - so it can
	 * generate again: to review when any take finished, to Failed with the reason when none did.
	 */
	void SettleDefinition(UMotionDef* Def, const FString& Reason);

	// The job strip's rows. Set at every step, cleared when the definition settles.
	void SetActivity(const FString& AssetPath, const FString& Doing, bool bCanCancel);
	void ClearActivity(const FString& AssetPath);

	/** The provider and character a take should be downloaded and imported through - the take's own. */
	bool ResolveTakeRoute(
		UMotionDef* Def,
		const FMotionCandidate& Take,
		TSharedPtr<IMotionProvider>& OutProvider,
		UMotionCharacter*& OutCharacter,
		FString& OutError) const;

	/** Poll every in-flight job. Driven by the ticker. */
	bool Tick(float DeltaTime);

	void OnJobFinished(FMotionBatch& Batch, const FMotionJobTracking& Job, const FMotionJobResult& JobResult);

	/** Called once every job for a definition has settled. */
	void OnDefinitionGenerated(FMotionBatch& Batch, UMotionDef* Def);

	/** Fetch, normalise and import the selected candidate. */
	void ProcessSelected(UMotionDef* Def);

	/** Normalise and import a take's file already on disk, through the take's own provider. */
	void NormalizeAndImport(UMotionDef* Def, const FString& MotionId, const FString& RawPath);

	/** Write an asset's package to disk, if anything dirtied it. */
	static void SaveAsset(UObject* Asset);

public:

	/**
	 * Put the finished clip on an animation track in the definition's prompt sequence.
	 *
	 * So that the thing you asked for and the thing you got are on adjacent rows: the beats above, the
	 * animation below, and any beat that came back wrong is one glance rather than one export.
	 *
	 * Only ever touches its own animation track, which it replaces. Silent when there is no prompt
	 * sequence, no binding, or no clip - none of those is a fault, and an import must not fail over
	 * where its result was displayed.
	 */
	static void PlaceTakeOnPromptSequence(UMotionDef* Def, UAnimSequence* Sequence);

	/**
	 * The same, against a sequence given rather than looked up.
	 *
	 * The lookup goes through `Def->Control.ConstraintSequence`, which is written by LinkDefinition -
	 * so calling the form above during creation, before the link is made, finds nothing and returns
	 * having done nothing. That is precisely the bug that left every freshly created prompt sequence
	 * with an empty animation row. Anything holding the sequence already should call this.
	 */
	static void PlaceTakeOnSequence(class ULevelSequence* Sequence, UAnimSequence* Clip);

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
	 * The same retarget, frame by frame into a clip that lives only in memory - for watching a take
	 * before it is chosen. The same retargeter and meshes the import uses, so what plays is what the
	 * import would produce; no asset, no package, no dialog.
	 */
	UAnimSequence* RetargetForPreview(
		UMotionDef* Def,
		UMotionCharacter* Character,
		UAnimSequence* SourceSequence,
		FString& OutError);

	/**
	 * Add one provider's share to an estimate, in that provider's own unit.
	 *
	 * Per provider rather than one project-wide plan, because a selection can span a local runner, a
	 * rented pod and a paid service, and each bills - or does not - in its own way. The old single plan
	 * priced a rented Kimodo pod at Uthana's per-second rate.
	 */
	static void AccumulateCost(
		FMotionCostEstimate& Estimate,
		const FMotionBilling& Billing,
		const FString& ProviderName,
		int32 Takes,
		float GeneratedSeconds,
		float DownloadSeconds);

	/** Write the summary sentence once every provider's share is in. */
	static void FinishCost(FMotionCostEstimate& Estimate, const TArray<FString>& Lines);

	/** Batch ids are readable rather than GUIDs, because a human reads them in logs. */
	static FString MakeBatchId();

	/** The rate a provider generates at, which is the only rate a clip should be handled at. */
	static int32 ResolveFrameRate(const TSharedPtr<IMotionProvider>& Provider);

	TMap<FString, FMotionBatch> Batches;

	/** One row per definition with work in flight. */
	TMap<FString, FMotionActivity> Activities;

	/** Transient previews of takes, by definition path and motion id. Kept alive here, never saved. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UAnimSequence>> Previews;

	/** Previews being fetched, so a second request waits for the first instead of fetching twice. */
	TMap<FString, TArray<TFunction<void(UAnimSequence*, const FString&)>>> PreviewWaiters;

	/** The stranded-definition sweep runs once, on the first tick. See Tick for why not in Initialize. */
	bool bSweptStrandedDefinitions = false;

	FOnMotionProviderStateChanged ProviderStateChanged;

	FTSTicker::FDelegateHandle TickHandle;
	double LastPollTime = 0.0;
};
