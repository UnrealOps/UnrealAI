// Copyright UnrealOps. All Rights Reserved.

#include "Models/UnrealAIModelTypes.h"

#include "Serialization/UnrealAIJsonValidation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Values/UnrealAIPhysicalAllocationValidationPrivate.h"
#include "Values/UnrealAITextValidationPrivate.h"

namespace
{
bool IsBoundedUtf8(const FString &Value, const int32 MaximumBytes, const bool bAllowEmpty)
{
	return UE::UnrealAI::PhysicalAllocation::Private::HasBoundedStringStorage(Value, MaximumBytes) &&
		   (bAllowEmpty || !Value.IsEmpty()) &&
		   UE::UnrealAI::TextValidation::Private::IsWellFormedSerializedString(Value) &&
		   UE::UnrealAI::TextValidation::Private::Utf8Length(Value) <= MaximumBytes;
}

bool IsStableName(const FName Value)
{
	if (Value.IsNone())
	{
		return false;
	}
	const FString Text = Value.ToString();
	if (!IsBoundedUtf8(Text, 128, false) || Text.TrimStartAndEnd() != Text)
	{
		return false;
	}
	for (const TCHAR Character : Text)
	{
		if (Character < 0x21 || Character > 0x7e)
		{
			return false;
		}
	}
	return true;
}

constexpr uint32 KnownModelCapabilityBits()
{
	return static_cast<uint32>(EUnrealAIModelCapability::Text) |
		   static_cast<uint32>(EUnrealAIModelCapability::StreamingText) |
		   static_cast<uint32>(EUnrealAIModelCapability::FunctionTools) |
		   static_cast<uint32>(EUnrealAIModelCapability::ParallelFunctionTools) |
		   static_cast<uint32>(EUnrealAIModelCapability::StructuredOutput) |
		   static_cast<uint32>(EUnrealAIModelCapability::ImageInput) |
		   static_cast<uint32>(EUnrealAIModelCapability::AudioInput) |
		   static_cast<uint32>(EUnrealAIModelCapability::Embeddings) |
		   static_cast<uint32>(EUnrealAIModelCapability::Reranking) |
		   static_cast<uint32>(EUnrealAIModelCapability::Transcription) |
		   static_cast<uint32>(EUnrealAIModelCapability::SpeechSynthesis) |
		   static_cast<uint32>(EUnrealAIModelCapability::RealtimeDuplexSpeech) |
		   static_cast<uint32>(EUnrealAIModelCapability::PromptCaching) |
		   static_cast<uint32>(EUnrealAIModelCapability::UsageReporting) |
		   static_cast<uint32>(EUnrealAIModelCapability::ProviderConversationState) |
		   static_cast<uint32>(EUnrealAIModelCapability::RequestCancellation);
}

bool IsInvocationName(const FString &Value)
{
	if (!IsBoundedUtf8(Value, FUnrealAIModelToolDescriptor::MaxInvocationNameUtf8Bytes, false))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('A') && Character <= TEXT('Z')) ||
																   (Character >= TEXT('0') && Character <= TEXT('9'));
		if (!bAlphaNumeric && Character != TEXT('_') && Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

bool IsJsonObject(const FString &Json, const int32 MaximumUtf8Bytes)
{
	FUnrealAIJsonPreflightLimits Limits;
	Limits.MaxUtf8Bytes = MaximumUtf8Bytes;
	Limits.MaxStringTokenCodeUnits = MaximumUtf8Bytes;
	FString PreflightError;
	if (!PreflightUnrealAIJson(Json, Limits, PreflightError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid();
}

bool IsProviderRequestIdToken(const FString &Value)
{
	if (!IsBoundedUtf8(Value, FUnrealAIModelProviderRequestId::MaxValueUtf8Bytes, false) ||
		Value.TrimStartAndEnd() != Value)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('A') && Character <= TEXT('Z')) ||
																   (Character >= TEXT('0') && Character <= TEXT('9'));
		if (!bAlphaNumeric &&
			Character != TEXT('.') && Character != TEXT('_') && Character != TEXT(':') && Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

bool IsEmptyError(const FUnrealAIModelError &Error)
{
	return Error.Category == EUnrealAIErrorCategory::None && Error.Code.IsNone() && Error.UserMessage.IsEmpty() &&
		   Error.DiagnosticMessage.IsEmpty() && !Error.bRetryable && Error.RetryAfterSeconds == 0.0f &&
		   Error.ProviderRequestId.IsEmpty() && Error.Metadata.IsEmpty();
}
} // namespace

bool FUnrealAIModelProviderDescriptor::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const uint32 RawCapabilities = static_cast<uint32>(Capabilities);
	if (!IsStableName(ProviderName) || RawCapabilities == 0 || (RawCapabilities & ~KnownModelCapabilityBits()) != 0 ||
		MaximumToolsPerRequest < 0 || MaximumToolsPerRequest > FUnrealAIModelRequest::MaxTools ||
		MaximumInputMessages < 1 || MaximumInputMessages > FUnrealAIModelRequest::MaxInputMessages ||
		MaximumOutputTokens < 1 || MaximumOutputTokens > FUnrealAIModelRequest::MaxOutputTokensLimit)
	{
		OutError = TEXT("Model provider descriptor is invalid or exceeds framework bounds.");
		return false;
	}
	if (EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::ParallelFunctionTools) &&
		!EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::FunctionTools))
	{
		OutError = TEXT("Parallel function tools require the function-tools capability.");
		return false;
	}
	if (EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::StreamingText) &&
		!EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::Text))
	{
		OutError = TEXT("Streaming text requires the text-generation capability.");
		return false;
	}
	if (EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::StructuredOutput) &&
		!EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::Text))
	{
		OutError = TEXT("Structured output requires the text-generation capability.");
		return false;
	}
	return true;
}

