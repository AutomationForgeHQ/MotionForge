#include "Providers/UthanaProvider.h"

#include "MotionForge.h"
#include "MotionCredentialStore.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

const FName FUthanaProvider::ProviderId = TEXT("Uthana");

namespace UthanaApi
{
	static const TCHAR* Endpoint = TEXT("https://uthana.com/graphql");
	static const TCHAR* FileHost = TEXT("https://uthana.com/motion/file/motion_viewer");

	/** Where a character comes back from, mesh and rig together. */
	static const TCHAR* CharacterBundleHost = TEXT("https://uthana.com/motion/bundle");

	/**
	 * Submit. The documented mutation for the async v3.0 family.
	 *
	 * Kept as a literal rather than assembled, so that when the schema moves - and a young API's
	 * schema will move - the fix is one visible string.
	 */
	/*
	 * Argument types are taken from the schema reference at /docs/api/graphql:
	 *
	 *   prompt String!   model String!   character_id String   length Float   rewrite_prompt Boolean
	 *
	 * length is Float even though the docs describe it as "an integer between 4-10" - a variable
	 * declared Int is rejected in a Float position, because a variable's declared type has to match
	 * rather than merely coerce.
	 *
	 * The mutation returns a wrapper, not the job: the id lives at create_text_to_motion_job.job.id.
	 */
	static const TCHAR* SubmitMutation = TEXT(
		"mutation Submit($prompt: String!, $model: String!, $character_id: String, "
		"$length: Float, $rewrite_prompt: Boolean) {"
		"  create_text_to_motion_job("
		"    prompt: $prompt, model: $model, character_id: $character_id, "
		"    length: $length, rewrite_prompt: $rewrite_prompt"
		"  ) { job { id status } }"
		"}");

	/**
	 * Poll.
	 *
	 * The argument is job_id. Uthana's own docs contradict each other here - the schema reference
	 * page writes job(id:) while the text-to-motion guide's working examples write job(job_id:) -
	 * and the working example is the one that matches the server.
	 *
	 * `result` is an Object scalar, so it takes no selection set; navigate into it after parsing.
	 */
	static const TCHAR* JobQuery = TEXT(
		"query Job($job_id: String!) {"
		"  job(job_id: $job_id) { id status result }"
		"}");

	/** Cheapest authenticated call available, for Test Connection. */
	static const TCHAR* PingQuery = TEXT("query { __typename }");

	/**
	 * Upload a character. Multipart, because `file` is a GraphQL Upload scalar.
	 *
	 * auto_rig only fires when the file arrives without a skeleton, so it stays on as a safety net
	 * even though everything this plugin sends is already rigged.
	 */
	static const TCHAR* CreateCharacterMutation = TEXT(
		"mutation Create($file: Upload!, $name: String!, $auto_rig: Boolean, "
		"$auto_rig_front_facing: Boolean, $include_fingers: Boolean, $rerig_target: String) {"
		"  create_character("
		"    file: $file, name: $name, auto_rig: $auto_rig, "
		"    auto_rig_front_facing: $auto_rig_front_facing, include_fingers: $include_fingers, "
		"    rerig_target: $rerig_target"
		"  ) { character { id name } auto_rig_confidence }"
		"}");

	/** Everything this account already holds, so an upload can be skipped. */
	static const TCHAR* CharactersQuery = TEXT("query { characters { id name } }");

	/** Documented ceiling on an uploaded character. */
	static constexpr int64 MaxUploadBytes = 30ll * 1024ll * 1024ll;

	/** Auto-rigging runs inside the request, and is documented as taking 30-60s. */
	static constexpr float UploadTimeoutSeconds = 300.f;

	static const TCHAR* RerigTargetToString(EMotionRerigTarget Target)
	{
		switch (Target)
		{
		case EMotionRerigTarget::UnrealEngine5: return TEXT("ue5");
		case EMotionRerigTarget::RobloxR15:     return TEXT("r15");
		default:                                return nullptr;
		}
	}
}

bool FUthanaProvider::HasCredential() const
{
	return FMotionCredentialStore::Has(GetCredentialServiceName());
}

