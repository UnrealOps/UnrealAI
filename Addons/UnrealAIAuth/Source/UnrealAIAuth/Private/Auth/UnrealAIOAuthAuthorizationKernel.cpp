// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"

#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

// Keep private helper symbols collision-free when Unreal unity-builds this implementation with other Auth sources.
#define MakeAuthError MakeOAuthAuthorizationKernelError
#define SecureResetBytes SecureResetOAuthAuthorizationBytes
#define SecureResetString SecureResetOAuthAuthorizationString

namespace
{
constexpr int32 StateRandomBytes = 32;
constexpr int32 NonceRandomBytes = 32;
constexpr int32 VerifierRandomBytes = 64;
constexpr int32 Sha256DigestBytes = 32;
constexpr int32 MaxAuthorizationClientIdUtf8Bytes = 512;
constexpr int32 MaxAudienceUtf8Bytes = 2048;
constexpr int32 MaxScopeUtf8Bytes = 256;
constexpr int32 MaxSubjectUtf8Bytes = 2048;
constexpr int32 MaxCallbackValueUtf8Bytes = 4096;
constexpr int32 MaxReplayDigests = 256;

const FName BearerTokenType(TEXT("Bearer"));
const FName SignatureUse(TEXT("sig"));

FUnrealAIProviderAccessError MakeAuthError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAIProviderAccessError NormalizeSeamError(const FUnrealAIProviderAccessError &Candidate)
{
	FString ShapeError;
	if (Candidate.IsError() && Candidate.ValidateShape(ShapeError))
	{
		return Candidate;
	}
	return MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
}

void SecureResetBytes(TArray<uint8> &Bytes)
{
	if (!Bytes.IsEmpty())
	{
		FMemory::Memzero(Bytes.GetData(), Bytes.Num());
	}
	Bytes.Empty();
}

void SecureResetString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

bool IsBoundedUtf8(const FStringView Value, const int32 MaxBytes, const bool bAllowEmpty = false)
{
	const FString Copy(Value);
	FTCHARToUTF8 Utf8(*Copy);
	return (bAllowEmpty || Utf8.Length() > 0) && Utf8.Length() <= MaxBytes;
}

bool IsVisibleAscii(const FStringView Value, const int32 MaxBytes, const bool bAllowEmpty = false)
{
	if (!IsBoundedUtf8(Value, MaxBytes, bAllowEmpty))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (Character < 0x21 || Character > 0x7e)
		{
			return false;
		}
	}
	return true;
}

bool IsOAuthScope(const FStringView Scope)
{
	if (!IsBoundedUtf8(Scope, MaxScopeUtf8Bytes))
	{
		return false;
	}
	for (const TCHAR Character : Scope)
	{
		const bool bAllowed =
			Character == 0x21 || (Character >= 0x23 && Character <= 0x5b) || (Character >= 0x5d && Character <= 0x7e);
		if (!bAllowed)
		{
			return false;
		}
	}
	return true;
}

bool IsBase64Url(const FStringView Value, const int32 ExactLength)
{
	if (Value.Len() != ExactLength)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!((Character >= TEXT('A') && Character <= TEXT('Z')) ||
			   (Character >= TEXT('a') && Character <= TEXT('z')) ||
				(Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('-') || Character == TEXT('_')))
		{
			return false;
		}
	}
	return true;
}

bool ConstantTimeEquals(const FStringView A, const FStringView B)
{
	uint32 Difference = static_cast<uint32>(A.Len() ^ B.Len());
	const int32 Count = FMath::Max(A.Len(), B.Len());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint32 Left = Index < A.Len() ? static_cast<uint32>(A[Index]) : 0u;
		const uint32 Right = Index < B.Len() ? static_cast<uint32>(B[Index]) : 0u;
		Difference |= Left ^ Right;
	}
	return Difference == 0;
}

bool IsSigningAlgorithm(const FName Algorithm)
{
	const FString Name = Algorithm.ToString();
	return Name == TEXT("RS256") || Name == TEXT("PS256") || Name == TEXT("ES256") || Name == TEXT("EdDSA");
}

bool KeyTypeMatchesAlgorithm(const FName KeyType, const FName Algorithm)
{
	const FString Type = KeyType.ToString();
	const FString Alg = Algorithm.ToString();
	if (Alg == TEXT("RS256") || Alg == TEXT("PS256"))
	{
		return Type == TEXT("RSA");
	}
	if (Alg == TEXT("ES256"))
	{
		return Type == TEXT("EC");
	}
	return Alg == TEXT("EdDSA") && Type == TEXT("OKP");
}

bool IsExactHttpsUri(const FString &Value, const bool bRequirePath)
{
	if (!IsVisibleAscii(Value, FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes) ||
		!Value.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive) ||
						  Value.Contains(TEXT("\\")) || Value.Contains(TEXT("?")) || Value.Contains(TEXT("#")))
	{
		return false;
	}
	const int32 SchemeBytes = 8;
	const int32 PathIndex = Value.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SchemeBytes);
	if (bRequirePath && (PathIndex == INDEX_NONE || PathIndex == Value.Len() - 1))
	{
		return false;
	}
	const FString OriginText = PathIndex == INDEX_NONE ? Value : Value.Left(PathIndex);
	if (OriginText.Contains(TEXT("@")))
	{
		return false;
	}
	FUnrealAIEndpointOrigin Origin;
	FString Error;
	return FUnrealAIEndpointOrigin::TryParse(OriginText, false, Origin, Error) && Origin.IsSecure() &&
		   Origin.ToString() == OriginText;
}

