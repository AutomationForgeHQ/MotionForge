#include "MotionForgeEditorSettings.h"

#include "MotionCredentialStore.h"
#include "MotionForge.h"

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
	const FString Service = CredentialProviderId.IsNone() ? TEXT("Uthana") : CredentialProviderId.ToString();
	CredentialStatus = FMotionCredentialStore::DescribeSource(Service);
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
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UMotionForgeEditorSettings, CredentialProviderId))
	{
		RefreshStatus();
	}
}

#endif
