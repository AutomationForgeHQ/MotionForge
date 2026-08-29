#include "MotionForgeSettings.h"

#include "MotionForge.h"
#include "MotionCredentialStore.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"

namespace MotionForgeSettingsPrivate
{
	/**
	 * Accept a content folder, or say why not.
	 *
	 * Deliberately strict about the mount point. A path under a root nothing has mounted produces
	 * packages that are created in memory, never written, and reported as successes - so the whole
	 * pipeline runs green and leaves nothing on disk.
	 */
	static bool ValidateContentRoot(const FString& In, FString& OutNormalised, FString& OutProblem)
	{
		OutNormalised = In.TrimStartAndEnd();

		while (OutNormalised.EndsWith(TEXT("/")))
		{
			OutNormalised.LeftChopInline(1);
		}

		if (OutNormalised.IsEmpty())
		{
			OutProblem = TEXT("Empty. Give a content path such as /Game/_EP1/Motion.");
			return false;
		}

		if (!OutNormalised.StartsWith(TEXT("/")))
		{
			OutProblem = FString::Printf(
				TEXT("'%s' is not a content path. It must start with a mount point, such as /Game/."),
				*OutNormalised);
			return false;
		}

		FText Reason;
		if (FPackageName::DoesPackageNameContainInvalidCharacters(OutNormalised, &Reason))
		{
			OutProblem = FString::Printf(TEXT("'%s' is not a usable content path: %s"),
				*OutNormalised, *Reason.ToString());
			return false;
		}

		if (FPackageName::GetPackageMountPoint(OutNormalised).IsNone())
		{
			OutProblem = FString::Printf(
				TEXT("Nothing is mounted at the root of '%s'. Content written there would be created ")
				TEXT("in memory and never saved. Use /Game/... unless you mean a specific plugin's ")
				TEXT("mount point."),
				*OutNormalised);
			return false;
		}

		return true;
	}
}

UMotionForgeSettings::UMotionForgeSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("MotionForge");
}

const UMotionForgeSettings* UMotionForgeSettings::Get()
{
	return GetDefault<UMotionForgeSettings>();
}

FMotionOutputPaths UMotionForgeSettings::GetOutputPaths() const
{
	FMotionOutputPaths Paths;

	Paths.Root        = OutputContentPath;
	Paths.Definitions = GetDefinitionsPath();
	Paths.Takes       = GetTakesPath();
	Paths.SourceTakes = GetSourceTakesPath();
	Paths.Characters  = GetCharactersPath();
	Paths.Rigs        = GetRigsPath();
	Paths.Sequences   = GetSequencesPath();
	Paths.Montages    = GetMontagesPath();

	return Paths;
}

FMotionOutputPaths UMotionForgeSettings::SetOutputRoot(const FString& ContentPath)
{
	UMotionForgeSettings* Settings = GetMutableDefault<UMotionForgeSettings>();

	FString Normalised;
	FString Problem;

	if (!MotionForgeSettingsPrivate::ValidateContentRoot(ContentPath, Normalised, Problem))
	{
		// Report what is configured, not what was asked for. A caller that ignores Problem then reads
		// the truth rather than believing the move happened.
		FMotionOutputPaths Paths = Settings->GetOutputPaths();
		Paths.Problem = Problem;
		return Paths;
	}

	Settings->OutputContentPath = Normalised;
	Settings->TryUpdateDefaultConfigFile();

	return Settings->GetOutputPaths();
}

#if WITH_EDITOR

void UMotionForgeSettings::PostInitProperties()
{
	Super::PostInitProperties();

	if (!HasAnyFlags(RF_ClassDefaultObject) || GIsEditor)
	{
		RefreshStatus();
	}
}

void UMotionForgeSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UMotionForgeSettings, ApiKeyEntry))
	{
		if (!ApiKeyEntry.IsEmpty())
		{
			const FString Service = CredentialProviderId.IsNone()
				? TEXT("Uthana")
				: CredentialProviderId.ToString();

			const bool bStored = FMotionCredentialStore::Set(Service, ApiKeyEntry);

			// Blank the field whether or not the write succeeded. Leaving a key sitting in a details
			// panel invites it into a screenshot, and the property is transient so it would be lost
			// on restart regardless - better that it visibly never persists.
			ApiKeyEntry.Empty();

			if (!bStored)
			{
				UE_LOG(LogMotionForge, Error,
					TEXT("Could not store the key for '%s'. Set the %s environment variable instead."),
					*Service, *FMotionCredentialStore::GetEnvironmentVariableName(Service));
			}
		}

		RefreshStatus();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UMotionForgeSettings, CredentialProviderId))
	{
		RefreshStatus();
	}
}

#endif // WITH_EDITOR

void UMotionForgeSettings::RefreshStatus()
{
	const FString Service = CredentialProviderId.IsNone() ? TEXT("Uthana") : CredentialProviderId.ToString();
	CredentialStatus = FMotionCredentialStore::DescribeSource(Service);
}

FString UMotionForgeSettings::GetAbsoluteStagingDirectory() const
{
	const FString Relative = StagingDirectory.IsEmpty() ? TEXT("Saved/MotionForge") : StagingDirectory;
	const FString Absolute = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Relative);

	IFileManager::Get().MakeDirectory(*Absolute, /*Tree*/ true);
	return Absolute;
}

bool UMotionForgeSettings::IsBlenderConfigured(FString& OutReason) const
{
	// Asked first, and separately from whether Blender exists.
	//
	// Normalisation used to be "off" only because nobody had set an executable path, which is not a
	// decision - it is an accident of what happens to be installed. Moving the project to a machine
	// with Blender on it silently turned the round trip back on, and every clip imported after that
	// arrived rotated ninety degrees and ten frames longer. The file was byte-identical, the code
	// unchanged, and the only difference was the computer.
	//
	// So the switch is explicit and the path stays configured: a provider that genuinely needs a
	// Blender round trip can have one without the setting meaning two things at once.
	if (!bNormalizeImportedClips)
	{
		OutReason = TEXT(
			"Clip normalisation is off, so downloads are imported exactly as the provider sent them. "
			"Turning it on runs a Blender round trip that currently reorients the rig - see "
			"bNormalizeImportedClips.");
		return false;
	}

	if (BlenderExecutable.FilePath.IsEmpty())
	{
		OutReason = TEXT("No Blender executable set - downloads will be imported without normalisation.");
		return false;
	}

	if (!FPaths::FileExists(BlenderExecutable.FilePath))
	{
		OutReason = FString::Printf(TEXT("Blender not found at '%s'."), *BlenderExecutable.FilePath);
		return false;
	}

	OutReason.Reset();
	return true;
}
