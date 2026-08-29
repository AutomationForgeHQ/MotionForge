// Fine control over one generation: reproducibility, sampler settings, and kinematic constraints.

#pragma once

#include "CoreMinimal.h"
#include "Engine/SkeletalMesh.h"
#include "MotionControl.generated.h"

class UPoseAsset;
struct FReferenceSkeleton;

/**
 * What a constraint pins.
 *
 * Constraints are a hint to the model, not a solver. Asking for something the prompt contradicts
 * produces artifacts rather than an error, so keep the two agreeing with each other.
 *
 * **Every pose type carries the same data.** A key holds a whole body pose whichever of these is
 * chosen; the type only decides how much of that pose the model is held to. That is what makes
 * authoring pleasant - pose the character once, then say how strictly this moment matters - and it
 * is also why "constrain the right hand" is not a different kind of key from "constrain everything".
 */
UENUM(BlueprintType)
enum class EMotionConstraintType : uint8
{
	/** Where the character stands on the ground, and optionally which way it faces. No pose. */
	RootPath	UMETA(DisplayName = "Root Path"),

	/** Every joint. Right for the anchors: the idle you start from and the idle you end on. */
	FullBody	UMETA(DisplayName = "Full Body Pose"),

	/**
	 * One end effector, and the hips.
	 *
	 * Right for the beat in the middle - the hand on the button - because everything the model is
	 * *not* told stays free for it to invent. A full-body key at that moment would leave it nothing
	 * to do but join up poses, which is the job we are trying to give away.
	 */
	LeftHand	UMETA(DisplayName = "Left Hand"),
	RightHand	UMETA(DisplayName = "Right Hand"),
	LeftFoot	UMETA(DisplayName = "Left Foot"),
	RightFoot	UMETA(DisplayName = "Right Foot")
};

