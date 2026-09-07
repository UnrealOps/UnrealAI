// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAIModelError.h"
#include "Auth/UnrealAIProviderAccess.h"

/** Plain input values. Game content classification and media egress authorization run before this boundary. */
enum class EUnrealAIModelRole : uint8
{
	System,
	Developer,
	User,
	Assistant,
	Tool
};
enum class EUnrealAIContentType : uint8
{
	Text,
	StructuredJson,
	Image,
	Audio,
	ToolCall,
	ToolResult,
	Citation,
	Refusal
};

struct UNREALAIACCESS_API FUnrealAIModelContentPart final
{
	static constexpr int32 MaxInlineBytes = 2 * 1024 * 1024;
	static constexpr int32 MaxTextUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxJsonUtf8Bytes = 64 * 1024;
	EUnrealAIContentType Type = EUnrealAIContentType::Text;
	FString Text;
	FString Json;
	FString MimeType;
	TArray<uint8> InlineBytes;
	bool Validate(FString &OutError) const;
};

struct UNREALAIACCESS_API FUnrealAIModelMessage final
{
	static constexpr int32 MaxContentParts = 64;
	FGuid MessageId;
	EUnrealAIModelRole Role = EUnrealAIModelRole::User;
	FName Participant;
	TArray<FUnrealAIModelContentPart> Content;
	bool Validate(FString &OutError) const;
};

enum class EUnrealAIModelCapability : uint32
{
	None = 0,
	Text = 1 << 0,
	StreamingText = 1 << 1,
	FunctionTools = 1 << 2,
	ParallelFunctionTools = 1 << 3,
	StructuredOutput = 1 << 4,
	ImageInput = 1 << 5,
	AudioInput = 1 << 6,
	Embeddings = 1 << 7,
	Reranking = 1 << 8,
	Transcription = 1 << 9,
	SpeechSynthesis = 1 << 10,
	RealtimeDuplexSpeech = 1 << 11,
	PromptCaching = 1 << 12,
	UsageReporting = 1 << 13,
	ProviderConversationState = 1 << 14,
	RequestCancellation = 1 << 15
};

ENUM_CLASS_FLAGS(EUnrealAIModelCapability);

enum class EUnrealAIModelEventKind : uint8
{
	Invalid,
	Started,
	TextDelta,
	Completed,
	Failed,
	Cancelled,
	TimedOut,
	TextCompleted,
	StructuredDelta,
	StructuredCompleted,
	ToolCallStarted,
	ToolCallArgumentsDelta,
	ToolCallCompleted,
	AudioDelta,
	AudioTranscriptDelta,
	UsageUpdated,
	ProviderMetadata
};

struct UNREALAIACCESS_API FUnrealAIModelProviderDescriptor final
{
	FName ProviderName;
	EUnrealAIModelCapability Capabilities = EUnrealAIModelCapability::None;
	int32 MaximumToolsPerRequest = 0;
	int32 MaximumInputMessages = 0;
	int32 MaximumOutputTokens = 0;

	bool ValidateShape(FString &OutError) const;
};

struct UNREALAIACCESS_API FUnrealAIModelProfileProjection final
{
	static constexpr int32 MaxTextOutputUtf8Bytes = 256 * 1024;
	static constexpr int32 MaxStructuredOutputUtf8Bytes = 256 * 1024;
	static constexpr int32 MaxToolArgumentUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxAggregateToolArgumentUtf8Bytes = 8 * 1024 * 1024;
	static constexpr int32 MaxAudioOutputBytes = 16 * 1024 * 1024;
	static constexpr int32 MaxTranscriptUtf8Bytes = 256 * 1024;
	static constexpr int32 MaxMetadataEntries = 64;

	FString ModelId;
	EUnrealAIModelCapability Capabilities = EUnrealAIModelCapability::None;
	int32 MaximumToolsPerRequest = 0;
	int32 MaximumToolOutputsPerRequest = 0;
	int32 MaximumInputMessages = 0;
	int32 MaximumOutputTokens = 0;
	int32 MaximumParallelToolCalls = 0;
	int32 MaximumTextOutputBytes = 0;
	int32 MaximumStructuredOutputBytes = 0;
	int32 MaximumToolArgumentBytes = 0;
	int32 MaximumAggregateToolArgumentBytes = 0;
	int32 MaximumAudioOutputBytes = 0;
	int32 MaximumTranscriptBytes = 0;
	int32 MaximumMetadataEntries = MaxMetadataEntries;

	bool ValidateAgainst(const FUnrealAIModelProviderDescriptor &Aggregate, FString &OutError) const;
	bool operator==(const FUnrealAIModelProfileProjection &Other) const;
	bool operator!=(const FUnrealAIModelProfileProjection &Other) const
	{
		return !(*this == Other);
	}
};