bool FUnrealAIModelProfileProjection::ValidateAgainst(const FUnrealAIModelProviderDescriptor &Aggregate,
													  FString &OutError) const
{
	OutError.Reset();
	FString AggregateError;
	const uint32 CapabilityBits = static_cast<uint32>(Capabilities);
	const uint32 AggregateBits = static_cast<uint32>(Aggregate.Capabilities);
	if (!Aggregate.ValidateShape(AggregateError) ||
		!IsBoundedUtf8(ModelId, FUnrealAIModelRequest::MaxModelIdUtf8Bytes, false) ||
		ModelId.TrimStartAndEnd() != ModelId || CapabilityBits == 0 ||
		(CapabilityBits & ~KnownModelCapabilityBits()) != 0 || (CapabilityBits & ~AggregateBits) != 0 ||
		MaximumToolsPerRequest < 0 || MaximumToolsPerRequest > Aggregate.MaximumToolsPerRequest ||
		MaximumToolOutputsPerRequest < 0 || MaximumToolOutputsPerRequest > FUnrealAIModelRequest::MaxToolOutputs ||
		MaximumToolOutputsPerRequest > Aggregate.MaximumToolsPerRequest || MaximumInputMessages < 1 ||
		MaximumInputMessages > Aggregate.MaximumInputMessages || MaximumOutputTokens < 1 ||
		MaximumOutputTokens > Aggregate.MaximumOutputTokens || MaximumParallelToolCalls < 0 ||
		MaximumParallelToolCalls > MaximumToolsPerRequest || MaximumParallelToolCalls > MaximumToolOutputsPerRequest ||
		MaximumTextOutputBytes < 0 || MaximumTextOutputBytes > MaxTextOutputUtf8Bytes ||
		MaximumStructuredOutputBytes < 0 || MaximumStructuredOutputBytes > MaxStructuredOutputUtf8Bytes ||
		MaximumToolArgumentBytes < 0 || MaximumToolArgumentBytes > MaxToolArgumentUtf8Bytes ||
		MaximumAggregateToolArgumentBytes < 0 ||
		MaximumAggregateToolArgumentBytes > MaxAggregateToolArgumentUtf8Bytes || MaximumAudioOutputBytes < 0 ||
		MaximumAudioOutputBytes > MaxAudioOutputBytes || MaximumTranscriptBytes < 0 ||
		MaximumTranscriptBytes > MaxTranscriptUtf8Bytes || MaximumMetadataEntries < 0 ||
		MaximumMetadataEntries > MaxMetadataEntries)
	{
		OutError = TEXT("Model profile projection is invalid or exceeds aggregate/framework bounds.");
		return false;
	}

	FUnrealAIModelProviderDescriptor ModelDescriptor = Aggregate;
	ModelDescriptor.Capabilities = Capabilities;
	ModelDescriptor.MaximumToolsPerRequest = MaximumToolsPerRequest;
	ModelDescriptor.MaximumInputMessages = MaximumInputMessages;
	ModelDescriptor.MaximumOutputTokens = MaximumOutputTokens;
	FString ModelDescriptorError;
	if (!ModelDescriptor.ValidateShape(ModelDescriptorError))
	{
		OutError = TEXT("Model profile projection violates a capability dependency.");
		return false;
	}

	const bool bTools = EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::FunctionTools);
	const bool bParallel = EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::ParallelFunctionTools);
	const bool bText = EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::Text);
	const bool bStructured = EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::StructuredOutput);
	const bool bAudioOutput = EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::SpeechSynthesis |
																EUnrealAIModelCapability::RealtimeDuplexSpeech);
	const bool bTranscript = EnumHasAnyFlags(Capabilities, EUnrealAIModelCapability::Transcription |
															   EUnrealAIModelCapability::RealtimeDuplexSpeech);
	if ((bTools && (MaximumToolsPerRequest < 1 || MaximumToolOutputsPerRequest < 1 || MaximumParallelToolCalls < 1 ||
					MaximumToolArgumentBytes < 1 || MaximumAggregateToolArgumentBytes < MaximumToolArgumentBytes)) ||
		(!bTools &&
		 (MaximumToolsPerRequest != 0 || MaximumToolOutputsPerRequest != 0 || MaximumParallelToolCalls != 0 ||
		  MaximumToolArgumentBytes != 0 || MaximumAggregateToolArgumentBytes != 0)) ||
		(bParallel && MaximumParallelToolCalls < 2) || (!bParallel && MaximumParallelToolCalls > 1) ||
		(bText != (MaximumTextOutputBytes > 0)) || (bStructured != (MaximumStructuredOutputBytes > 0)) ||
		(bAudioOutput != (MaximumAudioOutputBytes > 0)) || (bTranscript != (MaximumTranscriptBytes > 0)))
	{
		OutError = TEXT("Model profile projection capability and output limits are inconsistent.");
		return false;
	}
	return true;
}

