#include "MotionForgeSubsystem.h"
#include "MotionTakeProvenance.h"

#include "MotionForge.h"
#include "MotionDef.h"
#include "MotionCharacter.h"
#include "MotionForgeSettings.h"
#include "MotionCredentialStore.h"
#include "MotionNormalizeTask.h"
#include "MotionImporter.h"
#include "MotionPipeline.h"

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
#include "Hash/CityHash.h"
#include "AnimPose.h"
#include "LevelSequence.h"
#include "Retargeter/IKRetargetProcessor.h"
#include "Retargeter/IKRetargetProfile.h"
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

namespace MotionForgeCost
{
	/** Money to two places, with the currency symbol people expect for the common ones. */
	static FString Money(double Amount, const FString& Currency)
	{
		const FString Number = FString::Printf(TEXT("%.2f"), Amount);

		if (Currency == TEXT("USD")) { return TEXT("$") + Number; }
		if (Currency == TEXT("EUR")) { return TEXT("EUR ") + Number; }
		if (Currency == TEXT("GBP")) { return TEXT("GBP ") + Number; }
		if (Currency == TEXT("CHF")) { return TEXT("CHF ") + Number; }

		return Number + TEXT(" ") + Currency;
	}

	static FString Seconds(float Value)
	{
		return FMath::IsNearlyEqual(Value, FMath::RoundToFloat(Value), 0.01f)
			? FString::Printf(TEXT("%d s"), FMath::RoundToInt(Value))
			: FString::Printf(TEXT("%.1f s"), Value);
	}

	static FString Takes(int32 Count)
	{
		return Count == 1 ? TEXT("1 take") : FString::Printf(TEXT("%d takes"), Count);
	}
}

namespace MotionForgePrompt
{
	/**
	 * A prompt cut the way a provider that splits at full stops cuts it.
	 *
	 * Mirrors the runner's own split_prompt, which mirrors kimodo_gen: on "." and nothing else, empty
	 * pieces dropped. Pairing sentence i with duration i here is not a guess - it is exactly how the
	 * provider will pair them.
	 */
	static TArray<FString> Split(const FString& Prompt)
	{
		TArray<FString> Pieces;
		Prompt.ParseIntoArray(Pieces, TEXT("."), /*bCullEmpty*/ false);

		TArray<FString> Beats;
		for (FString& Piece : Pieces)
		{
			Piece.TrimStartAndEndInline();
			if (!Piece.IsEmpty())
			{
				Beats.Add(Piece);
			}
		}
		return Beats;
	}

	/** The first number written with a decimal point, which a splitting provider cuts in two. */
	static FString FindDecimal(const FString& Prompt)
	{
		for (int32 Index = 1; Index + 1 < Prompt.Len(); ++Index)
		{
			if (Prompt[Index] == TEXT('.') && FChar::IsDigit(Prompt[Index - 1]) && FChar::IsDigit(Prompt[Index + 1]))
			{
				int32 Start = Index - 1;
				while (Start > 0 && FChar::IsDigit(Prompt[Start - 1])) { --Start; }
				int32 End = Index + 1;
				while (End + 1 < Prompt.Len() && FChar::IsDigit(Prompt[End + 1])) { ++End; }
				return Prompt.Mid(Start, End - Start + 1);
			}
		}
		return FString();
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
	Activities.Empty();
	Previews.Empty();
	PreviewWaiters.Empty();

	Super::Deinitialize();
}

void UMotionForgeSubsystem::ReleaseStrandedDefinitions()
{
	// Batches live in memory and nowhere else, so nothing can be mid-flight at startup - whatever was
	// tracking these definitions died with the last editor. A definition left saying Generating is
	// therefore stale **by construction**, and `IsBusy` refuses to submit it ever again.
	//
	// Found the honest way, by closing the editor for a build while a generation was in flight.
	int32 Released = 0;

	for (const FString& Path : FindMotionDefs({}))
	{
		// Status is on the registry tag for anything saved since tags existed, so most of the library
		// is skipped without being loaded.
		const FAssetRegistryModule& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		const FAssetData Data = Registry.Get().GetAssetByObjectPath(FSoftObjectPath(Path));

		FString Tag;
		if (Data.IsValid() && Data.GetTagValue(UMotionDef::TagStatus, Tag)
			&& Tag != TEXT("Generating") && Tag != TEXT("Downloading") && Tag != TEXT("Processing"))
		{
			continue;
		}

		UMotionDef* Def = LoadDef(Path);

		if (Def == nullptr || !Def->IsBusy())
		{
			continue;
		}

		SettleDefinition(Def,
			TEXT("The editor closed while this was working, so nothing was left tracking it. Generating "
				 "again is safe; any takes the provider did finish are still listed."));
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

FName UMotionForgeSubsystem::ResolveProviderId(FName ProviderId)
{
	if (!ProviderId.IsNone())
	{
		return ProviderId;
	}

	// One place decides what None means - the setting, or the only provider installed. Two call sites
	// used to disagree, and a definition then resolved to one provider for readiness and another for
	// submission.
	const FMotionForgeModule* Module = FMotionForgeModule::GetPtrIfLoaded();
	return Module ? Module->ResolveDefaultProviderId() : NAME_None;
}

TSharedPtr<IMotionProvider> UMotionForgeSubsystem::FindProvider(FName ProviderId) const
{
	const FMotionForgeModule* Module = FMotionForgeModule::GetPtr();
	return Module ? Module->FindProvider(ResolveProviderId(ProviderId)) : nullptr;
}

FMotionReadiness UMotionForgeSubsystem::CheckReadiness(const FString& AssetPath) const
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		FMotionReadiness Readiness;
		Readiness.Blocker = EMotionBlocker::NoProvider;
		Readiness.Problem = FString::Printf(TEXT("No motion definition at '%s'."), *AssetPath);
		return Readiness;
	}

	FMotionResolvedRequest Resolved;
	ResolveInternal(Def, Resolved, nullptr, /*bForSubmit*/ false);
	return Resolved.Readiness;
}

FMotionResolvedRequest UMotionForgeSubsystem::ResolveRequest(const FString& AssetPath) const
{
	FMotionResolvedRequest Resolved;

	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		Resolved.AssetPath = AssetPath;
		Resolved.Readiness.Blocker = EMotionBlocker::NoProvider;
		Resolved.Readiness.Problem = FString::Printf(TEXT("No motion definition at '%s'."), *AssetPath);
		return Resolved;
	}

	ResolveInternal(Def, Resolved, nullptr, /*bForSubmit*/ false);
	return Resolved;
}

void UMotionForgeSubsystem::NotifyProviderStateChanged(FName ProviderId)
{
	// Resolved, so a listener comparing against the id it drew with always matches - a definition
	// with no provider set is drawn with the default's caps and must still hear about them.
	ProviderStateChanged.Broadcast(ResolveProviderId(ProviderId));
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

TArray<FMotionSetupStepInfo> UMotionForgeSubsystem::GetSetupSteps(FName ProviderId) const
{
	TArray<FMotionSetupStepInfo> Out;

	const TArray<FName> Ids = ProviderId.IsNone() ? GetProviderIds() : TArray<FName>{ ProviderId };

	for (const FName Id : Ids)
	{
		TSharedPtr<IMotionProvider> Provider = FindProvider(Id);
		if (!Provider.IsValid())
		{
			continue;
		}

		TArray<FMotionSetupStep> Steps;
		Provider->GetSetupSteps(Steps);

		for (const FMotionSetupStep& Step : Steps)
		{
			FMotionSetupStepInfo& Info = Out.AddDefaulted_GetRef();
			Info.ProviderId = Provider->GetProviderId();
			Info.Label = Step.Label.ToString();
			Info.Detail = Step.Detail.ToString();
			Info.ActionLabel = Step.ActionLabel.ToString();
			Info.HelpUrl = Step.HelpUrl;
			Info.bOptional = Step.bOptional;

			switch (Step.State)
			{
			case EMotionSetupState::Done:    Info.State = TEXT("Done"); break;
			case EMotionSetupState::Todo:    Info.State = TEXT("Todo"); break;
			case EMotionSetupState::Waiting: Info.State = TEXT("Waiting"); break;
			case EMotionSetupState::Blocked: Info.State = TEXT("Blocked"); break;
			default:                         Info.State = TEXT("Unknown"); break;
			}
		}
	}

	return Out;
}

FMotionProviderCaps UMotionForgeSubsystem::GetProviderCaps(FName ProviderId) const
{
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		// An empty ProviderId is the "no such provider" answer. Returning defaults with the id filled
		// in would look like a real capability report for a provider that does not exist.
		return FMotionProviderCaps();
	}

	// The declarations are gathered here rather than asked of every provider's GetCaps, so a provider
	// states each fact once, in the function named for it.
	FMotionProviderCaps Caps = Provider->GetCaps();
	Caps.Models = Provider->GetModels();
	Caps.PromptSplitting = Provider->GetPromptSplitting();
	Caps.Billing = Provider->GetBilling();
	Caps.Tagline = Provider->GetTagline().ToString();

	if (UClass* PipelineClass = Provider->GetPipelineClass())
	{
		Caps.PipelineClass = PipelineClass->GetPathName();
	}

	return Caps;
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

	return 30;
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
	TArray<FString> Problems;
	return CreateMotionDefChecked(Spec, Problems);
}

FString UMotionForgeSubsystem::CreateMotionDefChecked(const FMotionDefSpec& Spec, TArray<FString>& OutProblems)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	FString AssetName = Spec.AssetName.IsEmpty() ? TEXT("MD_Untitled") : Spec.AssetName;
	AssetName = ObjectTools::SanitizeObjectName(AssetName);

	// Creating over an existing definition would drop its takes, and on providers with no seed those
	// cannot be made again. Found by name anywhere in the project, not only under today's output root -
	// moving the root used to make this create a second definition of the same name somewhere else.
	{
		const FAssetRegistryModule& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByClass(UMotionDef::StaticClass()->GetClassPathName(), Assets, true);

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString() == AssetName)
			{
				if (UMotionDef* Existing = Cast<UMotionDef>(Asset.GetAsset()))
				{
					Existing->Modify();
					Existing->ApplySpec(Spec, &OutProblems);
					Existing->MarkPackageDirty();
					SaveAsset(Existing);
					return Existing->GetPathName();
				}
			}
		}
	}

	const FString PackagePath = Settings->GetDefinitionsPath() / AssetName;

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UE_LOG(LogMotionForge, Error, TEXT("Could not create package '%s'."), *PackagePath);
		return FString();
	}

	UMotionDef* Def = NewObject<UMotionDef>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Def)
	{
		return FString();
	}

	// Provider first, so the defaults pick a character that suits it, then everything else the spec says.
	if (!Spec.ProviderId.IsNone())
	{
		Def->ProviderId = Spec.ProviderId;
	}
	Def->ApplyProjectDefaults();
	Def->ApplySpec(Spec, &OutProblems);

	FAssetRegistryModule::AssetCreated(Def);

	// A package created from C++ is not dirty, and SaveAsset skips clean packages - so without this a
	// new definition existed in memory and nowhere else.
	Def->MarkPackageDirty();
	SaveAsset(Def);

	UE_LOG(LogMotionForge, Log, TEXT("Created motion definition '%s'."), *Def->GetPathName());
	return Def->GetPathName();
}

bool UMotionForgeSubsystem::UpdateMotionDef(const FString& AssetPath, const FMotionDefSpec& Spec)
{
	TArray<FString> Problems;
	return UpdateMotionDefChecked(AssetPath, Spec, Problems);
}

bool UMotionForgeSubsystem::UpdateMotionDefChecked(const FString& AssetPath, const FMotionDefSpec& Spec, TArray<FString>& OutProblems)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		return false;
	}

	Def->Modify();
	Def->ApplySpec(Spec, &OutProblems);
	Def->MarkPackageDirty();
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

	const UEnum* StatusEnum = StaticEnum<EMotionDefStatus>();

	for (const FAssetData& Asset : Assets)
	{
		if (StatusFilter.Num() > 0)
		{
			// The saved tag answers without loading. Only an asset saved before tags existed has to be
			// loaded to be asked.
			FString Tag;
			if (Asset.GetTagValue(UMotionDef::TagStatus, Tag) && StatusEnum)
			{
				const int64 Value = StatusEnum->GetValueByNameString(Tag);
				if (Value == INDEX_NONE || !StatusFilter.Contains(static_cast<EMotionDefStatus>(Value)))
				{
					continue;
				}
			}
			else
			{
				const UMotionDef* Def = Cast<UMotionDef>(Asset.GetAsset());
				if (!Def || !StatusFilter.Contains(Def->Status))
				{
					continue;
				}
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
				 "second character and orphan takes generated against the first."),
			*Character->GetDisplayName(), *Character->ProviderCharacterId));
		return;
	}

	TSharedPtr<IMotionProvider> Provider = FindProvider(Character->ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, Empty, FString::Printf(TEXT("No provider registered as '%s'."),
			*ResolveProviderId(Character->ProviderId).ToString()));
		return;
	}

	if (!Provider->SupportsCharacterManagement())
	{
		OnComplete(false, Empty, FString::Printf(
			TEXT("%s does not take uploaded characters. It generates on its own rig."),
			*Provider->GetDisplayName()));
		return;
	}

	if (!Provider->HasCredential())
	{
		OnComplete(false, Empty, FString::Printf(
			TEXT("No API key for %s. Add it on the Keys page, or in Editor Preferences."),
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
	const FName ProviderId = Provider->GetProviderId();

	Provider->UploadCharacter(FbxPath, Character->GetDisplayName(), Options,
		[WeakCharacter, FbxPath, ProviderId, OnComplete](const FMotionCharacterUploadResult& Upload)
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
						 "could be written. Paste it into Provider Character Id by hand."),
					*Upload.CharacterId));
				return;
			}

			Live->ProviderCharacterId = Upload.CharacterId;
			Live->SourceFbxPath = FbxPath;

			// An uploaded character belongs to the provider it was uploaded to - its id means nothing
			// anywhere else - so it stops being universal.
			if (Live->ProviderId.IsNone())
			{
				Live->ProviderId = ProviderId;
			}

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
	TSharedPtr<IMotionProvider> Provider = FindProvider(ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, {}, FString::Printf(TEXT("No provider registered as '%s'."), *ResolveProviderId(ProviderId).ToString()));
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

	// Stopping for review unless the payload asks otherwise. An unattended run spends without anyone
	// looking, so it has to be asked for by name.
	EMotionPipelineMode Mode = EMotionPipelineMode::HumanInTheLoop;
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

		const TSharedPtr<FJsonObject>* Options = nullptr;
		if ((*Entry)->TryGetObjectField(TEXT("pipelineOptions"), Options) && Options)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Options)->Values)
			{
				Spec.PipelineOptions.Add(Pair.Key, Pair.Value->AsString());
			}
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
// Characters and providers
// -------------------------------------------------------------------------------------------------

