// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthOpenSslAuthorizationCrypto.h"

#include "Auth/UnrealAIOAuthJwkPrivate.h"
#include "Auth/UnrealAIOAuthStrictJson.h"
#include "Misc/ScopeExit.h"

#if UNREALAI_OAUTH_OPENSSL
#define UI OPENSSL_UI
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#undef UI
#endif

namespace OAuthJson = UE::UnrealAI::Auth::Private;

namespace
{
constexpr int32 Sha256Bytes = 32;
constexpr int32 MaxCompactTokenBytes = FUnrealAIOAuthTokenSet::MaxIdTokenBytes;
constexpr int64 MaxUnixTimestamp = 253402300799LL;

FUnrealAIProviderAccessError MakeCryptoError(const EUnrealAIErrorCategory Category,
											 const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}

void SetOAuthCryptoInvalidResponse(FUnrealAIProviderAccessError &OutError)
{
	OutError = MakeCryptoError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
}

bool IsObjectName(const TArray<uint8> &Name, const ANSICHAR *Expected)
{
	const int32 Length = FCStringAnsi::Strlen(Expected);
	return Name.Num() == Length &&
		   (Length == 0 || FMemory::Memcmp(Name.GetData(), Expected, static_cast<SIZE_T>(Length)) == 0);
}

bool HasOnlyProtectedHeaderMembers(const OAuthJson::FStrictJsonValue &Header)
{
	if (Header.Type != OAuthJson::EStrictJsonType::Object || Header.ObjectNames.Num() != Header.ObjectValues.Num())
	{
		return false;
	}
	for (const TArray<uint8> &Name : Header.ObjectNames)
	{
		if (!IsObjectName(Name, "alg") && !IsObjectName(Name, "kid") && !IsObjectName(Name, "typ"))
		{
			return false;
		}
	}
	return true;
}

bool IsKnownAlgorithm(const OAuthJson::FStrictJsonValue &Value, FName &OutAlgorithm)
{
	if (Value.EqualsAscii("RS256"))
	{
		OutAlgorithm = FName(TEXT("RS256"));
	}
	else if (Value.EqualsAscii("PS256"))
	{
		OutAlgorithm = FName(TEXT("PS256"));
	}
	else if (Value.EqualsAscii("ES256"))
	{
		OutAlgorithm = FName(TEXT("ES256"));
	}
	else if (Value.EqualsAscii("EdDSA"))
	{
		OutAlgorithm = FName(TEXT("EdDSA"));
	}
	else
	{
		return false;
	}
	return true;
}

struct FCompactJws final
{
	TConstArrayView<uint8> HeaderSegment;
	TConstArrayView<uint8> ClaimsSegment;
	TConstArrayView<uint8> SignatureSegment;
	TConstArrayView<uint8> SigningInput;
};

bool SplitCompactJws(const TConstArrayView<uint8> Token, FCompactJws &Out)
{
	Out = {};
	if (Token.IsEmpty() || Token.Num() > MaxCompactTokenBytes)
	{
		return false;
	}
	int32 FirstDot = INDEX_NONE;
	int32 SecondDot = INDEX_NONE;
	for (int32 Index = 0; Index < Token.Num(); ++Index)
	{
		const uint8 Byte = Token[Index];
		const bool bBase64Url = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') ||
								(Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '_';
		if (Byte == '.')
		{
			if (FirstDot == INDEX_NONE)
			{
				FirstDot = Index;
			}
			else if (SecondDot == INDEX_NONE)
			{
				SecondDot = Index;
			}
			else
			{
				return false;
			}
		}
		else if (!bBase64Url)
		{
			return false;
		}
	}
	if (FirstDot <= 0 || SecondDot <= FirstDot + 1 || SecondDot >= Token.Num() - 1)
	{
		return false;
	}
	Out.HeaderSegment = Token.Slice(0, FirstDot);
	Out.ClaimsSegment = Token.Slice(FirstDot + 1, SecondDot - FirstDot - 1);
	Out.SignatureSegment = Token.Slice(SecondDot + 1, Token.Num() - SecondDot - 1);
	Out.SigningInput = Token.Slice(0, SecondDot);
	return true;
}

bool ParseProtectedHeader(const FCompactJws &Jws, FUnrealAIOidcProtectedHeader &OutHeader)
{
	OutHeader = {};
	TArray<uint8> HeaderBytes;
	ON_SCOPE_EXIT
	{
		OAuthJson::SecureResetOAuthWireBytes(HeaderBytes);
	};
	if (!OAuthJson::TryStrictBase64UrlDecode(Jws.HeaderSegment, 4096, HeaderBytes))
	{
		return false;
	}
	OAuthJson::FStrictJsonValue Header;
	OAuthJson::FStrictJsonParseLimits Limits;
	Limits.MaxTotalBytes = 4096;
	Limits.MaxDepth = 3;
	Limits.MaxNodes = 16;
	Limits.MaxStringBytes = FUnrealAIOidcJsonWebKey::MaxIdentifierUtf8Bytes;
	Limits.MaxContainerEntries = 8;
	if (!OAuthJson::ParseStrictJson(HeaderBytes, Limits, Header) || !HasOnlyProtectedHeaderMembers(Header))
	{
		return false;
	}
	const OAuthJson::FStrictJsonValue *Algorithm = Header.FindObjectValue("alg");
	const OAuthJson::FStrictJsonValue *KeyId = Header.FindObjectValue("kid");
	const OAuthJson::FStrictJsonValue *Type = Header.FindObjectValue("typ");
	if (Algorithm == nullptr || KeyId == nullptr || !IsKnownAlgorithm(*Algorithm, OutHeader.Algorithm) ||
		!KeyId->TryGetString(OutHeader.KeyId) || (Type != nullptr && !Type->EqualsAscii("JWT")))
	{
		OutHeader = {};
		return false;
	}
	FString ShapeError;
	if (!OutHeader.ValidateShape(ShapeError))
	{
		OutHeader = {};
		return false;
	}
	return true;
}

bool TryGetRequiredString(const OAuthJson::FStrictJsonValue &Object, const ANSICHAR *Name, FString &OutValue)
{
	const OAuthJson::FStrictJsonValue *Value = Object.FindObjectValue(Name);
	return Value != nullptr && Value->TryGetString(OutValue);
}

bool TryGetRequiredTime(const OAuthJson::FStrictJsonValue &Object, const ANSICHAR *Name, FDateTime &OutTime)
{
	const OAuthJson::FStrictJsonValue *Value = Object.FindObjectValue(Name);
	int64 Seconds = 0;
	if (Value == nullptr || !Value->TryGetInt64(Seconds) || Seconds < 0 || Seconds > MaxUnixTimestamp)
	{
		return false;
	}
	OutTime = FDateTime::FromUnixTimestamp(Seconds);
	return OutTime.GetTicks() > 0;
}

bool ParseVerifiedClaims(const TConstArrayView<uint8> ClaimsBytes, FUnrealAIOidcVerifiedClaims &OutClaims)
{
	OutClaims = {};
	OAuthJson::FStrictJsonValue Claims;
	OAuthJson::FStrictJsonParseLimits Limits;
	Limits.MaxTotalBytes = FUnrealAIOAuthTokenSet::MaxIdTokenBytes;
	Limits.MaxDepth = 5;
	Limits.MaxNodes = 128;
	Limits.MaxStringBytes = 4096;
	Limits.MaxContainerEntries = 32;
	if (!OAuthJson::ParseStrictJson(ClaimsBytes, Limits, Claims) || Claims.Type != OAuthJson::EStrictJsonType::Object ||
		!TryGetRequiredString(Claims, "iss", OutClaims.Issuer) ||
		!TryGetRequiredString(Claims, "sub", OutClaims.Subject) ||
		!TryGetRequiredString(Claims, "nonce", OutClaims.Nonce) ||
		!TryGetRequiredTime(Claims, "exp", OutClaims.ExpiresAtUtc) ||
		!TryGetRequiredTime(Claims, "iat", OutClaims.IssuedAtUtc))
	{
		OutClaims = {};
		return false;
	}

	const OAuthJson::FStrictJsonValue *Audience = Claims.FindObjectValue("aud");
	if (Audience == nullptr)
	{
		OutClaims = {};
		return false;
	}
	if (Audience->Type == OAuthJson::EStrictJsonType::String)
	{
		FString Value;
		if (!Audience->TryGetString(Value))
		{
			OutClaims = {};
			return false;
		}
		OutClaims.Audiences.Add(MoveTemp(Value));
	}
	else if (Audience->Type == OAuthJson::EStrictJsonType::Array && !Audience->ArrayValues.IsEmpty() &&
			 Audience->ArrayValues.Num() <= FUnrealAIOidcVerifiedClaims::MaxAudiences)
	{
		TSet<FString> Seen;
		for (const OAuthJson::FStrictJsonValue &Entry : Audience->ArrayValues)
		{
			FString Value;
			if (!Entry.TryGetString(Value) || Seen.Contains(Value))
			{
				OutClaims = {};
				return false;
			}
			Seen.Add(Value);
			OutClaims.Audiences.Add(MoveTemp(Value));
		}
	}
	else
	{
		OutClaims = {};
		return false;
	}

	if (const OAuthJson::FStrictJsonValue *AuthorizedParty = Claims.FindObjectValue("azp"))
	{
		if (!AuthorizedParty->TryGetString(OutClaims.AuthorizedParty))
		{
			OutClaims = {};
			return false;
		}
	}
	if (const OAuthJson::FStrictJsonValue *NotBefore = Claims.FindObjectValue("nbf"))
	{
		int64 Seconds = 0;
		if (!NotBefore->TryGetInt64(Seconds) || Seconds < 0 || Seconds > MaxUnixTimestamp)
		{
			OutClaims = {};
			return false;
		}
		OutClaims.NotBeforeUtc = FDateTime::FromUnixTimestamp(Seconds);
	}
	FString ShapeError;
	if (!OutClaims.ValidateShape(ShapeError))
	{
		OutClaims = {};
		return false;
	}
	return true;
}

#if UNREALAI_OAUTH_OPENSSL
bool ValidatePublicKeyProfile(EVP_PKEY *Key, const FName Algorithm)
{
	if (Key == nullptr)
	{
		return false;
	}
	const FString Name = Algorithm.ToString();
	const int32 Type = EVP_PKEY_base_id(Key);
	if (Name == TEXT("RS256") || Name == TEXT("PS256"))
	{
		const int32 Bits = EVP_PKEY_bits(Key);
		return Type == EVP_PKEY_RSA && Bits >= 2048 && Bits <= 8192;
	}
	if (Name == TEXT("ES256"))
	{
		const EC_KEY *Ec = EVP_PKEY_get0_EC_KEY(Key);
		const EC_GROUP *Group = Ec != nullptr ? EC_KEY_get0_group(Ec) : nullptr;
		return Type == EVP_PKEY_EC && Group != nullptr && EC_GROUP_get_curve_name(Group) == NID_X9_62_prime256v1;
	}
	return Name == TEXT("EdDSA") && Type == EVP_PKEY_ED25519;
}

bool EncodeSubjectPublicKeyInfo(EVP_PKEY *Key, TArray<uint8> &OutDer)
{
	OAuthJson::SecureResetOAuthWireBytes(OutDer);
	const int32 Length = i2d_PUBKEY(Key, nullptr);
	if (Length <= 0 || Length > FUnrealAIOidcJsonWebKey::MaxPublicKeyMaterialBytes)
	{
		return false;
	}
	OutDer.SetNumUninitialized(Length);
	unsigned char *Cursor = OutDer.GetData();
	if (i2d_PUBKEY(Key, &Cursor) != Length || Cursor != OutDer.GetData() + Length)
	{
		OAuthJson::SecureResetOAuthWireBytes(OutDer);
		return false;
	}
	return true;
}

bool VerifySignature(EVP_PKEY *PublicKey, const FName Algorithm, const FCompactJws &Jws,
					 TConstArrayView<uint8> Signature)
{
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
		return EVP_DigestVerifyInit(Context, &KeyContext, nullptr, nullptr, PublicKey) == 1 &&
			   EVP_DigestVerify(Context, Signature.GetData(), static_cast<SIZE_T>(Signature.Num()),
								Jws.SigningInput.GetData(), static_cast<SIZE_T>(Jws.SigningInput.Num())) == 1;
	}
	if (EVP_DigestVerifyInit(Context, &KeyContext, EVP_sha256(), nullptr, PublicKey) != 1 || KeyContext == nullptr)
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
	return EVP_DigestVerifyUpdate(Context, Jws.SigningInput.GetData(), static_cast<SIZE_T>(Jws.SigningInput.Num())) ==
			   1 &&
		   EVP_DigestVerifyFinal(Context, Signature.GetData(), static_cast<SIZE_T>(Signature.Num())) == 1;
}
#endif
} // namespace