bool FUnrealAIModelProfileProjection::operator==(const FUnrealAIModelProfileProjection &Other) const
{
	return ModelId == Other.ModelId && Capabilities == Other.Capabilities &&
		   MaximumToolsPerRequest == Other.MaximumToolsPerRequest &&
		   MaximumToolOutputsPerRequest == Other.MaximumToolOutputsPerRequest &&
		   MaximumInputMessages == Other.MaximumInputMessages && MaximumOutputTokens == Other.MaximumOutputTokens &&
		   MaximumParallelToolCalls == Other.MaximumParallelToolCalls &&
		   MaximumTextOutputBytes == Other.MaximumTextOutputBytes &&
		   MaximumStructuredOutputBytes == Other.MaximumStructuredOutputBytes &&
		   MaximumToolArgumentBytes == Other.MaximumToolArgumentBytes &&
		   MaximumAggregateToolArgumentBytes == Other.MaximumAggregateToolArgumentBytes &&
		   MaximumAudioOutputBytes == Other.MaximumAudioOutputBytes &&
		   MaximumTranscriptBytes == Other.MaximumTranscriptBytes &&
		   MaximumMetadataEntries == Other.MaximumMetadataEntries;
}

bool FUnrealAIModelToolDescriptor::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableName(StableName) || Version < 1 || !IsInvocationName(InvocationName) ||
		!IsBoundedUtf8(Description, MaxDescriptionUtf8Bytes, false) ||
		!IsBoundedUtf8(InputJsonSchema, MaxSchemaUtf8Bytes, false) ||
		!IsJsonObject(InputJsonSchema, MaxSchemaUtf8Bytes))
	{
		OutError = TEXT("Model tool descriptor requires a stable identity, bounded invocation name, and JSON schema.");
		return false;
	}
	return true;
}

bool FUnrealAIModelOutputContract::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsInvocationName(Name) || !IsBoundedUtf8(JsonSchema, MaxSchemaUtf8Bytes, false) ||
		!IsJsonObject(JsonSchema, MaxSchemaUtf8Bytes))
	{
		OutError = TEXT("Model output contract requires a bounded wire-safe name and JSON Schema object.");
		return false;
	}
	return true;
}

bool FUnrealAIModelToolCall::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsBoundedUtf8(ProviderCallId, MaxProviderCallIdUtf8Bytes, false) || !IsStableName(StableName) || Version < 1 ||
		!IsBoundedUtf8(ArgumentsJson, MaxArgumentsJsonUtf8Bytes, false) ||
		!IsJsonObject(ArgumentsJson, MaxArgumentsJsonUtf8Bytes))
	{
		OutError = TEXT("Model tool call is incomplete, oversized, or contains invalid JSON arguments.");
		return false;
	}
	return true;
}

bool FUnrealAIModelToolOutput::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsBoundedUtf8(ProviderCallId, FUnrealAIModelToolCall::MaxProviderCallIdUtf8Bytes, false) ||
		!IsBoundedUtf8(OutputJson, MaxOutputJsonUtf8Bytes, false) || !IsJsonObject(OutputJson, MaxOutputJsonUtf8Bytes))
	{
		OutError =
			TEXT("Model tool output requires a bounded call ID, JSON object output, and non-upgraded classification.");
		return false;
	}
	return true;
}

bool FUnrealAIModelProviderRequestId::IsSet() const
{
	return Kind != EUnrealAIModelProviderRequestIdKind::Invalid || !Value.IsEmpty();
}

bool FUnrealAIModelProviderRequestId::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (Kind != EUnrealAIModelProviderRequestIdKind::TransportRequest &&
		Kind != EUnrealAIModelProviderRequestIdKind::Response)
	{
		OutError = TEXT("Provider request ID kind is invalid.");
		return false;
	}
	if (!IsProviderRequestIdToken(Value))
	{
		OutError = TEXT("Provider request ID is empty, malformed, or exceeds its bound.");
		return false;
	}
	return true;
}

bool FUnrealAIModelUsageSnapshot::IsZero() const
{
	return InputTokens == 0 && CachedInputTokens == 0 && OutputTokens == 0 && ReasoningOutputTokens == 0 &&
		   AudioInputTokens == 0 && AudioOutputTokens == 0 && TotalTokens == 0 && !bFinal;
}

