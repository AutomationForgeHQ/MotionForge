#include "MotionForgeSubsystem.h"
#include "MotionTakeProvenance.h"

#include "MotionForge.h"
#include "MotionDef.h"
#include "MotionCharacter.h"
#include "MotionForgeSettings.h"
#include "MotionCredentialStore.h"
#include "MotionNormalizeTask.h"
#include "MotionImporter.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MotionForgeJson
{
	static FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	static FString Error(const FString& Message)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("error"), Message);
		return Serialize(Root);
	}

	static FString StatusToString(EMotionDefStatus Status)
	{
		switch (Status)
		{
		case EMotionDefStatus::Draft:          return TEXT("Draft");
		case EMotionDefStatus::Generating:     return TEXT("Generating");
		case EMotionDefStatus::AwaitingReview: return TEXT("AwaitingReview");
		case EMotionDefStatus::Downloading:    return TEXT("Downloading");
		case EMotionDefStatus::Processing:     return TEXT("Processing");
		case EMotionDefStatus::Ready:          return TEXT("Ready");
		case EMotionDefStatus::Failed:         return TEXT("Failed");
		}
		return TEXT("Unknown");
	}

	static FString JobStatusToString(EMotionJobStatus Status)
	{
		switch (Status)
		{
		case EMotionJobStatus::Pending:  return TEXT("Pending");
		case EMotionJobStatus::Running:  return TEXT("Running");
		case EMotionJobStatus::Finished: return TEXT("Finished");
		case EMotionJobStatus::Failed:   return TEXT("Failed");
		}
		return TEXT("Unknown");
	}
}

// -------------------------------------------------------------------------------------------------
// Lifetime
// -------------------------------------------------------------------------------------------------

void UMotionForgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// One slow ticker rather than one per job. Polling cadence is enforced inside Tick against the
	// interval in settings, because providers publish a recommended floor and hammering them is a
	// good way to get rate limited mid-batch.
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UMotionForgeSubsystem::Tick), 1.0f);

	// Providers are owned by the module, not by this subsystem, so an add-on plugin that loads after
	// the editor has already built the subsystem still turns up. Nothing is cached here.
	const FMotionForgeModule* Module = FMotionForgeModule::GetPtr();
	UE_LOG(LogMotionForge, Log, TEXT("MotionForge ready with %d provider(s)."),
		Module ? Module->GetProviderIds().Num() : 0);
}

void UMotionForgeSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	Batches.Empty();

	Super::Deinitialize();
}

void UMotionForgeSubsystem::ReleaseStrandedDefinitions()
{
	// Batches live in memory and nowhere else, so nothing can be mid-flight at startup - whatever was
	// tracking these definitions died with the last editor. A definition left saying Generating is
	// therefore stale **by construction**, and `IsBusy` refuses to submit it ever again: the Generate
	// button reports "already generating" for a batch that cannot exist, with no way out but editing
	// the asset by hand.
	//
	// Found the honest way, by closing the editor for a build while a generation was in flight.
	int32 Released = 0;

	for (const FString& Path : FindMotionDefs({}))
	{
		UMotionDef* Def = LoadDef(Path);

		if (Def == nullptr || !Def->IsBusy())
		{
			continue;
		}

		// Candidates are never touched. Jobs submitted before the restart may well have finished on
		// the provider, and their motion ids are the only route back to those takes - so a definition
		// with usable ones goes to review rather than being written off.
		if (Def->CountUsableCandidates() > 0)
		{
			Def->SetStatus(EMotionDefStatus::AwaitingReview);
		}
		else
		{
			Def->SetStatus(EMotionDefStatus::Failed,
				TEXT("The editor closed while this was generating, so nothing was left tracking it. "
					 "Generating again is safe; any takes the provider did finish are still listed as "
					 "candidates."));
		}

		SaveAsset(Def);
		++Released;
	}

	if (Released > 0)
	{
		UE_LOG(LogMotionForge, Log,
			TEXT("Released %d definition(s) left mid-flight by a previous session. They can generate "
				 "again."),
			Released);
	}
}

UMotionForgeSubsystem* UMotionForgeSubsystem::Get()
{
	return GEditor ? GEditor->GetEditorSubsystem<UMotionForgeSubsystem>() : nullptr;
}

TSharedPtr<IMotionProvider> UMotionForgeSubsystem::FindProvider(FName ProviderId) const
{
	const FMotionForgeModule* Module = FMotionForgeModule::GetPtr();
	if (!Module)
	{
		return nullptr;
	}

	const FName Resolved = ProviderId.IsNone() ? UMotionForgeSettings::Get()->DefaultProviderId : ProviderId;
	return Module->FindProvider(Resolved);
}

FMotionReadiness UMotionForgeSubsystem::CheckReadiness(const FString& AssetPath) const
{
	FMotionReadiness Readiness;

	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		Readiness.Blocker = EMotionBlocker::NoProvider;
		Readiness.Problem = FString::Printf(TEXT("No motion definition at '%s'."), *AssetPath);
		return Readiness;
	}

	TSharedPtr<IMotionProvider> Provider;
	UMotionCharacter* Character = nullptr;
	FString Error;

	Readiness.bCanGenerate = ResolveDefinition(Def, Provider, Character, Error, &Readiness.Blocker);
	Readiness.Problem = Readiness.bCanGenerate ? FString() : Error;

	return Readiness;
}

void UMotionForgeSubsystem::NotifyProviderStateChanged(FName ProviderId)
{
	// Resolved, so a listener comparing against the id it drew with always matches - a definition
	// with no provider set is drawn with the default's caps and must still hear about them.
	const FName Resolved = ProviderId.IsNone()
		? UMotionForgeSettings::Get()->DefaultProviderId
		: ProviderId;

	ProviderStateChanged.Broadcast(Resolved);
}

void UMotionForgeSubsystem::RefreshProviderState(FName ProviderId)
{
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		return;
	}

	const FName Resolved = Provider->GetProviderId();
	TWeakObjectPtr<UMotionForgeSubsystem> WeakThis(this);

	Provider->RefreshState([WeakThis, Resolved]()
	{
		if (UMotionForgeSubsystem* Self = WeakThis.Get())
		{
			Self->NotifyProviderStateChanged(Resolved);
		}
	});
}

TArray<FName> UMotionForgeSubsystem::GetProviderIds() const
{
	const FMotionForgeModule* Module = FMotionForgeModule::GetPtr();
	return Module ? Module->GetProviderIds() : TArray<FName>();
}

FMotionProviderCaps UMotionForgeSubsystem::GetProviderCaps(FName ProviderId) const
{
	if (TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId))
	{
		return Provider->GetCaps();
	}

	// An empty ProviderId is the "no such provider" answer. Returning defaults with the id filled in
	// would look like a real capability report for a provider that does not exist.
	return FMotionProviderCaps();
}

int32 UMotionForgeSubsystem::ResolveFrameRate(const TSharedPtr<IMotionProvider>& Provider)
{
	if (Provider.IsValid())
	{
		const int32 Native = Provider->GetCaps().NativeFrameRate;
		if (Native > 0)
		{
			return Native;
		}
	}

	return UMotionForgeSettings::Get()->TargetFrameRate;
}

// -------------------------------------------------------------------------------------------------
// Authoring
// -------------------------------------------------------------------------------------------------

UMotionDef* UMotionForgeSubsystem::LoadDef(const FString& AssetPath) const
{
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}
	return LoadObject<UMotionDef>(nullptr, *AssetPath);
}

FString UMotionForgeSubsystem::CreateMotionDef(const FMotionDefSpec& Spec)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	FString AssetName = Spec.AssetName.IsEmpty() ? TEXT("MD_Untitled") : Spec.AssetName;
	AssetName = ObjectTools::SanitizeObjectName(AssetName);

	const FString PackagePath = Settings->GetDefinitionsPath() / AssetName;

	// Creating over an existing definition would drop its candidates, and on providers with no seed
	// those takes cannot be regenerated. Update the existing one instead.
	if (UMotionDef* Existing = LoadObject<UMotionDef>(nullptr, *(PackagePath + TEXT(".") + AssetName)))
	{
		Existing->ApplySpec(Spec);
		SaveAsset(Existing);
		return Existing->GetPathName();
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UE_LOG(LogMotionForge, Error, TEXT("Could not create package '%s'."), *PackagePath);
		return FString();
	}

	UMotionDef* Def = NewObject<UMotionDef>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Def)
	{
		return FString();
	}

	Def->ApplySpec(Spec);
	Def->ApplyProjectDefaults();

	FAssetRegistryModule::AssetCreated(Def);
	SaveAsset(Def);

	UE_LOG(LogMotionForge, Log, TEXT("Created motion definition '%s'."), *Def->GetPathName());
	return Def->GetPathName();
}

bool UMotionForgeSubsystem::UpdateMotionDef(const FString& AssetPath, const FMotionDefSpec& Spec)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		return false;
	}

	Def->ApplySpec(Spec);
	SaveAsset(Def);
	return true;
}

TArray<FString> UMotionForgeSubsystem::FindMotionDefs(const TArray<EMotionDefStatus>& StatusFilter) const
{
	TArray<FString> Paths;

	const FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	Registry.Get().GetAssetsByClass(UMotionDef::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ true);

	for (const FAssetData& Asset : Assets)
	{
		// Status lives inside the asset, so filtering means loading it. Skip that entirely when the
		// caller wants everything - listing a large library should not fault in every definition.
		if (StatusFilter.Num() > 0)
		{
			const UMotionDef* Def = Cast<UMotionDef>(Asset.GetAsset());
			if (!Def || !StatusFilter.Contains(Def->Status))
			{
				continue;
			}
		}

		Paths.Add(Asset.GetSoftObjectPath().ToString());
	}

	return Paths;
}