FMotionProviderCaps FUthanaProvider::GetCaps() const
{
	FMotionProviderCaps Caps;
	Caps.ProviderId = ProviderId;
	Caps.DisplayName = GetDisplayName();
	Caps.DefaultModelId = GetDefaultModelId();

	Caps.bIsLocal = false;
	Caps.bIsMetered = true;
	Caps.bNeedsCredential = true;

	// v3.0 exposes no seed, which is the single fact that shapes how this plugin treats candidates:
	// a discarded take can never be produced again, so nothing is ever pruned automatically.
	Caps.bSupportsSeed = false;
	Caps.bSupportsConstraints = false;
	Caps.bSupportsPromptRewrite = true;

	// The whole point of Uthana: it generates against a character we uploaded, so clips come back on
	// our own bone names and need no retargeting.
	Caps.bSupportsCharacterUpload = true;
	Caps.bRequiresRetarget = false;

	// Asking for less re-times rather than resamples. A four second clip fetched at 30 arrives as an
	// 8.3 second file that imports without a warning and is simply wrong.
	Caps.NativeFrameRate = 60;

	GetLengthRange(FString(), Caps.MinLengthSeconds, Caps.MaxLengthSeconds);

	if (!HasCredential())
	{
		Caps.SetupHint = TEXT("Paste an API key into Project Settings > Plugins > MotionForge.");
	}

	return Caps;
}

void FUthanaProvider::GetLengthRange(const FString& ModelId, int32& OutMin, int32& OutMax) const
{
	// v3.0 will not generate below four seconds. Interaction verbs usually want two to three, so
	// generate at the floor and trim afterwards rather than fighting it.
	if (ModelId.IsEmpty() || ModelId.Contains(TEXT("3.0")))
	{
		OutMin = 4;
		OutMax = 10;
		return;
	}

	OutMin = 1;
	OutMax = 10;
}

FString FUthanaProvider::MakeViewerUrl(const FString& ProviderCharacterId, const FString& MotionId) const
{
	if (ProviderCharacterId.IsEmpty() || MotionId.IsEmpty())
	{
		return FString();
	}

	// Deep link into the web viewer. Watching is free; downloading is what a plan meters, so review
	// deliberately happens here rather than by pulling files down first.
	return FString::Printf(TEXT("https://uthana.com/motion/%s/%s"), *ProviderCharacterId, *MotionId);
}

TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> FUthanaProvider::MakeAuthenticatedRequest() const
{
	FString ApiKey;
	if (!FMotionCredentialStore::Get(GetCredentialServiceName(), ApiKey))
	{
		return nullptr;
	}

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(UthanaApi::Endpoint);
	Request->SetVerb(TEXT("POST"));

	// Documented only as "basic auth using your API key", which left the field order ambiguous.
	// Key-as-username with an empty password is what works - confirmed against a live account on
	// 2026-08-04, so do not "fix" this to the other order.
	const FString Credential = FBase64::Encode(ApiKey + TEXT(":"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Basic ") + Credential);

	// Do not let the key linger on the stack any longer than the header it went into.
	ApiKey.Empty();

	return Request;
}

TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> FUthanaProvider::MakeGraphQLRequest(
	const FString& Query,
	const TSharedPtr<FJsonObject>& Variables) const
{
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = MakeAuthenticatedRequest();
	if (!Request.IsValid())
	{
		return nullptr;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("query"), Query);
	if (Variables.IsValid())
	{
		Payload->SetObjectField(TEXT("variables"), Variables);
	}

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);

	return Request;
}

bool FUthanaProvider::ExtractGraphQLError(const TSharedPtr<FJsonObject>& Root, FString& OutError)
{
	if (!Root.IsValid())
	{
		OutError = TEXT("Response was not valid JSON.");
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	if (Root->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* First = nullptr;
		if ((*Errors)[0]->TryGetObject(First) && First)
		{
			(*First)->TryGetStringField(TEXT("message"), OutError);
		}

		if (OutError.IsEmpty())
		{
			OutError = TEXT("GraphQL returned an unspecified error.");
		}
		return true;
	}

	return false;
}

void FUthanaProvider::SubmitJob(const FMotionSubmitRequest& Request, FOnMotionSubmitComplete OnComplete)
{
	FMotionSubmitResult Failure;
	Failure.VariantIndex = Request.VariantIndex;

	TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("prompt"), Request.Prompt);
	Variables->SetStringField(TEXT("model"), Request.ModelId.IsEmpty() ? GetDefaultModelId() : Request.ModelId);
	Variables->SetNumberField(TEXT("length"), Request.Length);
	Variables->SetBoolField(TEXT("rewrite_prompt"), Request.bRewritePrompt);

	if (!Request.ProviderCharacterId.IsEmpty())
	{
		Variables->SetStringField(TEXT("character_id"), Request.ProviderCharacterId);
	}

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeGraphQLRequest(UthanaApi::SubmitMutation, Variables);
	if (!Http.IsValid())
	{
		Failure.Error = TEXT("No Uthana API key configured. Set one in Project Settings > Plugins > MotionForge.");
		OnComplete(Failure);
		return;
	}

	const int32 VariantIndex = Request.VariantIndex;

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete, VariantIndex](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FMotionSubmitResult Result;
			Result.VariantIndex = VariantIndex;

			if (!bConnected || !Response.IsValid())
			{
				Result.Error = TEXT("Could not reach uthana.com.");
				OnComplete(Result);
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(Reader, Root);

			if (ExtractGraphQLError(Root, Result.Error))
			{
				UE_LOG(LogMotionForge, Warning, TEXT("Submit failed (HTTP %d): %s"),
					Response->GetResponseCode(), *Response->GetContentAsString());
				OnComplete(Result);
				return;
			}

			const TSharedPtr<FJsonObject>* Data = nullptr;
			const TSharedPtr<FJsonObject>* Job = nullptr;
			if (Root->TryGetObjectField(TEXT("data"), Data) && Data
				&& (*Data)->TryGetObjectField(TEXT("create_text_to_motion_job"), Job) && Job)
			{
				// The mutation returns a wrapper whose only useful member is `job` - the id is one
				// level deeper than the shape suggests.
				const TSharedPtr<FJsonObject>* Inner = nullptr;
				if ((*Job)->TryGetObjectField(TEXT("job"), Inner) && Inner)
				{
					(*Inner)->TryGetStringField(TEXT("id"), Result.JobId);
				}
			}

			if (Result.JobId.IsEmpty())
			{
				// Log the body: a schema drift shows up here first, and guessing at it wastes far
				// more time than one line of output.
				Result.Error = TEXT("Submit succeeded but no job id was returned - the schema may have changed.");
				UE_LOG(LogMotionForge, Warning, TEXT("Unexpected submit response: %s"), *Response->GetContentAsString());
			}
			else
			{
				Result.bSuccess = true;
			}

			OnComplete(Result);
		});

	Http->ProcessRequest();
}

