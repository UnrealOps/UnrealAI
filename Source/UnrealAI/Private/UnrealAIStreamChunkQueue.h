#pragma once

#include "CoreMinimal.h"

/** Bounded byte queue used to hand HTTP stream chunks to the game thread. */
class FUnrealAIStreamChunkQueue
{
public:
	static constexpr int64 DefaultMaxQueuedBytes = 4 * 1024 * 1024;

	explicit FUnrealAIStreamChunkQueue(int64 InMaxQueuedBytes = DefaultMaxQueuedBytes)
		: MaxQueuedBytes(FMath::Max<int64>(0, InMaxQueuedBytes))
	{
	}

	bool Enqueue(const uint8* Data, int64 Length)
	{
		if (!Data || Length <= 0 || Length > MAX_int32 || Length > MaxQueuedBytes - QueuedByteCount)
		{
			return false;
		}

		TArray<uint8>& Chunk = Chunks.AddDefaulted_GetRef();
		Chunk.Append(Data, static_cast<int32>(Length));
		QueuedByteCount += Length;
		return true;
	}

	void Drain(TArray<TArray<uint8>>& OutChunks)
	{
		OutChunks = MoveTemp(Chunks);
		Chunks.Reset();
		QueuedByteCount = 0;
	}

	void Reset()
	{
		Chunks.Reset();
		QueuedByteCount = 0;
	}

	bool IsEmpty() const
	{
		return Chunks.IsEmpty();
	}

	int64 GetQueuedByteCount() const
	{
		return QueuedByteCount;
	}

private:
	TArray<TArray<uint8>> Chunks;
	int64 MaxQueuedBytes = 0;
	int64 QueuedByteCount = 0;
};
