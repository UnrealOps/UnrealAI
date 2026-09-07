// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthTokenEnvelope.h"

#include "Misc/Crc.h"

namespace
{
constexpr uint8 EnvelopeMagic[] = {'A', 'A', 'O', 'A', 'U', 'T', 'H', '\0'};
constexpr int32 EnvelopeMagicBytes = UE_ARRAY_COUNT(EnvelopeMagic);
constexpr int32 VersionPrefixBytes = EnvelopeMagicBytes + sizeof(uint16);
constexpr int32 FixedHeaderBytes = EnvelopeMagicBytes + sizeof(uint16) + sizeof(uint16) + sizeof(uint16) +
								   sizeof(uint16) + sizeof(int64) + (sizeof(uint32) * 8);
constexpr int32 ChecksumBytes = sizeof(uint32);
constexpr uint16 RefreshTokenFlag = 1 << 0;
constexpr uint16 IdTokenFlag = 1 << 1;
constexpr uint16 AccountRoutingFlag = 1 << 2;
constexpr uint16 KnownFlags = RefreshTokenFlag | IdTokenFlag | AccountRoutingFlag;
constexpr int64 TicksPerSecond = 10000000;
constexpr int64 MinimumUnixSeconds = -62135596800ll;
constexpr int64 MaximumUnixSeconds = 253402300799ll;
constexpr int32 MaxClientIdUtf8Bytes = 1024;

void SecureResetBytes(TArray<uint8> &Bytes)
{
	volatile uint8 *Wipe = Bytes.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Bytes.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Bytes.Empty();
}

bool IsDateTimeValueValid(const FDateTime &Value)
{
	return Value.GetTicks() >= FDateTime::MinValue().GetTicks() && Value.GetTicks() <= FDateTime::MaxValue().GetTicks();
}

bool IsStableIdentifier(const FName Name)
{
	if (Name.IsNone())
	{
		return false;
	}
	const FString Text = Name.ToString();
	FTCHARToUTF8 Utf8(*Text);
	if (Text.IsEmpty() || Utf8.Length() < 1 || Utf8.Length() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'));
		if ((!bAlphaNumeric && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-')) ||
															 ((Index == 0 || Index == Text.Len() - 1) &&
															  !bAlphaNumeric))
		{
			return false;
		}
	}
	return true;
}

bool ValidateTokenMaterial(const FUnrealAISecretValue &AccessToken, const FUnrealAISecretValue &RefreshToken,
						   const FUnrealAISecretValue &IdToken, const FUnrealAISecretValue &AccountRoutingValue,
						   const FDateTime &AccessTokenExpiresAtUtc, const FDateTime &NowUtc, FString &OutError)
{
	if (!IsDateTimeValueValid(NowUtc) || !IsDateTimeValueValid(AccessTokenExpiresAtUtc) ||
		AccessTokenExpiresAtUtc.GetTicks() % TicksPerSecond != 0 || AccessTokenExpiresAtUtc <= NowUtc)
	{
		OutError = TEXT("OAuth token envelope requires an unexpired whole-second UTC access expiry.");
		return false;
	}
	if (!AccessToken.IsSet() || AccessToken.Num() > FUnrealAIOAuthTokenSet::MaxAccessTokenBytes ||
		RefreshToken.Num() > FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes ||
		IdToken.Num() > FUnrealAIOAuthTokenSet::MaxIdTokenBytes ||
		AccountRoutingValue.Num() > FUnrealAIOAuthTokenSet::MaxAccountRoutingBytes)
	{
		OutError = TEXT("OAuth token material is missing or exceeds its compiled byte bound.");
		return false;
	}
	return true;
}

void AppendUInt16(TArray<uint8> &Bytes, const uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value & 0xffu));
	Bytes.Add(static_cast<uint8>((Value >> 8) & 0xffu));
}

void AppendUInt32(TArray<uint8> &Bytes, const uint32 Value)
{
	for (uint32 Shift = 0; Shift < 32; Shift += 8)
	{
		Bytes.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
	}
}

