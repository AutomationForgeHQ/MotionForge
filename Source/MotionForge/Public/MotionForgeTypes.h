// Shared vocabulary for the generation pipeline.

#pragma once

#include "CoreMinimal.h"
#include "MotionControl.h"
#include "MotionPromptSequence.h"
#include "MotionForgeTypes.generated.h"

/**
 * Where a motion definition has got to.
 *
 * The order matters - the pipeline only ever moves forward through these, and every operation checks
 * the current state before acting so that re-running a command costs nothing. An agent that retries
 * must not be able to pay for the same generation twice.
 */
UENUM(BlueprintType)
enum class EMotionDefStatus : uint8
{
	/** Authored but never submitted. */
	Draft			UMETA(DisplayName = "Draft"),

	/** Jobs are in flight with the provider. */
	Generating		UMETA(DisplayName = "Generating"),

	/** Candidates exist and one needs choosing. Human-in-the-loop mode parks here. */
	AwaitingReview	UMETA(DisplayName = "Awaiting Review"),

	/** A candidate is chosen and its file is being fetched. */
	Downloading		UMETA(DisplayName = "Downloading"),

	/** Downloaded, being normalised and imported. */
	Processing		UMETA(DisplayName = "Processing"),

	/** An animation sequence exists in the project. The end of this plugin's job. */
	Ready			UMETA(DisplayName = "Ready"),

	/** Something went wrong - see LastError. Re-running is safe. */
	Failed			UMETA(DisplayName = "Failed")
};

/** How far a batch runs before it wants a human. */
UENUM(BlueprintType)
enum class EMotionPipelineMode : uint8
{
	/**
	 * Generate, download every candidate, normalise and import without stopping.
	 *
	 * Spends unattended under either billing model - generated seconds on pay-as-you-go, downloaded
	 * seconds on a subscription. Always price it first.
	 */
	Automatic			UMETA(DisplayName = "Automatic"),

	/**
	 * Generate and stop at AwaitingReview so someone can look at the candidates and pick.
	 *
	 * Only saves money where downloads are what bill, i.e. a subscription. On pay-as-you-go the
	 * spend has already happened by the time this mode stops, so review costs nothing and saves
	 * nothing - it is still worth doing to pick the best take, just not as a cost control.
	 */
	HumanInTheLoop		UMETA(DisplayName = "Human In The Loop")
};

/** Provider-side job state, normalised across providers. */
UENUM(BlueprintType)
enum class EMotionJobStatus : uint8
{
	Pending		UMETA(DisplayName = "Pending"),
	Running		UMETA(DisplayName = "Running"),
	Finished	UMETA(DisplayName = "Finished"),
	Failed		UMETA(DisplayName = "Failed")
};

/**
 * One generated take.
 *
 * MotionId is the only durable handle. Several providers - Uthana's v3.0 among them - expose no seed,
 * so a generation cannot be reproduced: the motion lives on their server and this id is the only way
 * back to it. Candidates are therefore never pruned automatically.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionCandidate
{
	GENERATED_BODY()

	/** Provider job that produced this. Kept for diagnostics after the job itself expires. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString JobId;

	/** Provider-side motion id. Losing this loses the take permanently. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString MotionId;

	/** Which of the requested variants this was. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 VariantIndex = 0;

	/** Where a human can watch it without spending a download. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString ViewerUrl;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	EMotionJobStatus Status = EMotionJobStatus::Pending;

	/** True once the file has been fetched to the staging directory. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	bool bDownloaded = false;

	/** Absolute path of the raw file on disk, once downloaded. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString LocalRawPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FDateTime GeneratedAt;

	/** Provider error text when Status is Failed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString Error;

	// ---------------------------------------------------------------------------------------------
	// What made this take. Recorded at submission, so a take can be judged, imported and repeated
	// without consulting the definition as it is now - which may have moved to another provider,
	// another character or another prompt since.
	// ---------------------------------------------------------------------------------------------

	/** Numbered across every generation of the definition, so "Take 7" never repeats. Zero on takes made before numbering. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 TakeNumber = 0;

	/** The provider that made it. Download and import go through this one, not the definition's current one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FName ProviderId;

	/** The model id that was sent. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString ModelId;

	/** The Motion Character it was generated for. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FSoftObjectPath Character;

	/** The seed actually sent, variant walk included. -1 when the provider takes no seed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	int32 Seed = -1;

	/** Seconds that were asked for. What a per-second provider bills. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	float LengthSeconds = 0.f;

	/** The prompt exactly as it was sent, beats joined. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString PromptSent;

	/** The settings that went with it, as the provider names them: "seed=812 diffusion_steps=100". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString SettingsSent;

	/**
	 * A fingerprint of everything that decides what comes back - prompt, beats, character, provider,
	 * model and settings. When the definition's current fingerprint differs, the take is stale: it was
	 * made from something the definition no longer asks for.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString RecipeHash;

	/** What it was estimated to cost when it was submitted. Zero where generation is free. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	float EstimatedCost = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	FString Currency;

	/**
	 * Put out of sight rather than deleted.
	 *
	 * A take cannot always be made again - a provider with no seed never repeats one - so the list only
	 * ever hides, and Show Hidden brings a take back.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	bool bHidden = false;

	/** Past the job timeout and still being waited on, less often. Not a failure. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Candidate")
	bool bLate = false;

	bool IsValidCandidate() const { return !MotionId.IsEmpty(); }

	/** Finished, with a motion to fetch. */
	bool IsUsable() const { return Status == EMotionJobStatus::Finished && IsValidCandidate(); }

	/** A name a person can read: "Take 7", or "Take 3b" for a take that predates numbering. */
	FString GetLabel() const;
};

