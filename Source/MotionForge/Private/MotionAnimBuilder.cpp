#include "MotionAnimBuilder.h"

#include "Animation/AnimData/IAnimationDataModel.h"

#include "MotionForge.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/RichCurve.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "MotionAnimBuilder"

namespace MotionAnimBuilderPrivate
{

	/** Reference pose in component space. The reference skeleton always lists parents first. */
	static TArray<FTransform> BuildRefGlobals(const FReferenceSkeleton& Ref)
	{
		const TArray<FTransform>& Local = Ref.GetRefBonePose();

		TArray<FTransform> Global;
		Global.SetNum(Local.Num());

		for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
		{
			const int32 Parent = Ref.GetParentIndex(Bone);
			Global[Bone] = (Parent == INDEX_NONE) ? Local[Bone] : Local[Bone] * Global[Parent];
		}

		return Global;
	}

	/**
	 * Leg length ratio, source to target, using hip height as the stand-in.
	 *
	 * A generator with one fixed body has no idea how tall the game's character is, and an unscaled
	 * path makes a short character stride like a tall one. Shared so that reading a clip back out
	 * divides by exactly what building it multiplied by.
	 */
	static double MeasureScale(
		const FMotionSourceClip& Source, const FMotionRetargetMap& Map, const FReferenceSkeleton& Ref)
	{
		if (!Source.RestGlobalPositions.IsValidIndex(Source.HipsJointIndex))
		{
			return 1.0;
		}

		const TArray<FTransform> RefGlobal = BuildRefGlobals(Ref);
		const int32 HipsBone = Ref.FindBoneIndex(Map.HipsBone);

		const double SourceHipHeight = Source.RestGlobalPositions[Source.HipsJointIndex].Z;
		const double TargetHipHeight = RefGlobal.IsValidIndex(HipsBone)
			? RefGlobal[HipsBone].GetTranslation().Z
			: 0.0;

		return (SourceHipHeight > KINDA_SMALL_NUMBER && TargetHipHeight > KINDA_SMALL_NUMBER)
			? TargetHipHeight / SourceHipHeight
			: 1.0;
	}

	/**
	 * Which source joint drives which target bone, as an array indexed by bone.
	 *
	 * @param OutMissing Target bone names the map asks for that this skeleton does not have. Optional -
	 *                   only `Build` reports them, because only `Build` is in a position to refuse.
	 */
	static TArray<int32> BuildBoneToJoint(
		const FMotionSourceClip& Clip,
		const FMotionRetargetMap& Map,
		const FReferenceSkeleton& Ref,
		TArray<FString>* OutMissing = nullptr)
	{
		TArray<int32> BoneToJoint;
		BoneToJoint.Init(INDEX_NONE, Ref.GetNum());

		for (const TPair<FName, FName>& Pair : Map.JointToBone)
		{
			const int32 Joint = Clip.JointNames.IndexOfByKey(Pair.Key);
			if (Joint == INDEX_NONE)
			{
				continue;
			}

			const int32 Bone = Ref.FindBoneIndex(Pair.Value);
			if (Bone == INDEX_NONE)
			{
				if (OutMissing)
				{
					OutMissing->AddUnique(Pair.Value.ToString());
				}
				continue;
			}

			BoneToJoint[Bone] = Joint;
		}

		return BoneToJoint;
	}

