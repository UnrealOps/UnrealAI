// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthStrictJson.h"

#include "Misc/Base64.h"

namespace UE::UnrealAI::Auth::Private
{
void SecureResetOAuthWireBytes(TArray<uint8> &Bytes)
{
	if (Bytes.GetData() != nullptr && Bytes.Max() > 0)
	{
		FMemory::Memzero(Bytes.GetData(), Bytes.Max());
	}
	Bytes.Empty();
}

void SecureResetOAuthWireString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

FStrictJsonValue::~FStrictJsonValue()
{
	Reset();
}

FStrictJsonValue::FStrictJsonValue(FStrictJsonValue &&Other) noexcept
	: Type(Other.Type), bBoolean(Other.bBoolean), ScalarBytes(MoveTemp(Other.ScalarBytes)),
	  ArrayValues(MoveTemp(Other.ArrayValues)), ObjectNames(MoveTemp(Other.ObjectNames)),
	  ObjectValues(MoveTemp(Other.ObjectValues))
{
	Other.Type = EStrictJsonType::Null;
	Other.bBoolean = false;
}

FStrictJsonValue &FStrictJsonValue::operator=(FStrictJsonValue &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Type = Other.Type;
		bBoolean = Other.bBoolean;
		ScalarBytes = MoveTemp(Other.ScalarBytes);
		ArrayValues = MoveTemp(Other.ArrayValues);
		ObjectNames = MoveTemp(Other.ObjectNames);
		ObjectValues = MoveTemp(Other.ObjectValues);
		Other.Type = EStrictJsonType::Null;
		Other.bBoolean = false;
	}
	return *this;
}

void FStrictJsonValue::Reset()
{
	SecureResetOAuthWireBytes(ScalarBytes);
	for (FStrictJsonValue &Value : ArrayValues)
	{
		Value.Reset();
	}
	ArrayValues.Empty();
	for (TArray<uint8> &Name : ObjectNames)
	{
		SecureResetOAuthWireBytes(Name);
	}
	ObjectNames.Empty();
	for (FStrictJsonValue &Value : ObjectValues)
	{
		Value.Reset();
	}
	ObjectValues.Empty();
	Type = EStrictJsonType::Null;
	bBoolean = false;
}

const FStrictJsonValue *FStrictJsonValue::FindObjectValue(const ANSICHAR *Name) const
{
	if (Type != EStrictJsonType::Object || Name == nullptr || ObjectNames.Num() != ObjectValues.Num())
	{
		return nullptr;
	}
	const int32 Length = FCStringAnsi::Strlen(Name);
	for (int32 Index = 0; Index < ObjectNames.Num(); ++Index)
	{
		const TArray<uint8> &Candidate = ObjectNames[Index];
		if (Candidate.Num() == Length &&
			(Length == 0 || FMemory::Memcmp(Candidate.GetData(), Name, static_cast<SIZE_T>(Length)) == 0))
		{
			return &ObjectValues[Index];
		}
	}
	return nullptr;
}

bool FStrictJsonValue::EqualsAscii(const ANSICHAR *Expected) const
{
	if (Type != EStrictJsonType::String || Expected == nullptr)
	{
		return false;
	}
	const int32 Length = FCStringAnsi::Strlen(Expected);
	return ScalarBytes.Num() == Length &&
		   (Length == 0 || FMemory::Memcmp(ScalarBytes.GetData(), Expected, static_cast<SIZE_T>(Length)) == 0);
}

bool FStrictJsonValue::TryGetString(FString &OutValue) const
{
	return Type == EStrictJsonType::String && Utf8BytesToString(ScalarBytes, MAX_int32, OutValue, true);
}

bool FStrictJsonValue::TryGetInt64(int64 &OutValue) const
{
	OutValue = 0;
	if (Type != EStrictJsonType::Number || ScalarBytes.IsEmpty())
	{
		return false;
	}
	int32 Index = 0;
	bool bNegative = false;
	if (ScalarBytes[0] == '-')
	{
		bNegative = true;
		Index = 1;
	}
	if (Index >= ScalarBytes.Num())
	{
		return false;
	}
	uint64 Magnitude = 0;
	const uint64 Limit = bNegative ? static_cast<uint64>(MAX_int64) + 1u : static_cast<uint64>(MAX_int64);
	for (; Index < ScalarBytes.Num(); ++Index)
	{
		const uint8 Byte = ScalarBytes[Index];
		if (Byte < '0' || Byte > '9')
		{
			return false;
		}
		const uint64 Digit = static_cast<uint64>(Byte - '0');
		if (Magnitude > (Limit - Digit) / 10u)
		{
			return false;
		}
		Magnitude = Magnitude * 10u + Digit;
	}
	if (bNegative)
	{
		OutValue = Magnitude == static_cast<uint64>(MAX_int64) + 1u ? MIN_int64 : -static_cast<int64>(Magnitude);
	}
	else
	{
		OutValue = static_cast<int64>(Magnitude);
	}
	return true;
}