/**
 * Everything needed to author a motion definition without touching the editor UI.
 *
 * This is the shape scripts and agents build. Empty optional fields fall back to plugin settings so
 * a caller can pass a prompt and nothing else.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionDefSpec
{
	GENERATED_BODY()

	/** Asset name to create, e.g. "MD_PressPanelChest". Sanitised before use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString AssetName;

	/** Empty leaves an existing definition's prompt as it is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec", meta = (MultiLine = true))
	FString Prompt;

	/**
	 * Seconds for the whole clip. Clamped to whatever the chosen provider and model allow.
	 *
	 * Zero means "not given": an update leaves the definition's length alone, and a new definition
	 * gets five seconds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec", meta = (ClampMin = 0))
	int32 Length = 0;

	/**
	 * How many takes to generate.
	 *
	 * A spending decision wherever generated seconds are what bill: each variant is another
	 * Length seconds charged at submission, kept or discarded. Free on a subscription.
	 *
	 * Zero means "not given": an update leaves the definition's count alone, and a new definition gets
	 * **one**, so a caller that omits it cannot be charged a multiple it did not ask for. Ask for more
	 * only after reading the provider's billing - Preview Motion Request and Estimate Generation Cost
	 * both answer before anything is submitted.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec", meta = (ClampMin = 0, ClampMax = 16))
	int32 Variants = 0;

	/**
	 * Deprecated: set the provider option `rewrite_prompt` in Pipeline Options instead.
	 *
	 * Still honoured when false, for callers written before providers declared their own options.
	 * True is the default and changes nothing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	bool bRewritePrompt = true;

	/** Content path of the UMotionCharacter to generate for. Empty keeps the current one, or the default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString CharacterAssetPath;

	/** Empty uses the settings default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec",
		meta = (GetOptions = "/Script/MotionForge.MotionForgeSettings.GetProviderOptions"))
	FName ProviderId;

	/**
	 * The provider's model id. Empty keeps the pipeline's current model.
	 *
	 * Written into the provider's pipeline as its `model` option. A model the provider does not list
	 * is refused rather than stored, because a model id is a provider's private vocabulary - Uthana's
	 * name sent to Kimodo fails at the far end with nothing useful said.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString ModelId;

	/**
	 * The provider's own settings, by the names List Pipeline Options reports: for Kimodo
	 * `seed`, `diffusion_steps`, `cfg_type`, `cfg_text`, `cfg_constraint`, `postprocess`,
	 * `split_prompt_into_beats`; for Uthana `rewrite_prompt`. Values as text: "812", "true", "separated".
	 *
	 * Only the keys given are changed, so an update can set one setting without restating the others.
	 * A key the provider does not declare is refused and named.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	TMap<FString, FString> PipelineOptions;

	/**
	 * Constraints, beat durations and the prompt timeline.
	 *
	 * The authored half is applied field by field when it is filled in. Its sampler fields - seed,
	 * steps, guidance, post-process, splitting - are still honoured for callers written before
	 * Pipeline Options, and are written into the provider's pipeline.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FMotionControl Control;

	/**
	 * Seconds to keep, as [start, end]. Zero-length means "not given" and leaves the trim alone.
	 *
	 * Models with a minimum duration pad the action out to fill it, so most clips need topping and
	 * tailing to land on the motion you actually asked for. Set Clear Trim to remove one.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FVector2D TrimWindow = FVector2D::ZeroVector;

	/** Remove any trim, so the whole clip is kept. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	bool bClearTrim = false;
};

/** What a batch is doing, as returned by the status calls. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionBatchStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	FString BatchId;

	/**
	 * False when the id is not being tracked.
	 *
	 * A batch is dropped once every job settles, so an untracked id means either "finished a while
	 * ago" or "never existed" - the two are indistinguishable from here. Finished is reported true
	 * in that case, and per-definition status is the thing to read next.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	bool bTracked = false;

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	EMotionPipelineMode Mode = EMotionPipelineMode::HumanInTheLoop;

	/** Provider jobs in this batch. One definition contributes one job per requested variant. */
	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	int32 JobsTotal = 0;

	/** Jobs that have reached a terminal state, successfully or otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	int32 JobsSettled = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	bool bFinished = false;

	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	bool bCancelled = false;

	/** Asset paths this batch covers, so a caller can follow up per definition. */
	UPROPERTY(BlueprintReadOnly, Category = "Batch")
	TArray<FString> DefinitionPaths;
};

