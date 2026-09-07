// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/UnrealAIDigest.h"
#include "Testing/UnrealAITestClock.h"
#include "Misc/Base64.h"
#include "Runtime/UnrealAIClock.h"

namespace
{
FUnrealAIProviderAccessError MakeFailure()
{
	FUnrealAIProviderAccessError Error;
	Error.Category = EUnrealAIErrorCategory::Provider;
	Error.Code = EUnrealAIProviderAccessErrorCode::AuthFailed;
	return Error;
}

bool MakeSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret)
{
	TArray<uint8> Bytes;
	for (const ANSICHAR *Cursor = Text; Cursor != nullptr && *Cursor != '\0'; ++Cursor)
	{
		Bytes.Add(static_cast<uint8>(*Cursor));
	}
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

bool ContainsAscii(const TConstArrayView<uint8> Bytes, const ANSICHAR *Needle)
{
	int32 NeedleLength = 0;
	while (Needle != nullptr && Needle[NeedleLength] != '\0')
	{
		++NeedleLength;
	}
	if (NeedleLength == 0 || NeedleLength > Bytes.Num())
	{
		return false;
	}
	for (int32 Offset = 0; Offset <= Bytes.Num() - NeedleLength; ++Offset)
	{
		bool bMatch = true;
		for (int32 Index = 0; Index < NeedleLength; ++Index)
		{
			bMatch &= Bytes[Offset + Index] == static_cast<uint8>(Needle[Index]);
		}
		if (bMatch)
		{
			return true;
		}
	}
	return false;
}

FString ReadAsciiFormValue(const TConstArrayView<uint8> Bytes, const ANSICHAR *Marker)
{
	int32 MarkerLength = 0;
	while (Marker != nullptr && Marker[MarkerLength] != '\0')
	{
		++MarkerLength;
	}
	for (int32 Offset = 0; MarkerLength > 0 && Offset <= Bytes.Num() - MarkerLength; ++Offset)
	{
		bool bMatch = true;
		for (int32 Index = 0; Index < MarkerLength; ++Index)
		{
			bMatch &= Bytes[Offset + Index] == static_cast<uint8>(Marker[Index]);
		}
		if (!bMatch)
		{
			continue;
		}
		FString Value;
		for (int32 Index = Offset + MarkerLength; Index < Bytes.Num() && Bytes[Index] != static_cast<uint8>('&');
			 ++Index)
		{
			if (Bytes[Index] > 0x7f)
			{
				return {};
			}
			Value.AppendChar(static_cast<TCHAR>(Bytes[Index]));
		}
		return Value;
	}
	return {};
}

bool DecodeLowerHexDigest(const FString &Hex, TArray<uint8> &OutBytes)
{
	OutBytes.Reset();
	if (Hex.Len() != 64)
	{
		return false;
	}
	OutBytes.Reserve(32);
	auto Nibble = [](const TCHAR Character) -> int32
	{
		if (Character >= TEXT('0') && Character <= TEXT('9'))
		{
			return Character - TEXT('0');
		}
		if (Character >= TEXT('a') && Character <= TEXT('f'))
		{
			return Character - TEXT('a') + 10;
		}
		return INDEX_NONE;
	};
	for (int32 Index = 0; Index < Hex.Len(); Index += 2)
	{
		const int32 High = Nibble(Hex[Index]);
		const int32 Low = Nibble(Hex[Index + 1]);
		if (High == INDEX_NONE || Low == INDEX_NONE)
		{
			OutBytes.Reset();
			return false;
		}
		OutBytes.Add(static_cast<uint8>((High << 4) | Low));
	}
	return true;
}

bool ComputePortableSha256(const TConstArrayView<uint8> Input, TArray<uint8> &OutDigest)
{
	FString Digest;
	return UE::UnrealAI::Digest::Sha256(Input, Digest) && DecodeLowerHexDigest(Digest, OutDigest);
}

FString ComputePkceS256Challenge(const FString &Verifier)
{
	FTCHARToUTF8 Utf8(*Verifier);
	TArray<uint8> Digest;
	if (!ComputePortableSha256(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), Digest))
	{
		return {};
	}
	FString Encoded = FBase64::Encode(Digest);
	Encoded.ReplaceInline(TEXT("+"), TEXT("-"), ESearchCase::CaseSensitive);
	Encoded.ReplaceInline(TEXT("/"), TEXT("_"), ESearchCase::CaseSensitive);
	while (Encoded.EndsWith(TEXT("="), ESearchCase::CaseSensitive))
	{
		Encoded.LeftChopInline(1, EAllowShrinking::No);
	}
	return Encoded;
}