void AppendUInt64(TArray<uint8> &Bytes, const uint64 Value)
{
	for (uint32 Shift = 0; Shift < 64; Shift += 8)
	{
		Bytes.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
	}
}

bool ReadUInt16(const TConstArrayView<uint8> Bytes, int32 &Offset, uint16 &OutValue)
{
	if (Offset < 0 || Offset > Bytes.Num() - static_cast<int32>(sizeof(uint16)))
	{
		return false;
	}
	OutValue = static_cast<uint16>(Bytes[Offset]) | (static_cast<uint16>(Bytes[Offset + 1]) << 8);
	Offset += sizeof(uint16);
	return true;
}

bool ReadUInt32(const TConstArrayView<uint8> Bytes, int32 &Offset, uint32 &OutValue)
{
	if (Offset < 0 || Offset > Bytes.Num() - static_cast<int32>(sizeof(uint32)))
	{
		return false;
	}
	OutValue = 0;
	for (uint32 Shift = 0; Shift < 32; Shift += 8)
	{
		OutValue |= static_cast<uint32>(Bytes[Offset++]) << Shift;
	}
	return true;
}

bool ReadUInt64(const TConstArrayView<uint8> Bytes, int32 &Offset, uint64 &OutValue)
{
	if (Offset < 0 || Offset > Bytes.Num() - static_cast<int32>(sizeof(uint64)))
	{
		return false;
	}
	OutValue = 0;
	for (uint32 Shift = 0; Shift < 64; Shift += 8)
	{
		OutValue |= static_cast<uint64>(Bytes[Offset++]) << Shift;
	}
	return true;
}

