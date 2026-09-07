// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"
#include "Runtime/UnrealAICancellation.h"
#include "Runtime/UnrealAIClock.h"

/** Closed result from the injected compact-JWS verifier. */
enum class EUnrealAIOidcTokenVerificationResult : uint8
{
	Succeeded,
	Malformed,
	SignatureRejected,
	UnsupportedAlgorithm,
	Failed
};

/** Closed browser/loopback callback outcome. Provider text never crosses this boundary. */
enum class EUnrealAIOAuthBrowserCallbackKind : uint8
{
	Invalid,
	AuthorizationCode,
	Denied,
	Failed
};

/** Bounded operation context shared by browser, issuer, crypto, and revocation seams. */
struct UNREALAIAUTH_API FUnrealAIOAuthAuthorizationOperationContext final
{
	static constexpr double MaxTimeoutSeconds = 30.0 * 60.0;

	static bool TryCreate(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock, double TimeoutSeconds,
						  const FUnrealAICancellationToken &InCancellation,
						  FUnrealAIOAuthAuthorizationOperationContext &OutContext, FString &OutError);
	bool ValidateShape(FString &OutError) const;
	bool IsCancellationRequested() const;
	bool IsTimedOut() const;
	double RemainingSeconds() const;

  private:
	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIDeadline Deadline;
	FUnrealAICancellationToken Cancellation;
};

/** Provider-owned exact authorization-server contract. No endpoint is learned as authority from discovery. */
struct UNREALAIAUTH_API FUnrealAIOAuthTrustedAuthorizationServer final
{
	static constexpr int32 MaxUriUtf8Bytes = 2048;
	static constexpr int32 MaxSigningAlgorithms = 8;

	FString Issuer;
	FString DiscoveryEndpoint;
	FString AuthorizationEndpoint;
	FString TokenEndpoint;
	FString JwksEndpoint;
	FString RevocationEndpoint;
	TArray<FName> AllowedSigningAlgorithms;

	bool ValidateShape(FString &OutError) const;
};

/** Strict typed projection of one OIDC discovery document. */
struct UNREALAIAUTH_API FUnrealAIOidcDiscoveryDocument final
{
	FString Issuer;
	FString AuthorizationEndpoint;
	FString TokenEndpoint;
	FString JwksEndpoint;
	FString RevocationEndpoint;
	bool bPkceS256Supported = false;

	bool ValidateShape(FString &OutError) const;
};

/** One bounded public signing key returned by the issuer seam. */
struct UNREALAIAUTH_API FUnrealAIOidcJsonWebKey final
{
	static constexpr int32 MaxIdentifierUtf8Bytes = 256;
	static constexpr int32 MaxPublicKeyMaterialBytes = 16 * 1024;

	FString KeyId;
	FName KeyType;
	FName Use;
	FName Algorithm;
	TArray<uint8> PublicKeyMaterial;

	bool ValidateShape(FString &OutError) const;
};

/** Bounded JWKS snapshot. MaxAgeSeconds controls the kernel's monotonic cache. */
struct UNREALAIAUTH_API FUnrealAIOidcJsonWebKeySet final
{
	static constexpr int32 MaxKeys = 32;
	static constexpr double MaxCacheAgeSeconds = 24.0 * 60.0 * 60.0;

	TArray<FUnrealAIOidcJsonWebKey> Keys;
	double MaxAgeSeconds = 0.0;

	bool ValidateShape(FString &OutError) const;
};

/** Strict compact-JWS protected-header projection produced by the crypto seam. */
struct UNREALAIAUTH_API FUnrealAIOidcProtectedHeader final
{
	FName Algorithm;
	FString KeyId;

	bool ValidateShape(FString &OutError) const;
};

/** Verified OIDC claims. These values never enter public account status or diagnostics. */
struct UNREALAIAUTH_API FUnrealAIOidcVerifiedClaims final
{
	static constexpr int32 MaxAudiences = 8;

	FString Issuer;
	FString Subject;
	TArray<FString> Audiences;
	FString AuthorizedParty;
	FString Nonce;
	FDateTime ExpiresAtUtc;
	FDateTime IssuedAtUtc;
	TOptional<FDateTime> NotBeforeUtc;

	bool ValidateShape(FString &OutError) const;
};

/** System-browser launch request. The URL contains ephemeral PKCE transaction material and must not be logged. */
struct UNREALAIAUTH_API FUnrealAIOAuthBrowserAuthorizationLaunch final
{
	FString AuthorizationUrl;
	FString ExactRedirectUri;
	FString ExpectedIssuer;
	/** Ephemeral CSRF binding used by loopback adapters to ignore unrelated local probes before returning. */
	FString ExpectedState;

	bool ValidateShape(FString &OutError) const;
};