/** One generated take, reduced to what a caller needs to choose between them. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionTakeInfo
{
	GENERATED_BODY()

	/** The durable handle. Pass this to SelectTake. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FString MotionId;

	UPROPERTY(BlueprintReadOnly, Category = "Take")
	int32 Variant = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Take")
	EMotionJobStatus Status = EMotionJobStatus::Pending;

	/** Where this take can be watched without spending a download. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FString ViewerUrl;

	/** True while the fetched file is on disk. Importing it or watching it costs nothing and needs no provider. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	bool bDownloaded = false;

	/** Provider error text when Status is Failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FString Error;

	/** "Take 7". Numbered across every generation of the definition, so it never repeats. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FString Label;

	/** The provider and model that made it, which may differ from the definition's current ones. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FName ProviderId;

	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FString ModelId;

	/** The seed that was sent, or -1. With the same prompt, model and settings it makes this take again. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	int32 Seed = -1;

	UPROPERTY(BlueprintReadOnly, Category = "Take")
	float LengthSeconds = 0.f;

	/** The imported animation came from this take. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	bool bInGame = false;

	/** Made from a prompt, character, model or settings the definition no longer asks for. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	bool bStale = false;

	/** Hidden from the take list. Hide Take brings it back with Hidden false. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	bool bHidden = false;

	/** Estimated cost at submission. Zero where generation is free. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	float EstimatedCost = 0.f;
};

/** A definition and everything the pipeline knows about it. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionDefinitionStatus
{
	GENERATED_BODY()

	/** Content path, and the handle every other call takes. */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	EMotionDefStatus Status = EMotionDefStatus::Draft;

	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FString Prompt;

	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	int32 Length = 0;

	/** How many takes a generation would produce, and therefore what one would cost. */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	int32 Variants = 1;

	/**
	 * The provider this definition will actually generate on, resolved.
	 *
	 * Never None on a working project: a definition naming no provider uses the project's default,
	 * and reporting the blank would hide which one is about to be billed.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FName ProviderId;

	/**
	 * True when the provider above was inherited rather than chosen on this definition.
	 *
	 * The two are different facts and a library that draws them alike is how a definition made for
	 * one provider quietly generates on another - changing the project default silently moves every
	 * inherited definition with it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	bool bProviderInherited = false;

	/** Empty until a take is chosen. */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FString SelectedMotionId;

	/** Why the last operation failed. Empty when the last one succeeded. */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FString LastError;

	/** The imported animation, once Status is Ready. Empty before that. */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	FString ImportedSequencePath;

	/**
	 * The path above names an asset that is no longer in the project.
	 *
	 * A definition still reporting Ready whose clip was deleted, moved or never saved. Nothing else
	 * catches it - the status is stored on the definition and stays true to what the pipeline did,
	 * so only asking the asset registry can tell you the result is gone.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	bool bImportedSequenceMissing = false;

	/** Every take ever generated for this definition. Never pruned. */
	UPROPERTY(BlueprintReadOnly, Category = "Definition")
	TArray<FMotionTakeInfo> Takes;
};