bool DecodeStableName(const TConstArrayView<uint8> Bytes, FName &OutName)
{
	OutName = NAME_None;
	if (Bytes.IsEmpty() || Bytes.Num() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	FString Text;
	Text.Reserve(Bytes.Num());
	for (const uint8 Byte : Bytes)
	{
		const bool bAlphaNumeric = (Byte >= static_cast<uint8>('a') && Byte <= static_cast<uint8>('z')) ||
								   (Byte >= static_cast<uint8>('0') && Byte <= static_cast<uint8>('9'));
		if ((!bAlphaNumeric && Byte != static_cast<uint8>('.') && Byte != static_cast<uint8>('_') &&
			 Byte != static_cast<uint8>('-')) ||
			((Text.IsEmpty() || Text.Len() == Bytes.Num() - 1) && !bAlphaNumeric))
		{
			return false;
		}
		Text.AppendChar(static_cast<TCHAR>(Byte));
	}
	OutName = FName(*Text);
	return IsStableIdentifier(OutName) && OutName.ToString() == Text;
}

bool CopySecret(const TConstArrayView<uint8> Bytes, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	OutSecret.Reset();
	if (Bytes.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Copy;
	Copy.Append(Bytes.GetData(), Bytes.Num());
	return FUnrealAISecretValue::TryCreate(MoveTemp(Copy), OutSecret, OutError);
}

void AppendRaw(TArray<uint8> &OutBytes, const TConstArrayView<uint8> Bytes)
{
	if (!Bytes.IsEmpty())
	{
		OutBytes.Append(Bytes.GetData(), Bytes.Num());
	}
}

bool IsFormUnreserved(const uint8 Byte)
{
	return (Byte >= static_cast<uint8>('a') && Byte <= static_cast<uint8>('z')) ||
		   (Byte >= static_cast<uint8>('A') && Byte <= static_cast<uint8>('Z')) ||
		   (Byte >= static_cast<uint8>('0') && Byte <= static_cast<uint8>('9')) || Byte == static_cast<uint8>('-') ||
		   Byte == static_cast<uint8>('.') || Byte == static_cast<uint8>('_') || Byte == static_cast<uint8>('~');
}

bool AppendFormEncoded(TArray<uint8> &OutBytes, const TConstArrayView<uint8> Source)
{
	static constexpr ANSICHAR Hex[] = "0123456789ABCDEF";
	const int64 MaximumAdditionalBytes = static_cast<int64>(Source.Num()) * 3;
	if (MaximumAdditionalBytes > FUnrealAISecretValue::MaxSecretBytes ||
		static_cast<int64>(OutBytes.Num()) + MaximumAdditionalBytes > FUnrealAISecretValue::MaxSecretBytes)
	{
		return false;
	}
	for (const uint8 Byte : Source)
	{
		if (IsFormUnreserved(Byte))
		{
			OutBytes.Add(Byte);
		}
		else
		{
			OutBytes.Add(static_cast<uint8>('%'));
			OutBytes.Add(static_cast<uint8>(Hex[(Byte >> 4) & 0x0f]));
			OutBytes.Add(static_cast<uint8>(Hex[Byte & 0x0f]));
		}
	}
	return true;
}

bool AppendAsciiLiteral(TArray<uint8> &OutBytes, const ANSICHAR *Literal)
{
	const int32 Length = FCStringAnsi::Strlen(Literal);
	if (Length < 0 || OutBytes.Num() > FUnrealAISecretValue::MaxSecretBytes - Length)
	{
		return false;
	}
	OutBytes.Append(reinterpret_cast<const uint8 *>(Literal), Length);
	return true;
}

struct FEncodedSecretReset final
{
	explicit FEncodedSecretReset(FUnrealAISecretValue &InSecret) : Secret(InSecret) {}
	~FEncodedSecretReset()
	{
		Secret.Reset();
	}
	FUnrealAISecretValue &Secret;
};

struct FEnvelopeReset final
{
	explicit FEnvelopeReset(FUnrealAIOAuthTokenEnvelope &InEnvelope) : Envelope(InEnvelope) {}
	~FEnvelopeReset()
	{
		Envelope.Reset();
	}
	FUnrealAIOAuthTokenEnvelope &Envelope;
};
} // namespace

bool FUnrealAIOAuthTokenEnvelopeBinding::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableIdentifier(ProviderName) || !IsStableIdentifier(AuthProfileId) || !AccountId.IsValid())
	{
		OutError = TEXT("OAuth token envelope binding requires stable provider/profile names and a local account.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthTokenSet::Reset()
{
	AccessToken.Reset();
	RefreshToken.Reset();
	IdToken.Reset();
	AccountRoutingValue.Reset();
	AccessTokenExpiresAtUtc = FDateTime{};
}

bool FUnrealAIOAuthTokenEnvelope::IsSet() const
{
	FString Error;
	return Binding.ValidateShape(Error) && AccessToken.IsSet() && IsDateTimeValueValid(AccessTokenExpiresAtUtc);
}

const FUnrealAIOAuthTokenEnvelopeBinding &FUnrealAIOAuthTokenEnvelope::GetBinding() const
{
	return Binding;
}

FDateTime FUnrealAIOAuthTokenEnvelope::GetAccessTokenExpiresAtUtc() const
{
	return AccessTokenExpiresAtUtc;
}

bool FUnrealAIOAuthTokenEnvelope::HasRefreshToken() const
{
	return RefreshToken.IsSet();
}

bool FUnrealAIOAuthTokenEnvelope::HasIdToken() const
{
	return IdToken.IsSet();
}

bool FUnrealAIOAuthTokenEnvelope::HasAccountRoutingValue() const
{
	return AccountRoutingValue.IsSet();
}

bool FUnrealAIOAuthTokenEnvelope::IsExpiredAt(const FDateTime &NowUtc) const
{
	return !IsDateTimeValueValid(NowUtc) || !IsDateTimeValueValid(AccessTokenExpiresAtUtc) ||
		   AccessTokenExpiresAtUtc <= NowUtc;
}

bool FUnrealAIOAuthTokenEnvelope::TryTakeDispatchCredentials(const FDateTime &NowUtc,
															 FUnrealAISecretValue &OutBearerToken,
															 FUnrealAISecretValue &OutAccountRoutingValue,
															 FString &OutError)
{
	OutBearerToken.Reset();
	OutAccountRoutingValue.Reset();
	OutError.Reset();
	FString BindingError;
	if (!Binding.ValidateShape(BindingError) || !AccessToken.IsSet() || IsExpiredAt(NowUtc))
	{
		OutError = TEXT("OAuth dispatch credentials are unavailable, invalid, consumed, or expired.");
		return false;
	}
	OutBearerToken = MoveTemp(AccessToken);
	OutAccountRoutingValue = MoveTemp(AccountRoutingValue);
	return true;
}

bool FUnrealAIOAuthTokenEnvelope::TryMintRefreshRequestBody(const FStringView ClientId,
															FUnrealAISecretValue &OutFormBody, FString &OutError) const
{
	return FUnrealAIOAuthTokenEnvelopeCodec::TryMintRefreshFormBody(RefreshToken, ClientId, OutFormBody, OutError);
}

bool FUnrealAIOAuthTokenEnvelope::TryTakeRevocationCredential(FUnrealAISecretValue &OutToken, FString &OutError)
{
	OutToken.Reset();
	OutError.Reset();
	if (!IsSet())
	{
		OutError = TEXT("OAuth token envelope is not initialized.");
		return false;
	}
	if (RefreshToken.IsSet())
	{
		OutToken = MoveTemp(RefreshToken);
		return true;
	}
	if (AccessToken.IsSet())
	{
		OutToken = MoveTemp(AccessToken);
		return true;
	}
	OutError = TEXT("OAuth token envelope has no revocation credential.");
	return false;
}

bool FUnrealAIOAuthTokenEnvelope::TryApplyRefresh(FUnrealAIOAuthTokenSet &&RefreshedTokens, const FDateTime &NowUtc,
												  FString &OutError)
{
	OutError.Reset();
	FString BindingError;
	if (!Binding.ValidateShape(BindingError) ||
		!ValidateTokenMaterial(RefreshedTokens.AccessToken, RefreshedTokens.RefreshToken, RefreshedTokens.IdToken,
							   RefreshedTokens.AccountRoutingValue, RefreshedTokens.AccessTokenExpiresAtUtc, NowUtc,
							   OutError) ||
		(!RefreshedTokens.RefreshToken.IsSet() && !RefreshToken.IsSet()))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("OAuth refresh omitted a refresh token when no prior token was available.");
		}
		RefreshedTokens.Reset();
		return false;
	}

	const bool bExistingRoutingPresent = AccountRoutingValue.IsSet();
	const bool bRefreshedRoutingPresent = RefreshedTokens.AccountRoutingValue.IsSet();
	bool bRoutingMatches = bExistingRoutingPresent == bRefreshedRoutingPresent;
	if (bRoutingMatches && bExistingRoutingPresent)
	{
		const TConstArrayView<uint8> ExistingBytes = AccountRoutingValue.View();
		const TConstArrayView<uint8> RefreshedBytes = RefreshedTokens.AccountRoutingValue.View();
		bRoutingMatches = ExistingBytes.Num() == RefreshedBytes.Num();
		uint8 Difference = 0;
		if (bRoutingMatches)
		{
			for (int32 Index = 0; Index < ExistingBytes.Num(); ++Index)
			{
				Difference |= ExistingBytes[Index] ^ RefreshedBytes[Index];
			}
			bRoutingMatches = Difference == 0;
		}
	}
	if (!bRoutingMatches)
	{
		OutError = TEXT("OAuth refresh changed or omitted protected account-routing material.");
		RefreshedTokens.Reset();
		return false;
	}

	AccessToken = MoveTemp(RefreshedTokens.AccessToken);
	if (RefreshedTokens.RefreshToken.IsSet())
	{
		RefreshToken = MoveTemp(RefreshedTokens.RefreshToken);
	}
	if (RefreshedTokens.IdToken.IsSet())
	{
		IdToken = MoveTemp(RefreshedTokens.IdToken);
	}
	AccountRoutingValue = MoveTemp(RefreshedTokens.AccountRoutingValue);
	AccessTokenExpiresAtUtc = RefreshedTokens.AccessTokenExpiresAtUtc;
	RefreshedTokens.Reset();
	return true;
}

