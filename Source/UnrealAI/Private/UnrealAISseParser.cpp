#include "UnrealAISseParser.h"

#include "Containers/StringConv.h"

namespace UnrealAISseParserPrivate
{
	FUnrealAIError MakeProtocolError(const FString& Message)
	{
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.Type = TEXT("stream_protocol_error");
		Error.Code = TEXT("invalid_sse");
		Error.Message = Message;
		return Error;
	}

	FString Utf8BytesToString(TConstArrayView<uint8> Bytes)
	{
		if (Bytes.Num() == 0)
		{
			return FString();
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}
}

bool FUnrealAISseParser::Append(
	const uint8* Data,
	int64 Length,
	TArray<FUnrealAISseEvent>& OutEvents,
	FUnrealAIError& OutError)
{
	if (!Data || Length <= 0)
	{
		return true;
	}

	const uint8* Cursor = Data;
	int64 RemainingBytes = Length;
	while (RemainingBytes > 0)
	{
		const int32 ChunkLength = static_cast<int32>(FMath::Min<int64>(RemainingBytes, MaxEventBytes));
		PendingBytes.Append(Cursor, ChunkLength);
		if (!ConsumeLines(false, OutEvents, OutError))
		{
			return false;
		}
		Cursor += ChunkLength;
		RemainingBytes -= ChunkLength;
	}
	return true;
}

bool FUnrealAISseParser::Finish(TArray<FUnrealAISseEvent>& OutEvents, FUnrealAIError& OutError)
{
	if (!ConsumeLines(true, OutEvents, OutError))
	{
		return false;
	}
	DispatchEvent(OutEvents);
	return true;
}

bool FUnrealAISseParser::ConsumeLines(
	bool bAtEnd,
	TArray<FUnrealAISseEvent>& OutEvents,
	FUnrealAIError& OutError)
{
	int32 ConsumedBytes = 0;
	PendingScanOffset = FMath::Clamp(PendingScanOffset, 0, PendingBytes.Num());
	while (ConsumedBytes < PendingBytes.Num())
	{
		int32 LineEnd = INDEX_NONE;
		int32 TerminatorLength = 0;
		int32 NextScanOffset = PendingBytes.Num();
		for (int32 Index = FMath::Max(ConsumedBytes, PendingScanOffset); Index < PendingBytes.Num(); ++Index)
		{
			if (PendingBytes[Index] == '\n')
			{
				LineEnd = Index;
				TerminatorLength = 1;
				break;
			}
			if (PendingBytes[Index] == '\r')
			{
				if (Index + 1 >= PendingBytes.Num() && !bAtEnd)
				{
					NextScanOffset = Index;
					break;
				}
				LineEnd = Index;
				TerminatorLength = Index + 1 < PendingBytes.Num() && PendingBytes[Index + 1] == '\n' ? 2 : 1;
				break;
			}
		}

		if (LineEnd == INDEX_NONE)
		{
			PendingScanOffset = NextScanOffset;
			break;
		}

		const TConstArrayView<uint8> Line(PendingBytes.GetData() + ConsumedBytes, LineEnd - ConsumedBytes);
		if (!ProcessLine(Line, OutEvents, OutError))
		{
			return false;
		}
		ConsumedBytes = LineEnd + TerminatorLength;
		PendingScanOffset = ConsumedBytes;
	}

	if (ConsumedBytes > 0)
	{
		PendingBytes.RemoveAt(0, ConsumedBytes, EAllowShrinking::No);
		PendingScanOffset = FMath::Max(0, PendingScanOffset - ConsumedBytes);
	}

	if (bAtEnd && PendingBytes.Num() > 0)
	{
		const bool bProcessed = ProcessLine(PendingBytes, OutEvents, OutError);
		PendingBytes.Reset();
		PendingScanOffset = 0;
		return bProcessed;
	}

	if (PendingBytes.Num() + CurrentEventBytes > MaxEventBytes)
	{
		OutError = UnrealAISseParserPrivate::MakeProtocolError(
			TEXT("An SSE event exceeded the 1 MiB safety limit."));
		return false;
	}
	return true;
}

bool FUnrealAISseParser::ProcessLine(
	TConstArrayView<uint8> LineBytes,
	TArray<FUnrealAISseEvent>& OutEvents,
	FUnrealAIError& OutError)
{
	CurrentEventBytes += LineBytes.Num();
	if (CurrentEventBytes > MaxEventBytes)
	{
		OutError = UnrealAISseParserPrivate::MakeProtocolError(
			TEXT("An SSE event exceeded the 1 MiB safety limit."));
		return false;
	}

	const FString Line = UnrealAISseParserPrivate::Utf8BytesToString(LineBytes);
	if (Line.IsEmpty())
	{
		DispatchEvent(OutEvents);
		return true;
	}
	if (Line.StartsWith(TEXT(":")))
	{
		return true;
	}

	FString Field;
	FString Value;
	if (!Line.Split(TEXT(":"), &Field, &Value))
	{
		Field = Line;
	}
	if (Value.StartsWith(TEXT(" ")))
	{
		Value.RightChopInline(1);
	}

	if (Field == TEXT("event"))
	{
		CurrentEventType = Value;
	}
	else if (Field == TEXT("data"))
	{
		DataLines.Add(Value);
	}
	else if (Field == TEXT("id"))
	{
		CurrentId = Value;
	}
	return true;
}

void FUnrealAISseParser::DispatchEvent(TArray<FUnrealAISseEvent>& OutEvents)
{
	if (DataLines.Num() > 0)
	{
		FUnrealAISseEvent& Event = OutEvents.AddDefaulted_GetRef();
		Event.EventType = CurrentEventType.IsEmpty() ? TEXT("message") : CurrentEventType;
		Event.Data = FString::Join(DataLines, TEXT("\n"));
		Event.Id = CurrentId;
	}

	DataLines.Reset();
	CurrentEventType.Reset();
	CurrentEventBytes = 0;
}
