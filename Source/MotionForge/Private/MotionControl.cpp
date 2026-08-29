#include "MotionControl.h"

#include "MotionForge.h"

#include "Animation/AnimTypes.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "BoneContainer.h"
#include "BonePose.h"
#include "Misc/MemStack.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesContainer.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneMotionConstraintTrack.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannel.h"
#include "Animation/AnimSequence.h"
#include "Exporters/AnimSeqExportOption.h"
#include "MovieSceneToolHelpers.h"

namespace MotionControlPrivate
{
	/** Reference pose in component space. The reference skeleton always lists parents first. */
	static TArray<FTransform> RefGlobals(const FReferenceSkeleton& Ref)
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

	/** Which pose of the asset the key means, with the ambiguity spelled out rather than guessed at. */
	static bool ChoosePose(
		const UPoseAsset& Asset, FName Wanted, FName& OutChosen, int32& OutIndex, FString& OutError)
	{
		const TArray<FName>& Names = Asset.GetPoseFNames();

		if (Names.Num() == 0)
		{
			OutError = FString::Printf(TEXT("'%s' holds no poses."), *Asset.GetName());
			return false;
		}

		if (!Wanted.IsNone())
		{
			if (!Names.Contains(Wanted))
			{
				OutError = FString::Printf(TEXT("'%s' has no pose called '%s'. It has: %s."),
					*Asset.GetName(), *Wanted.ToString(),
					*FString::JoinBy(Names, TEXT(", "), [](const FName& N) { return N.ToString(); }));
				return false;
			}

			OutChosen = Wanted;
			OutIndex = Names.IndexOfByKey(Wanted);
			return true;
		}

		if (Names.Num() > 1)
		{
			// Deliberately not the first one. An asset made from an animation has frame zero as
			// `Pose_0`, which is normally the idle it began from, so quietly taking the first would
			// pin the clip to a resting pose and read as the model ignoring the constraint.
			OutError = FString::Printf(
				TEXT("'%s' holds %d poses, so Pose Name has to say which. It has: %s."),
				*Asset.GetName(), Names.Num(),
				*FString::JoinBy(Names, TEXT(", "), [](const FName& N) { return N.ToString(); }));
			return false;
		}

		OutChosen = Names[0];
		OutIndex = 0;
		return true;
	}
}

