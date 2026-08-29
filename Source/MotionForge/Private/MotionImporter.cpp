#include "MotionImporter.h"

#include "MotionForge.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace MotionImporterPrivate
{
	/**
	 * Run an import task with Interchange held off.
	 *
	 * Naming a factory on the task is not enough in 5.8 - Interchange intercepts FBX ahead of it and
	 * ignores UFbxImportUI entirely, so the skeleton and mesh-type options are discarded. Toggle the
	 * feature flag for the duration and restore it, leaving the rest of the session alone.
	 */
	static void RunWithLegacyFbx(UAssetImportTask* Task)
	{
		FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

		TArray<UAssetImportTask*> Tasks;
		Tasks.Add(Task);

		IConsoleVariable* InterchangeFbx =
			IConsoleManager::Get().FindConsoleVariable(TEXT("Interchange.FeatureFlags.Import.FBX"));

		const bool bWasEnabled = InterchangeFbx && InterchangeFbx->GetBool();
		if (bWasEnabled)
		{
			InterchangeFbx->Set(false, ECVF_SetByCode);
		}

		AssetTools.Get().ImportAssetTasks(Tasks);

		if (bWasEnabled)
		{
			InterchangeFbx->Set(true, ECVF_SetByCode);
		}
	}
}

USkeletalMesh* FMotionImporter::ImportSkeletalMesh(
	const FString& AbsoluteFbxPath,
	const FString& DestinationPackagePath,
	const FString& AssetName,
	FString& OutError)
{
	if (!FPaths::FileExists(AbsoluteFbxPath))
	{
		OutError = FString::Printf(TEXT("No file at '%s'."), *AbsoluteFbxPath);
		return nullptr;
	}

	UFbxImportUI* Options = NewObject<UFbxImportUI>();
	Options->MeshTypeToImport = FBXIT_SkeletalMesh;
	Options->OriginalImportType = FBXIT_SkeletalMesh;
	Options->bImportAsSkeletal = true;
	Options->bImportMesh = true;
	Options->bImportMaterials = false;
	Options->bImportTextures = false;
	Options->bCreatePhysicsAsset = false;

	// Deliberately no Skeleton: this import exists to *create* one, from the provider's own rig.
	// Handing it ours would merge the two and defeat the entire point.
	Options->Skeleton = nullptr;
	Options->bImportAnimations = false;

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = AbsoluteFbxPath;
	Task->DestinationPath = DestinationPackagePath;
	Task->DestinationName = ObjectTools::SanitizeObjectName(AssetName);
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;
	Task->bAsync = false;
	Task->Options = Options;
	Task->Factory = NewObject<UFbxFactory>();

	MotionImporterPrivate::RunWithLegacyFbx(Task);

	for (UObject* Imported : Task->GetObjects())
	{
		if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Imported))
		{
			return Mesh;
		}
	}

	for (const FString& Path : Task->ImportedObjectPaths)
	{
		if (USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Path))
		{
			return Mesh;
		}
	}

	OutError = FString::Printf(TEXT("Import produced no skeletal mesh from '%s'."), *AbsoluteFbxPath);
	return nullptr;
}

FMotionImportResult FMotionImporter::Import(const FMotionImportRequest& Request)
{
	FMotionImportResult Result;

	if (!FPaths::FileExists(Request.AbsoluteFbxPath))
	{
		Result.Error = FString::Printf(TEXT("No file at '%s'."), *Request.AbsoluteFbxPath);
		return Result;
	}

	if (Request.TargetSkeleton == nullptr)
	{
		// Without this the importer would either invent a skeleton or fail obscurely inside the FBX
		// pipeline, so refuse early with a message that says what is actually wrong.
		Result.Error = TEXT("No target skeleton - set one on the Motion Character asset.");
		return Result;
	}

	const FString SafeName = ObjectTools::SanitizeObjectName(Request.AssetName);

	UFbxImportUI* Options = NewObject<UFbxImportUI>();
	Options->MeshTypeToImport = FBXIT_Animation;
	Options->OriginalImportType = FBXIT_Animation;
	Options->bImportAsSkeletal = true;
	Options->bImportMesh = false;
	Options->bImportAnimations = true;
	Options->bImportMaterials = false;
	Options->bImportTextures = false;
	Options->bCreatePhysicsAsset = false;
	Options->Skeleton = Request.TargetSkeleton;

	if (Options->AnimSequenceImportData)
	{
		UFbxAnimSequenceImportData* AnimData = Options->AnimSequenceImportData;
		AnimData->bImportMeshesInBoneHierarchy = false;

		// Sample at the rate the file was fetched at. The two must agree.
		//
		// Letting the importer choose (CustomSampleRate 0) produced 7969 keys for a 4 second clip,
		// and asking for a rate the file was not written at stretches it - a 4 second clip fetched
		// at fps=30 arrived as 8.3 seconds because the provider re-times rather than resamples when
		// asked for less than its native rate. Fetch at the native rate and sample at the same
		// number; see MotionForgeSettings::TargetFrameRate.
		AnimData->bUseDefaultSampleRate = false;
		AnimData->CustomSampleRate = Request.FrameRate;
		AnimData->AnimationLength = EFBXAnimationLengthImportType::FBXALIT_ExportedTime;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Request.AbsoluteFbxPath;
	Task->DestinationPath = Request.DestinationPackagePath;
	Task->DestinationName = SafeName;
	Task->bAutomated = true;          // never prompt - this runs unattended
	Task->bReplaceExisting = true;    // regenerating a definition should update, not duplicate
	Task->bSave = false;              // caller decides when to write packages
	Task->bAsync = false;
	Task->Options = Options;

	// Name the factory explicitly. Left to routing, an FBX may be picked up by Interchange, which
	// ignores UFbxImportUI entirely and would quietly drop the target skeleton.
	Task->Factory = NewObject<UFbxFactory>();

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(Task);

	MotionImporterPrivate::RunWithLegacyFbx(Task);

	auto Succeed = [&Result, &Request](UAnimSequence* Sequence)
	{
		Result.bSuccess = true;
		Result.Sequence = Sequence;

		// Log the duration. A clip that imports "successfully" at twice its real length is the
		// failure this pipeline is most likely to hit and least likely to notice, so put the number
		// where it can be compared against the provider's viewer without opening the asset.
		UE_LOG(LogMotionForge, Log, TEXT("Imported '%s' onto skeleton '%s' - %.2fs, %d keys."),
			*Sequence->GetPathName(), *Request.TargetSkeleton->GetName(),
			Sequence->GetPlayLength(), Sequence->GetNumberOfSampledKeys());
	};

	for (UObject* Imported : Task->GetObjects())
	{
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(Imported))
		{
			Succeed(Sequence);
			return Result;
		}
	}

	// GetObjects only returns what the factory considers the primary asset. An FBX carrying a mesh
	// yields the skeletal mesh there and creates the animation alongside it as "<Name>_Anim", so a
	// clip that imported perfectly well can still look like a failure. Check the paths it actually
	// wrote before concluding anything.
	for (const FString& Path : Task->ImportedObjectPaths)
	{
		if (UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *Path))
		{
			UE_LOG(LogMotionForge, Warning,
				TEXT("'%s' imported as a secondary asset - the file probably still carries a mesh."),
				*Path);
			Succeed(Sequence);
			return Result;
		}
	}

	Result.Error = FString::Printf(
		TEXT("Import produced no animation sequence from '%s'. The file may have no animation track, ")
		TEXT("or its rig may not match skeleton '%s'."),
		*Request.AbsoluteFbxPath, *Request.TargetSkeleton->GetName());

	return Result;
}