bool IsExactLoopbackRedirect(const FString &Value)
{
	if (!IsVisibleAscii(Value, FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes) ||
		Value.Contains(TEXT("\\")) || Value.Contains(TEXT("?")) || Value.Contains(TEXT("#")))
	{
		return false;
	}
	const int32 SchemeEnd = Value.Find(TEXT("://"), ESearchCase::CaseSensitive);
	if (SchemeEnd == INDEX_NONE)
	{
		return false;
	}
	const int32 PathIndex = Value.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SchemeEnd + 3);
	if (PathIndex == INDEX_NONE || PathIndex == Value.Len() - 1)
	{
		return false;
	}
	const FString OriginText = Value.Left(PathIndex);
	if (OriginText.Contains(TEXT("@")))
	{
		return false;
	}
	FUnrealAIEndpointOrigin Origin;
	FString Error;
	return FUnrealAIEndpointOrigin::TryParse(OriginText, true, Origin, Error) && Origin.HasLoopbackHost() &&
		   Origin.ToString() == OriginText;
}

bool UniqueBoundedStrings(const TArray<FString> &Values, const int32 MaxCount, const int32 MaxUtf8Bytes,
						  const bool bScopes)
{
	if (Values.IsEmpty() || Values.Num() > MaxCount)
	{
		return false;
	}
	TSet<FString> Seen;
	for (const FString &Value : Values)
	{
		if ((bScopes ? !IsOAuthScope(Value) : !IsVisibleAscii(Value, MaxUtf8Bytes)) || Seen.Contains(Value))
		{
			return false;
		}
		Seen.Add(Value);
	}
	return true;
}

FString Base64UrlEncode(const TConstArrayView<uint8> Bytes)
{
	TArray<uint8> Copy;
	Copy.Append(Bytes.GetData(), Bytes.Num());
	FString Encoded = FBase64::Encode(Copy);
	SecureResetBytes(Copy);
	Encoded.ReplaceInline(TEXT("+"), TEXT("-"), ESearchCase::CaseSensitive);
	Encoded.ReplaceInline(TEXT("/"), TEXT("_"), ESearchCase::CaseSensitive);
	while (Encoded.EndsWith(TEXT("="), ESearchCase::CaseSensitive))
	{
		Encoded.LeftChopInline(1, EAllowShrinking::No);
	}
	return Encoded;
}

void AppendPercentEncoded(TArray<uint8> &OutBytes, const TConstArrayView<uint8> Input)
{
	static constexpr char Hex[] = "0123456789ABCDEF";
	for (const uint8 Byte : Input)
	{
		const bool bUnreserved = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') ||
								 (Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '.' || Byte == '_' ||
								 Byte == '~';
		if (bUnreserved)
		{
			OutBytes.Add(Byte);
		}
		else
		{
			OutBytes.Add('%');
			OutBytes.Add(static_cast<uint8>(Hex[(Byte >> 4) & 0x0f]));
			OutBytes.Add(static_cast<uint8>(Hex[Byte & 0x0f]));
		}
	}
}

void AppendAscii(TArray<uint8> &OutBytes, const ANSICHAR *Text)
{
	for (const ANSICHAR *Cursor = Text; Cursor != nullptr && *Cursor != '\0'; ++Cursor)
	{
		OutBytes.Add(static_cast<uint8>(*Cursor));
	}
}

void AppendEncodedString(TArray<uint8> &OutBytes, const FStringView Value)
{
	const FString Copy(Value);
	FTCHARToUTF8 Utf8(*Copy);
	AppendPercentEncoded(OutBytes, MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()));
}

FString PercentEncodedString(const FStringView Value)
{
	TArray<uint8> Bytes;
	AppendEncodedString(Bytes, Value);
	Bytes.Add(0);
	FString Result = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()));
	SecureResetBytes(Bytes);
	return Result;
}

bool ObserveContext(const FUnrealAIOAuthAuthorizationOperationContext &Context, FUnrealAIProviderAccessError &OutError)
{
	if (Context.IsCancellationRequested())
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
		return false;
	}
	if (Context.IsTimedOut())
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
		return false;
	}
	return true;
}

bool ValidateDiscovery(const FUnrealAIOAuthTrustedAuthorizationServer &Trusted,
					   const FUnrealAIOidcDiscoveryDocument &Document, FUnrealAIProviderAccessError &OutError)
{
	FString ShapeError;
	if (!Document.ValidateShape(ShapeError) || Document.Issuer != Trusted.Issuer ||
		Document.AuthorizationEndpoint != Trusted.AuthorizationEndpoint ||
		Document.TokenEndpoint != Trusted.TokenEndpoint || Document.JwksEndpoint != Trusted.JwksEndpoint ||
		Document.RevocationEndpoint != Trusted.RevocationEndpoint || !Document.bPkceS256Supported)
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
		return false;
	}
	return true;
}

bool ScopeSetContainsAll(const TArray<FString> &Granted, const TArray<FString> &Required)
{
	TSet<FString> GrantedSet;
	for (const FString &Scope : Granted)
	{
		GrantedSet.Add(Scope);
	}
	for (const FString &Scope : Required)
	{
		if (!GrantedSet.Contains(Scope))
		{
			return false;
		}
	}
	return true;
}

bool BuildTokenExchangeBody(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
							const TConstArrayView<uint8> AuthorizationCode, const FStringView Verifier,
							TArray<uint8> &OutBody)
{
	OutBody.Reset();
	AppendAscii(OutBody, "grant_type=authorization_code&code=");
	AppendPercentEncoded(OutBody, AuthorizationCode);
	AppendAscii(OutBody, "&redirect_uri=");
	AppendEncodedString(OutBody, Request.ExactRedirectUri);
	AppendAscii(OutBody, "&client_id=");
	AppendEncodedString(OutBody, Request.ClientId);
	AppendAscii(OutBody, "&code_verifier=");
	AppendEncodedString(OutBody, Verifier);
	return !OutBody.IsEmpty() && OutBody.Num() <= FUnrealAISecretValue::MaxSecretBytes;
}

