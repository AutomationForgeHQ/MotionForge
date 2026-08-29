// Building an animation from raw rotations on somebody else's rig.

#pragma once

#include "CoreMinimal.h"
#include "IMotionProvider.h"

class USkeleton;
class UAnimSequence;

/**
 * A clip as a generator handed it over: global rotations per joint per frame, on its own rig.
 *
 * Every value here is already in **Unreal's axes and centimetres**. Converting is the provider's
 * job, done once at the edge where the model's conventions are known, rather than being carried
 * inward for everything downstream to get wrong.
 *
 * Global rather than parent-relative rotations on purpose. Local rotations only mean anything
 * alongside the rest orientation they were authored against, so transplanting them onto a rig whose
 * bones rest differently is exactly the operation that produces a clip which imports cleanly and
 * animates twisted. Global orientations carry no such assumption.
 */
struct MOTIONFORGE_API FMotionSourceClip
{
	/** Joint names, in the order every array below is indexed. */
	TArray<FName> JointNames;

	/** Parent index per joint. -1 for the root joint. */
	TArray<int32> ParentIndices;

	/** Rest pose global rotation per joint. */
	TArray<FQuat> RestGlobalRotations;

	/** Rest pose global position per joint. Used to work out proportions against the target rig. */
	TArray<FVector> RestGlobalPositions;

	/** [Frame][Joint] global rotations. */
	TArray<TArray<FQuat>> GlobalRotations;

	/** [Frame] global position of the hips joint. */
	TArray<FVector> HipsPositions;

	/**
	 * [Frame] ground path for the root bone. Empty flattens the hips path instead.
	 *
	 * Worth supplying where the generator has one: a smoothed path is what a root bone wants, and
	 * the hips' own sway becomes bob on the pelvis rather than jitter on the root.
	 */
	TArray<FVector> RootPath;

	/** [Frame] facing for the root bone. Empty derives it from the hips' forward direction. */
	TArray<FQuat> RootFacing;

	/** Per-frame float curves to bake onto the result, by curve name. One value per frame. */
	TMap<FName, TArray<float>> Curves;

	int32 FrameRate = 30;

	/** Index of the hips joint - the one carrying world translation. */
	int32 HipsJointIndex = 0;

	int32 NumFrames() const { return GlobalRotations.Num(); }

	bool IsValid(FString& OutReason) const;

	/**
	 * Turn the whole clip about the vertical axis, rest pose and animation together.
	 *
	 * For reconciling a generator's idea of "forward" with a target rig's. Both can be individually
	 * correct and still disagree - Unreal calls +X forward, while the stock mannequin *mesh* faces +Y
	 * and is turned back by its Character blueprint - and a retargeter works in mesh space, so the
	 * difference lands on every limb.
	 *
	 * It has to be the whole clip. Rotating only the rest pose, or only the retarget pose that
	 * calibrates against it, leaves the animation disagreeing with its own reference by the same
	 * angle; the retargeter then reads that disagreement as motion and hands the rotation straight
	 * to the output. The give-away is a clip whose limbs are all correct relative to each other
	 * while the whole body points a quarter turn from where every other clip on that skeleton does.
	 */
	void RotateAboutZ(double YawDegrees);
};

/** Which source joint drives which bone of the project's skeleton. */
struct MOTIONFORGE_API FMotionRetargetMap
{
	/** Source joint name to target bone name. Joints with no entry drive nothing. */
	TMap<FName, FName> JointToBone;

	/**
	 * The bone that carries world travel, usually "root".
	 *
	 * None puts travel on the hips bone instead, for skeletons with no root. Where it is set, the
	 * clip gets proper Unreal root motion: horizontal path and facing on the root, everything else -
	 * the vertical bob, the sway, the lean - left on the pelvis where it belongs.
	 */
	FName RootBone;

	/** The target bone the source hips drive. */
	FName HipsBone;

	/**
	 * Which way the target skeleton calls forward, as a yaw in degrees. Frame zero is turned to face
	 * this.
	 *
	 * Every clip gets squared up so it starts at the origin facing a fixed direction - otherwise a
	 * take generated walking north-east arrives rotated in the asset and every montage built on it
	 * inherits the offset. What that fixed direction should be is the target skeleton's business,
	 * not the generator's: Unreal calls +X forward, but the stock mannequin *mesh* faces +Y and is
	 * turned back by its Character blueprint, so a clip squared up to +X lands a quarter turn from
	 * every other animation on that skeleton.
	 *
	 * Zero keeps the old behaviour of squaring up to +X, which is right for a skeleton that faces
	 * that way and wrong for one that does not. Measure it; do not assume it.
	 */
	double ForwardYawDegrees = 0.0;
};

