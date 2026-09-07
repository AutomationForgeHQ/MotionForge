// Editor Preferences > Automation Forge > MotionForge.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MotionForgeEditorSettings.generated.h"

/**
 * Signing in to a motion provider — one person's business, not the project's.
 *
 * This is the tier the credential fields always belonged in and were not: a key is per person and
 * per machine, so it has no place on a Project Settings page whose values are written to a committed
 * `DefaultEditor.ini`. Nothing here is `config` either — the field is `Transient`, hands its value
 * straight to the OS credential vault and blanks itself, and what persists is the vault entry,
 * outside the project directory where it cannot be copied, committed or zipped along with it.
 *
 * ForgeKeys, if installed, shows the same keys for every plugin on one page. This page keeps working
 * when it is not, which is the point of it.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "MotionForge"))
class MOTIONFORGE_API UMotionForgeEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	UMotionForgeEditorSettings();

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Automation Forge"); }

	static UMotionForgeEditorSettings* Get();

#if WITH_EDITOR
	virtual void PostInitProperties() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Which provider the fields below act on. */
	UPROPERTY(Transient, EditAnywhere, Category = "Credentials")
	FName CredentialProviderId = NAME_None;

	/**
	 * Paste an API key here to sign in.
	 *
	 * Stored in the OS credential vault the moment you commit the field, which is then cleared. The
	 * value is never saved to a config file and cannot be read back out through this panel.
	 */
	UPROPERTY(Transient, EditAnywhere, Category = "Credentials",
		meta = (PasswordField = true, DisplayName = "API Key"))
	FString ApiKeyEntry;

	// A note for us rather than for the reader of the tooltip: this page cannot carry buttons.
	// UFUNCTION(CallInEditor) does not render on a UDeveloperSettings page — the details
	// customization discards archetype objects before drawing them, and a settings panel edits the
	// CDO, which is one. Clearing and testing live on the Keys page, and on console commands.

	/**
	 * Whether a key is available, and where it is coming from.
	 *
	 * To clear or test one, open Tools ▸ Automation Forge ▸ Keys.
	 */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Credentials", meta = (DisplayName = "Status"))
	FString CredentialStatus;

	/** Re-read CredentialStatus. Called on load and whenever the fields above change. */
	void RefreshStatus();
};