bool UMotionForgeSubsystem::DoesCharacterSuit(
	const UMotionCharacter* Character, FName ProviderId, FString* OutReason, EMotionBlocker* OutBlocker) const
{
	FString Reason;
	EMotionBlocker Blocker = EMotionBlocker::None;

	auto Fail = [&](EMotionBlocker B, const FString& Why)
	{
		if (OutReason)  { *OutReason = Why; }
		if (OutBlocker) { *OutBlocker = B; }
		return false;
	};

	if (Character == nullptr)
	{
		return Fail(EMotionBlocker::NoCharacter, TEXT("No Motion Character."));
	}

	const FName Resolved = ResolveProviderId(ProviderId);
	TSharedPtr<IMotionProvider> Provider = FindProvider(Resolved);

	// A character naming another provider belongs to it: its rig, its uploaded id and its retargeter
	// were all made for that provider. Nothing else about it can make it suit this one.
	if (!Character->ProviderId.IsNone() && Character->ProviderId != Resolved)
	{
		TSharedPtr<IMotionProvider> Owner = FindProvider(Character->ProviderId);
		return Fail(EMotionBlocker::CharacterForOtherProvider, FString::Printf(
			TEXT("'%s' is prepared for %s, not %s."),
			*Character->GetDisplayName(),
			Owner.IsValid() ? *Owner->GetDisplayName() : *Character->ProviderId.ToString(),
			Provider.IsValid() ? *Provider->GetDisplayName() : *Resolved.ToString()));
	}

	if (!Provider.IsValid())
	{
		return Fail(EMotionBlocker::NoProvider, FString::Printf(TEXT("No provider registered as '%s'."), *Resolved.ToString()));
	}

	if (!Character->IsUsable(Reason))
	{
		return Fail(EMotionBlocker::CharacterUnusable, FString::Printf(TEXT("'%s': %s"), *Character->GetDisplayName(), *Reason));
	}

	if (!Provider->CheckCharacter(Character, Reason, Blocker))
	{
		return Fail(Blocker == EMotionBlocker::None ? EMotionBlocker::CharacterUnusable : Blocker, Reason);
	}

	if (OutReason)  { OutReason->Reset(); }
	if (OutBlocker) { *OutBlocker = EMotionBlocker::None; }
	return true;
}

TArray<FString> UMotionForgeSubsystem::FindCharactersFor(FName ProviderId) const
{
	const FName Resolved = ResolveProviderId(ProviderId);

	const FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	Registry.Get().GetAssetsByClass(UMotionCharacter::StaticClass()->GetClassPathName(), Assets, true);

	TArray<FString> Prepared;
	TArray<FString> Universal;

	for (const FAssetData& Asset : Assets)
	{
		const UMotionCharacter* Character = Cast<UMotionCharacter>(Asset.GetAsset());
		if (!DoesCharacterSuit(Character, Resolved))
		{
			continue;
		}

		(Character->ProviderId == Resolved ? Prepared : Universal).Add(Asset.GetSoftObjectPath().ToString());
	}

	Prepared.Sort();
	Universal.Sort();
	Prepared.Append(Universal);
	return Prepared;
}

FString UMotionForgeSubsystem::PickCharacterFor(const UMotionDef* Def, FName ProviderId) const
{
	const FName Resolved = ResolveProviderId(ProviderId);

	auto Suits = [this, Resolved](const FSoftObjectPath& Path)
	{
		if (Path.IsNull())
		{
			return false;
		}
		return DoesCharacterSuit(Cast<UMotionCharacter>(Path.TryLoad()), Resolved);
	};

	if (Def)
	{
		// The one this definition last used with this provider - switching back gives it back.
		if (const TSoftObjectPtr<UMotionCharacter>* Remembered = Def->CharacterByProvider.Find(Resolved))
		{
			if (Suits(Remembered->ToSoftObjectPath()))
			{
				return Remembered->ToString();
			}
		}

		if (Suits(Def->Character.ToSoftObjectPath()))
		{
			return Def->Character.ToString();
		}

		// The character its most recent take on this provider was made for.
		for (int32 Index = Def->Candidates.Num() - 1; Index >= 0; --Index)
		{
			const FMotionCandidate& Take = Def->Candidates[Index];
			if (Take.ProviderId == Resolved && Suits(Take.Character))
			{
				return Take.Character.ToString();
			}
		}
	}

	const FSoftObjectPath Default = UMotionForgeSettings::Get()->DefaultCharacter.ToSoftObjectPath();
	if (Suits(Default))
	{
		return Default.ToString();
	}

	const TArray<FString> Candidates = FindCharactersFor(Resolved);
	if (Candidates.Num() > 0)
	{
		return Candidates[0];
	}

	// Nothing passes every check - but a character prepared for this provider that needs one fix is
	// still the right character, and "none is prepared" would be false. Picked, and the caller says
	// what it needs.
	const FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FAssetData> Assets;
	Registry.Get().GetAssetsByClass(UMotionCharacter::StaticClass()->GetClassPathName(), Assets, true);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

	for (const FAssetData& Asset : Assets)
	{
		const UMotionCharacter* Character = Cast<UMotionCharacter>(Asset.GetAsset());
		if (Character && Character->ProviderId == Resolved)
		{
			return Asset.GetSoftObjectPath().ToString();
		}
	}

	return FString();
}

FString UMotionForgeSubsystem::CreateCharacterFromMesh(const FString& SkeletalMeshPath, FString& OutError)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("No skeletal mesh at '%s'."), *SkeletalMeshPath);
		return FString();
	}

	if (!Mesh->GetSkeleton())
	{
		OutError = FString::Printf(TEXT("'%s' has no skeleton, so nothing could play on it."), *Mesh->GetName());
		return FString();
	}

	FString BaseName = Mesh->GetName();
	BaseName.RemoveFromStart(TEXT("SKM_"));
	BaseName.RemoveFromStart(TEXT("SK_"));

	const FString AssetName = ObjectTools::SanitizeObjectName(TEXT("MC_") + BaseName);
	const FString PackagePath = UMotionForgeSettings::Get()->GetCharactersPath() / AssetName;

	if (UMotionCharacter* Existing = LoadObject<UMotionCharacter>(nullptr, *(PackagePath + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		OutError.Reset();
		return Existing->GetPathName();
	}

	UPackage* Package = CreatePackage(*PackagePath);
	UMotionCharacter* Character = Package
		? NewObject<UMotionCharacter>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;

	if (!Character)
	{
		OutError = FString::Printf(TEXT("Could not create '%s'."), *PackagePath);
		return FString();
	}

	Character->TargetSkeleton = Mesh->GetSkeleton();
	Character->PreviewMesh = Mesh;
	Character->DisplayName = BaseName;

	FAssetRegistryModule::AssetCreated(Character);
	Character->MarkPackageDirty();
	SaveAsset(Character);

	UE_LOG(LogMotionForge, Log, TEXT("Created Motion Character '%s' from '%s'."), *Character->GetPathName(), *Mesh->GetPathName());

	OutError.Reset();
	return Character->GetPathName();
}

bool UMotionForgeSubsystem::ClearCharacterProviderRig(const FString& CharacterPath, FString& OutMessage)
{
	UMotionCharacter* Character = LoadObject<UMotionCharacter>(nullptr, *CharacterPath);
	if (!Character)
	{
		OutMessage = FString::Printf(TEXT("No Motion Character at '%s'."), *CharacterPath);
		return false;
	}

	// The provider rig and its retargeter decide the retarget route; without them clips import straight
	// onto the character's own skeleton, which is right for a character the provider generates on.
	// The rig assets stay in the project, so this is undone by setting them again.
	Character->Modify();
	Character->ProviderMesh.Reset();
	Character->Retargeter.Reset();
	Character->MarkPackageDirty();
	SaveAsset(Character);

	OutMessage = FString::Printf(TEXT("'%s' now imports clips directly onto %s."),
		*Character->GetDisplayName(), *Character->TargetSkeleton.ToSoftObjectPath().GetAssetName());

	for (const FName Id : GetProviderIds())
	{
		NotifyProviderStateChanged(Id);
	}
	return true;
}

FString UMotionForgeSubsystem::OnDefinitionProviderChanged(UMotionDef* Def)
{
	if (Def == nullptr)
	{
		return FString();
	}

	const FName Provider = Def->GetResolvedProviderId();
	TSharedPtr<IMotionProvider> ProviderPtr = FindProvider(Provider);
	const FString ProviderName = ProviderPtr.IsValid() ? ProviderPtr->GetDisplayName() : Provider.ToString();

	// Its settings, created with the provider's defaults on first use and kept from before otherwise.
	Def->GetOrCreatePipeline(Provider);

	FString Message;

	UMotionCharacter* Current = Def->Character.LoadSynchronous();
	if (Current && DoesCharacterSuit(Current, Provider))
	{
		Message = FString::Printf(TEXT("%s suits %s, so it stays."), *Current->GetDisplayName(), *ProviderName);
	}
	else
	{
		const FString Picked = PickCharacterFor(Def, Provider);
		if (!Picked.IsEmpty())
		{
			Def->Character = TSoftObjectPtr<UMotionCharacter>(FSoftObjectPath(Picked));
			const UMotionCharacter* Now = Def->Character.LoadSynchronous();
			const FString Name = Now ? Now->GetDisplayName() : FSoftObjectPath(Picked).GetAssetName();

			FString Why;
			Message = DoesCharacterSuit(Now, Provider, &Why)
				? FString::Printf(TEXT("Switched the character to %s, which suits %s."), *Name, *ProviderName)
				: FString::Printf(TEXT("Switched the character to %s, prepared for %s. It needs a fix first: %s"),
					*Name, *ProviderName, *Why);
		}
		else
		{
			// Left as it was rather than cleared: nothing generates until a suitable one is chosen, and
			// the Character card lists how to make one.
			Message = FString::Printf(TEXT("No character is prepared for %s yet. Create one in the Character card."),
				*ProviderName);
		}
	}

	if (!Def->Character.IsNull())
	{
		Def->CharacterByProvider.Add(Provider, Def->Character);
	}

	Def->MarkPackageDirty();
	return Message;
}

FString UMotionForgeSubsystem::SetDefinitionProvider(const FString& AssetPath, FName ProviderId)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		return FString::Printf(TEXT("No motion definition at '%s'."), *AssetPath);
	}

	if (Def->IsBusy())
	{
		return TEXT("It is still working. Wait for it, or cancel, before switching provider.");
	}

	Def->Modify();

	// Remember who it used with the provider it is leaving, so coming back finds them again.
	if (!Def->Character.IsNull())
	{
		Def->CharacterByProvider.Add(Def->GetResolvedProviderId(), Def->Character);
	}

	Def->ProviderId = ProviderId;
	const FString Message = OnDefinitionProviderChanged(Def);
	SaveAsset(Def);
	return Message;
}

TArray<FMotionPipelineOption> UMotionForgeSubsystem::GetPipelineOptions(const FString& AssetPath, FName ProviderId) const
{
	const UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		return {};
	}

	const FName Resolved = ProviderId.IsNone() ? Def->GetResolvedProviderId() : ProviderId;
	const UMotionPipeline* Pipeline = Def->GetPipelineForRead(Resolved);
	return Pipeline ? Pipeline->DescribeOptions() : TArray<FMotionPipelineOption>();
}

bool UMotionForgeSubsystem::SetPipelineOption(const FString& AssetPath, const FString& Key, const FString& Value, FString& OutError)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		OutError = FString::Printf(TEXT("No motion definition at '%s'."), *AssetPath);
		return false;
	}

	UMotionPipeline* Pipeline = Def->GetOrCreatePipeline(Def->GetResolvedProviderId());
	if (!Pipeline)
	{
		OutError = FString::Printf(TEXT("%s declares no settings."), *Def->GetResolvedProviderId().ToString());
		return false;
	}

	Def->Modify();
	const bool bOk = Key.Equals(TEXT("model"), ESearchCase::IgnoreCase)
		? Pipeline->SetModelId(Value, OutError)
		: Pipeline->SetOption(Key, Value, OutError);

	if (bOk)
	{
		Def->MarkPackageDirty();
		SaveAsset(Def);
	}
	return bOk;
}

int32 UMotionForgeSubsystem::MigrateDefinitions()
{
	int32 Saved = 0;

	for (const FString& Path : FindMotionDefs({}))
	{
		UMotionDef* Def = LoadDef(Path);
		if (!Def)
		{
			continue;
		}

		const bool bMoved = Def->bMigratedOnLoad | Def->MigrateLegacySettings();
		if (!bMoved)
		{
			continue;
		}

		Def->bMigratedOnLoad = false;
		Def->MarkPackageDirty();
		SaveAsset(Def);
		++Saved;
	}

	UE_LOG(LogMotionForge, Log, TEXT("Resaved %d definition(s) with their settings in provider pipelines."), Saved);
	return Saved;
}

// -------------------------------------------------------------------------------------------------
// The resolver
// -------------------------------------------------------------------------------------------------

