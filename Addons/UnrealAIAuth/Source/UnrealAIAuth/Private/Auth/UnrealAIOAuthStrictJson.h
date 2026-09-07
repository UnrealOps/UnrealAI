// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UE::UnrealAI::Auth::Private
{
enum class EStrictJsonType : uint8
{
	Null,
	Boolean,
	Number,
	String,
	Array,
	Object
};

/** Move-only, wipe-on-destruction strict JSON value used for OAuth and JOSE wire material. */
struct FStrictJsonValue final
{
	FStrictJsonValue() = default;
	FStrictJsonValue(const FStrictJsonValue &) = delete;
	FStrictJsonValue &operator=(const FStrictJsonValue &) = delete;
	FStrictJsonValue(FStrictJsonValue &&Other) noexcept;
	FStrictJsonValue &operator=(FStrictJsonValue &&Other) noexcept;
	~FStrictJsonValue();

	EStrictJsonType Type = EStrictJsonType::Null;
	bool bBoolean = false;
	TArray<uint8> ScalarBytes;
	TArray<FStrictJsonValue> ArrayValues;
	TArray<TArray<uint8>> ObjectNames;
	TArray<FStrictJsonValue> ObjectValues;

	const FStrictJsonValue *FindObjectValue(const ANSICHAR *Name) const;
	bool EqualsAscii(const ANSICHAR *Expected) const;
	bool TryGetString(FString &OutValue) const;
	bool TryGetInt64(int64 &OutValue) const;
	void Reset();
};

struct FStrictJsonParseLimits final
{
	int32 MaxTotalBytes = 512 * 1024;
	int32 MaxDepth = 12;
	int32 MaxNodes = 2048;
	int32 MaxStringBytes = 64 * 1024;
	int32 MaxContainerEntries = 512;
};

void SecureResetOAuthWireBytes(TArray<uint8> &Bytes);
void SecureResetOAuthWireString(FString &Value);
bool ParseStrictJson(TConstArrayView<uint8> Bytes, const FStrictJsonParseLimits &Limits, FStrictJsonValue &OutValue);
bool TryStrictBase64UrlDecode(TConstArrayView<uint8> Encoded, int32 MaxDecodedBytes, TArray<uint8> &OutDecoded);
FString StrictBase64UrlEncode(TConstArrayView<uint8> Bytes);
bool IsVisibleAsciiBytes(TConstArrayView<uint8> Bytes, int32 MaxBytes, bool bAllowEmpty = false);
bool Utf8BytesToString(TConstArrayView<uint8> Bytes, int32 MaxBytes, FString &OutValue, bool bAllowEmpty = false);
} // namespace UE::UnrealAI::Auth::Private
