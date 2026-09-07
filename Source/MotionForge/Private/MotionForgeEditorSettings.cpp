#include "MotionForgeEditorSettings.h"

#include "MotionCredentialStore.h"
#include "MotionForge.h"
#include "IMotionProvider.h"

namespace
{
	/**
	 * The credential service the page's fields act on.
	 *
	 * An empty provider field means "whichever provider is the default" - the same resolution
	 * generation uses, so this page and a batch never disagree about whose key it is.
	 */
	FString ResolveService(FName ProviderId, bool& bOutNoProviders)
	{
		bOutNoProviders = false;
		FString Service = ProviderId.ToString();

		if (FMotionForgeModule* Module = FMotionForgeModule::GetPtrIfLoaded())
		{
			if (ProviderId.IsNone())
			{
				ProviderId = Module->ResolveDefaultProviderId();
				Service = ProviderId.ToString();
			}

			if (TSharedPtr<IMotionProvider> Provider = Module->FindProvider(ProviderId))
			{
				Service = Provider->GetCredentialServiceName();
			}
			else if (Module->GetProviderIds().Num() == 0)
			{
				bOutNoProviders = true;
			}
		}
		return Service;
	}
}

UMotionForgeEditorSettings::UMotionForgeEditorSettings()
{
	CategoryName = TEXT("Automation Forge");
	SectionName = TEXT("MotionForge");
}

UMotionForgeEditorSettings* UMotionForgeEditorSettings::Get()
{
	return GetMutableDefault<UMotionForgeEditorSettings>();
}

void UMotionForgeEditorSettings::RefreshStatus()
{
	bool bNoProviders = false;
	const FString Service = ResolveService(CredentialProviderId, bNoProviders);
	CredentialStatus = bNoProviders
		? TEXT("No providers are registered. Enable a provider plugin first.")
		: FMotionCredentialStore::DescribeSource(Service);
}

#if WITH_EDITOR

void UMotionForgeEditorSettings::PostInitProperties()
{
	Super::PostInitProperties();
	RefreshStatus();
}

void UMotionForgeEditorSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UMotionForgeEditorSettings, ApiKeyEntry))
	{
		if (!ApiKeyEntry.IsEmpty())
		{
			bool bNoProviders = false;
			const FString Service = ResolveService(CredentialProviderId, bNoProviders);

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
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UMotionForgeEditorSettings, CredentialProviderId))
	{
		RefreshStatus();
	}
}

#endif
