#include "MotionCredentialStore.h"

#include "MotionForge.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincred.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace MotionForgeCredentials
{
	/**
	 * Services that are not this plugin's, they are the family's.
	 *
	 * A Runpod key and a Hugging Face token are accounts a *person* has, not things a plugin owns, so
	 * naming their vault entries after whichever plugin happened to ask first was wrong: install a
	 * second plugin that rents GPUs and you are asked for the same key again, and now there are two
	 * copies to keep in step. These live under one family-wide name that every plugin agrees on.
	 *
	 * Agreeing on a *name* is not a dependency. Each plugin still carries its own vault code and works
	 * alone; they simply write to the same drawer.
	 */
	static const TCHAR* SharedNamespace = TEXT("AutomationForge");

	static FString SharedServiceFor(const FString& Service)
	{
		if (Service.Equals(TEXT("KimodoRunpod"), ESearchCase::IgnoreCase))       { return TEXT("Runpod"); }
		if (Service.Equals(TEXT("KimodoHuggingFace"), ESearchCase::IgnoreCase))  { return TEXT("HuggingFace"); }
		return FString();
	}

	/** Vault entries are namespaced so they are recognisable in the Windows credential list. */
	static FString MakeTargetName(const FString& Service)
	{
		const FString Shared = SharedServiceFor(Service);

		return Shared.IsEmpty()
			? FString::Printf(TEXT("MotionForge/%s"), *Service)
			: FString::Printf(TEXT("%s/%s"), SharedNamespace, *Shared);
	}

	/**
	 * Where a shared key used to live, or empty when it never moved.
	 *
	 * Read-only and deliberately so. A key already in the old entry keeps working, and gets written to
	 * the new one the next time somebody saves it - which is a migration nobody has to perform and
	 * nothing that copies a secret around behind their back.
	 */
	static FString LegacyTargetName(const FString& Service)
	{
		return SharedServiceFor(Service).IsEmpty()
			? FString()
			: FString::Printf(TEXT("MotionForge/%s"), *Service);
	}
}

FString FMotionCredentialStore::GetEnvironmentVariableName(const FString& Service)
{
	return FString::Printf(TEXT("MOTIONFORGE_%s_KEY"), *Service.ToUpper());
}

bool FMotionCredentialStore::IsVaultAvailable()
{
#if PLATFORM_WINDOWS
	return true;
#else
	// macOS Keychain and Linux libsecret backends are not written yet - those platforms are
	// environment-variable only, which still keeps the key out of the project folder.
	return false;
#endif
}

bool FMotionCredentialStore::Get(const FString& Service, FString& OutSecret)
{
	// Environment wins, so CI and headless runs never depend on an interactive user's vault.
	const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(*GetEnvironmentVariableName(Service));
	if (!FromEnv.IsEmpty())
	{
		OutSecret = FromEnv;
		return true;
	}

#if PLATFORM_WINDOWS
	FString Target = MotionForgeCredentials::MakeTargetName(Service);

	// A key stored before these two moved to the family-wide name is still a key the user gave us.
	const FString Legacy = MotionForgeCredentials::LegacyTargetName(Service);
	if (!Legacy.IsEmpty())
	{
		PCREDENTIALW Probe = nullptr;
		const bool bHasCurrent = ::CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Probe) && Probe;
		if (Probe) { ::CredFree(Probe); }

		if (!bHasCurrent)
		{
			Target = Legacy;
		}
	}

	PCREDENTIALW Credential = nullptr;
	if (::CredReadW(*Target, CRED_TYPE_GENERIC, 0, &Credential) && Credential)
	{
		// CredentialBlob is raw bytes, stored here as UTF-8 and not null terminated.
		const int32 ByteCount = static_cast<int32>(Credential->CredentialBlobSize);
		if (ByteCount > 0 && Credential->CredentialBlob)
		{
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Credential->CredentialBlob), ByteCount);
			OutSecret = FString(Converter.Length(), Converter.Get());
		}
		else
		{
			OutSecret.Reset();
		}

		::CredFree(Credential);
		return !OutSecret.IsEmpty();
	}
#endif

	OutSecret.Reset();
	return false;
}

bool FMotionCredentialStore::Set(const FString& Service, const FString& Secret)
{
	if (Secret.IsEmpty())
	{
		return Remove(Service);
	}

#if PLATFORM_WINDOWS
	const FString Target = MotionForgeCredentials::MakeTargetName(Service);
	const FTCHARToUTF8 Converter(*Secret);

	CREDENTIALW Credential = {};
	Credential.Type = CRED_TYPE_GENERIC;
	Credential.TargetName = const_cast<LPWSTR>(*Target);
	Credential.CredentialBlobSize = static_cast<DWORD>(Converter.Length());
	Credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<ANSICHAR*>(Converter.Get()));

	// LOCAL_MACHINE keeps the entry to this user on this machine and survives logout. Deliberately
	// not CRED_PERSIST_ENTERPRISE, which roams the credential with the user profile - that would put
	// the key on a domain server or synced profile, which is exactly what we are avoiding.
	Credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

	if (::CredWriteW(&Credential, 0))
	{
		UE_LOG(LogMotionForge, Log, TEXT("Stored credential for '%s' in the Windows credential vault."), *Service);
		return true;
	}

	UE_LOG(LogMotionForge, Error, TEXT("CredWriteW failed for '%s' (error %u)."), *Service, ::GetLastError());
	return false;
#else
	UE_LOG(LogMotionForge, Error,
		TEXT("No credential vault backend on this platform. Set the %s environment variable instead."),
		*GetEnvironmentVariableName(Service));
	return false;
#endif
}

bool FMotionCredentialStore::Remove(const FString& Service)
{
#if PLATFORM_WINDOWS
	const FString Target = MotionForgeCredentials::MakeTargetName(Service);
	if (::CredDeleteW(*Target, CRED_TYPE_GENERIC, 0))
	{
		UE_LOG(LogMotionForge, Log, TEXT("Removed credential for '%s'."), *Service);
		return true;
	}
#endif
	return false;
}

bool FMotionCredentialStore::Has(const FString& Service)
{
	FString Unused;
	const bool bFound = Get(Service, Unused);

	// Do not leave the secret sitting in a stack string longer than needed.
	Unused.Empty();
	return bFound;
}

FString FMotionCredentialStore::DescribeSource(const FString& Service)
{
	const FString EnvName = GetEnvironmentVariableName(Service);
	if (!FPlatformMisc::GetEnvironmentVariable(*EnvName).IsEmpty())
	{
		return FString::Printf(TEXT("Configured (environment variable %s)"), *EnvName);
	}

	FString Unused;
	if (Get(Service, Unused))
	{
		Unused.Empty();
		return TEXT("Configured (OS credential vault)");
	}

	return IsVaultAvailable()
		? TEXT("Not configured")
		: FString::Printf(TEXT("Not configured - this platform has no vault backend, set %s"), *EnvName);
}