TArray<FString> UMotionForgeSubsystem::ListMotionDefs(const FString& StatusFilter) const
{
	if (StatusFilter.IsEmpty())
	{
		return FindMotionDefs({});
	}

	const UEnum* StatusEnum = StaticEnum<EMotionDefStatus>();
	for (int32 Index = 0; Index < StatusEnum->NumEnums() - 1; ++Index)
	{
		const EMotionDefStatus Status = static_cast<EMotionDefStatus>(StatusEnum->GetValueByIndex(Index));
		if (MotionForgeJson::StatusToString(Status).Equals(StatusFilter, ESearchCase::IgnoreCase))
		{
			return FindMotionDefs({ Status });
		}
	}

	UE_LOG(LogMotionForge, Warning, TEXT("Unknown status filter '%s' - no definitions match."), *StatusFilter);
	return {};
}

FString UMotionForgeSubsystem::ExportCharacterFbx(
	const FString& CharacterAssetPath,
	const FString& AbsoluteOutputPath,
	FString& OutError)
{
	UMotionCharacter* Character = LoadObject<UMotionCharacter>(nullptr, *CharacterAssetPath);
	if (!Character)
	{
		OutError = FString::Printf(TEXT("No motion character at '%s'."), *CharacterAssetPath);
		return FString();
	}

	USkeletalMesh* Mesh = Character->PreviewMesh.LoadSynchronous();
	if (!Mesh)
	{
		OutError = FString::Printf(
			TEXT("'%s' has no Preview Mesh. Set it to the skeletal mesh you want the provider to "
				 "retarget onto - it must be on the character's Target Skeleton."),
			*Character->GetDisplayName());
		return FString();
	}

	// A mesh on the wrong skeleton would upload happily and then retarget onto a rig the imports do
	// not bind to, which surfaces much later as animation that looks subtly wrong. Refuse now.
	if (!Character->TargetSkeleton.IsNull() && Mesh->GetSkeleton() != Character->TargetSkeleton.LoadSynchronous())
	{
		OutError = FString::Printf(
			TEXT("Preview Mesh '%s' is not on Target Skeleton '%s'. Uploading it would pair the "
				 "provider id with a rig the imports cannot bind to."),
			*Mesh->GetName(), *Character->TargetSkeleton.ToString());
		return FString();
	}

	FString OutPath = AbsoluteOutputPath;
	if (OutPath.IsEmpty())
	{
		OutPath = UMotionForgeSettings::Get()->GetAbsoluteStagingDirectory()
			/ TEXT("Characters") / (Mesh->GetName() + TEXT(".fbx"));
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree*/ true);

	UFbxExportOption* Options = NewObject<UFbxExportOption>();
	Options->bASCII = false;
	Options->Collision = false;
	Options->LevelOfDetail = false;

	UAssetExportTask* Task = NewObject<UAssetExportTask>();
	Task->Object = Mesh;
	Task->Filename = OutPath;
	Task->Options = Options;
	Task->bSelected = false;
	Task->bReplaceIdentical = true;
	Task->bWriteEmptyFiles = false;
	Task->bUseFileArchive = false;

	// Both matter: bPrompt would block a headless or agent-driven run on a modal dialog, and
	// bAutomated is what suppresses the exporter's own option window.
	Task->bPrompt = false;
	Task->bAutomated = true;

	if (!UExporter::RunAssetExportTask(Task) || !FPaths::FileExists(OutPath))
	{
		OutError = Task->Errors.Num() > 0
			? FString::Join(Task->Errors, TEXT("; "))
			: FString::Printf(TEXT("FBX export of '%s' failed."), *Mesh->GetName());
		return FString();
	}

	// Setting a UPROPERTY from C++ does not dirty the package, and SaveAsset skips clean ones.
	Character->SourceFbxPath = OutPath;
	Character->MarkPackageDirty();
	SaveAsset(Character);

	OutError.Reset();
	UE_LOG(LogMotionForge, Log, TEXT("Exported '%s' to %s"), *Mesh->GetName(), *OutPath);
	return OutPath;
}

void UMotionForgeSubsystem::UploadCharacter(
	const FString& CharacterAssetPath,
	const FMotionCharacterUploadOptions& Options,
	bool bForce,
	TFunction<void(bool, const FMotionCharacterUpload&, const FString&)> OnComplete)
{
	const FMotionCharacterUpload Empty;

	UMotionCharacter* Character = LoadObject<UMotionCharacter>(nullptr, *CharacterAssetPath);
	if (!Character)
	{
		OnComplete(false, Empty, FString::Printf(TEXT("No motion character at '%s'."), *CharacterAssetPath));
		return;
	}

	// Repointing a character that already has an id would orphan every take generated against the
	// old one - and on a provider with no seed those takes cannot be reproduced.
	if (!Character->ProviderCharacterId.IsEmpty() && !bForce)
	{
		OnComplete(false, Empty, FString::Printf(
			TEXT("'%s' is already paired with provider character '%s'. Uploading again would create a "
				 "second character and orphan takes generated against the first. Pass Force if that "
				 "is genuinely what you want."),
			*Character->GetDisplayName(), *Character->ProviderCharacterId));
		return;
	}

	const FName ProviderId = Character->ProviderId.IsNone()
		? UMotionForgeSettings::Get()->DefaultProviderId
		: Character->ProviderId;

	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, Empty, FString::Printf(TEXT("No provider registered as '%s'."), *ProviderId.ToString()));
		return;
	}

	if (!Provider->SupportsCharacterManagement())
	{
		OnComplete(false, Empty, FString::Printf(
			TEXT("%s cannot upload characters. Export the FBX and add it through their web UI, then "
				 "paste the id into ProviderCharacterId."),
			*Provider->GetDisplayName()));
		return;
	}

	if (!Provider->HasCredential())
	{
		OnComplete(false, Empty, FString::Printf(
			TEXT("No API key for %s. Set one in Project Settings > Plugins > MotionForge."),
			*Provider->GetDisplayName()));
		return;
	}

	FString ExportError;
	const FString FbxPath = ExportCharacterFbx(CharacterAssetPath, FString(), ExportError);
	if (FbxPath.IsEmpty())
	{
		OnComplete(false, Empty, ExportError);
		return;
	}

	// Weak, not raw: the upload takes up to five minutes and the asset can be garbage collected or
	// the editor closed in that window.
	TWeakObjectPtr<UMotionCharacter> WeakCharacter = Character;

	Provider->UploadCharacter(FbxPath, Character->GetDisplayName(), Options,
		[WeakCharacter, FbxPath, OnComplete](const FMotionCharacterUploadResult& Upload)
		{
			const FMotionCharacterUpload Nothing;

			if (!Upload.bSuccess)
			{
				OnComplete(false, Nothing, Upload.Error);
				return;
			}

			UMotionCharacter* Live = WeakCharacter.Get();
			if (!Live)
			{
				// The character was created provider-side, so say the id rather than losing it.
				OnComplete(false, Nothing, FString::Printf(
					TEXT("Upload succeeded as character '%s', but the asset went away before the id "
						 "could be written. Paste it into ProviderCharacterId by hand."),
					*Upload.CharacterId));
				return;
			}

			Live->ProviderCharacterId = Upload.CharacterId;
			Live->SourceFbxPath = FbxPath;
			Live->MarkPackageDirty();
			SaveAsset(Live);

			FMotionCharacterUpload Result;
			Result.ProviderCharacterId = Upload.CharacterId;
			Result.Name = Upload.Name;
			Result.AutoRigConfidence = Upload.AutoRigConfidence;
			Result.UploadedFile = FbxPath;

			UE_LOG(LogMotionForge, Log, TEXT("Paired '%s' with provider character '%s'."),
				*Live->GetDisplayName(), *Upload.CharacterId);

			OnComplete(true, Result, FString());
		});
}

void UMotionForgeSubsystem::ListProviderCharacters(
	FName ProviderId,
	TFunction<void(bool, const TArray<FMotionRemoteCharacter>&, const FString&)> OnComplete)
{
	const FName Resolved = ProviderId.IsNone() ? UMotionForgeSettings::Get()->DefaultProviderId : ProviderId;

	TSharedPtr<IMotionProvider> Provider = FindProvider(Resolved);
	if (!Provider.IsValid())
	{
		OnComplete(false, {}, FString::Printf(TEXT("No provider registered as '%s'."), *Resolved.ToString()));
		return;
	}

	Provider->ListCharacters(
		[OnComplete](bool bSuccess, const TArray<FMotionProviderCharacter>& Characters, const FString& Error)
		{
			TArray<FMotionRemoteCharacter> Result;
			Result.Reserve(Characters.Num());

			for (const FMotionProviderCharacter& Character : Characters)
			{
				FMotionRemoteCharacter Entry;
				Entry.Id = Character.Id;
				Entry.Name = Character.Name;
				Result.Add(MoveTemp(Entry));
			}

			OnComplete(bSuccess, Result, Error);
		});
}

FMotionBatchSubmission UMotionForgeSubsystem::SubmitBatch(
	const TArray<FMotionDefSpec>& Specs, EMotionPipelineMode Mode)
{
	FMotionBatchSubmission Result;
	Result.AssetPaths.Reserve(Specs.Num());

	for (const FMotionDefSpec& Spec : Specs)
	{
		const FString Created = CreateMotionDef(Spec);
		if (!Created.IsEmpty())
		{
			Result.AssetPaths.Add(Created);
		}
	}

	Result.BatchId = StartGeneration(Result.AssetPaths, Mode);
	return Result;
}

