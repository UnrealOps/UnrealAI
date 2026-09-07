// Copyright EngineWorks. All Rights Reserved.

#include "Serialization/UnrealAIJsonValidation.h"

#include "Values/UnrealAIJsonValidationPrivate.h"

bool PreflightUnrealAIJson(const FString &Json, const FUnrealAIJsonPreflightLimits &Limits, FString &OutError)
{
	UE::UnrealAI::JsonValidation::Private::FJsonPreflightLimits InternalLimits;
	InternalLimits.MaxUtf8Bytes = Limits.MaxUtf8Bytes;
	InternalLimits.MaxDepth = Limits.MaxDepth;
	InternalLimits.MaxNotations = Limits.MaxNotations;
	InternalLimits.MaxContainerEntries = Limits.MaxContainerEntries;
	InternalLimits.MaxStringTokenCodeUnits = Limits.MaxStringTokenCodeUnits;
	return UE::UnrealAI::JsonValidation::Private::PreflightJson(Json, InternalLimits, OutError);
}