/**
 * What a provider actually charges for.
 *
 * This is the single most important setting in the plugin, because the two models invert the
 * correct workflow and getting it backwards costs real money:
 *
 *   PayPerGeneratedSecond   Every take is billed the moment it is generated, kept or discarded.
 *                           Downloads are free. Generate few variants; download all of them.
 *                           Reviewing before downloading saves nothing - the money is already spent.
 *
 *   PayPerDownloadedSecond  Generation is unmetered and only fetching is billed. Generate
 *                           generously, review, and download only the keeper.
 *
 * Uthana bills the first way on pay-as-you-go and the second way on a subscription, for the same
 * models at the same quality. Only the account tells you which, so it cannot be inferred.
 */
UENUM(BlueprintType)
enum class EMotionBillingModel : uint8
{
	/** Pay-as-you-go. Every generated second is billed; downloads are free. */
	PayPerGeneratedSecond	UMETA(DisplayName = "Per Generated Second (pay as you go)"),

	/** Subscription. Generation is free; every downloaded second comes out of a quota. */
	PayPerDownloadedSecond	UMETA(DisplayName = "Per Downloaded Second (subscription)")
};

/**
 * What a provider charges for, as a unit. Declared by the provider, never assumed by the core.
 *
 * Each has its own right workflow, which is why the cost line says which one applies rather than
 * printing a number: per generated second means few takes and download them all; per downloaded
 * second means many takes and import only the keeper; per hour means the machine bills whether
 * anything is generating or not.
 */
UENUM(BlueprintType)
enum class EMotionBillingUnit : uint8
{
	/** Nothing is billed by the provider. A runner on this machine, or one somebody else pays for. */
	Free					UMETA(DisplayName = "Free"),

	/** Every take is billed for the seconds asked for, when it is submitted, kept or not. */
	PerGeneratedSecond		UMETA(DisplayName = "Per generated second"),

	/** Generation is free; fetching a take bills the seconds fetched. */
	PerDownloadedSecond		UMETA(DisplayName = "Per downloaded second"),

	/** A machine rented by the hour. It bills while it runs, generating or not. */
	PerHour					UMETA(DisplayName = "Per hour")
};

/** How a provider bills right now. Returned by the provider; read by every cost line. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionBilling
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	EMotionBillingUnit Unit = EMotionBillingUnit::Free;

	/** Money per unit: per second for the per-second units, per hour for Per Hour. Zero when unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	float Rate = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	FString Currency = TEXT("USD");

	/**
	 * Money is being spent this minute whether or not anything generates - a rented machine that is up.
	 *
	 * The quiet cost. Nothing on screen looks busy, and the invoice still grows.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	bool bBillingNow = false;

	/** The plan, in the provider's words: "pay as you go", "subscription", "rented GPU". */
	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	FString PlanName;

	/** One sentence a cost line can show as it is: "free, runs on this machine". */
	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	FString Summary;

	/**
	 * Where Rate comes from, when it is not a figure the person entered: "Uthana's published price".
	 * Empty when it is theirs. Cost lines show it beside the rate.
	 *
	 * **A price nobody entered must not read as theirs.** A provider that ships a list price as its
	 * default otherwise states it in every confirmation exactly as it would a rate the person typed.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	FString RateNote;

	/** A take can be fetched to watch in the editor without spending anything. */
	UPROPERTY(BlueprintReadOnly, Category = "Billing")
	bool bFetchIsFree = true;
};

/** One model a provider offers, and the lengths it can make. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionModelInfo
{
	GENERATED_BODY()

	/** What is sent. The provider's own spelling. */
	UPROPERTY(BlueprintReadOnly, Category = "Model")
	FString Id;

	/** A short note for the picker: what it is good at, or what licence it carries. */
	UPROPERTY(BlueprintReadOnly, Category = "Model")
	FString Description;

	/** Shortest clip, in seconds. A model with a floor pads a shorter action out to fill it. */
	UPROPERTY(BlueprintReadOnly, Category = "Model")
	float MinSeconds = 1.f;

	/** Longest clip, in seconds, across all of its beats. */
	UPROPERTY(BlueprintReadOnly, Category = "Model")
	float MaxSeconds = 10.f;

	UPROPERTY(BlueprintReadOnly, Category = "Model")
	bool bDefault = false;
};

/**
 * Whether a provider cuts a prompt into beats, and the limits that follow from it.
 *
 * Beat durations mean something only where this says the provider splits. Everywhere else the whole
 * prompt is one generation of one length, and a beat total would be a number nobody asked for.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionPromptSplitting
{
	GENERATED_BODY()

	/** Cuts the prompt at every full stop and generates each piece in turn. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	bool bSplitsAtFullStops = false;

	/** Longest a single beat may be. The model's trained window; longer ones degrade. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	float MaxBeatSeconds = 10.f;

	/** Longest the whole clip may be, beats summed. */
	UPROPERTY(BlueprintReadOnly, Category = "Prompt")
	float MaxTotalSeconds = 10.f;
};