FString UMotionForgeSubsystem::SubmitBatchFromJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return MotionForgeJson::Error(TEXT("Payload was not valid JSON."));
	}

	EMotionPipelineMode Mode = UMotionForgeSettings::Get()->DefaultMode;
	FString ModeText;
	if (Root->TryGetStringField(TEXT("mode"), ModeText) && ModeText.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase))
	{
		Mode = EMotionPipelineMode::Automatic;
	}

	const TArray<TSharedPtr<FJsonValue>>* Definitions = nullptr;
	if (!Root->TryGetArrayField(TEXT("definitions"), Definitions) || !Definitions)
	{
		return MotionForgeJson::Error(TEXT("Payload had no 'definitions' array."));
	}

	TArray<FMotionDefSpec> Specs;
	for (const TSharedPtr<FJsonValue>& Value : *Definitions)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!Value->TryGetObject(Entry) || !Entry)
		{
			continue;
		}

		FMotionDefSpec Spec;
		(*Entry)->TryGetStringField(TEXT("assetName"), Spec.AssetName);
		(*Entry)->TryGetStringField(TEXT("prompt"), Spec.Prompt);
		(*Entry)->TryGetStringField(TEXT("characterAssetPath"), Spec.CharacterAssetPath);
		(*Entry)->TryGetStringField(TEXT("modelId"), Spec.ModelId);

		int32 Number = 0;
		if ((*Entry)->TryGetNumberField(TEXT("length"), Number))   { Spec.Length = Number; }
		if ((*Entry)->TryGetNumberField(TEXT("variants"), Number)) { Spec.Variants = Number; }

		bool bFlag = true;
		if ((*Entry)->TryGetBoolField(TEXT("rewritePrompt"), bFlag)) { Spec.bRewritePrompt = bFlag; }

		const TArray<TSharedPtr<FJsonValue>>* Trim = nullptr;
		if ((*Entry)->TryGetArrayField(TEXT("trim"), Trim) && Trim && Trim->Num() == 2)
		{
			Spec.TrimWindow = FVector2D((*Trim)[0]->AsNumber(), (*Trim)[1]->AsNumber());
		}

		FString ProviderText;
		if ((*Entry)->TryGetStringField(TEXT("providerId"), ProviderText))
		{
			Spec.ProviderId = FName(*ProviderText);
		}

		Specs.Add(MoveTemp(Spec));
	}

	const FMotionBatchSubmission Result = SubmitBatch(Specs, Mode);

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("batchId"), Result.BatchId);

	TArray<TSharedPtr<FJsonValue>> PathValues;
	for (const FString& Path : Result.AssetPaths)
	{
		PathValues.Add(MakeShared<FJsonValueString>(Path));
	}
	Response->SetArrayField(TEXT("created"), PathValues);

	return MotionForgeJson::Serialize(Response);
}

// -------------------------------------------------------------------------------------------------
// Pipeline
// -------------------------------------------------------------------------------------------------

bool UMotionForgeSubsystem::ResolveDefinition(
	UMotionDef* Def,
	TSharedPtr<IMotionProvider>& OutProvider,
	UMotionCharacter*& OutCharacter,
	FString& OutError,
	EMotionBlocker* OutBlocker) const
{
	if (OutBlocker) { *OutBlocker = EMotionBlocker::None; }

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	const FName ProviderId = Def->ProviderId.IsNone() ? Settings->DefaultProviderId : Def->ProviderId;
	OutProvider = FindProvider(ProviderId);
	if (!OutProvider.IsValid())
	{
		OutError = FString::Printf(TEXT("No provider registered as '%s'."), *ProviderId.ToString());
		if (OutBlocker) { *OutBlocker = EMotionBlocker::NoProvider; }
		return false;
	}

	const FMotionProviderCaps Caps = OutProvider->GetCaps();

	// A local provider has nothing to sign in to. Demanding a key it does not have would make the
	// free path unusable in exactly the case it exists for.
	if (Caps.bNeedsCredential && !OutProvider->HasCredential())
	{
		// Keys moved to Editor Preferences, and there is a Keys page now. The old sentence sent
		// people to a settings page that no longer holds them.
		OutError = FString::Printf(
			TEXT("No API key for %s. Set one on the Keys page, or in Editor Preferences."),
			*OutProvider->GetDisplayName());
		if (OutBlocker) { *OutBlocker = EMotionBlocker::NoCredential; }
		return false;
	}

	if (!Caps.SetupHint.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s is not ready: %s"), *OutProvider->GetDisplayName(), *Caps.SetupHint);
		if (OutBlocker) { *OutBlocker = EMotionBlocker::ProviderNotReady; }
		return false;
	}

	TSoftObjectPtr<UMotionCharacter> CharacterPtr = Def->Character.IsNull() ? Settings->DefaultCharacter : Def->Character;
	OutCharacter = CharacterPtr.LoadSynchronous();
	if (!OutCharacter)
	{
		OutError = TEXT("No Motion Character set, and no default configured in settings.");
		if (OutBlocker) { *OutBlocker = EMotionBlocker::NoCharacter; }
		return false;
	}

	FString CharacterReason;
	if (!OutCharacter->IsUsableForProvider(Caps.bSupportsCharacterUpload, CharacterReason))
	{
		OutError = FString::Printf(TEXT("Character '%s' is not usable with %s: %s"),
			*OutCharacter->GetDisplayName(), *OutProvider->GetDisplayName(), *CharacterReason);
		if (OutBlocker) { *OutBlocker = EMotionBlocker::CharacterUnusable; }
		return false;
	}

	// A definition whose prompt lives on a timeline is allowed an empty field here, because the field
	// is no longer where the prompt is. Refusing it would be a confidently wrong error message about
	// the one thing the artist did fill in.
	if (Def->Prompt.IsEmpty() && FMotionPromptSequence::FindTrack(Def->Control.ConstraintSequence.LoadSynchronous()) == nullptr)
	{
		OutError = TEXT("Prompt is empty.");
		if (OutBlocker) { *OutBlocker = EMotionBlocker::NoPrompt; }
		return false;
	}

	return true;
}

FString UMotionForgeSubsystem::MakeBatchId()
{
	static int32 Counter = 0;
	return FString::Printf(TEXT("batch_%s_%03d"),
		*FDateTime::Now().ToString(TEXT("%H%M%S")), ++Counter);
}

FString UMotionForgeSubsystem::Generate(const TArray<FString>& AssetPaths)
{
	return StartGeneration(AssetPaths, UMotionForgeSettings::Get()->DefaultMode);
}

FString UMotionForgeSubsystem::RunFullPipeline(const TArray<FString>& AssetPaths)
{
	return StartGeneration(AssetPaths, EMotionPipelineMode::Automatic);
}

