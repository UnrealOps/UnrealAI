// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Serialization/JsonReader.h"
#include "Values/UnrealAITextValidationPrivate.h"

namespace UE::UnrealAI::JsonValidation::Private
{
struct FJsonPreflightLimits
{
	int64 MaxUtf8Bytes = 64 * 1024;
	int32 MaxDepth = 64;
	int32 MaxNotations = 4096;
	int32 MaxContainerEntries = 1024;
	int32 MaxStringTokenCodeUnits = 64 * 1024;
};

inline bool TryReadHexCodeUnit(const FString &Json, const int32 FirstDigitIndex, uint32 &OutCodeUnit)
{
	if (FirstDigitIndex < 0 || FirstDigitIndex > Json.Len() - 4)
		return false;
	OutCodeUnit = 0;
	for (int32 Offset = 0; Offset < 4; ++Offset)
	{
		const TCHAR Character = Json[FirstDigitIndex + Offset];
		uint32 Nibble = 0;
		if (Character >= TEXT('0') && Character <= TEXT('9'))
			Nibble = static_cast<uint32>(Character - TEXT('0'));
		else if (Character >= TEXT('a') && Character <= TEXT('f'))
			Nibble = 10u + static_cast<uint32>(Character - TEXT('a'));
		else if (Character >= TEXT('A') && Character <= TEXT('F'))
			Nibble = 10u + static_cast<uint32>(Character - TEXT('A'));
		else
			return false;
		OutCodeUnit = (OutCodeUnit << 4u) | Nibble;
	}
	return true;
}

inline bool PreflightJson(const FString &Json, const FJsonPreflightLimits &Limits, FString &OutError)
{
	OutError.Reset();
	if (Limits.MaxUtf8Bytes < 0 || Limits.MaxDepth < 0 || Limits.MaxNotations <= 0 || Limits.MaxContainerEntries < 0 ||
		Limits.MaxStringTokenCodeUnits < 0 || Json.Len() > Limits.MaxUtf8Bytes)
	{
		OutError = TEXT("JSON is malformed or exceeds its UTF-8 bound.");
		return false;
	}
	const int64 MaximumAllocatedBytes = 2ll * (static_cast<int64>(Json.Len()) + 1ll) * sizeof(TCHAR) + 64ll * 1024ll;
	if (Json.GetAllocatedSize() > static_cast<SIZE_T>(MaximumAllocatedBytes) ||
		!UE::UnrealAI::TextValidation::Private::IsWellFormedSerializedString(Json) ||
		UE::UnrealAI::TextValidation::Private::Utf8Length(Json) > Limits.MaxUtf8Bytes)
	{
		OutError = TEXT("JSON is malformed or exceeds its UTF-8 bound.");
		return false;
	}

	bool bInString = false;
	int32 StringStart = INDEX_NONE;
	int32 StructuralDepth = 0;
	for (int32 Index = 0; Index < Json.Len(); ++Index)
	{
		const TCHAR Character = Json[Index];
		if (!bInString)
		{
			if (Character == TEXT('"'))
			{
				bInString = true;
				StringStart = Index + 1;
			}
			else if (Character == TEXT('{') || Character == TEXT('['))
			{
				if (++StructuralDepth > Limits.MaxDepth)
				{
					OutError = TEXT("JSON exceeds its structural-depth bound.");
					return false;
				}
			}
			else if (Character == TEXT('}') || Character == TEXT(']'))
			{
				if (--StructuralDepth < 0)
				{
					OutError = TEXT("JSON has unbalanced containers.");
					return false;
				}
			}
			continue;
		}
		if (Index - StringStart > Limits.MaxStringTokenCodeUnits)
		{
			OutError = TEXT("JSON contains an oversized string token.");
			return false;
		}
		if (Character == TEXT('"'))
		{
			bInString = false;
			continue;
		}
		if (Character != TEXT('\\'))
			continue;
		if (++Index >= Json.Len())
		{
			OutError = TEXT("JSON has an unterminated escape.");
			return false;
		}
		if (Json[Index] != TEXT('u'))
			continue;
		uint32 CodeUnit = 0;
		if (!TryReadHexCodeUnit(Json, Index + 1, CodeUnit))
		{
			OutError = TEXT("JSON has a malformed Unicode escape.");
			return false;
		}
		Index += 4;
		if (CodeUnit == 0u || CodeUnit == 0xfffeu || CodeUnit == 0xffffu ||
			(CodeUnit >= 0xdc00u && CodeUnit <= 0xdfffu))
		{
			OutError = TEXT("JSON has an invalid Unicode escape.");
			return false;
		}
		if (CodeUnit >= 0xd800u && CodeUnit <= 0xdbffu)
		{
			if (Index + 6 >= Json.Len() || Json[Index + 1] != TEXT('\\') || Json[Index + 2] != TEXT('u'))
			{
				OutError = TEXT("JSON has an unpaired high-surrogate escape.");
				return false;
			}
			uint32 LowSurrogate = 0;
			if (!TryReadHexCodeUnit(Json, Index + 3, LowSurrogate) || LowSurrogate < 0xdc00u || LowSurrogate > 0xdfffu)
			{
				OutError = TEXT("JSON has an unpaired high-surrogate escape.");
				return false;
			}
			Index += 6;
		}
	}
	if (bInString || StructuralDepth != 0)
	{
		OutError = TEXT("JSON has unterminated strings or containers.");
		return false;
	}

	struct FContainerState
	{
		bool bObject = false;
		int32 Entries = 0;
		TSet<FString> Keys;
	};
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TArray<FContainerState> Containers;
	EJsonNotation Notation = EJsonNotation::Error;
	int32 NotationCount = 0;
	int32 RootValueCount = 0;
	while (Reader->ReadNext(Notation))
	{
		if (Notation == EJsonNotation::Error)
		{
			OutError = TEXT("JSON reader rejected malformed or trailing input.");
			return false;
		}
		if (++NotationCount > Limits.MaxNotations)
		{
			OutError = TEXT("JSON exceeds its notation bound.");
			return false;
		}
		if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
		{
			const bool bClosingObject = Notation == EJsonNotation::ObjectEnd;
			if (Containers.IsEmpty() || Containers.Last().bObject != bClosingObject)
			{
				OutError = TEXT("JSON has mismatched containers.");
				return false;
			}
			Containers.Pop(EAllowShrinking::No);
			continue;
		}
		if (Containers.IsEmpty())
		{
			if (Notation != EJsonNotation::ObjectStart && Notation != EJsonNotation::ArrayStart)
			{
				OutError = TEXT("JSON root must be an object or array.");
				return false;
			}
			if (++RootValueCount > 1)
			{
				OutError = TEXT("JSON must contain exactly one root value.");
				return false;
			}
		}
		if (!Containers.IsEmpty())
		{
			FContainerState &Parent = Containers.Last();
			if (++Parent.Entries > Limits.MaxContainerEntries)
			{
				OutError = TEXT("JSON container exceeds its entry bound.");
				return false;
			}
			if (Parent.bObject)
			{
				FString Key = Reader->GetIdentifier();
				Key.ToLowerInline();
				if (Parent.Keys.Contains(Key))
				{
					OutError = TEXT("JSON contains duplicate or case-colliding object keys.");
					return false;
				}
				Parent.Keys.Add(MoveTemp(Key));
			}
		}
		if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
		{
			FContainerState State;
			State.bObject = Notation == EJsonNotation::ObjectStart;
			Containers.Add(MoveTemp(State));
		}
	}
	if (!Reader->GetErrorMessage().IsEmpty())
	{
		OutError = TEXT("JSON reader rejected malformed or trailing input.");
		return false;
	}
	if (!Containers.IsEmpty())
	{
		OutError = TEXT("JSON has unterminated containers.");
		return false;
	}
	if (RootValueCount != 1)
	{
		OutError = TEXT("JSON must contain exactly one root value.");
		return false;
	}
	return true;
}
} // namespace UE::UnrealAI::JsonValidation::Private