namespace
{
class FStrictJsonParser final
{
  public:
	FStrictJsonParser(const TConstArrayView<uint8> InBytes, const FStrictJsonParseLimits &InLimits)
		: Bytes(InBytes), Limits(InLimits)
	{
	}

	bool Parse(FStrictJsonValue &OutValue)
	{
		OutValue.Reset();
		if (Bytes.IsEmpty() || Bytes.Num() > Limits.MaxTotalBytes || Limits.MaxDepth < 1 || Limits.MaxDepth > 64 ||
			Limits.MaxNodes < 1 || Limits.MaxNodes > 65536 || Limits.MaxStringBytes < 1 ||
			Limits.MaxStringBytes > Limits.MaxTotalBytes || Limits.MaxContainerEntries < 1 ||
			Limits.MaxContainerEntries > Limits.MaxNodes)
		{
			return false;
		}
		SkipWhitespace();
		if (!ParseValue(0, OutValue))
		{
			OutValue.Reset();
			return false;
		}
		SkipWhitespace();
		if (Index != Bytes.Num())
		{
			OutValue.Reset();
			return false;
		}
		return true;
	}

  private:
	void SkipWhitespace()
	{
		while (Index < Bytes.Num() &&
			   (Bytes[Index] == ' ' || Bytes[Index] == '\t' || Bytes[Index] == '\r' || Bytes[Index] == '\n'))
		{
			++Index;
		}
	}

	bool Consume(const uint8 Expected)
	{
		if (Index >= Bytes.Num() || Bytes[Index] != Expected)
		{
			return false;
		}
		++Index;
		return true;
	}

	bool ReserveNode()
	{
		if (Nodes >= Limits.MaxNodes)
		{
			return false;
		}
		++Nodes;
		return true;
	}

	bool AppendByte(TArray<uint8> &Out, const uint8 Byte) const
	{
		if (Out.Num() >= Limits.MaxStringBytes)
		{
			return false;
		}
		Out.Add(Byte);
		return true;
	}

	bool AppendCodePoint(TArray<uint8> &Out, const uint32 CodePoint) const
	{
		if (CodePoint <= 0x7f)
		{
			return AppendByte(Out, static_cast<uint8>(CodePoint));
		}
		if (CodePoint <= 0x7ff)
		{
			return AppendByte(Out, static_cast<uint8>(0xc0u | (CodePoint >> 6u))) &&
				   AppendByte(Out, static_cast<uint8>(0x80u | (CodePoint & 0x3fu)));
		}
		if (CodePoint <= 0xffff && (CodePoint < 0xd800 || CodePoint > 0xdfff))
		{
			return AppendByte(Out, static_cast<uint8>(0xe0u | (CodePoint >> 12u))) &&
				   AppendByte(Out, static_cast<uint8>(0x80u | ((CodePoint >> 6u) & 0x3fu))) &&
				   AppendByte(Out, static_cast<uint8>(0x80u | (CodePoint & 0x3fu)));
		}
		if (CodePoint <= 0x10ffff)
		{
			return AppendByte(Out, static_cast<uint8>(0xf0u | (CodePoint >> 18u))) &&
				   AppendByte(Out, static_cast<uint8>(0x80u | ((CodePoint >> 12u) & 0x3fu))) &&
				   AppendByte(Out, static_cast<uint8>(0x80u | ((CodePoint >> 6u) & 0x3fu))) &&
				   AppendByte(Out, static_cast<uint8>(0x80u | (CodePoint & 0x3fu)));
		}
		return false;
	}

	static int32 Hex(const uint8 Byte)
	{
		if (Byte >= '0' && Byte <= '9')
		{
			return Byte - '0';
		}
		if (Byte >= 'a' && Byte <= 'f')
		{
			return Byte - 'a' + 10;
		}
		if (Byte >= 'A' && Byte <= 'F')
		{
			return Byte - 'A' + 10;
		}
		return INDEX_NONE;
	}

	bool ParseHexCodeUnit(uint32 &OutCodeUnit)
	{
		OutCodeUnit = 0;
		if (Index > Bytes.Num() - 4)
		{
			return false;
		}
		for (int32 Count = 0; Count < 4; ++Count)
		{
			const int32 Value = Hex(Bytes[Index++]);
			if (Value == INDEX_NONE)
			{
				return false;
			}
			OutCodeUnit = (OutCodeUnit << 4u) | static_cast<uint32>(Value);
		}
		return true;
	}