bool FMotionPoseResolve::Resolve(
	const FMotionConstraintKey& Key,
	const FReferenceSkeleton& Ref,
	EMotionConstraintType Type,
	const TSet<FName>& RequiredBones,
	FMotionPoseSample& OutPose,
	int32& OutCoverage,
	FString& OutError)
{
	using namespace MotionControlPrivate;

	OutCoverage = 0;

	UPoseAsset* Asset = Key.PoseAsset.LoadSynchronous();

	// Nothing named, so the captured pose is the pose. Not an error to have neither - a Root Path key
	// constrains where the body is without saying anything about its shape.
	if (!Asset)
	{
		OutPose = Key.Pose;
		OutCoverage = Key.Pose.Bones.Num();
		return true;
	}

	// An additive pose is a difference from a base, not a pose. Every number in it is valid and means
	// something else entirely, so used as an absolute constraint it folds the character up without
	// anything reporting a fault.
	if (Asset->IsValidAdditive())
	{
		OutError = FString::Printf(
			TEXT("'%s' is an additive pose asset. Additive poses are differences from a base pose - "
				 "'twenty degrees further than wherever you were' - so there is no absolute pose in it "
				 "to constrain to. Use a full pose asset, or capture the pose instead."),
			*Asset->GetName());
		return false;
	}

	FName Chosen;
	int32 ChosenIndex = INDEX_NONE;
	if (!ChoosePose(*Asset, Key.PoseName, Chosen, ChosenIndex, OutError))
	{
		return false;
	}

	USkeleton* Skeleton = Asset->GetSkeleton();
	if (!Skeleton)
	{
		OutError = FString::Printf(TEXT("'%s' has no skeleton."), *Asset->GetName());
		return false;
	}

	// Coverage first, because it decides whether this key is allowed at all and the answer is cheap.
	// A pose asset stores only the bone tracks it was built with - corrective and hand-shape poses
	// hold a handful on purpose - and what fills the rest is context the asset does not carry.
	const TArray<FName>& Tracks = Asset->GetTrackNames();

	TSet<FName> Present;
	int32 Covered = 0;
	for (const FName& Track : Tracks)
	{
		if (Ref.FindBoneIndex(Track) != INDEX_NONE)
		{
			Present.Add(Track);
			++Covered;
		}
	}

	// Only Full Body cares. Every other type honours one effector and never looks at the filler, so a
	// hand-shape asset driving a hand key is exactly right and refusing it would be silly.
	if (Type == EMotionConstraintType::FullBody)
	{
		// Against the bones that are actually read, not against the whole skeleton.
		//
		// A pose asset built from an animation carries only the bones that animation moved - two dozen
		// out of Quinn's three hundred and sixty one - and the twist, finger and IK bones it leaves out
		// were sitting at the reference pose in the animation too. Demanding the whole skeleton refuses
		// a pose that is genuinely whole-body, which is how this rule was written the first time.
		TArray<FString> Absent;
		for (const FName& Wanted : RequiredBones.Num() > 0 ? RequiredBones : Present)
		{
			if (!Present.Contains(Wanted))
			{
				Absent.Add(Wanted.ToString());
			}
		}

		if (Absent.Num() > 0)
		{
			Absent.Sort();

			OutError = FString::Printf(
				TEXT("'%s' does not carry %d of the bones this constraint reads (%s%s), so a Full Body "
					 "key from it would pin those to the reference pose - a character standing to "
					 "attention around whatever the asset does cover. Point this key at a whole-body "
					 "pose, or change it to the end effector you actually mean."),
				*Asset->GetName(), Absent.Num(),
				*FString::Join(TArrayView<const FString>(Absent.GetData(), FMath::Min(Absent.Num(), 6)),
					TEXT(", ")),
				Absent.Num() > 6 ? TEXT(", ...") : TEXT(""));
			return false;
		}
	}

	// Evaluate. The pose asset is driven by curves named after its poses, so weighting ours to one and
	// the rest to zero extracts it exactly as an anim graph would.
	TArray<FBoneIndexType> Required;
	Required.Reserve(Ref.GetNum());
	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
	{
		Required.Add(static_cast<FBoneIndexType>(Bone));
	}

	// FCompactPose allocates from the thread's FMemStack, and the stack asserts unless a mark is
	// scoping the allocation. An anim graph always has one; called from a tool, nothing does - so
	// evaluating a pose asset without this is not a leak or a wrong answer, it is an editor crash on
	// the first key that names an asset.
	FMemMark Mark(FMemStack::Get());

	FBoneContainer BoneContainer;
	BoneContainer.InitializeTo(Required, UE::Anim::FCurveFilterSettings(), *Skeleton);

	FCompactPose CompactPose;
	CompactPose.SetBoneContainer(&BoneContainer);
	CompactPose.ResetToRefPose();

	FBlendedCurve Curve;
	UE::Anim::FStackAttributeContainer Attributes;
	FAnimationPoseData PoseData(CompactPose, Curve, Attributes);

	// Which pose to extract travels in `PoseCurves` on the extraction context, carrying the pose's
	// *index* as well as its name - not in the FBlendedCurve, which is an output here.
	//
	// Worth being exact about, because getting it wrong fails quietly in both directions. Leave
	// PoseCurves empty and `NumPoses()` is zero, so the evaluation is skipped entirely and
	// `GetAnimationPose` returns false with nothing said about why. Weighting a curve named after the
	// pose looks like it ought to work - that is how a Pose By Name node reads to the eye - and does
	// nothing at all.
	//
	// One entry at weight 1.0 takes the single-pose path, which copies the pose's tracks straight out
	// rather than blending, so nothing is lost to interpolation.
	FAnimExtractContext Context;
	Context.PoseCurves.Add(FPoseCurve(ChosenIndex, Chosen, 1.0f));

	if (!Asset->GetAnimationPose(PoseData, Context))
	{
		OutError = FString::Printf(
			TEXT("Could not evaluate pose '%s' from '%s'."), *Chosen.ToString(), *Asset->GetName());
		return false;
	}

	// Local to component space, in the order the reference skeleton guarantees: parents first.
	const TArray<FTransform> RestGlobal = RefGlobals(Ref);

	TArray<FTransform> Global;
	Global.SetNum(Ref.GetNum());

	OutPose = FMotionPoseSample();
	OutPose.Bones.Empty(Ref.GetNum());

	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
	{
		const FCompactPoseBoneIndex CompactIndex =
			BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(Bone));

		const FTransform Local = CompactIndex.IsValid()
			? CompactPose[CompactIndex]
			: Ref.GetRefBonePose()[Bone];

		const int32 Parent = Ref.GetParentIndex(Bone);
		Global[Bone] = (Parent == INDEX_NONE) ? Local : Local * Global[Parent];

		OutPose.Bones.Add(Ref.GetBoneName(Bone), Global[Bone]);
	}

	OutCoverage = Covered;

	OutPose.Source = FString::Printf(TEXT("%s, pose '%s'%s"),
		*Asset->GetName(), *Chosen.ToString(),
		(Covered < Ref.GetNum())
			? *FString::Printf(TEXT(" (%d of %d bones, rest from ref pose)"), Covered, Ref.GetNum())
			: TEXT(""));

	return true;
}

