#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.generated.h"

UENUM(BlueprintType)
enum class EUnrealAIMessageRole : uint8
{
	System UMETA(DisplayName = "system"),
	Developer UMETA(DisplayName = "developer"),
	User UMETA(DisplayName = "user"),
	Assistant UMETA(DisplayName = "assistant"),
	Tool UMETA(DisplayName = "tool")
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIProviderConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	FName Name = TEXT("OpenAI");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	FString BaseUrl = TEXT("https://api.openai.com/v1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	FString BaseUrlEnvironmentVariable = TEXT("OPENAI_BASE_URL");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	FString DefaultModel = TEXT("gpt-5.6-luna");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	FString ModelEnvironmentVariable = TEXT("OPENAI_MODEL");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Authentication")
	bool bRequiresApiKey = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Authentication")
	FString ApiKeyEnvironmentVariable = TEXT("OPENAI_API_KEY");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Authentication", meta = (PasswordField = true))
	FString ApiKeyOverride;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenAI")
	FString OrganizationId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenAI")
	FString ProjectId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HTTP", meta = (ClampMin = "1.0", Units = "s"))
	float TimeoutSeconds = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HTTP")
	TMap<FString, FString> AdditionalHeaders;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIChatMessage
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Message")
	EUnrealAIMessageRole Role = EUnrealAIMessageRole::User;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Message", meta = (MultiLine = true))
	FString Content;

	/** Optional JSON value for multimodal or provider-specific content. Overrides Content when set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Message", meta = (MultiLine = true))
	FString ContentJson;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Message")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Message")
	FString ToolCallId;

	/** Optional JSON object merged into the message payload after standard fields are written. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Message", meta = (MultiLine = true))
	FString AdditionalFieldsJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIChatRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chat")
	FString Model;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chat")
	TArray<FUnrealAIChatMessage> Messages;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
	bool bUseTemperature = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (EditCondition = "bUseTemperature", ClampMin = "0.0", ClampMax = "2.0"))
	float Temperature = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
	bool bUseTopP = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (EditCondition = "bUseTopP", ClampMin = "0.0", ClampMax = "1.0"))
	float TopP = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bUseMaxCompletionTokens = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bUseMaxCompletionTokens", ClampMin = "1"))
	int32 MaxCompletionTokens = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bUseLegacyMaxTokens = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bUseLegacyMaxTokens", ClampMin = "1"))
	int32 MaxTokens = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = "1"))
	int32 NumChoices = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	TArray<FString> StopSequences;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FString User;

	/** Optional JSON object assigned to response_format. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (MultiLine = true))
	FString ResponseFormatJson;

	/** Reserved for future SSE support. Non-streaming requests are currently supported end to end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming")
	bool bStream = false;

	/** Optional JSON object merged into the root request payload after standard fields are written. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (MultiLine = true))
	FString AdditionalParametersJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIUsage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Usage")
	int32 PromptTokens = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Usage")
	int32 CompletionTokens = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Usage")
	int32 TotalTokens = 0;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIChatChoice
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Choice")
	int32 Index = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Choice")
	FString Role;

	UPROPERTY(BlueprintReadOnly, Category = "Choice")
	FString Content;

	UPROPERTY(BlueprintReadOnly, Category = "Choice")
	FString FinishReason;

	UPROPERTY(BlueprintReadOnly, Category = "Choice")
	FString RawMessageJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIChatResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	FString Object;

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	int64 CreatedUnixTime = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	FString Model;

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	TArray<FUnrealAIChatChoice> Choices;

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	FUnrealAIUsage Usage;

	UPROPERTY(BlueprintReadOnly, Category = "Response")
	FString RawJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	bool bIsError = false;

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	int32 HttpStatus = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	FString Param;

	UPROPERTY(BlueprintReadOnly, Category = "Error")
	FString RawJson;
};

DECLARE_DELEGATE_TwoParams(FUnrealAIChatCompletionNativeDelegate, const FUnrealAIChatResponse& /*Response*/, const FUnrealAIError& /*Error*/);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUnrealAIChatCompletionPin, const FUnrealAIChatResponse&, Response, const FUnrealAIError&, Error);