void FUnrealAIOAuthTokenEnvelope::Reset()
{
	Binding = FUnrealAIOAuthTokenEnvelopeBinding{};
	AccessToken.Reset();
	RefreshToken.Reset();
	IdToken.Reset();
	AccountRoutingValue.Reset();
	AccessTokenExpiresAtUtc = FDateTime{};
}

bool FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(const FUnrealAIOAuthTokenEnvelopeBinding &Binding,
												 FUnrealAIOAuthTokenSet &&Tokens, const FDateTime &NowUtc,
												 FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError)
{
	OutEnvelope.Reset();
	OutError.Reset();
	if (!Binding.ValidateShape(OutError) ||
		!ValidateTokenMaterial(Tokens.AccessToken, Tokens.RefreshToken, Tokens.IdToken, Tokens.AccountRoutingValue,
							   Tokens.AccessTokenExpiresAtUtc, NowUtc, OutError))
	{
		Tokens.Reset();
		return false;
	}

	OutEnvelope.Binding = Binding;
	OutEnvelope.AccessToken = MoveTemp(Tokens.AccessToken);
	OutEnvelope.RefreshToken = MoveTemp(Tokens.RefreshToken);
	OutEnvelope.IdToken = MoveTemp(Tokens.IdToken);
	OutEnvelope.AccountRoutingValue = MoveTemp(Tokens.AccountRoutingValue);
	OutEnvelope.AccessTokenExpiresAtUtc = Tokens.AccessTokenExpiresAtUtc;
	Tokens.Reset();
	return true;
}

