// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Containers/StringConv.h"
#include "Internationalization/Text.h"
#include "Values/UnrealAIPhysicalAllocationValidationPrivate.h"

namespace UE::UnrealAI::TextValidation::Private
{
inline bool IsWellFormedSerializedString(const FString &Value)
{
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const uint32 CodeUnit = static_cast<uint32>(Value[Index]);
		if (CodeUnit == 0)
			return false;
		if constexpr (sizeof(TCHAR) == 2)
		{
			if (CodeUnit >= 0xd800u && CodeUnit <= 0xdbffu)
			{
				if (++Index >= Value.Len())
					return false;
				const uint32 LowSurrogate = static_cast<uint32>(Value[Index]);
				if (LowSurrogate < 0xdc00u || LowSurrogate > 0xdfffu)
					return false;
				const uint32 Codepoint = 0x10000u + ((CodeUnit - 0xd800u) << 10u) + (LowSurrogate - 0xdc00u);
				if (!StringConv::IsValidCodepoint(Codepoint))
					return false;
			}
			else if (CodeUnit >= 0xdc00u && CodeUnit <= 0xdfffu)
			{
				return false;
			}
			else if (!StringConv::IsValidCodepoint(CodeUnit))
			{
				return false;
			}
		}
		else if ((CodeUnit >= 0xd800u && CodeUnit <= 0xdfffu) || !StringConv::IsValidCodepoint(CodeUnit))
		{
			return false;
		}
	}
	return true;
}

inline int64 Utf8Length(const FString &Value)
{
	return static_cast<int64>(FTCHARToUTF8_Convert::ConvertedLength(*Value, Value.Len()));
}

/** Counts Unicode scalar values after IsWellFormedSerializedString has accepted the input. */
inline int32 CodepointLength(const FString &Value)
{
	int32 Count = 0;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		if constexpr (sizeof(TCHAR) == 2)
		{
			const uint32 CodeUnit = static_cast<uint32>(Value[Index]);
			if (CodeUnit >= 0xd800u && CodeUnit <= 0xdbffu)
			{
				++Index;
			}
		}
		++Count;
	}
	return Count;
}

inline bool TryGetStableSourceView(const FText &Value, const FString *&OutSource, FString &OutError,
								   const TCHAR *FieldName)
{
	OutSource = nullptr;
	OutError.Reset();
	const bool bCanonicalEmpty = Value.IdenticalTo(FText::GetEmpty());
	const bool bHasAuthoredHistory = bCanonicalEmpty || Value.IsCultureInvariant() || Value.IsInitializedFromString() ||
									 Value.IsFromStringTable() || !FTextInspector::GetTextId(Value).IsEmpty();
	if (Value.IsTransient() || !bHasAuthoredHistory)
	{
		OutError = FString::Printf(TEXT("%s cannot use a generated or transient FText history."), FieldName);
		return false;
	}

	OutSource = FTextInspector::GetSourceString(Value);
	if (!bCanonicalEmpty && OutSource == nullptr)
	{
		OutError = FString::Printf(TEXT("%s has no stable authored source string."), FieldName);
		return false;
	}
	if (OutSource == nullptr)
	{
		static const FString EmptySource;
		OutSource = &EmptySource;
	}
	return true;
}

inline bool TryGetPhysicallyBoundedStableSourceView(const FText &Value, const int64 MaxUtf8Bytes,
													const FString *&OutSource, FString &OutError,
													const TCHAR *FieldName)
{
	OutSource = nullptr;
	OutError.Reset();
	if (MaxUtf8Bytes < 0)
	{
		OutError = FString::Printf(TEXT("%s has an invalid UTF-8 byte bound."), FieldName);
		return false;
	}

	// Inspecting the authored source is constant-time. Bound its direct backing
	// before IdenticalTo or any other operation that may walk FText history.
	const FString *AuthoredSource = FTextInspector::GetSourceString(Value);
	if (AuthoredSource != nullptr &&
		!UE::UnrealAI::PhysicalAllocation::Private::HasBoundedStringStorage(*AuthoredSource, MaxUtf8Bytes))
	{
		OutError = FString::Printf(TEXT("%s exceeds its physical string-allocation bound."), FieldName);
		return false;
	}

	if (!TryGetStableSourceView(Value, OutSource, OutError, FieldName))
	{
		return false;
	}
	if (!UE::UnrealAI::PhysicalAllocation::Private::HasBoundedStringStorage(*OutSource, MaxUtf8Bytes))
	{
		OutError = FString::Printf(TEXT("%s exceeds its physical string-allocation bound."), FieldName);
		return false;
	}
	return true;
}

inline bool TryGetStableSource(const FText &Value, FString &OutSource, FString &OutError, const TCHAR *FieldName)
{
	OutSource.Reset();
	const FString *StableSource = nullptr;
	if (!TryGetStableSourceView(Value, StableSource, OutError, FieldName))
		return false;
	OutSource = *StableSource;
	return true;
}

inline bool TryGetBoundedStableSource(const FText &Value, const int64 MaxUtf8Bytes, FString &OutSource,
									  int64 &OutUtf8Bytes, FString &OutError, const TCHAR *FieldName)
{
	OutSource.Reset();
	OutUtf8Bytes = 0;
	if (MaxUtf8Bytes < 0)
	{
		OutError = FString::Printf(TEXT("%s has an invalid UTF-8 byte bound."), FieldName);
		return false;
	}
	const FString *StableSource = nullptr;
	if (!TryGetPhysicallyBoundedStableSourceView(Value, MaxUtf8Bytes, StableSource, OutError, FieldName))
		return false;
	if (!IsWellFormedSerializedString(*StableSource))
	{
		OutError = FString::Printf(TEXT("%s contains an embedded NUL or malformed Unicode."), FieldName);
		return false;
	}
	OutUtf8Bytes = Utf8Length(*StableSource);
	if (OutUtf8Bytes > MaxUtf8Bytes)
	{
		OutError = FString::Printf(TEXT("%s exceeds its UTF-8 byte bound."), FieldName);
		return false;
	}
	OutSource = *StableSource;
	OutSource.Shrink();
	return true;
}
} // namespace UE::UnrealAI::TextValidation::Private