FString UMotionForgeSubsystem::StartGeneration(const TArray<FString>& AssetPaths, EMotionPipelineMode Mode)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	const FString BatchId = MakeBatchId();

	// Register the batch before submitting anything. A submit that fails at the transport layer can
	// invoke its callback immediately, and that callback looks the batch up by id - if it were not in
	// the map yet the result would be silently dropped.
	FMotionBatch NewBatch;
	NewBatch.BatchId = BatchId;
	NewBatch.Mode = Mode;
	NewBatch.StartedAt = FPlatformTime::Seconds();
	Batches.Add(BatchId, MoveTemp(NewBatch));

	FMotionBatch& Batch = Batches[BatchId];

	for (const FString& AssetPath : AssetPaths)
	{
		UMotionDef* Def = LoadDef(AssetPath);
		if (!Def)
		{
			UE_LOG(LogMotionForge, Warning, TEXT("Skipping '%s' - not a motion definition."), *AssetPath);
			continue;
		}

		// Idempotence. A definition already mid-flight is left alone rather than resubmitted, so a
		// caller that retries after a timeout does not pay twice for the same takes.
		if (Def->IsBusy())
		{
			UE_LOG(LogMotionForge, Log, TEXT("Skipping '%s' - already %s."),
				*Def->GetName(), *MotionForgeJson::StatusToString(Def->Status));
			continue;
		}

		TSharedPtr<IMotionProvider> Provider;
		UMotionCharacter* Character = nullptr;
		FString Error;
		if (!ResolveDefinition(Def, Provider, Character, Error))
		{
			Def->SetStatus(EMotionDefStatus::Failed, Error);
			SaveAsset(Def);
			continue;
		}

		int32 MinLength = 1;
		int32 MaxLength = 10;
		Provider->GetLengthRange(Def->ModelId, MinLength, MaxLength);

		// A prompt track on the definition's sequence is the prompt, and its sections are the beats.
		//
		// Resolved here, once, at the only point both providers pass through - so nothing downstream
		// has to know whether the words came from a text box or a timeline. Same rule as the
		// constraint sequence on the same asset: the sequence wins outright while it is set, and a
		// track that will not read refuses the generation rather than quietly falling back to wording
		// the artist stopped maintaining the day they laid it out in time.
		FMotionPromptRead Beats;
		FString PromptError;
		if (!FMotionPromptSequence::Resolve(Def, ResolveFrameRate(Provider), Beats, PromptError))
		{
			Def->SetStatus(EMotionDefStatus::Failed, PromptError);
			SaveAsset(Def);
			continue;
		}

		for (const FString& Problem : Beats.Problems)
		{
			UE_LOG(LogMotionForge, Warning, TEXT("'%s': %s"), *Def->GetName(), *Problem);
		}

		if (Beats.bFromSequence)
		{
			UE_LOG(LogMotionForge, Log,
				TEXT("'%s': prompt from '%s' - %d beat(s), %.2fs: %s"),
				*Def->GetName(), *Beats.SequencePath, Beats.Beats.Num(), Beats.TotalSeconds,
				*FString::JoinBy(Beats.Beats, TEXT(" | "),
					[](const FMotionPromptBeat& Beat)
					{
						return FString::Printf(TEXT("%.2fs '%s'"), Beat.Seconds, *Beat.Text);
					}));
		}

		// Per-beat durations decide the clip's length, because they *are* the clip: the length is
		// their sum and there is nothing left for `Length` to mean. Same rule as a constraint
		// sequence winning over an authored constraint list - the more specific statement wins
		// outright rather than being merged with the vaguer one.
		float BeatTotal = 0.f;
		for (const FMotionPromptBeat& Beat : Beats.Beats)
		{
			BeatTotal += Beat.Seconds;
		}

		const int32 Requested = Beats.Beats.Num() > 0
			? FMath::Max(1, FMath::RoundToInt(BeatTotal))
			: Def->Length;

		if (Beats.Beats.Num() > 0 && Requested != Def->Length)
		{
			UE_LOG(LogMotionForge, Log,
				TEXT("'%s': %d beat duration(s) totalling %.1fs, so Length %d is not used."),
				*Def->GetName(), Beats.Beats.Num(), BeatTotal, Def->Length);
		}

		// Per-beat durations are not clamped against the whole-clip range, because they are not a
		// whole-clip request: a provider that segments limits each *beat*, and the sum of legal beats
		// is legal however large it gets. Clamping here truncated a 2+7+3 request to 10, warned about
		// a length nobody asked for, and recorded a Length that disagreed with the clip - while the
		// runner correctly generated all twelve seconds from the beats it was given.
		const int32 Length = Beats.Beats.Num() > 0
			? Requested
			: FMath::Clamp(Requested, MinLength, MaxLength);

		if (Length != Requested)
		{
			// Worth saying out loud - a model with a four second floor pads a shorter request rather
			// than refusing it, and the padding is exactly what makes output look sluggish.
			UE_LOG(LogMotionForge, Warning,
				TEXT("'%s': length %d is outside %s's range for %s, using %d. Trim the result."),
				*Def->GetName(), Requested, *Provider->GetDisplayName(), *Def->ModelId, Length);
		}

		Def->SetStatus(EMotionDefStatus::Generating);
		Def->ActiveBatchId = Batch.BatchId;
		Batch.DefinitionPaths.AddUnique(Def->GetPathName());

		const FString DefPath = Def->GetPathName();
		const FName ProviderId = Provider->GetProviderId();

		for (int32 Variant = 0; Variant < FMath::Max(1, Def->Variants); ++Variant)
		{
			FMotionSubmitRequest Request;
			Request.Prompt = Beats.Prompt;
			Request.ModelId = Def->ModelId.IsEmpty() ? Provider->GetDefaultModelId() : Def->ModelId;
			Request.ProviderCharacterId = Character->ProviderCharacterId;
			Request.Length = Length;
			Request.bRewritePrompt = Def->bRewritePrompt;
			Request.VariantIndex = Variant;
			Request.Control = Def->Control;
			Request.TargetSkeleton = Character->TargetSkeleton.LoadSynchronous();

			// The resolved beats, whichever route they came from. Written onto the request rather
			// than back onto the asset: a prompt sequence is referenced and never baked, so the
			// definition on disk must not quietly acquire the timeline's numbers.
			Request.Control.BeatSeconds.Reset(Beats.Beats.Num());
			for (const FMotionPromptBeat& Beat : Beats.Beats)
			{
				Request.Control.BeatSeconds.Add(Beat.Seconds);
			}

			// A fixed seed and several variants is a contradiction: every take would be identical.
			// Walk the seed instead, so "four takes from seed 42" means four reproducible takes and
			// not one clip generated four times.
			if (Request.Control.Seed >= 0)
			{
				Request.Control.Seed += Variant;
			}

			Provider->SubmitJob(Request,
				[this, BatchId, DefPath, ProviderId](const FMotionSubmitResult& SubmitResult)
				{
					FMotionBatch* LiveBatch = Batches.Find(BatchId);
					UMotionDef* LiveDef = LoadDef(DefPath);
					if (!LiveBatch || !LiveDef)
					{
						return;
					}

					if (!SubmitResult.bSuccess)
					{
						FMotionCandidate Failed;
						Failed.VariantIndex = SubmitResult.VariantIndex;
						Failed.Status = EMotionJobStatus::Failed;
						Failed.Error = SubmitResult.Error;
						Failed.GeneratedAt = FDateTime::Now();
						LiveDef->Candidates.Add(Failed);

						UE_LOG(LogMotionForge, Warning, TEXT("'%s' variant %d: %s"),
							*LiveDef->GetName(), SubmitResult.VariantIndex, *SubmitResult.Error);

						OnDefinitionGenerated(*LiveBatch, LiveDef);
						return;
					}

					FMotionJobTracking Job;
					Job.JobId = SubmitResult.JobId;
					Job.DefinitionPath = DefPath;
					Job.ProviderId = ProviderId;
					Job.VariantIndex = SubmitResult.VariantIndex;
					Job.SubmittedAt = FPlatformTime::Seconds();
					LiveBatch->Jobs.Add(Job);

					FMotionCandidate Candidate;
					Candidate.JobId = SubmitResult.JobId;
					Candidate.VariantIndex = SubmitResult.VariantIndex;
					Candidate.Status = EMotionJobStatus::Pending;
					Candidate.GeneratedAt = FDateTime::Now();
					LiveDef->Candidates.Add(Candidate);
					LiveDef->MarkPackageDirty();
				});
		}

		SaveAsset(Def);
	}

	if (Batch.DefinitionPaths.Num() == 0)
	{
		UE_LOG(LogMotionForge, Warning, TEXT("Nothing eligible to generate."));
		Batches.Remove(BatchId);
		return FString();
	}

	UE_LOG(LogMotionForge, Log, TEXT("Batch %s: %d definition(s), mode %s."),
		*BatchId, Batch.DefinitionPaths.Num(),
		Mode == EMotionPipelineMode::Automatic ? TEXT("Automatic") : TEXT("HumanInTheLoop"));

	return BatchId;
}

bool UMotionForgeSubsystem::Tick(float DeltaTime)
{
	// Once, on the first tick rather than in Initialize.
	//
	// The sweep saves assets, and saving runs the validation subsystem - which at subsystem-init time
	// has not registered its Blueprint validators yet and says so, once per asset, in a warning that
	// reads like a fault in the asset rather than in when it was touched. Nothing is lost by waiting a
	// frame: nobody can press Generate before the editor has drawn.
	if (!bSweptStrandedDefinitions)
	{
		bSweptStrandedDefinitions = true;
		ReleaseStrandedDefinitions();
	}

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();
	const double Now = FPlatformTime::Seconds();

	if (Now - LastPollTime < FMath::Max(1, Settings->PollIntervalSeconds))
	{
		return true;
	}
	LastPollTime = Now;

	TArray<FString> FinishedBatches;

	for (TPair<FString, FMotionBatch>& Pair : Batches)
	{
		FMotionBatch& Batch = Pair.Value;

		if (Batch.bCancelled)
		{
			FinishedBatches.Add(Pair.Key);
			continue;
		}

		int32 Outstanding = 0;

		for (FMotionJobTracking& Job : Batch.Jobs)
		{
			if (Job.bSettled)
			{
				continue;
			}

			++Outstanding;

			if (Job.bPollInFlight)
			{
				continue;
			}

			if (Now - Job.SubmittedAt > Settings->JobTimeoutSeconds)
			{
				Job.bSettled = true;

				if (UMotionDef* Def = LoadDef(Job.DefinitionPath))
				{
					if (FMotionCandidate* Candidate = Def->Candidates.FindByPredicate(
						[&Job](const FMotionCandidate& C) { return C.JobId == Job.JobId; }))
					{
						Candidate->Status = EMotionJobStatus::Failed;
						Candidate->Error = TEXT("Timed out waiting for the provider.");
					}
					OnDefinitionGenerated(Batch, Def);
				}
				continue;
			}

			TSharedPtr<IMotionProvider> Provider = FindProvider(Job.ProviderId);
			if (!Provider.IsValid())
			{
				Job.bSettled = true;
				continue;
			}

			Job.bPollInFlight = true;

			const FString BatchId = Batch.BatchId;
			const FString JobId = Job.JobId;

			Provider->PollJob(JobId,
				[this, BatchId, JobId](const FMotionJobResult& JobResult)
				{
					FMotionBatch* LiveBatch = Batches.Find(BatchId);
					if (!LiveBatch)
					{
						return;
					}

					FMotionJobTracking* LiveJob = LiveBatch->Jobs.FindByPredicate(
						[&JobId](const FMotionJobTracking& J) { return J.JobId == JobId; });
					if (!LiveJob)
					{
						return;
					}

					LiveJob->bPollInFlight = false;

					if (JobResult.Status == EMotionJobStatus::Finished
						|| JobResult.Status == EMotionJobStatus::Failed)
					{
						LiveJob->bSettled = true;
						OnJobFinished(*LiveBatch, *LiveJob, JobResult);
					}
				});
		}

		if (Outstanding == 0 && Batch.Jobs.Num() > 0)
		{
			FinishedBatches.Add(Pair.Key);
		}
		else if (Batch.Jobs.Num() == 0 && Now - Batch.StartedAt > 60.0)
		{
			// Every submit failed, so there is nothing to poll and nothing will ever arrive. Without
			// this the batch would sit in the map forever being ticked over.
			UE_LOG(LogMotionForge, Warning, TEXT("Batch %s produced no jobs."), *Pair.Key);
			FinishedBatches.Add(Pair.Key);
		}
	}

	for (const FString& BatchId : FinishedBatches)
	{
		UE_LOG(LogMotionForge, Log, TEXT("Batch %s complete."), *BatchId);
		Batches.Remove(BatchId);
	}

	return true;
}