bool FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(FUnrealAIOAuthTokenEnvelope &&Envelope, const FDateTime &NowUtc,
												 FUnrealAISecretValue &OutEncodedEnvelope, FString &OutError)
{
	OutEncodedEnvelope.Reset();
	OutError.Reset();
	FEnvelopeReset EnvelopeReset(Envelope);
	if (!Envelope.Binding.ValidateShape(OutError) ||
		!ValidateTokenMaterial(Envelope.AccessToken, Envelope.RefreshToken, Envelope.IdToken,
							   Envelope.AccountRoutingValue, Envelope.AccessTokenExpiresAtUtc, NowUtc, OutError))
	{
		return false;
	}

	const FString ProviderText = Envelope.Binding.ProviderName.ToString();
	const FString ProfileText = Envelope.Binding.AuthProfileId.ToString();
	FTCHARToUTF8 ProviderUtf8(*ProviderText);
	FTCHARToUTF8 ProfileUtf8(*ProfileText);
	const TConstArrayView<uint8> AccessBytes = Envelope.AccessToken.View();
	const TConstArrayView<uint8> RefreshBytes = Envelope.RefreshToken.View();
	const TConstArrayView<uint8> IdBytes = Envelope.IdToken.View();
	const TConstArrayView<uint8> RoutingBytes = Envelope.AccountRoutingValue.View();
	const int64 TotalBytes = static_cast<int64>(FixedHeaderBytes) + ProviderUtf8.Length() + ProfileUtf8.Length() +
							 AccessBytes.Num() + RefreshBytes.Num() + IdBytes.Num() + RoutingBytes.Num() +
							 ChecksumBytes;
	if (ProviderUtf8.Length() > TNumericLimits<uint16>::Max() || ProfileUtf8.Length() > TNumericLimits<uint16>::Max() ||
		TotalBytes > FUnrealAISecretValue::MaxSecretBytes)
	{
		OutError = TEXT("OAuth token envelope exceeds its compiled encoded byte bound.");
		return false;
	}

	TArray<uint8> Wire;
	Wire.Reserve(static_cast<int32>(TotalBytes));
	Wire.Append(EnvelopeMagic, EnvelopeMagicBytes);
	AppendUInt16(Wire, CurrentVersion);
	uint16 Flags = 0;
	Flags |= RefreshBytes.IsEmpty() ? 0 : RefreshTokenFlag;
	Flags |= IdBytes.IsEmpty() ? 0 : IdTokenFlag;
	Flags |= RoutingBytes.IsEmpty() ? 0 : AccountRoutingFlag;
	AppendUInt16(Wire, Flags);
	AppendUInt16(Wire, static_cast<uint16>(ProviderUtf8.Length()));
	AppendUInt16(Wire, static_cast<uint16>(ProfileUtf8.Length()));
	AppendUInt64(Wire, static_cast<uint64>(Envelope.AccessTokenExpiresAtUtc.ToUnixTimestamp()));
	AppendUInt32(Wire, Envelope.Binding.AccountId.Value.A);
	AppendUInt32(Wire, Envelope.Binding.AccountId.Value.B);
	AppendUInt32(Wire, Envelope.Binding.AccountId.Value.C);
	AppendUInt32(Wire, Envelope.Binding.AccountId.Value.D);
	AppendUInt32(Wire, static_cast<uint32>(AccessBytes.Num()));
	AppendUInt32(Wire, static_cast<uint32>(RefreshBytes.Num()));
	AppendUInt32(Wire, static_cast<uint32>(IdBytes.Num()));
	AppendUInt32(Wire, static_cast<uint32>(RoutingBytes.Num()));
	Wire.Append(reinterpret_cast<const uint8 *>(ProviderUtf8.Get()), ProviderUtf8.Length());
	Wire.Append(reinterpret_cast<const uint8 *>(ProfileUtf8.Get()), ProfileUtf8.Length());
	AppendRaw(Wire, AccessBytes);
	AppendRaw(Wire, RefreshBytes);
	AppendRaw(Wire, IdBytes);
	AppendRaw(Wire, RoutingBytes);
	const uint32 Checksum = FCrc::MemCrc32(Wire.GetData(), Wire.Num());
	AppendUInt32(Wire, Checksum);
	if (Wire.Num() != TotalBytes)
	{
		SecureResetBytes(Wire);
		OutError = TEXT("OAuth token envelope encoding failed its physical-size invariant.");
		return false;
	}
	return FUnrealAISecretValue::TryCreate(MoveTemp(Wire), OutEncodedEnvelope, OutError);
}

