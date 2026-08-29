#include "MotionForge.h"

#if WITH_FORGE_KEYS
#include "ForgeKeyRegistry.h"
#endif
#include "MotionCredentialStore.h"
#include "MotionForgeSettings.h"
#include "MotionForgeSubsystem.h"
#include "IMotionProvider.h"
#include "Providers/UthanaProvider.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogMotionForge);

#define LOCTEXT_NAMESPACE "FMotionForgeModule"

namespace MotionForgeConsole
{
	/**
	 * Signing in happens on the settings page, but acting on the result cannot.
	 *
	 * UFUNCTION(CallInEditor) buttons do not render on a UDeveloperSettings page: the details
	 * customization drops archetype objects before deciding whether to draw them (ObjectDetails.cpp,
	 * AddCallInEditorMethods) and a settings panel edits the CDO, which is one. Console commands are
	 * the cheapest surface that actually works, and they suit CI and headless runs besides.
	 */

	static FName ResolveProvider(const TArray<FString>& Args)
	{
		if (Args.Num() > 0 && !Args[0].IsEmpty())
		{
			return FName(*Args[0]);
		}

		const UMotionForgeSettings* Settings = UMotionForgeSettings::Get();
		return Settings->DefaultProviderId.IsNone() ? FName(TEXT("Uthana")) : Settings->DefaultProviderId;
	}

	static void TestConnection(const TArray<FString>& Args)
	{
		UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
		if (!Forge)
		{
			UE_LOG(LogMotionForge, Error, TEXT("MotionForge subsystem is not available."));
			return;
		}

		const FName Provider = ResolveProvider(Args);
		UE_LOG(LogMotionForge, Log, TEXT("Testing connection to '%s'..."), *Provider.ToString());

		// Asynchronous - the result arrives in this log a moment later, not from this call.
		Forge->TestConnection(Provider);
	}

	static void ShowCredentialStatus(const TArray<FString>& Args)
	{
		const FName Provider = ResolveProvider(Args);
		UE_LOG(LogMotionForge, Log, TEXT("%s: %s"),
			*Provider.ToString(),
			*FMotionCredentialStore::DescribeSource(Provider.ToString()));
	}

	static void ClearCredential(const TArray<FString>& Args)
	{
		const FName Provider = ResolveProvider(Args);
		const FString Service = Provider.ToString();

		UE_LOG(LogMotionForge, Log, TEXT("%s"),
			FMotionCredentialStore::Remove(Service)
				? TEXT("Removed the stored key.")
				: TEXT("Nothing was stored."));

		// An environment variable outranks the vault, so clearing the vault may change nothing. Say so
		// rather than letting the report contradict the action just taken.
		if (FMotionCredentialStore::Has(Service))
		{
			UE_LOG(LogMotionForge, Warning,
				TEXT("A key is still available for '%s' from %s - the environment variable takes priority."),
				*Service, *FMotionCredentialStore::GetEnvironmentVariableName(Service));
		}
	}

	static void ListCharacters(const TArray<FString>& Args)
	{
		UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
		if (!Forge)
		{
			UE_LOG(LogMotionForge, Error, TEXT("MotionForge subsystem is not available."));
			return;
		}

		Forge->ListProviderCharacters(ResolveProvider(Args),
			[](bool bSuccess, const TArray<FMotionRemoteCharacter>& Characters, const FString& Error)
			{
				if (!bSuccess)
				{
					UE_LOG(LogMotionForge, Error, TEXT("Could not list characters: %s"), *Error);
					return;
				}

				UE_LOG(LogMotionForge, Log, TEXT("%d character(s) on the provider:"), Characters.Num());
				for (const FMotionRemoteCharacter& Character : Characters)
				{
					UE_LOG(LogMotionForge, Log, TEXT("  %s  %s"), *Character.Id, *Character.Name);
				}
			});
	}

