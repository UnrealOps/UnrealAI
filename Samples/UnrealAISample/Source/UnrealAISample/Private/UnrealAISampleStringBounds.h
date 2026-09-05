#pragma once

#include "Containers/StringConv.h"
#include "CoreMinimal.h"

namespace UnrealAISampleStringBounds
{
	/**
	 * Return a prefix no longer than MaxLength without cutting a UTF-16
	 * surrogate pair. Unreal's desktop TCHAR is UTF-16 on every supported host.
	 */
	inline FString LeftAtCodePointBoundary(const FString& Value, int32 MaxLength)
	{
		int32 PrefixLength = FMath::Clamp(MaxLength, 0, Value.Len());
		if (PrefixLength > 0
			&& PrefixLength < Value.Len()
			&& StringConv::IsHighSurrogate(static_cast<uint32>(Value[PrefixLength - 1]))
			&& StringConv::IsLowSurrogate(static_cast<uint32>(Value[PrefixLength])))
		{
			--PrefixLength;
		}
		return Value.Left(PrefixLength);
	}
}
