// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Math/NumericLimits.h"

namespace UE::UnrealAI::PhysicalAllocation::Private
{
inline bool TryAdd(const int64 A, const int64 B, int64 &Out)
{
	if (A < 0 || B < 0 || A > TNumericLimits<int64>::Max() - B)
	{
		return false;
	}
	Out = A + B;
	return true;
}

inline bool TryMultiply(const int64 A, const int64 B, int64 &Out)
{
	if (A < 0 || B < 0 || (A != 0 && B > TNumericLimits<int64>::Max() / A))
	{
		return false;
	}
	Out = A * B;
	return true;
}

inline bool TryAccumulate(const SIZE_T AdditionalBytes, const int64 MaximumBytes, int64 &InOutBytes)
{
	if (MaximumBytes < 0 || InOutBytes < 0 || InOutBytes > MaximumBytes ||
		AdditionalBytes > static_cast<SIZE_T>(MaximumBytes - InOutBytes))
	{
		return false;
	}
	InOutBytes += static_cast<int64>(AdditionalBytes);
	return true;
}

inline bool TryGetMaximumStringAllocatedBytes(const int64 MaxUtf8Bytes, int64 &OutMaximumBytes)
{
	// UTF-8 is never shorter than one byte per serialized code unit. Allow 2x
	// TCHAR storage plus a small allocator-granularity margin, but reject caller-
	// controlled reserve slack before scanning or copying the string.
	int64 CharacterCapacity = 0;
	int64 CharacterBytes = 0;
	int64 DoubledBytes = 0;
	return TryAdd(MaxUtf8Bytes, 1, CharacterCapacity) &&
		   TryMultiply(CharacterCapacity, static_cast<int64>(sizeof(TCHAR)), CharacterBytes) &&
		   TryMultiply(CharacterBytes, 2, DoubledBytes) && TryAdd(DoubledBytes, 64, OutMaximumBytes);
}

inline bool TryGetMaximumAggregateStringAllocatedBytes(const int64 MaxAggregateUtf8Bytes, const int32 MaxStrings,
													   int64 &OutMaximumBytes)
{
	// Share the logical aggregate across all strings while retaining a bounded
	// terminator and allocator-granularity allowance for each allocation.
	int64 CharactersWithTerminators = 0;
	int64 CharacterBytes = 0;
	int64 DoubledBytes = 0;
	int64 AllocatorMarginBytes = 0;
	return MaxStrings >= 0 && TryAdd(MaxAggregateUtf8Bytes, MaxStrings, CharactersWithTerminators) &&
		   TryMultiply(CharactersWithTerminators, static_cast<int64>(sizeof(TCHAR)), CharacterBytes) &&
		   TryMultiply(CharacterBytes, 2, DoubledBytes) && TryMultiply(MaxStrings, 64, AllocatorMarginBytes) &&
		   TryAdd(DoubledBytes, AllocatorMarginBytes, OutMaximumBytes);
}

inline bool HasBoundedStringStorage(const FString &Value, const int64 MaxUtf8Bytes)
{
	int64 MaximumAllocatedBytes = 0;
	return MaxUtf8Bytes >= 0 && static_cast<int64>(Value.Len()) <= MaxUtf8Bytes &&
		   TryGetMaximumStringAllocatedBytes(MaxUtf8Bytes, MaximumAllocatedBytes) &&
		   Value.GetAllocatedSize() <= static_cast<SIZE_T>(MaximumAllocatedBytes);
}

inline bool TryGetMaximumArrayAllocatedBytes(const int32 MaxEntries, const SIZE_T ElementSize, int64 &OutMaximumBytes)
{
	int64 EntriesWithTerminator = 0;
	int64 LogicalBytes = 0;
	int64 DoubledBytes = 0;
	return MaxEntries >= 0 && ElementSize <= static_cast<SIZE_T>(TNumericLimits<int64>::Max()) &&
		   TryAdd(static_cast<int64>(MaxEntries), 1, EntriesWithTerminator) &&
		   TryMultiply(EntriesWithTerminator, static_cast<int64>(ElementSize), LogicalBytes) &&
		   TryMultiply(LogicalBytes, 2, DoubledBytes) && TryAdd(DoubledBytes, 4096, OutMaximumBytes);
}

inline bool HasBoundedArrayStorage(const SIZE_T AllocatedBytes, const int32 NumEntries, const int32 MaxEntries,
								   const SIZE_T ElementSize)
{
	int64 MaximumAllocatedBytes = 0;
	return NumEntries >= 0 && NumEntries <= MaxEntries &&
		   TryGetMaximumArrayAllocatedBytes(MaxEntries, ElementSize, MaximumAllocatedBytes) &&
		   AllocatedBytes <= static_cast<SIZE_T>(MaximumAllocatedBytes);
}

inline bool TryGetMaximumSparseContainerAllocatedBytes(const int32 MaxEntries, const SIZE_T ElementSize,
													   int64 &OutMaximumBytes)
{
	// Sparse sets/maps need element storage, allocation flags, and hash buckets.
	// Four times the bounded logical footprint plus 4 KiB tolerates allocator
	// policy differences without accepting attacker-controlled removed slots.
	int64 EntriesWithTerminator = 0;
	int64 BytesPerEntry = 0;
	int64 LogicalBytes = 0;
	int64 ExpandedBytes = 0;
	return MaxEntries >= 0 && ElementSize <= static_cast<SIZE_T>(TNumericLimits<int64>::Max()) &&
		   TryAdd(static_cast<int64>(MaxEntries), 1, EntriesWithTerminator) &&
		   TryAdd(static_cast<int64>(ElementSize), 32, BytesPerEntry) &&
		   TryMultiply(EntriesWithTerminator, BytesPerEntry, LogicalBytes) &&
		   TryMultiply(LogicalBytes, 4, ExpandedBytes) && TryAdd(ExpandedBytes, 4096, OutMaximumBytes);
}

inline bool HasBoundedSparseContainerStorage(const SIZE_T AllocatedBytes, const int32 NumEntries,
											 const int32 MaxEntries, const SIZE_T ElementSize)
{
	int64 MaximumAllocatedBytes = 0;
	return NumEntries >= 0 && NumEntries <= MaxEntries &&
		   TryGetMaximumSparseContainerAllocatedBytes(MaxEntries, ElementSize, MaximumAllocatedBytes) &&
		   AllocatedBytes <= static_cast<SIZE_T>(MaximumAllocatedBytes);
}

inline bool TryGetMaximumVariantAllocatedBytes(const int64 MaxAggregateUtf8Bytes, const int32 MaxNodes,
											   const SIZE_T NodeSize, int64 &OutMaximumBytes)
{
	// Keep this formula aligned with safe-variant validation. The per-node
	// allowance covers the arena, compact child containers, hashes, string
	// terminators, and allocator rounding.
	constexpr int64 CompactContainerAndAllocatorOverheadBytesPerNode = 512;
	int64 MaximumStringStorageBytes = 0;
	int64 NodeAndOverheadBytes = 0;
	int64 MaximumArenaAndContainerBytes = 0;
	return MaxNodes >= 0 && NodeSize <= static_cast<SIZE_T>(TNumericLimits<int64>::Max()) &&
		   TryMultiply(MaxAggregateUtf8Bytes, static_cast<int64>(sizeof(TCHAR)), MaximumStringStorageBytes) &&
		   TryAdd(static_cast<int64>(NodeSize), CompactContainerAndAllocatorOverheadBytesPerNode,
				  NodeAndOverheadBytes) &&
		   TryMultiply(MaxNodes, NodeAndOverheadBytes, MaximumArenaAndContainerBytes) &&
		   TryAdd(MaximumStringStorageBytes, MaximumArenaAndContainerBytes, OutMaximumBytes);
}
} // namespace UE::UnrealAI::PhysicalAllocation::Private