/** One setting a provider's pipeline declares, reflected for agents. */
UENUM(BlueprintType)
enum class EMotionOptionType : uint8
{
	Bool,
	Int,
	Float,
	Enum,
	Text
};

USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionPipelineOption
{
	GENERATED_BODY()

	/** The provider's own name for it - what Pipeline Options and Set Motion Pipeline Option take. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString Key;

	UPROPERTY(BlueprintReadOnly, Category = "Option")
	EMotionOptionType Type = EMotionOptionType::Text;

	/** What it does, what it costs, and what we measured. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString Tooltip;

	/** Its value on this definition, as text. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString Value;

	/** Its value on a fresh pipeline. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString DefaultValue;

	/** The choices, for an enum or a picked list. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	TArray<FString> AllowedValues;

	/** The lowest value it takes, as text. Empty when there is no lower bound. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString Min;

	/** The highest value it takes, as text. Empty when there is no upper bound. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString Max;

	/** Drawn under Advanced in the window. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	bool bAdvanced = false;

	/** Why it cannot be changed right now, when it cannot. */
	UPROPERTY(BlueprintReadOnly, Category = "Option")
	FString DisabledReason;
};

/** What an operation would cost, under whichever billing model is configured. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionCostEstimate
{
	GENERATED_BODY()

	/**
	 * Seconds that would be fetched.
	 *
	 * Takes already on disk are excluded, because fetching them again costs nothing.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	int32 DownloadSeconds = 0;

	/**
	 * Seconds that would be produced - length times variants, across the definitions asked about.
	 *
	 * This is the number that bills on pay-as-you-go, and it is spent whether or not a take is ever
	 * downloaded or kept.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	int32 GeneratedSeconds = 0;

	/** Whichever of the two above the configured billing model actually charges for. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	int32 BilledSeconds = 0;

	/** What the project is configured to be billed on. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	EMotionBillingModel BillingModel = EMotionBillingModel::PayPerGeneratedSecond;

	/** BilledSeconds times the configured rate. Zero when no rate has been set. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	float EstimatedCost = 0.f;

	/** Currency the rate is in, purely for display. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	FString Currency;

	/** How many clips that covers. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	int32 Clips = 0;

	/** True when only chosen takes were counted rather than every usable one. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	bool bSelectedOnly = false;

	/** The unit the money is in. Mixed selections report the most expensive unit involved. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	EMotionBillingUnit Unit = EMotionBillingUnit::Free;

	/**
	 * The whole answer in one sentence, for a cost line or a confirmation: "about $1.50: 3 takes x 5 s,
	 * billed when submitted, kept or not". Per provider, joined, when a selection spans several.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	FString Summary;

	/** Pressing the button spends money now. A person should be asked first. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	bool bSpendsMoney = false;

	/** A rented machine is billing by the hour right now, whatever this operation does. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	bool bHourlyBillingNow = false;

	/** The hourly rate of that machine, when there is one. */
	UPROPERTY(BlueprintReadOnly, Category = "Cost")
	float HourlyRate = 0.f;
};