	/**
	 * The turn from the target rig's rest pose to the source rig's, per bone, read off the joints.
	 *
	 * A generator is free to report a rest pose whose rotations say nothing - Kimodo reports identity
	 * for every joint - and then the only description of how that rig stands is where its joints sit.
	 * Which is enough: a skeleton is rigid, so the vector from a joint to its child *is* the direction
	 * that bone points at rest. Measure the same segment on both rigs and the difference between the
	 * two rest poses becomes a rotation instead of an unknown.
	 *
	 * That difference is the T-pose against the A-pose. Without this term a source arm resting
	 * horizontal is treated as though it rested at the target's 45 degrees, and every frame of arm
	 * motion lands a rest-pose's worth of turn away from where it belongs - which is why the arms
	 * cross while the legs, whose two rests nearly agree, look fine.
	 *
	 * Two properties worth keeping if this is ever rewritten:
	 *
	 * - **Both segments are chosen by the same pairing.** The child is picked on the source and
	 *   followed through the bone map, never picked independently on each rig. Pick each rig's "first
	 *   child" separately and the pelvis compares its spine against the other's thigh.
	 * - **The turn is the minimal one.** `FindBetweenNormals` leaves no twist about the bone's own
	 *   axis, so where the two rests already agree it returns identity exactly and this whole term
	 *   vanishes. A frame-based alignment would instead invent a twist from whatever second axis it
	 *   was built with, and near-vertical bones - spine, neck, thigh - would get a large arbitrary one
	 *   from a tiny direction difference. That failure mode is the engine's own `AlignBone`, and it
	 *   costs the feet.
	 */
	static TArray<FQuat> BuildRestAlignment(
		const FMotionSourceClip& Clip,
		const TArray<int32>& BoneToJoint,
		const TArray<FTransform>& RefGlobal,
		const FReferenceSkeleton& Ref,
		TArray<FString>& OutSummary)
	{
		const int32 NumBones = BoneToJoint.Num();
		const int32 NumJoints = Clip.JointNames.Num();

		TArray<FQuat> Align;
		Align.Init(FQuat::Identity, NumBones);

		// Which bones got a real measurement, as opposed to being left at identity for want of one.
		TArray<bool> Measured;
		Measured.Init(false, NumBones);

		// Bone map the other way round, so a source joint can be followed to its bone.
		TArray<int32> JointToBone;
		JointToBone.Init(INDEX_NONE, NumJoints);
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (Clip.RestGlobalPositions.IsValidIndex(BoneToJoint[Bone]))
			{
				JointToBone[BoneToJoint[Bone]] = Bone;
			}
		}

		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			const int32 Joint = BoneToJoint[Bone];
			if (!Clip.RestGlobalPositions.IsValidIndex(Joint))
			{
				continue;
			}

			// The first direct child of this joint that drives a bone.
			//
			// One child, not all of them - and measured, not assumed. A pelvis or a chest has three,
			// which overdetermine the rotation, and fitting all three at once is the obvious
			// improvement. It was tried: it makes every number worse, head 4.9cm to 8.3cm, because the
			// least-squares fit resolves the disagreement between three children as a twist about the
			// spine and swings the neck sideways. One child gives a swing and no twist, which is the
			// weaker claim and the true one.
			//
			// A joint whose children are all unmapped measures nothing and inherits below. Following
			// the chain past an unmapped joint to reach a mapped grandchild would measure a segment
			// the two rigs do not necessarily divide the same way, which is worse than not measuring.
			int32 ChildJoint = INDEX_NONE;
			for (int32 Candidate = 0; Candidate < NumJoints; ++Candidate)
			{
				if (Clip.ParentIndices.IsValidIndex(Candidate)
					&& Clip.ParentIndices[Candidate] == Joint
					&& JointToBone[Candidate] != INDEX_NONE)
				{
					ChildJoint = Candidate;
					break;
				}
			}

			if (ChildJoint == INDEX_NONE)
			{
				// A leaf - hand, head, toe. Handled after the loop, once there is something to inherit.
				continue;
			}

			const int32 ChildBone = JointToBone[ChildJoint];

			const FVector SourceSegment =
				Clip.RestGlobalPositions[ChildJoint] - Clip.RestGlobalPositions[Joint];
			const FVector TargetSegment =
				RefGlobal[ChildBone].GetTranslation() - RefGlobal[Bone].GetTranslation();

			// A zero-length segment has no direction. Coincident joints are real - a shoulder sitting
			// on its clavicle, a rig with a doubled bone - and normalising one produces an arbitrary
			// axis rather than a failure, which is far worse than leaving the bone unaligned.
			if (SourceSegment.SizeSquared() < FMath::Square(0.01)
				|| TargetSegment.SizeSquared() < FMath::Square(0.01))
			{
				continue;
			}

			Align[Bone] = FQuat::FindBetweenNormals(
				TargetSegment.GetSafeNormal(), SourceSegment.GetSafeNormal());
			Align[Bone].Normalize();
			Measured[Bone] = true;