bool UMotionForgeSubsystem::ResolveInternal(
	UMotionDef* Def,
	FMotionResolvedRequest& Out,
	TArray<FMotionSubmitRequest>* OutRequests,
	bool bForSubmit,
	TSharedPtr<IMotionProvider>* OutProvider,
	UMotionCharacter** OutCharacter) const
{
	Out = FMotionResolvedRequest();
	Out.AssetPath = Def->GetPathName();

	FMotionReadiness& Readiness = Out.Readiness;

	// The first blocker wins and is what the window offers to fix; later ones would only compete with it.
	auto Block = [&Readiness](EMotionBlocker Blocker, const FString& Problem, const FString& Fix = FString())
	{
		if (Readiness.Blocker == EMotionBlocker::None || Readiness.Blocker == EMotionBlocker::ProviderStartable)
		{
			Readiness.Blocker = Blocker;
			Readiness.Problem = Problem;
			Readiness.FixLabel = Fix;
		}
	};

	Def->MigrateLegacySettings();

	// --- provider ---------------------------------------------------------------------------------

	Out.bProviderInherited = Def->ProviderId.IsNone();
	Out.ProviderId = ResolveProviderId(Def->ProviderId);

	TSharedPtr<IMotionProvider> Provider = FindProvider(Out.ProviderId);
	if (OutProvider)
	{
		*OutProvider = Provider;
	}

	if (!Provider.IsValid())
	{
		Block(EMotionBlocker::NoProvider, Out.ProviderId.IsNone()
			? FString(TEXT("Choose a provider in the Generate card. Several are installed and none is the project default."))
			: FString::Printf(TEXT("No provider '%s' is installed."), *Out.ProviderId.ToString()));
		Readiness.bCanGenerate = false;
		return false;
	}

	const FMotionProviderCaps Caps = Provider->GetCaps();
	Out.ProviderDisplayName = Provider->GetDisplayName();

	if (Def->IsBusy())
	{
		Block(EMotionBlocker::Busy, TEXT("It is already working. Wait for it, or cancel it."));
	}

	if (Caps.bNeedsCredential && !Provider->HasCredential())
	{
		Block(EMotionBlocker::NoCredential,
			FString::Printf(TEXT("%s needs an API key. Add it on the Keys page."), *Out.ProviderDisplayName),
			TEXT("Open Keys"));
	}

	if (!Caps.SetupHint.IsEmpty())
	{
		if (!Caps.PrepareLabel.IsEmpty())
		{
			// Not a refusal. Generate starts it first; the label says so on the button.
			if (Readiness.Blocker == EMotionBlocker::None)
			{
				Readiness.Blocker = EMotionBlocker::ProviderStartable;
				Readiness.Problem = Caps.SetupHint;
				Readiness.FixLabel = Caps.PrepareLabel;
			}
		}
		else
		{
			const FText Surface = Provider->GetSetupSurfaceLabel();
			Block(EMotionBlocker::ProviderNotReady,
				FString::Printf(TEXT("%s is not ready: %s"), *Out.ProviderDisplayName, *Caps.SetupHint),
				Surface.IsEmpty() ? FString() : FString::Printf(TEXT("Open %s"), *Surface.ToString()));
		}
	}

	// --- character --------------------------------------------------------------------------------

	UMotionCharacter* Character = Def->Character.LoadSynchronous();
	if (OutCharacter)
	{
		*OutCharacter = Character;
	}

	if (Character == nullptr)
	{
		const TArray<FString> Suitable = FindCharactersFor(Out.ProviderId);
		Block(EMotionBlocker::NoCharacter, Suitable.Num() > 0
			? FString::Printf(TEXT("Choose a character. %s suits %s."),
				*FSoftObjectPath(Suitable[0]).GetAssetName(), *Out.ProviderDisplayName)
			: FString::Printf(TEXT("No character is prepared for %s yet. Create one in the Character card."),
				*Out.ProviderDisplayName));
	}
	else
	{
		Out.CharacterPath = Character->GetPathName();
		Out.CharacterName = Character->GetDisplayName();
		Out.CharacterRoute = Provider->DescribeCharacterRoute(Character);

		FString Why;
		EMotionBlocker Blocker = EMotionBlocker::None;
		if (!DoesCharacterSuit(Character, Out.ProviderId, &Why, &Blocker))
		{
			if (Blocker == EMotionBlocker::CharacterForOtherProvider)
			{
				const TArray<FString> Suitable = FindCharactersFor(Out.ProviderId);
				Why += Suitable.Num() > 0
					? FString::Printf(TEXT(" Choose %s instead."), *FSoftObjectPath(Suitable[0]).GetAssetName())
					: FString::Printf(TEXT(" No character is prepared for %s yet."), *Out.ProviderDisplayName);
			}
			Block(Blocker, Why);
		}
	}

	// --- prompt -----------------------------------------------------------------------------------

	const int32 FrameRate = FMath::Max(1, Caps.NativeFrameRate);

	FMotionPromptRead Beats;
	FString PromptError;
	if (!FMotionPromptSequence::Resolve(Def, FrameRate, Beats, PromptError))
	{
		Block(EMotionBlocker::InvalidRequest, PromptError);
	}

	Out.bPromptFromTimeline = Beats.bFromSequence;
	Out.TimelinePath = Beats.SequencePath;
	Out.Prompt = Beats.Prompt;

	for (const FString& Problem : Beats.Problems)
	{
		Readiness.Warnings.Add(Problem);
	}

	if (Out.Prompt.TrimStartAndEnd().IsEmpty())
	{
		Block(EMotionBlocker::NoPrompt, Beats.bFromSequence
			? FString(TEXT("The prompt timeline has no beats with text."))
			: FString(TEXT("Write a prompt: who moves, what they do, and how it ends.")));
	}

	// --- the base request and the provider's settings ---------------------------------------------

	FMotionSubmitRequest Base;
	Base.Prompt = Out.Prompt;
	Base.ProviderCharacterId = Character ? Character->ProviderCharacterId : FString();
	Base.Definition = Def;

	// The authored half only: poses, the timeline, beat seconds. The sampler half is the pipeline's.
	Base.Control.Constraints = Def->Control.Constraints;
	Base.Control.ConstraintSequence = Def->Control.ConstraintSequence;
	Base.Control.ConstraintSequenceType = Def->Control.ConstraintSequenceType;

	const UMotionPipeline* Pipeline = Def->GetPipelineForRead(Out.ProviderId);
	if (Pipeline)
	{
		Pipeline->Apply(Base);
	}

	Base.ModelId = Base.ModelId.IsEmpty() ? Provider->GetDefaultModelId() : Base.ModelId;
	Out.ModelId = Base.ModelId;

	// --- length and beats -------------------------------------------------------------------------

	const FMotionPromptSplitting Splitting = Provider->GetPromptSplitting();

	float ModelMin = 1.f;
	float ModelMax = Splitting.MaxTotalSeconds;
	{
		const TArray<FMotionModelInfo> Models = Provider->GetModels();
		const FMotionModelInfo* Model = Models.FindByPredicate(
			[&Out](const FMotionModelInfo& M) { return M.Id == Out.ModelId; });

		if (Model)
		{
			ModelMin = Model->MinSeconds;
			ModelMax = Model->MaxSeconds;
		}
		else
		{
			int32 Min = 1;
			int32 Max = 10;
			Provider->GetLengthRange(Out.ModelId, Min, Max);
			ModelMin = Min;
			ModelMax = Max;

			if (Models.Num() > 0)
			{
				Block(EMotionBlocker::InvalidRequest, FString::Printf(
					TEXT("%s does not offer the model '%s'. Pick one in the Generate card."),
					*Out.ProviderDisplayName, *Out.ModelId));
			}
		}
	}

	Out.bSplitIntoBeats = Splitting.bSplitsAtFullStops && Base.Control.bSplitPromptIntoBeats;

	// The seconds per beat, from wherever they were stated. A timeline states them for every beat; a
	// definition only when somebody filled them in.
	TArray<float> BeatSeconds;
	if (Beats.bFromSequence)
	{
		for (const FMotionPromptBeat& Beat : Beats.Beats)
		{
			BeatSeconds.Add(Beat.Seconds);
		}
	}
	else
	{
		BeatSeconds = Def->Control.BeatSeconds;
	}

	float Length = Def->Length;

	if (Out.bSplitIntoBeats)
	{
		const TArray<FString> Sentences = MotionForgePrompt::Split(Out.Prompt);
		Out.BeatCount = FMath::Max(1, Sentences.Num());

		if (BeatSeconds.Num() > 0 && BeatSeconds.Num() != Out.BeatCount)
		{
			Block(EMotionBlocker::InvalidRequest, FString::Printf(
				TEXT("%d beat durations for a prompt %s divides into %d beats. Every full stop starts a beat; "
					 "match the durations to the sentences, or clear them to share the length evenly."),
				BeatSeconds.Num(), *Out.ProviderDisplayName, Out.BeatCount));
		}

		if (BeatSeconds.Num() != Out.BeatCount)
		{
			// Shared evenly by the provider, on whole frames. Worked out here the same way, so the beat
			// strip shows what will actually be generated.
			BeatSeconds.Reset();
			const int32 TotalFrames = FMath::Max(Out.BeatCount, FMath::RoundToInt(Length * FrameRate));
			const int32 PerBeat = TotalFrames / Out.BeatCount;
			const int32 Remainder = TotalFrames % Out.BeatCount;
			for (int32 Index = 0; Index < Out.BeatCount; ++Index)
			{
				BeatSeconds.Add(static_cast<float>(PerBeat + (Index < Remainder ? 1 : 0)) / FrameRate);
			}
			Base.Control.BeatSeconds.Reset();
		}
		else
		{
			Base.Control.BeatSeconds = BeatSeconds;

			float Sum = 0.f;
			for (const float Seconds : BeatSeconds)
			{
				Sum += Seconds;
			}

			if (!FMath::IsNearlyEqual(Sum, static_cast<float>(Def->Length), 0.05f))
			{
				Out.LengthNote = FString::Printf(TEXT("%s from the beats, not the %d s length."),
					*MotionForgeCost::Seconds(Sum), Def->Length);
			}
			Length = Sum;
		}

		float Start = 0.f;
		for (int32 Index = 0; Index < BeatSeconds.Num(); ++Index)
		{
			FMotionPromptBeat Beat;
			Beat.Text = Sentences.IsValidIndex(Index) ? Sentences[Index] : FString();
			Beat.Seconds = BeatSeconds[Index];
			Beat.StartSeconds = Start;
			Beat.StartFrame = FMath::RoundToInt(Start * FrameRate);
			Beat.Frames = FMath::RoundToInt(Beat.Seconds * FrameRate);
			Start += Beat.Seconds;
			Out.Beats.Add(Beat);

			if (Beat.Seconds > Splitting.MaxBeatSeconds + 0.01f)
			{
				Block(EMotionBlocker::LengthOutOfRange, FString::Printf(
					TEXT("Beat %d is %s. %s's limit is %s a beat; longer ones fall apart. Add a full stop "
						 "to split it, or shorten it."),
					Index + 1, *MotionForgeCost::Seconds(Beat.Seconds), *Out.ProviderDisplayName,
					*MotionForgeCost::Seconds(Splitting.MaxBeatSeconds)));
			}
			else if (Beat.Seconds < 0.1f)
			{
				Block(EMotionBlocker::LengthOutOfRange, FString::Printf(
					TEXT("Beat %d is too short to perform (%s)."), Index + 1, *MotionForgeCost::Seconds(Beat.Seconds)));
			}
		}

		if (Length > Splitting.MaxTotalSeconds + 0.01f)
		{
			Block(EMotionBlocker::LengthOutOfRange, FString::Printf(
				TEXT("The clip would be %s; %s makes at most %s in one take."),
				*MotionForgeCost::Seconds(Length), *Out.ProviderDisplayName,
				*MotionForgeCost::Seconds(Splitting.MaxTotalSeconds)));
		}

		// A decimal point is a beat boundary to a splitting provider. It is legal and almost never meant.
		const FString Decimal = MotionForgePrompt::FindDecimal(Out.Prompt);
		if (!Decimal.IsEmpty())
		{
			Readiness.Warnings.Add(FString::Printf(
				TEXT("\"%s\" will split a beat at its decimal point. Write the number in words."), *Decimal));
		}
	}
	else
	{
		Out.BeatCount = 1;

		// Beat seconds mean nothing to one generation of the whole prompt. A timeline still states the
		// length, as the sum of its beats; a definition's own beat seconds are ignored rather than
		// quietly summed into a length nobody asked for.
		Base.Control.BeatSeconds.Reset();

		if (Beats.bFromSequence && Beats.TotalSeconds > 0.f)
		{
			Length = Beats.TotalSeconds;
			Out.LengthNote = FString::Printf(TEXT("%s from the timeline."), *MotionForgeCost::Seconds(Length));
		}

		// One beat on a provider that splits, when splitting is off, is limited like any beat.
		const float Max = Splitting.bSplitsAtFullStops ? FMath::Min(ModelMax, Splitting.MaxBeatSeconds) : ModelMax;

		if (Length < ModelMin)
		{
			Out.LengthNote = FString::Printf(
				TEXT("Raised to %s, %s's shortest clip. The model pads a shorter action out to fill it; trim it on import."),
				*MotionForgeCost::Seconds(ModelMin), *Out.ProviderDisplayName);
			Length = ModelMin;
		}
		else if (Length > Max)
		{
			Out.LengthNote = FString::Printf(TEXT("Cut to %s, the longest %s makes in one piece."),
				*MotionForgeCost::Seconds(Max), *Out.ProviderDisplayName);
			Length = Max;
		}

		if (!Out.Prompt.IsEmpty())
		{
			FMotionPromptBeat Whole;
			Whole.Text = Out.Prompt;
			Whole.Seconds = Length;
			Whole.Frames = FMath::RoundToInt(Length * FrameRate);
			Out.Beats.Add(Whole);
		}
	}

	Out.LengthSeconds = Length;
	Base.LengthSeconds = Length;
	Base.Length = FMath::Max(1, FMath::RoundToInt(Length));

	// --- the provider's own checks, and the core's warnings ----------------------------------------

	if (Pipeline)
	{
		TArray<FString> Problems;
		Pipeline->Validate(Base, Problems, Readiness.Warnings);
		if (Problems.Num() > 0)
		{
			Block(EMotionBlocker::InvalidRequest, Problems[0]);
		}

		Out.Settings = Pipeline->DescribeSent(Base);
	}

	const int32 AuthoredKeys = Def->Control.CountAuthoredKeys();
	if (Caps.ConstraintTypes.Num() == 0)
	{
		if (AuthoredKeys > 0)
		{
			Readiness.Warnings.Add(FString::Printf(
				TEXT("This definition has %d constraint pose(s). %s does not take constraints, so they are not sent."),
				AuthoredKeys, *Out.ProviderDisplayName));
		}
	}
	else
	{
		switch (Base.Control.ConstraintSource)
		{
		case EMotionConstraintSource::None:
			Out.Constraints = TEXT("None: the prompt alone decides the motion.");
			break;

		case EMotionConstraintSource::Authored:
			Out.Constraints = AuthoredKeys > 0
				? FString::Printf(TEXT("%d authored pose key(s)."), AuthoredKeys)
				: TEXT("None authored.");
			break;

		case EMotionConstraintSource::Timeline:
			Out.Constraints = Def->Control.ConstraintSequence.IsNull()
				? TEXT("Timeline only, and there is no prompt timeline: nothing is sent.")
				: TEXT("The poses keyed on the prompt timeline, read when you generate.");
			break;

		default:
			Out.Constraints = !Def->Control.ConstraintSequence.IsNull()
				? (AuthoredKeys > 0
					? FString::Printf(TEXT("The poses keyed on the prompt timeline; if none are keyed there, the %d authored key(s)."), AuthoredKeys)
					: TEXT("The poses keyed on the prompt timeline, read when you generate."))
				: (AuthoredKeys > 0
					? FString::Printf(TEXT("%d authored pose key(s)."), AuthoredKeys)
					: TEXT("None yet. Pose the character on the prompt timeline to pin moments."));
			break;
		}
	}

	// --- takes and seeds --------------------------------------------------------------------------

	Out.Variants = FMath::Clamp(Def->Variants, 1, 16);

	for (int32 Variant = 0; Variant < Out.Variants; ++Variant)
	{
		// A fixed seed walks, so several takes from seed 42 are 42, 43, 44 - reproducible and different.
		Out.Seeds.Add(Caps.bSupportsSeed && Base.Control.Seed >= 0 ? Base.Control.Seed + Variant : -1);
	}

	// --- money ------------------------------------------------------------------------------------

	PriceRequest(*Provider, Out);

	// --- the recipe, for telling a stale take from a current one -----------------------------------

	{
		FString Recipe = FString::Printf(TEXT("%s|%s|%s|%s|%.3f|"),
			*Out.ProviderId.ToString(), *Out.ModelId, *Out.Prompt, *Out.CharacterPath, Out.LengthSeconds);

		for (const FMotionPromptBeat& Beat : Out.Beats)
		{
			Recipe += FString::Printf(TEXT("%.3f,"), Beat.Seconds);
		}

		// Everything in the pipeline except the seed, which differs per take by design.
		if (Pipeline)
		{
			for (const FString& Setting : Out.Settings)
			{
				if (!Setting.StartsWith(TEXT("seed=")))
				{
					Recipe += Setting + TEXT(";");
				}
			}
		}

		// The authored poses, for a provider that takes them: a pinned hand moved is a different
		// request, and a take made before it is an older recipe.
		//
		// Not the timeline's keys. They are only read at submission, and the timeline's own edit
		// signature cannot stand in for them: importing a take writes the clip onto that same sequence,
		// which would make every take look older the moment one was chosen.
		if (Caps.ConstraintTypes.Num() > 0 && Def->Control.Constraints.Num() > 0)
		{
			if (const FProperty* Authored = FMotionControl::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FMotionControl, Constraints)))
			{
				FString Poses;
				Authored->ExportTextItem_Direct(Poses, &Def->Control.Constraints, nullptr, nullptr, PPF_None);
				Recipe += TEXT("|poses:") + Poses;
			}
		}

		Out.RecipeHash = FString::Printf(TEXT("%016llx"), CityHash64(
			reinterpret_cast<const char*>(*Recipe), Recipe.Len() * sizeof(TCHAR)));
	}

	Out.bCanSubmit = Readiness.Blocker == EMotionBlocker::None || Readiness.Blocker == EMotionBlocker::ProviderStartable;
	Readiness.bCanGenerate = Out.bCanSubmit;

	if (!Out.bCanSubmit || OutRequests == nullptr)
	{
		return Out.bCanSubmit;
	}

	// --- the requests actually submitted ----------------------------------------------------------

	if (bForSubmit)
	{
		Base.TargetSkeleton = Character ? Character->TargetSkeleton.LoadSynchronous() : nullptr;

		TArray<FString> Warnings;
		Provider->PrepareRequest(Base, Warnings);

		for (const FString& Warning : Warnings)
		{
			UE_LOG(LogMotionForge, Warning, TEXT("'%s': %s"), *Def->GetName(), *Warning);
			Readiness.Warnings.Add(Warning);
		}
	}

	for (int32 Variant = 0; Variant < Out.Variants; ++Variant)
	{
		FMotionSubmitRequest Request = Base;
		Request.VariantIndex = Variant;

		if (Caps.bSupportsSeed)
		{
			// A random seed is chosen here, not left to the provider, so it can be recorded on the take -
			// which is what makes every take reproducible, not only the ones somebody seeded by hand.
			Request.Control.Seed = Base.Control.Seed >= 0
				? Base.Control.Seed + Variant
				: FMath::RandRange(1, MAX_int32 - 1);
			Out.Seeds[Variant] = Request.Control.Seed;
		}

		OutRequests->Add(MoveTemp(Request));
	}

	return true;
}

