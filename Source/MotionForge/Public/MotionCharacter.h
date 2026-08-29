// Who the motion is generated for.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MotionCharacter.generated.h"

class USkeleton;
class USkeletalMesh;
class UIKRetargeter;
class UControlRig;

/**
 * A character the provider can generate motion against, paired with the skeleton results import onto.
 *
 * Providers retarget on download when given a character id, so choosing the right character here is
 * what removes any need to retarget inside the engine afterwards - the file arrives already on the
 * right skeleton.
 *
 * Adding a character later is a new asset, never a code change.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Motion Character"))
class MOTIONFORGE_API UMotionCharacter : public UDataAsset
{
	GENERATED_BODY()

public:

	/** Shown in pickers and logs. Defaults to the asset name when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character")
	FString DisplayName;

	/**
	 * The provider's own id for this character.
	 *
	 * Upload the character to the provider once - by hand for now - and paste the id here. Leaving it
	 * empty means the provider generates against its own default character, which will import onto a
	 * skeleton that probably is not yours.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character")
	FString ProviderCharacterId;

	/** Which provider the id above belongs to. Ids are not portable between providers. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character")
	FName ProviderId;

	/**
	 * The skeleton finished animations end up on - the one the game actually uses.
	 *
	 * Clips do not import directly onto this. Providers do not return the rig they were given:
	 * Uthana strips this project's root and root1 and hands back a flatter hierarchy, so importing
	 * straight onto this skeleton binds animation to bones whose parents differ from the ones it was
	 * authored against, and the result is subtly and unfixably twisted. Import lands on
	 * ProviderMesh's skeleton instead and is retargeted here.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character")
	TSoftObjectPtr<USkeleton> TargetSkeleton;

	// ---------------------------------------------------------------------------------------------
	// Retargeting - how the provider's rig becomes ours
	// ---------------------------------------------------------------------------------------------

	/**
	 * The character exactly as the provider stores it, downloaded back from them.
	 *
	 * This is what clips import onto, because its bone names and hierarchy match the files they
	 * send byte for byte - no reconstruction, no guessing at stripped bones. Populate it with
	 * Import Provider Character rather than by hand.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character|Retargeting")
	TSoftObjectPtr<USkeletalMesh> ProviderMesh;

	/**
	 * Maps ProviderMesh's rig onto PreviewMesh's.
	 *
	 * Author it once per character in the IK Retargeter editor, with ProviderMesh as source and
	 * PreviewMesh as target. Every clip generated for this character then retargets automatically.
	 *
	 * Leave empty to stop after the provider-rig import and retarget by hand.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character|Retargeting")
	TSoftObjectPtr<UIKRetargeter> Retargeter;

	/**
	 * The mesh that represents this character, on TargetSkeleton.
	 *
	 * Used for previewing results, and exported to FBX when uploading the character to a provider -
	 * so it is what defines the rig the provider retargets onto. Optional only until you need to
	 * upload.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character")
	TSoftObjectPtr<USkeletalMesh> PreviewMesh;

	/**
	 * The Control Rig to pose this character with, for authoring constraints on a timeline.
	 *
	 * Put in a sequence built by Create Prompt Sequence, **disabled and with no keys** - so the
	 * generated take plays underneath as a preview and the rig sits there waiting. Key a pose on it and
	 * that moment becomes a constraint; leave it alone and it costs nothing.
	 *
	 * A property of the character rather than a setting, because a project with two skeletons has two
	 * rigs and nothing else can say which. Narrative Pro ships `CR_Mannequin_Body` for its mannequin,
	 * at `/NarrativePro/Pro/Core/Character/Biped/Art/Mannequin/Rig/`.
	 *
	 * Empty is fine and means no rig track is added. The beats still work; only posing is unavailable.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character")
	TSoftClassPtr<UControlRig> ControlRig;

	/**
	 * The file uploaded to the provider, recorded so the pairing can be reproduced later.
	 *
	 * Written automatically by ExportCharacterFbx. Nothing reads it - it exists so that "which mesh
	 * is this ProviderCharacterId actually of?" has an answer a year from now.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Character|Provenance")
	FString SourceFbxPath;

	/** DisplayName, or the asset name when that is empty. */
	UFUNCTION(BlueprintPure, Category = "Character")
	FString GetDisplayName() const;

	/**
	 * True when this character has the fields every provider needs.
	 *
	 * Does not check ProviderCharacterId - see IsUsableForProvider.
	 */
	UFUNCTION(BlueprintPure, Category = "Character")
	bool IsUsable(FString& OutReason) const;

	/**
	 * True when this character can be generated against by a particular provider.
	 *
	 * The pairing requirement is a fact about the provider, not the character. A service that
	 * retargets server-side needs a character uploaded to it and an id pasted back; a model that
	 * generates on its own fixed rig has neither, and demanding one makes the second kind of
	 * provider unusable for a reason that has nothing to do with it.
	 *
	 * @param bProviderNeedsCharacterId Take this from FMotionProviderCaps::bSupportsCharacterUpload
	 *        rather than deciding it here.
	 */
	UFUNCTION(BlueprintPure, Category = "Character")
	bool IsUsableForProvider(bool bProviderNeedsCharacterId, FString& OutReason) const;
};