	bool AppendRawUtf8(TArray<uint8> &Out)
	{
		if (Index >= Bytes.Num())
		{
			return false;
		}
		const uint8 Lead = Bytes[Index];
		int32 Count = 0;
		uint32 CodePoint = 0;
		uint32 Minimum = 0;
		if (Lead >= 0xc2 && Lead <= 0xdf)
		{
			Count = 2;
			CodePoint = Lead & 0x1fu;
			Minimum = 0x80;
		}
		else if (Lead >= 0xe0 && Lead <= 0xef)
		{
			Count = 3;
			CodePoint = Lead & 0x0fu;
			Minimum = 0x800;
		}
		else if (Lead >= 0xf0 && Lead <= 0xf4)
		{
			Count = 4;
			CodePoint = Lead & 0x07u;
			Minimum = 0x10000;
		}
		else
		{
			return false;
		}
		if (Index > Bytes.Num() - Count || Out.Num() > Limits.MaxStringBytes - Count)
		{
			return false;
		}
		for (int32 Offset = 1; Offset < Count; ++Offset)
		{
			const uint8 Continuation = Bytes[Index + Offset];
			if ((Continuation & 0xc0u) != 0x80u)
			{
				return false;
			}
			CodePoint = (CodePoint << 6u) | (Continuation & 0x3fu);
		}
		if (CodePoint < Minimum || CodePoint > 0x10ffff || (CodePoint >= 0xd800 && CodePoint <= 0xdfff))
		{
			return false;
		}
		Out.Append(Bytes.GetData() + Index, Count);
		Index += Count;
		return true;
	}

	bool ParseStringBytes(TArray<uint8> &Out)
	{
		SecureResetOAuthWireBytes(Out);
		if (!Consume('"'))
		{
			return false;
		}
		while (Index < Bytes.Num())
		{
			const uint8 Byte = Bytes[Index++];
			if (Byte == '"')
			{
				return true;
			}
			if (Byte == '\\')
			{
				if (Index >= Bytes.Num())
				{
					return false;
				}
				switch (Bytes[Index++])
				{
				case '"':
				case '\\':
				case '/':
					if (!AppendByte(Out, Bytes[Index - 1]))
					{
						return false;
					}
					break;
				case 'b':
					if (!AppendByte(Out, '\b'))
					{
						return false;
					}
					break;
				case 'f':
					if (!AppendByte(Out, '\f'))
					{
						return false;
					}
					break;
				case 'n':
					if (!AppendByte(Out, '\n'))
					{
						return false;
					}
					break;
				case 'r':
					if (!AppendByte(Out, '\r'))
					{
						return false;
					}
					break;
				case 't':
					if (!AppendByte(Out, '\t'))
					{
						return false;
					}
					break;
				case 'u':
				{
					uint32 First = 0;
					if (!ParseHexCodeUnit(First))
					{
						return false;
					}
					uint32 CodePoint = First;
					if (First >= 0xd800 && First <= 0xdbff)
					{
						if (Index > Bytes.Num() - 6 || Bytes[Index] != '\\' || Bytes[Index + 1] != 'u')
						{
							return false;
						}
						Index += 2;
						uint32 Second = 0;
						if (!ParseHexCodeUnit(Second) || Second < 0xdc00 || Second > 0xdfff)
						{
							return false;
						}
						CodePoint = 0x10000u + ((First - 0xd800u) << 10u) + (Second - 0xdc00u);
					}
					else if (First >= 0xdc00 && First <= 0xdfff)
					{
						return false;
					}
					if (!AppendCodePoint(Out, CodePoint))
					{
						return false;
					}
					break;
				}
				default:
					return false;
				}
				continue;
			}
			if (Byte < 0x20)
			{
				return false;
			}
			if (Byte < 0x80)
			{
				if (!AppendByte(Out, Byte))
				{
					return false;
				}
			}
			else
			{
				--Index;
				if (!AppendRawUtf8(Out))
				{
					return false;
				}
			}
		}
		return false;
	}