bool FUnrealAIModelUsageSnapshot::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const auto IsBoundedTokenCount = [](const int64 Value) { return Value >= 0 && Value <= MaxTokenCount; };
	const bool bInputTokensBounded = IsBoundedTokenCount(InputTokens);
	const bool bOutputTokensBounded = IsBoundedTokenCount(OutputTokens);
	const bool bTotalCanBeComputed =
		bInputTokensBounded && bOutputTokensBounded && OutputTokens <= MaxTokenCount - InputTokens;
	const bool bAggregateTotalInvalid =
		TotalTokens != 0 && (!bTotalCanBeComputed || TotalTokens != InputTokens + OutputTokens);
	const bool bFinalTotalInvalid =
		bFinal && (!bTotalCanBeComputed || (InputTokens + OutputTokens != 0 && TotalTokens == 0));
	if (!bInputTokensBounded || !IsBoundedTokenCount(CachedInputTokens) || !bOutputTokensBounded ||
		!IsBoundedTokenCount(ReasoningOutputTokens) || !IsBoundedTokenCount(AudioInputTokens) ||
		!IsBoundedTokenCount(AudioOutputTokens) || !IsBoundedTokenCount(TotalTokens) ||
		CachedInputTokens > InputTokens || AudioInputTokens > InputTokens || ReasoningOutputTokens > OutputTokens ||
		AudioOutputTokens > OutputTokens || bAggregateTotalInvalid || bFinalTotalInvalid)
	{
		OutError = TEXT("Provider model usage snapshot is negative, inconsistent, or exceeds its bound.");
		return false;
	}
	return true;
}

bool FUnrealAIModelUsageSnapshot::IsMonotonicFrom(const FUnrealAIModelUsageSnapshot &Previous) const
{
	return !Previous.bFinal && InputTokens >= Previous.InputTokens && CachedInputTokens >= Previous.CachedInputTokens &&
		   OutputTokens >= Previous.OutputTokens && ReasoningOutputTokens >= Previous.ReasoningOutputTokens &&
		   AudioInputTokens >= Previous.AudioInputTokens && AudioOutputTokens >= Previous.AudioOutputTokens &&
		   TotalTokens >= Previous.TotalTokens;
}

bool FUnrealAIModelUsageSnapshot::operator==(const FUnrealAIModelUsageSnapshot &Other) const
{
	return InputTokens == Other.InputTokens && CachedInputTokens == Other.CachedInputTokens &&
		   OutputTokens == Other.OutputTokens && ReasoningOutputTokens == Other.ReasoningOutputTokens &&
		   AudioInputTokens == Other.AudioInputTokens && AudioOutputTokens == Other.AudioOutputTokens &&
		   TotalTokens == Other.TotalTokens && bFinal == Other.bFinal;
}

bool FUnrealAIModelMetadata::IsSet() const
{
	return Key != EUnrealAIModelMetadataKey::Invalid || !Value.IsEmpty();
}

bool FUnrealAIModelMetadata::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (Key <= EUnrealAIModelMetadataKey::Invalid || Key > EUnrealAIModelMetadataKey::SafetyStatus ||
		!IsBoundedUtf8(Value, MaxValueUtf8Bytes, false) || Value.TrimStartAndEnd() != Value ||
		Value.Contains(TEXT("\r")) || Value.Contains(TEXT("\n")))
	{
		OutError = TEXT("Provider metadata key or value is invalid or exceeds its bound.");
		return false;
	}
	return true;
}

bool FUnrealAIModelAudioChunk::IsSet() const
{
	return Format != EUnrealAIModelAudioFormat::Invalid || SampleRateHz != 0 || ChannelCount != 0 || !Bytes.IsEmpty();
}

bool FUnrealAIModelAudioChunk::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	using UE::UnrealAI::PhysicalAllocation::Private::HasBoundedArrayStorage;
	if (Format <= EUnrealAIModelAudioFormat::Invalid || Format > EUnrealAIModelAudioFormat::Wav ||
		SampleRateHz < MinimumSampleRateHz || SampleRateHz > MaximumSampleRateHz || ChannelCount < 1 ||
		ChannelCount > MaximumChannels || Bytes.IsEmpty() ||
		!HasBoundedArrayStorage(Bytes.GetAllocatedSize(), Bytes.Num(), MaxBytes, sizeof(uint8)))
	{
		OutError = TEXT("Provider audio chunk format, layout, or byte payload is invalid or exceeds its bound.");
		return false;
	}
	return true;
}

bool FUnrealAIModelAudioChunk::operator==(const FUnrealAIModelAudioChunk &Other) const
{
	return Format == Other.Format && SampleRateHz == Other.SampleRateHz && ChannelCount == Other.ChannelCount &&
		   Bytes == Other.Bytes;
}

bool FUnrealAIModelContinuationBinding::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableName(ProviderName) || !IsStableName(ConnectionAlias) ||
		!IsBoundedUtf8(ModelId, MaxModelIdUtf8Bytes, false) || ModelId.TrimStartAndEnd() != ModelId ||
		!IsBoundedUtf8(ConnectionBinding, 16 * 1024, true))
	{
		OutError = TEXT("Model continuation binding is invalid or exceeds framework bounds.");
		return false;
	}
	return true;
}

