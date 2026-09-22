// What any motion generation service has to be able to do.

#pragma once

#include "CoreMinimal.h"
#include "MotionForgeTypes.h"
#include "MotionControl.h"
#include "UObject/StructOnScope.h"

class USkeleton;
class UAnimSequence;
class UMotionCharacter;
class UMotionDef;
class UMotionPipeline;

/** One generation request. */
struct FMotionSubmitRequest
{
	FString Prompt;
	FString ModelId;
	FString ProviderCharacterId;

	/**
	 * Seconds for the whole clip. Whole seconds on providers that take an integer; Kimodo takes the
	 * float in LengthSeconds.
	 */
	int32   Length = 5;
	float   LengthSeconds = 5.f;

	bool    bRewritePrompt = true;

	/** Which of the requested takes this is. Carried through so results can be matched back. */
	int32   VariantIndex = 0;

	/** The definition this request is for, when there is one. Providers may read it; they must not write it. */
	const UMotionDef* Definition = nullptr;

	/**
	 * The skeleton the result will land on. May be null.
	 *
	 * Only providers that accept authored poses need it, and they need it badly: a pose in
	 * `FMotionControl` is expressed in the author's own bone names against their own rest pose, so
	 * converting it into a generator's joint rotations is impossible without the skeleton it was
	 * authored on. Everything else about a request is rig-independent by design; this is the one
	 * thing that cannot be.
	 */
	USkeleton* TargetSkeleton = nullptr;

	/**
	 * Seed, sampler settings and constraints.
	 *
	 * Providers honour whatever their capabilities advertise and ignore the rest. A provider that
	 * cannot seed does not fail a request that asks for one - it logs and generates anyway, because
	 * refusing would make one definition unusable across providers for no benefit.
	 */
	FMotionControl Control;
};

/** Outcome of submitting. */
struct FMotionSubmitResult
{
	bool    bSuccess = false;
	FString JobId;
	FString Error;
	int32   VariantIndex = 0;
};

/** Outcome of polling a job. */
struct FMotionJobResult
{
	EMotionJobStatus Status = EMotionJobStatus::Pending;
	FString MotionId;
	FString Error;
};

/** Outcome of uploading a character. */
struct FMotionCharacterUploadResult
{
	bool    bSuccess = false;

	/** What to put in UMotionCharacter::ProviderCharacterId. */
	FString CharacterId;

	/** The name the provider recorded, which may be sanitised from the one asked for. */
	FString Name;

	/**
	 * How sure the provider's auto-rigger is, where it ran. Zero when it did not.
	 *
	 * Low confidence is worth surfacing rather than swallowing: a badly guessed rig produces motion
	 * that imports cleanly and animates wrongly, which is the expensive kind of failure.
	 */
	float   AutoRigConfidence = 0.f;

	FString Error;
};

/** One character already held by the provider. */
struct FMotionProviderCharacter
{
	FString Id;
	FString Name;
};

/**
 * Turning a provider's own downloaded artifact into an animation.
 *
 * Only used by providers that answer true to HandlesImport - ones whose output is not an FBX and so
 * cannot go through the shared Blender-and-Interchange path. Everything the pipeline knows about
 * where the asset goes is here; how to read the file is the provider's business alone.
 */
struct FMotionArtifactImport
{
	/** The file DownloadMotion wrote. */
	FString AbsoluteArtifactPath;

	/** Content path to create in, e.g. "/Game/_Generated/Motion". */
	FString DestinationPackagePath;

	/** Asset name to create. Already sanitised. */
	FString AssetName;

	/** Where the animation has to end up. Never null. */
	USkeleton* TargetSkeleton = nullptr;

	/** Seconds to keep, as [start, end]. Zero-length keeps everything. */
	FVector2D TrimWindow = FVector2D::ZeroVector;

	/** Strip root translation so the clip plays in place. */
	bool bZeroRootTranslation = false;