	static void UploadCharacter(const TArray<FString>& Args)
	{
		UMotionForgeSubsystem* Forge = UMotionForgeSubsystem::Get();
		if (!Forge)
		{
			UE_LOG(LogMotionForge, Error, TEXT("MotionForge subsystem is not available."));
			return;
		}

		if (Args.Num() == 0)
		{
			UE_LOG(LogMotionForge, Error,
				TEXT("Usage: MotionForge.UploadCharacter <MotionCharacter asset path> [force]"));
			return;
		}

		// Second argument rather than a flag, because forcing is rare and should read as deliberate.
		const bool bForce = Args.Num() > 1 && Args[1].Equals(TEXT("force"), ESearchCase::IgnoreCase);

		FMotionCharacterUploadOptions Options;
		Forge->UploadCharacter(Args[0], Options, bForce,
			[](bool bSuccess, const FMotionCharacterUpload& Result, const FString& Error)
			{
				if (!bSuccess)
				{
					UE_LOG(LogMotionForge, Error, TEXT("Upload failed: %s"), *Error);
					return;
				}

				UE_LOG(LogMotionForge, Log, TEXT("Paired as '%s' (%s), from %s"),
					*Result.ProviderCharacterId, *Result.Name, *Result.UploadedFile);

				// Zero means the provider recognised the rig and left it alone, which is what a mesh
				// exported from Unreal should produce. Anything else means it re-rigged, and motion
				// will come back on bones the project's skeleton may not have.
				if (Result.AutoRigConfidence > 0.f)
				{
					UE_LOG(LogMotionForge, Warning,
						TEXT("The provider auto-rigged this character (confidence %.2f). Motion will "
							 "come back on its guessed skeleton, not the one you exported."),
						Result.AutoRigConfidence);
				}
			});
	}

	static FAutoConsoleCommand ListCharactersCommand(
		TEXT("MotionForge.ListCharacters"),
		TEXT("List the characters a provider already holds. Optional argument: provider id."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ListCharacters));