/** Move-only browser/loopback callback. AuthorizationCode is present only for the success kind. */
struct UNREALAIAUTH_API FUnrealAIOAuthBrowserAuthorizationCallback final
{
	FUnrealAIOAuthBrowserAuthorizationCallback() = default;
	FUnrealAIOAuthBrowserAuthorizationCallback(const FUnrealAIOAuthBrowserAuthorizationCallback &) = delete;
	FUnrealAIOAuthBrowserAuthorizationCallback &operator=(const FUnrealAIOAuthBrowserAuthorizationCallback &) = delete;
	FUnrealAIOAuthBrowserAuthorizationCallback(FUnrealAIOAuthBrowserAuthorizationCallback &&) noexcept = default;
	FUnrealAIOAuthBrowserAuthorizationCallback &
	operator=(FUnrealAIOAuthBrowserAuthorizationCallback &&) noexcept = default;

	EUnrealAIOAuthBrowserCallbackKind Kind = EUnrealAIOAuthBrowserCallbackKind::Invalid;
	FString ExactRedirectUri;
	FString Issuer;
	FString State;
	FUnrealAISecretValue AuthorizationCode;

	bool ValidateShape(FString &OutError) const;
	void Reset();
};

/** Move-only token endpoint result. Raw provider response bodies never cross the issuer seam. */
struct UNREALAIAUTH_API FUnrealAIOAuthAuthorizationCodeResponse final
{
	FUnrealAIOAuthAuthorizationCodeResponse() = default;
	FUnrealAIOAuthAuthorizationCodeResponse(const FUnrealAIOAuthAuthorizationCodeResponse &) = delete;
	FUnrealAIOAuthAuthorizationCodeResponse &operator=(const FUnrealAIOAuthAuthorizationCodeResponse &) = delete;
	FUnrealAIOAuthAuthorizationCodeResponse(FUnrealAIOAuthAuthorizationCodeResponse &&) noexcept = default;
	FUnrealAIOAuthAuthorizationCodeResponse &operator=(FUnrealAIOAuthAuthorizationCodeResponse &&) noexcept = default;

	FUnrealAIOAuthTokenSet Tokens;
	FName TokenType;
	TArray<FString> GrantedScopes;

	bool ValidateShape(const FDateTime &NowUtc, FString &OutError) const;
	void Reset();
};

/** Trusted request for one browser-PKCE/OIDC authorization. */
struct UNREALAIAUTH_API FUnrealAIOAuthBrowserAuthorizationRequest final
{
	static constexpr int32 MaxScopes = 32;
	static constexpr double MaxClockSkewSeconds = 10.0 * 60.0;
	static constexpr double MaxIdentityTokenAgeSecondsLimit = 24.0 * 60.0 * 60.0;

	FUnrealAIRequestId RequestId;
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	FString ClientId;
	FString Audience;
	FString ExactRedirectUri;
	TArray<FString> RequestedScopes;
	/** Optional SHA-256 fingerprint returned by a prior successful authorization for subject continuity. */
	FString ExpectedSubjectFingerprint;
	double TimeoutSeconds = 0.0;
	double ClockSkewSeconds = 60.0;
	double MaxIdentityTokenAgeSeconds = 15.0 * 60.0;
	double MaxJwksCacheAgeSeconds = 60.0 * 60.0;

	bool ValidateShape(FString &OutError) const;
};

/** Successful generic authorization output. SubjectFingerprint is opaque and safe for exact local continuity checks. */
struct UNREALAIAUTH_API FUnrealAIOAuthAuthorizationResult final
{
	FUnrealAIOAuthAuthorizationResult() = default;
	FUnrealAIOAuthAuthorizationResult(const FUnrealAIOAuthAuthorizationResult &) = delete;
	FUnrealAIOAuthAuthorizationResult &operator=(const FUnrealAIOAuthAuthorizationResult &) = delete;
	FUnrealAIOAuthAuthorizationResult(FUnrealAIOAuthAuthorizationResult &&) noexcept = default;
	FUnrealAIOAuthAuthorizationResult &operator=(FUnrealAIOAuthAuthorizationResult &&) noexcept = default;

	FUnrealAIOAuthTokenSet Tokens;
	FString SubjectFingerprint;
	TArray<FString> GrantedScopes;

	void Reset();
};

/** Trusted bounded revocation request. Local deletion/quarantine remains the account provider's responsibility. */
struct UNREALAIAUTH_API FUnrealAIOAuthRevocationRequest final
{
	FUnrealAIRequestId RequestId;
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	FString ClientId;
	double TimeoutSeconds = 0.0;

	bool ValidateShape(FString &OutError) const;
};