bool BuildRevocationBody(const FUnrealAIOAuthRevocationRequest &Request, const TConstArrayView<uint8> Token,
						 TArray<uint8> &OutBody)
{
	OutBody.Reset();
	AppendAscii(OutBody, "token=");
	AppendPercentEncoded(OutBody, Token);
	AppendAscii(OutBody, "&client_id=");
	AppendEncodedString(OutBody, Request.ClientId);
	return !OutBody.IsEmpty() && OutBody.Num() <= FUnrealAISecretValue::MaxSecretBytes;
}
} // namespace

bool FUnrealAIOAuthAuthorizationOperationContext::TryCreate(
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock, const double TimeoutSeconds,
	const FUnrealAICancellationToken &InCancellation, FUnrealAIOAuthAuthorizationOperationContext &OutContext,
	FString &OutError)
{
	OutContext = {};
	OutError.Reset();
	if (!InCancellation.IsValid() || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 ||
		TimeoutSeconds > MaxTimeoutSeconds)
	{
		OutError = TEXT("OAuth operation context requires a valid cancellation token and bounded timeout.");
		return false;
	}
	OutContext.Clock = MoveTemp(InClock);
	OutContext.Deadline = FUnrealAIDeadline::FromNow(*OutContext.Clock, TimeoutSeconds);
	OutContext.Cancellation = InCancellation;
	return true;
}

bool FUnrealAIOAuthAuthorizationOperationContext::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!Clock.IsValid() || !Cancellation.IsValid() || !FMath::IsFinite(Deadline.AtMonotonicSeconds))
	{
		OutError = TEXT("OAuth operation context is invalid.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthAuthorizationOperationContext::IsCancellationRequested() const
{
	return !Cancellation.IsValid() || Cancellation.IsCancellationRequested();
}

bool FUnrealAIOAuthAuthorizationOperationContext::IsTimedOut() const
{
	return !Clock.IsValid() || Deadline.IsExpired(*Clock);
}

double FUnrealAIOAuthAuthorizationOperationContext::RemainingSeconds() const
{
	return Clock.IsValid() ? Deadline.RemainingSeconds(*Clock) : 0.0;
}

bool FUnrealAIOAuthTrustedAuthorizationServer::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsExactHttpsUri(Issuer, false) || !IsExactHttpsUri(DiscoveryEndpoint, true) ||
		!IsExactHttpsUri(AuthorizationEndpoint, true) || !IsExactHttpsUri(TokenEndpoint, true) ||
		!IsExactHttpsUri(JwksEndpoint, true) ||
		(!RevocationEndpoint.IsEmpty() && !IsExactHttpsUri(RevocationEndpoint, true)) ||
		AllowedSigningAlgorithms.IsEmpty() || AllowedSigningAlgorithms.Num() > MaxSigningAlgorithms)
	{
		OutError = TEXT("Trusted authorization server requires exact bounded HTTPS endpoints and signing algorithms.");
		return false;
	}
	TSet<FName> Seen;
	for (const FName Algorithm : AllowedSigningAlgorithms)
	{
		if (!IsSigningAlgorithm(Algorithm) || Seen.Contains(Algorithm))
		{
			OutError = TEXT("Trusted authorization server has an invalid or duplicate signing algorithm.");
			return false;
		}
		Seen.Add(Algorithm);
	}
	return true;
}

bool FUnrealAIOidcDiscoveryDocument::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsExactHttpsUri(Issuer, false) || !IsExactHttpsUri(AuthorizationEndpoint, true) ||
		!IsExactHttpsUri(TokenEndpoint, true) || !IsExactHttpsUri(JwksEndpoint, true) ||
		(!RevocationEndpoint.IsEmpty() && !IsExactHttpsUri(RevocationEndpoint, true)))
	{
		OutError = TEXT("OIDC discovery document contains an invalid endpoint.");
		return false;
	}
	return true;
}

bool FUnrealAIOidcJsonWebKey::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsVisibleAscii(KeyId, MaxIdentifierUtf8Bytes) || KeyType.IsNone() || Use != SignatureUse ||
		!IsSigningAlgorithm(Algorithm) || !KeyTypeMatchesAlgorithm(KeyType, Algorithm) || PublicKeyMaterial.IsEmpty() ||
		PublicKeyMaterial.Num() > MaxPublicKeyMaterialBytes)
	{
		OutError = TEXT("OIDC JSON Web Key has an invalid key identifier, purpose, algorithm, or material bound.");
		return false;
	}
	return true;
}

bool FUnrealAIOidcJsonWebKeySet::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (Keys.IsEmpty() || Keys.Num() > MaxKeys || !FMath::IsFinite(MaxAgeSeconds) || MaxAgeSeconds < 0.0 ||
		MaxAgeSeconds > MaxCacheAgeSeconds)
	{
		OutError = TEXT("OIDC JWKS has an invalid key count or cache bound.");
		return false;
	}
	TSet<FString> Seen;
	for (const FUnrealAIOidcJsonWebKey &Key : Keys)
	{
		FString KeyError;
		if (!Key.ValidateShape(KeyError) || Seen.Contains(Key.KeyId))
		{
			OutError = TEXT("OIDC JWKS contains an invalid or duplicate key.");
			return false;
		}
		Seen.Add(Key.KeyId);
	}
	return true;
}

bool FUnrealAIOidcProtectedHeader::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsSigningAlgorithm(Algorithm) || !IsVisibleAscii(KeyId, FUnrealAIOidcJsonWebKey::MaxIdentifierUtf8Bytes))
	{
		OutError = TEXT("OIDC protected header requires a supported asymmetric algorithm and bounded key identifier.");
		return false;
	}
	return true;
}