void UMotionForgeSubsystem::OnJobFinished(
	FMotionBatch& Batch,
	const FMotionJobTracking& Job,
	const FMotionJobResult& JobResult)
{
	UMotionDef* Def = LoadDef(Job.DefinitionPath);
	if (!Def)
	{
		return;
	}

	FMotionCandidate* Candidate = Def->Candidates.FindByPredicate(
		[&Job](const FMotionCandidate& C) { return C.JobId == Job.JobId; });

	if (!Candidate)
	{
		return;
	}

	Candidate->Status = JobResult.Status;
	Candidate->MotionId = JobResult.MotionId;
	Candidate->Error = JobResult.Error;

	if (JobResult.Status == EMotionJobStatus::Finished)
	{
		TSharedPtr<IMotionProvider> Provider = FindProvider(Job.ProviderId);
		UMotionCharacter* Character = Def->Character.LoadSynchronous();

		if (Provider.IsValid() && Character)
		{
			Candidate->ViewerUrl = Provider->MakeViewerUrl(Character->ProviderCharacterId, JobResult.MotionId);
		}

		UE_LOG(LogMotionForge, Log, TEXT("'%s' variant %d -> motion %s"),
			*Def->GetName(), Job.VariantIndex, *JobResult.MotionId);
	}

	Def->MarkPackageDirty();
	OnDefinitionGenerated(Batch, Def);
}

void UMotionForgeSubsystem::OnDefinitionGenerated(FMotionBatch& Batch, UMotionDef* Def)
{
	// Only act once every job belonging to this definition has settled.
	const FString DefPath = Def->GetPathName();
	for (const FMotionJobTracking& Job : Batch.Jobs)
	{
		if (Job.DefinitionPath == DefPath && !Job.bSettled)
		{
			return;
		}
	}

	const int32 Usable = Def->CountUsableCandidates();
	if (Usable == 0)
	{
		Def->SetStatus(EMotionDefStatus::Failed, TEXT("No variant generated successfully."));
		SaveAsset(Def);
		return;
	}

	if (Batch.Mode == EMotionPipelineMode::HumanInTheLoop)
	{
		Def->SetStatus(EMotionDefStatus::AwaitingReview);
		SaveAsset(Def);

		UE_LOG(LogMotionForge, Log, TEXT("'%s' has %d take(s) awaiting review."), *Def->GetName(), Usable);
		return;
	}

	// Automatic: take the first usable variant **from this batch** and carry on.
	//
	// Not the first usable candidate on the asset. Candidates accumulate and are never pruned, so on
	// the second run of a definition the oldest take is still at the front of the list - and picking
	// it means every regeneration silently re-imports the first clip ever made. Invisible until
	// somebody iterates: the prompt changes, the run reports success, and the animation does not move.
	//
	// Only the chosen take is downloaded. The others are not discarded - their motion ids are on the
	// asset and the provider still holds them - so they can be fetched later if this one turns out to
	// be wrong. Downloading all of them now would spend money on takes nobody has looked at.
	TSet<FString> JobsInThisBatch;
	for (const FMotionJobTracking& Job : Batch.Jobs)
	{
		if (Job.DefinitionPath == DefPath)
		{
			JobsInThisBatch.Add(Job.JobId);
		}
	}

	const FMotionCandidate* Chosen = nullptr;
	for (const FMotionCandidate& Candidate : Def->Candidates)
	{
		if (Candidate.Status == EMotionJobStatus::Finished
			&& Candidate.IsValidCandidate()
			&& JobsInThisBatch.Contains(Candidate.JobId))
		{
			Chosen = &Candidate;
			break;
		}
	}

	if (Chosen == nullptr)
	{
		// Older takes may well be usable, but importing one here would report this run as a success
		// while quietly delivering a clip from a prompt nobody just asked for.
		Def->SetStatus(EMotionDefStatus::Failed,
			TEXT("No variant from this run generated successfully. Earlier takes are still on the "
				 "asset and can be chosen by hand."));
		SaveAsset(Def);
		return;
	}

	Def->SelectedMotionId = Chosen->MotionId;

	SaveAsset(Def);
	ProcessSelected(Def);
}

bool UMotionForgeSubsystem::SelectCandidate(const FString& AssetPath, const FString& MotionId)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		return false;
	}

	if (!Def->FindCandidate(MotionId))
	{
		UE_LOG(LogMotionForge, Warning, TEXT("'%s' has no candidate '%s'."), *Def->GetName(), *MotionId);
		return false;
	}

	Def->SelectedMotionId = MotionId;
	SaveAsset(Def);
	return true;
}

FString UMotionForgeSubsystem::DownloadSelected(const TArray<FString>& AssetPaths)
{
	int32 Started = 0;

	for (const FString& AssetPath : AssetPaths)
	{
		UMotionDef* Def = LoadDef(AssetPath);
		if (!Def || Def->IsBusy())
		{
			continue;
		}

		if (Def->SelectedMotionId.IsEmpty())
		{
			UE_LOG(LogMotionForge, Warning, TEXT("'%s' has nothing selected."), *Def->GetName());
			continue;
		}

		ProcessSelected(Def);
		++Started;
	}

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), Started > 0);
	Response->SetNumberField(TEXT("started"), Started);
	return MotionForgeJson::Serialize(Response);
}

void UMotionForgeSubsystem::ProcessSelected(UMotionDef* Def)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	const FMotionCandidate* Selected = Def->FindSelectedCandidate();
	if (!Selected)
	{
		Def->SetStatus(EMotionDefStatus::Failed, TEXT("Selected motion is not among the candidates."));
		SaveAsset(Def);
		return;
	}

	// Already on disk from an earlier run - skip straight to processing rather than paying again.
	if (Selected->bDownloaded && FPaths::FileExists(Selected->LocalRawPath))
	{
		UE_LOG(LogMotionForge, Log, TEXT("'%s': reusing '%s'."), *Def->GetName(), *Selected->LocalRawPath);
		NormalizeAndImport(Def, Selected->LocalRawPath);
		return;
	}

	TSharedPtr<IMotionProvider> Provider;
	UMotionCharacter* Character = nullptr;
	FString Error;
	if (!ResolveDefinition(Def, Provider, Character, Error))
	{
		Def->SetStatus(EMotionDefStatus::Failed, Error);
		SaveAsset(Def);
		return;
	}

	// Not every provider hands back an FBX. Kimodo returns raw rotation matrices, and naming its
	// artifact .fbx would send it into an importer that cannot read it.
	const FString RawPath = Settings->GetAbsoluteStagingDirectory()
		/ FString::Printf(TEXT("%s_%s_raw.%s"),
			*Def->GetName(), *Selected->MotionId, *Provider->GetArtifactExtension());

	Def->SetStatus(EMotionDefStatus::Downloading);
	SaveAsset(Def);

	const FString DefPath = Def->GetPathName();
	const FString MotionId = Selected->MotionId;

	Provider->DownloadMotion(Character->ProviderCharacterId, MotionId, RawPath, ResolveFrameRate(Provider),
		[this, DefPath, MotionId, RawPath](bool bSuccess, const FString& DownloadError)
		{
			UMotionDef* LiveDef = LoadDef(DefPath);
			if (!LiveDef)
			{
				return;
			}

			if (!bSuccess)
			{
				LiveDef->SetStatus(EMotionDefStatus::Failed, DownloadError);
				SaveAsset(LiveDef);
				return;
			}

			if (FMotionCandidate* Candidate = LiveDef->FindCandidateMutable(MotionId))
			{
				Candidate->bDownloaded = true;
				Candidate->LocalRawPath = RawPath;
			}

			NormalizeAndImport(LiveDef, RawPath);
		});
}