// -------------------------------------------------------------------------------------------------
// Harvesting constraints from a Level Sequence
// -------------------------------------------------------------------------------------------------

namespace MotionSequencePrivate
{
	/** Root tracks and every binding's tracks, which between them is everything in the sequence. */
	static TArray<UMovieSceneTrack*> AllTracksIn(const UMovieScene& MovieScene)
	{
		TArray<UMovieSceneTrack*> Tracks;

		for (UMovieSceneTrack* Track : MovieScene.GetTracks())
		{
			Tracks.Add(Track);
		}

		// Bindings hold the interesting ones. A character's Control Rig, transform and animation
		// tracks all hang off its binding rather than off the movie scene directly.
		for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
		{
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				Tracks.Add(Track);
			}
		}

		return Tracks;
	}

	/** Every time anything in the sequence is keyed, plus any marked frames, in tick resolution. */
	static void CollectKeyTimes(const UMovieScene& MovieScene, TSet<FFrameNumber>& OutTimes)
	{
		for (UMovieSceneTrack* Track : AllTracksIn(MovieScene))
		{
			if (!Track)
			{
				continue;
			}

			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				if (!Section)
				{
					continue;
				}

				const FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

				for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
				{
					for (FMovieSceneChannel* Channel : Entry.GetChannels())
					{
						if (!Channel)
						{
							continue;
						}

						TArray<FFrameNumber> Times;
						Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);

						for (const FFrameNumber& Time : Times)
						{
							OutTimes.Add(Time);
						}
					}
				}
			}
		}

		// Marked frames are the escape hatch: a moment worth constraining that nothing happens to be
		// keyed on, or a way to constrain a sequence driven entirely by an unkeyed animation track.
		for (const FMovieSceneMarkedFrame& Marked : MovieScene.GetMarkedFrames())
		{
			OutTimes.Add(Marked.FrameNumber);
		}
	}

	/**
	 * One instant of an animation, as component-space transforms on the reference skeleton.
	 *
	 * By time, not by frame index. The bake's own frame rate is nobody's business but its own, and
	 * asking it for "frame 75" would silently mean a different instant the moment that rate differed
	 * from the sequence's or the generator's.
	 */
	static bool SampleAtTime(
		UAnimSequence& Animation,
		double TimeSeconds,
		const FReferenceSkeleton& Ref,
		USkeleton& Skeleton,
		TMap<FName, FTransform>& OutBones)
	{
		TArray<FBoneIndexType> Required;
		Required.Reserve(Ref.GetNum());
		for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
		{
			Required.Add(static_cast<FBoneIndexType>(Bone));
		}

		// See the pose asset path: a compact pose allocates from the thread's memory stack, which
		// asserts without a mark scoping it.
		FMemMark Mark(FMemStack::Get());

		FBoneContainer BoneContainer;
		BoneContainer.InitializeTo(Required, UE::Anim::FCurveFilterSettings(), Skeleton);

		FCompactPose CompactPose;
		CompactPose.SetBoneContainer(&BoneContainer);
		CompactPose.ResetToRefPose();

		FBlendedCurve Curve;
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData PoseData(CompactPose, Curve, Attributes);

		Animation.GetAnimationPose(PoseData, FAnimExtractContext(TimeSeconds));

		TArray<FTransform> Global;
		Global.SetNum(Ref.GetNum());

		OutBones.Empty(Ref.GetNum());

		for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
		{
			const FCompactPoseBoneIndex CompactIndex =
				BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(Bone));

			const FTransform Local = CompactIndex.IsValid()
				? CompactPose[CompactIndex]
				: Ref.GetRefBonePose()[Bone];

			const int32 Parent = Ref.GetParentIndex(Bone);
			Global[Bone] = (Parent == INDEX_NONE) ? Local : Local * Global[Parent];

			OutBones.Add(Ref.GetBoneName(Bone), Global[Bone]);
		}

		return OutBones.Num() > 0;
	}

	/** The first bound skeletal mesh component whose skeleton is the one we expect. */
	static USkeletalMeshComponent* FindPosedComponent(
		IMovieScenePlayer& Player,
		const UMovieScene& MovieScene,
		const USkeleton* Expected,
		FString& OutFound)
	{
		for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
		{
			for (TWeakObjectPtr<> Bound : Player.FindBoundObjects(Binding.GetObjectGuid(), MovieSceneSequenceID::Root))
			{
				AActor* Actor = Cast<AActor>(Bound.Get());
				USkeletalMeshComponent* Component =
					Actor ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr;

				if (!Component || !Component->GetSkinnedAsset())
				{
					continue;
				}

				const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Component->GetSkinnedAsset());

				if (Expected && Mesh && Mesh->GetSkeleton() != Expected)
				{
					continue;
				}

				OutFound = Actor->GetActorNameOrLabel();
				return Component;
			}
		}

		return nullptr;
	}
}