/**
 * Where the pipeline writes, resolved.
 *
 * The root is the only thing configured; every folder below it is derived. That is deliberate - the
 * sort by kind is a property of the pipeline rather than a preference, so pointing output somewhere
 * else moves the whole structure intact instead of letting it flatten into one folder on the way.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionOutputPaths
{
	GENERATED_BODY()

	/** The configured root. Everything below is this plus one folder name. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Root;

	/** Motion definitions - the prompt and its settings, which is the reproducible half. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Definitions;

	/** Imported animations on the target skeleton. The usable results. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Takes;

	/** Intermediates on the generator's own rig - for diagnosis, never for playing. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString SourceTakes;

	/** Motion characters: how a provider's rig maps onto ours. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Characters;

	/** IK rigs and retargeters built for a provider. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Rigs;

	/** Prompt sequences - a definition's beats on a timeline. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Sequences;

	/** Montages and their recipes, including any the Narrative add-ons build. */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Montages;

	/**
	 * Empty on success. Set when a root was refused, saying why.
	 *
	 * A refused change leaves every path above reporting what is still configured, so a caller that
	 * ignores this field reads the truth rather than the request it thought it had made.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Output")
	FString Problem;
};

/**
 * What a provider can actually do.
 *
 * Exists because the pipeline used to guess. A global "target frame rate" setting is correct for
 * exactly one provider and silently wrong for every other - and a wrong frame rate produces a clip
 * that imports without a warning, logs cleanly, and is the wrong length. The same goes for billing,
 * seeds and prompt rewriting: every one of them is a per-provider fact that the layer above has no
 * business assuming.
 *
 * Providers fill this in once. Everything else - the UI, the cost estimator, the agent tools - reads
 * it rather than special-casing a provider by name.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionProviderCaps
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FName ProviderId;

	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FString DisplayName;

	/** Runs on this machine or a machine you control, rather than as a paid service. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bIsLocal = false;

	/** False means generation is free and no cost estimate is worth reporting. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bIsMetered = true;

	/** False means there is no API key to set and Has Credential is always true. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bNeedsCredential = true;

	/**
	 * A generation can be reproduced exactly from its recipe.
	 *
	 * Where this is true, raw files are a cache. Where it is false they are the only copy that will
	 * ever exist, and nothing may delete them.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bSupportsSeed = false;

	/** Honours FMotionControl::Constraints. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bSupportsConstraints = false;

	/** Honours the definition's Rewrite Prompt flag. Where false it is ignored, not an error. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bSupportsPromptRewrite = false;

	/** Can be given this project's own character to generate against. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bSupportsCharacterUpload = false;

	/**
	 * Generates on its own fixed rig, so clips have to be moved onto the project's skeleton.
	 *
	 * The opposite of uploading a character. Where this is true the bone mapping is the pipeline's
	 * problem, not the provider's.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	bool bRequiresRetarget = false;

	/**
	 * The rate this provider generates at.
	 *
	 * Asking a provider for a different rate is not resampling on most of them - it re-times, and a
	 * four second clip comes back as an eight second one that imports without complaint.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	int32 NativeFrameRate = 30;

	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	int32 MinLengthSeconds = 1;

	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	int32 MaxLengthSeconds = 10;

	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FString DefaultModelId;

	/** What a human must do before this provider will work, when it is not ready. Empty when it is. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FString SetupHint;

	/**
	 * Not ready, but it can get itself ready - a stopped runner it can start. Generate then starts it
	 * rather than refusing, and a window offers this label as its button.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FString PrepareLabel;

	/** The models it offers, with their lengths. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	TArray<FMotionModelInfo> Models;

	/** Whether it cuts prompts into beats, and the beat limit. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FMotionPromptSplitting PromptSplitting;

	/** What it charges for, right now. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FMotionBilling Billing;

	/** The constraint types it honours. Empty means it takes none, and constraint tools are not offered. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	TArray<EMotionConstraintType> ConstraintTypes;

	/** The settings class it declares, by path, so an agent can list its options. Empty when it has none. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FString PipelineClass;

	/** One line on what this provider is, for a person choosing between them. */
	UPROPERTY(BlueprintReadOnly, Category = "Provider")
	FString Tagline;
};

/**
 * The one reason a definition cannot be generated, when there is one.
 *
 * A kind rather than only a sentence, because each of these has a different next action and the
 * panel should offer *that* one - a missing key wants the Keys page, a runner that is down wants
 * the runner panel, and a missing character wants the field above.
 */
UENUM(BlueprintType)
enum class EMotionBlocker : uint8
{
	None				UMETA(DisplayName = "None"),
	NoProvider			UMETA(DisplayName = "No Provider"),
	NoCredential		UMETA(DisplayName = "No Credential"),

	/** Not ready, and a person has to act: install something, grant access, fix a broken image. */
	ProviderNotReady	UMETA(DisplayName = "Provider Not Ready"),

	NoCharacter			UMETA(DisplayName = "No Character"),
	CharacterUnusable	UMETA(DisplayName = "Character Unusable"),
	NoPrompt			UMETA(DisplayName = "No Prompt"),

	/** The character was prepared for a different provider. Pick one that suits this provider. */
	CharacterForOtherProvider	UMETA(DisplayName = "Character For Another Provider"),

	/** The character retargets through a rig and the retargeter is missing. Caught before spending. */
	RetargetIncomplete	UMETA(DisplayName = "Retarget Incomplete"),

	/** The provider's own settings, or the beats, would be refused. Problem says which and why. */
	InvalidRequest		UMETA(DisplayName = "Invalid Request"),