void UMotionForgeSubsystem::PriceRequest(const IMotionProvider& Provider, FMotionResolvedRequest& Resolved)
{
	FMotionCostEstimate& Cost = Resolved.Cost;
	Cost = FMotionCostEstimate();

	// Billed in whole requested seconds where the provider takes an integer; that is what the invoice
	// counts, not the length of the file that comes back.
	const float PerTake = FMath::Max(1, FMath::RoundToInt(Resolved.LengthSeconds));

	AccumulateCost(Cost, Provider.GetBilling(), Provider.GetDisplayName(),
		Resolved.Variants, PerTake * Resolved.Variants, /*DownloadSeconds*/ 0.f);

	FinishCost(Cost, {});
}

void UMotionForgeSubsystem::AccumulateCost(
	FMotionCostEstimate& Estimate,
	const FMotionBilling& Billing,
	const FString& ProviderName,
	int32 Takes,
	float GeneratedSeconds,
	float DownloadSeconds)
{
	using namespace MotionForgeCost;

	Estimate.Clips += Takes;
	Estimate.GeneratedSeconds += FMath::CeilToInt(GeneratedSeconds);
	Estimate.DownloadSeconds += FMath::CeilToInt(DownloadSeconds);
	Estimate.Currency = Billing.Currency;

	FString Line;

	// A download estimate carries no takes to generate, only seconds to fetch - and is worded as
	// fetching, never as "0 takes".
	const bool bFetchOnly = Takes == 0 && GeneratedSeconds <= 0.f;

	switch (Billing.Unit)
	{
	case EMotionBillingUnit::Free:
		Line = bFetchOnly
			? FString::Printf(TEXT("fetching %s from %s: free."), *Seconds(DownloadSeconds), *ProviderName)
			: FString::Printf(TEXT("%s on %s: free, %s."), *MotionForgeCost::Takes(Takes), *ProviderName,
				*Billing.Summary.Replace(TEXT("free, "), TEXT("")).Replace(TEXT("free"), TEXT("nothing is billed")));
		break;

	case EMotionBillingUnit::PerGeneratedSecond:
	{
		const float Money = GeneratedSeconds * Billing.Rate;
		Estimate.BilledSeconds += FMath::CeilToInt(GeneratedSeconds);
		Estimate.EstimatedCost += Money;
		Estimate.BillingModel = EMotionBillingModel::PayPerGeneratedSecond;
		Estimate.bSpendsMoney |= GeneratedSeconds > 0.f;

		Line = bFetchOnly
			? FString::Printf(TEXT("fetching %s from %s: free, because %s bills when a take is generated."),
				*Seconds(DownloadSeconds), *ProviderName, *ProviderName)
			: Billing.Rate > 0.f
			? FString::Printf(TEXT("about %s on %s: %s, %s at %s a second%s, billed when submitted whether kept or not."),
				*MotionForgeCost::Money(Money, Billing.Currency), *ProviderName, *MotionForgeCost::Takes(Takes),
				*Seconds(Takes > 0 ? GeneratedSeconds / Takes : 0.f), *MotionForgeCost::Money(Billing.Rate, Billing.Currency),
				Billing.RateNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Billing.RateNote))
			: FString::Printf(TEXT("%s on %s: %s billed when submitted. Set the rate on the provider's settings page to see money."),
				*MotionForgeCost::Takes(Takes), *ProviderName, *Seconds(GeneratedSeconds));
		break;
	}

	case EMotionBillingUnit::PerDownloadedSecond:
	{
		const float Money = DownloadSeconds * Billing.Rate;
		Estimate.BilledSeconds += FMath::CeilToInt(DownloadSeconds);
		Estimate.EstimatedCost += Money;
		Estimate.BillingModel = EMotionBillingModel::PayPerDownloadedSecond;
		Estimate.bSpendsMoney |= DownloadSeconds > 0.f;

		Line = DownloadSeconds > 0.f
			? FString::Printf(TEXT("%s of your %s download quota%s."),
				*Seconds(DownloadSeconds), *ProviderName,
				Billing.Rate > 0.f ? *FString::Printf(TEXT(", about %s"), *MotionForgeCost::Money(Money, Billing.Currency)) : TEXT(""))
			: FString::Printf(TEXT("%s on %s: free to generate on your subscription. Importing a take uses its seconds of your download quota."),
				*MotionForgeCost::Takes(Takes), *ProviderName);
		break;
	}

	case EMotionBillingUnit::PerHour:
		Estimate.bHourlyBillingNow |= Billing.bBillingNow;
		Estimate.HourlyRate = FMath::Max(Estimate.HourlyRate, Billing.Rate);

		// A take adds nothing to an hourly bill; the machine being up is what costs. Saying so is the
		// honest cost line - "free" would be false and a per-take price would be invented.
		Line = FString::Printf(TEXT("%s: no charge per take. The rented GPU costs %s an hour%s."),
			bFetchOnly
				? *FString::Printf(TEXT("fetching %s from %s"), *Seconds(DownloadSeconds), *ProviderName)
				: *FString::Printf(TEXT("%s on %s"), *MotionForgeCost::Takes(Takes), *ProviderName),
			Billing.Rate > 0.f ? *MotionForgeCost::Money(Billing.Rate, Billing.Currency) : TEXT("an unknown amount"),
			Billing.bBillingNow ? TEXT(", billing now whether or not anything generates") : TEXT(" while it runs"));
		break;
	}

	// The most expensive unit involved names the estimate, so a mixed selection is never labelled free.
	if (static_cast<uint8>(Billing.Unit) > static_cast<uint8>(Estimate.Unit) || Estimate.Summary.IsEmpty())
	{
		Estimate.Unit = FMath::Max(Estimate.Unit, Billing.Unit);
	}

	if (!Line.IsEmpty())
	{
		Line[0] = FChar::ToUpper(Line[0]);
		Estimate.Summary += (Estimate.Summary.IsEmpty() ? TEXT("") : TEXT("\n")) + Line;
	}
}

void UMotionForgeSubsystem::FinishCost(FMotionCostEstimate& Estimate, const TArray<FString>& Lines)
{
	for (const FString& Line : Lines)
	{
		Estimate.Summary += (Estimate.Summary.IsEmpty() ? TEXT("") : TEXT("\n")) + Line;
	}
}

// -------------------------------------------------------------------------------------------------
// Pipeline
// -------------------------------------------------------------------------------------------------

FString UMotionForgeSubsystem::MakeBatchId()
{
	static int32 Counter = 0;
	return FString::Printf(TEXT("batch_%s_%03d"),
		*FDateTime::Now().ToString(TEXT("%H%M%S")), ++Counter);
}

FString UMotionForgeSubsystem::Generate(const TArray<FString>& AssetPaths)
{
	// Always stops for review. Whether a button spends unattended is not something a project setting
	// should be able to change behind the person pressing it.
	return StartGeneration(AssetPaths, EMotionPipelineMode::HumanInTheLoop);
}

FString UMotionForgeSubsystem::RunFullPipeline(const TArray<FString>& AssetPaths)
{
	return StartGeneration(AssetPaths, EMotionPipelineMode::Automatic);
}

FString UMotionForgeSubsystem::StartGeneration(const TArray<FString>& AssetPaths, EMotionPipelineMode Mode)
{
	const FString BatchId = MakeBatchId();

	// Register the batch before submitting anything. A submit that fails at the transport layer can
	// invoke its callback immediately, and that callback looks the batch up by id - if it were not in
	// the map yet the result would be silently dropped.
	FMotionBatch NewBatch;
	NewBatch.BatchId = BatchId;
	NewBatch.Mode = Mode;
	NewBatch.StartedAt = FPlatformTime::Seconds();
	Batches.Add(BatchId, MoveTemp(NewBatch));

	// Definitions whose provider has to be started first, grouped so each provider starts once.
	TMap<FName, TArray<FString>> WaitingOnProvider;

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

		FMotionResolvedRequest Resolved;
		ResolveInternal(Def, Resolved, nullptr, /*bForSubmit*/ false);

		if (!Resolved.bCanSubmit)
		{
			Def->SetStatus(EMotionDefStatus::Failed, Resolved.Readiness.Problem);
			SaveAsset(Def);
			continue;
		}

		FMotionBatch& Batch = Batches[BatchId];

		if (Resolved.Readiness.Blocker == EMotionBlocker::ProviderStartable)
		{
			WaitingOnProvider.FindOrAdd(Resolved.ProviderId).Add(Def->GetPathName());

			Def->SetStatus(EMotionDefStatus::Generating);
			Def->ActiveBatchId = BatchId;
			Batch.DefinitionPaths.AddUnique(Def->GetPathName());
			SetActivity(Def->GetPathName(), FString::Printf(
				TEXT("%s - then %d %s"), *Resolved.Readiness.FixLabel, Resolved.Variants,
				Resolved.Variants == 1 ? TEXT("take") : TEXT("takes")), /*bCanCancel*/ true);
			SaveAsset(Def);
			continue;
		}

		SubmitDefinition(Batch, Def);
	}

	for (const TPair<FName, TArray<FString>>& Waiting : WaitingOnProvider)
	{
		TSharedPtr<IMotionProvider> Provider = FindProvider(Waiting.Key);
		if (!Provider.IsValid())
		{
			continue;
		}

		Batches[BatchId].PendingPrepares++;

		const TArray<FString> Paths = Waiting.Value;
		TWeakObjectPtr<UMotionForgeSubsystem> WeakThis(this);

		Provider->PrepareForWork([WeakThis, BatchId, Paths](bool bReady, const FString& Message)
		{
			UMotionForgeSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			FMotionBatch* Batch = Self->Batches.Find(BatchId);

			for (const FString& Path : Paths)
			{
				UMotionDef* Def = Self->LoadDef(Path);
				if (!Def)
				{
					continue;
				}

				// Cancelled while the provider was starting: the definition has already been settled.
				if (Batch == nullptr || Batch->bCancelled || Def->ActiveBatchId != BatchId)
				{
					continue;
				}

				if (!bReady)
				{
					Def->SetStatus(EMotionDefStatus::Failed, Message);
					Self->ClearActivity(Path);
					SaveAsset(Def);
					continue;
				}

				// Back to ready-to-submit, so SubmitDefinition's own busy check does not skip it.
				Def->Status = EMotionDefStatus::Draft;
				Self->SubmitDefinition(*Batch, Def);
			}

			if (Batch)
			{
				Batch->PendingPrepares = FMath::Max(0, Batch->PendingPrepares - 1);
			}
		});
	}

	FMotionBatch* Batch = Batches.Find(BatchId);
	if (!Batch || Batch->DefinitionPaths.Num() == 0)
	{
		UE_LOG(LogMotionForge, Warning, TEXT("Nothing eligible to generate."));
		Batches.Remove(BatchId);
		return FString();
	}

	UE_LOG(LogMotionForge, Log, TEXT("Batch %s: %d definition(s), mode %s."),
		*BatchId, Batch->DefinitionPaths.Num(),
		Mode == EMotionPipelineMode::Automatic ? TEXT("Automatic") : TEXT("HumanInTheLoop"));

	return BatchId;
}

