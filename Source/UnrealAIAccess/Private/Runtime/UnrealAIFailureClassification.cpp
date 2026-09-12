// Copyright UnrealOps. All Rights Reserved.

#include "Runtime/UnrealAIFailureClassification.h"

#include "Serialization/UnrealAIJsonValidation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UnrealAI::Reliability
{
namespace
{
bool TryParseErrorBody(TConstArrayView<uint8> Body, TSharedPtr<FJsonObject> &Root)
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
	return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid();
}
} // namespace

bool IsPermanentQuotaCode(const FString &Code)
{
	static const TCHAR *Codes[] = {
		TEXT("insufficient_quota"), TEXT("billing_hard_limit_reached"), TEXT("billing_not_active"),
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
	TSharedPtr<FJsonObject> Root;
	if (!TryParseErrorBody(Body, Root))
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

FString GetPublicHttpFailureSummary(TConstArrayView<uint8> Body)
{
	TSharedPtr<FJsonObject> Root;
	if (!TryParseErrorBody(Body, Root))
	{
		return FString();
	}
	FString Message;
	Root->TryGetStringField(TEXT("detail"), Message);
	Root->TryGetStringField(TEXT("error"), Message);
	const TSharedPtr<FJsonObject> *NestedError = nullptr;
	if (Root->TryGetObjectField(TEXT("error"), NestedError) && NestedError && NestedError->IsValid())
	{
		Root = *NestedError;
	}
	Root->TryGetStringField(TEXT("message"), Message);
	FString Code;
	FString Parameter;
	Root->TryGetStringField(TEXT("code"), Code);
	Root->TryGetStringField(TEXT("param"), Parameter);
	// Only known structural parameter names and exact provider messages may produce public text.
	static const TCHAR *Parameters[] = {TEXT("max_output_tokens"), TEXT("temperature"), TEXT("top_p"),
		TEXT("store"), TEXT("stream"), TEXT("instructions"), TEXT("tools"), TEXT("tool_choice"),
		TEXT("parallel_tool_calls"), TEXT("text")};
	for (const TCHAR *Known : Parameters)
	{
		const bool bStructuredMatch = Code == TEXT("unsupported_parameter") && Parameter == Known;
		const bool bMessageMatch = Message == FString::Printf(TEXT("Unsupported parameter: %s"), Known) ||
			Message == FString::Printf(TEXT("Unsupported parameter: '%s'"), Known) ||
			Message == FString::Printf(TEXT("Unsupported parameter: '%s'."), Known);
		if (bStructuredMatch || bMessageMatch)
		{
			return FString::Printf(TEXT("Unsupported request parameter: %s."), Known);
		}
	}
	const bool bUnsupportedSubscriptionModel = Message.StartsWith(TEXT("The '"), ESearchCase::CaseSensitive) &&
		Message.EndsWith(TEXT("' model is not supported when using Codex with a ChatGPT account."), ESearchCase::CaseSensitive);
	if (Code == TEXT("model_not_found") || Code == TEXT("unsupported_model") || bUnsupportedSubscriptionModel)
	{
		return TEXT("The selected model is unavailable for this connection.");
	}
	if (Code == TEXT("invalid_json_schema") || Code == TEXT("invalid_function_parameters"))
	{
		return TEXT("The provider rejected a tool or output JSON schema.");
	}
	return FString();
}

bool IsRetryableHttpStatus(int32 Status)
{
	return Status == 408 || Status == 409 || Status == 429 || Status == 500 || Status == 502 || Status == 503 ||
		   Status == 504 || Status == 529;
}
} // namespace UE::UnrealAI::Reliability