void UMotionForgeSubsystem::NormalizeAndImport(UMotionDef* Def, const FString& RawPath)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	Def->SetStatus(EMotionDefStatus::Processing);

	const FName ProviderId = Def->ProviderId.IsNone() ? Settings->DefaultProviderId : Def->ProviderId;
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);

	UMotionCharacter* ArtifactCharacter = Def->Character.IsNull()
		? Settings->DefaultCharacter.LoadSynchronous()
		: Def->Character.LoadSynchronous();

	// Providers whose output is not an FBX build the animation themselves. They know exactly what
	// their numbers mean; routing them through a file format and a Blender pass in between only adds
	// two more places for a coordinate convention to be silently lost.
	if (Provider.IsValid() && Provider->HandlesImport())
	{
		if (!ArtifactCharacter)
		{
			Def->SetStatus(EMotionDefStatus::Failed, TEXT("No Motion Character to import against."));
			SaveAsset(Def);
			return;
		}

		// Same fork as the FBX path below. With a Provider Mesh the clip is built on the provider's
		// own rig and moved across by an IK Retargeter, which is what buys IK on the limbs; without
		// one it is built straight onto the game's skeleton by orientation matching alone. Both are
		// supported deliberately - the first is better, the second needs no setup at all.
		USkeletalMesh* ArtifactProviderMesh = ArtifactCharacter->ProviderMesh.LoadSynchronous();

		USkeleton* Skeleton = ArtifactProviderMesh
			? ArtifactProviderMesh->GetSkeleton()
			: ArtifactCharacter->TargetSkeleton.LoadSynchronous();

		if (!Skeleton)
		{
			Def->SetStatus(EMotionDefStatus::Failed, FString::Printf(
				TEXT("Character '%s' has no Target Skeleton, so there is nothing to build the "
					 "animation on."),
				*ArtifactCharacter->GetDisplayName()));
			SaveAsset(Def);
			return;
		}

		const bool bArtifactRetargeting = ArtifactProviderMesh != nullptr;

		FMotionArtifactImport ArtifactRequest;
		ArtifactRequest.AbsoluteArtifactPath = RawPath;
		ArtifactRequest.DestinationPackagePath = bArtifactRetargeting
			? Settings->GetSourceTakesPath()
			: Settings->GetTakesPath();
		ArtifactRequest.AssetName = bArtifactRetargeting
			? FString::Printf(TEXT("AS_%s_Source"), *Def->GetName())
			: FString::Printf(TEXT("AS_%s"), *Def->GetName());
		ArtifactRequest.TargetSkeleton = Skeleton;
		ArtifactRequest.TrimWindow = Def->TrimWindow;
		ArtifactRequest.bZeroRootTranslation = Settings->bZeroRootTranslation;

		const FMotionArtifactResult ArtifactResult = Provider->ImportArtifact(ArtifactRequest);
		if (!ArtifactResult.bSuccess)
		{
			Def->SetStatus(EMotionDefStatus::Failed, ArtifactResult.Error);
			SaveAsset(Def);
			return;
		}

		UAnimSequence* ArtifactSequence = ArtifactResult.Sequence.LoadSynchronous();

		if (bArtifactRetargeting)
		{
			FString RetargetError;
			UAnimSequence* Retargeted =
				RetargetToCharacterRig(Def, ArtifactCharacter, ArtifactSequence, RetargetError);

			if (!Retargeted)
			{
				// The source clip is on disk and correct; only the conversion is missing. Say so
				// rather than implying it has to be generated again.
				Def->SetStatus(EMotionDefStatus::Failed, FString::Printf(
					TEXT("%s Built on the provider rig as '%s' - fix the retarget setup and run "
						 "Download And Import Selected again; nothing needs regenerating."),
					*RetargetError, *ArtifactResult.Sequence.ToString()));
				SaveAsset(Def);
				return;
			}

			ArtifactSequence = Retargeted;
		}

		FMotionProvenance::Stamp(ArtifactSequence, Def, Provider->GetProviderId().ToString(),
			Def->ModelId.IsEmpty() ? Provider->GetDefaultModelId() : Def->ModelId, Def->SelectedMotionId,
			Provider->GetDisplayName(), Provider->GetCaps().bIsLocal,
			Provider->GetCaps().NativeFrameRate, /*bWasNormalized*/ false, bArtifactRetargeting);

		Def->ImportedSequence = ArtifactSequence;
		Def->SetStatus(EMotionDefStatus::Ready);
		SaveAsset(Def);

		PlaceTakeOnPromptSequence(Def, ArtifactSequence);

		UE_LOG(LogMotionForge, Log, TEXT("'%s' is ready: %s (built by %s%s)"),
			*Def->GetName(), *ArtifactSequence->GetPathName(), *Provider->GetDisplayName(),
			bArtifactRetargeting ? TEXT(", retargeted from its own rig") : TEXT(""));
		return;
	}

	FMotionNormalizeRequest NormalizeRequest;
	NormalizeRequest.AbsoluteInputPath = RawPath;
	NormalizeRequest.AbsoluteOutputPath = FPaths::Combine(
		FPaths::GetPath(RawPath),
		FPaths::GetBaseFilename(RawPath).Replace(TEXT("_raw"), TEXT("_clean")) + TEXT(".fbx"));
	NormalizeRequest.TrimWindow = Def->TrimWindow;
	NormalizeRequest.FrameRate = ResolveFrameRate(Provider);
	NormalizeRequest.bZeroRootTranslation = Settings->bZeroRootTranslation;
	NormalizeRequest.bEnsureRootBone = Settings->bEnsureRootBone;

	const FMotionNormalizeResult NormalizeResult = FMotionNormalizeTask::Run(NormalizeRequest);
	if (!NormalizeResult.bSuccess)
	{
		Def->SetStatus(EMotionDefStatus::Failed, NormalizeResult.Error);
		SaveAsset(Def);
		return;
	}

	UMotionCharacter* Character = Def->Character.IsNull()
		? Settings->DefaultCharacter.LoadSynchronous()
		: Def->Character.LoadSynchronous();

	if (!Character)
	{
		Def->SetStatus(EMotionDefStatus::Failed, TEXT("No Motion Character to import against."));
		SaveAsset(Def);
		return;
	}

	// Import onto the provider's own rig when we have a copy of it. Their file matches that skeleton
	// exactly, so nothing has to be reconstructed; the difference from our rig is then handled by a
	// retargeter, which is built for exactly that and does it correctly.
	USkeletalMesh* ProviderMesh = Character->ProviderMesh.LoadSynchronous();
	USkeleton* ImportSkeleton = ProviderMesh
		? ProviderMesh->GetSkeleton()
		: Character->TargetSkeleton.LoadSynchronous();

	if (!ImportSkeleton)
	{
		Def->SetStatus(EMotionDefStatus::Failed,
			TEXT("No skeleton to import against - set Provider Mesh (preferred) or Target Skeleton "
				 "on the Motion Character."));
		SaveAsset(Def);
		return;
	}

	const bool bRetargeting = ProviderMesh != nullptr;

	FMotionImportRequest ImportRequest;
	ImportRequest.AbsoluteFbxPath = NormalizeResult.OutputPath;
	ImportRequest.DestinationPackagePath = bRetargeting
		? Settings->GetSourceTakesPath()
		: Settings->GetTakesPath();

	// Land the provider-rig clip under a _Source name when it is an intermediate, so the name a
	// human reaches for always belongs to the asset on our own skeleton.
	ImportRequest.AssetName = bRetargeting
		? FString::Printf(TEXT("AS_%s_Source"), *Def->GetName())
		: FString::Printf(TEXT("AS_%s"), *Def->GetName());

	ImportRequest.TargetSkeleton = ImportSkeleton;
	ImportRequest.FrameRate = ResolveFrameRate(Provider);

	const FMotionImportResult ImportResult = FMotionImporter::Import(ImportRequest);
	if (!ImportResult.bSuccess)
	{
		Def->SetStatus(EMotionDefStatus::Failed, ImportResult.Error);
		SaveAsset(Def);
		return;
	}

	UAnimSequence* Imported = ImportResult.Sequence.LoadSynchronous();
	UAnimSequence* Final = Imported;

	if (bRetargeting)
	{
		FString RetargetError;
		UAnimSequence* Retargeted = RetargetToCharacterRig(Def, Character, Imported, RetargetError);

		if (!Retargeted)
		{
			// The source clip is on disk and correct; only the conversion is missing. Say that,
			// rather than implying the whole thing has to be generated again - it does not, and on
			// pay-as-you-go regenerating would cost money for nothing.
			Def->SetStatus(EMotionDefStatus::Failed, FString::Printf(
				TEXT("%s Imported on the provider rig as '%s' - fix the retarget setup and run "
					 "Download And Import Selected again; nothing needs regenerating."),
				*RetargetError, *ImportResult.Sequence.ToString()));
			SaveAsset(Def);
			return;
		}

		Final = Retargeted;
	}

	// Whether the Blender round trip actually ran, not whether it was asked for. The distinction is
	// the whole reason this field exists: it used to depend on which computer did the import.
	FMotionProvenance::Stamp(Final, Def, Provider->GetProviderId().ToString(),
		Def->ModelId.IsEmpty() ? Provider->GetDefaultModelId() : Def->ModelId, Def->SelectedMotionId,
		Provider->GetDisplayName(), Provider->GetCaps().bIsLocal,
		Provider->GetCaps().NativeFrameRate, !NormalizeResult.bSkipped, bRetargeting);

	Def->ImportedSequence = Final;
	Def->SetStatus(EMotionDefStatus::Ready);
	SaveAsset(Def);

	PlaceTakeOnPromptSequence(Def, Final);

	UE_LOG(LogMotionForge, Log, TEXT("'%s' is ready: %s%s%s"),
		*Def->GetName(),
		*Final->GetPathName(),
		bRetargeting ? TEXT(" (retargeted from the provider rig)") : TEXT(""),
		NormalizeResult.bSkipped ? TEXT(" (imported without normalisation)") : TEXT(""));
}