void UMotionForgeSubsystem::SubmitDefinition(FMotionBatch& Batch, UMotionDef* Def)
{
	FMotionResolvedRequest Resolved;
	TArray<FMotionSubmitRequest> Requests;
	TSharedPtr<IMotionProvider> Provider;
	UMotionCharacter* Character = nullptr;

	const FString DefPath = Def->GetPathName();

	if (!ResolveInternal(Def, Resolved, &Requests, /*bForSubmit*/ true, &Provider, &Character)
		|| Requests.Num() == 0 || !Provider.IsValid())
	{
		Def->SetStatus(EMotionDefStatus::Failed, Resolved.Readiness.Problem.IsEmpty()
			? FString(TEXT("Nothing could be submitted."))
			: Resolved.Readiness.Problem);
		ClearActivity(DefPath);
		SaveAsset(Def);
		return;
	}

	const FName ProviderId = Provider->GetProviderId();
	const FString BatchId = Batch.BatchId;
	const FMotionBilling Billing = Provider->GetBilling();
	const float CostPerTake = Resolved.Variants > 0 ? Resolved.Cost.EstimatedCost / Resolved.Variants : 0.f;

	Def->Modify();
	Def->SetStatus(EMotionDefStatus::Generating);
	Def->ActiveBatchId = BatchId;
	Batch.DefinitionPaths.AddUnique(DefPath);

	if (!Def->Character.IsNull())
	{
		Def->CharacterByProvider.Add(ProviderId, Def->Character);
	}

	SetActivity(DefPath, FString::Printf(TEXT("Generating %d %s on %s"),
		Requests.Num(), Requests.Num() == 1 ? TEXT("take") : TEXT("takes"), *Resolved.ProviderDisplayName),
		/*bCanCancel*/ true);

	if (FMotionActivity* Activity = Activities.Find(DefPath))
	{
		Activity->Total = Requests.Num();
	}

	// Every take gets its record before it is submitted, carrying everything that made it. The
	// callback only fills in the job id - so a take shows in the list the moment Generate is pressed,
	// and a take whose submission fails still says what it was asked to be.
	TArray<int32> TakeNumbers;

	for (const FMotionSubmitRequest& Request : Requests)
	{
		FMotionCandidate Take;
		Take.TakeNumber = Def->NextTakeNumber++;
		Take.VariantIndex = Request.VariantIndex;
		Take.Status = EMotionJobStatus::Pending;
		Take.GeneratedAt = FDateTime::Now();
		Take.ProviderId = ProviderId;
		Take.ModelId = Request.ModelId;
		Take.Character = Character ? FSoftObjectPath(Character) : FSoftObjectPath();
		Take.Seed = Provider->GetCaps().bSupportsSeed ? Request.Control.Seed : -1;
		Take.LengthSeconds = Request.LengthSeconds;
		Take.PromptSent = Request.Prompt;
		Take.RecipeHash = Resolved.RecipeHash;
		Take.EstimatedCost = Billing.Unit == EMotionBillingUnit::PerGeneratedSecond ? CostPerTake : 0.f;
		Take.Currency = Billing.Currency;

		TArray<FString> Settings;
		for (const FString& Setting : Resolved.Settings)
		{
			Settings.Add(Setting.StartsWith(TEXT("seed=")) ? FString::Printf(TEXT("seed=%d"), Take.Seed) : Setting);
		}
		Take.SettingsSent = FString::Join(Settings, TEXT(" "));

		TakeNumbers.Add(Take.TakeNumber);
		Def->Candidates.Add(MoveTemp(Take));
	}

	Batch.PendingSubmits.FindOrAdd(DefPath) += Requests.Num();
	Def->MarkPackageDirty();
	SaveAsset(Def);

	for (int32 Index = 0; Index < Requests.Num(); ++Index)
	{
		const int32 TakeNumber = TakeNumbers[Index];

		Provider->SubmitJob(Requests[Index],
			[this, BatchId, DefPath, ProviderId, TakeNumber](const FMotionSubmitResult& SubmitResult)
			{
				FMotionBatch* LiveBatch = Batches.Find(BatchId);
				UMotionDef* LiveDef = LoadDef(DefPath);
				if (!LiveBatch || !LiveDef)
				{
					return;
				}

				if (int32* Pending = LiveBatch->PendingSubmits.Find(DefPath))
				{
					*Pending = FMath::Max(0, *Pending - 1);
				}

				FMotionCandidate* Take = LiveDef->Candidates.FindByPredicate(
					[TakeNumber](const FMotionCandidate& C) { return C.TakeNumber == TakeNumber; });

				if (!SubmitResult.bSuccess)
				{
					if (Take)
					{
						Take->Status = EMotionJobStatus::Failed;
						Take->Error = SubmitResult.Error;
					}

					UE_LOG(LogMotionForge, Warning, TEXT("'%s' take %d: %s"),
						*LiveDef->GetName(), TakeNumber, *SubmitResult.Error);

					LiveDef->MarkPackageDirty();
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

				if (Take)
				{
					Take->JobId = SubmitResult.JobId;
				}
				LiveDef->MarkPackageDirty();
			});
	}
}

bool UMotionForgeSubsystem::Tick(float DeltaTime)
{
	// Once, on the first tick rather than in Initialize.
	//
	// The sweep saves assets, and saving runs the validation subsystem - which at subsystem-init time
	// has not registered its Blueprint validators yet and says so, once per asset, in a warning that
	// reads like a fault in the asset rather than in when it was touched.
	if (!bSweptStrandedDefinitions)
	{
		bSweptStrandedDefinitions = true;
		ReleaseStrandedDefinitions();
	}

	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();
	const double Now = FPlatformTime::Seconds();
	const double Interval = FMath::Max(1, Settings->PollIntervalSeconds);

	if (Now - LastPollTime < Interval)
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

			// Past the timeout is late, not failed. The provider may still finish the job - and on a
			// paid one, bill for it - so dropping it would throw away a take already paid for. It is
			// polled less often from here, and Cancel is how a person stops waiting.
			if (!Job.bLate && Now - Job.SubmittedAt > Settings->JobTimeoutSeconds)
			{
				Job.bLate = true;

				if (UMotionDef* Def = LoadDef(Job.DefinitionPath))
				{
					if (FMotionCandidate* Candidate = Def->Candidates.FindByPredicate(
						[&Job](const FMotionCandidate& C) { return C.JobId == Job.JobId; }))
					{
						Candidate->bLate = true;
					}
				}

				if (FMotionActivity* Activity = Activities.Find(Job.DefinitionPath))
				{
					Activity->bLate = true;
				}

				UE_LOG(LogMotionForge, Warning,
					TEXT("Job %s is past the %ds timeout. Still waiting, less often; cancel to stop."),
					*Job.JobId, Settings->JobTimeoutSeconds);
			}

			if (Job.bLate && Now - Job.LastPolledAt < Interval * 4.0)
			{
				continue;
			}

			TSharedPtr<IMotionProvider> Provider = FindProvider(Job.ProviderId);
			if (!Provider.IsValid())
			{
				// The provider's plugin went away mid-batch. Settle the take with the reason, so the
				// definition can finish rather than wait for a poll that can never come.
				Job.bSettled = true;

				if (UMotionDef* Def = LoadDef(Job.DefinitionPath))
				{
					if (FMotionCandidate* Candidate = Def->Candidates.FindByPredicate(
						[&Job](const FMotionCandidate& C) { return C.JobId == Job.JobId; }))
					{
						Candidate->Status = EMotionJobStatus::Failed;
						Candidate->Error = FString::Printf(
							TEXT("%s was unloaded while this was generating. The provider may still hold the take."),
							*Job.ProviderId.ToString());
					}
					OnDefinitionGenerated(Batch, Def);
				}
				continue;
			}

			Job.bPollInFlight = true;
			Job.LastPolledAt = Now;

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
					if (!LiveJob || LiveJob->bSettled)
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
					else if (JobResult.Status == EMotionJobStatus::Running)
					{
						// Worth showing: queued and running are different waits, and on a shared GPU the
						// difference explains the clock.
						if (UMotionDef* Def = LoadDef(LiveJob->DefinitionPath))
						{
							if (FMotionCandidate* Candidate = Def->Candidates.FindByPredicate(
								[&JobId](const FMotionCandidate& C) { return C.JobId == JobId; }))
							{
								Candidate->Status = EMotionJobStatus::Running;
							}
						}
					}
				});
		}

		const int32 PendingSubmits = [&Batch]()
		{
			int32 Sum = 0;
			for (const TPair<FString, int32>& Pending : Batch.PendingSubmits)
			{
				Sum += Pending.Value;
			}
			return Sum;
		}();

		if (Outstanding == 0 && PendingSubmits == 0 && Batch.PendingPrepares == 0)
		{
			// Every job settled, or nothing was ever submitted - either way nothing more will arrive.
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
	Candidate->bLate = false;

	if (JobResult.Status == EMotionJobStatus::Finished)
	{
		// The viewer link is for the character the take was made for, which is not necessarily the
		// definition's character by the time the take finishes.
		TSharedPtr<IMotionProvider> Provider = FindProvider(Job.ProviderId);
		const UMotionCharacter* Character = Cast<UMotionCharacter>(Candidate->Character.TryLoad());

		if (Provider.IsValid())
		{
			Candidate->ViewerUrl = Provider->MakeViewerUrl(
				Character ? Character->ProviderCharacterId : FString(), JobResult.MotionId);
		}

		UE_LOG(LogMotionForge, Log, TEXT("'%s' %s -> motion %s"),
			*Def->GetName(), *Candidate->GetLabel(), *JobResult.MotionId);
	}

	if (FMotionActivity* Activity = Activities.Find(Job.DefinitionPath))
	{
		++Activity->Done;
	}

	Def->MarkPackageDirty();
	OnDefinitionGenerated(Batch, Def);
}

void UMotionForgeSubsystem::OnDefinitionGenerated(FMotionBatch& Batch, UMotionDef* Def)
{
	// Only act once every take belonging to this definition has settled - submitted and answered.
	const FString DefPath = Def->GetPathName();

	if (const int32* Pending = Batch.PendingSubmits.Find(DefPath))
	{
		if (*Pending > 0)
		{
			return;
		}
	}

	for (const FMotionJobTracking& Job : Batch.Jobs)
	{
		if (Job.DefinitionPath == DefPath && !Job.bSettled)
		{
			return;
		}
	}

	// Cancelled already settled it.
	if (Def->ActiveBatchId != Batch.BatchId && !Def->IsBusy())
	{
		return;
	}

	ClearActivity(DefPath);

	// Takes from this run, told apart from older ones by the batch's jobs.
	TSet<FString> JobsInThisBatch;
	for (const FMotionJobTracking& Job : Batch.Jobs)
	{
		if (Job.DefinitionPath == DefPath)
		{
			JobsInThisBatch.Add(Job.JobId);
		}
	}

	const FMotionCandidate* Chosen = nullptr;
	int32 NewUsable = 0;
	for (const FMotionCandidate& Candidate : Def->Candidates)
	{
		if (Candidate.IsUsable() && JobsInThisBatch.Contains(Candidate.JobId))
		{
			++NewUsable;
			if (!Chosen)
			{
				Chosen = &Candidate;
			}
		}
	}

	if (NewUsable == 0)
	{
		// Say why, from the first failed take of this run, rather than a generic line.
		FString Why = TEXT("No take generated successfully.");
		for (int32 Index = Def->Candidates.Num() - 1; Index >= 0; --Index)
		{
			const FMotionCandidate& Candidate = Def->Candidates[Index];
			if (Candidate.Status == EMotionJobStatus::Failed && !Candidate.Error.IsEmpty())
			{
				Why = FString::Printf(TEXT("%s failed: %s"), *Candidate.GetLabel(), *Candidate.Error);
				break;
			}
		}

		// Older takes may well be usable, and are still there to choose.
		if (Def->CountUsableCandidates() > 0)
		{
			Def->SetStatus(EMotionDefStatus::AwaitingReview);
			Def->LastError = Why;
		}
		else
		{
			Def->SetStatus(EMotionDefStatus::Failed, Why);
		}
		SaveAsset(Def);
		return;
	}

	if (Batch.Mode == EMotionPipelineMode::HumanInTheLoop)
	{
		Def->SetStatus(EMotionDefStatus::AwaitingReview);
		SaveAsset(Def);

		UE_LOG(LogMotionForge, Log, TEXT("'%s' has %d new take(s) to review."), *Def->GetName(), NewUsable);
		return;
	}

	// Automatic: take the first usable take **from this batch** and carry on. Not the first usable one
	// on the asset - that would re-import the oldest take on every regeneration.
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
		UE_LOG(LogMotionForge, Warning, TEXT("'%s' has no take '%s'."), *Def->GetName(), *MotionId);
		return false;
	}

	Def->Modify();
	Def->SelectedMotionId = MotionId;
	Def->MarkPackageDirty();
	SaveAsset(Def);
	return true;
}