	bool ParseNumber(FStrictJsonValue &Out)
	{
		const int32 Start = Index;
		if (Index < Bytes.Num() && Bytes[Index] == '-')
		{
			++Index;
		}
		if (Index >= Bytes.Num())
		{
			return false;
		}
		if (Bytes[Index] == '0')
		{
			++Index;
			if (Index < Bytes.Num() && Bytes[Index] >= '0' && Bytes[Index] <= '9')
			{
				return false;
			}
		}
		else if (Bytes[Index] >= '1' && Bytes[Index] <= '9')
		{
			while (Index < Bytes.Num() && Bytes[Index] >= '0' && Bytes[Index] <= '9')
			{
				++Index;
			}
		}
		else
		{
			return false;
		}
		if (Index < Bytes.Num() && Bytes[Index] == '.')
		{
			++Index;
			const int32 FractionStart = Index;
			while (Index < Bytes.Num() && Bytes[Index] >= '0' && Bytes[Index] <= '9')
			{
				++Index;
			}
			if (Index == FractionStart)
			{
				return false;
			}
		}
		if (Index < Bytes.Num() && (Bytes[Index] == 'e' || Bytes[Index] == 'E'))
		{
			++Index;
			if (Index < Bytes.Num() && (Bytes[Index] == '+' || Bytes[Index] == '-'))
			{
				++Index;
			}
			const int32 ExponentStart = Index;
			while (Index < Bytes.Num() && Bytes[Index] >= '0' && Bytes[Index] <= '9')
			{
				++Index;
			}
			if (Index == ExponentStart)
			{
				return false;
			}
		}
		const int32 Length = Index - Start;
		if (Length <= 0 || Length > Limits.MaxStringBytes)
		{
			return false;
		}
		Out.Type = EStrictJsonType::Number;
		Out.ScalarBytes.Append(Bytes.GetData() + Start, Length);
		return true;
	}

	bool ParseLiteral(const ANSICHAR *Literal, const EStrictJsonType Type, FStrictJsonValue &Out,
					  const bool bBoolean = false)
	{
		const int32 Length = FCStringAnsi::Strlen(Literal);
		if (Index > Bytes.Num() - Length ||
			FMemory::Memcmp(Bytes.GetData() + Index, Literal, static_cast<SIZE_T>(Length)) != 0)
		{
			return false;
		}
		Index += Length;
		Out.Type = Type;
		Out.bBoolean = bBoolean;
		return true;
	}

	bool ParseArray(const int32 Depth, FStrictJsonValue &Out)
	{
		if (!Consume('['))
		{
			return false;
		}
		Out.Type = EStrictJsonType::Array;
		SkipWhitespace();
		if (Consume(']'))
		{
			return true;
		}
		while (Out.ArrayValues.Num() < Limits.MaxContainerEntries)
		{
			FStrictJsonValue Child;
			if (!ParseValue(Depth + 1, Child))
			{
				return false;
			}
			Out.ArrayValues.Add(MoveTemp(Child));
			SkipWhitespace();
			if (Consume(']'))
			{
				return true;
			}
			if (!Consume(','))
			{
				return false;
			}
			SkipWhitespace();
		}
		return false;
	}

	bool ParseObject(const int32 Depth, FStrictJsonValue &Out)
	{
		if (!Consume('{'))
		{
			return false;
		}
		Out.Type = EStrictJsonType::Object;
		SkipWhitespace();
		if (Consume('}'))
		{
			return true;
		}
		while (Out.ObjectNames.Num() < Limits.MaxContainerEntries)
		{
			TArray<uint8> Name;
			if (!ParseStringBytes(Name))
			{
				SecureResetOAuthWireBytes(Name);
				return false;
			}
			for (const TArray<uint8> &Existing : Out.ObjectNames)
			{
				if (Existing == Name)
				{
					SecureResetOAuthWireBytes(Name);
					return false;
				}
			}
			SkipWhitespace();
			if (!Consume(':'))
			{
				SecureResetOAuthWireBytes(Name);
				return false;
			}
			SkipWhitespace();
			FStrictJsonValue Child;
			if (!ParseValue(Depth + 1, Child))
			{
				SecureResetOAuthWireBytes(Name);
				return false;
			}
			Out.ObjectNames.Add(MoveTemp(Name));
			Out.ObjectValues.Add(MoveTemp(Child));
			SkipWhitespace();
			if (Consume('}'))
			{
				return true;
			}
			if (!Consume(','))
			{
				return false;
			}
			SkipWhitespace();
		}
		return false;
	}

	bool ParseValue(const int32 Depth, FStrictJsonValue &Out)
	{
		if (Depth > Limits.MaxDepth || !ReserveNode() || Index >= Bytes.Num())
		{
			return false;
		}
		switch (Bytes[Index])
		{
		case '{':
			return ParseObject(Depth, Out);
		case '[':
			return ParseArray(Depth, Out);
		case '"':
			Out.Type = EStrictJsonType::String;
			return ParseStringBytes(Out.ScalarBytes);
		case 't':
			return ParseLiteral("true", EStrictJsonType::Boolean, Out, true);
		case 'f':
			return ParseLiteral("false", EStrictJsonType::Boolean, Out, false);
		case 'n':
			return ParseLiteral("null", EStrictJsonType::Null, Out);
		default:
			return ParseNumber(Out);
		}
	}