/**
 * A body pose, sampled on whichever skeleton it was authored on.
 *
 * Component-space, in Unreal's axes and centimetres, keyed by the author's own bone names. Nothing
 * here knows about any generator's rig: the provider maps these onto its own joints and converts the
 * axes, which is the only place that conversion belongs.
 *
 * Deliberately not a list of joint rotations in the generator's order. Authors pose *their*
 * character - the one with the right proportions, the mesh they can see, and the control rig they
 * already know - and a pose expressed in their bone names survives a change of generator, a change
 * of bone map, and a rig that has bones the generator has never heard of.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionPoseSample
{
	GENERATED_BODY()

	/**
	 * Where this pose was taken from, for the Details panel to have something to say.
	 *
	 * A map of three hundred transforms tells a reader nothing. "SKM_Quinn, from AS_Idle frame 0"
	 * tells them what they need: which skeleton it is in, and what to re-capture if it is wrong.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pose")
	FString Source;

	/**
	 * The mesh this pose was read off, when it was read off one rather than out of an animation.
	 *
	 * The rest pose a conversion cancels through has to be the one the pose is expressed in, and a
	 * `USkeletalMesh` and its `USkeleton` do not have to agree: Quinn's mesh carries 166 bones and
	 * the skeleton asset 361, with hips 2.9cm apart. Using the wrong one is not a crash - it is a
	 * few degrees on the spine and a character that stands slightly too tall, which is exactly the
	 * size of error that survives every check and ruins nothing visibly enough to investigate.
	 *
	 * Empty when the pose came from an animation asset, which is evaluated in the skeleton's space.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pose")
	TSoftObjectPtr<USkeletalMesh> SourceMesh;

	/**
	 * Component-space transform per bone. Only mapped bones are read; extras are ignored.
	 *
	 * Read-only on purpose. A pose is captured, never assembled: there is no useful way to type
	 * three hundred transforms, and a property that looks editable and cannot be edited is worse
	 * than one that admits it. Fill it with Author Pose Constraint or Capture Pose Key.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Pose")
	TMap<FName, FTransform> Bones;

	bool IsValid() const { return Bones.Num() > 0; }
};

/**
 * One moment a constraint pins something down.
 *
 * Positions are in **Unreal's own axes and centimetres**, relative to where the character starts.
 * Providers convert into whatever their model wants; nothing above this line should ever have to
 * think in another engine's coordinate system.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionConstraintKey
{
	GENERATED_BODY()

	/**
	 * Which frame of the generated clip this pins, counting from zero.
	 *
	 * Frames, not seconds, because that is what the model indexes. Multiply by the provider's native
	 * frame rate - Get Provider Capabilities reports it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key", meta = (ClampMin = 0))
	int32 Frame = 0;

	/**
	 * The body at this moment, on the authoring skeleton.
	 *
	 * Required by every type except Root Path. An end-effector constraint needs it too: the model is
	 * given the whole pose and told which part of it to honour, so a hand key without a pose has
	 * nothing to place the hand *from*.
	 *
	 * Ignored when `PoseAsset` is set - see there.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key")
	FMotionPoseSample Pose;

	/**
	 * A saved pose to drive this key from instead of the captured one.
	 *
	 * The reusable half of authoring. A captured pose belongs to one key on one definition; a pose
	 * asset is a thing in its own right - the idle every clip should settle into, a standard grip -
	 * that many definitions can point at, and editing it updates all of them.
	 *
	 * **Supersedes `Pose` when set.** Both can be filled: capturing into a key that already names an
	 * asset leaves the capture there, unused, rather than destroying it. Clear this to fall back.
	 *
	 * Three things about pose assets are worth knowing before pointing at one, because two of them
	 * fail silently:
	 *
	 * - **One asset holds many named poses.** Making one from an animation gives a pose per frame,
	 *   named `Pose_0`, `Pose_1`... so `PoseName` says which. It can stay empty when the asset holds
	 *   exactly one.
	 * - **An additive pose is a difference, not a pose.** It says "twenty degrees further than
	 *   wherever you already were", which as an absolute constraint is meaningless. Refused.
	 * - **A pose need not cover every bone.** Corrective and hand-shape poses deliberately hold a
	 *   handful. Bones the asset does not carry come from the reference pose, which is what Unreal
	 *   itself does and what you see previewing the asset alone. Fine for an end-effector key, which
	 *   only honours the effector anyway - refused for a Full Body key, where it would pin the whole
	 *   character to a rest pose nobody asked for.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key")
	TSoftObjectPtr<UPoseAsset> PoseAsset;

	/**
	 * Which pose inside `PoseAsset`. Empty picks the only one, and is an error when there are several.
	 *
	 * Not defaulted to the first. An asset made from an animation has its first frame as `Pose_0`,
	 * which is usually the idle it started from - so quietly taking the first would pin the clip to a
	 * resting pose and look like the model ignoring the constraint.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key")
	FName PoseName;

	/**
	 * Where the character stands, relative to where it starts. Ground plane only; height comes from
	 * the pose.
	 *
	 * Zero at frame zero is not a suggestion - generators canonicalise a clip so it begins at the
	 * origin, so a path that starts anywhere else is silently re-centred and every later waypoint
	 * lands somewhere other than intended.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key")
	FVector2D GroundPosition = FVector2D::ZeroVector;

	/**
	 * Which way the whole body faces, in degrees about Unreal's up axis. Root Path only.
	 *
	 * Degrees here because that is what a human types; providers that want a direction vector build
	 * one. Separate from the pose because facing is the one thing a path constrains that a pose does
	 * not already say.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key")
	float HeadingDegrees = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Key")
	bool bUseHeading = false;
};

/**
 * Turning whatever a key names into the one pose the providers actually read.
 *
 * A key can hold a captured pose, or point at a saved one, and a provider should not have to know
 * which - it asks for the pose and gets a pose. Keeping that decision here rather than in each
 * provider is what stops two of them disagreeing about which source wins.
 */
struct MOTIONFORGE_API FMotionPoseResolve
{
	/**
	 * The effective pose for a key: the pose asset if it names one, otherwise the captured sample.
	 *
	 * @param Ref   The reference skeleton the pose is wanted in. Missing bones come from its rest
	 *              pose, so this has to be the skeleton the constraint will be converted against.
	 * @param Type  What the key constrains. Only used to decide whether partial coverage is
	 *              acceptable - it is, except for a full body key.
	 * @param RequiredBones The bones that have to be covered for a full body key to mean anything -
	 *              the ones the provider's bone map actually reads. Empty demands the whole skeleton,
	 *              which is almost never what is wanted: a pose asset built from an animation carries
	 *              only the bones that animation moved, and the twist and finger bones it leaves out
	 *              were at the reference pose in the animation too.
	 * @param OutCoverage How many bones came from the asset rather than the rest pose, for reporting.
	 *                    Equal to the bone count when the pose covers everything.
	 */
	static bool Resolve(
		const struct FMotionConstraintKey& Key,
		const FReferenceSkeleton& Ref,
		EMotionConstraintType Type,
		const TSet<FName>& RequiredBones,
		FMotionPoseSample& OutPose,
		int32& OutCoverage,
		FString& OutError);
};