bool UMotionForgeSubsystem::ChooseAndImport(const FString& AssetPath, const FString& MotionId)
{
	if (!SelectCandidate(AssetPath, MotionId))
	{
		return false;
	}

	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def || Def->IsBusy())
	{
		return false;
	}

	ProcessSelected(Def);
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
			UE_LOG(LogMotionForge, Warning, TEXT("'%s' has no take chosen."), *Def->GetName());
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

bool UMotionForgeSubsystem::ResolveTakeRoute(
	UMotionDef* Def,
	const FMotionCandidate& Take,
	TSharedPtr<IMotionProvider>& OutProvider,
	UMotionCharacter*& OutCharacter,
	FString& OutError) const
{
	// The provider that made the take, whatever the definition uses now. Importing an Uthana take
	// through Kimodo because the definition moved on would read a file in the wrong format.
	const FName ProviderId = Take.ProviderId.IsNone() ? ResolveProviderId(Def->ProviderId) : Take.ProviderId;

	OutProvider = FindProvider(ProviderId);
	if (!OutProvider.IsValid())
	{
		OutError = FString::Printf(TEXT("%s was made by %s, which is not installed. Install it to import this take."),
			*Take.GetLabel(), *ProviderId.ToString());
		return false;
	}

	const FMotionProviderCaps Caps = OutProvider->GetCaps();

	UMotionCharacter* Current = Def->Character.LoadSynchronous();
	UMotionCharacter* Maker = Cast<UMotionCharacter>(Take.Character.TryLoad());

	// A provider that generates against an uploaded character returns motion on that character's rig,
	// so the take belongs with the character it was made for. One that generates on its own rig
	// returns motion any suitable character can take, so the definition's current one wins - which is
	// what lets a take made for the wrong character be imported after fixing the character.
	if (Caps.bSupportsCharacterUpload)
	{
		OutCharacter = Maker ? Maker : Current;
	}
	else
	{
		OutCharacter = (Current && DoesCharacterSuit(Current, ProviderId)) ? Current : Maker;
	}

	if (!OutCharacter)
	{
		OutError = TEXT("No Motion Character to import this take onto.");
		return false;
	}

	FString Why;
	if (!DoesCharacterSuit(OutCharacter, ProviderId, &Why))
	{
		OutError = Why;
		return false;
	}

	return true;
}

void UMotionForgeSubsystem::ProcessSelected(UMotionDef* Def)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();

	const FMotionCandidate* Selected = Def->FindSelectedCandidate();
	if (!Selected)
	{
		Def->SetStatus(EMotionDefStatus::Failed, TEXT("The chosen take is not among this definition's takes."));
		SaveAsset(Def);
		return;
	}

	if (!Selected->IsUsable())
	{
		Def->SetStatus(EMotionDefStatus::Failed, FString::Printf(TEXT("%s has not finished generating."), *Selected->GetLabel()));
		SaveAsset(Def);
		return;
	}

	const FString MotionId = Selected->MotionId;
	const FString Label = Selected->GetLabel();

	// Already on disk from an earlier run - skip straight to importing rather than paying again.
	const FString OnDisk = FindTakeFile(*Selected);
	if (!OnDisk.IsEmpty())
	{
		UE_LOG(LogMotionForge, Log, TEXT("'%s': reusing '%s'."), *Def->GetName(), *OnDisk);
		NormalizeAndImport(Def, MotionId, OnDisk);
		return;
	}

	TSharedPtr<IMotionProvider> Provider;
	UMotionCharacter* Character = nullptr;
	FString Error;
	if (!ResolveTakeRoute(Def, *Selected, Provider, Character, Error))
	{
		Def->SetStatus(EMotionDefStatus::Failed, Error);
		SaveAsset(Def);
		return;
	}

	// Not every provider hands back an FBX. Kimodo returns raw rotation matrices, and naming its
	// artifact .fbx would send it into an importer that cannot read it.
	const FString RawPath = Settings->GetAbsoluteStagingDirectory()
		/ FString::Printf(TEXT("%s_%s_raw.%s"),
			*Def->GetName(), *MotionId, *Provider->GetArtifactExtension());

	Def->SetStatus(EMotionDefStatus::Downloading);
	SetActivity(Def->GetPathName(), FString::Printf(TEXT("Downloading %s"), *Label), /*bCanCancel*/ false);
	SaveAsset(Def);

	const FString DefPath = Def->GetPathName();

	// The uploaded id of the character the take was made for, which is what the provider files it under.
	const UMotionCharacter* Maker = Cast<UMotionCharacter>(Selected->Character.TryLoad());
	const FString ProviderCharacterId = Maker ? Maker->ProviderCharacterId : Character->ProviderCharacterId;

	Provider->DownloadMotion(ProviderCharacterId, MotionId, RawPath, ResolveFrameRate(Provider),
		[this, DefPath, MotionId, RawPath](bool bSuccess, const FString& DownloadError)
		{
			UMotionDef* LiveDef = LoadDef(DefPath);
			if (!LiveDef)
			{
				return;
			}

			if (!bSuccess)
			{
				ClearActivity(DefPath);
				LiveDef->SetStatus(EMotionDefStatus::Failed, DownloadError);
				SaveAsset(LiveDef);
				return;
			}

			if (FMotionCandidate* Candidate = LiveDef->FindCandidateMutable(MotionId))
			{
				Candidate->bDownloaded = true;
				Candidate->LocalRawPath = RawPath;
			}

			NormalizeAndImport(LiveDef, MotionId, RawPath);
		});
}

void UMotionForgeSubsystem::NormalizeAndImport(UMotionDef* Def, const FString& MotionId, const FString& RawPath)
{
	const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();
	const FString DefPath = Def->GetPathName();

	const FMotionCandidate* Take = Def->FindCandidate(MotionId);
	if (!Take)
	{
		ClearActivity(DefPath);
		Def->SetStatus(EMotionDefStatus::Failed, TEXT("The take being imported is no longer on this definition."));
		SaveAsset(Def);
		return;
	}

	TSharedPtr<IMotionProvider> Provider;
	UMotionCharacter* Character = nullptr;
	FString RouteError;
	if (!ResolveTakeRoute(Def, *Take, Provider, Character, RouteError))
	{
		ClearActivity(DefPath);
		Def->SetStatus(EMotionDefStatus::Failed, RouteError);
		SaveAsset(Def);
		return;
	}

	const FMotionCandidate TakeCopy = *Take;

	Def->SetStatus(EMotionDefStatus::Processing);
	SetActivity(DefPath, FString::Printf(TEXT("Importing %s"), *TakeCopy.GetLabel()), /*bCanCancel*/ false);

	// Whether a take lands on the provider's rig and is retargeted, or straight on the game's skeleton,
	// is the character's choice: a Provider Mesh set means retarget.
	USkeletalMesh* ProviderMesh = Character->ProviderMesh.LoadSynchronous();
	const bool bRetargeting = ProviderMesh != nullptr;

	USkeleton* ImportSkeleton = bRetargeting
		? ProviderMesh->GetSkeleton()
		: Character->TargetSkeleton.LoadSynchronous();

	if (!ImportSkeleton)
	{
		ClearActivity(DefPath);
		Def->SetStatus(EMotionDefStatus::Failed, bRetargeting
			? FString::Printf(TEXT("'%s''s Provider Mesh '%s' has no skeleton, so there is nothing to build the take on."),
				*Character->GetDisplayName(), *ProviderMesh->GetName())
			: FString::Printf(TEXT("'%s' has no Target Skeleton, so there is nothing to build the take on."),
				*Character->GetDisplayName()));
		SaveAsset(Def);
		return;
	}

	UAnimSequence* Built = nullptr;
	bool bWasNormalized = false;

	// Providers whose output is not an FBX build the animation themselves. They know exactly what
	// their numbers mean; routing them through a file format and a Blender pass in between only adds
	// two more places for a coordinate convention to be silently lost.
	if (Provider->HandlesImport())
	{
		FMotionArtifactImport ArtifactRequest;
		ArtifactRequest.AbsoluteArtifactPath = RawPath;
		ArtifactRequest.DestinationPackagePath = bRetargeting ? Settings->GetSourceTakesPath() : Settings->GetTakesPath();
		ArtifactRequest.AssetName = bRetargeting
			? FString::Printf(TEXT("AS_%s_Source"), *Def->GetName())
			: FString::Printf(TEXT("AS_%s"), *Def->GetName());
		ArtifactRequest.TargetSkeleton = ImportSkeleton;
		ArtifactRequest.TrimWindow = Def->TrimWindow;
		ArtifactRequest.bZeroRootTranslation = Settings->bZeroRootTranslation;

		const FMotionArtifactResult ArtifactResult = Provider->ImportArtifact(ArtifactRequest);
		if (!ArtifactResult.bSuccess)
		{
			ClearActivity(DefPath);
			Def->SetStatus(EMotionDefStatus::Failed, ArtifactResult.Error);
			SaveAsset(Def);
			return;
		}

		Built = ArtifactResult.Sequence.LoadSynchronous();
	}
	else
	{
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
			ClearActivity(DefPath);
			Def->SetStatus(EMotionDefStatus::Failed, NormalizeResult.Error);
			SaveAsset(Def);
			return;
		}
		bWasNormalized = !NormalizeResult.bSkipped;

		FMotionImportRequest ImportRequest;
		ImportRequest.AbsoluteFbxPath = NormalizeResult.OutputPath;
		ImportRequest.DestinationPackagePath = bRetargeting ? Settings->GetSourceTakesPath() : Settings->GetTakesPath();

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
			ClearActivity(DefPath);
			Def->SetStatus(EMotionDefStatus::Failed, ImportResult.Error);
			SaveAsset(Def);
			return;
		}

		Built = ImportResult.Sequence.LoadSynchronous();
	}

	UAnimSequence* Final = Built;

	if (bRetargeting && Built)
	{
		FString RetargetError;
		UAnimSequence* Retargeted = RetargetToCharacterRig(Def, Character, Built, RetargetError);

		if (!Retargeted)
		{
			// The source clip is on disk and correct; only the conversion is missing. Say so rather
			// than implying it has to be generated again - on pay-as-you-go that would cost money.
			ClearActivity(DefPath);
			Def->SetStatus(EMotionDefStatus::Failed, FString::Printf(
				TEXT("%s The take was built on the provider rig as '%s'. Fix the retarget setup and press "
					 "Import again; nothing needs generating again."),
				*RetargetError, *Built->GetName()));
			SaveAsset(Def);
			return;
		}

		Final = Retargeted;
	}

	if (!Final)
	{
		ClearActivity(DefPath);
		Def->SetStatus(EMotionDefStatus::Failed, TEXT("The import produced no animation."));
		SaveAsset(Def);
		return;
	}

	FMotionProvenance::Stamp(Final, Def, Provider->GetProviderId().ToString(),
		TakeCopy.ModelId.IsEmpty() ? Provider->GetDefaultModelId() : TakeCopy.ModelId, MotionId,
		Provider->GetDisplayName(), Provider->GetCaps().bIsLocal,
		Provider->GetCaps().NativeFrameRate, bWasNormalized, bRetargeting, &TakeCopy);

	Def->ImportedSequence = Final;
	Def->ImportedMotionId = MotionId;
	ClearActivity(DefPath);
	Def->SetStatus(EMotionDefStatus::Ready);
	SaveAsset(Def);

	// The preview of this take has done its job; the real clip is what should be watched now.
	Previews.Remove(DefPath + TEXT("|") + MotionId);

	PlaceTakeOnPromptSequence(Def, Final);

	UE_LOG(LogMotionForge, Log, TEXT("'%s' is ready: %s from %s (%s%s)"),
		*Def->GetName(), *Final->GetPathName(), *TakeCopy.GetLabel(), *Provider->GetDisplayName(),
		bRetargeting ? TEXT(", retargeted from the provider rig") : TEXT(""));
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
					 "the game's skeleton."),
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
		OutError = TEXT("Retargeting needs the character's Preview Mesh: the mesh on the game's skeleton.");
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

			// The intermediate has done its job. Delete it unless somebody asked to keep it - it is
			// derivable twice over, from the cached file and from the recipe. Deleted only after the
			// retargeted result is safely saved, and only if the two are genuinely different assets.
			if (!Settings->bKeepSourceClips && Sequence != SourceSequence)
			{
				DeleteSourceClip(SourceSequence);
			}

			return Sequence;
		}
	}

	OutError = FString::Printf(
		TEXT("Retargeting '%s' produced no animation. The IK Retargeter's source and target rigs "
			 "most likely do not match the Provider Mesh and Preview Mesh."),
		*SourceSequence->GetName());
	return nullptr;
}