UAnimSequence* UMotionForgeSubsystem::RetargetToCharacterRig(
	UMotionDef* Def,
	UMotionCharacter* Character,
	UAnimSequence* SourceSequence,
	FString& OutError)
{
	// The clip's own retargeter wins over the character's, so one clip can be moved across
	// differently from the rest of the library without changing anything the others depend on.
	const bool bOverridden = !Def->RetargeterOverride.IsNull();

	UIKRetargeter* Retargeter = bOverridden
		? Def->RetargeterOverride.LoadSynchronous()
		: Character->Retargeter.LoadSynchronous();

	if (!Retargeter)
	{
		OutError = bOverridden
			? FString::Printf(
				TEXT("'%s' has a Retargeter Override that could not be loaded (%s), so the clip "
					 "cannot be moved onto the game's skeleton. Clear the override to fall back to "
					 "the character's."),
				*Def->GetName(), *Def->RetargeterOverride.ToString())
			: FString::Printf(
				TEXT("'%s' has a Provider Mesh but no Retargeter, so the clip cannot be moved onto "
					 "the game's skeleton. Author an IK Retargeter from Provider Mesh to Preview "
					 "Mesh and set it on the character."),
				*Character->GetDisplayName());
		return nullptr;
	}

	UE_CLOG(bOverridden, LogMotionForge, Log,
		TEXT("'%s' is retargeting with its own override, %s, rather than the character's."),
		*Def->GetName(), *Retargeter->GetName());

	USkeletalMesh* SourceMesh = Character->ProviderMesh.LoadSynchronous();
	USkeletalMesh* TargetMesh = Character->PreviewMesh.LoadSynchronous();

	if (!TargetMesh)
	{
		OutError = TEXT("Retargeting needs Preview Mesh set to the mesh on the game's skeleton.");
		return nullptr;
	}

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	FIKRetargetBatchOperationInputs Inputs;
	Inputs.AssetsToRetarget.Add(FAssetData(SourceSequence));
	Inputs.SourceMesh = SourceMesh;
	Inputs.TargetMesh = TargetMesh;
	Inputs.IKRetargetAsset = Retargeter;
	Inputs.TargetPath = Settings->GetTakesPath();
	Inputs.bUseSourcePath = false;
	Inputs.bOverwriteExistingFiles = true;
	Inputs.bIncludeReferencedAssets = false;

	// The source is named AS_<Def>_Source; strip the suffix so the retargeted result takes the
	// plain name. Left alone, the batch operation would append its own and produce AS_X_Source_1.
	Inputs.Search = TEXT("_Source");
	Inputs.Replace = TEXT("");

	const TArray<FAssetData> Produced = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);

	for (const FAssetData& Asset : Produced)
	{
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset()))
		{
			SaveAsset(Sequence);

			// The intermediate has done its job. Delete it unless somebody asked to keep it.
			//
			// It is derivable twice over - the cached .mfmo rebuilds it without touching the
			// provider, and a seeded generator reproduces the clip outright - so keeping it by
			// default only doubles the asset count and leaves two similarly-named animations for the
			// next person to choose between.
			//
			// Deleted only after the retargeted result is safely saved, and only if the two are
			// genuinely different assets, so a failure upstream never costs both.
			if (!Settings->bKeepSourceClips && Sequence != SourceSequence)
			{
				DeleteSourceClip(SourceSequence);
			}

			return Sequence;
		}
	}

	OutError = FString::Printf(
		TEXT("Retargeting '%s' produced no animation. The IK Retargeter's source and target rigs "
			 "most likely do not match Provider Mesh and Preview Mesh."),
		*SourceSequence->GetName());
	return nullptr;
}

void UMotionForgeSubsystem::DeleteSourceClip(UAnimSequence* SourceSequence)
{
	if (!SourceSequence)
	{
		return;
	}

	const FString Name = SourceSequence->GetName();

	// Force-deleted, because the retargeter leaves a reference behind.
	//
	// `RunBatchRetarget` records the asset it came from, so an ordinary delete refuses on a live
	// referencer and the clip stays in the project looking like the setting did nothing. The
	// references being severed are exactly the ones being made obsolete.
	TArray<UObject*> ToDelete = { SourceSequence };

	const int32 Deleted = ObjectTools::ForceDeleteObjects(ToDelete, /*ShowConfirmation*/ false);

	UE_CLOG(Deleted > 0, LogMotionForge, Log,
		TEXT("Removed the intermediate '%s'. Turn on Keep Source Clips to keep them."), *Name);

	UE_CLOG(Deleted == 0, LogMotionForge, Warning,
		TEXT("Could not remove the intermediate '%s'; it is still in the project."), *Name);
}

void UMotionForgeSubsystem::ImportProviderCharacter(
	const FString& CharacterAssetPath,
	TFunction<void(bool, const FString&, const FString&)> OnComplete)
{
	UMotionCharacter* Character = LoadObject<UMotionCharacter>(nullptr, *CharacterAssetPath);
	if (!Character)
	{
		OnComplete(false, FString(), FString::Printf(TEXT("No motion character at '%s'."), *CharacterAssetPath));
		return;
	}

	if (Character->ProviderCharacterId.IsEmpty())
	{
		OnComplete(false, FString(), FString::Printf(
			TEXT("'%s' is not paired with a provider yet. Upload it first."),
			*Character->GetDisplayName()));
		return;
	}

	const FName ProviderId = Character->ProviderId.IsNone()
		? UMotionForgeSettings::Get()->DefaultProviderId
		: Character->ProviderId;

	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, FString(), FString::Printf(TEXT("No provider registered as '%s'."), *ProviderId.ToString()));
		return;
	}

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();
	const FString FilePath = Settings->GetAbsoluteStagingDirectory()
		/ TEXT("Characters") / FString::Printf(TEXT("%s_provider.fbx"), *Character->GetName());

	TWeakObjectPtr<UMotionCharacter> WeakCharacter = Character;
	const FString OutputPath = Settings->GetRigsPath();
	const FString AssetName = FString::Printf(TEXT("SKM_%s_Provider"), *Character->GetName());

	Provider->DownloadCharacterFile(Character->ProviderCharacterId, FilePath,
		[WeakCharacter, FilePath, OutputPath, AssetName, OnComplete](bool bSuccess, const FString& Error)
		{
			if (!bSuccess)
			{
				OnComplete(false, FString(), Error);
				return;
			}

			UMotionCharacter* Live = WeakCharacter.Get();
			if (!Live)
			{
				OnComplete(false, FString(), TEXT("The character asset went away during download."));
				return;
			}

			FString ImportError;
			USkeletalMesh* Mesh = FMotionImporter::ImportSkeletalMesh(
				FilePath, OutputPath, AssetName, ImportError);

			if (!Mesh)
			{
				OnComplete(false, FString(), ImportError);
				return;
			}

			Live->ProviderMesh = Mesh;
			Live->MarkPackageDirty();
			SaveAsset(Live);
			SaveAsset(Mesh);

			UE_LOG(LogMotionForge, Log,
				TEXT("Imported provider rig for '%s' as %s. Author an IK Retargeter from it to the "
					 "Preview Mesh and set it on the character."),
				*Live->GetDisplayName(), *Mesh->GetPathName());

			OnComplete(true, Mesh->GetPathName(), FString());
		});
}

bool UMotionForgeSubsystem::CancelBatch(const FString& BatchId)
{
	if (FMotionBatch* Batch = Batches.Find(BatchId))
	{
		Batch->bCancelled = true;

		// Definitions are left where they are rather than reset. Their jobs keep running provider
		// side and the motion ids are already recorded, so the work is recoverable.
		UE_LOG(LogMotionForge, Log, TEXT("Batch %s cancelled - submitted jobs keep running."), *BatchId);
		return true;
	}
	return false;
}

// -------------------------------------------------------------------------------------------------
// Observation
// -------------------------------------------------------------------------------------------------

TArray<FMotionDefinitionStatus> UMotionForgeSubsystem::GetStatus(const TArray<FString>& AssetPaths) const
{
	TArray<FString> Paths = AssetPaths;
	if (Paths.Num() == 0)
	{
		Paths = FindMotionDefs({});
	}

	TArray<FMotionDefinitionStatus> Result;
	Result.Reserve(Paths.Num());

	for (const FString& Path : Paths)
	{
		UMotionDef* Def = LoadDef(Path);
		if (!Def)
		{
			continue;
		}

		FMotionDefinitionStatus Entry;
		Entry.AssetPath = Path;
		Entry.Name = Def->GetName();
		Entry.Status = Def->Status;
		Entry.Prompt = Def->Prompt;
		Entry.Length = Def->Length;
		Entry.Variants = Def->Variants;
		Entry.SelectedMotionId = Def->SelectedMotionId;
		Entry.LastError = Def->LastError;
		Entry.ImportedSequencePath = Def->ImportedSequence.ToString();

		// Resolved, and flagged when it was inherited. A definition naming no provider is not a
		// definition with no provider - it follows the project default, and which one it landed on is
		// the fact worth reporting.
		Entry.bProviderInherited = Def->ProviderId.IsNone();
		Entry.ProviderId = Entry.bProviderInherited
			? UMotionForgeSettings::Get()->DefaultProviderId
			: Def->ProviderId;

		// Ready with nothing to show for it. The status records what the pipeline did and stays true
		// after the clip is deleted, so the registry is the only thing that knows.
		if (!Entry.ImportedSequencePath.IsEmpty())
		{
			const FAssetRegistryModule& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

			Entry.bImportedSequenceMissing =
				!AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(Entry.ImportedSequencePath)).IsValid();
		}

		Entry.Takes.Reserve(Def->Candidates.Num());
		for (const FMotionCandidate& Candidate : Def->Candidates)
		{
			FMotionTakeInfo Take;
			Take.MotionId = Candidate.MotionId;
			Take.Variant = Candidate.VariantIndex;
			Take.Status = Candidate.Status;
			Take.ViewerUrl = Candidate.ViewerUrl;
			Take.bDownloaded = Candidate.bDownloaded;
			Take.Error = Candidate.Error;
			Entry.Takes.Add(MoveTemp(Take));
		}

		Result.Add(MoveTemp(Entry));
	}

	return Result;
}

FMotionBatchStatus UMotionForgeSubsystem::GetBatchStatus(const FString& BatchId) const
{
	FMotionBatchStatus Status;
	Status.BatchId = BatchId;

	const FMotionBatch* Batch = Batches.Find(BatchId);
	if (!Batch)
	{
		// A batch disappears once every job settles, so "unknown" and "finished" look the same from
		// outside. Report finished rather than implying failure, and flag that it is untracked so a
		// caller can tell the difference between "no jobs left" and "no such batch".
		Status.bTracked = false;
		Status.bFinished = true;
		return Status;
	}

	int32 Settled = 0;
	for (const FMotionJobTracking& Job : Batch->Jobs)
	{
		if (Job.bSettled)
		{
			++Settled;
		}
	}

	Status.bTracked = true;
	Status.Mode = Batch->Mode;
	Status.JobsTotal = Batch->Jobs.Num();
	Status.JobsSettled = Settled;
	Status.bFinished = Batch->Jobs.Num() > 0 && Settled == Batch->Jobs.Num();
	Status.bCancelled = Batch->bCancelled;
	Status.DefinitionPaths = Batch->DefinitionPaths;
	return Status;
}