			// Reported, not silent. The numbers here are the whole diagnosis when a limb comes out
			// wrong: arms in the tens of degrees and legs near zero is a T-posed source landing on an
			// A-posed target, which is correct and expected. A large angle on a near-vertical bone is
			// not - it means the segment being measured is not the one intended, usually because the
			// bone map skips a joint the two rigs disagree about.
			const double Degrees = FMath::RadiansToDegrees(Align[Bone].GetAngle());
			if (Degrees > 1.0)
			{
				OutSummary.Add(FString::Printf(TEXT("%s->%s %.0f"),
					*Ref.GetBoneName(Bone).ToString(), *Ref.GetBoneName(ChildBone).ToString(), Degrees));
			}
		}

		// Leaves inherit from the bone above.
		//
		// A hand, a head or a toe points at nothing, so no segment measures how the two rigs rest it -
		// and leaving it at identity is not the safe choice it looks like. Its *position* is decided by
		// the chain above and comes out right either way, so a bone distance says nothing; what is
		// wrong is its own orientation, by the sixty degrees the forearm above it was just corrected
		// by. That is a hand hanging off the wrist at right angles, invisible to every number here and
		// the first thing anybody notices watching it play.
		//
		// The bone above is the best estimate available, and for a wrist, an ankle or a skull it is a
		// good one: whatever a rig does differently at rest, it does to the whole limb.
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (BoneToJoint[Bone] == INDEX_NONE || Measured[Bone])
			{
				continue;
			}

			for (int32 Ancestor = Ref.GetParentIndex(Bone);
				Ancestor != INDEX_NONE;
				Ancestor = Ref.GetParentIndex(Ancestor))
			{
				if (Measured[Ancestor])
				{
					Align[Bone] = Align[Ancestor];
					break;
				}
			}
		}

		return Align;
	}

	/** Yaw-only rotation from a forward vector, for deriving a root facing when none was given. */
	static FQuat FlattenToYaw(const FQuat& Rotation)
	{
		FVector Forward = Rotation.GetForwardVector();
		Forward.Z = 0.0;

		if (Forward.IsNearlyZero())
		{
			return FQuat::Identity;
		}

		Forward.Normalize();
		return FRotationMatrix::MakeFromX(Forward).ToQuat();
	}
}

void FMotionSourceClip::RotateAboutZ(double YawDegrees)
{
	if (FMath::IsNearlyZero(YawDegrees, 0.01))
	{
		return;
	}

	const FQuat Yaw(FVector::ZAxisVector, FMath::DegreesToRadians(YawDegrees));

	// Every orientation is global, so the turn pre-multiplies; every position is global too, so it
	// gets rotated outright. Miss one of these arrays and the clip is internally inconsistent in a
	// way that reads as a retargeting bug rather than as a missing line here.
	for (FQuat& Rotation : RestGlobalRotations)
	{
		Rotation = Yaw * Rotation;
	}

	for (FVector& Position : RestGlobalPositions)
	{
		Position = Yaw.RotateVector(Position);
	}

	for (TArray<FQuat>& Frame : GlobalRotations)
	{
		for (FQuat& Rotation : Frame)
		{
			Rotation = Yaw * Rotation;
		}
	}

	for (FVector& Position : HipsPositions)
	{
		Position = Yaw.RotateVector(Position);
	}

	for (FVector& Position : RootPath)
	{
		Position = Yaw.RotateVector(Position);
	}

	for (FQuat& Facing : RootFacing)
	{
		Facing = Yaw * Facing;
	}
}

bool FMotionSourceClip::IsValid(FString& OutReason) const
{
	const int32 NumJoints = JointNames.Num();

	if (NumJoints == 0)
	{
		OutReason = TEXT("the clip names no joints");
		return false;
	}

	if (ParentIndices.Num() != NumJoints
		|| RestGlobalRotations.Num() != NumJoints
		|| RestGlobalPositions.Num() != NumJoints)
	{
		OutReason = FString::Printf(
			TEXT("the clip describes %d joints but its rest pose arrays are %d/%d/%d long"),
			NumJoints, ParentIndices.Num(), RestGlobalRotations.Num(), RestGlobalPositions.Num());
		return false;
	}

	if (NumFrames() == 0)
	{
		OutReason = TEXT("the clip has no frames");
		return false;
	}

	for (int32 Frame = 0; Frame < GlobalRotations.Num(); ++Frame)
	{
		if (GlobalRotations[Frame].Num() != NumJoints)
		{
			OutReason = FString::Printf(
				TEXT("frame %d has %d rotations for %d joints"),
				Frame, GlobalRotations[Frame].Num(), NumJoints);
			return false;
		}
	}

	if (HipsPositions.Num() != NumFrames())
	{
		OutReason = FString::Printf(TEXT("the clip has %d frames but %d hip positions"),
			NumFrames(), HipsPositions.Num());
		return false;
	}

	if (!JointNames.IsValidIndex(HipsJointIndex))
	{
		OutReason = FString::Printf(TEXT("hips joint index %d is out of range"), HipsJointIndex);
		return false;
	}

	if (FrameRate <= 0)
	{
		OutReason = TEXT("the clip has no frame rate");
		return false;
	}

	return true;
}