UAnimSequence* UMotionForgeSubsystem::RetargetForPreview(
	UMotionDef* Def,
	UMotionCharacter* Character,
	UAnimSequence* SourceSequence,
	FString& OutError)
{
	// The engine's batch retarget writes assets through the asset tools, which refuse any folder
	// outside a content root with a modal dialog - /Temp included - so a preview cannot use it. This
	// is its per-frame loop, the part that does the retargeting, into a clip nobody saves.
	UIKRetargeter* Retargeter = !Def->RetargeterOverride.IsNull()
		? Def->RetargeterOverride.LoadSynchronous()
		: Character->Retargeter.LoadSynchronous();
	USkeletalMesh* SourceMesh = Character->ProviderMesh.LoadSynchronous();
	USkeletalMesh* TargetMesh = Character->PreviewMesh.LoadSynchronous();

	if (!Retargeter || !SourceMesh || !TargetMesh || !TargetMesh->GetSkeleton() || !SourceSequence)
	{
		OutError = TEXT("The preview needs the character's Retargeter, Provider Mesh and Preview Mesh, as the import does.");
		return nullptr;
	}

	FRetargetProfile Profile;
	Profile.FillProfileWithAssetSettings(Retargeter);

	FRetargetInitParameters InitParams;
	InitParams.SourceSkeletalMesh = SourceMesh;
	InitParams.TargetSkeletalMesh = TargetMesh;
	InitParams.RetargeterAsset = Retargeter;
	InitParams.CustomProfile = &Profile;
	InitParams.bSuppressWarnings = true;

	FIKRetargetProcessor Processor;
	Processor.Initialize(InitParams);
	if (!Processor.IsInitialized())
	{
		OutError = FString::Printf(TEXT("The IK Retargeter %s could not start with %s and %s. Its rigs most likely do not match them."),
			*Retargeter->GetName(), *SourceMesh->GetName(), *TargetMesh->GetName());
		return nullptr;
	}

	const FRetargetSkeleton& Source = Processor.GetSkeleton(ERetargetSourceOrTarget::Source);
	const FRetargetSkeleton& Target = Processor.GetSkeleton(ERetargetSourceOrTarget::Target);
	const int32 NumSourceBones = Source.BoneNames.Num();
	const int32 NumTargetBones = Target.BoneNames.Num();
	const int32 NumFrames = SourceSequence->GetNumberOfSampledKeys();

	if (NumFrames <= 0 || NumTargetBones == 0)
	{
		OutError = TEXT("The clip has no frames to retarget.");
		return nullptr;
	}

	TArray<FRawAnimSequenceTrack> Tracks;
	Tracks.SetNum(NumTargetBones);
	for (FRawAnimSequenceTrack& Track : Tracks)
	{
		Track.PosKeys.SetNum(NumFrames);
		Track.RotKeys.SetNum(NumFrames);
		Track.ScaleKeys.SetNum(NumFrames);
	}

	FAnimPoseEvaluationOptions Evaluation;
	Evaluation.OptionalSkeletalMesh = SourceMesh;
	Evaluation.bExtractRootMotion = false;
	Evaluation.bIncorporateRootMotionIntoPose = true;

	TArray<FTransform> SourcePose;
	SourcePose.SetNum(NumSourceBones);

	Processor.OnPlaybackReset();

	for (int32 Frame = 0; Frame < NumFrames; ++Frame)
	{
		FAnimPose Pose;
		UAnimPoseExtensions::GetAnimPoseAtFrame(SourceSequence, Frame, Evaluation, Pose);

		for (int32 Bone = 0; Bone < NumSourceBones; ++Bone)
		{
			SourcePose[Bone] = UAnimPoseExtensions::GetBonePose(Pose, Source.BoneNames[Bone], EAnimPoseSpaces::World);
			SourcePose[Bone].SetScale3D(FVector::OneVector);
		}

		const float Time = SourceSequence->GetTimeAtFrame(Frame);
		const float DeltaTime = Frame > 0 ? Time - SourceSequence->GetTimeAtFrame(Frame - 1) : Time;

		Processor.ApplySourceScaleToPose(SourcePose);
		Processor.UpdateOpsFromAnimSequence(SourceSequence, Time);

		FRetargetRunParameters RunParams;
		RunParams.SourceGlobalPose = &SourcePose;
		RunParams.Profile = &Profile;
		RunParams.DeltaTime = DeltaTime;
		const TArray<FTransform>& TargetGlobal = Processor.RunRetargeter(RunParams);

		TArray<FTransform> TargetLocal = TargetGlobal;
		Target.UpdateLocalTransformsBelowBone(0, TargetLocal, TargetGlobal);

		for (int32 Bone = 0; Bone < NumTargetBones; ++Bone)
		{
			Tracks[Bone].PosKeys[Frame] = FVector3f(TargetLocal[Bone].GetLocation());
			Tracks[Bone].RotKeys[Frame] = FQuat4f(TargetLocal[Bone].GetRotation().GetNormalized());
			Tracks[Bone].ScaleKeys[Frame] = FVector3f(TargetLocal[Bone].GetScale3D());
		}
	}

	UAnimSequence* Clip = NewObject<UAnimSequence>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UAnimSequence::StaticClass(), FName(*(SourceSequence->GetName() + TEXT("_OnCharacter")))),
		RF_Transient);
	Clip->SetSkeleton(TargetMesh->GetSkeleton());

	IAnimationDataController& Controller = Clip->GetController();
	constexpr bool bShouldTransact = false;
	Controller.OpenBracket(FText::FromString(TEXT("Retargeting a preview")), bShouldTransact);
	Controller.InitializeModel();
	Controller.SetFrameRate(SourceSequence->GetSamplingFrameRate(), bShouldTransact);
	Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, NumFrames - 1)), bShouldTransact);

	for (int32 Bone = 0; Bone < NumTargetBones; ++Bone)
	{
		Controller.AddBoneCurve(Target.BoneNames[Bone], bShouldTransact);
		Controller.SetBoneTrackKeys(Target.BoneNames[Bone], Tracks[Bone].PosKeys, Tracks[Bone].RotKeys, Tracks[Bone].ScaleKeys, bShouldTransact);
	}

	Controller.NotifyPopulated();
	Controller.CloseBracket(bShouldTransact);

	return Clip;
}

void UMotionForgeSubsystem::DeleteSourceClip(UAnimSequence* SourceSequence)
{
	if (!SourceSequence)
	{
		return;
	}

	const FString Name = SourceSequence->GetName();

	// Force-deleted, because the retargeter leaves a reference behind: `RunBatchRetarget` records
	// the asset it came from, so an ordinary delete refuses on a live referencer.
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

	TSharedPtr<IMotionProvider> Provider = FindProvider(Character->ProviderId);
	if (!Provider.IsValid())
	{
		OnComplete(false, FString(), FString::Printf(TEXT("No provider registered as '%s'."),
			*ResolveProviderId(Character->ProviderId).ToString()));
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

// -------------------------------------------------------------------------------------------------
// Cancelling, hiding, and what is running
// -------------------------------------------------------------------------------------------------

void UMotionForgeSubsystem::SettleDefinition(UMotionDef* Def, const FString& Reason)
{
	if (Def == nullptr)
	{
		return;
	}

	// Takes still waiting are marked, not deleted. On a paid provider a submitted job may still finish
	// and bill, and its record is the only trace of that.
	for (FMotionCandidate& Candidate : Def->Candidates)
	{
		if (Candidate.Status == EMotionJobStatus::Pending || Candidate.Status == EMotionJobStatus::Running)
		{
			Candidate.Status = EMotionJobStatus::Failed;
			Candidate.Error = TEXT("Stopped waiting before it finished. The provider may still complete it.");
			Candidate.bLate = false;
		}
	}

	if (Def->CountUsableCandidates() > 0)
	{
		Def->SetStatus(EMotionDefStatus::AwaitingReview);
		Def->LastError = Reason;
	}
	else
	{
		Def->SetStatus(EMotionDefStatus::Failed, Reason);
	}

	Def->ActiveBatchId.Reset();
	ClearActivity(Def->GetPathName());
	SaveAsset(Def);
}

bool UMotionForgeSubsystem::CancelBatch(const FString& BatchId)
{
	FMotionBatch* Batch = Batches.Find(BatchId);
	if (!Batch)
	{
		return false;
	}

	Batch->bCancelled = true;

	// Every definition goes somewhere it can generate from again. Leaving them at Generating was a
	// wedge only an editor restart undid.
	for (const FString& Path : Batch->DefinitionPaths)
	{
		if (UMotionDef* Def = LoadDef(Path))
		{
			if (Def->IsBusy() && (Def->ActiveBatchId == BatchId || Def->ActiveBatchId.IsEmpty()))
			{
				SettleDefinition(Def, TEXT("Cancelled."));
			}
		}
	}

	UE_LOG(LogMotionForge, Log, TEXT("Batch %s cancelled. Submitted jobs may still finish on the provider."), *BatchId);
	return true;
}

bool UMotionForgeSubsystem::CancelDefinition(const FString& AssetPath)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def || !Def->IsBusy())
	{
		return false;
	}

	const FString DefPath = Def->GetPathName();

	// Its jobs stop being polled wherever they are; the rest of its batch carries on.
	for (TPair<FString, FMotionBatch>& Pair : Batches)
	{
		for (FMotionJobTracking& Job : Pair.Value.Jobs)
		{
			if (Job.DefinitionPath == DefPath)
			{
				Job.bSettled = true;
			}
		}
		Pair.Value.PendingSubmits.Remove(DefPath);
	}

	// Downloading and importing cannot be interrupted part way. Only the waiting can be.
	if (Def->Status == EMotionDefStatus::Generating)
	{
		SettleDefinition(Def, TEXT("Cancelled."));
		return true;
	}

	return false;
}

bool UMotionForgeSubsystem::HideTake(const FString& AssetPath, const FString& MotionId, bool bHidden)
{
	UMotionDef* Def = LoadDef(AssetPath);
	if (!Def)
	{
		return false;
	}

	FMotionCandidate* Take = Def->FindCandidateMutable(MotionId);
	if (!Take)
	{
		return false;
	}

	Def->Modify();
	Take->bHidden = bHidden;
	Def->MarkPackageDirty();
	SaveAsset(Def);
	return true;
}

void UMotionForgeSubsystem::SetActivity(const FString& AssetPath, const FString& Doing, bool bCanCancel)
{
	FMotionActivity& Activity = Activities.FindOrAdd(AssetPath);
	Activity.AssetPath = AssetPath;
	Activity.DefinitionName = FSoftObjectPath(AssetPath).GetAssetName();
	Activity.Doing = Doing;
	Activity.StartedAt = FDateTime::Now();
	Activity.bCanCancel = bCanCancel;
	Activity.bLate = false;
	Activity.Done = 0;
	Activity.Total = 0;
}

void UMotionForgeSubsystem::ClearActivity(const FString& AssetPath)
{
	Activities.Remove(AssetPath);
}

TArray<FMotionActivity> UMotionForgeSubsystem::GetActivities() const
{
	TArray<FMotionActivity> Out;
	Activities.GenerateValueArray(Out);
	Out.Sort([](const FMotionActivity& A, const FMotionActivity& B) { return A.StartedAt < B.StartedAt; });
	return Out;
}

// -------------------------------------------------------------------------------------------------
// Previews
// -------------------------------------------------------------------------------------------------

bool UMotionForgeSubsystem::CanPreviewTake(const FString& AssetPath, const FString& MotionId, FString& OutWhyNot) const
{
	const UMotionDef* Def = LoadDef(AssetPath);
	const FMotionCandidate* Take = Def ? Def->FindCandidate(MotionId) : nullptr;

	if (!Take)
	{
		OutWhyNot = TEXT("No such take.");
		return false;
	}

	if (!Take->IsUsable())
	{
		OutWhyNot = TEXT("It has not finished generating.");
		return false;
	}

	if (!FindTakeFile(*Take).IsEmpty())
	{
		return true;
	}

	TSharedPtr<IMotionProvider> Provider = FindProvider(Take->ProviderId.IsNone() ? Def->ProviderId : Take->ProviderId);
	if (!Provider.IsValid())
	{
		OutWhyNot = TEXT("The provider that made it is not installed.");
		return false;
	}

	const FMotionBilling Billing = Provider->GetBilling();
	if (!Billing.bFetchIsFree)
	{
		OutWhyNot = FString::Printf(
			TEXT("Fetching a take from %s bills your plan, so it is not fetched just to look. Watch it on %s's site for free, or import it."),
			*Provider->GetDisplayName(), *Provider->GetDisplayName());
		return false;
	}

	// Said now rather than after a connection times out. The file is not here, so only the provider
	// has it, and a stopped runner cannot hand anything over.
	const FMotionProviderCaps Caps = Provider->GetCaps();
	if (!Caps.SetupHint.IsEmpty())
	{
		const FString Surface = Provider->GetSetupSurfaceLabel().ToString();
		OutWhyNot = FString::Printf(TEXT("The file is not on disk, and %s is not ready to send it. %s%s"),
			*Provider->GetDisplayName(),
			Caps.PrepareLabel.IsEmpty() ? *Caps.SetupHint : *FString::Printf(TEXT("%s first."), *Caps.PrepareLabel),
			Surface.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" %s in the Generate card does it."), *Surface));
		return false;
	}

	return true;
}

UAnimSequence* UMotionForgeSubsystem::FindPreview(const FString& AssetPath, const FString& MotionId) const
{
	const TObjectPtr<UAnimSequence>* Found = Previews.Find(AssetPath + TEXT("|") + MotionId);
	return Found ? Found->Get() : nullptr;
}

FString UMotionForgeSubsystem::FindTakeFile(const FMotionCandidate& Take)
{
	if (!Take.bDownloaded || Take.LocalRawPath.IsEmpty())
	{
		return FString();
	}

	if (FPaths::FileExists(Take.LocalRawPath))
	{
		return Take.LocalRawPath;
	}

	const FString InStaging = UMotionForgeSettings::Get()->GetAbsoluteStagingDirectory() / FPaths::GetCleanFilename(Take.LocalRawPath);
	return FPaths::FileExists(InStaging) ? InStaging : FString();
}