namespace UE::UnrealAI::Auth::Private
{
bool TryEncodeJwkSubjectPublicKeyInfo(const FStrictJsonValue &KeyObject, FName &OutKeyType, FName &OutAlgorithm,
									  TArray<uint8> &OutDer)
{
	OutKeyType = NAME_None;
	OutAlgorithm = NAME_None;
	SecureResetOAuthWireBytes(OutDer);
#if !UNREALAI_OAUTH_OPENSSL
	return false;
#else
	if (KeyObject.Type != EStrictJsonType::Object || KeyObject.FindObjectValue("d") != nullptr ||
		KeyObject.FindObjectValue("p") != nullptr || KeyObject.FindObjectValue("q") != nullptr ||
		KeyObject.FindObjectValue("dp") != nullptr || KeyObject.FindObjectValue("dq") != nullptr ||
		KeyObject.FindObjectValue("qi") != nullptr || KeyObject.FindObjectValue("oth") != nullptr ||
		KeyObject.FindObjectValue("k") != nullptr)
	{
		return false;
	}
	const FStrictJsonValue *KeyType = KeyObject.FindObjectValue("kty");
	const FStrictJsonValue *Algorithm = KeyObject.FindObjectValue("alg");
	const FStrictJsonValue *Use = KeyObject.FindObjectValue("use");
	if (KeyType == nullptr || Algorithm == nullptr || Use == nullptr || !Use->EqualsAscii("sig") ||
		!IsKnownAlgorithm(*Algorithm, OutAlgorithm))
	{
		return false;
	}
	if (const FStrictJsonValue *KeyOperations = KeyObject.FindObjectValue("key_ops"))
	{
		if (KeyOperations->Type != EStrictJsonType::Array || KeyOperations->ArrayValues.Num() != 1 ||
			!KeyOperations->ArrayValues[0].EqualsAscii("verify"))
		{
			return false;
		}
	}

	EVP_PKEY *PublicKey = nullptr;
	ON_SCOPE_EXIT
	{
		EVP_PKEY_free(PublicKey);
	};
	if (KeyType->EqualsAscii("RSA") && (OutAlgorithm == FName(TEXT("RS256")) || OutAlgorithm == FName(TEXT("PS256"))))
	{
		const FStrictJsonValue *Modulus = KeyObject.FindObjectValue("n");
		const FStrictJsonValue *Exponent = KeyObject.FindObjectValue("e");
		TArray<uint8> ModulusBytes;
		TArray<uint8> ExponentBytes;
		ON_SCOPE_EXIT
		{
			SecureResetOAuthWireBytes(ModulusBytes);
			SecureResetOAuthWireBytes(ExponentBytes);
		};
		if (Modulus == nullptr || Exponent == nullptr || Modulus->Type != EStrictJsonType::String ||
			Exponent->Type != EStrictJsonType::String ||
			!TryStrictBase64UrlDecode(Modulus->ScalarBytes, 1024, ModulusBytes) ||
			!TryStrictBase64UrlDecode(Exponent->ScalarBytes, 8, ExponentBytes) || ModulusBytes.Num() < 256 ||
			ModulusBytes.Num() > 1024 || ExponentBytes.IsEmpty() || ExponentBytes.Num() > 4 || ModulusBytes[0] == 0 ||
			ExponentBytes[0] == 0)
		{
			return false;
		}
		uint64 ExponentValue = 0;
		for (const uint8 Byte : ExponentBytes)
		{
			ExponentValue = (ExponentValue << 8u) | Byte;
		}
		if (ExponentValue < 3 || (ExponentValue & 1u) == 0)
		{
			return false;
		}
		BIGNUM *N = BN_bin2bn(ModulusBytes.GetData(), ModulusBytes.Num(), nullptr);
		BIGNUM *E = BN_bin2bn(ExponentBytes.GetData(), ExponentBytes.Num(), nullptr);
		RSA *Rsa = RSA_new();
		if (N == nullptr || E == nullptr || Rsa == nullptr || RSA_set0_key(Rsa, N, E, nullptr) != 1)
		{
			BN_free(N);
			BN_free(E);
			RSA_free(Rsa);
			return false;
		}
		PublicKey = EVP_PKEY_new();
		if (PublicKey == nullptr || EVP_PKEY_assign_RSA(PublicKey, Rsa) != 1)
		{
			RSA_free(Rsa);
			return false;
		}
		OutKeyType = FName(TEXT("RSA"));
	}
	else if (KeyType->EqualsAscii("EC") && OutAlgorithm == FName(TEXT("ES256")))
	{
		const FStrictJsonValue *Curve = KeyObject.FindObjectValue("crv");
		const FStrictJsonValue *X = KeyObject.FindObjectValue("x");
		const FStrictJsonValue *Y = KeyObject.FindObjectValue("y");
		TArray<uint8> XBytes;
		TArray<uint8> YBytes;
		TArray<uint8> PointBytes;
		ON_SCOPE_EXIT
		{
			SecureResetOAuthWireBytes(XBytes);
			SecureResetOAuthWireBytes(YBytes);
			SecureResetOAuthWireBytes(PointBytes);
		};
		if (Curve == nullptr || !Curve->EqualsAscii("P-256") || X == nullptr || Y == nullptr ||
			X->Type != EStrictJsonType::String || Y->Type != EStrictJsonType::String ||
			!TryStrictBase64UrlDecode(X->ScalarBytes, 32, XBytes) ||
			!TryStrictBase64UrlDecode(Y->ScalarBytes, 32, YBytes) || XBytes.Num() != 32 || YBytes.Num() != 32)
		{
			return false;
		}
		PointBytes.Reserve(65);
		PointBytes.Add(0x04);
		PointBytes.Append(XBytes);
		PointBytes.Append(YBytes);
		EC_KEY *Ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
		const EC_GROUP *Group = Ec != nullptr ? EC_KEY_get0_group(Ec) : nullptr;
		EC_POINT *Point = Group != nullptr ? EC_POINT_new(Group) : nullptr;
		if (Ec == nullptr || Point == nullptr ||
			EC_POINT_oct2point(Group, Point, PointBytes.GetData(), PointBytes.Num(), nullptr) != 1 ||
			EC_POINT_is_on_curve(Group, Point, nullptr) != 1 || EC_KEY_set_public_key(Ec, Point) != 1 ||
			EC_KEY_check_key(Ec) != 1)
		{
			EC_POINT_free(Point);
			EC_KEY_free(Ec);
			return false;
		}
		EC_POINT_free(Point);
		PublicKey = EVP_PKEY_new();
		if (PublicKey == nullptr || EVP_PKEY_assign_EC_KEY(PublicKey, Ec) != 1)
		{
			EC_KEY_free(Ec);
			return false;
		}
		OutKeyType = FName(TEXT("EC"));
	}
	else if (KeyType->EqualsAscii("OKP") && OutAlgorithm == FName(TEXT("EdDSA")))
	{
		const FStrictJsonValue *Curve = KeyObject.FindObjectValue("crv");
		const FStrictJsonValue *X = KeyObject.FindObjectValue("x");
		TArray<uint8> XBytes;
		ON_SCOPE_EXIT
		{
			SecureResetOAuthWireBytes(XBytes);
		};
		if (Curve == nullptr || !Curve->EqualsAscii("Ed25519") || X == nullptr || X->Type != EStrictJsonType::String ||
			!TryStrictBase64UrlDecode(X->ScalarBytes, 32, XBytes) || XBytes.Num() != 32)
		{
			return false;
		}
		PublicKey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, XBytes.GetData(), XBytes.Num());
		OutKeyType = FName(TEXT("OKP"));
	}
	else
	{
		return false;
	}
	if (!ValidatePublicKeyProfile(PublicKey, OutAlgorithm) || !EncodeSubjectPublicKeyInfo(PublicKey, OutDer))
	{
		OutKeyType = NAME_None;
		OutAlgorithm = NAME_None;
		return false;
	}
	return true;
#endif
}
} // namespace UE::UnrealAI::Auth::Private