	/** Asks for more seconds than the provider can make. */
	LengthOutOfRange	UMETA(DisplayName = "Length Out Of Range"),

	/** Not ready, but it can start itself - Generate does that first. Not a refusal. */
	ProviderStartable	UMETA(DisplayName = "Provider Can Be Started"),

	/** Already generating, downloading or importing. */
	Busy				UMETA(DisplayName = "Busy")
};

/**
 * Whether a definition could be generated right now, asked before anything is spent.
 *
 * Everything here was already checked at submit time and reported as a failure afterwards, which
 * is the wrong moment: a window that offers Generate to a definition with no character is lying
 * about what the button will do. Same answer either way, because this asks the same code.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionReadiness
{
	GENERATED_BODY()

	/**
	 * Generate would do something useful now. True also when the provider must start first
	 * (Blocker is Provider Startable) - pressing Generate starts it, then submits.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	bool bCanGenerate = false;

	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	EMotionBlocker Blocker = EMotionBlocker::None;

	/** What is missing, in one sentence. Empty when nothing is. */
	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	FString Problem;

	/** Things worth knowing that do not stop a generation: a decimal point that will split a beat, poses a provider ignores. */
	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	TArray<FString> Warnings;

	/** The button that fixes the blocker, when one can: "Start Kimodo runner", "Open Keys". Empty otherwise. */
	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	FString FixLabel;
};

/**
 * Exactly what Generate would send, before anything is spent. The single source every surface reads:
 * readiness, the cost line, the "will be sent" block, the confirmation, and the submission itself.
 *
 * One resolver, because two drifted. An estimate that prices one request while another is submitted
 * is worse than no estimate - MeshForge once quoted fifteen credits for a job that cost thirty.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionResolvedRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString AssetPath;

	/** The provider it goes to, resolved. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FName ProviderId;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString ProviderDisplayName;

	/** The definition names no provider and follows the project default. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	bool bProviderInherited = false;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString ModelId;

	/** The prompt as it will be sent, beats joined. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString Prompt;

	/** The prompt is read off a timeline rather than the text field. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	bool bPromptFromTimeline = false;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString TimelinePath;

	/** The beats and their seconds. Empty where the provider does not split, or durations are left to it. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	TArray<FMotionPromptBeat> Beats;

	/** How many beats the provider will find in the prompt. One where it does not split. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	int32 BeatCount = 1;

	/** The provider cuts this prompt at its full stops. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	bool bSplitIntoBeats = false;

	/** Seconds that will be asked for. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	float LengthSeconds = 0.f;

	/** Why the length is not what the definition says, when it is not: clamped, or taken from the beats. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString LengthNote;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	int32 Variants = 1;

	/** Seeds per take, as they will be sent. -1 entries mean "chosen at submission, and recorded on the take". */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	TArray<int32> Seeds;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString CharacterPath;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString CharacterName;

	/** Where the motion lands: "direct onto SK_Mannequin", "retargeted from Kimodo's rig", "Uthana character 'Quinn'". */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString CharacterRoute;

	/** The provider's settings as they will be sent, "key=value". */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	TArray<FString> Settings;

	/** Where constraint poses come from and how many there are, in a sentence. Empty for a provider that takes none. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString Constraints;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FMotionCostEstimate Cost;

	/** Nothing is stopping it. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	bool bCanSubmit = false;

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FMotionReadiness Readiness;

	/** Fingerprint of everything that decides what comes back. A take whose hash differs is stale. */
	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FString RecipeHash;
};

/** What a definition is doing right now, for a job strip: one row per definition with work in flight. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionActivity
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	FString DefinitionName;

	/** What is happening now, in words: "Starting the Kimodo runner", "Generating 3 takes", "Importing Take 7". */
	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	FString Doing;

	/** Takes finished of takes asked for, while generating. */
	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	int32 Done = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	int32 Total = 0;

	/** When this step started, for the clock. */
	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	FDateTime StartedAt;

	/** Taking longer than the timeout. Still waiting; not a failure. */
	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	bool bLate = false;

	UPROPERTY(BlueprintReadOnly, Category = "Activity")
	bool bCanCancel = false;
};

