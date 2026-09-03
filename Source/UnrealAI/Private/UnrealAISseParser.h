#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.h"

struct FUnrealAISseEvent
{
	FString EventType;
	FString Data;
	FString Id;
};

/** Incremental UTF-8 Server-Sent Events decoder. */
class FUnrealAISseParser
{
public:
	static constexpr int32 MaxEventBytes = 1024 * 1024;

	bool Append(const uint8* Data, int64 Length, TArray<FUnrealAISseEvent>& OutEvents, FUnrealAIError& OutError);
	bool Finish(TArray<FUnrealAISseEvent>& OutEvents, FUnrealAIError& OutError);

private:
	TArray<uint8> PendingBytes;
	TArray<FString> DataLines;
	FString CurrentEventType;
	FString CurrentId;
	int32 CurrentEventBytes = 0;
	int32 PendingScanOffset = 0;

	bool ConsumeLines(bool bAtEnd, TArray<FUnrealAISseEvent>& OutEvents, FUnrealAIError& OutError);
	bool ProcessLine(TConstArrayView<uint8> LineBytes, TArray<FUnrealAISseEvent>& OutEvents, FUnrealAIError& OutError);
	void DispatchEvent(TArray<FUnrealAISseEvent>& OutEvents);
};