struct UNREALAIACCESS_API FUnrealAIModelToolDescriptor final
{
	static constexpr int32 MaxInvocationNameUtf8Bytes = 64;
	static constexpr int32 MaxDescriptionUtf8Bytes = 8 * 1024;
	static constexpr int32 MaxSchemaUtf8Bytes = 64 * 1024;

	FName StableName;
	int32 Version = 1;
	/** Bounded transport-safe name mapped back to StableName by the executor. */
	FString InvocationName;
	FString Description;
	FString InputJsonSchema;
	bool bStrict = true;

	bool ValidateShape(FString &OutError) const;
};

struct UNREALAIACCESS_API FUnrealAIModelOutputContract final
{
	static constexpr int32 MaxNameUtf8Bytes = 64;
	static constexpr int32 MaxSchemaUtf8Bytes = 64 * 1024;

	/** Bounded wire-safe schema name. It is configuration, never a provider identity or authorization input. */
	FString Name;
	/** One bounded JSON Schema object serialized as UTF-8 JSON. */
	FString JsonSchema;
	bool bStrict = true;

	bool ValidateShape(FString &OutError) const;
};

struct UNREALAIACCESS_API FUnrealAIModelToolCall final
{
	static constexpr int32 MaxProviderCallIdUtf8Bytes = 1024;
	static constexpr int32 MaxArgumentsJsonUtf8Bytes = 64 * 1024;

	FString ProviderCallId;
	FName StableName;
	int32 Version = 1;
	FString ArgumentsJson;

	bool ValidateShape(FString &OutError) const;
};

struct UNREALAIACCESS_API FUnrealAIModelToolOutput final
{
	static constexpr int32 MaxOutputJsonUtf8Bytes = 64 * 1024;

	FString ProviderCallId;
	FString OutputJson;
	bool ValidateShape(FString &OutError) const;
};

enum class EUnrealAIModelProviderRequestIdKind : uint8
{
	Invalid,
	TransportRequest,
	Response
};

struct UNREALAIACCESS_API FUnrealAIModelProviderRequestId final
{
	static constexpr int32 MaxValueUtf8Bytes = 1024;

	EUnrealAIModelProviderRequestIdKind Kind = EUnrealAIModelProviderRequestIdKind::Invalid;
	FString Value;

	bool IsSet() const;
	bool ValidateShape(FString &OutError) const;
	bool operator==(const FUnrealAIModelProviderRequestId &Other) const
	{
		return Kind == Other.Kind && Value == Other.Value;
	}
};

struct UNREALAIACCESS_API FUnrealAIModelUsageSnapshot final
{
	static constexpr int64 MaxTokenCount = 1000000000000LL;

	int64 InputTokens = 0;
	int64 CachedInputTokens = 0;
	int64 OutputTokens = 0;
	int64 ReasoningOutputTokens = 0;
	int64 AudioInputTokens = 0;
	int64 AudioOutputTokens = 0;
	int64 TotalTokens = 0;
	bool bFinal = false;

	bool IsZero() const;
	bool ValidateShape(FString &OutError) const;
	bool IsMonotonicFrom(const FUnrealAIModelUsageSnapshot &Previous) const;
	bool operator==(const FUnrealAIModelUsageSnapshot &Other) const;
};

enum class EUnrealAIModelMetadataKey : uint8
{
	Invalid,
	FinishReason,
	ResponseStatus,
	ServiceTier,
	ModelRevision,
	Region,
	CacheStatus,
	SafetyStatus
};

struct UNREALAIACCESS_API FUnrealAIModelMetadata final
{
	static constexpr int32 MaxValueUtf8Bytes = 256;

	EUnrealAIModelMetadataKey Key = EUnrealAIModelMetadataKey::Invalid;
	FString Value;

	bool IsSet() const;
	bool ValidateShape(FString &OutError) const;
	bool operator==(const FUnrealAIModelMetadata &Other) const
	{
		return Key == Other.Key && Value == Other.Value;
	}
};

enum class EUnrealAIModelAudioFormat : uint8
{
	Invalid,
	Pcm16,
	Float32,
	MuLaw,
	ALaw,
	Mp3,
	Opus,
	Aac,
	Flac,
	Wav
};

struct UNREALAIACCESS_API FUnrealAIModelAudioChunk final
{
	static constexpr int32 MaxBytes = 64 * 1024;
	static constexpr int32 MinimumSampleRateHz = 8000;
	static constexpr int32 MaximumSampleRateHz = 192000;
	static constexpr int32 MaximumChannels = 8;

	EUnrealAIModelAudioFormat Format = EUnrealAIModelAudioFormat::Invalid;
	int32 SampleRateHz = 0;
	int32 ChannelCount = 0;
	TArray<uint8> Bytes;