	/**
	 * A clip to take the bones this generator does not drive from, instead of the reference pose.
	 *
	 * **This is the fingers.** A model that predicts thirty joints predicts no finger joints, so every
	 * bone it does not name keeps whatever the skeleton's reference pose has - one fixed hand shape,
	 * held for the entire clip. On a character with its arms at its sides that shape is what puts a
	 * thumb inside a thigh on every single frame, which is a defect no amount of correcting the *arm*
	 * can fix, because the arm is not what is wrong.
	 *
	 * Any hand-animated clip on the same skeleton will do; one frame of it is read. Null keeps the
	 * reference pose, which is the old behaviour.
	 */
	UAnimSequence* UntrackedPoseSource = nullptr;

	/** Which second of that clip to read. Its own frame zero unless somebody picked a better one. */
	float UntrackedPoseTime = 0.f;

	/**
	 * Build a preview that is never saved: a transient package under /Temp, no asset registry entry,
	 * nothing on disk. For watching a take before choosing it, so ten looks do not leave ten clips in
	 * the project.
	 */
	bool bTransient = false;
};

struct FMotionArtifactResult
{
	bool bSuccess = false;
	TSoftObjectPtr<UAnimSequence> Sequence;
	FString Error;
};

/**
 * One thing a provider needs done to a character before it can generate for it - upload it, build a
 * rig - offered as a button on the character without the core knowing what the step is.
 */
struct FMotionCharacterSetupAction
{
	/** The button: "Upload to Uthana", "Build Kimodo rig". */
	FText Label;

	/** What pressing it does, what it costs, how long it takes. */
	FText Tooltip;

	/** Already done for this character. The button then reads as a redo, not a step. */
	bool bDone = false;

	/** A step without which the provider cannot generate for this character. */
	bool bRequired = false;

	/** Done state in words: "Uploaded as 'Quinn' (id c0f2...)". */
	FText Status;

	/** Asks before running, with this sentence: a second upload creates a second character on the provider. */
	FText Confirmation;

	/**
	 * Settings the person may change before running it, drawn as a small form beside the button - the
	 * provider's own upload options, say. Run reads the same memory. Null when there is nothing to set.
	 */
	TSharedPtr<FStructOnScope> Options;

	/** Runs the step. Completes on the game thread with a sentence for the character window. */
	TFunction<void(TFunction<void(bool /*bSuccess*/, const FString& /*Message*/)>)> Run;
};

/** How far a provider's own setup has got, one step per row on the Get Started page. */
enum class EMotionSetupState : uint8
{
	Done,
	Todo,
	/** Waiting on something slow - a download, a review - that needs no action now. */
	Waiting,
	/** Cannot be started until an earlier step is done. */
	Blocked,
	/** Not measured yet. Never drawn as done or as broken. */
	Unknown
};

/** One setup step, stated by the provider and drawn by the core. */
struct FMotionSetupStep
{
	FText Label;

	/** What the state means, measured: "Docker Desktop 4.41 is running", "No token stored". */
	FText Detail;

	EMotionSetupState State = EMotionSetupState::Unknown;

	/** The button that moves this step on, when one exists. */
	FText ActionLabel;
	TFunction<void()> Action;

	/** A page to read or visit, when the step happens outside the editor: a licence to accept. */
	FString HelpUrl;
	FText HelpLabel;

	/** Optional steps are drawn quietly and never block the provider. */
	bool bOptional = false;
};

using FOnMotionSubmitComplete   = TFunction<void(const FMotionSubmitResult&)>;
using FOnMotionJobPolled        = TFunction<void(const FMotionJobResult&)>;
using FOnMotionDownloadComplete = TFunction<void(bool /*bSuccess*/, const FString& /*Error*/)>;
using FOnMotionTestComplete     = TFunction<void(bool /*bSuccess*/, const FString& /*Message*/)>;
using FOnMotionCharacterUploaded = TFunction<void(const FMotionCharacterUploadResult&)>;
using FOnMotionCharactersListed =
	TFunction<void(bool /*bSuccess*/, const TArray<FMotionProviderCharacter>& /*Characters*/, const FString& /*Error*/)>;

