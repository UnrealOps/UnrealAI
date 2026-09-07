// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIOAuthAuthorizationIssuerHttp.h"
#include "Auth/UnrealAIOAuthOpenSslAuthorizationCrypto.h"
#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Runtime/UnrealAICancellation.h"
#include "Runtime/UnrealAIClock.h"
#include "Transport/UnrealAIOAuthIssuerHttpClient.h"

#if UNREALAI_OAUTH_OPENSSL
#define UI OPENSSL_UI
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#undef UI
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace AgentOAuthProductionIssuerJoseTests
{
void SecureResetTestBytes(TArray<uint8> &Bytes)
{
	if (!Bytes.IsEmpty())
	{
		FMemory::Memzero(Bytes.GetData(), Bytes.Max());
	}
	Bytes.Empty();
}

TArray<uint8> Utf8Bytes(const FString &Value)
{
	FTCHARToUTF8 Utf8(*Value);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

FString Base64Url(const TConstArrayView<uint8> Bytes)
{
	FString Encoded = FBase64::Encode(Bytes.GetData(), static_cast<uint32>(Bytes.Num()), EBase64Mode::Standard);
	Encoded.ReplaceInline(TEXT("+"), TEXT("-"), ESearchCase::CaseSensitive);
	Encoded.ReplaceInline(TEXT("/"), TEXT("_"), ESearchCase::CaseSensitive);
	while (Encoded.EndsWith(TEXT("="), ESearchCase::CaseSensitive))
	{
		Encoded.LeftChopInline(1, EAllowShrinking::No);
	}
	return Encoded;
}

struct FScriptedHttpResponse final
{
	int32 StatusCode = 200;
	FString ContentType = TEXT("application/json; charset=utf-8");
	FString CacheControl;
	FString Body;
};

class FScriptedIssuerHttpClient final : public IUnrealAIOAuthIssuerHttpClient
{
  public:
	FScriptedHttpResponse Next;
	int32 Calls = 0;
	EUnrealAIOAuthIssuerHttpMethod LastMethod = EUnrealAIOAuthIssuerHttpMethod::Invalid;
	FString LastUrl;
	TArray<uint8> LastFormBody;

	~FScriptedIssuerHttpClient() override
	{
		SecureResetTestBytes(LastFormBody);
	}

	bool Execute(const FUnrealAIOAuthAuthorizationOperationContext &, const FUnrealAIOAuthIssuerHttpRequest &Request,
				 FUnrealAIOAuthIssuerHttpResponse &OutResponse, FUnrealAIProviderAccessError &OutError) override
	{
		++Calls;
		LastMethod = Request.Method;
		LastUrl = Request.ExactUrl;
		SecureResetTestBytes(LastFormBody);
		LastFormBody.Append(Request.FormBody);
		OutResponse.Reset();
		OutResponse.StatusCode = Next.StatusCode;
		OutResponse.ExactEffectiveUrl = Request.ExactUrl;
		OutResponse.ContentType = Next.ContentType;
		OutResponse.CacheControl = Next.CacheControl;
		OutResponse.Body = Utf8Bytes(Next.Body);
		OutError = {};
		return true;
	}
};

bool MakeContext(const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> &Clock,
				 const FUnrealAICancellationToken &Cancellation,
				 FUnrealAIOAuthAuthorizationOperationContext &OutContext)
{
	FString Error;
	return FUnrealAIOAuthAuthorizationOperationContext::TryCreate(Clock, 30.0, Cancellation, OutContext, Error);
}

#if UNREALAI_OAUTH_OPENSSL
EVP_PKEY *GenerateKey(const FName Algorithm)
{
	const FString Name = Algorithm.ToString();
	EVP_PKEY_CTX *Context = nullptr;
	if (Name == TEXT("RS256") || Name == TEXT("PS256"))
	{
		Context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
		if (Context == nullptr || EVP_PKEY_keygen_init(Context) != 1 ||
			EVP_PKEY_CTX_set_rsa_keygen_bits(Context, 2048) != 1)
		{
			EVP_PKEY_CTX_free(Context);
			return nullptr;
		}
	}
	else if (Name == TEXT("ES256"))
	{
		Context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		if (Context == nullptr || EVP_PKEY_keygen_init(Context) != 1 ||
			EVP_PKEY_CTX_set_ec_paramgen_curve_nid(Context, NID_X9_62_prime256v1) != 1)
		{
			EVP_PKEY_CTX_free(Context);
			return nullptr;
		}
	}
	else
	{
		Context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
		if (Context == nullptr || EVP_PKEY_keygen_init(Context) != 1)
		{
			EVP_PKEY_CTX_free(Context);
			return nullptr;
		}
	}
	EVP_PKEY *Key = nullptr;
	const bool bGenerated = EVP_PKEY_keygen(Context, &Key) == 1;
	EVP_PKEY_CTX_free(Context);
	return bGenerated ? Key : nullptr;
}

bool PublicKeyDer(EVP_PKEY *Key, TArray<uint8> &OutDer)
{
	SecureResetTestBytes(OutDer);
	const int32 Length = i2d_PUBKEY(Key, nullptr);
	if (Length <= 0)
	{
		return false;
	}
	OutDer.SetNumUninitialized(Length);
	unsigned char *Cursor = OutDer.GetData();
	return i2d_PUBKEY(Key, &Cursor) == Length;
}

bool RsaJwk(EVP_PKEY *Key, FString &OutJwk)
{
	const RSA *Rsa = EVP_PKEY_get0_RSA(Key);
	const BIGNUM *N = nullptr;
	const BIGNUM *E = nullptr;
	RSA_get0_key(Rsa, &N, &E, nullptr);
	if (N == nullptr || E == nullptr)
	{
		return false;
	}
	TArray<uint8> NBytes;
	TArray<uint8> EBytes;
	ON_SCOPE_EXIT
	{
		SecureResetTestBytes(NBytes);
		SecureResetTestBytes(EBytes);
	};
	NBytes.SetNumUninitialized(BN_num_bytes(N));
	EBytes.SetNumUninitialized(BN_num_bytes(E));
	if (BN_bn2bin(N, NBytes.GetData()) != NBytes.Num() || BN_bn2bin(E, EBytes.GetData()) != EBytes.Num())
	{
		return false;
	}
	OutJwk = FString::Printf(
		TEXT("{\"kty\":\"RSA\",\"use\":\"sig\",\"alg\":\"RS256\",\"kid\":\"rsa-one\",\"n\":\"%s\",\"e\":\"%s\"}"),
			 *Base64Url(NBytes), *Base64Url(EBytes));
	return true;
}

bool SignCompact(EVP_PKEY *Key, const FName Algorithm, const FString &HeaderJson, const FString &ClaimsJson,
				 FString &OutCompact)
{
	TArray<uint8> HeaderBytes = Utf8Bytes(HeaderJson);
	TArray<uint8> ClaimsBytes = Utf8Bytes(ClaimsJson);
	const FString SigningText = Base64Url(HeaderBytes) + TEXT(".") + Base64Url(ClaimsBytes);
	TArray<uint8> SigningBytes = Utf8Bytes(SigningText);
	TArray<uint8> Signature;
	ON_SCOPE_EXIT
	{
		SecureResetTestBytes(HeaderBytes);
		SecureResetTestBytes(ClaimsBytes);
		SecureResetTestBytes(SigningBytes);
		SecureResetTestBytes(Signature);
	};

	EVP_MD_CTX *Context = EVP_MD_CTX_new();
	if (Context == nullptr)
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		EVP_MD_CTX_free(Context);
	};
	EVP_PKEY_CTX *KeyContext = nullptr;
	const FString Name = Algorithm.ToString();
	if (Name == TEXT("EdDSA"))
	{
		SIZE_T Length = 0;
		if (EVP_DigestSignInit(Context, &KeyContext, nullptr, nullptr, Key) != 1 ||
			EVP_DigestSign(Context, nullptr, &Length, SigningBytes.GetData(), SigningBytes.Num()) != 1 || Length <= 0 ||
			Length > 256)
		{
			return false;
		}
		Signature.SetNumUninitialized(static_cast<int32>(Length));
		if (EVP_DigestSign(Context, Signature.GetData(), &Length, SigningBytes.GetData(), SigningBytes.Num()) != 1)
		{
			return false;
		}
		Signature.SetNum(static_cast<int32>(Length), EAllowShrinking::No);
	}
	else
	{
		if (EVP_DigestSignInit(Context, &KeyContext, EVP_sha256(), nullptr, Key) != 1 || KeyContext == nullptr)
		{
			return false;
		}
		if (Name == TEXT("RS256") && EVP_PKEY_CTX_set_rsa_padding(KeyContext, RSA_PKCS1_PADDING) <= 0)
		{
			return false;
		}
		if (Name == TEXT("PS256") && (EVP_PKEY_CTX_set_rsa_padding(KeyContext, RSA_PKCS1_PSS_PADDING) <= 0 ||
									  EVP_PKEY_CTX_set_rsa_mgf1_md(KeyContext, EVP_sha256()) <= 0 ||
									  EVP_PKEY_CTX_set_rsa_pss_saltlen(KeyContext, SHA256_DIGEST_LENGTH) <= 0))
		{
			return false;
		}
		SIZE_T Length = 0;
		if (EVP_DigestSignUpdate(Context, SigningBytes.GetData(), SigningBytes.Num()) != 1 ||
			EVP_DigestSignFinal(Context, nullptr, &Length) != 1 || Length <= 0 || Length > 2048)
		{
			return false;
		}
		Signature.SetNumUninitialized(static_cast<int32>(Length));
		if (EVP_DigestSignFinal(Context, Signature.GetData(), &Length) != 1)
		{
			return false;
		}
		Signature.SetNum(static_cast<int32>(Length), EAllowShrinking::No);
	}

	if (Name == TEXT("ES256"))
	{
		const unsigned char *Cursor = Signature.GetData();
		ECDSA_SIG *EcSignature = d2i_ECDSA_SIG(nullptr, &Cursor, Signature.Num());
		if (EcSignature == nullptr || Cursor != Signature.GetData() + Signature.Num())
		{
			ECDSA_SIG_free(EcSignature);
			return false;
		}
		const BIGNUM *R = nullptr;
		const BIGNUM *S = nullptr;
		ECDSA_SIG_get0(EcSignature, &R, &S);
		TArray<uint8> Raw;
		Raw.SetNumZeroed(64);
		const bool bConverted =
			BN_bn2binpad(R, Raw.GetData(), 32) == 32 && BN_bn2binpad(S, Raw.GetData() + 32, 32) == 32;
		ECDSA_SIG_free(EcSignature);
		if (!bConverted)
		{
			SecureResetTestBytes(Raw);
			return false;
		}
		SecureResetTestBytes(Signature);
		Signature = MoveTemp(Raw);
	}
	OutCompact = SigningText + TEXT(".") + Base64Url(Signature);
	return true;
}

FUnrealAIOidcJsonWebKey MakeVerificationKey(EVP_PKEY *Key, const FName Algorithm)
{
	FUnrealAIOidcJsonWebKey Result;
	Result.KeyId = TEXT("fixture-key");
	Result.Algorithm = Algorithm;
	const FString Name = Algorithm.ToString();
	Result.KeyType =
		Name == TEXT("ES256") ? FName(TEXT("EC")) : Name == TEXT("EdDSA") ? FName(TEXT("OKP")) : FName(TEXT("RSA"));
	Result.Use = FName(TEXT("sig"));
	PublicKeyDer(Key, Result.PublicKeyMaterial);
	return Result;
}
#endif
} // namespace AgentOAuthProductionIssuerJoseTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthProductionIssuerWireTest,
								 "UnrealAI.Auth.OAuthOIDC.ProductionIssuer.StrictDiscoveryJwksTokenRevocation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthProductionIssuerWireTest::RunTest(const FString &Parameters)
{
	using namespace AgentOAuthProductionIssuerJoseTests;
#if !UNREALAI_OAUTH_OPENSSL
	AddInfo(TEXT("Engine OpenSSL is unavailable on this target; production issuer JWK conversion fails closed."));
	return true;
#else
	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5));
	TSharedRef<FScriptedIssuerHttpClient, ESPMode::ThreadSafe> Http =
		MakeShared<FScriptedIssuerHttpClient, ESPMode::ThreadSafe>();
	FUnrealAIOAuthAuthorizationIssuerHttp Issuer(Http, Clock);
	FUnrealAICancellationSource Cancellation;
	FUnrealAIOAuthAuthorizationOperationContext Context;
	TestTrue(TEXT("A bounded operation context is created"), MakeContext(Clock, Cancellation.GetToken(), Context));

	Http->Next.Body = TEXT("{\"issuer\":\"https://issuer.example.test\",\"authorization_endpoint\":\"https://issuer.example.test/authorize\",") TEXT("\"token_endpoint\":\"https://issuer.example.test/token\",\"jwks_uri\":\"https://issuer.example.test/jwks\",")
		TEXT("\"revocation_endpoint\":\"https://issuer.example.test/revoke\",\"code_challenge_methods_supported\":[\"S256\"]}");
	FUnrealAIOidcDiscoveryDocument Discovery;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("Strict discovery is accepted"),
				  Issuer.Discover(Context, TEXT("https://issuer.example.test/.well-known/openid-configuration"),
												Discovery, Error));
	TestTrue(TEXT("Discovery explicitly admits PKCE S256"), Discovery.bPkceS256Supported);
	TestEqual(TEXT("Discovery request is exact GET"), Http->LastMethod, EUnrealAIOAuthIssuerHttpMethod::Get);

	EVP_PKEY *RsaKey = GenerateKey(FName(TEXT("RS256")));
	TestNotNull(TEXT("RSA test key is generated"), RsaKey);
	ON_SCOPE_EXIT
	{
		EVP_PKEY_free(RsaKey);
	};
	FString Jwk;
	TestTrue(TEXT("RSA public JWK is encoded"), RsaKey != nullptr && RsaJwk(RsaKey, Jwk));
	Http->Next.Body = FString::Printf(TEXT("{\"keys\":[%s]}"), *Jwk);
	Http->Next.CacheControl = TEXT("public, max-age=3600");
	FUnrealAIOidcJsonWebKeySet Keys;
	TestTrue(TEXT("A strict public JWKS becomes canonical DER SPKI"),
				  Issuer.FetchJsonWebKeys(Context, TEXT("https://issuer.example.test/jwks"), Keys, Error));
	TestEqual(TEXT("Exactly one key is admitted"), Keys.Keys.Num(), 1);
	TestEqual(TEXT("Cache max-age is bounded and projected"), Keys.MaxAgeSeconds, 3600.0);
	TestTrue(TEXT("Canonical public material is non-empty"),
				  Keys.Keys.Num() == 1 && !Keys.Keys[0].PublicKeyMaterial.IsEmpty());

	Http->Next.Body =
		TEXT("{\"access_token\":\"access-secret\",\"refresh_token\":\"refresh-secret\",\"id_token\":\"header.payload.signature\",") TEXT("\"token_type\":\"Bearer\",\"expires_in\":3600,\"scope\":\"openid profile\"}");
	Http->Next.CacheControl.Reset();
	const TArray<uint8> FormBody = Utf8Bytes(TEXT("grant_type=authorization_code&code=secret"));
	FUnrealAIOAuthAuthorizationCodeResponse TokenResponse;
	TestTrue(TEXT("A bounded token response is parsed into move-only secrets"),
				  Issuer.ExchangeAuthorizationCode(Context, TEXT("https://issuer.example.test/token"), FormBody,
																 TokenResponse, Error));
	TestEqual(TEXT("Token exchange uses exact form POST"), Http->LastMethod, EUnrealAIOAuthIssuerHttpMethod::PostForm);
	TestEqual(TEXT("The secret form is passed without string conversion"), Http->LastFormBody, FormBody);
	TestEqual(TEXT("Granted scopes remain exact"), TokenResponse.GrantedScopes.Num(), 2);

	Http->Next.StatusCode = 204;
	Http->Next.ContentType.Reset();
	Http->Next.Body.Reset();
	TestTrue(TEXT("A successful empty RFC 7009 response is accepted"),
				  Issuer.Revoke(Context, TEXT("https://issuer.example.test/revoke"), FormBody, Error));

	Http->Next.StatusCode = 200;
	Http->Next.ContentType = TEXT("application/json");
	Http->Next.Body = TEXT("{\"issuer\":\"https://issuer.example.test\",\"issuer\":\"https://attacker.example.test\"}");
	TestFalse(TEXT("Duplicate discovery members fail closed"),
				   Issuer.Discover(Context, TEXT("https://issuer.example.test/.well-known/openid-configuration"),
												 Discovery, Error));
	TestEqual(TEXT("Duplicate JSON has a closed protocol error"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);

	Http->Next.Body =
		FString::Printf(TEXT("{\"keys\":[%s]}"), *Jwk.Replace(TEXT("\"n\":"), TEXT("\"d\":\"private\",\"n\":")));
	TestFalse(TEXT("Any private JWK parameter fails the whole set closed"),
				   Issuer.FetchJsonWebKeys(Context, TEXT("https://issuer.example.test/jwks"), Keys, Error));

	Http->Next.Body = FString::Printf(TEXT("{\"keys\":[%s]}"),
										   *Jwk.Replace(TEXT("\"n\":"), TEXT("\"key_ops\":[\"encrypt\"],\"n\":")));
	TestFalse(TEXT("A JWK with incompatible key operations fails closed"),
				   Issuer.FetchJsonWebKeys(Context, TEXT("https://issuer.example.test/jwks"), Keys, Error));

	Http->Next.StatusCode = 302;
	Http->Next.Body.Reset();
	TestFalse(TEXT("Redirect responses never reach discovery parsing"),
				   Issuer.Discover(Context, TEXT("https://issuer.example.test/.well-known/openid-configuration"),
												 Discovery, Error));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthProductionJoseProfilesTest,
								 "UnrealAI.Auth.OAuthOIDC.ProductionJOSE.StrictProfilesAndSignatureBeforeClaims",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthProductionJoseProfilesTest::RunTest(const FString &Parameters)
{
	using namespace AgentOAuthProductionIssuerJoseTests;
	FUnrealAIOAuthOpenSslAuthorizationCrypto Crypto;
#if !UNREALAI_OAUTH_OPENSSL
	TestFalse(TEXT("Unsupported targets advertise fail-closed JOSE"), Crypto.IsPlatformSupported());
	return true;
#else
	TestTrue(TEXT("This target has the engine OpenSSL JOSE backend"), Crypto.IsPlatformSupported());
	FUnrealAIProviderAccessError ShaError;
	TArray<uint8> ShaDigest;
	const TArray<uint8> ShaInput = Utf8Bytes(TEXT("abc"));
	TestTrue(TEXT("Production OAuth SHA-256 accepts the known-vector input"),
				  Crypto.Sha256(ShaInput, ShaDigest, ShaError));
	TestEqual(TEXT("Production OAuth SHA-256 matches the external abc vector"), FBase64::Encode(ShaDigest),
				   FString(TEXT("ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=")));
	const FString Claims =
		TEXT("{\"iss\":\"https://issuer.example.test\",\"sub\":\"provider-subject\",\"aud\":[\"client-one\"],")
			TEXT("\"azp\":\"client-one\",\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",")
				TEXT("\"exp\":2051312400,\"iat\":2051308800,\"nbf\":2051308790}");
	const TArray<FName> Algorithms = {FName(TEXT("RS256")), FName(TEXT("PS256")), FName(TEXT("ES256")),
																						FName(TEXT("EdDSA"))};
	for (const FName Algorithm : Algorithms)
	{
		EVP_PKEY *KeyPair = GenerateKey(Algorithm);
		TestNotNull(*FString::Printf(TEXT("%s signing key is generated"), *Algorithm.ToString()), KeyPair);
		if (KeyPair == nullptr)
		{
			continue;
		}
		FUnrealAIOidcJsonWebKey PublicKey = MakeVerificationKey(KeyPair, Algorithm);
		const FString Header =
			FString::Printf(TEXT("{\"alg\":\"%s\",\"kid\":\"fixture-key\",\"typ\":\"JWT\"}"), *Algorithm.ToString());
		FString Compact;
		TestTrue(*FString::Printf(TEXT("%s compact JWS is signed"), *Algorithm.ToString()),
								  SignCompact(KeyPair, Algorithm, Header, Claims, Compact));
		EVP_PKEY_free(KeyPair);

		TArray<uint8> CompactBytes = Utf8Bytes(Compact);
		FUnrealAIOidcProtectedHeader Protected;
		FUnrealAIProviderAccessError Error;
		TestTrue(*FString::Printf(TEXT("%s protected header is admitted"), *Algorithm.ToString()),
								  Crypto.InspectProtectedHeader(CompactBytes, Protected, Error));
		FUnrealAIOidcVerifiedClaims Verified;
		TestEqual(*FString::Printf(TEXT("%s signature and claims verify"), *Algorithm.ToString()),
								   Crypto.VerifyAndDecodeIdToken(CompactBytes, PublicKey, Verified, Error),
								   EUnrealAIOidcTokenVerificationResult::Succeeded);
		TestEqual(TEXT("Verified subject is projected only after signature"), Verified.Subject,
					   FString(TEXT("provider-subject")));
		if (Algorithm == FName(TEXT("RS256")))
		{
			FUnrealAIOidcJsonWebKey TrailingDerKey = PublicKey;
			TrailingDerKey.PublicKeyMaterial.Add(0);
			Verified = {};
			TestEqual(TEXT("Trailing DER public-key bytes fail closed"),
						   Crypto.VerifyAndDecodeIdToken(CompactBytes, TrailingDerKey, Verified, Error),
						   EUnrealAIOidcTokenVerificationResult::Malformed);
		}

		const int32 FirstDot = Compact.Find(TEXT("."), ESearchCase::CaseSensitive);
		if (FirstDot != INDEX_NONE && FirstDot + 2 < Compact.Len())
		{
			Compact[FirstDot + 1] = Compact[FirstDot + 1] == TEXT('e') ? TEXT('f') : TEXT('e');
		}
		CompactBytes = Utf8Bytes(Compact);
		Verified = {};
		TestEqual(*FString::Printf(TEXT("%s tampering is rejected before claim parsing"), *Algorithm.ToString()),
								   Crypto.VerifyAndDecodeIdToken(CompactBytes, PublicKey, Verified, Error),
								   EUnrealAIOidcTokenVerificationResult::SignatureRejected);
		TestTrue(TEXT("Rejected signatures expose no claims"), Verified.Subject.IsEmpty());
	}

	EVP_PKEY *DuplicateKey = GenerateKey(FName(TEXT("RS256")));
	FString DuplicateCompact;
	TestTrue(
		TEXT("Duplicate-header fixture is signed"),
			 DuplicateKey != nullptr &&
				 SignCompact(DuplicateKey, FName(TEXT("RS256")),
												 TEXT("{\"alg\":\"RS256\",\"alg\":\"RS256\",\"kid\":\"fixture-key\"}"),
													  Claims, DuplicateCompact));
	FString ExtendedCompact;
	TestTrue(TEXT("Extended-header fixture is signed"),
				  DuplicateKey != nullptr &&
					  SignCompact(DuplicateKey, FName(TEXT("RS256")),
													  TEXT("{\"alg\":\"RS256\",\"kid\":\"fixture-key\",\"crit\":[]}"),
														   Claims, ExtendedCompact));
	EVP_PKEY_free(DuplicateKey);
	FUnrealAIOidcProtectedHeader Header;
	FUnrealAIProviderAccessError Error;
	const TArray<uint8> DuplicateBytes = Utf8Bytes(DuplicateCompact);
	TestFalse(TEXT("Duplicate protected-header members fail closed"),
				   Crypto.InspectProtectedHeader(DuplicateBytes, Header, Error));
	TArray<uint8> ExtendedBytes = Utf8Bytes(ExtendedCompact);
	TestFalse(TEXT("Unrecognized protected-header members fail closed"),
				   Crypto.InspectProtectedHeader(ExtendedBytes, Header, Error));
	const int32 FirstDot = ExtendedCompact.Find(TEXT("."), ESearchCase::CaseSensitive);
	if (FirstDot != INDEX_NONE)
	{
		ExtendedCompact.InsertAt(FirstDot, TEXT('='));
	}
	ExtendedBytes = Utf8Bytes(ExtendedCompact);
	TestFalse(TEXT("Padded non-canonical compact segments fail closed"),
				   Crypto.InspectProtectedHeader(ExtendedBytes, Header, Error));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthProductionIssuerNativeCancellationTest,
								 "UnrealAI.Auth.OAuthOIDC.ProductionIssuer.NativeCancellationDrains",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthProductionIssuerNativeCancellationTest::RunTest(const FString &Parameters)
{
#if !PLATFORM_MAC
	AddInfo(TEXT("The built-in issuer HTTP client currently fails closed outside macOS."));
	return true;
#else
	using namespace AgentOAuthProductionIssuerJoseTests;
	FUnrealAICancellationSource Cancellation;
	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2));
	FUnrealAIOAuthAuthorizationOperationContext Context;
	TestTrue(TEXT("A bounded cancellation context is created"), MakeContext(Clock, Cancellation.GetToken(), Context));
	FUnrealAIOAuthIssuerHttpClientOptions Options;
	Options.bSuspendNativeTaskForTesting = true;
	Options.AfterNativeTaskCreatedReservedForTesting =
		MakeShared<TFunction<void()>, ESPMode::ThreadSafe>([&Cancellation]() { Cancellation.Cancel(); });
	TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe> Client =
		CreateAgentPlatformOAuthIssuerHttpClient(Options);
	FUnrealAIOAuthIssuerHttpRequest Request;
	Request.ExactUrl = TEXT("https://issuer.example.test/discovery");
	Request.Method = EUnrealAIOAuthIssuerHttpMethod::Get;
	FUnrealAIOAuthIssuerHttpResponse Response;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("A suspended native task returns only through physical cancellation"),
				   Client->Execute(Context, Request, Response, Error));
	TestEqual(TEXT("Physical cancellation preserves the closed cancelled result"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthCancelled);
	TestEqual(TEXT("Cancelled native requests expose no response body"), Response.Body.Num(), 0);
	return true;
#endif
}

#endif