void FUthanaProvider::PollJob(const FString& JobId, FOnMotionJobPolled OnComplete)
{
	TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("job_id"), JobId);

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeGraphQLRequest(UthanaApi::JobQuery, Variables);
	if (!Http.IsValid())
	{
		FMotionJobResult Result;
		Result.Status = EMotionJobStatus::Failed;
		Result.Error = TEXT("No Uthana API key configured.");
		OnComplete(Result);
		return;
	}

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FMotionJobResult Result;

			if (!bConnected || !Response.IsValid())
			{
				// A dropped poll is not a dead job - stay Pending so the caller retries rather than
				// abandoning work that has already been paid for.
				Result.Status = EMotionJobStatus::Pending;
				Result.Error = TEXT("Poll request failed.");
				OnComplete(Result);
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(Reader, Root);

			if (ExtractGraphQLError(Root, Result.Error))
			{
				Result.Status = EMotionJobStatus::Failed;
				OnComplete(Result);
				return;
			}

			const TSharedPtr<FJsonObject>* Data = nullptr;
			const TSharedPtr<FJsonObject>* Job = nullptr;
			if (!Root->TryGetObjectField(TEXT("data"), Data) || !Data
				|| !(*Data)->TryGetObjectField(TEXT("job"), Job) || !Job)
			{
				Result.Status = EMotionJobStatus::Failed;
				Result.Error = TEXT("Poll response had no job object.");
				UE_LOG(LogMotionForge, Warning, TEXT("Unexpected poll response: %s"), *Response->GetContentAsString());
				OnComplete(Result);
				return;
			}

			FString StatusText;
			(*Job)->TryGetStringField(TEXT("status"), StatusText);
			StatusText = StatusText.ToUpper();

			if (StatusText == TEXT("FINISHED") || StatusText == TEXT("COMPLETED") || StatusText == TEXT("SUCCESS"))
			{
				Result.Status = EMotionJobStatus::Finished;

				// `result` is a JSON scalar, so a server is free to send it as an embedded object or
				// as a string holding the same JSON. Accept both rather than betting on one.
				TSharedPtr<FJsonObject> Outer;

				const TSharedPtr<FJsonObject>* AsObject = nullptr;
				FString AsString;

				if ((*Job)->TryGetObjectField(TEXT("result"), AsObject) && AsObject)
				{
					Outer = *AsObject;
				}
				else if ((*Job)->TryGetStringField(TEXT("result"), AsString) && !AsString.IsEmpty())
				{
					TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(AsString);
					FJsonSerializer::Deserialize(ResultReader, Outer);
				}

				const TSharedPtr<FJsonObject>* Inner = nullptr;
				if (Outer.IsValid() && Outer->TryGetObjectField(TEXT("result"), Inner) && Inner)
				{
					(*Inner)->TryGetStringField(TEXT("id"), Result.MotionId);
				}

				if (Result.MotionId.IsEmpty())
				{
					Result.Status = EMotionJobStatus::Failed;
					Result.Error = TEXT("Job finished but carried no motion id.");
					UE_LOG(LogMotionForge, Warning, TEXT("Finished job without id: %s"), *Response->GetContentAsString());
				}
			}
			else if (StatusText == TEXT("FAILED") || StatusText == TEXT("ERROR"))
			{
				Result.Status = EMotionJobStatus::Failed;
				Result.Error = TEXT("Provider reported the job failed.");
			}
			else if (StatusText == TEXT("RUNNING") || StatusText == TEXT("PROCESSING"))
			{
				Result.Status = EMotionJobStatus::Running;
			}
			else
			{
				Result.Status = EMotionJobStatus::Pending;
			}

			OnComplete(Result);
		});

	Http->ProcessRequest();
}

void FUthanaProvider::DownloadMotion(
	const FString& ProviderCharacterId,
	const FString& MotionId,
	const FString& AbsoluteLocalPath,
	int32 FrameRate,
	FOnMotionDownloadComplete OnComplete)
{
	FString ApiKey;
	if (!FMotionCredentialStore::Get(GetCredentialServiceName(), ApiKey))
	{
		OnComplete(false, TEXT("No Uthana API key configured."));
		return;
	}

	// Plain file fetch rather than GraphQL. This function always fetches; the caller decides whether
	// it already holds the file.
	//
	// Re-fetching is free on pay-as-you-go - verified by re-downloading the same clips repeatedly
	// across a session with no change to the balance, which is what makes iterating on the import
	// side cost nothing. A subscription meters downloads, so the same assumption there would be
	// expensive and has not been tested.
	//
	// no_mesh is not an optimisation, it is a correctness fix. With the character mesh in the file,
	// Unreal imports a skeletal mesh as the primary asset and the animation as a side effect, the
	// mesh's 'root' node collides with the skeleton's 'root' bone and gets silently renamed to
	// 'root1' - which is a real bone in the Narrative rig - and the clip ends up bound to a brand
	// new skeleton lying on its side. Animation-only files have none of those problems.
	const FString Url = FString::Printf(TEXT("%s/%s/%s/fbx/motion.fbx?fps=%d&no_mesh=true"),
		UthanaApi::FileHost, *ProviderCharacterId, *MotionId, FrameRate);

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(Url);
	Http->SetVerb(TEXT("GET"));
	Http->SetHeader(TEXT("Authorization"), TEXT("Basic ") + FBase64::Encode(ApiKey + TEXT(":")));
	ApiKey.Empty();

	const FString TargetPath = AbsoluteLocalPath;

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete, TargetPath, MotionId](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnComplete(false, TEXT("Download request failed."));
				return;
			}

			if (Response->GetResponseCode() != 200)
			{
				OnComplete(false, FString::Printf(TEXT("Download returned HTTP %d for motion %s."),
					Response->GetResponseCode(), *MotionId));
				return;
			}

			const TArray<uint8>& Payload = Response->GetContent();
			if (Payload.Num() == 0)
			{
				OnComplete(false, TEXT("Download returned an empty file."));
				return;
			}

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetPath), /*Tree*/ true);

			if (!FFileHelper::SaveArrayToFile(Payload, *TargetPath))
			{
				OnComplete(false, FString::Printf(TEXT("Could not write '%s'."), *TargetPath));
				return;
			}

			UE_LOG(LogMotionForge, Log, TEXT("Downloaded motion %s to %s (%d bytes)."),
				*MotionId, *TargetPath, Payload.Num());

			OnComplete(true, FString());
		});

	Http->ProcessRequest();
}