/**
 * A motion generation service.
 *
 * Deliberately narrow: submit, poll, download, and enough metadata to drive a UI. Anything specific
 * to one vendor - GraphQL versus REST, how a job id is shaped, which model names exist - stays behind
 * this line so the pipeline above never learns about it.
 *
 * Every call is asynchronous and completes on the game thread.
 */
class MOTIONFORGE_API IMotionProvider
{
public:

	virtual ~IMotionProvider() = default;

	/** Stable identifier used in settings and motion definitions, e.g. "Uthana". */
	virtual FName GetProviderId() const = 0;

	/** Human readable name for UI. */
	virtual FString GetDisplayName() const = 0;

	/**
	 * Which plugin this provider ships in, for the shared Keys page.
	 *
	 * Not MotionForge, for anything that actually has a key: the registration lives in the core,
	 * because that is where providers announce themselves - so without this every key would be
	 * attributed to MotionForge, which is a core and spends nothing. Somebody uninstalling the
	 * plugin that owns a key would go looking for the wrong one.
	 *
	 * Defaults to MotionForge so a provider that forgets is merely unhelpful rather than wrong: a
	 * provider compiled into the core genuinely would belong to it.
	 */
	virtual FText GetOwningPluginName() const { return NSLOCTEXT("MotionForge", "OwnerCore", "MotionForge"); }

	/**
	 * What this provider can do, so nothing above has to special-case it by name.
	 *
	 * Deliberately not optional. Every field here was once an assumption baked into the pipeline for
	 * one provider, and each of them was wrong for the second one.
	 */
	virtual FMotionProviderCaps GetCaps() const = 0;

	/**
	 * Extension the artifacts this provider downloads arrive with, without the dot.
	 *
	 * Only affects the name of the staged file. The pipeline never parses it.
	 */
	virtual FString GetArtifactExtension() const { return TEXT("fbx"); }

	/**
	 * True when this provider turns its own downloads into animations rather than handing over an
	 * FBX for the shared import path.
	 *
	 * The right answer for anything that is not an FBX. A provider handing back raw rotations knows
	 * exactly what they mean; going through a file format in the middle only adds a place for a
	 * coordinate convention to be quietly lost.
	 */
	virtual bool HandlesImport() const { return false; }

	/** Build an animation from a downloaded artifact. Only called when HandlesImport is true. */
	virtual FMotionArtifactResult ImportArtifact(const FMotionArtifactImport& Request)
	{
		FMotionArtifactResult Result;
		Result.Error = TEXT("This provider does not import its own artifacts.");
		return Result;
	}

	/** Service name this provider's secret is stored under in the credential vault. */
	virtual FString GetCredentialServiceName() const = 0;

	/**
	 * Where a person signs up for a key, shown beside the field that asks for one.
	 * Optional — a provider that needs no account, or has no sign-up page, leaves it empty.
	 */
	virtual FString GetCredentialHelpUrl() const { return FString(); }

	/**
	 * What this provider's key is for, in one line, where the generic sentence would mislead.
	 * Empty means "motion generation through this provider", which is true of the hosted ones.
	 */
	virtual FText GetCredentialPurpose() const { return FText(); }

	/**
	 * True when the provider works without a key — a runner on loopback, for instance, where
	 * demanding a token would be ceremony. Says so on the Keys page instead of implying a fault.
	 */
	virtual bool IsCredentialOptional() const { return false; }

	/**
	 * The name of the place a person sets this provider up, when it has one beyond a key.
	 *
	 * Empty for a hosted service, where an API key is the whole of it. A provider that runs on
	 * hardware somebody has to manage - a container to start, a GPU to rent - has more to say, and
	 * this is how it offers it without MotionForge knowing what a container is.
	 *
	 * MotionForge asks the provider rather than looking for a plugin by name, so a provider added
	 * later gets the same button with no change here. The one rule is the family's: an add-on may
	 * be absent, so an empty answer must remain perfectly ordinary.
	 */
	virtual FText GetSetupSurfaceLabel() const { return FText(); }

	/**
	 * Open that place. Called only when the label above is non-empty.
	 *
	 * The provider does the opening, because only it knows what it registered - a tab, a window, a
	 * settings page. Nothing above needs to learn.
	 */
	virtual void OpenSetupSurface() const {}

