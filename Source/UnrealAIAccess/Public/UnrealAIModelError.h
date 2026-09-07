// Copyright EngineWorks. All Rights Reserved.

#pragma once
#include "UnrealAIAccessTypes.h"

/** Public-safe error envelope plus separately redacted internal diagnostics. */
struct UNREALAIACCESS_API FUnrealAIModelError
{
	static constexpr int32 MaxCodeUtf8Bytes = 128;
	static constexpr int32 MaxUserMessageUtf8Bytes = 16 * 1024;
	static constexpr int32 MaxDiagnosticUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxProviderRequestIdUtf8Bytes = 1024;
	static constexpr int32 MaxMetadataEntries = 256;
	static constexpr int32 MaxMetadataKeyUtf8Bytes = 128;
	static constexpr int32 MaxMetadataValueUtf8Bytes = 8 * 1024;
	EUnrealAIErrorCategory Category = EUnrealAIErrorCategory::None;
	FName Code;
	FText UserMessage;
	/** Trace/debug-only detail. Callers must redact before construction and must not display it automatically. */
	FString DiagnosticMessage;
	bool bRetryable = false;
	float RetryAfterSeconds = 0.0f;
	FString ProviderRequestId;
	TMap<FName, FString> Metadata;

	bool IsError() const
	{
		return Category != EUnrealAIErrorCategory::None;
	}
	bool ValidateShape(FString &OutError) const;

	/** Validates original storage without a copy that would discard retained allocation. */
	static bool ValidateFields(EUnrealAIErrorCategory Category, FName Code, const FText &UserMessage,
							   const FString &DiagnosticMessage, float RetryAfterSeconds,
							   const FString &ProviderRequestId, const TMap<FName, FString> &Metadata,
							   FString &OutError);
};
