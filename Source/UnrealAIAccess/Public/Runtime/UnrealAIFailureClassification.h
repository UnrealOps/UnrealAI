// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Shared failure classification. No response content is returned or logged. */
namespace UE::UnrealAI::Reliability
{
constexpr int32 MaximumErrorBodyBytes = 64 * 1024;

UNREALAIACCESS_API bool IsPermanentQuotaCode(const FString &Code);
UNREALAIACCESS_API bool IsPermanentQuotaResponse(TConstArrayView<uint8> Body);
UNREALAIACCESS_API bool IsRetryableHttpStatus(int32 Status);
} // namespace UE::UnrealAI::Reliability
