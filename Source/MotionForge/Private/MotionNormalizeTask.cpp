#include "MotionNormalizeTask.h"

#include "MotionForge.h"
#include "MotionForgeSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

FString FMotionNormalizeTask::GetScriptPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MotionForge"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	return FPaths::ConvertRelativePathToFull(
		Plugin->GetBaseDir() / TEXT("Resources") / TEXT("normalize_motion.py"));
}

FMotionNormalizeResult FMotionNormalizeTask::Run(const FMotionNormalizeRequest& Request)
{
	FMotionNormalizeResult Result;

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	FString Reason;
	if (!Settings->IsBlenderConfigured(Reason))
	{
		// Not an error. The pipeline should still deliver something importable.
		UE_LOG(LogMotionForge, Warning, TEXT("Skipping normalisation: %s"), *Reason);
		Result.bSuccess = true;
		Result.bSkipped = true;
		Result.OutputPath = Request.AbsoluteInputPath;
		return Result;
	}

	const FString ScriptPath = GetScriptPath();
	if (ScriptPath.IsEmpty() || !FPaths::FileExists(ScriptPath))
	{
		Result.Error = FString::Printf(TEXT("Normalisation script missing at '%s'."), *ScriptPath);
		return Result;
	}

	FString Arguments = FString::Printf(
		TEXT("--background --python \"%s\" -- --in \"%s\" --out \"%s\" --fps %d"),
		*ScriptPath,
		*Request.AbsoluteInputPath,
		*Request.AbsoluteOutputPath,
		Request.FrameRate);

	if (Request.TrimWindow.Y > Request.TrimWindow.X)
	{
		Arguments += FString::Printf(TEXT(" --trim %.3f,%.3f"), Request.TrimWindow.X, Request.TrimWindow.Y);
	}

	if (Request.bZeroRootTranslation)
	{
		Arguments += TEXT(" --zero-root");
	}

	if (Request.bEnsureRootBone)
	{
		Arguments += TEXT(" --ensure-root");
	}

	UE_LOG(LogMotionForge, Verbose, TEXT("Blender: %s %s"), *Settings->BlenderExecutable.FilePath, *Arguments);

	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	FProcHandle Handle = FPlatformProcess::CreateProc(
		*Settings->BlenderExecutable.FilePath,
		*Arguments,
		/*bLaunchDetached*/ false,
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		nullptr,
		/*PriorityModifier*/ 0,
		nullptr,
		WritePipe);

	if (!Handle.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		Result.Error = FString::Printf(TEXT("Could not launch Blender at '%s'."),
			*Settings->BlenderExecutable.FilePath);
		return Result;
	}

	const double Deadline = FPlatformTime::Seconds() + FMath::Max(10, Settings->BlenderTimeoutSeconds);
	FString Output;

	while (FPlatformProcess::IsProcRunning(Handle))
	{
		Output += FPlatformProcess::ReadPipe(ReadPipe);

		if (FPlatformTime::Seconds() > Deadline)
		{
			FPlatformProcess::TerminateProc(Handle);
			FPlatformProcess::CloseProc(Handle);
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

			Result.Error = FString::Printf(TEXT("Blender timed out after %ds."), Settings->BlenderTimeoutSeconds);
			return Result;
		}

		FPlatformProcess::Sleep(0.05f);
	}

	Output += FPlatformProcess::ReadPipe(ReadPipe);

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(Handle, &ReturnCode);
	FPlatformProcess::CloseProc(Handle);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	if (ReturnCode != 0)
	{
		// Blender's own message is far more useful than the exit code, so surface it rather than
		// leaving it buried in a pipe.
		Result.Error = FString::Printf(TEXT("Blender exited with code %d.\n%s"), ReturnCode, *Output);
		return Result;
	}

	if (!FPaths::FileExists(Request.AbsoluteOutputPath))
	{
		Result.Error = FString::Printf(TEXT("Blender reported success but wrote no file at '%s'.\n%s"),
			*Request.AbsoluteOutputPath, *Output);
		return Result;
	}

	Result.bSuccess = true;
	Result.OutputPath = Request.AbsoluteOutputPath;
	return Result;
}