/**
 * The three things `Build` does to a clip before any bone is touched, named so they can be undone.
 *
 * A clip does not land on a skeleton in the frame the generator produced it in: it gets turned to
 * face the target's forward, moved so it starts at the origin, and scaled to the target's leg length.
 * Anything reading a built animation *back* into the generator's terms - an authored constraint, a
 * round trip check - has to reverse exactly these three, with exactly these values.
 *
 * Hence one struct and one function that produces it, rather than two places computing the same
 * numbers. The failure mode when they drift is not a crash: the arms come out a quarter turn wrong
 * and the spine and legs, being roughly vertical, look fine.
 */
struct MOTIONFORGE_API FMotionClipFrame
{
	/** The world turn applied to every orientation, so frame zero faces the target's forward. */
	FQuat Unrotate = FQuat::Identity;

	/** Clip-space ground position of frame zero, subtracted so the clip starts at the origin. */
	FVector FirstGround = FVector::ZeroVector;

	/** Leg length ratio, source to target. Translation is multiplied by it; nothing else is. */
	double Scale = 1.0;

	/** False when the clip was too malformed to measure - treat the values as meaningless. */
	bool bValid = false;
};

/**
 * Turns a source clip into a UAnimSequence on the project's own skeleton.
 *
 * This is a rotation retarget and nothing more. It matches orientations bone for bone and scales
 * translation by the difference in leg length, which is enough for locomotion and gesture. It does
 * not solve IK, so a source rig with very different proportions will still slide its feet - bake the
 * generator's foot contacts as curves and drive a foot lock if that matters.
 *
 * Nothing here is specific to any one generator. A second local model that hands back rotations can
 * fill in FMotionSourceClip and reuse all of it.
 */
class MOTIONFORGE_API FMotionAnimBuilder
{
public:

	static FMotionArtifactResult Build(
		const FMotionSourceClip& Clip,
		const FMotionRetargetMap& Map,
		const FMotionArtifactImport& Request);

	/**
	 * The frame `Build` will put this clip in, without building anything.
	 *
	 * @param FirstFrame The first kept frame, if a trim window is in play. The clip is squared up on
	 *                   whichever frame becomes frame zero of the asset, not on the clip's own.
	 */
	static FMotionClipFrame MeasureFrame(
		const FMotionSourceClip& Clip,
		const FMotionRetargetMap& Map,
		const FReferenceSkeleton& Ref,
		int32 FirstFrame = 0);

	/**
	 * How the two rigs differ at rest, per target bone, where the source's rest rotations do not say.
	 *
	 * A generator may report a rest pose whose rotations carry no information - Kimodo reports identity
	 * for every joint - leaving the joint *positions* as the only description of how that rig stands.
	 * This recovers the difference from them: per bone, the direction from a joint to its first mapped
	 * child measured on both rigs, and the minimal rotation between the two.
	 *
	 * Public for the same reason `MeasureFrame` is. Anything converting a pose *back* into the
	 * generator's terms - an authored constraint, a round trip check - has to undo exactly what `Build`
	 * applied, and the only way two files stay in agreement about that is for one of them to compute it
	 * and the other to ask. They did drift once: the import gained this term and the authoring path did
	 * not, which sent an authored arm to the generator fifty-five degrees out while every joint the two
	 * rigs already agreed about looked perfect.
	 *
	 * Identity on every bone when the two rigs rest the same way, so a source rig that matches the
	 * target costs nothing and changes nothing.
	 *
	 * @param Map Only `JointToBone` is read. The root and hips bones do not take part.
	 * @return Per target bone, indexed by bone. Identity for bones no joint drives.
	 */
	static TArray<FQuat> MeasureRestAlignment(
		const FMotionSourceClip& Clip,
		const FMotionRetargetMap& Map,
		const FReferenceSkeleton& Ref);

	/**
	 * The frame a pose *authored* on the target rig sits in, where there is no clip to measure.
	 *
	 * Same three placements, two of which are known rather than measured. An author poses a character
	 * standing at the origin facing the rig's own forward, which is what a generated clip is squared
	 * up to - so the origin term is zero and the turn is the rig's forward outright, with no clip
	 * facing to cancel against. Only the leg-length scale still has to be worked out, and it is the
	 * same ratio `MeasureFrame` uses.
	 *
	 * @param Rest The generator's rest pose, for its hip height. Needs no frames.
	 */
	static FMotionClipFrame MeasureAuthoringFrame(
		const FMotionSourceClip& Rest,
		const FMotionRetargetMap& Map,
		const FReferenceSkeleton& Ref);
};
