// Copyright UnrealOps. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"

/** Optional native controls. Defaults also protect the simple UObject and Blueprint APIs. */
struct UNREALAI_API FUnrealAIExecutionLimits
{
	int32 MaxConcurrentRequests = 32;
	int32 MaxRequestBytes = 4 * 1024 * 1024;
	int32 MaxResponseBytes = 16 * 1024 * 1024;
	int32 MaxStreamEvents = 8192;

	bool Validate() const
	{
		return MaxConcurrentRequests > 0 && MaxConcurrentRequests <= 256 && MaxRequestBytes > 0 &&
			   MaxRequestBytes <= 4 * 1024 * 1024 && MaxResponseBytes > 0 && MaxResponseBytes <= 16 * 1024 * 1024 &&
			   MaxStreamEvents > 0 && MaxStreamEvents <= 65536;
	}
};