bool FUnrealAIOAuthOpenSslAuthorizationCrypto::IsPlatformSupported()
{
#if UNREALAI_OAUTH_OPENSSL
	return true;
#else
	return false;
#endif
}

bool FUnrealAIOAuthOpenSslAuthorizationCrypto::GenerateSecureRandomBytes(const int32 NumBytes, TArray<uint8> &OutBytes,
																		 FUnrealAIProviderAccessError &OutError)
{
	OAuthJson::SecureResetOAuthWireBytes(OutBytes);
	OutError = {};
#if UNREALAI_OAUTH_OPENSSL
	if (NumBytes <= 0 || NumBytes > 4096)
	{
		OutError =
			MakeCryptoError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	OutBytes.SetNumUninitialized(NumBytes);
	if (RAND_bytes(OutBytes.GetData(), NumBytes) != 1)
	{
		OAuthJson::SecureResetOAuthWireBytes(OutBytes);
		OutError = MakeCryptoError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return false;
	}
	return true;
#else
	OutError = MakeCryptoError(EUnrealAIErrorCategory::UnsupportedCapability,
							   EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
	return false;
#endif
}

bool FUnrealAIOAuthOpenSslAuthorizationCrypto::Sha256(const TConstArrayView<uint8> Input, TArray<uint8> &OutDigest,
													  FUnrealAIProviderAccessError &OutError)
{
	OAuthJson::SecureResetOAuthWireBytes(OutDigest);
	OutError = {};
#if UNREALAI_OAUTH_OPENSSL
	if (Input.IsEmpty() || Input.Num() > 1024 * 1024)
	{
		OutError =
			MakeCryptoError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	OutDigest.SetNumUninitialized(Sha256Bytes);
	if (SHA256(Input.GetData(), static_cast<SIZE_T>(Input.Num()), OutDigest.GetData()) == nullptr)
	{
		OAuthJson::SecureResetOAuthWireBytes(OutDigest);
		OutError = MakeCryptoError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return false;
	}
	return true;
#else
	OutError = MakeCryptoError(EUnrealAIErrorCategory::UnsupportedCapability,
							   EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
	return false;
#endif
}

bool FUnrealAIOAuthOpenSslAuthorizationCrypto::InspectProtectedHeader(const TConstArrayView<uint8> CompactIdToken,
																	  FUnrealAIOidcProtectedHeader &OutHeader,
																	  FUnrealAIProviderAccessError &OutError)
{
	OutHeader = {};
	OutError = {};
	FCompactJws Jws;
	if (!SplitCompactJws(CompactIdToken, Jws) || !ParseProtectedHeader(Jws, OutHeader))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return false;
	}
	return true;
}

EUnrealAIOidcTokenVerificationResult FUnrealAIOAuthOpenSslAuthorizationCrypto::VerifyAndDecodeIdToken(
	const TConstArrayView<uint8> CompactIdToken, const FUnrealAIOidcJsonWebKey &Key,
	FUnrealAIOidcVerifiedClaims &OutClaims, FUnrealAIProviderAccessError &OutError)
{
	OutClaims = {};
	OutError = {};
#if !UNREALAI_OAUTH_OPENSSL
	OutError = MakeCryptoError(EUnrealAIErrorCategory::UnsupportedCapability,
							   EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
	return EUnrealAIOidcTokenVerificationResult::UnsupportedAlgorithm;
#else
	FString KeyError;
	FCompactJws Jws;
	FUnrealAIOidcProtectedHeader Header;
	if (!Key.ValidateShape(KeyError) || !SplitCompactJws(CompactIdToken, Jws) || !ParseProtectedHeader(Jws, Header))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::Malformed;
	}
	if (Header.Algorithm != Key.Algorithm || Header.KeyId != Key.KeyId)
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::SignatureRejected;
	}
	TArray<uint8> Signature;
	TArray<uint8> ClaimsBytes;
	ON_SCOPE_EXIT
	{
		OAuthJson::SecureResetOAuthWireBytes(Signature);
		OAuthJson::SecureResetOAuthWireBytes(ClaimsBytes);
	};
	if (!OAuthJson::TryStrictBase64UrlDecode(Jws.SignatureSegment, 2048, Signature) ||
		!OAuthJson::TryStrictBase64UrlDecode(Jws.ClaimsSegment, FUnrealAIOAuthTokenSet::MaxIdTokenBytes, ClaimsBytes))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::Malformed;
	}
	if (Header.Algorithm == FName(TEXT("ES256")) && Signature.Num() == 64)
	{
		BIGNUM *R = BN_bin2bn(Signature.GetData(), 32, nullptr);
		BIGNUM *S = BN_bin2bn(Signature.GetData() + 32, 32, nullptr);
		ECDSA_SIG *EcSignature = ECDSA_SIG_new();
		if (R == nullptr || S == nullptr || EcSignature == nullptr || ECDSA_SIG_set0(EcSignature, R, S) != 1)
		{
			BN_free(R);
			BN_free(S);
			ECDSA_SIG_free(EcSignature);
			SetOAuthCryptoInvalidResponse(OutError);
			return EUnrealAIOidcTokenVerificationResult::Malformed;
		}
		const int32 DerLength = i2d_ECDSA_SIG(EcSignature, nullptr);
		TArray<uint8> DerSignature;
		if (DerLength <= 0 || DerLength > 80)
		{
			ECDSA_SIG_free(EcSignature);
			SetOAuthCryptoInvalidResponse(OutError);
			return EUnrealAIOidcTokenVerificationResult::Malformed;
		}
		DerSignature.SetNumUninitialized(DerLength);
		unsigned char *Cursor = DerSignature.GetData();
		const bool bEncoded = i2d_ECDSA_SIG(EcSignature, &Cursor) == DerLength;
		ECDSA_SIG_free(EcSignature);
		if (!bEncoded)
		{
			OAuthJson::SecureResetOAuthWireBytes(DerSignature);
			SetOAuthCryptoInvalidResponse(OutError);
			return EUnrealAIOidcTokenVerificationResult::Malformed;
		}
		OAuthJson::SecureResetOAuthWireBytes(Signature);
		Signature = MoveTemp(DerSignature);
	}
	else if (Header.Algorithm == FName(TEXT("ES256")))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::Malformed;
	}

	const unsigned char *Cursor = Key.PublicKeyMaterial.GetData();
	EVP_PKEY *PublicKey = d2i_PUBKEY(nullptr, &Cursor, Key.PublicKeyMaterial.Num());
	ON_SCOPE_EXIT
	{
		EVP_PKEY_free(PublicKey);
	};
	if (PublicKey == nullptr || Cursor != Key.PublicKeyMaterial.GetData() + Key.PublicKeyMaterial.Num() ||
		!ValidatePublicKeyProfile(PublicKey, Header.Algorithm))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::Malformed;
	}
	if (!VerifySignature(PublicKey, Header.Algorithm, Jws, Signature))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::SignatureRejected;
	}
	if (!ParseVerifiedClaims(ClaimsBytes, OutClaims))
	{
		SetOAuthCryptoInvalidResponse(OutError);
		return EUnrealAIOidcTokenVerificationResult::Malformed;
	}
	return EUnrealAIOidcTokenVerificationResult::Succeeded;
#endif
}