	/**
	 * Re-read whatever this provider caches about its own readiness, then call back.
	 *
	 * `GetCaps` is answered from a cache, because it is called every frame by anything drawing a
	 * panel. That cache is why a runner somebody started a minute ago could still be reported as
	 * down: nothing had asked it since. Anything that shows readiness should call this on a slow
	 * timer rather than trusting what it was told when it opened.
	 *
	 * Default is a no-op that completes immediately, which is correct for a provider whose caps are
	 * constants. Keep the work small - a single health call, not a shell out.
	 */
	virtual void RefreshState(TFunction<void()> OnDone) { OnDone(); }

	/** Model used when a definition names none. */
	virtual FString GetDefaultModelId() const = 0;

	// ---------------------------------------------------------------------------------------------
	// What this provider offers, declared so nothing above has to know it by name.
	// ---------------------------------------------------------------------------------------------

	/**
	 * The settings class a definition keeps for this provider, a UMotionPipeline subclass.
	 *
	 * Null for a provider with nothing to set beyond the prompt, length and takes every provider shares.
	 */
	virtual UClass* GetPipelineClass() const { return nullptr; }

	/** The models it offers. The default is the one model GetDefaultModelId names, with GetLengthRange's lengths. */
	virtual TArray<FMotionModelInfo> GetModels() const;

	/** Whether it cuts a prompt into beats, and the per-beat and whole-clip limits. Default: it does not. */
	virtual FMotionPromptSplitting GetPromptSplitting() const;

	/**
	 * How it bills, right now. The default follows the caps: free when not metered, and "billed, rate
	 * unknown" when metered - a provider that bills should say how.
	 */
	virtual FMotionBilling GetBilling() const;

	/** One line on what this provider is, for someone choosing: "Hosted, paid per generated second". */
	virtual FText GetTagline() const { return FText(); }

	/**
	 * Get ready for a generation that is about to be submitted - start a stopped runner, wake a pod.
	 *
	 * Called when readiness reports Provider Startable, before submitting, so Generate starts what it
	 * needs rather than refusing. Completes on the game thread; false with a sentence when it could not.
	 */
	virtual void PrepareForWork(TFunction<void(bool /*bReady*/, const FString& /*Message*/)> OnDone)
	{
		OnDone(true, FString());
	}

	/** What Prepare For Work is doing right now, with how long it has taken. Empty when it is not running. */
	virtual FString GetPreparationProgress() const { return FString(); }

	/**
	 * Turn the definition-level request into what this provider will send, once per definition and
	 * before it is copied per take. The place for anything expensive - reading poses off a timeline -
	 * so four takes do not pay for it four times. Warnings go to the job strip and the log.
	 */
	virtual void PrepareRequest(FMotionSubmitRequest& Request, TArray<FString>& OutWarnings) const {}

	/**
	 * Whether a character can be generated for by this provider, and what is missing when it cannot.
	 *
	 * The provider-specific half: an uploaded id where the provider generates against uploaded
	 * characters, a retargeter where a rig is set. The provider-agnostic half - a target skeleton - is
	 * checked by the core first.
	 *
	 * @param OutBlocker Which kind of problem, so the window offers the right action.
	 */
	virtual bool CheckCharacter(const UMotionCharacter* Character, FString& OutReason, EMotionBlocker& OutBlocker) const;

	/** Where clips land for this character, in words: "direct onto SK_Mannequin", "retargeted from Kimodo's rig". */
	virtual FString DescribeCharacterRoute(const UMotionCharacter* Character) const;

	/** The buttons this provider offers on a Motion Character: upload it, build a rig for it. */
	virtual void GetCharacterSetupActions(UMotionCharacter* Character, TArray<FMotionCharacterSetupAction>& OutActions) {}