FMotionClipFrame FMotionAnimBuilder::MeasureFrame(
	const FMotionSourceClip& Clip,
	const FMotionRetargetMap& Map,
	const FReferenceSkeleton& Ref,
	int32 FirstFrame)
{
	using namespace MotionAnimBuilderPrivate;

	FMotionClipFrame Frame;

	const int32 SourceFrames = Clip.NumFrames();

	if (SourceFrames == 0
		|| !Clip.RestGlobalPositions.IsValidIndex(Clip.HipsJointIndex)
		|| !Clip.HipsPositions.IsValidIndex(0))
	{
		return Frame;
	}

	const int32 At = FMath::Clamp(FirstFrame, 0, SourceFrames - 1);

	const FVector Raw = (Clip.RootPath.Num() == SourceFrames)
		? Clip.RootPath[At]
		: Clip.HipsPositions[At];

	Frame.FirstGround = FVector(Raw.X, Raw.Y, 0.0);

	const FQuat FirstFacing = (Clip.RootFacing.Num() == SourceFrames)
		? FlattenToYaw(Clip.RootFacing[At])
		: FlattenToYaw(Clip.GlobalRotations[At][Clip.HipsJointIndex]);

	const FQuat Canonical(FVector::ZAxisVector, FMath::DegreesToRadians(Map.ForwardYawDegrees));
	Frame.Unrotate = Canonical * FirstFacing.Inverse();

	Frame.Scale = MeasureScale(Clip, Map, Ref);

	Frame.bValid = true;
	return Frame;
}

TArray<FQuat> FMotionAnimBuilder::MeasureRestAlignment(
	const FMotionSourceClip& Clip,
	const FMotionRetargetMap& Map,
	const FReferenceSkeleton& Ref)
{
	using namespace MotionAnimBuilderPrivate;

	// Same three inputs `Build` uses, derived the same way, so the answer cannot differ from the one
	// the clip was built with.
	const TArray<int32> BoneToJoint = BuildBoneToJoint(Clip, Map, Ref);
	const TArray<FTransform> RefGlobal = BuildRefGlobals(Ref);

	TArray<FString> Unused;
	return BuildRestAlignment(Clip, BoneToJoint, RefGlobal, Ref, Unused);
}

FMotionClipFrame FMotionAnimBuilder::MeasureAuthoringFrame(
	const FMotionSourceClip& Rest,
	const FMotionRetargetMap& Map,
	const FReferenceSkeleton& Ref)
{
	using namespace MotionAnimBuilderPrivate;

	FMotionClipFrame Frame;

	// The rig's own forward, and nothing else.
	//
	// `MeasureFrame` computes `Canonical * FirstFacing^-1` because a generated clip arrives pointing
	// wherever it was generated pointing. An authored pose does not: it is posed on the target rig, in
	// the target rig's space, so the only difference between it and the generator's canonical frame is
	// which way that rig calls forward.
	Frame.Unrotate = FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Map.ForwardYawDegrees));

	// Zero, not measured. A generator canonicalises its output to begin at the origin, so an author's
	// key is read relative to where the character starts whatever the actor's world position is.
	Frame.FirstGround = FVector::ZeroVector;

	Frame.Scale = MeasureScale(Rest, Map, Ref);

	Frame.bValid = true;
	return Frame;
}

namespace MotionUntracked
{
	/**
	 * One pose for every bone a generator did not drive.
	 *
	 * Read once, from one frame, and applied as a constant - because that is what it is. A model that
	 * does not predict fingers does not predict them moving either, and pretending otherwise by
	 * sampling the source clip over time would animate the hands from a pose that has nothing to do
	 * with them.
	 */
	TMap<FName, FTransform> ReadPose(const UAnimSequence* Source, float AtTime, const FReferenceSkeleton& Ref)
	{
		TMap<FName, FTransform> Pose;

		if (Source == nullptr)
		{
			return Pose;
		}

		const TScriptInterface<IAnimationDataModel> Model = Source->GetDataModelInterface();
		if (!Model)
		{
			return Pose;
		}

		TArray<FName> TrackNames;
		Model->GetBoneTrackNames(TrackNames);

		for (const FName TrackName : TrackNames)
		{
			if (Ref.FindBoneIndex(TrackName) == INDEX_NONE)
			{
				continue;
			}

			FTransform Local;
			Source->GetBoneTransform(
				Local, FSkeletonPoseBoneIndex(Ref.FindBoneIndex(TrackName)),
				FAnimExtractContext(static_cast<double>(AtTime)), true);

			Pose.Add(TrackName, Local);
		}

		return Pose;
	}
}