void FUthanaProvider::DownloadCharacterFile(
	const FString& CharacterId,
	const FString& AbsoluteLocalPath,
	FOnMotionDownloadComplete OnComplete)
{
	FString ApiKey;
	if (!FMotionCredentialStore::Get(GetCredentialServiceName(), ApiKey))
	{
		OnComplete(false, TEXT("No Uthana API key configured."));
		return;
	}

	// A different host path to the motion files: the bundle endpoint returns the character itself,
	// mesh and rig, as Uthana holds it after ingest.
	const FString Url = FString::Printf(TEXT("%s/%s/character.fbx"),
		UthanaApi::CharacterBundleHost, *CharacterId);

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(Url);
	Http->SetVerb(TEXT("GET"));
	Http->SetHeader(TEXT("Authorization"), TEXT("Basic ") + FBase64::Encode(ApiKey + TEXT(":")));
	Http->SetTimeout(UthanaApi::UploadTimeoutSeconds);
	ApiKey.Empty();

	const FString TargetPath = AbsoluteLocalPath;

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete, TargetPath, CharacterId](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnComplete(false, TEXT("Character download request failed."));
				return;
			}

			if (Response->GetResponseCode() != 200)
			{
				OnComplete(false, FString::Printf(TEXT("Character download returned HTTP %d for %s."),
					Response->GetResponseCode(), *CharacterId));
				return;
			}

			const TArray<uint8>& Payload = Response->GetContent();
			if (Payload.Num() == 0)
			{
				OnComplete(false, TEXT("Character download returned an empty file."));
				return;
			}

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetPath), /*Tree*/ true);

			if (!FFileHelper::SaveArrayToFile(Payload, *TargetPath))
			{
				OnComplete(false, FString::Printf(TEXT("Could not write '%s'."), *TargetPath));
				return;
			}

			UE_LOG(LogMotionForge, Log, TEXT("Downloaded character %s to %s (%d bytes)."),
				*CharacterId, *TargetPath, Payload.Num());

			OnComplete(true, FString());
		});

	Http->ProcessRequest();
}

void FUthanaProvider::TestConnection(FOnMotionTestComplete OnComplete)
{
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeGraphQLRequest(UthanaApi::PingQuery, nullptr);
	if (!Http.IsValid())
	{
		OnComplete(false, TEXT("No API key configured."));
		return;
	}

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnComplete(false, TEXT("Could not reach uthana.com."));
				return;
			}

			const int32 Code = Response->GetResponseCode();
			if (Code == 401 || Code == 403)
			{
				OnComplete(false, FString::Printf(
					TEXT("Rejected with HTTP %d - the key is wrong, or the basic-auth field order is."), Code));
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(Reader, Root);

			FString Error;
			if (ExtractGraphQLError(Root, Error))
			{
				OnComplete(false, Error);
				return;
			}

			OnComplete(true, TEXT("Connected to Uthana."));
		});

	Http->ProcessRequest();
}

// -------------------------------------------------------------------------------------------------
// Characters
// -------------------------------------------------------------------------------------------------