void UMotionForgeSubsystem::PreviewTake(
	const FString& AssetPath,
	const FString& MotionId,
	TFunction<void(UAnimSequence*, const FString&)> OnReady)
{
	const FString Key = AssetPath + TEXT("|") + MotionId;

	if (UAnimSequence* Cached = FindPreview(AssetPath, MotionId))
	{
		OnReady(Cached, FString());
		return;
	}

	// Already being fetched for somebody else: wait for the same answer rather than fetching twice.
	if (TArray<TFunction<void(UAnimSequence*, const FString&)>>* Waiting = PreviewWaiters.Find(Key))
	{
		Waiting->Add(MoveTemp(OnReady));
		return;
	}

	FString WhyNot;
	if (!CanPreviewTake(AssetPath, MotionId, WhyNot))
	{
		OnReady(nullptr, WhyNot);
		return;
	}

	UMotionDef* Def = LoadDef(AssetPath);
	const FMotionCandidate* Take = Def ? Def->FindCandidate(MotionId) : nullptr;
	if (!Def || !Take)
	{
		OnReady(nullptr, TEXT("No such take."));
		return;
	}

	TSharedPtr<IMotionProvider> Provider;
	UMotionCharacter* Character = nullptr;
	FString Error;
	if (!ResolveTakeRoute(Def, *Take, Provider, Character, Error))
	{
		OnReady(nullptr, Error);
		return;
	}

	PreviewWaiters.Add(Key).Add(MoveTemp(OnReady));

	TWeakObjectPtr<UMotionForgeSubsystem> WeakThis(this);
	const TWeakObjectPtr<UMotionCharacter> WeakCharacter = Character;
	const FString Label = Take->GetLabel();
	const FString DefName = Def->GetName();
	const FVector2D Trim = Def->TrimWindow;

	// Build, on the game thread, once the file is on disk.
	const TWeakObjectPtr<UMotionDef> WeakDef = Def;
	auto Build = [WeakThis, WeakDef, Key, Provider, WeakCharacter, Label, DefName, Trim](const FString& RawPath)
	{
		UMotionForgeSubsystem* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}

		UAnimSequence* Clip = nullptr;
		FString BuildError;

		UMotionCharacter* Live = WeakCharacter.Get();
		UMotionDef* LiveDef = WeakDef.Get();
		USkeleton* Skeleton = Live ? Live->TargetSkeleton.LoadSynchronous() : nullptr;

		// The route the import takes, so what plays here is what choosing the take would put in the
		// game. A character with a provider rig gets the clip built on that rig and then retargeted;
		// building straight onto its own skeleton instead showed a different result from the one an
		// import produced - on a CC5 character, a torso leaning back that the import did not have.
		USkeletalMesh* ProviderMesh = Live ? Live->ProviderMesh.LoadSynchronous() : nullptr;
		const bool bRetargets = ProviderMesh && LiveDef
			&& (!Live->Retargeter.IsNull() || !LiveDef->RetargeterOverride.IsNull());
		USkeleton* BuildSkeleton = ProviderMesh && ProviderMesh->GetSkeleton() ? ProviderMesh->GetSkeleton() : Skeleton;

		// A unique name each time: previews are transient and never collide with a real clip, and a
		// second preview of another take must not overwrite the one somebody is comparing against.
		static int32 Counter = 0;
		const FString AssetName = ObjectTools::SanitizeObjectName(
			FString::Printf(TEXT("Preview_%s_%s_%d"), *DefName, *Label.Replace(TEXT(" "), TEXT("")), ++Counter));

		if (Skeleton == nullptr)
		{
			BuildError = TEXT("The character has no Target Skeleton to preview on.");
		}
		else if (Provider->HandlesImport())
		{
			FMotionArtifactImport Request;
			Request.AbsoluteArtifactPath = RawPath;
			Request.DestinationPackagePath = TEXT("/Temp/MotionForgePreview");
			Request.AssetName = AssetName;
			Request.TargetSkeleton = BuildSkeleton;
			Request.TrimWindow = Trim;
			Request.bZeroRootTranslation = false;
			Request.bTransient = true;

			const FMotionArtifactResult Result = Provider->ImportArtifact(Request);
			Clip = Result.bSuccess ? Result.Sequence.Get() : nullptr;
			BuildError = Result.Error;
		}
		else
		{
			// An FBX is imported into the transient root and never saved.
			FMotionImportRequest Request;
			Request.AbsoluteFbxPath = RawPath;
			Request.DestinationPackagePath = TEXT("/Temp/MotionForgePreview");
			Request.AssetName = AssetName;
			Request.TargetSkeleton = BuildSkeleton;
			Request.FrameRate = ResolveFrameRate(Provider);

			const FMotionImportResult Result = FMotionImporter::Import(Request);
			Clip = Result.bSuccess ? Result.Sequence.Get() : nullptr;
			BuildError = Result.Error;

			if (Clip)
			{
				Clip->ClearFlags(RF_Standalone | RF_Public);
				Clip->SetFlags(RF_Transient);
			}
		}

		// Then across, with the same retargeter the import uses, in memory: nothing written to the
		// project and nothing saved.
		if (Clip && bRetargets)
		{
			FString RetargetError;
			UAnimSequence* Moved = Self->RetargetForPreview(LiveDef, Live, Clip, RetargetError);
			Clip = Moved;
			if (!Moved)
			{
				BuildError = RetargetError;
			}
		}

		if (Clip)
		{
			Self->Previews.Add(Key, Clip);
		}

		TArray<TFunction<void(UAnimSequence*, const FString&)>> Waiters;
		Self->PreviewWaiters.RemoveAndCopyValue(Key, Waiters);

		for (TFunction<void(UAnimSequence*, const FString&)>& Waiter : Waiters)
		{
			Waiter(Clip, Clip ? FString() : (BuildError.IsEmpty() ? FString(TEXT("The preview could not be built.")) : BuildError));
		}
	};

	const FString OnDisk = FindTakeFile(*Take);
	if (!OnDisk.IsEmpty())
	{
		Build(OnDisk);
		return;
	}

	// Fetched into staging exactly where an import would put it, and recorded on the take - so
	// choosing it afterwards reuses the file instead of fetching again.
	const FString RawPath = UMotionForgeSettings::Get()->GetAbsoluteStagingDirectory()
		/ FString::Printf(TEXT("%s_%s_raw.%s"), *Def->GetName(), *MotionId, *Provider->GetArtifactExtension());

	const UMotionCharacter* Maker = Cast<UMotionCharacter>(Take->Character.TryLoad());
	const FString ProviderCharacterId = Maker ? Maker->ProviderCharacterId : Character->ProviderCharacterId;

	Provider->DownloadMotion(ProviderCharacterId, MotionId, RawPath, ResolveFrameRate(Provider),
		[WeakThis, AssetPath, MotionId, Key, RawPath, Build](bool bSuccess, const FString& DownloadError)
		{
			UMotionForgeSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			if (!bSuccess)
			{
				TArray<TFunction<void(UAnimSequence*, const FString&)>> Waiters;
				Self->PreviewWaiters.RemoveAndCopyValue(Key, Waiters);
				for (TFunction<void(UAnimSequence*, const FString&)>& Waiter : Waiters)
				{
					Waiter(nullptr, DownloadError);
				}
				return;
			}

			if (UMotionDef* LiveDef = Self->LoadDef(AssetPath))
			{
				if (FMotionCandidate* Candidate = LiveDef->FindCandidateMutable(MotionId))
				{
					Candidate->bDownloaded = true;
					Candidate->LocalRawPath = RawPath;
					LiveDef->MarkPackageDirty();
				}
			}

			Build(RawPath);
		});
}

TArray<FString> UMotionForgeSubsystem::GetClipUsers(const FString& AssetPath) const
{
	TArray<FString> Users;

	const UMotionDef* Def = LoadDef(AssetPath);
	if (!Def || Def->ImportedSequence.IsNull())
	{
		return Users;
	}

	const FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	TArray<FName> Referencers;
	Registry.Get().GetReferencers(FName(*Def->ImportedSequence.ToSoftObjectPath().GetLongPackageName()), Referencers);

	const FString Own = Def->GetOutermost()->GetName();
	for (const FName& Referencer : Referencers)
	{
		const FString Name = Referencer.ToString();

		// The definition itself, and its prompt timeline, are not users: they are where it came from.
		if (Name == Own || (!Def->Control.ConstraintSequence.IsNull()
			&& Name == Def->Control.ConstraintSequence.ToSoftObjectPath().GetLongPackageName()))
		{
			continue;
		}
		Users.Add(FPackageName::GetShortName(Name));
	}

	Users.Sort();
	return Users;
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

	const FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

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
		Entry.ProviderId = ResolveProviderId(Def->ProviderId);

		// Ready with nothing to show for it. The status records what the pipeline did and stays true
		// after the clip is deleted, so the registry is the only thing that knows.
		if (!Entry.ImportedSequencePath.IsEmpty())
		{
			Entry.bImportedSequenceMissing =
				!AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(Entry.ImportedSequencePath)).IsValid();
		}

		// The recipe the definition asks for now, so every take can say whether it still matches.
		FMotionResolvedRequest Current;
		ResolveInternal(Def, Current, nullptr, false);

		Entry.Takes.Reserve(Def->Candidates.Num());
		for (const FMotionCandidate& Candidate : Def->Candidates)
		{
			FMotionTakeInfo Take;
			Take.MotionId = Candidate.MotionId;
			Take.Variant = Candidate.VariantIndex;
			Take.Status = Candidate.Status;
			Take.ViewerUrl = Candidate.ViewerUrl;
			// Whether the file is there now, not whether it once was.
			Take.bDownloaded = !FindTakeFile(Candidate).IsEmpty();
			Take.Error = Candidate.Error;
			Take.Label = Candidate.GetLabel();
			Take.ProviderId = Candidate.ProviderId;
			Take.ModelId = Candidate.ModelId;
			Take.Seed = Candidate.Seed;
			Take.LengthSeconds = Candidate.LengthSeconds;
			Take.bHidden = Candidate.bHidden;
			Take.EstimatedCost = Candidate.EstimatedCost;
			Take.bInGame = !Def->ImportedSequence.IsNull() && !Entry.bImportedSequenceMissing
				&& !Candidate.MotionId.IsEmpty() && Candidate.MotionId == Def->ImportedMotionId;
			Take.bStale = !Candidate.RecipeHash.IsEmpty() && Candidate.RecipeHash != Current.RecipeHash;
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
		// outside. Report finished rather than implying failure, and flag that it is untracked.
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
	Status.bFinished = Batch->Jobs.Num() > 0 && Settled == Batch->Jobs.Num() && Batch->PendingPrepares == 0;
	Status.bCancelled = Batch->bCancelled;
	Status.DefinitionPaths = Batch->DefinitionPaths;
	return Status;
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

	// Fetching only, priced by the provider that made each take, in its own unit - a selection can
	// span a free local runner, a rented pod and a paid service at once. What generating again would
	// cost is a different question with its own answer: Preview Motion Request, or Estimate Generation
	// Cost. Folding it in here once priced a free pay-as-you-go download at the generation rate.
	struct FShare { FMotionBilling Billing; FString Name; float Downloaded = 0.f; };
	TMap<FName, FShare> Shares;

	for (const FString& Path : Paths)
	{
		UMotionDef* Def = LoadDef(Path);
		if (!Def)
		{
			continue;
		}

		auto AddDownload = [this, &Shares, &Estimate, Def](const FMotionCandidate& Take)
		{
			TSharedPtr<IMotionProvider> Maker = FindProvider(Take.ProviderId.IsNone() ? Def->ProviderId : Take.ProviderId);
			if (!Maker.IsValid())
			{
				return;
			}
			FShare& Share = Shares.FindOrAdd(Maker->GetProviderId());
			Share.Billing = Maker->GetBilling();
			Share.Name = Maker->GetDisplayName();
			Share.Downloaded += Take.LengthSeconds > 0.f ? FMath::CeilToFloat(Take.LengthSeconds) : Def->Length;
			++Estimate.Clips;
		};

		// A file already on disk costs nothing to use again, wherever its recorded path points.
		if (bSelectedOnly)
		{
			if (const FMotionCandidate* Selected = Def->FindSelectedCandidate())
			{
				if (FindTakeFile(*Selected).IsEmpty())
				{
					AddDownload(*Selected);
				}
			}
		}
		else
		{
			for (const FMotionCandidate& Take : Def->Candidates)
			{
				if (Take.IsUsable() && FindTakeFile(Take).IsEmpty())
				{
					AddDownload(Take);
				}
			}
		}
	}

	for (const TPair<FName, FShare>& Share : Shares)
	{
		AccumulateCost(Estimate, Share.Value.Billing, Share.Value.Name, /*Takes=*/0, /*Generated=*/0.f, Share.Value.Downloaded);
	}

	if (Estimate.Summary.IsEmpty())
	{
		Estimate.Summary = TEXT("Nothing to fetch: every take asked about is already on disk, so importing it costs nothing.");
	}

	return Estimate;
}

FMotionCostEstimate UMotionForgeSubsystem::EstimateGenerationCost(const TArray<FMotionDefSpec>& Specs) const
{
	FMotionCostEstimate Estimate;

	struct FShare { FMotionBilling Billing; FString Name; int32 Takes = 0; float Generated = 0.f; };
	TMap<FName, FShare> Shares;

	for (const FMotionDefSpec& Spec : Specs)
	{
		TSharedPtr<IMotionProvider> Provider = FindProvider(Spec.ProviderId);
		if (!Provider.IsValid())
		{
			continue;
		}

		// Clamp the way submission will, or the estimate understates a four-second floor.
		float Length = Spec.Length > 0 ? Spec.Length : 5;
		const FString ModelId = Spec.ModelId.IsEmpty() ? Provider->GetDefaultModelId() : Spec.ModelId;

		for (const FMotionModelInfo& Model : Provider->GetModels())
		{
			if (Model.Id == ModelId)
			{
				Length = FMath::Clamp(Length, Model.MinSeconds, Model.MaxSeconds);
			}
		}

		const int32 Takes = Spec.Variants > 0 ? Spec.Variants : 1;

		FShare& Share = Shares.FindOrAdd(Provider->GetProviderId());
		Share.Billing = Provider->GetBilling();
		Share.Name = Provider->GetDisplayName();
		Share.Takes += Takes;
		Share.Generated += FMath::RoundToInt(Length) * Takes;
	}

	for (const TPair<FName, FShare>& Share : Shares)
	{
		AccumulateCost(Estimate, Share.Value.Billing, Share.Value.Name, Share.Value.Takes, Share.Value.Generated, 0.f);
	}

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
			Object->SetStringField(TEXT("label"), Take.Label);
			Object->SetStringField(TEXT("motionId"), Take.MotionId);
			Object->SetNumberField(TEXT("variant"), Take.Variant);
			Object->SetStringField(TEXT("status"), MotionForgeJson::JobStatusToString(Take.Status));
			Object->SetStringField(TEXT("viewerUrl"), Take.ViewerUrl);
			Object->SetBoolField(TEXT("downloaded"), Take.bDownloaded);
			Object->SetStringField(TEXT("error"), Take.Error);
			Object->SetNumberField(TEXT("seed"), Take.Seed);
			Object->SetBoolField(TEXT("inGame"), Take.bInGame);
			Object->SetBoolField(TEXT("stale"), Take.bStale);
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
	Root->SetNumberField(TEXT("generatedSeconds"), Estimate.GeneratedSeconds);
	Root->SetNumberField(TEXT("estimatedCost"), Estimate.EstimatedCost);
	Root->SetStringField(TEXT("currency"), Estimate.Currency);
	Root->SetNumberField(TEXT("clips"), Estimate.Clips);
	Root->SetBoolField(TEXT("selectedOnly"), Estimate.bSelectedOnly);
	Root->SetStringField(TEXT("summary"), Estimate.Summary);
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

	const bool bStored = FMotionCredentialStore::Set(Provider->GetCredentialServiceName(), Secret);
	NotifyProviderStateChanged(Provider->GetProviderId());
	return bStored;
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
