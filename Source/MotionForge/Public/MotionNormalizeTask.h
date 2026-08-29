// Running the Blender normalisation pass.

#pragma once

#include "CoreMinimal.h"

/** What normalisation needs to know. */
struct FMotionNormalizeRequest
{
	FString AbsoluteInputPath;
	FString AbsoluteOutputPath;

	/** Seconds to keep. Zero length keeps the whole clip. */
	FVector2D TrimWindow = FVector2D::ZeroVector;

	int32 FrameRate = 30;
	bool  bZeroRootTranslation = true;
	bool  bEnsureRootBone = true;
};

struct FMotionNormalizeResult
{
	bool    bSuccess = false;

	/** The file the importer should take. Falls back to the input when normalisation was skipped. */
	FString OutputPath;

	/** True when Blender was not configured and the raw file is being passed through. */
	bool    bSkipped = false;

	FString Error;
};

/**
 * Invokes the shipped Blender script on a downloaded file.
 *
 * Blocking, and deliberately so - it runs per file inside a pipeline that is already asynchronous at
 * the network layer, and a subprocess that takes a second or two is not worth another layer of
 * callbacks. The timeout in settings stops a wedged Blender hanging the editor indefinitely.
 *
 * When no Blender path is configured this reports success with bSkipped set, so the pipeline carries
 * on and imports the provider's file untouched. That clip will usually have no root bone and baked-in
 * translation, which is worth a warning but not worth failing over.
 */
class MOTIONFORGE_API FMotionNormalizeTask
{
public:

	static FMotionNormalizeResult Run(const FMotionNormalizeRequest& Request);

	/** Absolute path of the normalisation script shipped with the plugin. */
	static FString GetScriptPath();
};
