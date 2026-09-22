// A provider's own settings, as typed properties a person can see and an agent can read.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MotionForgeTypes.h"
#include "MotionPipeline.generated.h"

struct FMotionSubmitRequest;

// Why a class per provider rather than a list of options with a string map. A string map draws no
// slider, no dropdown, no greyed-out field and no tooltip, which is exactly what a person choosing
// between Kimodo's guidance settings needs. MeshForge learned it first: its flat option list survived
// only as what agents read, and the typed layer is what people use. This is the same split.
//
// The class lives in the provider's plugin and uses the vendor's field names on screen. The core
// never names a vendor: it asks each provider for its pipeline class, draws whatever it gets, and
// calls Apply before submitting. A third provider gets a correct window without the core changing.

/**
 * The settings one motion provider offers, stored on a Motion Definition.
 *
 * A definition keeps one instance per provider it has used, so switching Kimodo, then Uthana, then
 * Kimodo again gives the Kimodo settings back as they were.
 *
 * Subclass rules, copied from MeshForge's pipelines because they are what kept those usable:
 *
 * - Properties are named and displayed as the vendor names them (`diffusion_steps`, not "Quality
 *   Steps"), with `meta = (WireName = "...")` giving the key agents use. Our knowledge goes in the
 *   tooltip: what it costs, what it did when measured, when it is a trap.
 * - `AdvancedDisplay` puts a setting under Advanced in the window.
 * - Every setting a combination can make meaningless also has a Validate sentence, because a greyed-out
 *   field is invisible to an agent and to anything submitted without a window.
 * - Apply writes the settings into the request. Without it a pipeline is decoration.
 */
UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced, CollapseCategories,
	meta = (DisplayName = "Motion Pipeline"))
class MOTIONFORGE_API UMotionPipeline : public UObject
{
	GENERATED_BODY()

public:

	/** The provider that reads these settings. */
	virtual FName GetProviderId() const PURE_VIRTUAL(UMotionPipeline::GetProviderId, return NAME_None;);

	/**
	 * Called once, when a definition first uses this provider and gets its own copy of these settings.
	 * The place to apply project defaults - never the constructor, which also builds the class default
	 * before project settings are reliably loaded.
	 */
	virtual void OnCreated() {}

	/** The model this pipeline asks for. Empty means the provider's default. */
	virtual FString GetModelId() const { return FString(); }

	/**
	 * Choose a model. Refused, with the reason, when the provider does not offer it - a model id is
	 * a provider's private vocabulary, and storing another provider's name fails at the far end.
	 */
	virtual bool SetModelId(const FString& ModelId, FString& OutError);

	/**
	 * Write these settings into the request the provider will read.
	 *
	 * Called once per definition, before the request is copied per take - so anything expensive
	 * belongs in the provider's PrepareRequest, not here.
	 */
	virtual void Apply(FMotionSubmitRequest& Request) const {}

	/**
	 * What is wrong with the request these settings would make, and what is merely worth knowing.
	 *
	 * Problems stop the generation, each one sentence naming the fault and the way out. Warnings are
	 * shown and do not stop anything.
	 */
	virtual void Validate(
		const FMotionSubmitRequest& Request,
		TArray<FString>& OutProblems,
		TArray<FString>& OutWarnings) const {}

	/** The settings as they will be sent, "key=value", for the record and the "will be sent" block. */
	virtual TArray<FString> DescribeSent(const FMotionSubmitRequest& Request) const;

	/**
	 * Take over the values a definition carried before providers declared their own settings.
	 *
	 * Called once when an older definition loads. Only values that differ from the old defaults are
	 * meaningful, and a model the provider does not offer is dropped with a log line rather than kept.
	 */
	virtual void MigrateLegacy(const FString& LegacyModelId, bool bLegacyRewritePrompt, const FMotionControl& LegacyControl) {}

	/**
	 * A stable string of everything this pipeline would change about a take.
	 *
	 * Reflected rather than hand-written, so a subclass cannot gain a setting and forget it here.
	 */
	FString Signature() const;

	/**
	 * These settings as the flat list agents read, with their current values.
	 *
	 * From the same reflected properties the window draws, so the two surfaces cannot disagree.
	 */
	TArray<FMotionPipelineOption> DescribeOptions() const;

	/**
	 * Set one setting by the key Describe Options reports, from text.
	 *
	 * @return false with OutError naming the fault - an unknown key lists the ones that exist.
	 */
	bool SetOption(const FString& Key, const FString& Value, FString& OutError);

	/** The property behind a key, by wire name or by property name. Null when there is none. */
	FProperty* FindOptionProperty(const FString& Key) const;

	/** A key for a property: its WireName when it declares one, its own name otherwise. */
	static FString GetOptionKey(const FProperty* Property);

	/** Whether a property is one of the subclass's settings rather than bookkeeping. */
	static bool IsSettingProperty(const FProperty* Property);
};
