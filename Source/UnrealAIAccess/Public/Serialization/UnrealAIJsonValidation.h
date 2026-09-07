// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Provider-neutral pre-DOM JSON limits for untrusted bounded payloads. */
struct UNREALAIACCESS_API FUnrealAIJsonPreflightLimits final
{
	int64 MaxUtf8Bytes = 64 * 1024;
	int32 MaxDepth = 64;
	int32 MaxNotations = 4096;
	int32 MaxContainerEntries = 1024;
	int32 MaxStringTokenCodeUnits = 64 * 1024;
};

/**
 * Performs bounded lexical/structural validation before a recursive JSON DOM
 * parser is entered. Duplicate and case-colliding object keys fail closed.
 */
UNREALAIACCESS_API bool PreflightUnrealAIJson(const FString &Json, const FUnrealAIJsonPreflightLimits &Limits,
											  FString &OutError);