EMotionHarvestResult FMotionSequenceConstraints::Harvest(
	ULevelSequence* Sequence,
	UWorld* World,
	const USkeleton* ExpectedSkeleton,
	int32 FrameRate,
	EMotionConstraintType DefaultType,
	TArray<FMotionConstraint>& OutConstraints,
	double& OutDurationSeconds,
	TArray<FString>& OutWarnings,
	FString& OutError)
{
	using namespace MotionSequencePrivate;

	OutConstraints.Reset();
	OutDurationSeconds = 0.0;

	// Keys are gathered flat and grouped at the end, because a key's type is decided by where it sits
	// rather than by which list it went into.
	TArray<FMotionConstraintKey> OutKeys;
	TArray<EMotionConstraintType> KeyTypes;

	if (!Sequence)
	{
		OutError = TEXT("No Level Sequence to read constraints from.");
		return EMotionHarvestResult::Failed;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = FString::Printf(TEXT("'%s' has no movie scene in it."), *Sequence->GetName());
		return EMotionHarvestResult::Failed;
	}

	if (!World)
	{
		OutError = TEXT("No world to evaluate the sequence in.");
		return EMotionHarvestResult::Failed;
	}

	if (FrameRate <= 0)
	{
		OutError = TEXT("The generator's frame rate is not known, so sequence times cannot be converted.");
		return EMotionHarvestResult::Failed;
	}

	const FFrameRate Tick = MovieScene->GetTickResolution();

	// Optional. Without one every key takes the sequence-wide type, which is the ordinary case.
	const UMovieSceneMotionConstraintTrack* ConstraintTrack = nullptr;
	for (UMovieSceneTrack* Track : AllTracksIn(*MovieScene))
	{
		if (const UMovieSceneMotionConstraintTrack* Found =
				Cast<UMovieSceneMotionConstraintTrack>(Track))
		{
			ConstraintTrack = Found;
			break;
		}
	}

	TSet<FFrameNumber> Times;
	CollectKeyTimes(*MovieScene, Times);

	// Not a failure. A sequence carrying only a prompt track is the ordinary case now, and it is not
	// claiming to supply poses at all - so the caller should use whatever was authored on the asset
	// rather than being left with nothing. See EMotionHarvestResult.
	if (Times.Num() == 0)
	{
		OutError = FString::Printf(
			TEXT("Nothing in '%s' is keyed, so it supplies no constraint poses. Key the character on "
				 "the frames that matter - the pose it starts in, the beat in the middle, the pose it "
				 "ends on - or add marked frames."),
			*Sequence->GetName());
		return EMotionHarvestResult::NothingKeyed;
	}

	TArray<FFrameNumber> Ordered = Times.Array();
	Ordered.Sort();

	const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
	if (Playback.HasLowerBound() && Playback.HasUpperBound())
	{
		OutDurationSeconds =
			Tick.AsSeconds(Playback.GetUpperBoundValue() - Playback.GetLowerBoundValue());
	}

	// Sparse beats dense, and this is the route where that is easiest to get wrong: auto-key puts a
	// key down on every nudge, and a hundred constrained frames is not a generation, it is a replay
	// of the artist's own animation with extra steps.
	if (Ordered.Num() > 20)
	{
		OutWarnings.Add(FString::Printf(
			TEXT("'%s' is keyed on %d distinct times, which will become %d constraints. Past about "
				 "twenty the model spends its capacity satisfying keys instead of making motion. If "
				 "this is auto-key noise rather than deliberate poses, thin it out or use marked "
				 "frames instead."),
			*Sequence->GetName(), Ordered.Num(), Ordered.Num()));
	}

	// Wake anything that has been muted, **before the player exists.**
	//
	// A constraint rig is meant to sit disabled: it holds the poses you want pinned, and muting it is
	// what lets the generated take play underneath as a preview. But a disabled track does not
	// evaluate, so the bake would sample the preview instead - producing constraints that are the
	// model's own output, faithfully fed back to it, with nothing anywhere saying so.
	//
	// The order is the whole fix. `bIsEvalDisabled` is read when the sequence is **compiled**, so
	// clearing it after the player has been created and played changes a flag nothing will look at
	// again: the first version of this did exactly that and worked only when the rig happened to be
	// enabled already. Reported as "generating with the rig track turned off does not work", which is
	// precisely what it was.
	//
	// Key *times* are collected straight off the channels and never had this problem, which is what
	// makes the failure quiet: the right number of constraints, at the right moments, holding the
	// wrong poses.
	TArray<UMovieSceneTrack*> Muted;

	for (UMovieSceneTrack* Track : AllTracksIn(*MovieScene))
	{
		if (Track && Track->IsEvalDisabled())
		{
			Muted.Add(Track);
			Track->SetEvalDisabled(false);
#if WITH_EDITOR
			Track->SetLocalEvalDisabled(false);
#endif
		}
	}

	// Declared before the player's own scope guard, so it runs *after* it - the player is stopped and
	// gone before anything is muted again.
	ON_SCOPE_EXIT
	{
		for (UMovieSceneTrack* Track : Muted)
		{
			Track->SetEvalDisabled(true);
		}
	};

	// A player rather than the Sequencer UI, so this works from a tool, from a commandlet, and with
	// nothing open. It poses the bound actor in the level for as long as it runs and restores it after.
	ALevelSequenceActor* Actor = nullptr;
	ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(
		World, Sequence, FMovieSceneSequencePlaybackSettings(), Actor);

	if (!Player)
	{
		OutError = FString::Printf(
			TEXT("Could not create a player for '%s'."), *Sequence->GetName());
		return EMotionHarvestResult::Failed;
	}

	ON_SCOPE_EXIT
	{
		Player->Stop();
		if (Actor)
		{
			Actor->Destroy();
		}
	};

	Player->Play();
	Player->Pause();

	FString ActorName;

	USkeletalMeshComponent* Posed =
		FindPosedComponent(*Player, *MovieScene, ExpectedSkeleton, ActorName);

	if (!Posed)
	{
		OutError = FString::Printf(
			TEXT("'%s' has nothing bound to it with the right skeleton%s. Add the character to the "
				 "sequence, on the skeleton this definition generates for."),
			*Sequence->GetName(),
			ExpectedSkeleton ? *FString::Printf(TEXT(" (%s)"), *ExpectedSkeleton->GetName()) : TEXT(""));
		return EMotionHarvestResult::Failed;
	}

	USkeleton* PosedSkeleton = Posed->GetSkinnedAsset() ? Posed->GetSkinnedAsset()->GetSkeleton() : nullptr;
	if (!PosedSkeleton)
	{
		OutError = FString::Printf(TEXT("'%s' has no skeleton to sample."), *ActorName);
		return EMotionHarvestResult::Failed;
	}

	// Bake once, into nothing.
	//
	// Scrubbing the player and reading the component back does not work: the evaluation never reaches
	// the component in an editor world, and every scrub position returns the pose the actor is already
	// standing in. Sequencer's own export does reach it, and it is the only thing that knows how to
	// resolve a Control Rig - which is what an artist will actually be posing with.
	//
	// The asset is transient and never saved. What is wanted from it is a handful of instants; the
	// interpolation it necessarily contains between them is not read and would be wrong to read, since
	// the whole point is that the model invents the in-betweens.
	UAnimSequence* Baked = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
	Baked->SetSkeleton(PosedSkeleton);

	UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>();
	// Transforms and nothing else. Curves, morph targets and timecode are all irrelevant to a pose,
	// and every one of them costs bake time on a clip that is about to be thrown away.
	ExportOptions->bExportTransforms = true;
	ExportOptions->bExportMorphTargets = false;
	ExportOptions->bExportAttributeCurves = false;
	ExportOptions->bExportMaterialCurves = false;

	// Component space, not world. A constraint is measured from the character's own origin, so where
	// the actor happens to be standing in the level must not leak into the pose.
	ExportOptions->bRecordInWorldSpace = false;

	// No transaction. This bake is transient and never seen; putting it on the undo stack would give
	// the artist an undo entry for something that no longer exists.
	ExportOptions->bTransactRecording = false;

	FAnimExportSequenceParameters ExportParams;
	ExportParams.MovieSceneSequence = Sequence;
	ExportParams.RootMovieSceneSequence = Sequence;
	ExportParams.Player = Player;
	ExportParams.bForceUseOfMovieScenePlaybackRange = true;

	if (!MovieSceneToolHelpers::ExportToAnimSequence(Baked, ExportOptions, ExportParams, Posed))
	{
		OutError = FString::Printf(
			TEXT("Could not bake '%s' to sample its poses."), *Sequence->GetName());
		return EMotionHarvestResult::Failed;
	}

	ON_SCOPE_EXIT
	{
		Baked->MarkAsGarbage();
	};

	// The bake starts at the playback range, not at zero, so a sequence that begins at frame 100 has
	// its first baked instant at time zero. Every keyed time is measured from the same origin.
	const double StartSeconds = Playback.HasLowerBound()
		? Tick.AsSeconds(Playback.GetLowerBoundValue())
		: 0.0;

	// The *skeleton's* reference skeleton, not the mesh's.
	//
	// The bake is an animation on the skeleton, so it is evaluated in the skeleton's space and against
	// the skeleton's bone indices. Reading it out through the mesh's reference skeleton instead mixes
	// two index spaces that do not agree - Quinn's mesh carries 166 bones and its skeleton 361 - and
	// every bone lookup misses, so every sampled frame comes back as the reference pose. Identical at
	// every moment, which looks exactly like a sequence that never animated anything.
	const FReferenceSkeleton& Ref = PosedSkeleton->GetReferenceSkeleton();

	for (const FFrameNumber& Time : Ordered)
	{
		// Seconds, then frames, from the same origin the bake used. Neither the sequence's display
		// rate nor the bake's own rate enters into it.
		const double Seconds = Tick.AsSeconds(Time);

		FMotionConstraintKey Key;
		Key.Frame = FMath::Max(0, FMath::RoundToInt(Seconds * FrameRate));

		if (!SampleAtTime(*Baked, Seconds - StartSeconds, Ref, *PosedSkeleton, Key.Pose.Bones))
		{
			OutError = FString::Printf(
				TEXT("Nothing came back from the bake of '%s' at %.2fs."), *Sequence->GetName(), Seconds);
			return EMotionHarvestResult::Failed;
		}

		// No source mesh recorded, deliberately. These bones are in the *skeleton's* space, because
		// that is where an animation is evaluated - the same as a pose sampled from an AnimSequence,
		// and unlike one captured off a posed component. The conversion has to cancel through the rest
		// pose the sample was taken in, so naming a mesh here would send it through the wrong one.
		Key.Pose.Source = FString::Printf(TEXT("%s, %s at %.2fs"),
			*PosedSkeleton->GetName(), *Sequence->GetName(), Seconds);

		// Two keyed times can land on the same clip frame once seconds are rounded - a sequence at
		// 60fps feeding a 30fps generator halves the resolution. Keeping both would pin one instant to
		// two poses, which the model resolves by averaging into neither.
		// What this moment pins: a Motion Constraint span covering it, or the sequence-wide default.
		EMotionConstraintType TypeHere = DefaultType;

		if (ConstraintTrack)
		{
			ConstraintTrack->FindTypeAt(Time, TypeHere);
		}

		const int32 Existing = OutKeys.IndexOfByPredicate(
			[&Key](const FMotionConstraintKey& At) { return At.Frame == Key.Frame; });

		if (Existing != INDEX_NONE)
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Two keyed times both land on clip frame %d at %dfps; keeping the later one."),
				Key.Frame, FrameRate));

			OutKeys[Existing] = MoveTemp(Key);
			KeyTypes[Existing] = TypeHere;
			continue;
		}

		OutKeys.Add(MoveTemp(Key));
		KeyTypes.Add(TypeHere);
	}

	// A sample that is just the reference pose means the sequence never drove the character.
	//
	// This is the failure this route actually has, and it is silent by nature: the times are right,
	// the key count is right, the payload is well formed, and every frame carries the pose the actor
	// happened to be standing in. Sent, it reads as the model ignoring three constraints rather than
	// as three constraints that were never really taken - so it is refused here instead.
	//
	// **Measured against the rest pose, not against each other.** The earlier test - "every sample is
	// identical" - was wrong, and wrong at exactly the moment the feature works: holding one pose
	// across two keys is *the* gesture for stopping a limb drifting, and it produces two identical
	// samples on purpose. It refused the authored constraints that had just been proven to fix a
	// sagging arm.
	//
	// Causes seen: the binding resolving to an actor the sequence does not actually animate, and a
	// component that is not evaluating its animation in the editor world.
	if (OutKeys.Num() > 0)
	{
		const TArray<FTransform> RestGlobal = MotionControlPrivate::RefGlobals(Ref);

		bool bAnyDifference = false;

		for (int32 Index = 0; Index < OutKeys.Num() && !bAnyDifference; ++Index)
		{
			for (int32 Bone = 0; Bone < Ref.GetNum() && !bAnyDifference; ++Bone)
			{
				const FTransform* Sampled = OutKeys[Index].Pose.Bones.Find(Ref.GetBoneName(Bone));

				if (Sampled && !Sampled->Equals(RestGlobal[Bone], 0.01))
				{
					bAnyDifference = true;
				}
			}
		}

		if (!bAnyDifference)
		{
			OutError = FString::Printf(
				TEXT("All %d moments read from '%s' came back as the skeleton's rest pose, so the "
					 "sequence is not driving the character - the samples are just how it stands with "
					 "nothing posing it. Check that the binding resolves to that character, and that "
					 "whatever poses it is not muted."),
				OutKeys.Num(), *Sequence->GetName());

			OutKeys.Reset();
			return EMotionHarvestResult::Failed;
		}
	}

	// Grouped by type, one entry each, which is the shape the providers already read. A clip pinning a
	// hand in the middle and the whole body at the ends therefore needs nothing new downstream.
	for (int32 Index = 0; Index < OutKeys.Num(); ++Index)
	{
		FMotionConstraint* Group = OutConstraints.FindByPredicate(
			[Type = KeyTypes[Index]](const FMotionConstraint& At) { return At.Type == Type; });

		if (Group == nullptr)
		{
			FMotionConstraint Fresh;
			Fresh.Type = KeyTypes[Index];
			Group = &OutConstraints[OutConstraints.Add(MoveTemp(Fresh))];
		}

		Group->Keys.Add(MoveTemp(OutKeys[Index]));
	}

	return EMotionHarvestResult::Ok;
}
