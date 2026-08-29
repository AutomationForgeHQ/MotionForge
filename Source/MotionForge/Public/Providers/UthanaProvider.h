// Uthana - GraphQL text-to-motion.

#pragma once

#include "CoreMinimal.h"
#include "IMotionProvider.h"
#include "Interfaces/IHttpRequest.h"

/**
 * Talks to Uthana's GraphQL API at https://uthana.com/graphql.
 *
 * Two model families behave differently and the difference matters:
 *
 *   text-to-motion-2.0  synchronous, 0.25-10s, exposes seed/cfg_scale/steps
 *   text-to-motion-3.0  asynchronous, 4-10s integer, no seed, higher quality
 *
 * Only the async shape is implemented, because v3.0 is the one worth generating with. The absence of
 * a seed on v3.0 is the reason candidates are never discarded anywhere in this plugin: a take that is
 * not kept can never be produced again, only re-rolled into something different.
 */
class MOTIONFORGE_API FUthanaProvider : public IMotionProvider
{
public:

	static const FName ProviderId;

	virtual FName GetProviderId() const override { return ProviderId; }
	virtual FString GetDisplayName() const override { return TEXT("Uthana"); }
	virtual FString GetCredentialServiceName() const override { return TEXT("Uthana"); }
	virtual FString GetDefaultModelId() const override { return TEXT("text-to-motion-3.0"); }

	virtual FMotionProviderCaps GetCaps() const override;

	virtual void GetLengthRange(const FString& ModelId, int32& OutMin, int32& OutMax) const override;
	virtual FString MakeViewerUrl(const FString& ProviderCharacterId, const FString& MotionId) const override;
	virtual bool HasCredential() const override;

	virtual void SubmitJob(const FMotionSubmitRequest& Request, FOnMotionSubmitComplete OnComplete) override;
	virtual void PollJob(const FString& JobId, FOnMotionJobPolled OnComplete) override;
	virtual void DownloadMotion(
		const FString& ProviderCharacterId,
		const FString& MotionId,
		const FString& AbsoluteLocalPath,
		int32 FrameRate,
		FOnMotionDownloadComplete OnComplete) override;
	virtual void TestConnection(FOnMotionTestComplete OnComplete) override;

	virtual bool SupportsCharacterManagement() const override { return true; }
	virtual void UploadCharacter(
		const FString& AbsoluteFilePath,
		const FString& Name,
		const FMotionCharacterUploadOptions& Options,
		FOnMotionCharacterUploaded OnComplete) override;
	virtual void ListCharacters(FOnMotionCharactersListed OnComplete) override;
	virtual void DownloadCharacterFile(
		const FString& CharacterId,
		const FString& AbsoluteLocalPath,
		FOnMotionDownloadComplete OnComplete) override;

private:

	/** A POST to the GraphQL endpoint with auth attached, but no body. Null when no credential. */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> MakeAuthenticatedRequest() const;

	/** Build a POST to the GraphQL endpoint with auth attached. Null when no credential is set. */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> MakeGraphQLRequest(
		const FString& Query,
		const TSharedPtr<class FJsonObject>& Variables) const;

	/**
	 * Pull the first "errors" message out of a GraphQL envelope.
	 *
	 * GraphQL answers 200 with an errors array rather than an HTTP error code, so a response that
	 * looks successful at the transport layer still has to be inspected.
	 */
	static bool ExtractGraphQLError(const TSharedPtr<class FJsonObject>& Root, FString& OutError);
};