/** Injected cryptographic boundary. Production implementations must use a reviewed platform crypto backend. */
class UNREALAIAUTH_API IUnrealAIOAuthAuthorizationCrypto
{
  public:
	virtual ~IUnrealAIOAuthAuthorizationCrypto() = default;
	virtual bool GenerateSecureRandomBytes(int32 NumBytes, TArray<uint8> &OutBytes,
										   FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool Sha256(TConstArrayView<uint8> Input, TArray<uint8> &OutDigest,
						FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool InspectProtectedHeader(TConstArrayView<uint8> CompactIdToken, FUnrealAIOidcProtectedHeader &OutHeader,
										FUnrealAIProviderAccessError &OutError) = 0;
	virtual EUnrealAIOidcTokenVerificationResult VerifyAndDecodeIdToken(TConstArrayView<uint8> CompactIdToken,
																		const FUnrealAIOidcJsonWebKey &Key,
																		FUnrealAIOidcVerifiedClaims &OutClaims,
																		FUnrealAIProviderAccessError &OutError) = 0;
};

/** Injected exact-issuer protocol boundary. All secret byte views are synchronous and must not be retained or logged.
 */
class UNREALAIAUTH_API IUnrealAIOAuthAuthorizationIssuer
{
  public:
	virtual ~IUnrealAIOAuthAuthorizationIssuer() = default;
	virtual bool Discover(const FUnrealAIOAuthAuthorizationOperationContext &Context,
						  FStringView ExactDiscoveryEndpoint, FUnrealAIOidcDiscoveryDocument &OutDocument,
						  FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool FetchJsonWebKeys(const FUnrealAIOAuthAuthorizationOperationContext &Context,
								  FStringView ExactJwksEndpoint, FUnrealAIOidcJsonWebKeySet &OutKeySet,
								  FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool ExchangeAuthorizationCode(const FUnrealAIOAuthAuthorizationOperationContext &Context,
										   FStringView ExactTokenEndpoint, TConstArrayView<uint8> FormBody,
										   FUnrealAIOAuthAuthorizationCodeResponse &OutResponse,
										   FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool Revoke(const FUnrealAIOAuthAuthorizationOperationContext &Context, FStringView ExactRevocationEndpoint,
						TConstArrayView<uint8> FormBody, FUnrealAIProviderAccessError &OutError) = 0;
};

/** Injected system-browser plus exact loopback callback boundary. */
class UNREALAIAUTH_API IUnrealAIOAuthAuthorizationBrowser
{
  public:
	virtual ~IUnrealAIOAuthAuthorizationBrowser() = default;
	virtual bool Authorize(const FUnrealAIOAuthAuthorizationOperationContext &Context,
						   const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch,
						   FUnrealAIOAuthBrowserAuthorizationCallback &OutCallback,
						   FUnrealAIProviderAccessError &OutError) = 0;
};

/**
 * Synchronous, cancellation-aware authorization executor used by the retained async coordinator.
 *
 * Implementations may block only the coordinator's bounded worker thread. They must observe the supplied cancellation
 * token and the request deadline through their injected browser/issuer seams, return within that bound, and never
 * retain secret views supplied to Revoke.
 */
class UNREALAIAUTH_API IUnrealAIOAuthAuthorizationExecutor
{
  public:
	virtual ~IUnrealAIOAuthAuthorizationExecutor() = default;
	virtual bool AuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
									  const FUnrealAICancellationToken &Cancellation,
									  FUnrealAIOAuthAuthorizationResult &OutResult,
									  FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool Revoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
						const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) = 0;
};

/**
 * Generic, provider-neutral browser-PKCE/OIDC and revocation kernel.
 *
 * The provider supplies only exact trusted issuer/client/resource metadata and injected platform seams. The kernel
 * generates transaction material, validates discovery and callbacks, enforces JWKS/algorithm/claim bindings, caches
 * keys monotonically with one forced rotation retry, and returns one synchronous terminal result.
 */
class UNREALAIAUTH_API FUnrealAIOAuthAuthorizationKernel final : public IUnrealAIOAuthAuthorizationExecutor
{
  public:
	FUnrealAIOAuthAuthorizationKernel(TSharedRef<IUnrealAIOAuthAuthorizationCrypto, ESPMode::ThreadSafe> InCrypto,
									  TSharedRef<IUnrealAIOAuthAuthorizationIssuer, ESPMode::ThreadSafe> InIssuer,
									  TSharedRef<IUnrealAIOAuthAuthorizationBrowser, ESPMode::ThreadSafe> InBrowser,
									  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock);
	~FUnrealAIOAuthAuthorizationKernel();
	FUnrealAIOAuthAuthorizationKernel(const FUnrealAIOAuthAuthorizationKernel &) = delete;
	FUnrealAIOAuthAuthorizationKernel &operator=(const FUnrealAIOAuthAuthorizationKernel &) = delete;

	bool AuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
							  const FUnrealAICancellationToken &Cancellation,
							  FUnrealAIOAuthAuthorizationResult &OutResult,
							  FUnrealAIProviderAccessError &OutError) override;
	bool Revoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
				const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) override;
	void ClearCaches();

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