void UMotionForgeSubsystem::ApplyBilling(FMotionCostEstimate& Estimate, bool bAnyMetered)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	Estimate.BillingModel = Settings->BillingModel;

	// A local provider bills nothing, so reporting a plan and a rate against it would be a lie with a
	// number attached. The second counts stay - they are still the honest size of the work - but
	// nothing is billed and nothing is owed.
	if (!bAnyMetered)
	{
		Estimate.BilledSeconds = 0;
		Estimate.EstimatedCost = 0.f;
		Estimate.Currency = Settings->Currency;
		return;
	}
	Estimate.Currency = Settings->Currency;

	Estimate.BilledSeconds = Settings->BillingModel == EMotionBillingModel::PayPerGeneratedSecond
		? Estimate.GeneratedSeconds
		: Estimate.DownloadSeconds;

	Estimate.EstimatedCost = Estimate.BilledSeconds * Settings->RatePerBilledSecond;
}

FMotionCostEstimate UMotionForgeSubsystem::EstimateCost(const TArray<FString>& AssetPaths, bool bSelectedOnly) const
{
	TArray<FString> Paths = AssetPaths;
	if (Paths.Num() == 0)
	{
		Paths = FindMotionDefs({});
	}

	FMotionCostEstimate Estimate;
	Estimate.bSelectedOnly = bSelectedOnly;

	bool bAnyMetered = false;

	for (const FString& Path : Paths)
	{
		if (const UMotionDef* Def = LoadDef(Path))
		{
			const int32 Seconds = Def->EstimateDownloadSeconds(bSelectedOnly);
			Estimate.DownloadSeconds += Seconds;
			if (Seconds > 0)
			{
				++Estimate.Clips;
			}

			// What re-running Generate on this definition would produce, so the same call answers
			// "what would fetching these cost" and "what would making them again cost".
			Estimate.GeneratedSeconds += Def->Length * FMath::Max(1, Def->Variants);

			if (TSharedPtr<IMotionProvider> Provider = FindProvider(Def->ProviderId))
			{
				bAnyMetered |= Provider->GetCaps().bIsMetered;
			}
		}
	}

	ApplyBilling(Estimate, bAnyMetered);
	return Estimate;
}

FMotionCostEstimate UMotionForgeSubsystem::EstimateGenerationCost(const TArray<FMotionDefSpec>& Specs) const
{
	FMotionCostEstimate Estimate;

	bool bAnyMetered = false;

	for (const FMotionDefSpec& Spec : Specs)
	{
		// Clamp the way submission will, or the estimate understates a four-second floor.
		int32 Length = Spec.Length;

		const FName ProviderId = Spec.ProviderId.IsNone()
			? UMotionForgeSettings::Get()->DefaultProviderId
			: Spec.ProviderId;

		if (TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId))
		{
			int32 Min = 0;
			int32 Max = 0;
			Provider->GetLengthRange(Spec.ModelId, Min, Max);
			Length = FMath::Clamp(Length, Min, Max);

			bAnyMetered |= Provider->GetCaps().bIsMetered;
		}

		Estimate.GeneratedSeconds += Length * FMath::Max(1, Spec.Variants);
		++Estimate.Clips;
	}

	ApplyBilling(Estimate, bAnyMetered);
	return Estimate;
}

bool UMotionForgeSubsystem::GetCredentialInfo(FName ProviderId, FMotionCredentialInfo& OutInfo) const
{
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		return false;
	}

	OutInfo.ProviderId = Provider->GetProviderId();
	OutInfo.DisplayName = Provider->GetDisplayName();
	OutInfo.bConfigured = Provider->HasCredential();
	OutInfo.Source = FMotionCredentialStore::DescribeSource(Provider->GetCredentialServiceName());
	return true;
}

FString UMotionForgeSubsystem::GetStatusJson(const TArray<FString>& AssetPaths) const
{
	TArray<TSharedPtr<FJsonValue>> Entries;

	for (const FMotionDefinitionStatus& Definition : GetStatus(AssetPaths))
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Definition.AssetPath);
		Entry->SetStringField(TEXT("name"), Definition.Name);
		Entry->SetStringField(TEXT("status"), MotionForgeJson::StatusToString(Definition.Status));
		Entry->SetStringField(TEXT("prompt"), Definition.Prompt);
		Entry->SetNumberField(TEXT("length"), Definition.Length);
		Entry->SetNumberField(TEXT("variants"), Definition.Variants);
		Entry->SetStringField(TEXT("provider"), Definition.ProviderId.ToString());
		Entry->SetBoolField(TEXT("providerInherited"), Definition.bProviderInherited);
		Entry->SetStringField(TEXT("selectedMotionId"), Definition.SelectedMotionId);
		Entry->SetStringField(TEXT("lastError"), Definition.LastError);
		Entry->SetStringField(TEXT("sequence"), Definition.ImportedSequencePath);
		Entry->SetBoolField(TEXT("sequenceMissing"), Definition.bImportedSequenceMissing);

		TArray<TSharedPtr<FJsonValue>> TakeValues;
		for (const FMotionTakeInfo& Take : Definition.Takes)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("motionId"), Take.MotionId);
			Object->SetNumberField(TEXT("variant"), Take.Variant);
			Object->SetStringField(TEXT("status"), MotionForgeJson::JobStatusToString(Take.Status));
			Object->SetStringField(TEXT("viewerUrl"), Take.ViewerUrl);
			Object->SetBoolField(TEXT("downloaded"), Take.bDownloaded);
			Object->SetStringField(TEXT("error"), Take.Error);
			TakeValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		Entry->SetArrayField(TEXT("candidates"), TakeValues);

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetArrayField(TEXT("definitions"), Entries);
	return MotionForgeJson::Serialize(Root);
}

FString UMotionForgeSubsystem::GetBatchStatusJson(const FString& BatchId) const
{
	const FMotionBatchStatus Status = GetBatchStatus(BatchId);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("batchId"), Status.BatchId);
	Root->SetBoolField(TEXT("finished"), Status.bFinished);

	if (!Status.bTracked)
	{
		Root->SetStringField(TEXT("note"),
			TEXT("Not tracked - either finished or never existed. Check definition status."));
		return MotionForgeJson::Serialize(Root);
	}

	TArray<TSharedPtr<FJsonValue>> PathValues;
	for (const FString& Path : Status.DefinitionPaths)
	{
		PathValues.Add(MakeShared<FJsonValueString>(Path));
	}

	Root->SetStringField(TEXT("mode"),
		Status.Mode == EMotionPipelineMode::Automatic ? TEXT("Automatic") : TEXT("HumanInTheLoop"));
	Root->SetNumberField(TEXT("jobsTotal"), Status.JobsTotal);
	Root->SetNumberField(TEXT("jobsSettled"), Status.JobsSettled);
	Root->SetBoolField(TEXT("cancelled"), Status.bCancelled);
	Root->SetArrayField(TEXT("definitions"), PathValues);
	return MotionForgeJson::Serialize(Root);
}

FString UMotionForgeSubsystem::EstimateCostJson(const TArray<FString>& AssetPaths, bool bSelectedOnly) const
{
	const FMotionCostEstimate Estimate = EstimateCost(AssetPaths, bSelectedOnly);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetNumberField(TEXT("downloadSeconds"), Estimate.DownloadSeconds);
	Root->SetNumberField(TEXT("clips"), Estimate.Clips);
	Root->SetBoolField(TEXT("selectedOnly"), Estimate.bSelectedOnly);
	Root->SetStringField(TEXT("note"),
		TEXT("Seconds of motion that would be fetched. Already-downloaded takes are excluded. What a "
			 "second costs depends on your plan."));
	return MotionForgeJson::Serialize(Root);
}

FString UMotionForgeSubsystem::DescribeCredential(FName ProviderId) const
{
	FMotionCredentialInfo Info;
	if (!GetCredentialInfo(ProviderId, Info))
	{
		return MotionForgeJson::Error(FString::Printf(TEXT("No provider '%s'."), *ProviderId.ToString()));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("provider"), Info.DisplayName);
	Root->SetBoolField(TEXT("configured"), Info.bConfigured);
	Root->SetStringField(TEXT("source"), Info.Source);
	return MotionForgeJson::Serialize(Root);
}

bool UMotionForgeSubsystem::SetCredential(FName ProviderId, const FString& Secret)
{
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		return false;
	}

	return FMotionCredentialStore::Set(Provider->GetCredentialServiceName(), Secret);
}

void UMotionForgeSubsystem::TestConnection(FName ProviderId)
{
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		UE_LOG(LogMotionForge, Error, TEXT("No provider '%s'."), *ProviderId.ToString());
		return;
	}

	Provider->TestConnection(
		[](bool bSuccess, const FString& Message)
		{
			if (bSuccess)
			{
				UE_LOG(LogMotionForge, Log, TEXT("Connection test: %s"), *Message);
			}
			else
			{
				UE_LOG(LogMotionForge, Error, TEXT("Connection test failed: %s"), *Message);
			}
		});
}

void UMotionForgeSubsystem::SaveAsset(UObject* Asset)
{
	if (!Asset)
	{
		return;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package || !Package->IsDirty())
	{
		return;
	}

	const FString FileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;

	UPackage::SavePackage(Package, Asset, *FileName, Args);
}
