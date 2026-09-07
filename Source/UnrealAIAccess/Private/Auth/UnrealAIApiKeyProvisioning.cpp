// Copyright UnrealOps. All Rights Reserved.
#include "Auth/UnrealAIApiKeyProvisioning.h"

bool FUnrealAIApiKeyProvisionResult::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!RequestId.IsValid())
	{
		OutError = TEXT("API-key provision result requires a valid request ID.");
		return false;
	}
	const bool bSuccess = Kind == EUnrealAIApiKeyProvisionKind::Stored || Kind == EUnrealAIApiKeyProvisionKind::Deleted;
	if (bSuccess)
	{
		if (Error.IsError())
		{
			OutError = TEXT("Successful API-key provisioning cannot contain an error.");
			return false;
		}
		return true;
	}
	if ((Kind != EUnrealAIApiKeyProvisionKind::Failed && Kind != EUnrealAIApiKeyProvisionKind::Cancelled &&
		 Kind != EUnrealAIApiKeyProvisionKind::TimedOut) ||
		!Error.IsError())
	{
		OutError = TEXT("API-key provision result has an invalid terminal/error combination.");
		return false;
	}
	if ((Kind == EUnrealAIApiKeyProvisionKind::Cancelled && Error.Category != EUnrealAIErrorCategory::Cancelled) ||
		(Kind == EUnrealAIApiKeyProvisionKind::TimedOut && Error.Category != EUnrealAIErrorCategory::Timeout))
	{
		OutError = TEXT("API-key provision cancellation or timeout category is inconsistent.");
		return false;
	}
	return Error.ValidateShape(OutError);
}