/** One step of a provider's own setup, as the Get Started page draws it. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionSetupStepInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	FName ProviderId;

	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	FString Label;

	/** Done, Todo, Waiting, Blocked or Unknown. Unknown means it has not been measured - never assume either way. */
	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	FString State;

	/** What the state means, measured. */
	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	FString Detail;

	/** The button a person would press. Many are for a person only - a licence to accept, an app to install. */
	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	FString ActionLabel;

	/** Where the step happens outside the editor, when it does. */
	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	FString HelpUrl;

	UPROPERTY(BlueprintReadOnly, Category = "Setup")
	bool bOptional = false;
};

/** Whether a provider can be used, without revealing how. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionCredentialInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Credential")
	FName ProviderId;

	UPROPERTY(BlueprintReadOnly, Category = "Credential")
	FString DisplayName;

	/** False means generation will fail until a person sets the key on the Keys page or in Editor Preferences. */
	UPROPERTY(BlueprintReadOnly, Category = "Credential")
	bool bConfigured = false;

	/** Where the key is read from - the OS credential vault or an environment variable. */
	UPROPERTY(BlueprintReadOnly, Category = "Credential")
	FString Source;
};

/** Which rig a provider should give an uploaded character. */
UENUM(BlueprintType)
enum class EMotionRerigTarget : uint8
{
	/**
	 * Keep the rig the file already has.
	 *
	 * The right choice for a mesh exported from Unreal, which arrives already rigged: motion then
	 * comes back on exactly the bone names the project's skeleton uses, and imports bind without
	 * anything having to be guessed.
	 */
	KeepExisting	UMETA(DisplayName = "Keep Existing Rig"),

	/** Retarget onto the provider's UE5 mannequin skeleton. */
	UnrealEngine5	UMETA(DisplayName = "Unreal Engine 5"),

	/** Retarget onto the provider's Roblox R15 skeleton. */
	RobloxR15		UMETA(DisplayName = "Roblox R15")
};

/** How a provider should treat a character being uploaded. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionCharacterUploadOptions
{
	GENERATED_BODY()

	/**
	 * Let the provider rig the file when it arrives without a skeleton.
	 *
	 * A mesh exported from Unreal is already rigged, so this is normally a no-op. Harmless to leave
	 * on, and the safety net if someone uploads a raw sculpt.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upload")
	bool bAutoRig = true;

	/** Orient an auto-rigged character to face forward. Ignored when nothing was auto-rigged. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upload")
	bool bAutoRigFrontFacing = true;

	/** Include finger joints. Worth keeping for anything that handles objects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upload")
	bool bIncludeFingers = true;

	/**
	 * Whether to re-rig onto one of the provider's own skeletons.
	 *
	 * Leave at Keep Existing for meshes exported from this project. Re-rigging replaces the bone
	 * names motion comes back on, which is exactly what a UE-exported character does not want.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upload")
	EMotionRerigTarget RerigTarget = EMotionRerigTarget::KeepExisting;
};

/** A character the provider already holds. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionRemoteCharacter
{
	GENERATED_BODY()

	/** Paste into UMotionCharacter::ProviderCharacterId to use it. */
	UPROPERTY(BlueprintReadOnly, Category = "Character")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Character")
	FString Name;
};

/** What came back from uploading a character. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionCharacterUpload
{
	GENERATED_BODY()

	/** Written into the UMotionCharacter automatically; repeated here so a caller can see it. */
	UPROPERTY(BlueprintReadOnly, Category = "Upload")
	FString ProviderCharacterId;

	/** The name the provider recorded, which may differ from the one asked for. */
	UPROPERTY(BlueprintReadOnly, Category = "Upload")
	FString Name;

	/**
	 * Auto-rigger confidence, or zero when it did not run.
	 *
	 * A low number on a character that was supposed to arrive pre-rigged means the provider did not
	 * recognise the skeleton and rigged it itself - worth checking before generating a whole library
	 * against it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Upload")
	float AutoRigConfidence = 0.f;

	/** Absolute path of the file that was sent, also recorded on the character asset. */
	UPROPERTY(BlueprintReadOnly, Category = "Upload")
	FString UploadedFile;
};

/**
 * What came back from authoring and queueing a set of definitions in one call.
 *
 * Not to be confused with FMotionSubmitResult in IMotionProvider.h, which is one provider job's
 * outcome. This is the whole batch.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionBatchSubmission
{
	GENERATED_BODY()

	/** Definitions that were created or updated, in the order they were given. */
	UPROPERTY(BlueprintReadOnly, Category = "Submit")
	TArray<FString> AssetPaths;

	/** Empty when nothing was eligible to generate. */
	UPROPERTY(BlueprintReadOnly, Category = "Submit")
	FString BatchId;
};