bool FUnrealAIOidcVerifiedClaims::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsExactHttpsUri(Issuer, false) || !IsBoundedUtf8(Subject, MaxSubjectUtf8Bytes) ||
		!UniqueBoundedStrings(Audiences, MaxAudiences, MaxAudienceUtf8Bytes, false) ||
		(!AuthorizedParty.IsEmpty() && !IsVisibleAscii(AuthorizedParty, MaxAuthorizationClientIdUtf8Bytes)) ||
		!IsBase64Url(Nonce, 43) || ExpiresAtUtc.GetTicks() <= 0 || IssuedAtUtc.GetTicks() <= 0 ||
		(NotBeforeUtc.IsSet() && NotBeforeUtc.GetValue().GetTicks() <= 0))
	{
		OutError = TEXT("Verified OIDC claims have an invalid issuer, subject, audience, nonce, or time shape.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthBrowserAuthorizationLaunch::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsVisibleAscii(AuthorizationUrl, FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes * 4) ||
		!AuthorizationUrl.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive) ||
									 !IsExactLoopbackRedirect(ExactRedirectUri) ||
									 !IsExactHttpsUri(ExpectedIssuer, false) || !IsBase64Url(ExpectedState, 43))
	{
		OutError = TEXT("OAuth browser launch has an invalid URL, redirect, issuer, or state.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthBrowserAuthorizationCallback::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const bool bKnownKind = Kind == EUnrealAIOAuthBrowserCallbackKind::AuthorizationCode ||
							Kind == EUnrealAIOAuthBrowserCallbackKind::Denied ||
							Kind == EUnrealAIOAuthBrowserCallbackKind::Failed;
	if (!bKnownKind || !IsExactLoopbackRedirect(ExactRedirectUri) || !IsExactHttpsUri(Issuer, false) ||
		!IsBase64Url(State, 43) ||
		((Kind == EUnrealAIOAuthBrowserCallbackKind::AuthorizationCode) != AuthorizationCode.IsSet()) ||
		AuthorizationCode.Num() > MaxCallbackValueUtf8Bytes)
	{
		OutError = TEXT("OAuth browser callback has an invalid outcome or bounded callback binding.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthBrowserAuthorizationCallback::Reset()
{
	Kind = EUnrealAIOAuthBrowserCallbackKind::Invalid;
	ExactRedirectUri.Reset();
	Issuer.Reset();
	SecureResetString(State);
	AuthorizationCode.Reset();
}

bool FUnrealAIOAuthAuthorizationCodeResponse::ValidateShape(const FDateTime &NowUtc, FString &OutError) const
{
	OutError.Reset();
	if (TokenType != BearerTokenType || !Tokens.AccessToken.IsSet() || !Tokens.IdToken.IsSet() ||
		Tokens.AccessToken.Num() > FUnrealAIOAuthTokenSet::MaxAccessTokenBytes ||
		Tokens.RefreshToken.Num() > FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes ||
		Tokens.IdToken.Num() > FUnrealAIOAuthTokenSet::MaxIdTokenBytes || Tokens.AccessTokenExpiresAtUtc <= NowUtc ||
		!UniqueBoundedStrings(GrantedScopes, FUnrealAIOAuthBrowserAuthorizationRequest::MaxScopes, MaxScopeUtf8Bytes,
							  true))
	{
		OutError = TEXT("OAuth token response is incomplete, expired, over-sized, or has invalid scope/token type.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthAuthorizationCodeResponse::Reset()
{
	Tokens.Reset();
	TokenType = NAME_None;
	GrantedScopes.Reset();
}

bool FUnrealAIOAuthBrowserAuthorizationRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString ServerError;
	if (!RequestId.IsValid() || !Server.ValidateShape(ServerError) ||
		!IsVisibleAscii(ClientId, MaxAuthorizationClientIdUtf8Bytes) ||
		!IsVisibleAscii(Audience, MaxAudienceUtf8Bytes) || !IsExactLoopbackRedirect(ExactRedirectUri) ||
		!UniqueBoundedStrings(RequestedScopes, MaxScopes, MaxScopeUtf8Bytes, true) ||
		!RequestedScopes.Contains(
			TEXT("openid")) ||
			(!ExpectedSubjectFingerprint.IsEmpty() && !IsBase64Url(ExpectedSubjectFingerprint, 43)) ||
			!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 ||
			TimeoutSeconds > FUnrealAIOAuthAuthorizationOperationContext::MaxTimeoutSeconds ||
			!FMath::IsFinite(ClockSkewSeconds) || ClockSkewSeconds < 0.0 || ClockSkewSeconds > MaxClockSkewSeconds ||
			!FMath::IsFinite(MaxIdentityTokenAgeSeconds) || MaxIdentityTokenAgeSeconds <= 0.0 ||
			MaxIdentityTokenAgeSeconds > MaxIdentityTokenAgeSecondsLimit || !FMath::IsFinite(MaxJwksCacheAgeSeconds) ||
			MaxJwksCacheAgeSeconds < 0.0 || MaxJwksCacheAgeSeconds > FUnrealAIOidcJsonWebKeySet::MaxCacheAgeSeconds)
	{
		OutError = TEXT("OAuth browser authorization request has invalid trusted metadata or bounded policy.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthAuthorizationResult::Reset()
{
	Tokens.Reset();
	SubjectFingerprint.Reset();
	GrantedScopes.Reset();
}

bool FUnrealAIOAuthRevocationRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString ServerError;
	if (!RequestId.IsValid() || !Server.ValidateShape(ServerError) || Server.RevocationEndpoint.IsEmpty() ||
		!IsVisibleAscii(ClientId, MaxAuthorizationClientIdUtf8Bytes) || !FMath::IsFinite(TimeoutSeconds) ||
		TimeoutSeconds <= 0.0 || TimeoutSeconds > FUnrealAIOAuthAuthorizationOperationContext::MaxTimeoutSeconds)
	{
		OutError = TEXT("OAuth revocation request requires a trusted revocation endpoint and bounded request.");
		return false;
	}
	return true;
}

struct FUnrealAIOAuthAuthorizationKernel::FImpl final
{
	struct FKeyCacheEntry final
	{
		FUnrealAIOidcJsonWebKeySet KeySet;
		double CachedAtMonotonicSeconds = 0.0;
		double ExpiresAtMonotonicSeconds = 0.0;
	};

	FImpl(TSharedRef<IUnrealAIOAuthAuthorizationCrypto, ESPMode::ThreadSafe> InCrypto,
		  TSharedRef<IUnrealAIOAuthAuthorizationIssuer, ESPMode::ThreadSafe> InIssuer,
		  TSharedRef<IUnrealAIOAuthAuthorizationBrowser, ESPMode::ThreadSafe> InBrowser,
		  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
		: Crypto(MoveTemp(InCrypto)), Issuer(MoveTemp(InIssuer)), Browser(MoveTemp(InBrowser)), Clock(MoveTemp(InClock))
	{
	}

	bool GenerateBase64Url(const FUnrealAIOAuthAuthorizationOperationContext &Context, const int32 RandomBytes,
						   FString &OutValue, FUnrealAIProviderAccessError &OutError)
	{
		OutValue.Reset();
		TArray<uint8> Bytes;
		OutError = {};
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		const bool bGenerated = Crypto->GenerateSecureRandomBytes(RandomBytes, Bytes, OutError);
		if (!ObserveContext(Context, OutError))
		{
			SecureResetBytes(Bytes);
			return false;
		}
		if (!bGenerated || Bytes.Num() != RandomBytes)
		{
			SecureResetBytes(Bytes);
			OutError = NormalizeSeamError(OutError);
			return false;
		}
		OutValue = Base64UrlEncode(Bytes);
		SecureResetBytes(Bytes);
		if (!IsBase64Url(OutValue, RandomBytes == VerifierRandomBytes ? 86 : 43))
		{
			SecureResetString(OutValue);
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		OutError = {};
		return true;
	}

	bool FetchKeySet(const FUnrealAIOAuthAuthorizationOperationContext &Context,
					 const FUnrealAIOAuthBrowserAuthorizationRequest &Request, const bool bForceRefresh,
					 FUnrealAIOidcJsonWebKeySet &OutKeySet, FUnrealAIProviderAccessError &OutError)
	{
		OutKeySet = {};
		OutError = {};
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		const double Now = Clock->MonotonicSeconds();
		FString CacheKey = Request.Server.Issuer;
		CacheKey.AppendChar(TEXT('\n'));
		CacheKey.Append(Request.Server.JwksEndpoint);
		if (!bForceRefresh && Request.MaxJwksCacheAgeSeconds > 0.0)
		{
			FScopeLock Lock(&Mutex);
			if (const FKeyCacheEntry *Cached = KeyCache.Find(CacheKey);
				Cached != nullptr && FMath::IsFinite(Now) && Now >= Cached->CachedAtMonotonicSeconds &&
				Now < Cached->ExpiresAtMonotonicSeconds &&
				Now - Cached->CachedAtMonotonicSeconds < Request.MaxJwksCacheAgeSeconds)
			{
				OutKeySet = Cached->KeySet;
				return true;
			}
		}

		FUnrealAIOidcJsonWebKeySet Fetched;
		OutError = {};
		const bool bFetched = Issuer->FetchJsonWebKeys(Context, Request.Server.JwksEndpoint, Fetched, OutError);
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		if (!bFetched)
		{
			OutError = NormalizeSeamError(OutError);
			return false;
		}
		FString ShapeError;
		if (!Fetched.ValidateShape(ShapeError))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
			return false;
		}
		const double CacheAge = FMath::Min(Fetched.MaxAgeSeconds, Request.MaxJwksCacheAgeSeconds);
		const double CachedAt = Clock->MonotonicSeconds();
		const double ExpiresAt = CachedAt + CacheAge;
		if (CacheAge > 0.0 && FMath::IsFinite(CachedAt) && FMath::IsFinite(ExpiresAt))
		{
			FScopeLock Lock(&Mutex);
			FKeyCacheEntry &Entry = KeyCache.FindOrAdd(CacheKey);
			Entry.KeySet = Fetched;
			Entry.CachedAtMonotonicSeconds = CachedAt;
			Entry.ExpiresAtMonotonicSeconds = ExpiresAt;
		}
		OutKeySet = MoveTemp(Fetched);
		OutError = {};
		return true;
	}

	const FUnrealAIOidcJsonWebKey *FindKey(const FUnrealAIOidcJsonWebKeySet &KeySet,
										   const FUnrealAIOidcProtectedHeader &Header) const
	{
		return KeySet.Keys.FindByPredicate(
			[&Header](const FUnrealAIOidcJsonWebKey &Key)
			{
				return Key.KeyId == Header.KeyId && Key.Algorithm == Header.Algorithm && Key.Use == SignatureUse &&
					   KeyTypeMatchesAlgorithm(Key.KeyType, Header.Algorithm);
			});
	}

	bool MarkCallbackStateConsumed(const FUnrealAIOAuthAuthorizationOperationContext &Context, const FStringView State,
								   FUnrealAIProviderAccessError &OutError)
	{
		const FString StateCopy(State);
		FTCHARToUTF8 Utf8(*StateCopy);
		TArray<uint8> Digest;
		OutError = {};
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		const bool bHashed =
			Crypto->Sha256(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), Digest, OutError);
		if (!ObserveContext(Context, OutError))
		{
			SecureResetBytes(Digest);
			return false;
		}
		if (!bHashed || Digest.Num() != Sha256DigestBytes)
		{
			SecureResetBytes(Digest);
			OutError = NormalizeSeamError(OutError);
			return false;
		}
		FString Fingerprint = Base64UrlEncode(Digest);
		SecureResetBytes(Digest);
		FScopeLock Lock(&Mutex);
		if (ConsumedStateDigests.Contains(Fingerprint))
		{
			SecureResetString(Fingerprint);
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
			return false;
		}
		ConsumedStateDigests.Add(Fingerprint);
		while (ConsumedStateDigests.Num() > MaxReplayDigests)
		{
			ConsumedStateDigests.RemoveAt(0, 1, EAllowShrinking::No);
		}
		OutError = {};
		return true;
	}

	bool VerifyIdToken(const FUnrealAIOAuthAuthorizationOperationContext &Context,
					   const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
					   const TConstArrayView<uint8> CompactIdToken, FUnrealAIOidcVerifiedClaims &OutClaims,
					   FUnrealAIProviderAccessError &OutError)
	{
		OutClaims = {};
		FUnrealAIOidcProtectedHeader Header;
		OutError = {};
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		const bool bInspected = Crypto->InspectProtectedHeader(CompactIdToken, Header, OutError);
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		if (!bInspected)
		{
			OutError = NormalizeSeamError(OutError);
			return false;
		}
		FString HeaderError;
		if (!Header.ValidateShape(HeaderError) || !Request.Server.AllowedSigningAlgorithms.Contains(Header.Algorithm))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
			return false;
		}

		FUnrealAIOidcJsonWebKeySet KeySet;
		bool bForcedRefresh = false;
		if (!FetchKeySet(Context, Request, false, KeySet, OutError))
		{
			return false;
		}
		const FUnrealAIOidcJsonWebKey *Key = FindKey(KeySet, Header);
		if (Key == nullptr)
		{
			bForcedRefresh = true;
			if (!FetchKeySet(Context, Request, true, KeySet, OutError) || (Key = FindKey(KeySet, Header)) == nullptr)
			{
				if (!OutError.IsError())
				{
					OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
											 EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
				}
				return false;
			}
		}

		OutError = {};
		EUnrealAIOidcTokenVerificationResult Verification =
			Crypto->VerifyAndDecodeIdToken(CompactIdToken, *Key, OutClaims, OutError);
		if (Verification == EUnrealAIOidcTokenVerificationResult::SignatureRejected && !bForcedRefresh)
		{
			if (!FetchKeySet(Context, Request, true, KeySet, OutError) || (Key = FindKey(KeySet, Header)) == nullptr)
			{
				if (!OutError.IsError())
				{
					OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
											 EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
				}
				return false;
			}
			OutError = {};
			Verification = Crypto->VerifyAndDecodeIdToken(CompactIdToken, *Key, OutClaims, OutError);
		}
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		if (Verification != EUnrealAIOidcTokenVerificationResult::Succeeded)
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
			return false;
		}
		OutError = {};
		return true;
	}

	bool ValidateClaims(const FUnrealAIOAuthAuthorizationOperationContext &Context,
						const FUnrealAIOAuthBrowserAuthorizationRequest &Request, const FStringView ExpectedNonce,
						const FUnrealAIOidcVerifiedClaims &Claims, FString &OutSubjectFingerprint,
						FUnrealAIProviderAccessError &OutError)
	{
		OutSubjectFingerprint.Reset();
		OutError = {};
		if (!ObserveContext(Context, OutError))
		{
			return false;
		}
		FString ShapeError;
		if (!Claims.ValidateShape(ShapeError) || Claims.Issuer != Request.Server.Issuer ||
			!Claims.Audiences.Contains(Request.Audience) ||
			(Claims.Audiences.Num() > 1 && Claims.AuthorizedParty.IsEmpty()) ||
			(!Claims.AuthorizedParty.IsEmpty() && Claims.AuthorizedParty != Request.ClientId) ||
			!ConstantTimeEquals(Claims.Nonce, ExpectedNonce))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
			return false;
		}

		const FDateTime Now = Clock->UtcNow();
		const FTimespan Skew = FTimespan::FromSeconds(Request.ClockSkewSeconds);
		const FTimespan MaxAge = FTimespan::FromSeconds(Request.MaxIdentityTokenAgeSeconds);
		const FTimespan MaxLifetime =
			FTimespan::FromSeconds(FUnrealAIOAuthBrowserAuthorizationRequest::MaxIdentityTokenAgeSecondsLimit);
		if (Claims.ExpiresAtUtc <= Claims.IssuedAtUtc || Now > Claims.ExpiresAtUtc + Skew ||
			Claims.IssuedAtUtc > Now + Skew || Now - Claims.IssuedAtUtc > MaxAge + Skew ||
			Claims.ExpiresAtUtc - Claims.IssuedAtUtc > MaxLifetime + Skew ||
			(Claims.NotBeforeUtc.IsSet() &&
			 (Claims.NotBeforeUtc.GetValue() > Now + Skew || Claims.NotBeforeUtc.GetValue() >= Claims.ExpiresAtUtc)))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid);
			return false;
		}

		FString SubjectBinding = Claims.Issuer;
		SubjectBinding.AppendChar(TEXT('\n'));
		SubjectBinding.Append(Claims.Subject);
		FTCHARToUTF8 Utf8(*SubjectBinding);
		TArray<uint8> Digest;
		OutError = {};
		const bool bHashed =
			Crypto->Sha256(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), Digest, OutError);
		if (!ObserveContext(Context, OutError))
		{
			SecureResetString(SubjectBinding);
			SecureResetBytes(Digest);
			return false;
		}
		if (!bHashed || Digest.Num() != Sha256DigestBytes)
		{
			SecureResetString(SubjectBinding);
			SecureResetBytes(Digest);
			OutError = NormalizeSeamError(OutError);
			return false;
		}
		SecureResetString(SubjectBinding);
		OutSubjectFingerprint = Base64UrlEncode(Digest);
		SecureResetBytes(Digest);
		if (!Request.ExpectedSubjectFingerprint.IsEmpty() &&
			!ConstantTimeEquals(OutSubjectFingerprint, Request.ExpectedSubjectFingerprint))
		{
			SecureResetString(OutSubjectFingerprint);
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
			return false;
		}
		OutError = {};
		return true;
	}

	void ClearCaches()
	{
		FScopeLock Lock(&Mutex);
		KeyCache.Reset();
		for (FString &Digest : ConsumedStateDigests)
		{
			SecureResetString(Digest);
		}
		ConsumedStateDigests.Reset();
	}

	TSharedRef<IUnrealAIOAuthAuthorizationCrypto, ESPMode::ThreadSafe> Crypto;
	TSharedRef<IUnrealAIOAuthAuthorizationIssuer, ESPMode::ThreadSafe> Issuer;
	TSharedRef<IUnrealAIOAuthAuthorizationBrowser, ESPMode::ThreadSafe> Browser;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FCriticalSection Mutex;
	TMap<FString, FKeyCacheEntry> KeyCache;
	TArray<FString> ConsumedStateDigests;
};

FUnrealAIOAuthAuthorizationKernel::FUnrealAIOAuthAuthorizationKernel(
	TSharedRef<IUnrealAIOAuthAuthorizationCrypto, ESPMode::ThreadSafe> InCrypto,
	TSharedRef<IUnrealAIOAuthAuthorizationIssuer, ESPMode::ThreadSafe> InIssuer,
	TSharedRef<IUnrealAIOAuthAuthorizationBrowser, ESPMode::ThreadSafe> InBrowser,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
	: Impl(MakeUnique<FImpl>(MoveTemp(InCrypto), MoveTemp(InIssuer), MoveTemp(InBrowser), MoveTemp(InClock)))
{
}

FUnrealAIOAuthAuthorizationKernel::~FUnrealAIOAuthAuthorizationKernel()
{
	Impl->ClearCaches();
}

bool FUnrealAIOAuthAuthorizationKernel::AuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
															 const FUnrealAICancellationToken &Cancellation,
															 FUnrealAIOAuthAuthorizationResult &OutResult,
															 FUnrealAIProviderAccessError &OutError)
{
	OutResult.Reset();
	OutError = {};
	FString ShapeError;
	if (!Request.ValidateShape(ShapeError) || !Cancellation.IsValid())
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}

	FUnrealAIOAuthAuthorizationOperationContext Context;
	if (!FUnrealAIOAuthAuthorizationOperationContext::TryCreate(Impl->Clock, Request.TimeoutSeconds, Cancellation,
																Context, ShapeError) ||
		!ObserveContext(Context, OutError))
	{
		if (!OutError.IsError())
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::InvalidArgument,
									 EUnrealAIProviderAccessErrorCode::InvalidRequest);
		}
		return false;
	}

	FUnrealAIOidcDiscoveryDocument Discovery;
	OutError = {};
	const bool bDiscovered = Impl->Issuer->Discover(Context, Request.Server.DiscoveryEndpoint, Discovery, OutError);
	if (!ObserveContext(Context, OutError))
	{
		return false;
	}
	if (!bDiscovered)
	{
		OutError = NormalizeSeamError(OutError);
		return false;
	}
	if (!ValidateDiscovery(Request.Server, Discovery, OutError))
	{
		return false;
	}

	FString State;
	FString Nonce;
	FString Verifier;
	FString Challenge;
	FUnrealAIOAuthBrowserAuthorizationLaunch Launch;
	FUnrealAIOAuthBrowserAuthorizationCallback Callback;
	FUnrealAIOAuthAuthorizationCodeResponse TokenResponse;
	TArray<uint8> FormBody;
	ON_SCOPE_EXIT
	{
		SecureResetString(State);
		SecureResetString(Nonce);
		SecureResetString(Verifier);
		SecureResetString(Challenge);
		SecureResetString(Launch.AuthorizationUrl);
		SecureResetString(Launch.ExpectedState);
		Callback.Reset();
		TokenResponse.Reset();
		SecureResetBytes(FormBody);
	};

	if (!Impl->GenerateBase64Url(Context, StateRandomBytes, State, OutError) ||
		!Impl->GenerateBase64Url(Context, NonceRandomBytes, Nonce, OutError) ||
		!Impl->GenerateBase64Url(Context, VerifierRandomBytes, Verifier, OutError))
	{
		return false;
	}
	FTCHARToUTF8 VerifierUtf8(*Verifier);
	TArray<uint8> ChallengeDigest;
	OutError = {};
	const bool bChallengeHashed =
		Impl->Crypto->Sha256(MakeArrayView(reinterpret_cast<const uint8 *>(VerifierUtf8.Get()), VerifierUtf8.Length()),
							 ChallengeDigest, OutError);
	if (!ObserveContext(Context, OutError))
	{
		SecureResetBytes(ChallengeDigest);
		return false;
	}
	if (!bChallengeHashed || ChallengeDigest.Num() != Sha256DigestBytes)
	{
		SecureResetBytes(ChallengeDigest);
		OutError = NormalizeSeamError(OutError);
		return false;
	}
	Challenge = Base64UrlEncode(ChallengeDigest);
	SecureResetBytes(ChallengeDigest);
	if (!IsBase64Url(Challenge, 43))
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}

	const FString Scope = FString::Join(Request.RequestedScopes, TEXT(" "));
	Launch.AuthorizationUrl = Request.Server.AuthorizationEndpoint;
	Launch.AuthorizationUrl.Append(TEXT("?response_type=code&client_id="));
	Launch.AuthorizationUrl.Append(PercentEncodedString(Request.ClientId));
	Launch.AuthorizationUrl.Append(TEXT("&redirect_uri="));
	Launch.AuthorizationUrl.Append(PercentEncodedString(Request.ExactRedirectUri));
	Launch.AuthorizationUrl.Append(TEXT("&scope="));
	Launch.AuthorizationUrl.Append(PercentEncodedString(Scope));
	Launch.AuthorizationUrl.Append(TEXT("&state="));
	Launch.AuthorizationUrl.Append(State);
	Launch.AuthorizationUrl.Append(TEXT("&nonce="));
	Launch.AuthorizationUrl.Append(Nonce);
	Launch.AuthorizationUrl.Append(TEXT("&code_challenge="));
	Launch.AuthorizationUrl.Append(Challenge);
	Launch.AuthorizationUrl.Append(TEXT("&code_challenge_method=S256"));
	Launch.ExactRedirectUri = Request.ExactRedirectUri;
	Launch.ExpectedIssuer = Request.Server.Issuer;
	Launch.ExpectedState = State;
	if (!Launch.ValidateShape(ShapeError))
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::InvalidConfiguration,
								 EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	OutError = {};
	const bool bBrowserCompleted = Impl->Browser->Authorize(Context, Launch, Callback, OutError);
	if (!ObserveContext(Context, OutError))
	{
		return false;
	}
	if (!bBrowserCompleted)
	{
		OutError = NormalizeSeamError(OutError);
		return false;
	}
	if (!Callback.ValidateShape(ShapeError) || Callback.ExactRedirectUri != Request.ExactRedirectUri ||
		Callback.Issuer != Request.Server.Issuer || !ConstantTimeEquals(Callback.State, State))
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
		return false;
	}
	if (!Impl->MarkCallbackStateConsumed(Context, State, OutError))
	{
		return false;
	}
	if (Callback.Kind != EUnrealAIOAuthBrowserCallbackKind::AuthorizationCode)
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}

	if (!BuildTokenExchangeBody(Request, Callback.AuthorizationCode.View(), Verifier, FormBody))
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
		return false;
	}
	OutError = {};
	const bool bExchanged = Impl->Issuer->ExchangeAuthorizationCode(Context, Request.Server.TokenEndpoint, FormBody,
																	TokenResponse, OutError);
	if (!ObserveContext(Context, OutError))
	{
		return false;
	}
	if (!bExchanged)
	{
		OutError = NormalizeSeamError(OutError);
		return false;
	}
	if (!TokenResponse.ValidateShape(Impl->Clock->UtcNow(), ShapeError))
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
		return false;
	}
	if (!ScopeSetContainsAll(TokenResponse.GrantedScopes, Request.RequestedScopes))
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
								 EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient);
		return false;
	}

	FUnrealAIOidcVerifiedClaims Claims;
	if (!Impl->VerifyIdToken(Context, Request, TokenResponse.Tokens.IdToken.View(), Claims, OutError))
	{
		return false;
	}
	FString SubjectFingerprint;
	if (!Impl->ValidateClaims(Context, Request, Nonce, Claims, SubjectFingerprint, OutError))
	{
		return false;
	}
	OutResult.Tokens = MoveTemp(TokenResponse.Tokens);
	OutResult.SubjectFingerprint = MoveTemp(SubjectFingerprint);
	OutResult.GrantedScopes = MoveTemp(TokenResponse.GrantedScopes);
	TokenResponse.Reset();
	OutError = {};
	return true;
}

