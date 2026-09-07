// Copyright EngineWorks. All Rights Reserved.

#include "Runtime/UnrealAIFailureClassification.h"

#include "Serialization/UnrealAIJsonValidation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UnrealAI::Reliability
{
bool IsPermanentQuotaCode(const FString &Code)
{
	static const TCHAR *Codes[] = {
		TEXT("insufficient_quota"),	 TEXT("billing_hard_limit_reached"),   TEXT("billing_not_active"),
																				TEXT("usage_limit_reached"), TEXT("enforced_spend_limit_reached"), TEXT("credit_balance_too_low")};
	for (const TCHAR *Known : Codes)
	{
		if (Code.Equals(Known, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool IsPermanentQuotaResponse(TConstArrayView<uint8> Body)
{
	if (Body.IsEmpty() || Body.Num() > MaximumErrorBodyBytes)
	{
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Body.GetData()), Body.Num());
	const FString Json(Converted.Length(), Converted.Get());
	// Reject malformed UTF-8, duplicate keys, and excessive nesting before allocating a DOM.
	const FTCHARToUTF8 RoundTrip(*Json);
	if (RoundTrip.Length() != Body.Num() || FMemory::Memcmp(RoundTrip.Get(), Body.GetData(), Body.Num()) != 0)
	{
		return false;
	}
	FString Error;
	FUnrealAIJsonPreflightLimits Limits;
	Limits.MaxUtf8Bytes = MaximumErrorBodyBytes;
	if (!PreflightUnrealAIJson(Json, Limits, Error))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> *ProviderError = nullptr;
	if (!Root->TryGetObjectField(TEXT("error"), ProviderError))
	{
		Root->TryGetObjectField(TEXT("details"), ProviderError);
	}
	if (!ProviderError || !ProviderError->IsValid())
	{
		return false;
	}
	FString Code;
	FString Type;
	if (!(*ProviderError)->TryGetStringField(TEXT("code"), Code))
	{
		(*ProviderError)->TryGetStringField(TEXT("error_code"), Code);
	}
	(*ProviderError)->TryGetStringField(TEXT("type"), Type);
	return IsPermanentQuotaCode(Code) || IsPermanentQuotaCode(Type);
}

bool IsRetryableHttpStatus(int32 Status)
{
	return Status == 408 || Status == 409 || Status == 429 || Status == 500 || Status == 502 || Status == 503 ||
		   Status == 504 || Status == 529;
}
} // namespace UE::UnrealAI::Reliability
