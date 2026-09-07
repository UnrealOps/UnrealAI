// Copyright UnrealOps. All Rights Reserved.
#include "UnrealAIModelError.h"
#include "Values/UnrealAIPhysicalAllocationValidationPrivate.h"
#include "Values/UnrealAITextValidationPrivate.h"

namespace
{
bool FitsCommonTypeUtf8(const FString &Value, const int32 MaxBytes)
{
	return UE::UnrealAI::PhysicalAllocation::Private::HasBoundedStringStorage(Value, MaxBytes) &&
		   UE::UnrealAI::TextValidation::Private::IsWellFormedSerializedString(Value) &&
		   UE::UnrealAI::TextValidation::Private::Utf8Length(Value) <= MaxBytes;
}

bool HasBoundedErrorMetadataStorage(const TMap<FName, FString> &Metadata)
{
	using namespace UE::UnrealAI::PhysicalAllocation::Private;
	if (!HasBoundedSparseContainerStorage(Metadata.GetAllocatedSize(), Metadata.Num(),
										  FUnrealAIModelError::MaxMetadataEntries, sizeof(TPair<FName, FString>)))
	{
		return false;
	}

	int64 MaximumContainerBytes = 0;
	int64 MaximumValueBytes = 0;
	int64 MaximumNestedBytes = 0;
	int64 MaximumTotalBytes = 0;
	if (!TryGetMaximumSparseContainerAllocatedBytes(FUnrealAIModelError::MaxMetadataEntries,
													sizeof(TPair<FName, FString>), MaximumContainerBytes) ||
		!TryGetMaximumStringAllocatedBytes(FUnrealAIModelError::MaxMetadataValueUtf8Bytes, MaximumValueBytes) ||
		!TryMultiply(FUnrealAIModelError::MaxMetadataEntries, MaximumValueBytes, MaximumNestedBytes) ||
		!TryAdd(MaximumContainerBytes, MaximumNestedBytes, MaximumTotalBytes))
	{
		return false;
	}

	int64 AllocatedBytes = static_cast<int64>(Metadata.GetAllocatedSize());
	for (const TPair<FName, FString> &Pair : Metadata)
	{
		if (!HasBoundedStringStorage(Pair.Value, FUnrealAIModelError::MaxMetadataValueUtf8Bytes) ||
			!TryAccumulate(Pair.Value.GetAllocatedSize(), MaximumTotalBytes, AllocatedBytes))
		{
			return false;
		}
	}
	return true;
}
} // namespace

bool FUnrealAIModelError::ValidateShape(FString &OutError) const
{
	return ValidateFields(Category, Code, UserMessage, DiagnosticMessage, RetryAfterSeconds, ProviderRequestId,
						  Metadata, OutError);
}

bool FUnrealAIModelError::ValidateFields(const EUnrealAIErrorCategory Category, const FName Code,
										 const FText &UserMessage, const FString &DiagnosticMessage,
										 const float RetryAfterSeconds, const FString &ProviderRequestId,
										 const TMap<FName, FString> &Metadata, FString &OutError)
{
	OutError.Reset();
	if (!HasBoundedErrorMetadataStorage(Metadata))
	{
		OutError = TEXT("Error retains excessive metadata allocation.");
		return false;
	}
	FString StableUserMessage;
	int64 UserMessageBytes = 0;
	if (!UE::UnrealAI::TextValidation::Private::TryGetBoundedStableSource(UserMessage, MaxUserMessageUtf8Bytes,
																		  StableUserMessage, UserMessageBytes, OutError,
																		  TEXT("Error user message")))
	{
		return false;
	}
	if (static_cast<uint8>(Category) > static_cast<uint8>(EUnrealAIErrorCategory::Internal))
	{
		OutError = TEXT("Error has an unknown category.");
		return false;
	}
	if (!FitsCommonTypeUtf8(Code.ToString(), MaxCodeUtf8Bytes) ||
		!FitsCommonTypeUtf8(DiagnosticMessage, MaxDiagnosticUtf8Bytes) ||
		!FitsCommonTypeUtf8(ProviderRequestId, MaxProviderRequestIdUtf8Bytes) || Metadata.Num() > MaxMetadataEntries)
	{
		OutError = TEXT("Error exceeds code, message, provider ID, or metadata bounds.");
		return false;
	}
	if (Category != EUnrealAIErrorCategory::None && Code.IsNone())
	{
		OutError = TEXT("Non-empty error categories require a stable machine code.");
		return false;
	}
	if (!FMath::IsFinite(RetryAfterSeconds) || RetryAfterSeconds < 0.0f)
	{
		OutError = TEXT("Error retry delay must be finite and non-negative.");
		return false;
	}
	for (const TPair<FName, FString> &Pair : Metadata)
	{
		if (Pair.Key.IsNone() || !FitsCommonTypeUtf8(Pair.Key.ToString(), MaxMetadataKeyUtf8Bytes) ||
			!FitsCommonTypeUtf8(Pair.Value, MaxMetadataValueUtf8Bytes))
		{
			OutError = TEXT("Error contains invalid or oversized metadata.");
			return false;
		}
	}
	return true;
}