	static FAutoConsoleCommand UploadCharacterCommand(
		TEXT("MotionForge.UploadCharacter"),
		TEXT("Export a Motion Character's mesh and upload it to its provider, writing the returned id "
			 "back into the asset. Usage: MotionForge.UploadCharacter <asset path> [force]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&UploadCharacter));

	static FAutoConsoleCommand TestConnectionCommand(
		TEXT("MotionForge.TestConnection"),
		TEXT("Make one cheap authenticated call to a provider. Optional argument: provider id, "
			 "defaulting to the one in settings. The result appears under LogMotionForge."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TestConnection));

	static FAutoConsoleCommand CredentialStatusCommand(
		TEXT("MotionForge.CredentialStatus"),
		TEXT("Report whether a provider has a usable API key, and where it is read from. Never prints "
			 "the key. Optional argument: provider id."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ShowCredentialStatus));

	static FAutoConsoleCommand ClearCredentialCommand(
		TEXT("MotionForge.ClearKey"),
		TEXT("Forget the stored key for a provider. Optional argument: provider id."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ClearCredential));
}

// -------------------------------------------------------------------------------------------------
// The provider registry
// -------------------------------------------------------------------------------------------------

FMotionForgeModule* FMotionForgeModule::GetPtr()
{
	// Load, do not merely fetch. An add-on registering from its own StartupModule may well get here
	// before MotionForge's module has started, and GetModulePtr would hand back null on exactly
	// those runs - producing a provider that never appears, intermittently, with no error.
	return FModuleManager::Get().LoadModulePtr<FMotionForgeModule>(TEXT("MotionForge"));
}

FMotionForgeModule* FMotionForgeModule::GetPtrIfLoaded()
{
	return FModuleManager::GetModulePtr<FMotionForgeModule>(TEXT("MotionForge"));
}

void FMotionForgeModule::RegisterProvider(TSharedRef<IMotionProvider> Provider)
{
	const FName Id = Provider->GetProviderId();

	// Replace rather than refuse. A hot reload re-runs StartupModule without ShutdownModule having
	// run first, and refusing here would leave the stale instance in place - which is the harder
	// failure to diagnose of the two.
	const bool bReplaced = Providers.Contains(Id);
	Providers.Add(Id, Provider);

	UE_LOG(LogMotionForge, Log, TEXT("Provider '%s' %s."),
		*Id.ToString(), bReplaced ? TEXT("re-registered") : TEXT("registered"));

	// Offer this provider's key to the shared Keys page — if ForgeKeys happens to be installed.
	// MotionForge does not link it and does not require it: without it this is a null check and the
	// key is still set on MotionForge's own settings page, which is the path that always works.
#if WITH_FORGE_KEYS
	if (IForgeKeysModule* Keys = IForgeKeysModule::GetOrLoad())
	{
		const FString Service = Provider->GetCredentialServiceName();
		const FString Display = Provider->GetDisplayName();

		FForgeKeyProvider Key;
		Key.Id = FName(*FString::Printf(TEXT("MotionForge.%s"), *Id.ToString()));
		Key.DisplayName = FText::FromString(Display);
		Key.Owner = LOCTEXT("MotionForgeOwner", "MotionForge");
		Key.Purpose = Provider->GetCredentialPurpose().IsEmpty()
			? FText::Format(
				LOCTEXT("MotionKeyPurpose", "Motion generation through {0}. Without it this provider cannot be used."),
				FText::FromString(Display))
			: Provider->GetCredentialPurpose();
		Key.bOptional = Provider->IsCredentialOptional();
		Key.HelpUrl = Provider->GetCredentialHelpUrl();
		Key.VaultEntryName = FString::Printf(TEXT("MotionForge/%s"), *Service);
		Key.EnvironmentVariableName = FMotionCredentialStore::GetEnvironmentVariableName(Service);

		// The operations stay here, on MotionForge's own store. ForgeKeys holds no vault code.
		Key.IsSet    = [Service]() { return FMotionCredentialStore::Has(Service); };
		Key.Describe = [Service]() { return FMotionCredentialStore::DescribeSource(Service); };
		Key.Store    = [Service](const FString& Secret) { return FMotionCredentialStore::Set(Service, Secret); };
		Key.Clear    = [Service]() { return FMotionCredentialStore::Remove(Service); };

		// Weak, so a provider whose plugin unloaded mid-test cannot be called through a dangling handle.
		TWeakPtr<IMotionProvider> WeakProvider = Provider.ToSharedPtr();
		Key.Test = [WeakProvider](FForgeKeyTestResult Done)
		{
			if (TSharedPtr<IMotionProvider> Pinned = WeakProvider.Pin())
			{
				Pinned->TestConnection([Done](bool bSuccess, const FString& Message)
				{
					Done(bSuccess, FText::FromString(Message));
				});
			}
			else
			{
				Done(false, LOCTEXT("ProviderGone", "That provider is no longer loaded."));
			}
		};
		Keys->Registry().Register(MoveTemp(Key));
	}
#endif

	OnProvidersChanged.Broadcast();
}

void FMotionForgeModule::UnregisterProvider(FName ProviderId)
{
	if (Providers.Remove(ProviderId) > 0)
	{
		UE_LOG(LogMotionForge, Log, TEXT("Provider '%s' unregistered."), *ProviderId.ToString());
#if WITH_FORGE_KEYS
		if (IForgeKeysModule* Keys = IForgeKeysModule::GetIfLoaded())
		{
			Keys->Registry().Unregister(FName(*FString::Printf(TEXT("MotionForge.%s"), *ProviderId.ToString())));
		}
#endif
		OnProvidersChanged.Broadcast();
	}
}

TSharedPtr<IMotionProvider> FMotionForgeModule::FindProvider(FName ProviderId) const
{
	const TSharedPtr<IMotionProvider>* Found = Providers.Find(ProviderId);
	return Found ? *Found : nullptr;
}

TArray<FName> FMotionForgeModule::GetProviderIds() const
{
	TArray<FName> Ids;
	Providers.GetKeys(Ids);
	Ids.Sort(FNameLexicalLess());
	return Ids;
}

void FMotionForgeModule::StartupModule()
{
	// Uthana goes through the same registry an add-on uses. Keeping the first-party provider on a
	// private path would let the registry rot untested until the day someone needed it.
	RegisterProvider(MakeShared<FUthanaProvider>());
}

void FMotionForgeModule::ShutdownModule()
{
	Providers.Empty();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMotionForgeModule, MotionForge)