/**
 * Reading constraint keys out of a Level Sequence the artist authored.
 *
 * Two questions, answered separately because they fail separately: *when* is a constraint, and *what*
 * is the body doing then.
 *
 * **When** comes from the key times already in the sequence - every channel of every section, plus any
 * marked frames - so the artist marks a moment by keying it, which is what they were going to do
 * anyway. Read at the movie scene's tick resolution and converted through seconds, never through the
 * display rate: the rate shown on the timeline is a viewing preference, and a clip whose constraints
 * moved because somebody switched the timeline to 60fps would be a miserable thing to debug.
 *
 * **What** comes from evaluating the sequence and reading the posed skeleton, not from the tracks. A
 * Control Rig track holds control values, not bone transforms, and an additive or layered setup holds
 * neither - the only thing that knows the final pose is the evaluated character. Reading bones also
 * means this does not care how the artist posed it: control rig, an animation track, a hand-keyed
 * skeleton, or all three at once.
 */
/**
 * How a harvest ended, because "no poses came back" has two meanings and they want opposite handling.
 *
 * A sequence now carries beats as well as poses (see `UMovieSceneMotionPromptTrack`), so one that
 * supplies only beats is entirely legitimate and is **not** a failure. Collapsing that into the same
 * answer as a broken read is what made authored constraints unreachable the moment a prompt track
 * existed: the definition had keys, the sequence had none, and nothing was sent.
 */
enum class EMotionHarvestResult : uint8
{
	/** Poses were read. They replace the authored list outright. */
	Ok,

	/**
	 * Nothing in the sequence is keyed, so it is not claiming to supply poses.
	 *
	 * Fall through to the authored list. This is the ordinary state of a sequence that exists to hold
	 * a prompt track.
	 */
	NothingKeyed,

	/**
	 * The sequence meant to supply poses and could not - wrong skeleton, nothing bound, every sample
	 * identical.
	 *
	 * **Send nothing.** Somebody pointed this at a sequence and meant it; generating from a different
	 * set of poses because the sequence would not read is a worse outcome than generating from none.
	 */
	Failed
};

struct MOTIONFORGE_API FMotionSequenceConstraints
{
	/**
	 * @param Sequence       The sequence to read. Its bindings supply the character.
	 * @param World          Editor world to evaluate in - the sequence's bindings resolve against it.
	 * @param ExpectedSkeleton The skeleton the keys have to be on. A sequence bound to some other
	 *                       character would otherwise produce a confidently wrong constraint.
	 * @param FrameRate      The generator's frame rate, for turning seconds into clip frames.
	 * @param DefaultType What a keyed moment pins when no Motion Constraint span covers it. The
	 *                    sequence-wide answer, from the prompt track.
	 * @param OutConstraints One entry per type actually used, each holding the keys that resolved to
	 *                       it - which is the shape the providers already read, so a clip pinning a
	 *                       hand in the middle and the whole body at the ends needs nothing new
	 *                       downstream.
	 * @param OutDurationSeconds The sequence's playback range, which is what the clip's length should
	 *                       be. Reported rather than written: silently resizing somebody's definition
	 *                       is not a service.
	 */
	static EMotionHarvestResult Harvest(
		class ULevelSequence* Sequence,
		class UWorld* World,
		const class USkeleton* ExpectedSkeleton,
		int32 FrameRate,
		EMotionConstraintType DefaultType,
		TArray<struct FMotionConstraint>& OutConstraints,
		double& OutDurationSeconds,
		TArray<FString>& OutWarnings,
		FString& OutError);
};

/** One thing held to a path or a pose across a clip. */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionConstraint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constraint")
	EMotionConstraintType Type = EMotionConstraintType::RootPath;

	/**
	 * The moments this constraint pins. Sparse beats dense.
	 *
	 * Twenty keys of one type is about the practical ceiling; past that the model spends its capacity
	 * satisfying constraints instead of producing motion, and quality falls off. Root paths are the
	 * exception and can carry more.
	 *
	 * A key at frame zero is optional. Without one the model invents an opening from its own
	 * distribution and works towards the first key it *is* given, which is usually what you want:
	 * constrain the moments that matter and leave the approach to the model.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constraint")
	TArray<FMotionConstraintKey> Keys;
};

/**
 * Everything about a generation beyond the prompt and its length.
 *
 * Every field has an "unset" value that means *let the provider choose*, so a definition that fills
 * none of this behaves exactly as it did before this struct existed. Providers that cannot honour a
 * field ignore it and log rather than failing - see FMotionProviderCaps for what a provider supports.
 */