bool FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(FUnrealAISecretValue &&EncodedEnvelope,
												 const FUnrealAIOAuthTokenEnvelopeBinding &ExpectedBinding,
												 const FDateTime &NowUtc, FUnrealAIOAuthTokenEnvelope &OutEnvelope,
												 FString &OutError)
{
	return TryDecodeInternal(MoveTemp(EncodedEnvelope), ExpectedBinding, NowUtc, false, OutEnvelope, OutError);
}

bool FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(FUnrealAISecretValue &&EncodedEnvelope,
														   const FUnrealAIOAuthTokenEnvelopeBinding &ExpectedBinding,
														   const FDateTime &NowUtc,
														   FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError)
{
	return TryDecodeInternal(MoveTemp(EncodedEnvelope), ExpectedBinding, NowUtc, true, OutEnvelope, OutError);
}

bool FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeInternal(FUnrealAISecretValue &&EncodedEnvelope,
														 const FUnrealAIOAuthTokenEnvelopeBinding &ExpectedBinding,
														 const FDateTime &NowUtc, const bool bAllowExpiredAccess,
														 FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError)
{
	OutEnvelope.Reset();
	OutError.Reset();
	FEncodedSecretReset EncodedReset(EncodedEnvelope);
	if (!ExpectedBinding.ValidateShape(OutError) || !IsDateTimeValueValid(NowUtc))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("OAuth token envelope decode requires a valid UTC instant.");
		}
		return false;
	}

	const TConstArrayView<uint8> Wire = EncodedEnvelope.View();
	if (Wire.Num() < VersionPrefixBytes)
	{
		OutError = TEXT("OAuth token envelope is malformed or truncated.");
		return false;
	}
	for (int32 Index = 0; Index < EnvelopeMagicBytes; ++Index)
	{
		if (Wire[Index] != EnvelopeMagic[Index])
		{
			OutError = TEXT("OAuth token envelope has an invalid format marker.");
			return false;
		}
	}
	int32 Offset = EnvelopeMagicBytes;
	uint16 Version = 0;
	if (!ReadUInt16(Wire, Offset, Version))
	{
		OutError = TEXT("OAuth token envelope is malformed or truncated.");
		return false;
	}
	if (Version != CurrentVersion)
	{
		OutError = TEXT("OAuth token envelope uses an unknown version.");
		return false;
	}
	if (Wire.Num() < FixedHeaderBytes + ChecksumBytes)
	{
		OutError = TEXT("OAuth token envelope is malformed or truncated.");
		return false;
	}

	const int32 ChecksumOffset = Wire.Num() - ChecksumBytes;
	int32 StoredChecksumOffset = ChecksumOffset;
	uint32 StoredChecksum = 0;
	if (!ReadUInt32(Wire, StoredChecksumOffset, StoredChecksum) ||
		StoredChecksum != FCrc::MemCrc32(Wire.GetData(), ChecksumOffset))
	{
		OutError = TEXT("OAuth token envelope checksum validation failed.");
		return false;
	}

	uint16 Flags = 0;
	uint16 ProviderBytes = 0;
	uint16 ProfileBytes = 0;
	uint64 RawExpirySeconds = 0;
	uint32 AccountA = 0;
	uint32 AccountB = 0;
	uint32 AccountC = 0;
	uint32 AccountD = 0;
	uint32 AccessBytes = 0;
	uint32 RefreshBytes = 0;
	uint32 IdBytes = 0;
	uint32 RoutingBytes = 0;
	if (!ReadUInt16(Wire, Offset, Flags) || !ReadUInt16(Wire, Offset, ProviderBytes) ||
		!ReadUInt16(Wire, Offset, ProfileBytes) || !ReadUInt64(Wire, Offset, RawExpirySeconds) ||
		!ReadUInt32(Wire, Offset, AccountA) || !ReadUInt32(Wire, Offset, AccountB) ||
		!ReadUInt32(Wire, Offset, AccountC) || !ReadUInt32(Wire, Offset, AccountD) ||
		!ReadUInt32(Wire, Offset, AccessBytes) || !ReadUInt32(Wire, Offset, RefreshBytes) ||
		!ReadUInt32(Wire, Offset, IdBytes) || !ReadUInt32(Wire, Offset, RoutingBytes))
	{
		OutError = TEXT("OAuth token envelope header is malformed.");
		return false;
	}
	if ((Flags & ~KnownFlags) != 0 || AccessBytes < 1 ||
		AccessBytes > static_cast<uint32>(FUnrealAIOAuthTokenSet::MaxAccessTokenBytes) ||
		RefreshBytes > static_cast<uint32>(FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes) ||
		IdBytes > static_cast<uint32>(FUnrealAIOAuthTokenSet::MaxIdTokenBytes) ||
		RoutingBytes > static_cast<uint32>(FUnrealAIOAuthTokenSet::MaxAccountRoutingBytes) ||
		((Flags & RefreshTokenFlag) != 0) != (RefreshBytes > 0) || ((Flags & IdTokenFlag) != 0) != (IdBytes > 0) ||
		((Flags & AccountRoutingFlag) != 0) != (RoutingBytes > 0))
	{
		OutError = TEXT("OAuth token envelope flags or field lengths are invalid.");
		return false;
	}

	const int64 ExpirySeconds = static_cast<int64>(RawExpirySeconds);
	if (ExpirySeconds < MinimumUnixSeconds || ExpirySeconds > MaximumUnixSeconds)
	{
		OutError = TEXT("OAuth token envelope contains an invalid UTC expiry.");
		return false;
	}
	const FDateTime ExpiryUtc = FDateTime::FromUnixTimestamp(ExpirySeconds);
	if (!bAllowExpiredAccess && ExpiryUtc <= NowUtc)
	{
		OutError = TEXT("OAuth token envelope access credential has expired.");
		return false;
	}

	const int64 PayloadBytes =
		static_cast<int64>(ProviderBytes) + ProfileBytes + AccessBytes + RefreshBytes + IdBytes + RoutingBytes;
	if (PayloadBytes < 0 || static_cast<int64>(Offset) + PayloadBytes != ChecksumOffset)
	{
		OutError = TEXT("OAuth token envelope payload length is inconsistent.");
		return false;
	}

	const TConstArrayView<uint8> ProviderView = Wire.Slice(Offset, ProviderBytes);
	Offset += ProviderBytes;
	const TConstArrayView<uint8> ProfileView = Wire.Slice(Offset, ProfileBytes);
	Offset += ProfileBytes;
	FUnrealAIOAuthTokenEnvelopeBinding ParsedBinding;
	if (!DecodeStableName(ProviderView, ParsedBinding.ProviderName) ||
		!DecodeStableName(ProfileView, ParsedBinding.AuthProfileId))
	{
		OutError = TEXT("OAuth token envelope binding names are invalid.");
		return false;
	}
	ParsedBinding.AccountId.Value = FGuid(AccountA, AccountB, AccountC, AccountD);
	if (!(ParsedBinding == ExpectedBinding))
	{
		OutError = TEXT("OAuth token envelope binding does not match the requested provider, profile, and account.");
		return false;
	}

	FUnrealAIOAuthTokenEnvelope Parsed;
	Parsed.Binding = ParsedBinding;
	Parsed.AccessTokenExpiresAtUtc = ExpiryUtc;
	if (!CopySecret(Wire.Slice(Offset, AccessBytes), Parsed.AccessToken, OutError))
	{
		return false;
	}
	Offset += AccessBytes;
	if (!CopySecret(Wire.Slice(Offset, RefreshBytes), Parsed.RefreshToken, OutError))
	{
		return false;
	}
	Offset += RefreshBytes;
	if (!CopySecret(Wire.Slice(Offset, IdBytes), Parsed.IdToken, OutError))
	{
		return false;
	}
	Offset += IdBytes;
	if (!CopySecret(Wire.Slice(Offset, RoutingBytes), Parsed.AccountRoutingValue, OutError))
	{
		return false;
	}
	Offset += RoutingBytes;
	if (Offset != ChecksumOffset)
	{
		OutError = TEXT("OAuth token envelope decoding failed its exact-consumption invariant.");
		return false;
	}
	OutEnvelope = MoveTemp(Parsed);
	return true;
}

