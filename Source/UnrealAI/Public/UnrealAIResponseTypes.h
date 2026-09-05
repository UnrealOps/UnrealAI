#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.h"
#include "UnrealAIResponseTypes.generated.h"

UENUM(BlueprintType)
enum class EUnrealAIResponseItemType : uint8
{
	Message,
	ToolCall,
	ToolResult,
	ProviderData
};

UENUM(BlueprintType)
enum class EUnrealAIResponsePartType : uint8
{
	Text,
	Refusal,
	ProviderData
};

UENUM(BlueprintType)
enum class EUnrealAIResponseStatus : uint8
{
	Completed,
	Incomplete,
	Failed,
	Cancelled
};

UENUM(BlueprintType)
enum class EUnrealAIToolChoice : uint8
{
	Auto,
	None,
	Required,
	Named
};

UENUM(BlueprintType)
enum class EUnrealAIResponseFormat : uint8
{
	Text,
	JsonObject,
	JsonSchema
};

UENUM(BlueprintType)
enum class EUnrealAIResponseEventType : uint8
{
	ItemAdded,
	PartAdded,
	TextDelta,
	RefusalDelta,
	ToolArgumentsDelta,
	ItemCompleted,
	Usage,
	ProviderEvent
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponsePart
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	EUnrealAIResponsePartType Type = EUnrealAIResponsePartType::Text;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString Text;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString ProviderType;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString RawJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIToolDefinition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString ParametersJson = TEXT("{\"type\":\"object\",\"properties\":{}}");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bStrict = false;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponseItem
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	EUnrealAIResponseItemType Type = EUnrealAIResponseItemType::Message;

	/** Local item identity; distinct from the provider tool call ID. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	EUnrealAIMessageRole Role = EUnrealAIMessageRole::User;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	TArray<FUnrealAIResponsePart> Content;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString CallId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString ToolName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString ArgumentsJson;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString Output;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bToolError = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bComplete = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString ProviderType;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString RawJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponseHistory
{
	GENERATED_BODY()

	/** Opaque provider/protocol/model binding. Never log continuation data. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString Binding;

	/** Original protocol input items, including signed blocks. Use BuildContinuationRequest. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString ItemsJson;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponseRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString Model;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString Instructions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	TArray<FUnrealAIResponseItem> Input;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	TArray<FUnrealAIToolDefinition> Tools;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	EUnrealAIToolChoice ToolChoice = EUnrealAIToolChoice::Auto;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString NamedTool;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	EUnrealAIResponseFormat OutputFormat = EUnrealAIResponseFormat::Text;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString OutputSchemaJson;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString OutputSchemaName = TEXT("response");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bStrictOutput = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bUseTemperature = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	float Temperature = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bUseTopP = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	float TopP = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bUseMaxOutputTokens = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	int32 MaxOutputTokens = 512;

	/** Opt in to OpenAI Responses storage; local replay is the default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	bool bStore = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString PreviousResponseId;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIResponseHistory History;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FUnrealAIRequestRetryOptions RetryOptions;

	/** Protocol-native extensions. SDK-owned fields cannot be overridden. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Responses")
	FString AdditionalParametersJson;
};

/** Implemented protocol features, not a guarantee of a configured model's capabilities. */
USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponseCapabilities
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bText = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bStreaming = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bTools = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bRequiredToolChoice = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bNamedToolChoice = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bStrictTools = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bJsonObject = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bJsonSchema = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bStoredContinuation = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bUsesOpenAIResponses = false;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString Model;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	TArray<FUnrealAIResponseItem> Output;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIUsage Usage;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString FinishReason;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString IncompleteReason;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString RawJson;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIResponseHistory Continuation;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	bool bStored = false;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponseResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIRequestHandle RequestHandle;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	EUnrealAIResponseStatus Status = EUnrealAIResponseStatus::Failed;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIResponse Response;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIError Error;
};

USTRUCT(BlueprintType)
struct UNREALAI_API FUnrealAIResponseEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIRequestHandle RequestHandle;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	EUnrealAIResponseEventType Type = EUnrealAIResponseEventType::ProviderEvent;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	int32 ItemIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	int32 PartIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString ItemId;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString Delta;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIResponseItem Item;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FUnrealAIUsage Usage;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString ProviderType;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI|Responses")
	FString RawJson;
};

DECLARE_DELEGATE_OneParam(FUnrealAIResponseNativeDelegate, const FUnrealAIResponseResult&);
DECLARE_DELEGATE_OneParam(FUnrealAIResponseEventNativeDelegate, const FUnrealAIResponseEvent&);
// K2 async nodes derive data pins from the first delegate. All paths share a signature.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FUnrealAIResponseActionPin,
	const FUnrealAIResponseResult&, Result, const FUnrealAIResponseEvent&, ResponseEvent, const FUnrealAIRetryEvent&, RetryEvent);