USTRUCT(BlueprintType)
struct MOTIONFORGE_API FMotionControl
{
	GENERATED_BODY()

	/**
	 * Makes a generation reproducible. -1 asks for a fresh random one.
	 *
	 * On a provider that honours it, prompt + model + seed + sampler settings is a complete recipe:
	 * the clip can be thrown away and recreated exactly, forever, so the raw file is a cache rather
	 * than the only copy. On a provider without seeds a discarded take is gone for good, which is why
	 * candidates are never pruned automatically.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	int32 Seed = -1;

	/** Denoising steps. Zero uses the provider's default. More is slower and slightly better. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = 0, ClampMax = 500))
	int32 DiffusionSteps = 0;

	/** How hard to follow the prompt. Zero uses the provider's default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = 0.0))
	float TextGuidance = 0.f;

	/** How hard to follow the constraints. Zero uses the provider's default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = 0.0))
	float ConstraintGuidance = 0.f;

	/**
	 * Let the provider clean up foot sliding and constraint drift after sampling.
	 *
	 * Worth leaving on. The raw output of a motion diffusion model skates, and the cleanup pass is
	 * what makes contact with the ground look like contact.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bPostProcess = true;

	/**
	 * Let the provider cut the prompt into beats at its sentence breaks, and generate each one.
	 *
	 * On by default, because it is what a segmenting model does natively and what a written-out
	 * multi-beat prompt is asking for. `Length` is then the whole clip, shared between the beats.
	 *
	 * **Turn it off when a full stop is punctuation rather than structure.** A prompt is divided by
	 * things nobody thinks of as a sentence break - a decimal number splits "hold for 2.5 seconds"
	 * into two beats, and so does an abbreviation - and the symptom is a beat too short to perform
	 * rather than an error. Off, the whole prompt is one generation and `Length` is its duration.
	 *
	 * Providers that do not segment ignore this entirely.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bSplitPromptIntoBeats = true;

	/**
	 * Seconds for each beat, in order. Empty shares `Length` out evenly.
	 *
	 * The reason to fill this in is that beats are rarely equal: a step, a long deliberate hold and
	 * a quick recovery want 2, 7 and 3 rather than 4, 4 and 4. The clip is then the sum.
	 *
	 * **One entry per beat, or the request is refused.** Count them the way the provider does rather
	 * than the way the sentence reads - the runner quotes the actual division back when they
	 * disagree, which is the fastest way to find the full stop you did not know was there.
	 *
	 * Ignored when `bSplitPromptIntoBeats` is off, and on providers that do not segment.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control",
		meta = (EditCondition = "bSplitPromptIntoBeats"))
	TArray<float> BeatSeconds;

	/** Where the body has to be, and when. Empty means the prompt decides everything. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	TArray<FMotionConstraint> Constraints;

	/**
	 * A Level Sequence to read the constraint poses out of, instead of the list above.
	 *
	 * The fastest way to author a whole clip, and the one closest to how an animator already works:
	 * put the character in a sequence, key the poses that matter, point this at it. Every time the rig
	 * is keyed becomes a constraint, so keying *is* the marking - there is no second step where you
	 * say which moments count.
	 *
	 * **Referenced, never baked.** The sequence stays live: edit it, and the next generation follows.
	 * Nothing is copied into this asset, so there is one copy of the poses and it is the one the
	 * artist is looking at.
	 *
	 * **Wins outright.** With this set, `Constraints` above is ignored entirely rather than merged -
	 * two sources of truth for the same clip is how a definition ends up pinned to poses nobody can
	 * find. Clear it to go back to the authored list, which is left untouched meanwhile.
	 *
	 * Times are read in seconds and converted to the generator's frame rate, so the sequence's display
	 * rate is a viewing preference and cannot silently change what is sent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	TSoftObjectPtr<class ULevelSequence> ConstraintSequence;

	/**
	 * What the poses harvested from `ConstraintSequence` constrain.
	 *
	 * Full Body suits the usual shape of a sequence - a rest pose at each end and the beat in the
	 * middle. Choose an end effector when the sequence exists to place a hand and the rest of the body
	 * should stay the model's business.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	EMotionConstraintType ConstraintSequenceType = EMotionConstraintType::FullBody;

	/** True when nothing here would change what the provider does. */
	bool IsDefault() const
	{
		return Seed < 0
			&& DiffusionSteps == 0
			&& TextGuidance == 0.f
			&& ConstraintGuidance == 0.f
			&& bPostProcess
			&& bSplitPromptIntoBeats
			&& BeatSeconds.Num() == 0
			&& Constraints.Num() == 0;
	}
};
