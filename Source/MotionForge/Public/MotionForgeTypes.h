// Shared vocabulary for the generation pipeline.

#pragma once

#include "CoreMinimal.h"
#include "MotionControl.h"
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

	bool IsValidCandidate() const { return !MotionId.IsEmpty(); }
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec", meta = (MultiLine = true))
	FString Prompt;

	/** Seconds. Clamped to whatever the chosen provider and model allow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	int32 Length = 5;

	/**
	 * How many takes to generate.
	 *
	 * A spending decision wherever generated seconds are what bill: each variant is another
	 * Length seconds charged at submission, kept or discarded. Free on a subscription.
	 *
	 * **Defaults to one**, so a caller that omits it cannot be charged a multiple it did not ask for.
	 * Ask for more only after reading the provider's billing model - EstimateGenerationCost answers
	 * before anything is submitted, and on an unmetered provider the answer is zero.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec", meta = (ClampMin = 1))
	int32 Variants = 1;

	/**
	 * Let the provider rewrite the prompt into its own phrasing before generating.
	 *
	 * Improves results on some models and flattens deliberate phrasing on others, so it is worth
	 * testing both ways per provider rather than assuming.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	bool bRewritePrompt = true;

	/** Content path of the UMotionCharacter to generate for. Empty uses the settings default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString CharacterAssetPath;

	/** Empty uses the settings default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec",
		meta = (GetOptions = "/Script/MotionForge.MotionForgeSettings.GetProviderOptions"))
	FName ProviderId;

	/** Empty uses the provider's default model. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FString ModelId;

	/**
	 * Seed, sampler settings and constraints. Leave alone to let the provider decide everything.
	 *
	 * Ignored field by field on providers that cannot honour it - check Get Provider Capabilities
	 * rather than assuming a seed took effect.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FMotionControl Control;

	/**
	 * Seconds to keep, as [start, end]. Zero-length means keep everything.
	 *
	 * Models with a minimum duration pad the action out to fill it, so most clips need topping and
	 * tailing to land on the motion you actually asked for.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spec")
	FVector2D TrimWindow = FVector2D::ZeroVector;
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

	/** True once fetched to the staging directory. Downloading it again costs nothing. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	bool bDownloaded = false;

	/** Provider error text when Status is Failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Take")
	FString Error;
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
	ProviderNotReady	UMETA(DisplayName = "Provider Not Ready"),
	NoCharacter			UMETA(DisplayName = "No Character"),
	CharacterUnusable	UMETA(DisplayName = "Character Unusable"),
	NoPrompt			UMETA(DisplayName = "No Prompt")
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

	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	bool bCanGenerate = false;

	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	EMotionBlocker Blocker = EMotionBlocker::None;

	/** What is missing, in one sentence. Empty when nothing is. */
	UPROPERTY(BlueprintReadOnly, Category = "Readiness")
	FString Problem;
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

	/** False means generation will fail until a human signs in through Project Settings. */
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