bool FUnrealAIOAuthAuthorizationKernel::Revoke(const FUnrealAIOAuthRevocationRequest &Request,
											   FUnrealAISecretValue &&Token,
											   const FUnrealAICancellationToken &Cancellation,
											   FUnrealAIProviderAccessError &OutError)
{
	OutError = {};
	ON_SCOPE_EXIT
	{
		Token.Reset();
	};
	FString ShapeError;
	if (!Request.ValidateShape(ShapeError) || !Cancellation.IsValid() || !Token.IsSet())
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	FUnrealAIOAuthAuthorizationOperationContext Context;
	if (!FUnrealAIOAuthAuthorizationOperationContext::TryCreate(Impl->Clock, Request.TimeoutSeconds, Cancellation,
																Context, ShapeError) ||
		!ObserveContext(Context, OutError))
	{
		if (!OutError.IsError())
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::InvalidArgument,
									 EUnrealAIProviderAccessErrorCode::InvalidRequest);
		}
		return false;
	}
	FUnrealAIOidcDiscoveryDocument Discovery;
	OutError = {};
	const bool bDiscovered = Impl->Issuer->Discover(Context, Request.Server.DiscoveryEndpoint, Discovery, OutError);
	if (!ObserveContext(Context, OutError))
	{
		return false;
	}
	if (!bDiscovered)
	{
		OutError = NormalizeSeamError(OutError);
		return false;
	}
	if (!ValidateDiscovery(Request.Server, Discovery, OutError))
	{
		return false;
	}
	TArray<uint8> FormBody;
	ON_SCOPE_EXIT
	{
		SecureResetBytes(FormBody);
	};
	if (!BuildRevocationBody(Request, Token.View(), FormBody))
	{
		OutError =
			MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
		return false;
	}
	OutError = {};
	const bool bRevoked = Impl->Issuer->Revoke(Context, Request.Server.RevocationEndpoint, FormBody, OutError);
	if (!ObserveContext(Context, OutError))
	{
		return false;
	}
	if (!bRevoked)
	{
		OutError = NormalizeSeamError(OutError);
		return false;
	}
	OutError = {};
	return true;
}

void FUnrealAIOAuthAuthorizationKernel::ClearCaches()
{
	Impl->ClearCaches();
}

#undef SecureResetString
#undef SecureResetBytes
#undef MakeAuthError