FMotionArtifactResult FMotionAnimBuilder::Build(
	const FMotionSourceClip& Clip,
	const FMotionRetargetMap& Map,
	const FMotionArtifactImport& Request)
{
	using namespace MotionAnimBuilderPrivate;

	FMotionArtifactResult Result;

	FString Reason;
	if (!Clip.IsValid(Reason))
	{
		Result.Error = FString::Printf(TEXT("The generated clip is malformed: %s."), *Reason);
		return Result;
	}

	if (!Request.TargetSkeleton)
	{
		Result.Error = TEXT("No target skeleton to build the animation on.");
		return Result;
	}

	const FReferenceSkeleton& Ref = Request.TargetSkeleton->GetReferenceSkeleton();
	const TArray<FTransform>& RefLocal = Ref.GetRefBonePose();
	const TArray<FTransform> RefGlobal = BuildRefGlobals(Ref);
	const int32 NumBones = Ref.GetNum();

	if (NumBones == 0)
	{
		Result.Error = FString::Printf(TEXT("Skeleton '%s' has no bones."),
			*Request.TargetSkeleton->GetName());
		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// Wire source joints to target bones
	// -----------------------------------------------------------------------------------------

	// Per target bone: which source joint drives it, or INDEX_NONE.
	TArray<FString> Missing;
	const TArray<int32> BoneToJoint = BuildBoneToJoint(Clip, Map, Ref, &Missing);

	int32 Mapped = 0;
	for (const int32 Joint : BoneToJoint)
	{
		Mapped += (Joint != INDEX_NONE) ? 1 : 0;
	}

	if (Mapped == 0)
	{
		Result.Error = FString::Printf(
			TEXT("None of the generator's joints match a bone on '%s'. Expected names like %s - "
				 "check the Motion Character's Target Skeleton is the one you meant."),
			*Request.TargetSkeleton->GetName(),
			Missing.Num() > 0 ? *FString::Join(Missing, TEXT(", ")) : TEXT("pelvis, spine_01, hand_l"));
		return Result;
	}

	if (Missing.Num() > 0)
	{
		// Not fatal. A skeleton without a ball bone or a second neck joint still animates correctly
		// from the rest; saying which bones went unused is more useful than refusing.
		UE_LOG(LogMotionForge, Warning,
			TEXT("'%s' has no bone named %s - those joints will not be driven."),
			*Request.TargetSkeleton->GetName(), *FString::Join(Missing, TEXT(", ")));
	}

	const int32 RootBone = Map.RootBone.IsNone() ? INDEX_NONE : Ref.FindBoneIndex(Map.RootBone);
	const int32 HipsBone = Ref.FindBoneIndex(Map.HipsBone);

	if (HipsBone == INDEX_NONE)
	{
		Result.Error = FString::Printf(
			TEXT("'%s' has no bone named '%s', so there is nowhere to put the character's position."),
			*Request.TargetSkeleton->GetName(), *Map.HipsBone.ToString());
		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// Rest offsets, and proportions
	// -----------------------------------------------------------------------------------------

	// How the two rigs differ at rest, measured off their joint positions rather than taken from the
	// rest rotations - which a generator is entitled to report as identity, and Kimodo does.
	//
	// `MeasureRestAlignment` is the same thing for callers outside this function, and anything undoing
	// this build has to use it. Both go through `BuildRestAlignment` so there is one implementation.
	TArray<FString> AlignSummary;
	const TArray<FQuat> RestAlign = BuildRestAlignment(Clip, BoneToJoint, RefGlobal, Ref, AlignSummary);

	if (AlignSummary.Num() > 0)
	{
		UE_LOG(LogMotionForge, Log, TEXT("Rest pose differs on %d bone(s), in degrees: %s."),
			AlignSummary.Num(), *FString::Join(AlignSummary, TEXT(", ")));
	}

	// The whole retarget, in one line per bone. Offset absorbs every difference between how the two
	// rigs rest - T-pose against A-pose, and each rig's own idea of which way a bone's axes point -
	// so that a source joint at rest lands the target bone in the source's rest *pose*.
	//
	// Note what that is not: the target's own rest. A source standing in a T should put the target in
	// a T, not in the A-pose it happens to be authored in, because the two rigs have to agree about
	// the pose in space and only one of them is describing it. The alignment term is exactly that
	// disagreement, and it is identity wherever the two rigs already rest the same way - so on a
	// source rig that rests like the target this reduces to the rest offset alone.
	TArray<FQuat> RestOffset;
	RestOffset.Init(FQuat::Identity, NumBones);

	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		const int32 Joint = BoneToJoint[Bone];
		if (Joint != INDEX_NONE)
		{
			RestOffset[Bone] = Clip.RestGlobalRotations[Joint].Inverse()
				* RestAlign[Bone]
				* RefGlobal[Bone].GetRotation();
			RestOffset[Bone].Normalize();
		}
	}

	// -----------------------------------------------------------------------------------------
	// Frames to keep
	// -----------------------------------------------------------------------------------------

	const int32 SourceFrames = Clip.NumFrames();

	int32 FirstFrame = 0;
	int32 LastFrame = SourceFrames - 1;

	if (Request.TrimWindow.Y > Request.TrimWindow.X && Request.TrimWindow.Y > 0.0)
	{
		FirstFrame = FMath::Clamp(FMath::RoundToInt(Request.TrimWindow.X * Clip.FrameRate), 0, SourceFrames - 1);
		LastFrame = FMath::Clamp(FMath::RoundToInt(Request.TrimWindow.Y * Clip.FrameRate), FirstFrame, SourceFrames - 1);
	}

	const int32 NumKeys = LastFrame - FirstFrame + 1;
	if (NumKeys < 2)
	{
		Result.Error = FString::Printf(
			TEXT("The trim window keeps only %d frame(s) of a %d frame clip. Widen it or clear it."),
			NumKeys, SourceFrames);
		return Result;
	}

	// -----------------------------------------------------------------------------------------
	// Root path and facing
	// -----------------------------------------------------------------------------------------

	// Every value this build puts the clip in a different frame with, in one place, so that reading
	// the result back out again reverses the same three numbers rather than a second guess at them.
	const FMotionClipFrame Placement = MeasureFrame(Clip, Map, Ref, FirstFrame);
	const double Scale = Placement.Scale;

	if (!FMath::IsNearlyEqual(Scale, 1.0, 0.02))
	{
		UE_LOG(LogMotionForge, Log,
			TEXT("Scaling motion by %.3f - the generator's rig stands %.1fcm at the hips, '%s' stands %.1fcm."),
			Scale, Clip.RestGlobalPositions[Clip.HipsJointIndex].Z,
			*Request.TargetSkeleton->GetName(), RefGlobal[HipsBone].GetTranslation().Z);
	}

	const bool bHaveRootPath = Clip.RootPath.Num() == SourceFrames;
	const bool bHaveRootFacing = Clip.RootFacing.Num() == SourceFrames;

	TArray<FVector> Path;
	TArray<FQuat> Facing;
	Path.Reserve(NumKeys);
	Facing.Reserve(NumKeys);

	for (int32 Frame = FirstFrame; Frame <= LastFrame; ++Frame)
	{
		const FVector Raw = bHaveRootPath ? Clip.RootPath[Frame] : Clip.HipsPositions[Frame];
		Path.Add(FVector(Raw.X, Raw.Y, 0.0));

		Facing.Add(bHaveRootFacing
			? FlattenToYaw(Clip.RootFacing[Frame])
			: FlattenToYaw(Clip.GlobalRotations[Frame][Clip.HipsJointIndex]));
	}

	// Start every clip at the origin facing the way the target skeleton does. Without the squaring up
	// a take generated walking north-east arrives rotated in the asset and every montage built on it
	// inherits the offset; without the target's own heading it gets squared up to +X, which is only
	// forward for skeletons that happen to agree with Unreal about that.
	//
	// This is the single place a clip's heading is decided, so it is also the only place worth
	// correcting one. Turning the source data upstream achieves nothing - it arrives here and gets
	// normalised straight back - and turning the *rig* upstream without turning the clip leaves the
	// two disagreeing, which a retargeter faithfully reproduces as a skew.
	const FQuat Unrotate = Placement.Unrotate;
	const FVector FirstGround = Placement.FirstGround;

	// -----------------------------------------------------------------------------------------
	// Per-frame tracks
	// -----------------------------------------------------------------------------------------

	// Only bones that actually move get a track. Anything else stays on the reference pose, which is
	// both correct and much smaller than writing rest keys for three hundred bones.
	TArray<int32> TrackedBones;
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		if (BoneToJoint[Bone] != INDEX_NONE || Bone == RootBone || Bone == HipsBone)
		{
			TrackedBones.Add(Bone);
		}
	}

	TMap<int32, TArray<FVector3f>> Positions;
	TMap<int32, TArray<FQuat4f>> Rotations;
	TMap<int32, TArray<FVector3f>> Scales;

	for (int32 Bone : TrackedBones)
	{
		Positions.Add(Bone).Reserve(NumKeys);
		Rotations.Add(Bone).Reserve(NumKeys);
		Scales.Add(Bone).Reserve(NumKeys);
	}

	TArray<FQuat> Desired;
	Desired.SetNum(NumBones);

	for (int32 Key = 0; Key < NumKeys; ++Key)
	{
		const int32 Frame = FirstFrame + Key;
		const TArray<FQuat>& SourceGlobals = Clip.GlobalRotations[Frame];

		const FQuat RootRotation = Request.bZeroRootTranslation
			? FQuat::Identity
			: (Unrotate * Facing[Key]);

		FVector RootTranslation = Unrotate.RotateVector((Path[Key] - FirstGround) * Scale);

		// Hips keep their height; only the horizontal start is moved to the origin.
		const FVector RawHips = Clip.HipsPositions[Frame];
		FVector HipsWorld = Unrotate.RotateVector(
			FVector(RawHips.X - FirstGround.X, RawHips.Y - FirstGround.Y, RawHips.Z) * Scale);

		if (Request.bZeroRootTranslation)
		{
			// In place: take the travel out of both, leaving the bob, the sway and any turn on the
			// body. The character still turns and shifts weight, it just stops going anywhere.
			HipsWorld -= RootTranslation;
			RootTranslation = FVector::ZeroVector;
		}

		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			const int32 Parent = Ref.GetParentIndex(Bone);
			const FQuat ParentGlobal = (Parent == INDEX_NONE) ? FQuat::Identity : Desired[Parent];

			if (Bone == RootBone)
			{
				Desired[Bone] = RootRotation;
			}
			else if (BoneToJoint[Bone] != INDEX_NONE)
			{
				// Unrotate turns the whole character to face forward at frame zero. Applied on the
				// left because it is a rotation of the world, not of the bone.
				const FQuat World = Unrotate * SourceGlobals[BoneToJoint[Bone]];

				// The rest term on the right, so it is read first and the source's world rotation is
				// applied to the result. There used to be a setting here offering the other two
				// orderings; they are wrong, not merely untried. The source's rotation is a turn *in
				// the world*, because that is what a global orientation is, so the only correct place
				// for it is outermost. What the orderings were really standing in for was a missing
				// rest term - with the rest rotations identity, two of the three were literally the
				// same expression - and that gap is now filled by `RestAlign` above.
				Desired[Bone] = World * RestOffset[Bone];
				Desired[Bone].Normalize();
			}
			else
			{
				Desired[Bone] = ParentGlobal * RefLocal[Bone].GetRotation();
			}
		}

		for (int32 Bone : TrackedBones)
		{
			const int32 Parent = Ref.GetParentIndex(Bone);
			const FQuat ParentGlobal = (Parent == INDEX_NONE) ? FQuat::Identity : Desired[Parent];

			FQuat Local = ParentGlobal.Inverse() * Desired[Bone];
			Local.Normalize();

			FVector Translation = RefLocal[Bone].GetTranslation();

			if (Bone == RootBone)
			{
				Translation = RootTranslation;
			}
			else if (Bone == HipsBone)
			{
				// Relative to whatever the root ended up being, so the two never double-count travel.
				Translation = (RootBone != INDEX_NONE)
					? RootRotation.Inverse().RotateVector(HipsWorld - RootTranslation)
					: HipsWorld;
			}

			Positions[Bone].Add(FVector3f(Translation));
			Rotations[Bone].Add(FQuat4f(Local));
			Scales[Bone].Add(FVector3f(RefLocal[Bone].GetScale3D()));
		}
	}

	// -----------------------------------------------------------------------------------------
	// The asset
	// -----------------------------------------------------------------------------------------

	const FString PackageName = Request.DestinationPackagePath / Request.AssetName;
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Request.AssetName);

	// Reuse rather than duplicate. Regenerating a definition should replace its animation in place,
	// so everything already referencing it keeps working.
	UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	UPackage* Package = Sequence ? Sequence->GetOutermost() : CreatePackage(*PackageName);

	const bool bCreated = (Sequence == nullptr);
	if (bCreated)
	{
		Sequence = NewObject<UAnimSequence>(Package, FName(*Request.AssetName), RF_Public | RF_Standalone);
	}

	Sequence->SetSkeleton(Request.TargetSkeleton);

	{
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.OpenBracket(LOCTEXT("BuildMotion", "Building generated motion"));

		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(Clip.FrameRate, 1));
		Controller.SetNumberOfFrames(FFrameNumber(NumKeys - 1));

		for (int32 Bone : TrackedBones)
		{
			const FName BoneName = Ref.GetBoneName(Bone);
			Controller.AddBoneCurve(BoneName);
			Controller.SetBoneTrackKeys(BoneName, Positions[Bone], Rotations[Bone], Scales[Bone]);
		}

		// The bones this generator never drove, posed from somewhere better than the reference pose.
		//
		// **This is the fingers**, and it is the only place they can come from: a model predicting
		// thirty joints predicts none of them, so without this every clip wears one fixed hand shape
		// for its whole length - the shape that puts a thumb inside a thigh on every frame of any clip
		// with its arms down.
		//
		// Only bones that actually differ get a track. Writing a constant curve for all hundred-odd
		// untracked bones would double the asset to say nothing.
		if (Request.UntrackedPoseSource)
		{
			const TMap<FName, FTransform> Pose = MotionUntracked::ReadPose(
				Request.UntrackedPoseSource, Request.UntrackedPoseTime, Ref);

			int32 Posed = 0;

			for (const TPair<FName, FTransform>& Entry : Pose)
			{
				const int32 Bone = Ref.FindBoneIndex(Entry.Key);

				if (Bone == INDEX_NONE || TrackedBones.Contains(Bone))
				{
					continue;
				}

				if (Entry.Value.Equals(RefLocal[Bone], 0.01f))
				{
					continue;
				}

				TArray<FVector3f> PosedPositions;
				TArray<FQuat4f> PosedRotations;
				TArray<FVector3f> PosedScales;

				PosedPositions.Init(FVector3f(Entry.Value.GetLocation()), NumKeys);
				PosedRotations.Init(FQuat4f(Entry.Value.GetRotation()), NumKeys);
				PosedScales.Init(FVector3f(Entry.Value.GetScale3D()), NumKeys);

				Controller.AddBoneCurve(Entry.Key);
				Controller.SetBoneTrackKeys(Entry.Key, PosedPositions, PosedRotations, PosedScales);

				++Posed;
			}

			UE_LOG(LogMotionForge, Log,
				TEXT("Posed %d bone(s) the generator does not drive from '%s'."),
				Posed, *Request.UntrackedPoseSource->GetName());
		}

		// Contact flags and anything else the generator measured. Free information that would
		// otherwise have to be re-derived by hand, and the raw material for a foot lock.
		for (const TPair<FName, TArray<float>>& Curve : Clip.Curves)
		{
			if (Curve.Value.Num() != SourceFrames)
			{
				continue;
			}

			const FAnimationCurveIdentifier CurveId(Curve.Key, ERawCurveTrackTypes::RCT_Float);
			Controller.AddCurve(CurveId);

			TArray<FRichCurveKey> Keys;
			Keys.Reserve(NumKeys);
			for (int32 Key = 0; Key < NumKeys; ++Key)
			{
				Keys.Emplace(static_cast<float>(Key) / Clip.FrameRate, Curve.Value[FirstFrame + Key]);
			}

			Controller.SetCurveKeys(CurveId, Keys);
		}

		Controller.NotifyPopulated();
		Controller.CloseBracket();
	}

	Sequence->PostEditChange();
	Sequence->MarkPackageDirty();

	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Sequence);
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;

	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());

	if (!UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs))
	{
		UE_LOG(LogMotionForge, Warning,
			TEXT("Built '%s' but could not save it. The asset exists in memory; save it by hand."),
			*ObjectPath);
	}

	UE_LOG(LogMotionForge, Log,
		TEXT("Built %s: %d keys at %dfps (%.2fs), %d bone track(s), %d curve(s)."),
		*Request.AssetName, NumKeys, Clip.FrameRate,
		static_cast<float>(NumKeys - 1) / Clip.FrameRate, TrackedBones.Num(), Clip.Curves.Num());

	Result.bSuccess = true;
	Result.Sequence = Sequence;
	return Result;
}

#undef LOCTEXT_NAMESPACE