EUnrealAIModelCapability
FUnrealAIModelRequest::GetEffectiveRequiredCapabilities(const EUnrealAIModelCapability ExplicitCapabilities) const
{
	EUnrealAIModelCapability Required = ExplicitCapabilities | EUnrealAIModelCapability::Text;
	if (OutputContract.IsSet())
	{
		Required |= EUnrealAIModelCapability::StructuredOutput;
	}
	if (!Tools.IsEmpty() || !ToolOutputs.IsEmpty())
	{
		Required |= EUnrealAIModelCapability::FunctionTools;
	}
	for (const FUnrealAIModelMessage &Message : InputMessages)
	{
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			if (Part.Type == EUnrealAIContentType::Image)
			{
				Required |= EUnrealAIModelCapability::ImageInput;
			}
			else if (Part.Type == EUnrealAIContentType::Audio)
			{
				Required |= EUnrealAIModelCapability::AudioInput;
			}
			else if (Part.Type == EUnrealAIContentType::ToolCall || Part.Type == EUnrealAIContentType::ToolResult)
			{
				Required |= EUnrealAIModelCapability::FunctionTools;
			}
		}
	}
	return Required;
}

bool FUnrealAIModelRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	using UE::UnrealAI::PhysicalAllocation::Private::HasBoundedArrayStorage;
	if (!RequestId.IsValid() || !IsStableName(ConnectionAlias) || !IsBoundedUtf8(ModelId, MaxModelIdUtf8Bytes, false) ||
		ModelId.TrimStartAndEnd() != ModelId || !IsBoundedUtf8(ConnectionBinding, 16 * 1024, true) ||
		!IsBoundedUtf8(Instructions, MaxInstructionsUtf8Bytes, true) ||
		!HasBoundedArrayStorage(InputMessages.GetAllocatedSize(), InputMessages.Num(), MaxInputMessages,
								sizeof(FUnrealAIModelMessage)) ||
		!HasBoundedArrayStorage(Tools.GetAllocatedSize(), Tools.Num(), MaxTools,
								sizeof(FUnrealAIModelToolDescriptor)) ||
		!HasBoundedArrayStorage(ToolOutputs.GetAllocatedSize(), ToolOutputs.Num(), MaxToolOutputs,
								sizeof(FUnrealAIModelToolOutput)) ||
		MaxOutputTokens < 1 || MaxOutputTokens > MaxOutputTokensLimit || !FMath::IsFinite(TimeoutSeconds) ||
		TimeoutSeconds <= 0.0f || TimeoutSeconds > MaxTimeoutSeconds)
	{
		OutError = TEXT("Model request identity, configuration, payload count, output limit, or timeout is invalid.");
		return false;
	}
	if (InputMessages.IsEmpty() && ToolOutputs.IsEmpty())
	{
		OutError = TEXT("Model request requires input messages or tool outputs.");
		return false;
	}
	if (!ToolOutputs.IsEmpty() && (!Continuation.IsValid() || !Continuation->IsValid()))
	{
		OutError = TEXT("Model tool outputs require a valid opaque provider continuation.");
		return false;
	}
	if (Continuation.IsValid() && !Continuation->IsValid())
	{
		OutError = TEXT("Model request contains an invalid provider continuation.");
		return false;
	}
	if (OutputContract.IsSet() && !OutputContract->ValidateShape(OutError))
	{
		return false;
	}
	uint64 RetainedBytes = Instructions.GetAllocatedSize() + ConnectionBinding.GetAllocatedSize() +
						   ModelId.GetAllocatedSize() + InputMessages.GetAllocatedSize() + Tools.GetAllocatedSize() +
						   ToolOutputs.GetAllocatedSize();
	if (OutputContract.IsSet())
	{
		RetainedBytes += OutputContract->Name.GetAllocatedSize() + OutputContract->JsonSchema.GetAllocatedSize();
	}
	TSet<FString> InvocationNames;
	TMap<FName, TSet<int32>> VersionsByStableName;
	for (const FUnrealAIModelMessage &Message : InputMessages)
	{
		if (!Message.Validate(OutError))
		{
			return false;
		}
		RetainedBytes += Message.Content.GetAllocatedSize();
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			RetainedBytes += Part.Text.GetAllocatedSize() + Part.Json.GetAllocatedSize() +
							 Part.MimeType.GetAllocatedSize() + Part.InlineBytes.GetAllocatedSize();
			if (RetainedBytes > MaxRetainedRequestBytes)
			{
				OutError = TEXT("Model request exceeds its total retained storage bound.");
				return false;
			}
		}
	}
	for (const FUnrealAIModelToolDescriptor &Tool : Tools)
	{
		if (!Tool.ValidateShape(OutError))
		{
			return false;
		}
		if (InvocationNames.Contains(Tool.InvocationName))
		{
			OutError = TEXT("Model request contains duplicate tool invocation names.");
			return false;
		}
		TSet<int32> &Versions = VersionsByStableName.FindOrAdd(Tool.StableName);
		if (Versions.Contains(Tool.Version))
		{
			OutError = TEXT("Model request contains duplicate tool stable-name/version identities.");
			return false;
		}
		RetainedBytes += Tool.InvocationName.GetAllocatedSize() + Tool.Description.GetAllocatedSize() +
						 Tool.InputJsonSchema.GetAllocatedSize();
		if (RetainedBytes > MaxRetainedRequestBytes)
		{
			OutError = TEXT("Model request exceeds its total retained storage bound.");
			return false;
		}
		InvocationNames.Add(Tool.InvocationName);
		Versions.Add(Tool.Version);
	}
	TSet<FString> OutputCallIds;
	for (const FUnrealAIModelToolOutput &Output : ToolOutputs)
	{
		if (!Output.ValidateShape(OutError) || OutputCallIds.Contains(Output.ProviderCallId))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Model request contains duplicate tool-output call IDs.");
			}
			return false;
		}
		RetainedBytes += Output.ProviderCallId.GetAllocatedSize() + Output.OutputJson.GetAllocatedSize();
		if (RetainedBytes > MaxRetainedRequestBytes)
		{
			OutError = TEXT("Model request exceeds its total retained storage bound.");
			return false;
		}
		OutputCallIds.Add(Output.ProviderCallId);
	}
	return true;
}