FString ReadQueryValue(const FString &Url, const FString &Name)
{
	const FString Marker = Name + TEXT("=");
	const int32 Start = Url.Find(Marker, ESearchCase::CaseSensitive);
	if (Start == INDEX_NONE)
	{
		return {};
	}
	const int32 ValueStart = Start + Marker.Len();
	const int32 End = Url.Find(TEXT("&"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValueStart);
	return End == INDEX_NONE ? Url.Mid(ValueStart) : Url.Mid(ValueStart, End - ValueStart);
}

struct FKernelFixtureState final
{
	FString CapturedNonce;
	FString CapturedState;
	FString CapturedCodeChallenge;
	FString ExpectedIssuer;
	FString ExpectedAudience;
	FString ExpectedClientId;
	FString Subject = TEXT("provider-subject-one");
	FDateTime Now = FDateTime(2035, 1, 2, 3, 4, 5);
};

enum class EClaimsMutation : uint8
{
	None,
	WrongIssuer,
	WrongAudience,
	WrongAuthorizedParty,
	WrongNonce,
	Expired,
	FutureNotBefore,
	FutureIssuedAt,
	NotBeforeAfterExpiry,
	ExcessiveLifetime,
	ChangedSubject
};

class FKernelTestCrypto final : public IUnrealAIOAuthAuthorizationCrypto
{
  public:
	FKernelTestCrypto(TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> InFixture,
					  TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> InClock)
		: Fixture(MoveTemp(InFixture)), Clock(MoveTemp(InClock))
	{
	}

	bool GenerateSecureRandomBytes(const int32 NumBytes, TArray<uint8> &OutBytes,
								   FUnrealAIProviderAccessError &OutError) override
	{
		OutError = {};
		OutBytes.SetNumUninitialized(NumBytes);
		for (int32 Index = 0; Index < NumBytes; ++Index)
		{
			OutBytes[Index] = static_cast<uint8>((RandomCall * 67 + Index * 13 + 1) & 0xff);
		}
		++RandomCall;
		if (AdvanceRandomSeconds > 0.0)
		{
			Clock->Advance(FTimespan::FromSeconds(AdvanceRandomSeconds));
			AdvanceRandomSeconds = 0.0;
		}
		return true;
	}

	bool Sha256(const TConstArrayView<uint8> Input, TArray<uint8> &OutDigest,
				FUnrealAIProviderAccessError &OutError) override
	{
		OutError = {};
		if (!ComputePortableSha256(Input, OutDigest))
		{
			OutError = MakeFailure();
			return false;
		}
		return true;
	}

	bool InspectProtectedHeader(const TConstArrayView<uint8> CompactIdToken, FUnrealAIOidcProtectedHeader &OutHeader,
								FUnrealAIProviderAccessError &OutError) override
	{
		++InspectCalls;
		OutError = {};
		if (CompactIdToken.IsEmpty())
		{
			OutError = MakeFailure();
			return false;
		}
		OutHeader.Algorithm = HeaderAlgorithm;
		OutHeader.KeyId = HeaderKeyId;
		return true;
	}

	EUnrealAIOidcTokenVerificationResult VerifyAndDecodeIdToken(const TConstArrayView<uint8> CompactIdToken,
																const FUnrealAIOidcJsonWebKey &Key,
																FUnrealAIOidcVerifiedClaims &OutClaims,
																FUnrealAIProviderAccessError &OutError) override
	{
		++VerifyCalls;
		OutError = {};
		if (CompactIdToken.IsEmpty() || Key.KeyId != HeaderKeyId)
		{
			return EUnrealAIOidcTokenVerificationResult::Malformed;
		}
		if (bRejectEverySignature || (bRejectFirstSignature && VerifyCalls == 1))
		{
			OutError = MakeFailure();
			return EUnrealAIOidcTokenVerificationResult::SignatureRejected;
		}
		OutClaims.Issuer = Fixture->ExpectedIssuer;
		OutClaims.Subject = Fixture->Subject;
		OutClaims.Audiences = {Fixture->ExpectedAudience};
		OutClaims.AuthorizedParty = Fixture->ExpectedClientId;
		OutClaims.Nonce = Fixture->CapturedNonce;
		OutClaims.IssuedAtUtc = Fixture->Now - FTimespan::FromMinutes(1);
		OutClaims.ExpiresAtUtc = Fixture->Now + FTimespan::FromMinutes(10);
		OutClaims.NotBeforeUtc = Fixture->Now - FTimespan::FromMinutes(1);
		switch (Mutation)
		{
		case EClaimsMutation::WrongIssuer:
			OutClaims.Issuer = TEXT("https://attacker.example.test");
			break;
		case EClaimsMutation::WrongAudience:
			OutClaims.Audiences = {TEXT("https://other.example.test/resource")};
			break;
		case EClaimsMutation::WrongAuthorizedParty:
			OutClaims.Audiences.Add(TEXT("https://second.example.test/resource"));
			OutClaims.AuthorizedParty = TEXT("other-client");
			break;
		case EClaimsMutation::WrongNonce:
			OutClaims.Nonce = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
			break;
		case EClaimsMutation::Expired:
			OutClaims.ExpiresAtUtc = Fixture->Now - FTimespan::FromMinutes(5);
			break;
		case EClaimsMutation::FutureNotBefore:
			OutClaims.NotBeforeUtc = Fixture->Now + FTimespan::FromHours(1);
			break;
		case EClaimsMutation::FutureIssuedAt:
			OutClaims.IssuedAtUtc = Fixture->Now + FTimespan::FromHours(1);
			OutClaims.ExpiresAtUtc = Fixture->Now + FTimespan::FromHours(2);
			break;
		case EClaimsMutation::NotBeforeAfterExpiry:
			OutClaims.ExpiresAtUtc = Fixture->Now + FTimespan::FromSeconds(10);
			OutClaims.NotBeforeUtc = Fixture->Now + FTimespan::FromSeconds(20);
			break;
		case EClaimsMutation::ExcessiveLifetime:
			OutClaims.ExpiresAtUtc = OutClaims.IssuedAtUtc + FTimespan::FromHours(25);
			break;
		case EClaimsMutation::ChangedSubject:
			OutClaims.Subject = TEXT("provider-subject-two");
			break;
		case EClaimsMutation::None:
		default:
			break;
		}
		return EUnrealAIOidcTokenVerificationResult::Succeeded;
	}

	void ResetRandomSequence()
	{
		RandomCall = 0;
	}

	TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> Fixture;
	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock;
	FName HeaderAlgorithm = TEXT("RS256");
	FString HeaderKeyId = TEXT("test-key-one");
	EClaimsMutation Mutation = EClaimsMutation::None;
	bool bRejectFirstSignature = false;
	bool bRejectEverySignature = false;
	double AdvanceRandomSeconds = 0.0;
	int32 RandomCall = 0;
	int32 InspectCalls = 0;
	int32 VerifyCalls = 0;
};

class FKernelTestIssuer final : public IUnrealAIOAuthAuthorizationIssuer
{
  public:
	explicit FKernelTestIssuer(TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> InFixture)
		: Fixture(MoveTemp(InFixture))
	{
	}

	bool Discover(const FUnrealAIOAuthAuthorizationOperationContext &Context, const FStringView ExactDiscoveryEndpoint,
				  FUnrealAIOidcDiscoveryDocument &OutDocument, FUnrealAIProviderAccessError &OutError) override
	{
		++DiscoveryCalls;
		LastDiscoveryEndpoint = FString(ExactDiscoveryEndpoint);
		OutError = {};
		if (bFailDiscovery || Context.IsCancellationRequested() || Context.IsTimedOut())
		{
			OutError = MakeFailure();
			return false;
		}
		OutDocument = Discovery;
		return true;
	}

	bool FetchJsonWebKeys(const FUnrealAIOAuthAuthorizationOperationContext &Context,
						  const FStringView ExactJwksEndpoint, FUnrealAIOidcJsonWebKeySet &OutKeySet,
						  FUnrealAIProviderAccessError &OutError) override
	{
		++JwksCalls;
		LastJwksEndpoint = FString(ExactJwksEndpoint);
		OutError = {};
		if (bFailJwks || Context.IsCancellationRequested() || Context.IsTimedOut())
		{
			OutError = MakeFailure();
			return false;
		}
		if (JwksCalls == 1 && FirstKeySet.IsSet())
		{
			OutKeySet = FirstKeySet.GetValue();
		}
		else
		{
			OutKeySet = KeySet;
		}
		return true;
	}

	bool ExchangeAuthorizationCode(const FUnrealAIOAuthAuthorizationOperationContext &Context,
								   const FStringView ExactTokenEndpoint, const TConstArrayView<uint8> FormBody,
								   FUnrealAIOAuthAuthorizationCodeResponse &OutResponse,
								   FUnrealAIProviderAccessError &OutError) override
	{
		++ExchangeCalls;
		LastTokenEndpoint = FString(ExactTokenEndpoint);
		bSawAuthorizationCode = ContainsAscii(FormBody, "code=test-authorization-code");
		bSawPkceVerifier = ContainsAscii(FormBody, "&code_verifier=");
		const FString PkceVerifier = ReadAsciiFormValue(FormBody, "code_verifier=");
		bSawMatchingPkce =
			!PkceVerifier.IsEmpty() && ComputePkceS256Challenge(PkceVerifier) == Fixture->CapturedCodeChallenge;
		bSawExactRedirect = ContainsAscii(FormBody, "redirect_uri=http%3A%2F%2F127.0.0.1%3A43821%2Fcallback");
		OutError = {};
		if (bFailExchange || !bSawMatchingPkce || Context.IsCancellationRequested() || Context.IsTimedOut())
		{
			OutError = MakeFailure();
			return false;
		}
		OutResponse.Reset();
		MakeSecret("test-access-token", OutResponse.Tokens.AccessToken);
		MakeSecret("test-refresh-token", OutResponse.Tokens.RefreshToken);
		MakeSecret("test.compact.id-token", OutResponse.Tokens.IdToken);
		OutResponse.Tokens.AccessTokenExpiresAtUtc = Fixture->Now + FTimespan::FromHours(1);
		OutResponse.TokenType = bWrongTokenType ? FName(TEXT("MAC")) : FName(TEXT("Bearer"));
		OutResponse.GrantedScopes =
			bMissingScope ? TArray<FString>{TEXT("openid")} : TArray<FString>{TEXT("openid"), TEXT("profile")};
		return true;
	}

	bool Revoke(const FUnrealAIOAuthAuthorizationOperationContext &Context, const FStringView ExactRevocationEndpoint,
				const TConstArrayView<uint8> FormBody, FUnrealAIProviderAccessError &OutError) override
	{
		++RevocationCalls;
		LastRevocationEndpoint = FString(ExactRevocationEndpoint);
		bSawRevocationToken = ContainsAscii(FormBody, "token=token-to-revoke");
		OutError = {};
		if (bFailRevocation || Context.IsCancellationRequested() || Context.IsTimedOut())
		{
			OutError = MakeFailure();
			return false;
		}
		return true;
	}

	TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> Fixture;
	FUnrealAIOidcDiscoveryDocument Discovery;
	FUnrealAIOidcJsonWebKeySet KeySet;
	TOptional<FUnrealAIOidcJsonWebKeySet> FirstKeySet;
	bool bFailDiscovery = false;
	bool bFailJwks = false;
	bool bFailExchange = false;
	bool bFailRevocation = false;
	bool bWrongTokenType = false;
	bool bMissingScope = false;
	bool bSawAuthorizationCode = false;
	bool bSawPkceVerifier = false;
	bool bSawMatchingPkce = false;
	bool bSawExactRedirect = false;
	bool bSawRevocationToken = false;
	int32 DiscoveryCalls = 0;
	int32 JwksCalls = 0;
	int32 ExchangeCalls = 0;
	int32 RevocationCalls = 0;
	FString LastDiscoveryEndpoint;
	FString LastJwksEndpoint;
	FString LastTokenEndpoint;
	FString LastRevocationEndpoint;
};

class FKernelTestBrowser final : public IUnrealAIOAuthAuthorizationBrowser
{
  public:
	explicit FKernelTestBrowser(TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> InFixture,
								TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> InClock)
		: Fixture(MoveTemp(InFixture)), Clock(MoveTemp(InClock))
	{
	}

	bool Authorize(const FUnrealAIOAuthAuthorizationOperationContext &Context,
				   const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch,
				   FUnrealAIOAuthBrowserAuthorizationCallback &OutCallback,
				   FUnrealAIProviderAccessError &OutError) override
	{
		++Calls;
		OutError = {};
		bSawS256 =
			Launch.AuthorizationUrl.Contains(TEXT("code_challenge_method=S256"), ESearchCase::CaseSensitive) &&
											 !ReadQueryValue(Launch.AuthorizationUrl, TEXT("code_challenge")).IsEmpty();
		Fixture->CapturedCodeChallenge = bCorruptPkceAssociation
			? FString::ChrN(43, TEXT('A')) : ReadQueryValue(Launch.AuthorizationUrl, TEXT("code_challenge"));
		Fixture->CapturedState = ReadQueryValue(Launch.AuthorizationUrl, TEXT("state"));
		Fixture->CapturedNonce = ReadQueryValue(Launch.AuthorizationUrl, TEXT("nonce"));
		bSawExpectedState = Launch.ExpectedState == Fixture->CapturedState;
		if (AdvanceSeconds > 0.0)
		{
			Clock->Advance(FTimespan::FromSeconds(AdvanceSeconds));
		}
		if (bFail || Context.IsCancellationRequested() || Context.IsTimedOut())
		{
			OutError = MakeFailure();
			return false;
		}
		OutCallback.Reset();
		OutCallback.Kind =
			bDenied ? EUnrealAIOAuthBrowserCallbackKind::Denied : EUnrealAIOAuthBrowserCallbackKind::AuthorizationCode;
		OutCallback.ExactRedirectUri =
			bWrongRedirect ? TEXT("http://127.0.0.1:43822/callback") : Launch.ExactRedirectUri;
		OutCallback.Issuer = bWrongIssuer ? TEXT("https://attacker.example.test") : Launch.ExpectedIssuer;
		OutCallback.State = bWrongState ? TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") : Fixture->CapturedState;
		if (!bDenied)
		{
			MakeSecret("test-authorization-code", OutCallback.AuthorizationCode);
		}
		return true;
	}

	TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> Fixture;
	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock;
	bool bFail = false;
	bool bDenied = false;
	bool bWrongState = false;
	bool bWrongIssuer = false;
	bool bWrongRedirect = false;
	bool bCorruptPkceAssociation = false;
	double AdvanceSeconds = 0.0;
	bool bSawS256 = false;
	bool bSawExpectedState = false;
	int32 Calls = 0;
};

FUnrealAIOAuthTrustedAuthorizationServer MakeServer()
{
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	Server.Issuer = TEXT("https://issuer.example.test");
	Server.DiscoveryEndpoint = TEXT("https://issuer.example.test/.well-known/openid-configuration");
	Server.AuthorizationEndpoint = TEXT("https://issuer.example.test/oauth2/authorize");
	Server.TokenEndpoint = TEXT("https://issuer.example.test/oauth2/token");
	Server.JwksEndpoint = TEXT("https://issuer.example.test/oauth2/jwks");
	Server.RevocationEndpoint = TEXT("https://issuer.example.test/oauth2/revoke");
	Server.AllowedSigningAlgorithms = {FName(TEXT("RS256"))};
	return Server;
}

FUnrealAIOidcDiscoveryDocument MakeDiscovery(const FUnrealAIOAuthTrustedAuthorizationServer &Server)
{
	FUnrealAIOidcDiscoveryDocument Discovery;
	Discovery.Issuer = Server.Issuer;
	Discovery.AuthorizationEndpoint = Server.AuthorizationEndpoint;
	Discovery.TokenEndpoint = Server.TokenEndpoint;
	Discovery.JwksEndpoint = Server.JwksEndpoint;
	Discovery.RevocationEndpoint = Server.RevocationEndpoint;
	Discovery.bPkceS256Supported = true;
	return Discovery;
}

FUnrealAIOidcJsonWebKeySet MakeKeySet(const FString &KeyId)
{
	FUnrealAIOidcJsonWebKey Key;
	Key.KeyId = KeyId;
	Key.KeyType = TEXT("RSA");
	Key.Use = TEXT("sig");
	Key.Algorithm = TEXT("RS256");
	Key.PublicKeyMaterial = {1, 2, 3, 4};
	FUnrealAIOidcJsonWebKeySet Set;
	Set.Keys.Add(MoveTemp(Key));
	Set.MaxAgeSeconds = 600.0;
	return Set;
}

FUnrealAIOAuthBrowserAuthorizationRequest MakeRequest(const FUnrealAIOAuthTrustedAuthorizationServer &Server)
{
	FUnrealAIOAuthBrowserAuthorizationRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.Server = Server;
	Request.ClientId = TEXT("test-public-client");
	Request.Audience = TEXT("https://api.example.test/resource");
	Request.ExactRedirectUri = TEXT("http://127.0.0.1:43821/callback");
	Request.RequestedScopes = {TEXT("openid"), TEXT("profile")};
	Request.TimeoutSeconds = 30.0;
	Request.ClockSkewSeconds = 30.0;
	Request.MaxIdentityTokenAgeSeconds = 900.0;
	Request.MaxJwksCacheAgeSeconds = 600.0;
	return Request;
}

struct FKernelFixture final
{
	FKernelFixture()
	{
		Server = MakeServer();
		Request = MakeRequest(Server);
		State->ExpectedIssuer = Server.Issuer;
		State->ExpectedAudience = Request.Audience;
		State->ExpectedClientId = Request.ClientId;
		Clock = MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(State->Now, 100.0);
		Crypto = MakeShared<FKernelTestCrypto, ESPMode::ThreadSafe>(State, Clock.ToSharedRef());
		Issuer = MakeShared<FKernelTestIssuer, ESPMode::ThreadSafe>(State);
		Issuer->Discovery = MakeDiscovery(Server);
		Issuer->KeySet = MakeKeySet(Crypto->HeaderKeyId);
		Browser = MakeShared<FKernelTestBrowser, ESPMode::ThreadSafe>(State, Clock.ToSharedRef());
		Kernel = MakeUnique<FUnrealAIOAuthAuthorizationKernel>(Crypto.ToSharedRef(), Issuer.ToSharedRef(),
															   Browser.ToSharedRef(), Clock.ToSharedRef());
	}

	FUnrealAICancellationSource NewCancellation() const
	{
		return FUnrealAICancellationSource();
	}

	TSharedRef<FKernelFixtureState, ESPMode::ThreadSafe> State = MakeShared<FKernelFixtureState, ESPMode::ThreadSafe>();
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	FUnrealAIOAuthBrowserAuthorizationRequest Request;
	TSharedPtr<FUnrealAITestClock, ESPMode::ThreadSafe> Clock;
	TSharedPtr<FKernelTestCrypto, ESPMode::ThreadSafe> Crypto;
	TSharedPtr<FKernelTestIssuer, ESPMode::ThreadSafe> Issuer;
	TSharedPtr<FKernelTestBrowser, ESPMode::ThreadSafe> Browser;
	TUniquePtr<FUnrealAIOAuthAuthorizationKernel> Kernel;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthAuthorizationKernelHappyPathTest,
								 "UnrealAI.Auth.OAuthOIDC.BrowserPkceClaimsCacheAndRevocation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthAuthorizationKernelHappyPathTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("PKCE S256 matches the RFC 7636 known vector"),
				   ComputePkceS256Challenge(TEXT("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
											FString(TEXT("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")));
	FKernelFixture Fixture;
	FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
	FUnrealAIOAuthAuthorizationResult Result;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("browser PKCE/OIDC succeeds"),
				  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
	TestFalse(TEXT("success has no public-safe error"), Error.IsError());
	TestTrue(TEXT("access token returned"), Result.Tokens.AccessToken.IsSet());
	TestTrue(TEXT("refresh token returned"), Result.Tokens.RefreshToken.IsSet());
	TestTrue(TEXT("ID token retained after validation"), Result.Tokens.IdToken.IsSet());
	TestEqual(TEXT("opaque subject fingerprint is SHA-256 base64url width"), Result.SubjectFingerprint.Len(), 43);
	TestTrue(TEXT("browser launch used S256"), Fixture.Browser->bSawS256);
	TestTrue(TEXT("browser launch carries the exact ephemeral state binding"), Fixture.Browser->bSawExpectedState);
	TestTrue(TEXT("token exchange included authorization code"), Fixture.Issuer->bSawAuthorizationCode);
	TestTrue(TEXT("token exchange included verifier"), Fixture.Issuer->bSawPkceVerifier);
	TestTrue(TEXT("fake issuer independently recomputed and accepted the S256 verifier"),
				  Fixture.Issuer->bSawMatchingPkce);
	TestTrue(TEXT("token exchange included exact redirect"), Fixture.Issuer->bSawExactRedirect);
	TestEqual(TEXT("discovery target is exact"), Fixture.Issuer->LastDiscoveryEndpoint,
				   Fixture.Server.DiscoveryEndpoint);
	TestEqual(TEXT("token target is exact"), Fixture.Issuer->LastTokenEndpoint, Fixture.Server.TokenEndpoint);
	TestEqual(TEXT("JWKS target is exact"), Fixture.Issuer->LastJwksEndpoint, Fixture.Server.JwksEndpoint);

	FUnrealAISecretValue RevocationToken;
	MakeSecret("token-to-revoke", RevocationToken);
	FUnrealAIOAuthRevocationRequest Revocation;
	Revocation.RequestId.Value = FGuid::NewGuid();
	Revocation.Server = Fixture.Server;
	Revocation.ClientId = FString(Fixture.Request.ClientId);
	Revocation.TimeoutSeconds = 10.0;
	Cancellation = Fixture.NewCancellation();
	Error = {};
	TestTrue(TEXT("bounded revocation succeeds"),
				  Fixture.Kernel->Revoke(Revocation, MoveTemp(RevocationToken), Cancellation.GetToken(), Error));
	TestTrue(TEXT("revocation saw exact token body"), Fixture.Issuer->bSawRevocationToken);
	TestEqual(TEXT("revocation target is exact"), Fixture.Issuer->LastRevocationEndpoint,
				   Fixture.Server.RevocationEndpoint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIOAuthAuthorizationKernelClaimFaultsTest,
	"UnrealAI.Auth.OAuthOIDC.StrictCallbackSignatureIssuerAudienceAzpNonceTimeSubjectScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthAuthorizationKernelClaimFaultsTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TArray<EClaimsMutation> Mutations = {EClaimsMutation::WrongIssuer,
											   EClaimsMutation::WrongAudience,
											   EClaimsMutation::WrongAuthorizedParty,
											   EClaimsMutation::WrongNonce,
											   EClaimsMutation::Expired,
											   EClaimsMutation::FutureNotBefore,
											   EClaimsMutation::FutureIssuedAt,
											   EClaimsMutation::NotBeforeAfterExpiry,
											   EClaimsMutation::ExcessiveLifetime};
	for (const EClaimsMutation Mutation : Mutations)
	{
		FKernelFixture Fixture;
		Fixture.Crypto->Mutation = Mutation;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("mutated verified claim fails closed"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestTrue(TEXT("claim failure is typed"), Error.IsError());
		TestFalse(TEXT("claim failure returns no token"), Result.Tokens.AccessToken.IsSet());
	}

	{
		FKernelFixture Fixture;
		Fixture.Browser->bCorruptPkceAssociation = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("a verifier that does not match the authorization-time S256 challenge is rejected"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("PKCE mismatch reaches exactly one rejecting token exchange"), Fixture.Issuer->ExchangeCalls, 1);
		TestFalse(TEXT("fake issuer records the S256 mismatch"), Fixture.Issuer->bSawMatchingPkce);
	}
	{
		FKernelFixture Fixture;
		Fixture.Browser->bWrongState = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("wrong state fails before exchange"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("state mismatch never reaches token endpoint"), Fixture.Issuer->ExchangeCalls, 0);
	}
	{
		FKernelFixture Fixture;
		Fixture.Browser->bDenied = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("valid provider denial fails authorization"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("valid provider denial is distinguished from a malformed callback"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthFailed);
		TestEqual(TEXT("valid provider denial stops before token exchange"), Fixture.Issuer->ExchangeCalls, 0);
		Fixture.Crypto->ResetRandomSequence();
		Fixture.Browser->bDenied = false;
		Fixture.Request.RequestId.Value = FGuid::NewGuid();
		FUnrealAICancellationSource ReplayCancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult ReplayResult;
		Error = {};
		TestFalse(TEXT("a state consumed by denial cannot later authorize"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, ReplayCancellation.GetToken(),
															ReplayResult, Error));
		TestEqual(TEXT("denial-state replay never reaches token exchange"), Fixture.Issuer->ExchangeCalls, 0);
	}
	{
		FKernelFixture Fixture;
		Fixture.Browser->bWrongRedirect = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("wrong redirect fails closed"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
	}
	{
		FKernelFixture Fixture;
		Fixture.Browser->bWrongIssuer = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("wrong callback issuer fails closed"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
	}
	{
		FKernelFixture Fixture;
		Fixture.Issuer->Discovery.JwksEndpoint = TEXT("https://attacker.example.test/jwks");
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("discovery cannot redirect JWKS authority"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("mismatched discovery stops before browser"), Fixture.Browser->Calls, 0);
	}
	{
		FKernelFixture Fixture;
		Fixture.Crypto->HeaderAlgorithm = TEXT("ES256");
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("unapproved protected algorithm fails closed"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
	}
	{
		FKernelFixture Fixture;
		Fixture.Crypto->bRejectEverySignature = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("signature rejection fails after one rotation retry"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("signature rejection fetches JWKS at most twice"), Fixture.Issuer->JwksCalls, 2);
	}
	{
		FKernelFixture Fixture;
		Fixture.Issuer->bMissingScope = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("scope loss fails closed"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("scope loss is typed"), Error.Code, EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient);
	}

	FString PriorSubject;
	{
		FKernelFixture Fixture;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("baseline subject authorization succeeds"),
					  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		PriorSubject = Result.SubjectFingerprint;
	}
	{
		FKernelFixture Fixture;
		Fixture.Request.ExpectedSubjectFingerprint = PriorSubject;
		Fixture.Crypto->Mutation = EClaimsMutation::ChangedSubject;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("subject continuity mismatch requires reauthorization"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthAuthorizationKernelRaceBoundsTest,
								 "UnrealAI.Auth.OAuthOIDC.JwksRotationReplayCancellationTimeoutTerminalOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthAuthorizationKernelRaceBoundsTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		FKernelFixture Fixture;
		Fixture.Issuer->FirstKeySet = MakeKeySet(TEXT("retired-key"));
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("unknown kid forces one JWKS rotation fetch"),
					  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("rotation fetched exactly twice"), Fixture.Issuer->JwksCalls, 2);
		TestEqual(TEXT("signature verified exactly once after key selection"), Fixture.Crypto->VerifyCalls, 1);
	}
	{
		FKernelFixture Fixture;
		Fixture.Crypto->bRejectFirstSignature = true;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("rotated signature succeeds after one forced refresh"),
					  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestFalse(TEXT("successful rotation clears prior seam error"), Error.IsError());
		TestEqual(TEXT("rotated signature fetches JWKS exactly twice"), Fixture.Issuer->JwksCalls, 2);
		TestEqual(TEXT("rotated signature verifies exactly twice"), Fixture.Crypto->VerifyCalls, 2);
	}
	{
		FKernelFixture Fixture;
		FUnrealAICancellationSource FirstCancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult FirstResult;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("cache policy baseline authorization succeeds"),
					  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, FirstCancellation.GetToken(), FirstResult,
														   Error));
		Fixture.Request.RequestId.Value = FGuid::NewGuid();
		Fixture.Request.MaxJwksCacheAgeSeconds = 0.0;
		FUnrealAICancellationSource SecondCancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult SecondResult;
		TestTrue(TEXT("zero-cache authorization succeeds"),
					  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, SecondCancellation.GetToken(), SecondResult,
														   Error));
		TestEqual(TEXT("later zero-cache policy bypasses earlier cached JWKS"), Fixture.Issuer->JwksCalls, 2);
	}
	{
		FKernelFixture Fixture;
		FUnrealAICancellationSource FirstCancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult FirstResult;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("first deterministic transaction succeeds"),
					  Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, FirstCancellation.GetToken(), FirstResult,
														   Error));
		Fixture.Crypto->ResetRandomSequence();
		Fixture.Request.RequestId.Value = FGuid::NewGuid();
		FUnrealAICancellationSource SecondCancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult SecondResult;
		Error = {};
		TestFalse(TEXT("replayed callback state is fenced"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, SecondCancellation.GetToken(),
															SecondResult, Error));
		TestEqual(TEXT("replay stops before second token exchange"), Fixture.Issuer->ExchangeCalls, 1);
	}
	{
		FKernelFixture Fixture;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("pre-cancelled request fails before discovery"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("cancellation is typed"), Error.Code, EUnrealAIProviderAccessErrorCode::AuthCancelled);
		TestEqual(TEXT("pre-cancellation made no issuer call"), Fixture.Issuer->DiscoveryCalls, 0);
	}
	{
		FKernelFixture Fixture;
		Fixture.Request.TimeoutSeconds = 1.0;
		Fixture.Crypto->AdvanceRandomSeconds = 2.0;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("deadline crossed by random seam fails once"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("late random result is typed timeout"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthTimedOut);
		TestEqual(TEXT("late random result never launches browser"), Fixture.Browser->Calls, 0);
	}
	{
		FKernelFixture Fixture;
		Fixture.Request.TimeoutSeconds = 1.0;
		Fixture.Browser->AdvanceSeconds = 2.0;
		FUnrealAICancellationSource Cancellation = Fixture.NewCancellation();
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("deadline crossed by browser result fails once"),
					   Fixture.Kernel->AuthorizeBrowserPkce(Fixture.Request, Cancellation.GetToken(), Result, Error));
		TestEqual(TEXT("timeout is typed"), Error.Code, EUnrealAIProviderAccessErrorCode::AuthTimedOut);
		TestEqual(TEXT("late browser result never exchanges token"), Fixture.Issuer->ExchangeCalls, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