	/**
	 * Ways this provider can be directed beyond the prompt, offered on a definition - pin a pose, show
	 * the constraints that will be sent. Drawn in the definition window's Direct card, which appears
	 * only for providers that return something. Same shape as a character action: a label, an optional
	 * options form, and a Run that answers with a sentence.
	 */
	virtual void GetDirectActions(UMotionDef* Definition, TArray<FMotionCharacterSetupAction>& OutActions) {}

	/**
	 * A short guide to directing this provider, in plain prose for the Direct card: what each lever
	 * does and when to reach for it. Empty for a provider that is directed by its prompt alone.
	 */
	virtual FText GetDirectingGuide() const { return FText(); }

	/**
	 * This provider's own setup, as steps for someone who has just installed it: keys, access grants,
	 * the machine it runs on. Drawn on the Get Started page. Measured, never assumed - a step whose
	 * state was not checked says Unknown.
	 */
	virtual void GetSetupSteps(TArray<FMotionSetupStep>& OutSteps) const {}

	/**
	 * Clip length limits for a model, in seconds.
	 *
	 * Worth respecting rather than discovering: a model with a four second floor silently pads a two
	 * second action to fill the time, which is where limp, slow-motion output comes from.
	 */
	virtual void GetLengthRange(const FString& ModelId, int32& OutMin, int32& OutMax) const = 0;

	/** Where a human can watch a take without spending a download. Empty when unsupported. */
	virtual FString MakeViewerUrl(const FString& ProviderCharacterId, const FString& MotionId) const = 0;

	/** True when a usable credential is available. */
	virtual bool HasCredential() const = 0;

	/** Start one generation. */
	virtual void SubmitJob(const FMotionSubmitRequest& Request, FOnMotionSubmitComplete OnComplete) = 0;

	/** Ask whether a job has finished, and get its motion id when it has. */
	virtual void PollJob(const FString& JobId, FOnMotionJobPolled OnComplete) = 0;

	/** Fetch a finished motion to an absolute local path. This is the call that usually costs money. */
	virtual void DownloadMotion(
		const FString& ProviderCharacterId,
		const FString& MotionId,
		const FString& AbsoluteLocalPath,
		int32 FrameRate,
		FOnMotionDownloadComplete OnComplete) = 0;

	/** Cheapest possible authenticated call, for the settings Test Connection button. */
	virtual void TestConnection(FOnMotionTestComplete OnComplete) = 0;

	// ---------------------------------------------------------------------------------------------
	// Characters
	//
	// Optional. A provider that cannot manage characters server-side leaves these alone and the
	// pipeline falls back to a human uploading by hand and pasting the id back.
	// ---------------------------------------------------------------------------------------------

	/** True when UploadCharacter and ListCharacters do anything. */
	virtual bool SupportsCharacterManagement() const { return false; }

	/**
	 * Upload a rigged character file and get back the id motion is generated against.
	 *
	 * @param AbsoluteFilePath An .fbx, .glb or .gltf on disk.
	 * @param Name             Display name on the provider.
	 * @param Options          Rigging behaviour; see FMotionCharacterUploadOptions.
	 */
	virtual void UploadCharacter(
		const FString& AbsoluteFilePath,
		const FString& Name,
		const FMotionCharacterUploadOptions& Options,
		FOnMotionCharacterUploaded OnComplete)
	{
		FMotionCharacterUploadResult Result;
		Result.Error = TEXT("This provider cannot upload characters.");
		OnComplete(Result);
	}

	/** Every character this account already holds, so an existing one can be reused. */
	virtual void ListCharacters(FOnMotionCharactersListed OnComplete)
	{
		OnComplete(false, {}, TEXT("This provider cannot list characters."));
	}

	/**
	 * Fetch a character back as the provider stores it, mesh and rig included.
	 *
	 * Worth having even though we uploaded the character in the first place: providers normalise
	 * rigs on ingest and generate motion against the normalised one, so this - not the file that
	 * was sent - is the skeleton their animation actually fits.
	 */
	virtual void DownloadCharacterFile(
		const FString& CharacterId,
		const FString& AbsoluteLocalPath,
		FOnMotionDownloadComplete OnComplete)
	{
		OnComplete(false, TEXT("This provider cannot download characters."));
	}
};