bool FUnrealAIModelEvent::IsTerminal() const
{
	return Kind == EUnrealAIModelEventKind::Completed || Kind == EUnrealAIModelEventKind::Failed ||
		   Kind == EUnrealAIModelEventKind::Cancelled || Kind == EUnrealAIModelEventKind::TimedOut;
}

bool FUnrealAIModelEvent::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	using UE::UnrealAI::PhysicalAllocation::Private::HasBoundedArrayStorage;
	if (!RequestId.IsValid() || SequenceNumber < 1 || SequenceNumber >= MAX_int64 ||
		!HasBoundedArrayStorage(ProviderRequestIds.GetAllocatedSize(), ProviderRequestIds.Num(), MaxProviderRequestIds,
								sizeof(FUnrealAIModelProviderRequestId)) ||
		!IsBoundedUtf8(TextDelta, MaxTextDeltaUtf8Bytes, true) ||
		!IsBoundedUtf8(FinalText, MaxFinalTextUtf8Bytes, true) ||
		!IsBoundedUtf8(StructuredDelta, MaxStructuredDeltaUtf8Bytes, true) ||
		!IsBoundedUtf8(StructuredJson, MaxStructuredJsonUtf8Bytes, true) ||
		!IsBoundedUtf8(ToolCallId, FUnrealAIModelToolCall::MaxProviderCallIdUtf8Bytes, true) ||
		!IsBoundedUtf8(ToolArgumentsDelta, MaxToolArgumentsDeltaUtf8Bytes, true) ||
		!IsBoundedUtf8(AudioTranscriptDelta, MaxTranscriptDeltaUtf8Bytes, true))
	{
		OutError = TEXT("Model event identity, sequence, or bounded payload storage is invalid.");
		return false;
	}

	TSet<EUnrealAIModelProviderRequestIdKind> ProviderRequestIdKinds;
	EUnrealAIModelProviderRequestIdKind PreviousProviderRequestIdKind = EUnrealAIModelProviderRequestIdKind::Invalid;
	for (const FUnrealAIModelProviderRequestId &ProviderRequestId : ProviderRequestIds)
	{
		if (!ProviderRequestId.ValidateShape(OutError) || ProviderRequestIdKinds.Contains(ProviderRequestId.Kind) ||
			ProviderRequestId.Kind <= PreviousProviderRequestIdKind)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Model event provider request ID namespaces must be unique and canonically ordered.");
			}
			return false;
		}
		ProviderRequestIdKinds.Add(ProviderRequestId.Kind);
		PreviousProviderRequestIdKind = ProviderRequestId.Kind;
	}
	if (!Error.ValidateShape(OutError))
	{
		return false;
	}
	if (!Error.ProviderRequestId.IsEmpty())
	{
		if (!IsProviderRequestIdToken(Error.ProviderRequestId))
		{
			OutError = TEXT("Model event error contains an invalid provider request ID.");
			return false;
		}
		bool bMatchedEventId = false;
		for (const FUnrealAIModelProviderRequestId &ProviderRequestId : ProviderRequestIds)
		{
			bMatchedEventId |= ProviderRequestId.Value == Error.ProviderRequestId;
		}
		if (!bMatchedEventId)
		{
			OutError = TEXT("Model event and error provider request IDs disagree.");
			return false;
		}
	}

	const bool bToolCallEmpty = ToolCall.ProviderCallId.IsEmpty() && ToolCall.StableName.IsNone() &&
								ToolCall.Version == 1 && ToolCall.ArgumentsJson.IsEmpty();
	const bool bUsageEmpty = Usage.IsZero();
	const bool bMetadataEmpty = !Metadata.IsSet();
	const bool bAudioEmpty = !Audio.IsSet();
	const bool bErrorEmpty = IsEmptyError(Error);
	const auto HasOnlyCommonEnvelope = [&]()
	{
		return TextDelta.IsEmpty() && FinalText.IsEmpty() && StructuredDelta.IsEmpty() && StructuredJson.IsEmpty() &&
			   ToolCallId.IsEmpty() && ToolOutputIndex == INDEX_NONE && ToolStableName.IsNone() && ToolVersion == 0 &&
			   ToolArgumentsDelta.IsEmpty() && bToolCallEmpty && bAudioEmpty && AudioTranscriptDelta.IsEmpty() &&
			   bUsageEmpty && bMetadataEmpty;
	};

	switch (Kind)
	{
	case EUnrealAIModelEventKind::Started:
		if (!HasOnlyCommonEnvelope() || !bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Started model event cannot contain output, an error, or a continuation.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::TextDelta:
		if (TextDelta.IsEmpty() || !FinalText.IsEmpty() || !StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() ||
			!ToolCallId.IsEmpty() || ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() ||
			!bUsageEmpty || !bMetadataEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Text-delta model event must contain only a non-empty delta.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::TextCompleted:
		if (FinalText.IsEmpty() || !TextDelta.IsEmpty() || !StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() ||
			!ToolCallId.IsEmpty() || ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() ||
			!bUsageEmpty || !bMetadataEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Text-completed model event must contain only bounded final text.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::StructuredDelta:
		if (StructuredDelta.IsEmpty() || !TextDelta.IsEmpty() || !FinalText.IsEmpty() || !StructuredJson.IsEmpty() ||
			!ToolCallId.IsEmpty() || ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() ||
			!bUsageEmpty || !bMetadataEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Structured-delta model event must contain only a non-empty JSON fragment.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::StructuredCompleted:
		if (StructuredJson.IsEmpty() || !IsJsonObject(StructuredJson, MaxStructuredJsonUtf8Bytes) ||
			!TextDelta.IsEmpty() || !FinalText.IsEmpty() || !StructuredDelta.IsEmpty() || !ToolCallId.IsEmpty() ||
			ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() ||
			!bUsageEmpty || !bMetadataEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Structured-completed model event requires only a bounded JSON object.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::ToolCallStarted:
		if (!IsProviderRequestIdToken(ToolCallId) || ToolOutputIndex < 0 || ToolOutputIndex > 4096 ||
			!IsStableName(ToolStableName) || ToolVersion < 1 || !TextDelta.IsEmpty() || !FinalText.IsEmpty() ||
			!StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() || !ToolArgumentsDelta.IsEmpty() ||
			!bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() || !bUsageEmpty || !bMetadataEmpty ||
			!bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Tool-call-started model event has an invalid identity or inactive payload.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::ToolCallArgumentsDelta:
		if (!IsProviderRequestIdToken(ToolCallId) || ToolOutputIndex < 0 || ToolOutputIndex > 4096 ||
			ToolArgumentsDelta.IsEmpty() || !ToolStableName.IsNone() || ToolVersion != 0 || !TextDelta.IsEmpty() ||
			!FinalText.IsEmpty() || !StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() || !bToolCallEmpty ||
			!bAudioEmpty || !AudioTranscriptDelta.IsEmpty() || !bUsageEmpty || !bMetadataEmpty || !bErrorEmpty ||
			Continuation.IsValid())
		{
			OutError = TEXT("Tool-arguments-delta model event has an invalid identity, delta, or inactive payload.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::ToolCallCompleted:
		if (ToolOutputIndex < 0 || ToolOutputIndex > 4096 || !ToolCall.ValidateShape(OutError) ||
			!ToolCallId.IsEmpty() || !ToolStableName.IsNone() || ToolVersion != 0 || !ToolArgumentsDelta.IsEmpty() ||
			!TextDelta.IsEmpty() || !FinalText.IsEmpty() || !StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() ||
			!bAudioEmpty || !AudioTranscriptDelta.IsEmpty() || !bUsageEmpty || !bMetadataEmpty || !bErrorEmpty ||
			Continuation.IsValid())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Tool-call-completed model event has an invalid call or inactive payload.");
			}
			return false;
		}
		break;
	case EUnrealAIModelEventKind::AudioDelta:
		if (!Audio.ValidateShape(OutError) || !TextDelta.IsEmpty() || !FinalText.IsEmpty() ||
			!StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() || !ToolCallId.IsEmpty() ||
			ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !AudioTranscriptDelta.IsEmpty() || !bUsageEmpty ||
			!bMetadataEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Audio-delta model event has an invalid audio chunk or inactive payload.");
			}
			return false;
		}
		break;
	case EUnrealAIModelEventKind::AudioTranscriptDelta:
		if (AudioTranscriptDelta.IsEmpty() || !TextDelta.IsEmpty() || !FinalText.IsEmpty() ||
			!StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() || !ToolCallId.IsEmpty() ||
			ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !bUsageEmpty || !bMetadataEmpty ||
			!bErrorEmpty || Continuation.IsValid())
		{
			OutError = TEXT("Audio-transcript-delta model event must contain only a non-empty transcript delta.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::UsageUpdated:
		if (!Usage.ValidateShape(OutError) || !TextDelta.IsEmpty() || !FinalText.IsEmpty() ||
			!StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() || !ToolCallId.IsEmpty() ||
			ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() ||
			!bMetadataEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Usage-updated model event has invalid usage or inactive payload.");
			}
			return false;
		}
		break;
	case EUnrealAIModelEventKind::ProviderMetadata:
		if (!Metadata.ValidateShape(OutError) || !TextDelta.IsEmpty() || !FinalText.IsEmpty() ||
			!StructuredDelta.IsEmpty() || !StructuredJson.IsEmpty() || !ToolCallId.IsEmpty() ||
			ToolOutputIndex != INDEX_NONE || !ToolStableName.IsNone() || ToolVersion != 0 ||
			!ToolArgumentsDelta.IsEmpty() || !bToolCallEmpty || !bAudioEmpty || !AudioTranscriptDelta.IsEmpty() ||
			!bUsageEmpty || !bErrorEmpty || Continuation.IsValid())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Provider-metadata model event has invalid metadata or inactive payload.");
			}
			return false;
		}
		break;
	case EUnrealAIModelEventKind::Completed:
		if (!HasOnlyCommonEnvelope() || !bErrorEmpty || (Continuation.IsValid() && !Continuation->IsValid()))
		{
			OutError = TEXT("Completed model event must contain only an optional valid opaque continuation.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::Failed:
		if (!HasOnlyCommonEnvelope() || !Error.IsError() || Error.Category == EUnrealAIErrorCategory::Cancelled ||
			Error.Category == EUnrealAIErrorCategory::Timeout || Continuation.IsValid())
		{
			OutError = TEXT("Failed model event requires a non-cancellation error and no output or continuation.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::Cancelled:
		if (!HasOnlyCommonEnvelope() || Error.Category != EUnrealAIErrorCategory::Cancelled || Continuation.IsValid())
		{
			OutError = TEXT("Cancelled model event requires a cancellation error and no output or continuation.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::TimedOut:
		if (!HasOnlyCommonEnvelope() || Error.Category != EUnrealAIErrorCategory::Timeout || Continuation.IsValid())
		{
			OutError = TEXT("Timed-out model event requires a timeout error and no output or continuation.");
			return false;
		}
		break;
	case EUnrealAIModelEventKind::Invalid:
	default:
		OutError = TEXT("Model event kind is invalid.");
		return false;
	}
	return true;
}

bool FUnrealAIModelContentPart::Validate(FString &OutError) const
{
	using namespace UE::UnrealAI::PhysicalAllocation::Private;
	OutError.Reset();
	if (static_cast<uint8>(Type) > static_cast<uint8>(EUnrealAIContentType::Refusal) ||
		!IsBoundedUtf8(Text, MaxTextUtf8Bytes, true) || !IsBoundedUtf8(Json, MaxJsonUtf8Bytes, true) ||
		!IsBoundedUtf8(MimeType, 256, true) ||
		!HasBoundedArrayStorage(InlineBytes.GetAllocatedSize(), InlineBytes.Num(), MaxInlineBytes, sizeof(uint8)))
	{
		OutError = TEXT("Model content exceeds its type or physical storage bounds.");
		return false;
	}
	if (Type == EUnrealAIContentType::Image)
	{
		const bool bPng = MimeType == TEXT("image/png") && InlineBytes.Num() >= 8 &&
										   FMemory::Memcmp(InlineBytes.GetData(), "\x89PNG\r\n\x1a\n", 8) == 0;
		const bool bJpeg = MimeType == TEXT("image/jpeg") && InlineBytes.Num() >= 3 && InlineBytes[0] == 0xff &&
											InlineBytes[1] == 0xd8 && InlineBytes[2] == 0xff;
		const bool bGif = MimeType == TEXT("image/gif") && InlineBytes.Num() >= 6 &&
										   (FMemory::Memcmp(InlineBytes.GetData(), "GIF87a", 6) == 0 ||
											FMemory::Memcmp(InlineBytes.GetData(), "GIF89a", 6) == 0);
		const bool bWebp = MimeType == TEXT("image/webp") && InlineBytes.Num() >= 12 &&
											FMemory::Memcmp(InlineBytes.GetData(), "RIFF", 4) == 0 &&
											FMemory::Memcmp(InlineBytes.GetData() + 8, "WEBP", 4) == 0;
		if ((!bPng && !bJpeg && !bGif && !bWebp) || !Text.IsEmpty() || !Json.IsEmpty())
		{
			OutError = TEXT("Inline image requires matching encoded bytes and MIME type.");
			return false;
		}
	}
	else if (!InlineBytes.IsEmpty())
	{
		OutError = TEXT("Only inline image input is implemented by this model protocol.");
		return false;
	}
	return true;
}

bool FUnrealAIModelMessage::Validate(FString &OutError) const
{
	using namespace UE::UnrealAI::PhysicalAllocation::Private;
	OutError.Reset();
	if (static_cast<uint8>(Role) > static_cast<uint8>(EUnrealAIModelRole::Tool) || Content.IsEmpty() ||
		!HasBoundedArrayStorage(Content.GetAllocatedSize(), Content.Num(), MaxContentParts,
								sizeof(FUnrealAIModelContentPart)))
	{
		OutError = TEXT("Model message has an invalid role or content count.");
		return false;
	}
	for (const FUnrealAIModelContentPart &Part : Content)
	{
		if (!Part.Validate(OutError))
		{
			return false;
		}
	}
	return true;
}

bool FUnrealAIModelContinuationAccess::TryAcquire(
	TSharedRef<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation, FName ExpectedProviderName,
	FUnrealAIModelContinuationAccess &OutAccess, FString &OutError)
{
	OutAccess.Implementation.Reset();
	OutError.Reset();
	if (Continuation->GetProviderName() != ExpectedProviderName || !Continuation->IsValid())
	{
		OutError = TEXT("Continuation is stale or belongs to a different provider.");
		return false;
	}
	OutAccess.Implementation = MoveTemp(Continuation);
	return true;
}