	bool IsSet() const;
	bool ValidateShape(FString &OutError) const;
	bool operator==(const FUnrealAIModelAudioChunk &Other) const;
};

struct UNREALAIACCESS_API FUnrealAIModelContinuationBinding final
{
	static constexpr int32 MaxModelIdUtf8Bytes = 256;

	FName ProviderName;
	FName ConnectionAlias;
	FString ModelId;
	/** Frozen connection identity supplied by the execution service, never a credential. */
	FString ConnectionBinding;

	bool ValidateShape(FString &OutError) const;
};

/** Opaque single-use history; provider, exact connection, model, and original request content remain bound. */
class UNREALAIACCESS_API IUnrealAIModelContinuation
{
  public:
	virtual ~IUnrealAIModelContinuation() = default;
	virtual FName GetProviderName() const = 0;
	virtual bool IsValid() const = 0;
	virtual bool TryConsume(const FUnrealAIModelContinuationBinding &ExpectedBinding, FString &OutError) const = 0;
	virtual const void *GetImplementationTypeToken() const = 0;
	virtual FString GetRedactedDisplay() const = 0;
};

class UNREALAIACCESS_API FUnrealAIModelContinuationAccess final
{
  public:
	static bool TryAcquire(TSharedRef<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation,
						   FName ExpectedProviderName, FUnrealAIModelContinuationAccess &OutAccess, FString &OutError);
	bool IsValid() const
	{
		return Implementation.IsValid();
	}
	const IUnrealAIModelContinuation *GetImplementation() const
	{
		return Implementation.Get();
	}

  private:
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Implementation;
};

struct UNREALAIACCESS_API FUnrealAIModelRequest final
{
	static constexpr uint64 MaxRetainedRequestBytes = 16 * 1024 * 1024;
	static constexpr int32 MaxModelIdUtf8Bytes = 256;
	static constexpr int32 MaxInstructionsUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxInputMessages = 128;
	static constexpr int32 MaxTools = 128;
	static constexpr int32 MaxToolOutputs = 128;
	static constexpr int32 MaxOutputTokensLimit = 128 * 1024;
	static constexpr float MaxTimeoutSeconds = 1800.0f;

	FUnrealAIRequestId RequestId;
	FName ConnectionAlias;
	FString ModelId;
	FString ConnectionBinding;
	FString Instructions;
	TArray<FUnrealAIModelMessage> InputMessages;
	TArray<FUnrealAIModelToolDescriptor> Tools;
	TArray<FUnrealAIModelToolOutput> ToolOutputs;
	/** Optional normalized final-output contract. Its presence requires StructuredOutput capability. */
	TOptional<FUnrealAIModelOutputContract> OutputContract;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation;
	int32 MaxOutputTokens = 1024;
	float TimeoutSeconds = 60.0f;
	/** Provider storage is off by default; adapters must opt in explicitly per request. */
	bool bAllowProviderStorage = false;

	/**
	 * Returns the explicit requirements plus every capability implied by the
	 * normalized request. Text generation is always required; tools, tool
	 * outputs, tool content, structured output, image content, and audio content
	 * add their matching capabilities.
	 */
	EUnrealAIModelCapability GetEffectiveRequiredCapabilities(
		EUnrealAIModelCapability ExplicitCapabilities = EUnrealAIModelCapability::None) const;
	bool ValidateShape(FString &OutError) const;
};

struct UNREALAIACCESS_API FUnrealAIModelEvent final
{
	static constexpr int32 MaxTextDeltaUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxFinalTextUtf8Bytes = 256 * 1024;
	static constexpr int32 MaxStructuredDeltaUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxStructuredJsonUtf8Bytes = 256 * 1024;
	static constexpr int32 MaxToolArgumentsDeltaUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxTranscriptDeltaUtf8Bytes = 64 * 1024;
	static constexpr int32 MaxProviderRequestIds = 2;

	FUnrealAIRequestId RequestId;
	EUnrealAIModelEventKind Kind = EUnrealAIModelEventKind::Invalid;
	int64 SequenceNumber = 0;
	TArray<FUnrealAIModelProviderRequestId> ProviderRequestIds;
	FString TextDelta;
	FString FinalText;
	FString StructuredDelta;
	FString StructuredJson;
	FString ToolCallId;
	int32 ToolOutputIndex = INDEX_NONE;
	FName ToolStableName;
	int32 ToolVersion = 0;
	FString ToolArgumentsDelta;
	FUnrealAIModelToolCall ToolCall;
	FUnrealAIModelAudioChunk Audio;
	FString AudioTranscriptDelta;
	FUnrealAIModelUsageSnapshot Usage;
	FUnrealAIModelMetadata Metadata;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation;
	FUnrealAIModelError Error;

	bool IsTerminal() const;
	bool ValidateShape(FString &OutError) const;
};
