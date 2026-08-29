// Getting a normalised FBX into the project as an animation sequence.

#pragma once

#include "CoreMinimal.h"

class USkeleton;
class UAnimSequence;
class USkeletalMesh;

struct FMotionImportRequest
{
	FString AbsoluteFbxPath;

	/** Content path to import into, e.g. "/Game/_Generated/Motion". */
	FString DestinationPackagePath;

	/** Asset name to create. Sanitised before use. */
	FString AssetName;

	/** Animations must bind to a skeleton - there is no meaningful default. */
	USkeleton* TargetSkeleton = nullptr;

	int32 FrameRate = 30;
};

struct FMotionImportResult
{
	bool bSuccess = false;
	TSoftObjectPtr<UAnimSequence> Sequence;
	FString Error;
};

/**
 * Imports one FBX as an animation sequence and nothing else.
 *
 * No montage, no curves, no notifies. Those are conventions belonging to whatever consumes these
 * animations, and baking them in here would tie this plugin to one project's way of working.
 */
class MOTIONFORGE_API FMotionImporter
{
public:

	static FMotionImportResult Import(const FMotionImportRequest& Request);

	/**
	 * Import an FBX as a skeletal mesh, creating a new skeleton from whatever rig it carries.
	 *
	 * Used once per character to bring the provider's own rig into the project, so generated clips
	 * have something to import onto that matches them exactly. Deliberately does not take a target
	 * skeleton: merging into an existing one is the thing this exists to avoid.
	 */
	static USkeletalMesh* ImportSkeletalMesh(
		const FString& AbsoluteFbxPath,
		const FString& DestinationPackagePath,
		const FString& AssetName,
		FString& OutError);
};