void FUthanaProvider::UploadCharacter(
	const FString& AbsoluteFilePath,
	const FString& Name,
	const FMotionCharacterUploadOptions& Options,
	FOnMotionCharacterUploaded OnComplete)
{
	FMotionCharacterUploadResult Failure;

	const FString Extension = FPaths::GetExtension(AbsoluteFilePath, /*bIncludeDot*/ false).ToLower();
	if (Extension != TEXT("fbx") && Extension != TEXT("glb") && Extension != TEXT("gltf"))
	{
		Failure.Error = FString::Printf(
			TEXT("'%s' is not a format Uthana accepts. Send .fbx, .glb or .gltf."), *AbsoluteFilePath);
		OnComplete(Failure);
		return;
	}

	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *AbsoluteFilePath))
	{
		Failure.Error = FString::Printf(TEXT("Could not read '%s'."), *AbsoluteFilePath);
		OnComplete(Failure);
		return;
	}

	// Checked here rather than left to the server, because a rejection after uploading tens of
	// megabytes is a slow way to learn something knowable up front.
	if (FileBytes.Num() > UthanaApi::MaxUploadBytes)
	{
		Failure.Error = FString::Printf(
			TEXT("'%s' is %.1f MB; Uthana's limit is 30 MB. Export a lower LOD or strip unused "
				 "material slots."),
			*FPaths::GetCleanFilename(AbsoluteFilePath), FileBytes.Num() / (1024.0 * 1024.0));
		OnComplete(Failure);
		return;
	}

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http = MakeAuthenticatedRequest();
	if (!Http.IsValid())
	{
		Failure.Error = TEXT("No API key configured.");
		OnComplete(Failure);
		return;
	}

	// GraphQL multipart request spec: an `operations` part holding the query with the file position
	// nulled out, a `map` part saying which part fills which variable, then the file itself.
	TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetField(TEXT("file"), MakeShared<FJsonValueNull>());
	Variables->SetStringField(TEXT("name"), Name);
	Variables->SetBoolField(TEXT("auto_rig"), Options.bAutoRig);
	Variables->SetBoolField(TEXT("auto_rig_front_facing"), Options.bAutoRigFrontFacing);
	Variables->SetBoolField(TEXT("include_fingers"), Options.bIncludeFingers);

	if (const TCHAR* Rerig = UthanaApi::RerigTargetToString(Options.RerigTarget))
	{
		Variables->SetStringField(TEXT("rerig_target"), Rerig);
	}
	else
	{
		// Explicit null rather than omitted: the variable is declared in the mutation, and leaving a
		// declared variable unsupplied is a GraphQL error on some servers.
		Variables->SetField(TEXT("rerig_target"), MakeShared<FJsonValueNull>());
	}

	TSharedRef<FJsonObject> Operations = MakeShared<FJsonObject>();
	Operations->SetStringField(TEXT("query"), UthanaApi::CreateCharacterMutation);
	Operations->SetObjectField(TEXT("variables"), Variables);

	FString OperationsJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OperationsJson);
	FJsonSerializer::Serialize(Operations, Writer);

	const FString Boundary = FString::Printf(TEXT("MotionForge%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString FileName = FPaths::GetCleanFilename(AbsoluteFilePath);

	auto AppendUtf8 = [](TArray<uint8>& Body, const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		Body.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	};

	TArray<uint8> Body;
	Body.Reserve(FileBytes.Num() + 1024);

	AppendUtf8(Body, FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendUtf8(Body, TEXT("Content-Disposition: form-data; name=\"operations\"\r\n\r\n"));
	AppendUtf8(Body, OperationsJson + TEXT("\r\n"));

	AppendUtf8(Body, FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendUtf8(Body, TEXT("Content-Disposition: form-data; name=\"map\"\r\n\r\n"));
	AppendUtf8(Body, TEXT("{\"0\":[\"variables.file\"]}\r\n"));

	AppendUtf8(Body, FString::Printf(TEXT("--%s\r\n"), *Boundary));
	AppendUtf8(Body, FString::Printf(
		TEXT("Content-Disposition: form-data; name=\"0\"; filename=\"%s\"\r\n"), *FileName));
	AppendUtf8(Body, TEXT("Content-Type: application/octet-stream\r\n\r\n"));
	Body.Append(FileBytes);
	AppendUtf8(Body, TEXT("\r\n"));

	AppendUtf8(Body, FString::Printf(TEXT("--%s--\r\n"), *Boundary));

	Http->SetHeader(TEXT("Content-Type"),
		FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
	Http->SetContent(Body);
	Http->SetTimeout(UthanaApi::UploadTimeoutSeconds);

	UE_LOG(LogMotionForge, Log, TEXT("Uploading character '%s' (%.1f MB) to Uthana..."),
		*Name, FileBytes.Num() / (1024.0 * 1024.0));

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete, Name](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FMotionCharacterUploadResult Result;

			if (!bConnected || !Response.IsValid())
			{
				Result.Error = TEXT("Could not reach uthana.com. Auto-rigging can take a minute - if "
									"this was a timeout the character may still have been created, so "
									"check List Provider Characters before uploading again.");
				OnComplete(Result);
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(Reader, Root);

			if (ExtractGraphQLError(Root, Result.Error))
			{
				UE_LOG(LogMotionForge, Warning, TEXT("Character upload failed (HTTP %d): %s"),
					Response->GetResponseCode(), *Response->GetContentAsString());
				OnComplete(Result);
				return;
			}

			const TSharedPtr<FJsonObject>* Data = nullptr;
			const TSharedPtr<FJsonObject>* Create = nullptr;
			const TSharedPtr<FJsonObject>* Character = nullptr;

			if (Root.IsValid()
				&& Root->TryGetObjectField(TEXT("data"), Data) && Data
				&& (*Data)->TryGetObjectField(TEXT("create_character"), Create) && Create
				&& (*Create)->TryGetObjectField(TEXT("character"), Character) && Character)
			{
				(*Character)->TryGetStringField(TEXT("id"), Result.CharacterId);
				(*Character)->TryGetStringField(TEXT("name"), Result.Name);

				double Confidence = 0.0;
				if ((*Create)->TryGetNumberField(TEXT("auto_rig_confidence"), Confidence))
				{
					Result.AutoRigConfidence = static_cast<float>(Confidence);
				}
			}

			if (Result.CharacterId.IsEmpty())
			{
				Result.Error = TEXT("Upload succeeded but no character id came back - the schema may "
									"have changed.");
				UE_LOG(LogMotionForge, Warning, TEXT("Unexpected upload response: %s"),
					*Response->GetContentAsString());
				OnComplete(Result);
				return;
			}

			Result.bSuccess = true;
			OnComplete(Result);
		});

	Http->ProcessRequest();
}

void FUthanaProvider::ListCharacters(FOnMotionCharactersListed OnComplete)
{
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http =
		MakeGraphQLRequest(UthanaApi::CharactersQuery, nullptr);

	if (!Http.IsValid())
	{
		OnComplete(false, {}, TEXT("No API key configured."));
		return;
	}

	Http->OnProcessRequestComplete().BindLambda(
		[OnComplete](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				OnComplete(false, {}, TEXT("Could not reach uthana.com."));
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			FJsonSerializer::Deserialize(Reader, Root);

			FString Error;
			if (ExtractGraphQLError(Root, Error))
			{
				OnComplete(false, {}, Error);
				return;
			}

			TArray<FMotionProviderCharacter> Characters;

			const TSharedPtr<FJsonObject>* Data = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;

			if (Root.IsValid()
				&& Root->TryGetObjectField(TEXT("data"), Data) && Data
				&& (*Data)->TryGetArrayField(TEXT("characters"), Entries) && Entries)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Entries)
				{
					const TSharedPtr<FJsonObject>* Entry = nullptr;
					if (!Value->TryGetObject(Entry) || !Entry)
					{
						continue;
					}

					FMotionProviderCharacter Character;
					(*Entry)->TryGetStringField(TEXT("id"), Character.Id);
					(*Entry)->TryGetStringField(TEXT("name"), Character.Name);

					if (!Character.Id.IsEmpty())
					{
						Characters.Add(MoveTemp(Character));
					}
				}
			}

			OnComplete(true, Characters, FString());
		});

	Http->ProcessRequest();
}
