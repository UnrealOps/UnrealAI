#pragma once

#include "CoreMinimal.h"
#include "UnrealAIRequestLifetime.h"
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

UENUM(BlueprintType)
enum class EUnrealAIProviderApi : uint8
{
	OpenAICompatibleChatCompletions UMETA(DisplayName = "OpenAI-Compatible Chat Completions"),
	AnthropicMessages UMETA(DisplayName = "Anthropic Messages"),
	GeminiGenerateContent UMETA(DisplayName = "Google Gemini Generate Content")
};

UENUM(BlueprintType)
enum class EUnrealAIResponseApi : uint8
{
	ProviderDefault,
	ChatProtocol,
	OpenAIResponses
};

UENUM(BlueprintType)
enum class EUnrealAIRetryMode : uint8
{
	UseProviderPolicy UMETA(DisplayName = "Use Provider Policy"),
	Disabled UMETA(DisplayName = "Disabled"),
	OverrideMaxRetries UMETA(DisplayName = "Override Maximum Retries")
};

UENUM(BlueprintType)
enum class EUnrealAIRetryReason : uint8
{
	ConnectionError UMETA(DisplayName = "Connection Error"),
	Timeout UMETA(DisplayName = "Timeout"),
	HttpError UMETA(DisplayName = "HTTP Error"),
	EmptyStream UMETA(DisplayName = "Empty Stream")
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIRetryPolicy
{
	GENERATED_BODY()

	/** Number of retries after the initial attempt. Zero disables automatic retries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Retry", meta = (ClampMin = "0", ClampMax = "10"))
	int32 MaxRetries = 2;

	/** Base delay before the first retry. Later retries use exponential backoff. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Retry",
			  meta = (ClampMin = "0.1", ClampMax = "3600.0", Units = "s"))
	float InitialDelaySeconds = 1.0f;

	/** Maximum time UnrealAI is willing to wait before one retry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Retry",
			  meta = (ClampMin = "0.1", ClampMax = "3600.0", Units = "s"))
	float MaxDelaySeconds = 60.0f;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIRequestRetryOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Retry")
	EUnrealAIRetryMode Mode = EUnrealAIRetryMode::UseProviderPolicy;

	/** Used only when Mode is OverrideMaxRetries. Zero disables retries for this request. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Retry",
			  meta = (EditCondition = "Mode == EUnrealAIRetryMode::OverrideMaxRetries", ClampMin = "0",
					  ClampMax = "10"))
	int32 MaxRetries = 2;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIProviderConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	FName Name = TEXT("OpenAI");

	/** Selects the provider wire protocol. Existing profiles default to OpenAI-compatible Chat Completions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	EUnrealAIProviderApi Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;

	/** Protocol for CreateResponse/StreamResponse only. Chat methods retain Api. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Provider")
	EUnrealAIResponseApi ResponseApi = EUnrealAIResponseApi::ProviderDefault;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Authentication",
			  meta = (PasswordField = true, DeprecatedProperty,
					  DeprecationMessage = "Use environment credentials or an injected SDK credential source."))
	FString ApiKeyOverride;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenAI")
	FString OrganizationId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenAI")
	FString ProjectId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HTTP", meta = (ClampMin = "1.0", Units = "s"))
	float TimeoutSeconds = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HTTP")
	TMap<FString, FString> AdditionalHeaders;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HTTP|Retry")
	FUnrealAIRetryPolicy RetryPolicy;
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling",
			  meta = (EditCondition = "bUseTemperature", ClampMin = "0.0", ClampMax = "2.0"))
	float Temperature = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
	bool bUseTopP = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling",
			  meta = (EditCondition = "bUseTopP", ClampMin = "0.0", ClampMax = "1.0"))
	float TopP = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bUseMaxCompletionTokens = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output",
			  meta = (EditCondition = "bUseMaxCompletionTokens", ClampMin = "1"))
	int32 MaxCompletionTokens = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bUseLegacyMaxTokens = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output",
			  meta = (EditCondition = "bUseLegacyMaxTokens", ClampMin = "1"))
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

	/** Deprecated. Use StreamChatCompletion or the Stream Chat Completion Blueprint node. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming",
			  meta = (DeprecatedProperty, DeprecationMessage = "Use the dedicated UnrealAI streaming API instead."))
	bool bStream = false;

	/** Optional JSON object merged into the root request payload after standard fields are written. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (MultiLine = true))
	FString AdditionalParametersJson;

	/** Inherit, disable, or override the configured provider's retry count for this request. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reliability")
	FUnrealAIRequestRetryOptions RetryOptions;
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

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIRequestHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Request")
	FGuid Id;

	/** Native observation only; intentionally excluded from serialization and Blueprint pins. */
	TSharedPtr<const IUnrealAIRequestLifetime, ESPMode::ThreadSafe> Lifetime;

	bool IsValid() const
	{
		return Id.IsValid();
	}
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIRetryEvent
{
	GENERATED_BODY()

	/** Identifies the logical request across every HTTP attempt. */
	UPROPERTY(BlueprintReadOnly, Category = "Retry")
	FUnrealAIRequestHandle RequestHandle;

	/** One-based retry number. The initial HTTP attempt is not a retry. */
	UPROPERTY(BlueprintReadOnly, Category = "Retry")
	int32 RetryNumber = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Retry")
	int32 MaxRetries = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Retry", meta = (Units = "s"))
	float DelaySeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Retry")
	EUnrealAIRetryReason Reason = EUnrealAIRetryReason::HttpError;

	/** HTTP response status, or zero when no provider response was received. */
	UPROPERTY(BlueprintReadOnly, Category = "Retry")
	int32 HttpStatus = 0;
};

UENUM(BlueprintType)
enum class EUnrealAIChatStreamEventType : uint8
{
	TextDelta UMETA(DisplayName = "Text Delta"),
	ChoiceFinished UMETA(DisplayName = "Choice Finished"),
	Usage UMETA(DisplayName = "Usage"),
	ProviderEvent UMETA(DisplayName = "Provider Event")
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIChatStreamEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	EUnrealAIChatStreamEventType Type = EUnrealAIChatStreamEventType::ProviderEvent;

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	int32 ChoiceIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Stream", meta = (MultiLine = true))
	FString TextDelta;

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	FString Role;

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	FString FinishReason;

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	FUnrealAIUsage Usage;

	/** Native SSE event name or provider JSON event type when one is available. */
	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	FString ProviderEventType;

	/** Provider event data. Treat this as potentially sensitive response content. */
	UPROPERTY(BlueprintReadOnly, Category = "Stream", meta = (MultiLine = true))
	FString RawJson;
};

UENUM(BlueprintType)
enum class EUnrealAIChatStreamStatus : uint8
{
	Completed,
	Failed,
	Cancelled
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIChatStreamResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	EUnrealAIChatStreamStatus Status = EUnrealAIChatStreamStatus::Failed;

	/** Complete or partial response accumulated before the terminal state. */
	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	FUnrealAIChatResponse Response;

	UPROPERTY(BlueprintReadOnly, Category = "Stream")
	FUnrealAIError Error;
};

DECLARE_DELEGATE_TwoParams(FUnrealAIChatCompletionNativeDelegate, const FUnrealAIChatResponse & /*Response*/,
						   const FUnrealAIError & /*Error*/);
DECLARE_DELEGATE_OneParam(FUnrealAIRetryNativeDelegate, const FUnrealAIRetryEvent & /*Event*/);
DECLARE_DELEGATE_OneParam(FUnrealAIChatStreamEventNativeDelegate, const FUnrealAIChatStreamEvent & /*Event*/);
DECLARE_DELEGATE_OneParam(FUnrealAIChatStreamTerminalNativeDelegate, const FUnrealAIChatStreamResult & /*Result*/);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUnrealAIChatCompletionPin, const FUnrealAIChatResponse &, Response,
											 const FUnrealAIError &, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUnrealAIRetryPin, const FUnrealAIRetryEvent &, RetryEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUnrealAIChatStreamEventPin, const FUnrealAIChatStreamEvent &, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUnrealAIChatStreamCancelledPin, const FUnrealAIChatResponse &,
											PartialResponse);
