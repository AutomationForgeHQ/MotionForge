// What any motion generation service has to be able to do.

#pragma once

#include "CoreMinimal.h"
#include "MotionForgeTypes.h"
#include "MotionControl.h"

class USkeleton;
class UAnimSequence;

/** One generation request. */
struct FMotionSubmitRequest
{
	FString Prompt;
	FString ModelId;
	FString ProviderCharacterId;
	int32   Length = 5;
	bool    bRewritePrompt = true;

	/** Which of the requested takes this is. Carried through so results can be matched back. */
	int32   VariantIndex = 0;

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
};

struct FMotionArtifactResult
{
	bool bSuccess = false;
	TSoftObjectPtr<UAnimSequence> Sequence;
	FString Error;
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

	/** Model used when a definition names none. */
	virtual FString GetDefaultModelId() const = 0;

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
