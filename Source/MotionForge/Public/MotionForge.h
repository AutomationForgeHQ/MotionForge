#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

class IMotionProvider;

/** Filter the Output Log on "LogMotionForge" to follow submit, poll, download, normalise and import. */
MOTIONFORGE_API DECLARE_LOG_CATEGORY_EXTERN(LogMotionForge, Log, All);

/**
 * MotionForge's module, and the registry add-on plugins join.
 *
 * Providers live here rather than on the subsystem for one reason: module startup order. An add-on
 * loads and registers whenever its own loading phase says, which may be before or after the editor
 * builds the subsystem. Holding the list on the module means neither order loses a provider - the
 * subsystem reads whatever is registered when it comes up, and hears about anything that arrives
 * later through OnProvidersChanged.
 *
 * A shared pointer rather than an IModularFeature: the pipeline hands providers into callbacks that
 * outlive the call, and a raw pointer whose owning module unloaded mid-batch is a crash rather than
 * an error message.
 */
class MOTIONFORGE_API FMotionForgeModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * MotionForge's module, loading it if it has not started yet.
	 *
	 * **Use this from an add-on's StartupModule.** Two plugins in the same loading phase start in an
	 * order nobody controls, and a `.uplugin` dependency guarantees only that MotionForge is
	 * *enabled* - not that its module ran first. Merely looking the module up therefore works on
	 * some runs and returns null on others, and the failure is a provider that silently never
	 * registers. Loading on demand removes the ordering question entirely.
	 */
	static FMotionForgeModule* GetPtr();

	/**
	 * The module if it is already up, never loading it.
	 *
	 * For shutdown paths, where loading a module in order to tell it something is being torn down
	 * would be worse than doing nothing.
	 */
	static FMotionForgeModule* GetPtrIfLoaded();

	/**
	 * Make a provider available to the pipeline.
	 *
	 * Call from an add-on module's StartupModule. Registering the same id twice replaces the first,
	 * which makes hot reload survivable.
	 */
	void RegisterProvider(TSharedRef<IMotionProvider> Provider);

	/** Take a provider back out again. Call from ShutdownModule, or in-flight jobs will outlive it. */
	void UnregisterProvider(FName ProviderId);

	TSharedPtr<IMotionProvider> FindProvider(FName ProviderId) const;
	TArray<FName> GetProviderIds() const;

	DECLARE_MULTICAST_DELEGATE(FOnProvidersChanged);
	FOnProvidersChanged OnProvidersChanged;

private:

	TMap<FName, TSharedPtr<IMotionProvider>> Providers;
};