bool FUnrealAIOAuthTokenEnvelopeCodec::TryMintRefreshFormBody(const FUnrealAISecretValue &RefreshToken,
															  const FStringView ClientId,
															  FUnrealAISecretValue &OutFormBody, FString &OutError)
{
	OutFormBody.Reset();
	OutError.Reset();
	const FString ClientIdText(ClientId);
	FTCHARToUTF8 ClientIdUtf8(*ClientIdText);
	if (!RefreshToken.IsSet() || RefreshToken.Num() > FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes ||
		ClientIdText.IsEmpty() || ClientIdUtf8.Length() < 1 || ClientIdUtf8.Length() > MaxClientIdUtf8Bytes)
	{
		OutError = TEXT("OAuth refresh form requires a bounded refresh token and client identifier.");
		return false;
	}
	for (const TCHAR Character : ClientIdText)
	{
		if (Character < 0x21 || Character > 0x7e)
		{
			OutError = TEXT("OAuth refresh client identifier must contain bounded printable ASCII.");
			return false;
		}
	}

	TArray<uint8> Body;
	Body.Reserve(
		FMath::Min(FUnrealAISecretValue::MaxSecretBytes, 128 + RefreshToken.Num() * 3 + ClientIdUtf8.Length() * 3));
	const TConstArrayView<uint8> ClientIdBytes(reinterpret_cast<const uint8 *>(ClientIdUtf8.Get()),
											   ClientIdUtf8.Length());
	if (!AppendAsciiLiteral(Body, "grant_type=refresh_token&refresh_token=") ||
		!AppendFormEncoded(Body, RefreshToken.View()) || !AppendAsciiLiteral(Body, "&client_id=") ||
		!AppendFormEncoded(Body, ClientIdBytes))
	{
		SecureResetBytes(Body);
		OutError = TEXT("OAuth refresh form exceeds its compiled encoded byte bound.");
		return false;
	}
	return FUnrealAISecretValue::TryCreate(MoveTemp(Body), OutFormBody, OutError);
}