	TConstArrayView<uint8> Bytes;
	FStrictJsonParseLimits Limits;
	int32 Index = 0;
	int32 Nodes = 0;
};
} // namespace

bool ParseStrictJson(const TConstArrayView<uint8> Bytes, const FStrictJsonParseLimits &Limits,
					 FStrictJsonValue &OutValue)
{
	return FStrictJsonParser(Bytes, Limits).Parse(OutValue);
}

FString StrictBase64UrlEncode(const TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty())
	{
		return {};
	}
	TArray<uint8> Copy;
	Copy.Append(Bytes.GetData(), Bytes.Num());
	FString Result = FBase64::Encode(Copy);
	SecureResetOAuthWireBytes(Copy);
	Result.ReplaceInline(TEXT("+"), TEXT("-"), ESearchCase::CaseSensitive);
	Result.ReplaceInline(TEXT("/"), TEXT("_"), ESearchCase::CaseSensitive);
	while (Result.EndsWith(TEXT("="), ESearchCase::CaseSensitive))
	{
		Result.LeftChopInline(1, EAllowShrinking::No);
	}
	return Result;
}

bool TryStrictBase64UrlDecode(const TConstArrayView<uint8> Encoded, const int32 MaxDecodedBytes,
							  TArray<uint8> &OutDecoded)
{
	SecureResetOAuthWireBytes(OutDecoded);
	if (Encoded.IsEmpty() || MaxDecodedBytes <= 0 || Encoded.Num() > MaxDecodedBytes * 2 || Encoded.Num() % 4 == 1)
	{
		return false;
	}
	FString Text;
	Text.Reserve(Encoded.Num() + 3);
	for (const uint8 Byte : Encoded)
	{
		const bool bAllowed = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') ||
							  (Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '_';
		if (!bAllowed)
		{
			SecureResetOAuthWireString(Text);
			return false;
		}
		Text.AppendChar(Byte == '-' ? TEXT('+') : Byte == '_' ? TEXT('/') : static_cast<TCHAR>(Byte));
	}
	while (Text.Len() % 4 != 0)
	{
		Text.AppendChar(TEXT('='));
	}
	const bool bDecoded = FBase64::Decode(Text, OutDecoded);
	SecureResetOAuthWireString(Text);
	if (!bDecoded || OutDecoded.IsEmpty() || OutDecoded.Num() > MaxDecodedBytes)
	{
		SecureResetOAuthWireBytes(OutDecoded);
		return false;
	}
	FString Canonical = StrictBase64UrlEncode(OutDecoded);
	bool bCanonical = Canonical.Len() == Encoded.Num();
	for (int32 Index = 0; bCanonical && Index < Encoded.Num(); ++Index)
	{
		bCanonical = Canonical[Index] == static_cast<TCHAR>(Encoded[Index]);
	}
	SecureResetOAuthWireString(Canonical);
	if (!bCanonical)
	{
		SecureResetOAuthWireBytes(OutDecoded);
	}
	return bCanonical;
}

bool IsVisibleAsciiBytes(const TConstArrayView<uint8> Bytes, const int32 MaxBytes, const bool bAllowEmpty)
{
	if ((!bAllowEmpty && Bytes.IsEmpty()) || Bytes.Num() > MaxBytes)
	{
		return false;
	}
	for (const uint8 Byte : Bytes)
	{
		if (Byte < 0x21 || Byte > 0x7e)
		{
			return false;
		}
	}
	return true;
}

bool Utf8BytesToString(const TConstArrayView<uint8> Bytes, const int32 MaxBytes, FString &OutValue,
					   const bool bAllowEmpty)
{
	SecureResetOAuthWireString(OutValue);
	if ((!bAllowEmpty && Bytes.IsEmpty()) || Bytes.Num() > MaxBytes)
	{
		return false;
	}
	if (Bytes.IsEmpty())
	{
		return true;
	}
	// ParseStrictJson already validated UTF-8. Revalidate by requiring a lossless round trip for direct callers.
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
	OutValue = FString(Converted.Length(), Converted.Get());
	FTCHARToUTF8 RoundTrip(*OutValue);
	if (RoundTrip.Length() != Bytes.Num() ||
		FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData(), static_cast<SIZE_T>(Bytes.Num())) != 0)
	{
		SecureResetOAuthWireString(OutValue);
		return false;
	}
	return true;
}
} // namespace UE::UnrealAI::Auth::Private
