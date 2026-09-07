// Copyright EngineWorks. All Rights Reserved.

#include "Gemini/UnrealAIGeminiInteractionsProtocol.h"

#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Misc/Base64.h"
#include "Misc/ScopeLock.h"
#include "Serialization/UnrealAIJsonValidation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
const FName GeminiProviderName(TEXT("gemini.interactions"));
constexpr TCHAR GeminiOrigin[] = TEXT("https://generativelanguage.googleapis.com");
constexpr TCHAR GeminiAudience[] = TEXT("https://generativelanguage.googleapis.com/v1/interactions");
constexpr int32 MaxFingerprintBytes = 8 * 1024 * 1024;

FUnrealAIModelError MakeGeminiError(const EUnrealAIErrorCategory Category, const FName Code, const TCHAR *UserMessage,
									const TCHAR *Diagnostic, const bool bRetryable = false)
{
	FUnrealAIModelError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.UserMessage = FText::FromString(UserMessage);
	Error.DiagnosticMessage = Diagnostic;
	Error.bRetryable = bRetryable;
	return Error;
}

bool SerializeJsonObject(const TSharedRef<FJsonObject> &Object, FString &OutJson)
{
	OutJson.Reset();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	return FJsonSerializer::Serialize(Object, Writer) && Writer->Close();
}

bool ParseJsonObject(const FString &Json, const int32 MaximumBytes, TSharedPtr<FJsonObject> &OutObject)
{
	OutObject.Reset();
	FUnrealAIJsonPreflightLimits Limits;
	Limits.MaxUtf8Bytes = MaximumBytes;
	Limits.MaxStringTokenCodeUnits = MaximumBytes;
	FString Error;
	if (!PreflightUnrealAIJson(Json, Limits, Error))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool IsValidUtf8(const TConstArrayView<uint8> Bytes)
{
	for (int32 Index = 0; Index < Bytes.Num();)
	{
		const uint8 Lead = Bytes[Index++];
		if (Lead <= 0x7f)
		{
			continue;
		}
		int32 Continuations = 0;
		uint32 CodePoint = 0;
		uint32 Minimum = 0;
		if (Lead >= 0xc2 && Lead <= 0xdf)
		{
			Continuations = 1;
			CodePoint = Lead & 0x1f;
			Minimum = 0x80;
		}
		else if (Lead >= 0xe0 && Lead <= 0xef)
		{
			Continuations = 2;
			CodePoint = Lead & 0x0f;
			Minimum = 0x800;
		}
		else if (Lead >= 0xf0 && Lead <= 0xf4)
		{
			Continuations = 3;
			CodePoint = Lead & 0x07;
			Minimum = 0x10000;
		}
		else
		{
			return false;
		}
		if (Continuations > Bytes.Num() - Index)
		{
			return false;
		}
		for (int32 Offset = 0; Offset < Continuations; ++Offset)
		{
			const uint8 Byte = Bytes[Index++];
			if ((Byte & 0xc0) != 0x80)
			{
				return false;
			}
			CodePoint = (CodePoint << 6) | (Byte & 0x3f);
		}
		if (CodePoint < Minimum || CodePoint > 0x10ffff || (CodePoint >= 0xd800 && CodePoint <= 0xdfff))
		{
			return false;
		}
	}
	return true;
}

bool IsImageRequest(const FUnrealAIModelRequest &Request)
{
	for (const FUnrealAIModelMessage &Message : Request.InputMessages)
	{
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			if (Part.Type == EUnrealAIContentType::Image)
			{
				return true;
			}
		}
	}
	return false;
}

bool AppendFingerprintField(FString &InOut, int64 &InOutUtf8Bytes, const FString &Value)
{
	const FTCHARToUTF8 ValueUtf8(*Value);
	const FString Prefix = FString::Printf(TEXT("%d:"), ValueUtf8.Length());
	const FTCHARToUTF8 PrefixUtf8(*Prefix);
	if (InOutUtf8Bytes > MaxFingerprintBytes - PrefixUtf8.Length() - ValueUtf8.Length() - 1)
	{
		return false;
	}
	InOut += Prefix;
	InOut += Value;
	InOut.AppendChar(TEXT(';'));
	InOutUtf8Bytes += PrefixUtf8.Length() + ValueUtf8.Length() + 1;
	return true;
}

bool BuildHistoryFingerprint(const FUnrealAIModelRequest &Request, FString &OutFingerprint)
{
	OutFingerprint.Reset();
	int64 Utf8Bytes = 0;
	auto Append = [&OutFingerprint, &Utf8Bytes](const FString &Value)
	{ return AppendFingerprintField(OutFingerprint, Utf8Bytes, Value); };
	if (!Append(Request.Instructions) || !Append(FString::FromInt(Request.InputMessages.Num())))
	{
		return false;
	}
	for (const FUnrealAIModelMessage &Message : Request.InputMessages)
	{
		if (!Append(Message.MessageId.ToString()) || !Append(FString::FromInt(static_cast<int32>(Message.Role))) ||
			!Append(Message.Participant.ToString()) || !Append(FString::FromInt(Message.Content.Num())))
		{
			return false;
		}
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			if (!Append(FString::FromInt(static_cast<int32>(Part.Type))) || !Append(Part.Text) || !Append(Part.Json) ||
				!Append(Part.MimeType) || !Append(FBase64::Encode(Part.InlineBytes)))
			{
				return false;
			}
		}
	}
	for (const FUnrealAIModelToolDescriptor &Tool : Request.Tools)
	{
		if (!Append(Tool.StableName.ToString()) || !Append(FString::FromInt(Tool.Version)) ||
			!Append(Tool.InvocationName) || !Append(Tool.InputJsonSchema))
		{
			return false;
		}
	}
	if (Request.OutputContract.IsSet() &&
		(!Append(Request.OutputContract->Name) || !Append(Request.OutputContract->JsonSchema) ||
		 !Append(Request.OutputContract->bStrict ? TEXT("1") : TEXT("0"))))
	{
		return false;
	}
	return true;
}

FUnrealAIModelContinuationBinding MakeBinding(const FUnrealAIModelRequest &Request)
{
	FUnrealAIModelContinuationBinding Binding;
	Binding.ProviderName = GeminiProviderName;
	Binding.ConnectionAlias = Request.ConnectionAlias;
	Binding.ModelId = Request.ModelId;
	Binding.ConnectionBinding = Request.ConnectionBinding;
	return Binding;
}

bool MatchesBinding(const FUnrealAIModelContinuationBinding &A, const FUnrealAIModelContinuationBinding &B)
{
	return A.ProviderName == B.ProviderName && A.ConnectionAlias == B.ConnectionAlias && A.ModelId == B.ModelId &&
		   A.ConnectionBinding == B.ConnectionBinding;
}

class FUnrealAIGeminiInteractionsContinuation final : public IUnrealAIModelContinuation
{
  public:
	static const void *TypeToken()
	{
		static const uint8 Token = 0;
		return &Token;
	}

	FUnrealAIGeminiInteractionsContinuation(FUnrealAIModelContinuationBinding InBinding, FString InHistoryFingerprint,
											FString InStepsJson, TSet<FString> InToolCallIds)
		: Binding(MoveTemp(InBinding)), HistoryFingerprint(MoveTemp(InHistoryFingerprint)),
		  StepsJson(MoveTemp(InStepsJson)), ToolCallIds(MoveTemp(InToolCallIds))
	{
	}

	FName GetProviderName() const override
	{
		return GeminiProviderName;
	}

	bool IsValid() const override
	{
		FScopeLock Lock(&Mutex);
		return !bConsumed && !StepsJson.IsEmpty() && !ToolCallIds.IsEmpty();
	}

	bool TryConsume(const FUnrealAIModelContinuationBinding &ExpectedBinding, FString &OutError) const override
	{
		OutError.Reset();
		FScopeLock Lock(&Mutex);
		if (bConsumed || !MatchesBinding(Binding, ExpectedBinding))
		{
			OutError = TEXT("Gemini continuation is consumed or bound to another request context.");
			return false;
		}
		bConsumed = true;
		return true;
	}

	const void *GetImplementationTypeToken() const override
	{
		return TypeToken();
	}

	FString GetRedactedDisplay() const override
	{
		return TEXT("<redacted:gemini-interactions-continuation>");
	}

	bool TryCopyForRequest(const FUnrealAIModelRequest &Request, FString &OutStepsJson, TSet<FString> &OutToolCallIds,
						   FString &OutError) const
	{
		FString Fingerprint;
		if (!BuildHistoryFingerprint(Request, Fingerprint))
		{
			OutError = TEXT("Gemini continuation history exceeds its bound.");
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (bConsumed || !MatchesBinding(Binding, MakeBinding(Request)) || Fingerprint != HistoryFingerprint)
		{
			OutError = TEXT("Gemini continuation history or binding does not match the request.");
			return false;
		}
		OutStepsJson = StepsJson;
		OutToolCallIds = ToolCallIds;
		return true;
	}

  private:
	FUnrealAIModelContinuationBinding Binding;
	FString HistoryFingerprint;
	FString StepsJson;
	TSet<FString> ToolCallIds;
	mutable FCriticalSection Mutex;
	mutable bool bConsumed = false;
};

bool BuildImageBlock(const FUnrealAIModelContentPart &Part, const FUnrealAIGeminiInteractionsBuildContext &Context,
					 TSharedPtr<FJsonValue> &OutValue, FUnrealAIModelError &OutError)
{
	FString ShapeError;
	if (Part.Type != EUnrealAIContentType::Image || !Part.Validate(ShapeError) ||
		Part.InlineBytes.Num() > Context.MaximumImageBytes)
	{
		OutError.Category = EUnrealAIErrorCategory::Vision;
		OutError.Code = TEXT("image_input_invalid");
		OutError.UserMessage =
			FText::FromString(TEXT("Inline image input is invalid or exceeds the configured bound."));
		return false;
	}

	TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("image"));
	Block->SetStringField(TEXT("data"), FBase64::Encode(Part.InlineBytes));
	Block->SetStringField(TEXT("mime_type"), Part.MimeType);
	OutValue = MakeShared<FJsonValueObject>(Block);
	return true;
}

bool BuildContentBlocks(const FUnrealAIModelMessage &Message, const FUnrealAIModelRequest &Request,
						const FUnrealAIGeminiInteractionsBuildContext &Context,
						TArray<TSharedPtr<FJsonValue>> &OutBlocks, FUnrealAIModelError &OutError)
{
	OutBlocks.Reset();
	for (const FUnrealAIModelContentPart &Part : Message.Content)
	{
		TSharedPtr<FJsonValue> Block;
		if (Part.Type == EUnrealAIContentType::Text || Part.Type == EUnrealAIContentType::StructuredJson)
		{
			const FString &Text = Part.Type == EUnrealAIContentType::Text ? Part.Text : Part.Json;
			if (Text.IsEmpty())
			{
				OutError =
					MakeGeminiError(EUnrealAIErrorCategory::InvalidArgument,
									TEXT("gemini_empty_content"),
										 TEXT("Empty Gemini content is not supported."),
											  TEXT("Gemini Interactions rejected an empty normalized content part."));
				return false;
			}
			TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
			TextBlock->SetStringField(TEXT("type"), TEXT("text"));
			TextBlock->SetStringField(TEXT("text"), Text);
			Block = MakeShared<FJsonValueObject>(TextBlock);
		}
		else if (Part.Type == EUnrealAIContentType::Image && Message.Role != EUnrealAIModelRole::Assistant)
		{
			if (!BuildImageBlock(Part, Context, Block, OutError))
			{
				return false;
			}
		}
		else
		{
			OutError = MakeGeminiError(
				EUnrealAIErrorCategory::UnsupportedCapability,
				TEXT("gemini_content_type_unsupported"),
					 TEXT("This content type is not supported by Gemini Interactions."),
						  TEXT("Gemini Interactions rejected a normalized content type or assistant image."));
			return false;
		}
		OutBlocks.Add(MoveTemp(Block));
	}
	return !OutBlocks.IsEmpty();
}

bool ParseRetainedSteps(const FString &Json, TArray<TSharedPtr<FJsonValue>> &OutSteps)
{
	OutSteps.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!ParseJsonObject(Json, FUnrealAIGeminiInteractionsWireRequest::MaxBodyBytes, Root))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>> *Steps = nullptr;
	if (!Root->TryGetArrayField(TEXT("steps"), Steps) || Steps == nullptr || Steps->IsEmpty())
	{
		return false;
	}
	OutSteps = *Steps;
	return true;
}

bool BuildInput(const FUnrealAIModelRequest &Request, const FUnrealAIGeminiInteractionsBuildContext &Context,
				FString &OutSystemInstruction, TArray<TSharedPtr<FJsonValue>> &OutInput,
				TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> &OutContinuation,
				FUnrealAIModelContinuationBinding &OutBinding, FUnrealAIModelError &OutError)
{
	OutSystemInstruction = Request.Instructions;
	OutInput.Reset();
	OutContinuation.Reset();
	OutBinding = {};
	for (const FUnrealAIModelMessage &Message : Request.InputMessages)
	{
		if (Message.Role == EUnrealAIModelRole::System || Message.Role == EUnrealAIModelRole::Developer)
		{
			for (const FUnrealAIModelContentPart &Part : Message.Content)
			{
				const FString *Text = Part.Type == EUnrealAIContentType::Text			  ? &Part.Text
									  : Part.Type == EUnrealAIContentType::StructuredJson ? &Part.Json
																						  : nullptr;
				if (Text == nullptr || Text->IsEmpty())
				{
					OutError = MakeGeminiError(
						EUnrealAIErrorCategory::UnsupportedCapability,
						TEXT("gemini_system_content_unsupported"),
							 TEXT("This system content type is not supported by Gemini."),
								  TEXT("Gemini Interactions accepts only normalized text/JSON system content."));
					return false;
				}
				if (!OutSystemInstruction.IsEmpty())
				{
					OutSystemInstruction += TEXT("\n\n");
				}
				OutSystemInstruction +=
					Message.Role == EUnrealAIModelRole::Developer ? TEXT("[developer]\n") : TEXT("[system]\n");
				OutSystemInstruction += *Text;
			}
			continue;
		}
		FString StepType;
		if (Message.Role == EUnrealAIModelRole::User)
		{
			StepType = TEXT("user_input");
		}
		else if (Message.Role == EUnrealAIModelRole::Assistant)
		{
			StepType = TEXT("model_output");
		}
		else
		{
			OutError = MakeGeminiError(EUnrealAIErrorCategory::UnsupportedCapability,
									   TEXT("gemini_message_role_unsupported"),
											TEXT("This message role is not supported by Gemini."),
												 TEXT("Gemini Interactions rejected a provider-neutral message role."));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Blocks;
		if (!BuildContentBlocks(Message, Request, Context, Blocks, OutError))
		{
			return false;
		}
		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("type"), StepType);
		Step->SetArrayField(TEXT("content"), MoveTemp(Blocks));
		OutInput.Add(MakeShared<FJsonValueObject>(Step));
	}
	if (OutInput.IsEmpty())
	{
		OutError = MakeGeminiError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("gemini_input_empty"),
										TEXT("Gemini requires at least one input step."),
											 TEXT("Gemini Interactions request contained only system instructions."));
		return false;
	}
	if (Request.ToolOutputs.IsEmpty())
	{
		if (Request.Continuation.IsValid())
		{
			OutError = MakeGeminiError(
				EUnrealAIErrorCategory::InvalidArgument,
				TEXT("gemini_continuation_without_outputs"),
					 TEXT("The Gemini continuation requires tool results."),
						  TEXT("Gemini Interactions rejected a continuation without normalized tool outputs."));
			return false;
		}
		return true;
	}

	FUnrealAIModelContinuationAccess Access;
	FString AccessError;
	if (!Request.Continuation.IsValid() ||
		!FUnrealAIModelContinuationAccess::TryAcquire(Request.Continuation.ToSharedRef(), GeminiProviderName, Access,
													  AccessError) ||
		!Access.IsValid() || Access.GetImplementation() == nullptr ||
		Access.GetImplementation()->GetImplementationTypeToken() !=
			FUnrealAIGeminiInteractionsContinuation::TypeToken())
	{
		OutError = MakeGeminiError(
			EUnrealAIErrorCategory::InvalidArgument,
			TEXT("gemini_continuation_invalid"),
				 TEXT("The Gemini continuation is invalid."),
					  TEXT("Gemini Interactions could not acquire its exact continuation implementation."));
		return false;
	}
	const auto *Continuation = static_cast<const FUnrealAIGeminiInteractionsContinuation *>(Access.GetImplementation());
	FString StepsJson;
	TSet<FString> PendingIds;
	if (!Continuation->TryCopyForRequest(Request, StepsJson, PendingIds, AccessError))
	{
		OutError =
			MakeGeminiError(EUnrealAIErrorCategory::InvalidArgument,
							TEXT("gemini_continuation_binding_mismatch"),
								 TEXT("The Gemini continuation does not match this request."),
									  TEXT("Gemini Interactions rejected continuation binding or history drift."));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Steps;
	if (!ParseRetainedSteps(StepsJson, Steps))
	{
		OutError =
			MakeGeminiError(EUnrealAIErrorCategory::ProviderProtocol,
							TEXT("gemini_continuation_payload_invalid"),
								 TEXT("The Gemini continuation cannot be used."),
									  TEXT("Gemini Interactions continuation retained invalid stateless steps."));
		return false;
	}
	// Stateless Gemini tool turns must replay every generated step exactly. Keep
	// the parsed array available below so call IDs can be rebound to their
	// admitted invocation names before emitting function_result steps.
	OutInput.Append(Steps);
	TSet<FString> SuppliedIds;
	for (const FUnrealAIModelToolOutput &Output : Request.ToolOutputs)
	{
		if (!PendingIds.Contains(Output.ProviderCallId) || SuppliedIds.Contains(Output.ProviderCallId))
		{
			OutError = MakeGeminiError(
				EUnrealAIErrorCategory::InvalidArgument,
				TEXT("gemini_tool_output_mismatch"),
					 TEXT("The Gemini tool results are invalid."),
						  TEXT("Gemini Interactions rejected secret, duplicate, or unrequested tool output."));
			return false;
		}
		FString InvocationName;
		// The retained function_call step is the authority for call ID/name;
		// recover it from the admitted tool list by matching the pending call in
		// the retained JSON below.
		for (const TSharedPtr<FJsonValue> &StepValue : Steps)
		{
			const TSharedPtr<FJsonObject> Step = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
			FString Type;
			FString CallId;
			if (Step.IsValid() &&
				Step->TryGetStringField(TEXT("type"), Type) &&
										Type == TEXT("function_call") &&
													 Step->TryGetStringField(TEXT("id"), CallId) &&
																			 CallId == Output.ProviderCallId)
			{
				Step->TryGetStringField(TEXT("name"), InvocationName);
				break;
			}
		}
		if (InvocationName.IsEmpty())
		{
			OutError = MakeGeminiError(
				EUnrealAIErrorCategory::ProviderProtocol,
				TEXT("gemini_continuation_tool_identity_missing"),
					 TEXT("The Gemini continuation cannot be used."),
						  TEXT("Gemini Interactions retained tool call lacks its admitted invocation name."));
			return false;
		}
		TSharedRef<FJsonObject> ResultContent = MakeShared<FJsonObject>();
		ResultContent->SetStringField(TEXT("type"), TEXT("text"));
		ResultContent->SetStringField(TEXT("text"), Output.OutputJson);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("type"), TEXT("function_result"));
		Result->SetStringField(TEXT("name"), InvocationName);
		Result->SetStringField(TEXT("call_id"), Output.ProviderCallId);
		TArray<TSharedPtr<FJsonValue>> ResultParts;
		ResultParts.Add(MakeShared<FJsonValueObject>(ResultContent));
		Result->SetArrayField(TEXT("result"), MoveTemp(ResultParts));
		OutInput.Add(MakeShared<FJsonValueObject>(Result));
		SuppliedIds.Add(Output.ProviderCallId);
	}
	if (SuppliedIds.Num() != PendingIds.Num())
	{
		OutError = MakeGeminiError(
			EUnrealAIErrorCategory::InvalidArgument,
			TEXT("gemini_tool_outputs_incomplete"),
				 TEXT("All Gemini tool results are required."),
					  TEXT("Gemini continuation requires one result for every retained function call."));
		return false;
	}
	OutContinuation = Request.Continuation;
	OutBinding = MakeBinding(Request);
	return true;
}
} // namespace

bool FUnrealAIGeminiInteractionsWireRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (BodyUtf8.IsEmpty() || BodyUtf8.Num() > MaxBodyBytes || !IsValidUtf8(BodyUtf8))
	{
		OutError = TEXT("Gemini Interactions wire body is empty, oversized, or invalid UTF-8.");
		return false;
	}
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR *>(BodyUtf8.GetData()), BodyUtf8.Num());
	TSharedPtr<FJsonObject> Root;
	if (Text.Length() <= 0 || !ParseJsonObject(FString(Text.Length(), Text.Get()), MaxBodyBytes, Root))
	{
		OutError = TEXT("Gemini Interactions wire body is not one bounded JSON object.");
		return false;
	}
	return true;
}

bool FUnrealAIGeminiInteractionsBuildContext::ValidateShape(const bool bRequiresImageResolver, FString &OutError) const
{
	OutError.Reset();
	FString DestinationError;
	if (!Destination.ValidateShape(DestinationError) || Destination.ModelProviderName != GeminiProviderName ||
		Destination.AuthScheme != EUnrealAIAuthScheme::ApiKey ||
		Destination.BillingMode != EUnrealAIBillingMode::ApiMetered ||
		Destination.EndpointOrigin.ToString() != GeminiOrigin || Destination.Audience != GeminiAudience ||
		MaximumImageBytes < 1 || MaximumImageBytes > FUnrealAIGeminiInteractionsWireRequest::MaxResolvedImageBytes)
	{
		OutError = TEXT("Gemini build context requires its exact API-key destination and bounded media resolver.");
		return false;
	}
	return true;
}

bool FUnrealAIGeminiInteractionsContinuationCommit::IsRequired() const
{
	return Continuation.IsValid();
}

bool FUnrealAIGeminiInteractionsContinuationCommit::TryCommit(FString &OutError)
{
	OutError.Reset();
	if (!Continuation.IsValid())
	{
		return true;
	}
	const TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Staged = MoveTemp(Continuation);
	const FUnrealAIModelContinuationBinding StagedBinding = Binding;
	Binding = {};
	return Staged->TryConsume(StagedBinding, OutError);
}

void FUnrealAIGeminiInteractionsContinuationCommit::Reset()
{
	Continuation.Reset();
	Binding = {};
}

bool FUnrealAIGeminiInteractionsRequestBuilder::Build(const FUnrealAIModelRequest &Request,
													  const FUnrealAIGeminiInteractionsBuildContext &Context,
													  FUnrealAIGeminiInteractionsWireRequest &OutWireRequest,
													  FUnrealAIModelError &OutError)
{
	FUnrealAIGeminiInteractionsContinuationCommit Commit;
	if (!Stage(Request, Context, OutWireRequest, Commit, OutError))
	{
		return false;
	}
	FString CommitError;
	if (!Commit.TryCommit(CommitError))
	{
		OutWireRequest = {};
		OutError = MakeGeminiError(
			EUnrealAIErrorCategory::InvalidArgument,
			TEXT("gemini_continuation_commit_failed"),
				 TEXT("The Gemini continuation could not be consumed."),
					  TEXT("Gemini Interactions continuation changed before its staged consume committed."));
		return false;
	}
	return true;
}

bool FUnrealAIGeminiInteractionsRequestBuilder::Stage(
	const FUnrealAIModelRequest &Request, const FUnrealAIGeminiInteractionsBuildContext &Context,
	FUnrealAIGeminiInteractionsWireRequest &OutWireRequest,
	FUnrealAIGeminiInteractionsContinuationCommit &OutContinuationCommit, FUnrealAIModelError &OutError)
{
	OutWireRequest = {};
	OutContinuationCommit.Reset();
	OutError = {};
	FString ShapeError;
	if (!Request.ValidateShape(ShapeError))
	{
		OutError = MakeGeminiError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("gemini_request_invalid"),
										TEXT("The Gemini request is invalid."),
											 TEXT("Gemini Interactions request failed normalized shape validation."));
		return false;
	}
	const bool bHasImages = IsImageRequest(Request);
	if (!Context.ValidateShape(bHasImages, ShapeError))
	{
		OutError = MakeGeminiError(
			EUnrealAIErrorCategory::InvalidConfiguration,
			TEXT("gemini_build_context_invalid"),
				 TEXT("The Gemini connection is invalid."),
					  TEXT("Gemini Interactions request lowering rejected destination or media policy."));
		return false;
	}

	FString SystemInstruction;
	TArray<TSharedPtr<FJsonValue>> Input;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> StagedContinuation;
	FUnrealAIModelContinuationBinding StagedBinding;
	if (!BuildInput(Request, Context, SystemInstruction, Input, StagedContinuation, StagedBinding, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Request.ModelId);
	Root->SetArrayField(TEXT("input"), MoveTemp(Input));
	Root->SetBoolField(TEXT("stream"), true);
	// This adapter intentionally uses exact stateless replay for tool turns. The
	// public API defaults to storage, so opt out explicitly on every request.
	Root->SetBoolField(TEXT("store"), false);
	if (!SystemInstruction.IsEmpty())
	{
		Root->SetStringField(TEXT("system_instruction"), SystemInstruction);
	}
	TSharedRef<FJsonObject> GenerationConfig = MakeShared<FJsonObject>();
	GenerationConfig->SetNumberField(TEXT("max_output_tokens"), Request.MaxOutputTokens);
	Root->SetObjectField(TEXT("generation_config"), GenerationConfig);

	if (!Request.Tools.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		Tools.Reserve(Request.Tools.Num());
		for (const FUnrealAIModelToolDescriptor &Tool : Request.Tools)
		{
			TSharedPtr<FJsonObject> Schema;
			if (!ParseJsonObject(Tool.InputJsonSchema, FUnrealAIModelToolDescriptor::MaxSchemaUtf8Bytes, Schema))
			{
				OutError = MakeGeminiError(
					EUnrealAIErrorCategory::SchemaValidation,
					TEXT("gemini_tool_schema_invalid"),
						 TEXT("A Gemini tool schema is invalid."),
							  TEXT("Gemini Interactions could not parse a normalized tool input schema."));
				return false;
			}
			TSharedRef<FJsonObject> WireTool = MakeShared<FJsonObject>();
			WireTool->SetStringField(TEXT("type"), TEXT("function"));
			WireTool->SetStringField(TEXT("name"), Tool.InvocationName);
			WireTool->SetStringField(TEXT("description"), Tool.Description);
			WireTool->SetObjectField(TEXT("parameters"), Schema.ToSharedRef());
			Tools.Add(MakeShared<FJsonValueObject>(WireTool));
		}
		Root->SetArrayField(TEXT("tools"), MoveTemp(Tools));
	}

	if (Request.OutputContract.IsSet())
	{
		TSharedPtr<FJsonObject> Schema;
		if (!ParseJsonObject(Request.OutputContract->JsonSchema, FUnrealAIModelOutputContract::MaxSchemaUtf8Bytes,
							 Schema))
		{
			OutError =
				MakeGeminiError(EUnrealAIErrorCategory::SchemaValidation,
								TEXT("gemini_output_schema_invalid"),
									 TEXT("The Gemini output schema is invalid."),
										  TEXT("Gemini Interactions could not parse the normalized output contract."));
			return false;
		}
		TSharedRef<FJsonObject> ResponseFormat = MakeShared<FJsonObject>();
		ResponseFormat->SetStringField(TEXT("type"), TEXT("text"));
		ResponseFormat->SetStringField(TEXT("mime_type"), TEXT("application/json"));
		ResponseFormat->SetObjectField(TEXT("schema"), Schema.ToSharedRef());
		Root->SetObjectField(TEXT("response_format"), ResponseFormat);
	}

	FString Json;
	if (!SerializeJsonObject(Root, Json))
	{
		OutError = MakeGeminiError(EUnrealAIErrorCategory::ProviderProtocol,
								   TEXT("gemini_request_serialization_failed"),
										TEXT("The Gemini request could not be created."),
											 TEXT("Gemini Interactions request JSON serialization failed."));
		return false;
	}
	FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() <= 0 || Utf8.Length() > FUnrealAIGeminiInteractionsWireRequest::MaxBodyBytes)
	{
		OutError =
			MakeGeminiError(EUnrealAIErrorCategory::InvalidArgument,
							TEXT("gemini_request_too_large"),
								 TEXT("The Gemini request exceeds the supported size."),
									  TEXT("Gemini Interactions request body exceeded its framework wire bound."));
		return false;
	}
	OutWireRequest.BodyUtf8.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	if (!OutWireRequest.ValidateShape(ShapeError))
	{
		OutWireRequest = {};
		OutError = MakeGeminiError(EUnrealAIErrorCategory::ProviderProtocol,
								   TEXT("gemini_wire_request_invalid"),
										TEXT("The Gemini request could not be created."),
											 TEXT("Gemini Interactions serialized an invalid bounded wire request."));
		return false;
	}
	OutContinuationCommit.Continuation = MoveTemp(StagedContinuation);
	OutContinuationCommit.Binding = MoveTemp(StagedBinding);
	return true;
}

namespace
{
bool TryGetIntegralField(const TSharedPtr<FJsonObject> &Object, const TCHAR *Name, int64 &OutValue,
						 const int64 Maximum = FUnrealAIModelUsageSnapshot::MaxTokenCount)
{
	OutValue = 0;
	double Number = 0.0;
	if (!Object.IsValid() || !Object->TryGetNumberField(Name, Number) || !FMath::IsFinite(Number) || Number < 0.0 ||
		Number > static_cast<double>(Maximum) || FMath::FloorToDouble(Number) != Number)
	{
		return false;
	}
	OutValue = static_cast<int64>(Number);
	return true;
}

bool TryGetStepIndex(const TSharedPtr<FJsonObject> &Object, int32 &OutIndex)
{
	int64 Value = 0;
	if (!TryGetIntegralField(Object, TEXT("index"), Value, FUnrealAIGeminiInteractionsStreamDecoder::MaxSteps - 1))
	{
		return false;
	}
	OutIndex = static_cast<int32>(Value);
	return true;
}

const FUnrealAIModelToolDescriptor *FindToolByInvocation(const FUnrealAIModelRequest &Request,
														 const FString &InvocationName)
{
	return Request.Tools.FindByPredicate([&InvocationName](const FUnrealAIModelToolDescriptor &Tool)
										 { return Tool.InvocationName == InvocationName; });
}

struct FGeminiInteractionStep final
{
	enum class EKind : uint8
	{
		Unknown,
		Thought,
		ModelOutput,
		FunctionCall
	};

	EKind Kind = EKind::Unknown;
	TSharedPtr<FJsonObject> Retained;
	FString Text;
	FString Signature;
	FString ToolCallId;
	FString InvocationName;
	FName StableName;
	int32 Version = 1;
	FString Arguments;
	int32 ToolOutputIndex = INDEX_NONE;
	bool bStopped = false;
	bool bContinuationSafe = true;
};

bool AppendTextContent(TSharedPtr<FJsonObject> &Step, const FString &Delta)
{
	if (!Step.IsValid())
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Content;
	const TArray<TSharedPtr<FJsonValue>> *Existing = nullptr;
	if (Step->TryGetArrayField(TEXT("content"), Existing) && Existing != nullptr)
	{
		Content = *Existing;
	}
	if (!Content.IsEmpty())
	{
		const TSharedPtr<FJsonObject> Last = Content.Last().IsValid() ? Content.Last()->AsObject() : nullptr;
		FString Type;
		FString Text;
		if (Last.IsValid() && Last->TryGetStringField(TEXT("type"), Type) &&
													  Type == TEXT("text") &&
																   Last->TryGetStringField(TEXT("text"), Text))
		{
			Last->SetStringField(TEXT("text"), Text + Delta);
			Step->SetArrayField(TEXT("content"), MoveTemp(Content));
			return true;
		}
	}
	TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), Delta);
	Content.Add(MakeShared<FJsonValueObject>(TextBlock));
	Step->SetArrayField(TEXT("content"), MoveTemp(Content));
	return true;
}

bool AppendThoughtSummary(TSharedPtr<FJsonObject> &Step, const TSharedPtr<FJsonObject> &ContentObject)
{
	if (!Step.IsValid() || !ContentObject.IsValid())
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Summary;
	const TArray<TSharedPtr<FJsonValue>> *Existing = nullptr;
	if (Step->TryGetArrayField(TEXT("summary"), Existing) && Existing != nullptr)
	{
		Summary = *Existing;
	}
	Summary.Add(MakeShared<FJsonValueObject>(ContentObject.ToSharedRef()));
	Step->SetArrayField(TEXT("summary"), MoveTemp(Summary));
	return true;
}

bool ParseInitialTextContent(const TSharedPtr<FJsonObject> &Step, FString &OutText)
{
	OutText.Reset();
	const TArray<TSharedPtr<FJsonValue>> *Content = nullptr;
	if (!Step.IsValid() || !Step->TryGetArrayField(TEXT("content"), Content) || Content == nullptr)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue> &Value : *Content)
	{
		const TSharedPtr<FJsonObject> Block = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Type;
		FString Text;
		if (!Block.IsValid() || !Block->TryGetStringField(TEXT("type"), Type) ||
														  Type != TEXT("text") ||
																	   !Block->TryGetStringField(TEXT("text"), Text))
		{
			return false;
		}
		OutText += Text;
	}
	return true;
}

bool SerializeRetainedSteps(const TMap<int32, FGeminiInteractionStep> &Steps, FString &OutJson,
							TSet<FString> &OutToolIds)
{
	OutJson.Reset();
	OutToolIds.Reset();
	TArray<int32> Indices;
	Steps.GenerateKeyArray(Indices);
	Indices.Sort();
	TArray<TSharedPtr<FJsonValue>> RetainedSteps;
	for (const int32 Index : Indices)
	{
		const FGeminiInteractionStep *Step = Steps.Find(Index);
		if (Step == nullptr || !Step->bStopped || !Step->bContinuationSafe ||
			Step->Kind == FGeminiInteractionStep::EKind::Unknown || !Step->Retained.IsValid())
		{
			return false;
		}
		if (Step->Kind == FGeminiInteractionStep::EKind::Thought && Step->Signature.IsEmpty())
		{
			return false;
		}
		if (Step->Kind == FGeminiInteractionStep::EKind::FunctionCall)
		{
			TSharedPtr<FJsonObject> Arguments;
			if (!ParseJsonObject(Step->Arguments, FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes, Arguments) ||
				OutToolIds.Contains(Step->ToolCallId))
			{
				return false;
			}
			Step->Retained->SetObjectField(TEXT("arguments"), Arguments.ToSharedRef());
			OutToolIds.Add(Step->ToolCallId);
		}
		RetainedSteps.Add(MakeShared<FJsonValueObject>(Step->Retained.ToSharedRef()));
	}
	if (OutToolIds.IsEmpty())
	{
		return false;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("steps"), MoveTemp(RetainedSteps));
	return SerializeJsonObject(Root, OutJson) &&
		   FTCHARToUTF8(*OutJson).Length() <= FUnrealAIGeminiInteractionsWireRequest::MaxBodyBytes;
}
} // namespace

class FUnrealAIGeminiInteractionsStreamDecoder::FImpl final
{
  public:
	FImpl(const FUnrealAIModelRequest &InRequest, FEventSink InSink, FIgnoredEventObserver InIgnoredEventObserver)
		: Request(InRequest), Sink(MoveTemp(InSink)), IgnoredEventObserver(MoveTemp(InIgnoredEventObserver))
	{
		BuildHistoryFingerprint(Request, HistoryFingerprint);
	}

	bool PushBytes(const TConstArrayView<uint8> Bytes, FString &OutError)
	{
		OutError.Reset();
		if (bTerminal && (!bCompleted || bDoneReceived))
		{
			OutError = TEXT("Gemini Interactions stream is already terminal.");
			return false;
		}
		if (Bytes.IsEmpty())
		{
			return true;
		}
		if (TotalStreamBytes > MaxStreamBytes - Bytes.Num() || Buffer.Num() > MaxStreamBytes - Bytes.Num())
		{
			return Fail(TEXT("Gemini Interactions stream exceeded its byte bound."), TEXT("gemini_stream_too_large"),
																						  OutError);
		}
		Buffer.Append(Bytes.GetData(), Bytes.Num());
		TotalStreamBytes += Bytes.Num();
		while (true)
		{
			int32 EventEnd = INDEX_NONE;
			int32 NextEventStart = INDEX_NONE;
			if (!FindEventBoundary(EventEnd, NextEventStart))
			{
				if (Buffer.Num() - Head > MaxEventBytes)
				{
					return Fail(TEXT("Gemini Interactions SSE event exceeded its byte bound."),
									 TEXT("gemini_event_too_large"), OutError);
				}
				break;
			}
			const TConstArrayView<uint8> EventBytes(Buffer.GetData() + Head, EventEnd - Head);
			if (EventBytes.Num() > MaxEventBytes || (!EventBytes.IsEmpty() && !ProcessEvent(EventBytes, OutError)))
			{
				return false;
			}
			Head = NextEventStart;
			if (Head > 64 * 1024 && Head >= Buffer.Num() / 2)
			{
				Buffer.RemoveAt(0, Head, EAllowShrinking::No);
				Head = 0;
			}
		}
		return true;
	}

	bool Finish(FString &OutError)
	{
		OutError.Reset();
		if (bTerminal)
		{
			return bCompleted;
		}
		if (Buffer.Num() != Head)
		{
			return Fail(TEXT("Gemini Interactions stream ended with an incomplete SSE event."),
							 TEXT("gemini_stream_truncated"), OutError);
		}
		return Fail(TEXT("Gemini Interactions stream ended without interaction.completed."),
						 TEXT("gemini_terminal_missing"), OutError);
	}

	void Cancel()
	{
		if (bTerminal)
		{
			return;
		}
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::Cancelled);
		Event.Error = MakeGeminiError(EUnrealAIErrorCategory::Cancelled,
									  TEXT("gemini_request_cancelled"),
										   TEXT("The Gemini request was cancelled."),
												TEXT("Gemini Interactions stream cancelled by caller."));
		Emit(MoveTemp(Event));
		bTerminal = true;
	}

	bool IsTerminal() const
	{
		return bTerminal;
	}

  private:
	bool FindEventBoundary(int32 &OutEventEnd, int32 &OutNextEventStart) const
	{
		OutEventEnd = INDEX_NONE;
		OutNextEventStart = INDEX_NONE;
		int32 LineStart = Head;
		for (int32 Index = Head; Index < Buffer.Num();)
		{
			if (Buffer[Index] != '\r' && Buffer[Index] != '\n')
			{
				++Index;
				continue;
			}
			const int32 LineEnd = Index;
			if (Buffer[Index] == '\r' && Index + 1 < Buffer.Num() && Buffer[Index + 1] == '\n')
			{
				Index += 2;
			}
			else
			{
				++Index;
			}
			if (LineEnd == LineStart)
			{
				OutEventEnd = LineStart;
				OutNextEventStart = Index;
				return true;
			}
			LineStart = Index;
		}
		return false;
	}

	bool ProcessEvent(const TConstArrayView<uint8> EventBytes, FString &OutError)
	{
		if (!IsValidUtf8(EventBytes))
		{
			return Fail(TEXT("Gemini Interactions SSE event is invalid UTF-8."), TEXT("gemini_event_utf8_invalid"),
																					  OutError);
		}
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(EventBytes.GetData()), EventBytes.Num());
		const FString Text(Converted.Length(), Converted.Get());
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FString EventName;
		FString Data;
		for (const FString &Line : Lines)
		{
			if (Line.StartsWith(TEXT(":")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("event:")))
			{
				EventName = Line.Mid(6);
				if (EventName.StartsWith(TEXT(" ")))
				{
					EventName.RightChopInline(1);
				}
			}
			else if (Line.StartsWith(TEXT("data:")))
			{
				FString Piece = Line.Mid(5);
				if (Piece.StartsWith(TEXT(" ")))
				{
					Piece.RightChopInline(1);
				}
				if (!Data.IsEmpty())
				{
					Data.AppendChar(TEXT('\n'));
				}
				Data += Piece;
			}
		}
		if (Data.IsEmpty())
		{
			return true;
		}
		if (EventName == TEXT("done") && Data == TEXT("[DONE]"))
		{
			if (!bCompleted)
			{
				return Fail(TEXT("Gemini Interactions ended before interaction.completed."),
								 TEXT("gemini_done_out_of_order"), OutError);
			}
			bDoneReceived = true;
			return true;
		}
		if (bTerminal)
		{
			OutError = TEXT("Gemini Interactions emitted data after interaction.completed.");
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!ParseJsonObject(Data, MaxEventBytes, Root))
		{
			return Fail(TEXT("Gemini Interactions SSE data is malformed JSON."), TEXT("gemini_event_json_invalid"),
																					  OutError);
		}
		FString EventType;
		if (!Root->TryGetStringField(TEXT("event_type"), EventType) || EventType.IsEmpty() ||
									 (!EventName.IsEmpty() && EventName != EventType))
		{
			return Fail(TEXT("Gemini Interactions SSE event type is missing or inconsistent."),
							 TEXT("gemini_event_type_invalid"), OutError);
		}
		return ProcessTypedEvent(EventType, Root, OutError);
	}

	bool ProcessTypedEvent(const FString &Type, const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		if (Type == TEXT("error"))
		{
			return ProcessProviderError(Root, OutError);
		}
		if (Type == TEXT("interaction.created"))
		{
			return ProcessInteractionCreated(Root, OutError);
		}
		if (Type == TEXT("interaction.status_update"))
		{
			return ProcessStatusUpdate(Root, OutError);
		}
		if (Type == TEXT("step.start"))
		{
			return ProcessStepStart(Root, OutError);
		}
		if (Type == TEXT("step.delta"))
		{
			return ProcessStepDelta(Root, OutError);
		}
		if (Type == TEXT("step.stop"))
		{
			return ProcessStepStop(Root, OutError);
		}
		if (Type == TEXT("interaction.completed"))
		{
			return ProcessInteractionCompleted(Root, OutError);
		}
		if (IgnoredEventObserver)
		{
			IgnoredEventObserver();
		}
		return true;
	}

	bool ProcessInteractionCreated(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		const TSharedPtr<FJsonObject> *Interaction = nullptr;
		FString Status;
		if (bStarted ||
			!Root->TryGetObjectField(
				TEXT("interaction"), Interaction) || Interaction == nullptr || !Interaction->IsValid() ||
				!(*Interaction)
					 ->TryGetStringField(
						 TEXT("id"), ResponseId) || ResponseId.IsEmpty() ||
						 !(*Interaction)->TryGetStringField(TEXT("status"), Status) || Status != TEXT("in_progress"))
		{
			return Fail(TEXT("Gemini Interactions interaction.created is malformed or duplicate."),
							 TEXT("gemini_interaction_created_invalid"), OutError);
		}
		FUnrealAIModelProviderRequestId Id;
		Id.Kind = EUnrealAIModelProviderRequestIdKind::Response;
		Id.Value = ResponseId;
		FString IdError;
		if (!Id.ValidateShape(IdError))
		{
			return Fail(TEXT("Gemini Interactions response ID is invalid."), TEXT("gemini_response_id_invalid"),
																				  OutError);
		}
		bStarted = true;
		Emit(MakeEvent(EUnrealAIModelEventKind::Started));
		FString Model;
		if ((*Interaction)
				->TryGetStringField(TEXT("model"), Model) && !Model.IsEmpty() &&
									FTCHARToUTF8(*Model).Length() <= FUnrealAIModelMetadata::MaxValueUtf8Bytes)
		{
			FUnrealAIModelEvent Metadata = MakeEvent(EUnrealAIModelEventKind::ProviderMetadata);
			Metadata.Metadata.Key = EUnrealAIModelMetadataKey::ModelRevision;
			Metadata.Metadata.Value = MoveTemp(Model);
			Emit(MoveTemp(Metadata));
		}
		return true;
	}

	bool ProcessStatusUpdate(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		FString InteractionId;
		FString Status;
		if (!bStarted || !Root->TryGetStringField(TEXT("interaction_id"), InteractionId) ||
												  InteractionId != ResponseId ||
												  !Root->TryGetStringField(TEXT("status"), Status) || Status.IsEmpty())
		{
			return Fail(TEXT("Gemini Interactions status update is malformed or misbound."),
							 TEXT("gemini_status_update_invalid"), OutError);
		}
		static const TSet<FString> KnownStatuses = {TEXT("in_progress"), TEXT("requires_action")};
		if (!KnownStatuses.Contains(Status) && IgnoredEventObserver)
		{
			IgnoredEventObserver();
		}
		return true;
	}

	bool ProcessStepStart(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		int32 Index = INDEX_NONE;
		const TSharedPtr<FJsonObject> *StepObject = nullptr;
		FString Type;
		if (!bStarted || !TryGetStepIndex(Root, Index) || Steps.Contains(Index) || Steps.Num() >= MaxSteps ||
			!Root->TryGetObjectField(TEXT("step"), StepObject) || StepObject == nullptr || !StepObject->IsValid() ||
									 !(*StepObject)->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
		{
			return Fail(TEXT("Gemini Interactions step.start is malformed or out of order."),
							 TEXT("gemini_step_start_invalid"), OutError);
		}
		FGeminiInteractionStep Step;
		Step.Retained = *StepObject;
		if (Type == TEXT("model_output"))
		{
			Step.Kind = FGeminiInteractionStep::EKind::ModelOutput;
			if (!ParseInitialTextContent(*StepObject, Step.Text) ||
				FTCHARToUTF8(*Step.Text).Length() > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes)
			{
				return Fail(TEXT("Gemini Interactions initial model output is unsupported or oversized."),
								 TEXT("gemini_model_output_start_invalid"), OutError);
			}
			if (!Step.Text.IsEmpty())
			{
				FUnrealAIModelEvent Event =
					MakeEvent(Request.OutputContract.IsSet() ? EUnrealAIModelEventKind::StructuredDelta
															 : EUnrealAIModelEventKind::TextDelta);
				if (Request.OutputContract.IsSet())
				{
					Event.StructuredDelta = Step.Text;
				}
				else
				{
					Event.TextDelta = Step.Text;
				}
				Emit(MoveTemp(Event));
			}
		}
		else if (Type == TEXT("thought"))
		{
			Step.Kind = FGeminiInteractionStep::EKind::Thought;
			(*StepObject)->TryGetStringField(TEXT("signature"), Step.Signature);
			if (FTCHARToUTF8(*Step.Signature).Length() > MaxEventBytes)
			{
				return Fail(TEXT("Gemini Interactions thought signature is oversized."),
								 TEXT("gemini_thought_signature_invalid"), OutError);
			}
		}
		else if (Type == TEXT("function_call"))
		{
			Step.Kind = FGeminiInteractionStep::EKind::FunctionCall;
			if (!(*StepObject)
					 ->TryGetStringField(TEXT("id"), Step.ToolCallId) || Step.ToolCallId.IsEmpty() ||
										 !(*StepObject)
											  ->TryGetStringField(TEXT("name"), Step.InvocationName) ||
																  Step.InvocationName.IsEmpty())
			{
				return Fail(TEXT("Gemini Interactions function call identity is malformed."),
								 TEXT("gemini_tool_start_invalid"), OutError);
			}
			const FUnrealAIModelToolDescriptor *Tool = FindToolByInvocation(Request, Step.InvocationName);
			if (Tool == nullptr || ToolCallIds.Contains(Step.ToolCallId))
			{
				return Fail(TEXT("Gemini Interactions requested an unknown or duplicate function."),
								 TEXT("gemini_tool_not_admitted"), OutError);
			}
			Step.StableName = Tool->StableName;
			Step.Version = Tool->Version;
			Step.ToolOutputIndex = NextToolOutputIndex++;
			const TSharedPtr<FJsonObject> *InitialArguments = nullptr;
			if ((*StepObject)
					->TryGetObjectField(TEXT("arguments"), InitialArguments) && InitialArguments != nullptr &&
										InitialArguments->IsValid() && (*InitialArguments)->Values.Num() > 0 &&
										!SerializeJsonObject((*InitialArguments).ToSharedRef(), Step.Arguments))
			{
				return Fail(TEXT("Gemini Interactions initial function arguments are malformed."),
								 TEXT("gemini_tool_arguments_invalid"), OutError);
			}
			ToolCallIds.Add(Step.ToolCallId);
			FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::ToolCallStarted);
			Event.ToolCallId = Step.ToolCallId;
			Event.ToolOutputIndex = Step.ToolOutputIndex;
			Event.ToolStableName = Step.StableName;
			Event.ToolVersion = Step.Version;
			Emit(MoveTemp(Event));
			if (!Step.Arguments.IsEmpty())
			{
				FUnrealAIModelEvent ArgumentsEvent = MakeEvent(EUnrealAIModelEventKind::ToolCallArgumentsDelta);
				ArgumentsEvent.ToolCallId = Step.ToolCallId;
				ArgumentsEvent.ToolOutputIndex = Step.ToolOutputIndex;
				ArgumentsEvent.ToolArgumentsDelta = Step.Arguments;
				Emit(MoveTemp(ArgumentsEvent));
			}
		}
		else
		{
			Step.bContinuationSafe = false;
			if (IgnoredEventObserver)
			{
				IgnoredEventObserver();
			}
		}
		Steps.Add(Index, MoveTemp(Step));
		return true;
	}

	bool ProcessStepDelta(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		int32 Index = INDEX_NONE;
		const TSharedPtr<FJsonObject> *Delta = nullptr;
		FString Type;
		if (!TryGetStepIndex(Root, Index) ||
			!Root->TryGetObjectField(TEXT("delta"), Delta) || Delta == nullptr || !Delta->IsValid() ||
									 !(*Delta)->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
		{
			return Fail(TEXT("Gemini Interactions step.delta is malformed."), TEXT("gemini_step_delta_invalid"),
																				   OutError);
		}
		FGeminiInteractionStep *Step = Steps.Find(Index);
		if (Step == nullptr || Step->bStopped)
		{
			return Fail(TEXT("Gemini Interactions step.delta is out of order."), TEXT("gemini_step_delta_out_of_order"),
																					  OutError);
		}
		if (Step->Kind == FGeminiInteractionStep::EKind::Unknown)
		{
			Step->bContinuationSafe = false;
			if (IgnoredEventObserver)
			{
				IgnoredEventObserver();
			}
			return true;
		}
		if (Type == TEXT("text") && Step->Kind == FGeminiInteractionStep::EKind::ModelOutput)
		{
			FString DeltaText;
			if (!(*Delta)->TryGetStringField(
					TEXT("text"), DeltaText) || DeltaText.IsEmpty() ||
					FTCHARToUTF8(*DeltaText).Length() > FUnrealAIModelEvent::MaxTextDeltaUtf8Bytes ||
					FTCHARToUTF8(*(Step->Text + DeltaText)).Length() > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes ||
					!AppendTextContent(Step->Retained, DeltaText))
			{
				return Fail(TEXT("Gemini Interactions text delta is empty or oversized."),
								 TEXT("gemini_text_delta_invalid"), OutError);
			}
			Step->Text += DeltaText;
			FUnrealAIModelEvent Event =
				MakeEvent(Request.OutputContract.IsSet() ? EUnrealAIModelEventKind::StructuredDelta
														 : EUnrealAIModelEventKind::TextDelta);
			if (Request.OutputContract.IsSet())
			{
				Event.StructuredDelta = MoveTemp(DeltaText);
			}
			else
			{
				Event.TextDelta = MoveTemp(DeltaText);
			}
			Emit(MoveTemp(Event));
			return true;
		}
		if (Type == TEXT("arguments_delta") && Step->Kind == FGeminiInteractionStep::EKind::FunctionCall)
		{
			FString Partial;
			if (!(*Delta)->TryGetStringField(TEXT("arguments"), Partial) || Partial.IsEmpty() ||
											 FTCHARToUTF8(*Partial).Length() >
												 FUnrealAIModelEvent::MaxToolArgumentsDeltaUtf8Bytes ||
											 FTCHARToUTF8(*(Step->Arguments + Partial)).Length() >
												 FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes)
			{
				return Fail(TEXT("Gemini Interactions function arguments delta is empty or oversized."),
								 TEXT("gemini_tool_delta_invalid"), OutError);
			}
			Step->Arguments += Partial;
			FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::ToolCallArgumentsDelta);
			Event.ToolCallId = Step->ToolCallId;
			Event.ToolOutputIndex = Step->ToolOutputIndex;
			Event.ToolArgumentsDelta = MoveTemp(Partial);
			Emit(MoveTemp(Event));
			return true;
		}
		if (Type == TEXT("thought_signature") && Step->Kind == FGeminiInteractionStep::EKind::Thought)
		{
			FString Signature;
			if (!(*Delta)->TryGetStringField(TEXT("signature"), Signature) || Signature.IsEmpty() ||
											 FTCHARToUTF8(*Signature).Length() > MaxEventBytes)
			{
				return Fail(TEXT("Gemini Interactions thought signature delta is invalid."),
								 TEXT("gemini_thought_signature_invalid"), OutError);
			}
			Step->Signature = MoveTemp(Signature);
			Step->Retained->SetStringField(TEXT("signature"), Step->Signature);
			return true;
		}
		if (Type == TEXT("thought_summary") && Step->Kind == FGeminiInteractionStep::EKind::Thought)
		{
			const TSharedPtr<FJsonObject> *Content = nullptr;
			FString ContentType;
			if (!(*Delta)->TryGetObjectField(
					TEXT("content"), Content) || Content == nullptr || !Content->IsValid() ||
					!(*Content)->TryGetStringField(TEXT("type"), ContentType) ||
												   (ContentType != TEXT("text") && ContentType != TEXT("image")) ||
													!AppendThoughtSummary(Step->Retained, *Content))
			{
				return Fail(TEXT("Gemini Interactions thought summary delta is invalid."),
								 TEXT("gemini_thought_summary_invalid"), OutError);
			}
			// Thought summaries remain opaque continuation material and never become
			// provider-neutral text or observer diagnostics.
			return true;
		}
		if (IgnoredEventObserver)
		{
			IgnoredEventObserver();
		}
		Step->bContinuationSafe = false;
		return true;
	}

	bool ProcessStepStop(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		int32 Index = INDEX_NONE;
		if (!TryGetStepIndex(Root, Index))
		{
			return Fail(TEXT("Gemini Interactions step.stop is malformed."), TEXT("gemini_step_stop_invalid"),
																				  OutError);
		}
		FGeminiInteractionStep *Step = Steps.Find(Index);
		if (Step == nullptr || Step->bStopped)
		{
			return Fail(TEXT("Gemini Interactions step.stop is duplicate or out of order."),
							 TEXT("gemini_step_stop_out_of_order"), OutError);
		}
		Step->bStopped = true;
		if (Step->Kind == FGeminiInteractionStep::EKind::Unknown)
		{
			return true;
		}
		if (Step->Kind == FGeminiInteractionStep::EKind::Thought)
		{
			if (Step->Signature.IsEmpty())
			{
				return Fail(TEXT("Gemini Interactions thought step omitted its required signature."),
								 TEXT("gemini_thought_signature_missing"), OutError);
			}
			return true;
		}
		if (Step->Kind == FGeminiInteractionStep::EKind::ModelOutput)
		{
			if (Step->Text.IsEmpty())
			{
				return Fail(TEXT("Gemini Interactions completed an empty model output step."),
								 TEXT("gemini_text_empty"), OutError);
			}
			FUnrealAIModelEvent Event =
				MakeEvent(Request.OutputContract.IsSet() ? EUnrealAIModelEventKind::StructuredCompleted
														 : EUnrealAIModelEventKind::TextCompleted);
			if (Request.OutputContract.IsSet())
			{
				TSharedPtr<FJsonObject> Structured;
				if (!ParseJsonObject(Step->Text, FUnrealAIModelEvent::MaxStructuredJsonUtf8Bytes, Structured))
				{
					return Fail(TEXT("Gemini Interactions structured output is not a JSON object."),
									 TEXT("gemini_structured_output_invalid"), OutError);
				}
				Event.StructuredJson = Step->Text;
			}
			else
			{
				Event.FinalText = Step->Text;
			}
			Emit(MoveTemp(Event));
			bHasUsableOutput = true;
			return true;
		}

		if (Step->Arguments.IsEmpty())
		{
			Step->Arguments = TEXT("{}");
		}
		TSharedPtr<FJsonObject> Arguments;
		if (!ParseJsonObject(Step->Arguments, FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes, Arguments))
		{
			return Fail(TEXT("Gemini Interactions completed malformed function arguments."),
							 TEXT("gemini_tool_arguments_invalid"), OutError);
		}
		Step->Retained->SetObjectField(TEXT("arguments"), Arguments.ToSharedRef());
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::ToolCallCompleted);
		Event.ToolOutputIndex = Step->ToolOutputIndex;
		Event.ToolCall.ProviderCallId = Step->ToolCallId;
		Event.ToolCall.StableName = Step->StableName;
		Event.ToolCall.Version = Step->Version;
		Event.ToolCall.ArgumentsJson = Step->Arguments;
		Emit(MoveTemp(Event));
		bHasUsableOutput = true;
		return true;
	}

	bool ProcessInteractionCompleted(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		const TSharedPtr<FJsonObject> *Interaction = nullptr;
		const TSharedPtr<FJsonObject> *Usage = nullptr;
		FString Id;
		FString Status;
		if (!bStarted || bUsageEmitted ||
			!Root->TryGetObjectField(
				TEXT("interaction"), Interaction) || Interaction == nullptr || !Interaction->IsValid() ||
				!(*Interaction)
					 ->TryGetStringField(
						 TEXT("id"), Id) || Id != ResponseId ||
						 !(*Interaction)
							  ->TryGetStringField(
								  TEXT("status"), Status) ||
								  (Status != TEXT("completed") &&
												  Status != TEXT("requires_action") &&
																 Status != TEXT("incomplete") &&
																				Status != TEXT("failed") &&
																							   Status !=
																								   TEXT("cancelled")))
		{
			return Fail(TEXT("Gemini Interactions interaction.completed is malformed or misbound."),
							 TEXT("gemini_interaction_completed_invalid"), OutError);
		}

		const bool bFailureTerminal = Status == TEXT("failed") || Status == TEXT("cancelled");
		const bool bHasUsage = (*Interaction)->TryGetObjectField(TEXT("usage"), Usage);
		if ((bHasUsage && (Usage == nullptr || !Usage->IsValid())) || (!bFailureTerminal && !bHasUsage))
		{
			return Fail(TEXT("Gemini Interactions final usage is missing or malformed."), TEXT("gemini_usage_invalid"),
																							   OutError);
		}
		if (!bFailureTerminal)
		{
			for (const TPair<int32, FGeminiInteractionStep> &Pair : Steps)
			{
				if (!Pair.Value.bStopped)
				{
					return Fail(TEXT("Gemini Interactions completed with an open step."), TEXT("gemini_step_open"),
																							   OutError);
				}
			}
			const bool bTextCompletion =
				ToolCallIds.IsEmpty() && (Status == TEXT("completed") || Status == TEXT("incomplete"));
			const bool bToolCompletion = !ToolCallIds.IsEmpty() && Status == TEXT("requires_action");
			if (!bHasUsableOutput || (!bTextCompletion && !bToolCompletion))
			{
				return Fail(TEXT("Gemini Interactions completed with inconsistent output status."),
								 TEXT("gemini_completion_status_invalid"), OutError);
			}
		}

		FUnrealAIModelEvent StatusMetadata = MakeEvent(EUnrealAIModelEventKind::ProviderMetadata);
		StatusMetadata.Metadata.Key = EUnrealAIModelMetadataKey::ResponseStatus;
		StatusMetadata.Metadata.Value = Status;
		Emit(MoveTemp(StatusMetadata));
		if (Status == TEXT("incomplete"))
		{
			FUnrealAIModelEvent FinishMetadata = MakeEvent(EUnrealAIModelEventKind::ProviderMetadata);
			FinishMetadata.Metadata.Key = EUnrealAIModelMetadataKey::FinishReason;
			FinishMetadata.Metadata.Value = TEXT("incomplete");
			Emit(MoveTemp(FinishMetadata));
		}
		FString ServiceTier;
		if ((*Interaction)->TryGetStringField(TEXT("service_tier"), ServiceTier) && !ServiceTier.IsEmpty())
		{
			static const TSet<FString> KnownTiers = {TEXT("standard"), TEXT("priority"), TEXT("flex")};
			FUnrealAIModelEvent TierMetadata = MakeEvent(EUnrealAIModelEventKind::ProviderMetadata);
			TierMetadata.Metadata.Key = EUnrealAIModelMetadataKey::ServiceTier;
			TierMetadata.Metadata.Value = KnownTiers.Contains(ServiceTier) ? ServiceTier : TEXT("other");
			Emit(MoveTemp(TierMetadata));
		}
		if (bHasUsage)
		{
			int64 ProviderInput = 0;
			int64 ProviderVisibleOutput = 0;
			int64 ProviderTotal = 0;
			int64 ProviderCached = 0;
			int64 ProviderThought = 0;
			if (!TryGetIntegralField(*Usage, TEXT("total_input_tokens"), ProviderInput) ||
									 !TryGetIntegralField(*Usage, TEXT("total_output_tokens"), ProviderVisibleOutput) ||
														  !TryGetIntegralField(
															  *Usage, TEXT("total_tokens"), ProviderTotal) ||
															  ProviderTotal < ProviderInput ||
															  ProviderTotal - ProviderInput < ProviderVisibleOutput)
			{
				return Fail(TEXT("Gemini Interactions final usage is malformed."), TEXT("gemini_usage_invalid"),
																						OutError);
			}
			if ((*Usage)->HasField(TEXT("total_cached_tokens")) &&
								   (!TryGetIntegralField(*Usage, TEXT("total_cached_tokens"), ProviderCached) ||
														 ProviderCached > ProviderInput))
			{
				return Fail(TEXT("Gemini Interactions cached usage is malformed."), TEXT("gemini_usage_invalid"),
																						 OutError);
			}
			const int64 AggregateOutput = ProviderTotal - ProviderInput;
			if ((*Usage)->HasField(TEXT("total_thought_tokens")) &&
								   (!TryGetIntegralField(*Usage, TEXT("total_thought_tokens"), ProviderThought) ||
														 ProviderThought > AggregateOutput))
			{
				return Fail(TEXT("Gemini Interactions thought usage is malformed."), TEXT("gemini_usage_invalid"),
																						  OutError);
			}
			FUnrealAIModelEvent UsageEvent = MakeEvent(EUnrealAIModelEventKind::UsageUpdated);
			UsageEvent.Usage.InputTokens = ProviderInput;
			UsageEvent.Usage.CachedInputTokens = ProviderCached;
			UsageEvent.Usage.OutputTokens = AggregateOutput;
			UsageEvent.Usage.ReasoningOutputTokens = ProviderThought;
			UsageEvent.Usage.TotalTokens = ProviderTotal;
			UsageEvent.Usage.bFinal = true;
			Emit(MoveTemp(UsageEvent));
			bUsageEmitted = true;
		}

		if (bFailureTerminal)
		{
			EUnrealAIModelEventKind Kind =
				Status == TEXT("cancelled") ? EUnrealAIModelEventKind::Cancelled : EUnrealAIModelEventKind::Failed;
			EUnrealAIErrorCategory Category =
				Status == TEXT("cancelled") ? EUnrealAIErrorCategory::Cancelled : EUnrealAIErrorCategory::Provider;
			FName Code = Status == TEXT("cancelled") ? FName(TEXT("gemini_interaction_cancelled"))
													 : FName(TEXT("gemini_interaction_failed"));
			bool bRetryable = false;
			const TSharedPtr<FJsonObject> *InteractionError = nullptr;
			if ((*Interaction)->TryGetObjectField(TEXT("error"), InteractionError))
			{
				if (InteractionError == nullptr || !InteractionError->IsValid())
				{
					return Fail(TEXT("Gemini Interactions terminal error is malformed."),
									 TEXT("gemini_interaction_error_invalid"), OutError);
				}
				int64 RpcCode = 0;
				if ((*InteractionError)
						->HasField(TEXT("code")) && !TryGetIntegralField(*InteractionError, TEXT("code"), RpcCode))
				{
					return Fail(TEXT("Gemini Interactions terminal error code is malformed."),
									 TEXT("gemini_interaction_error_invalid"), OutError);
				}
				if (Status == TEXT("failed"))
				{
					switch (RpcCode)
					{
					case 3:
						Category = EUnrealAIErrorCategory::InvalidArgument;
						Code = TEXT("gemini_invalid_request");
						break;
					case 4:
						Kind = EUnrealAIModelEventKind::TimedOut;
						Category = EUnrealAIErrorCategory::Timeout;
						Code = TEXT("gemini_interaction_timed_out");
						bRetryable = true;
						break;
					case 7:
						Category = EUnrealAIErrorCategory::PolicyDenied;
						Code = TEXT("gemini_permission_denied");
						break;
					case 8:
						Category = EUnrealAIErrorCategory::RateLimited;
						Code = TEXT("gemini_rate_limited");
						bRetryable = true;
						break;
					case 14:
						Code = TEXT("gemini_unavailable");
						bRetryable = true;
						break;
					case 16:
						Category = EUnrealAIErrorCategory::NotAuthorized;
						Code = TEXT("gemini_not_authorized");
						break;
					default:
						break;
					}
				}
			}
			FUnrealAIModelEvent Terminal = MakeEvent(Kind);
			Terminal.Error = MakeGeminiError(
				Category, Code,
				Status == TEXT("cancelled")
					? TEXT("The Gemini interaction was cancelled.")
					: TEXT("The Gemini interaction failed."),
						   TEXT("Gemini Interactions emitted a terminal interaction resource."), bRetryable);
			bTerminal = true;
			bCompleted = true;
			Emit(MoveTemp(Terminal));
			return true;
		}

		FUnrealAIModelEvent Completed = MakeEvent(EUnrealAIModelEventKind::Completed);
		if (Status == TEXT("requires_action"))
		{
			FString StepsJson;
			TSet<FString> RetainedToolIds;
			if (HistoryFingerprint.IsEmpty() || !SerializeRetainedSteps(Steps, StepsJson, RetainedToolIds) ||
				RetainedToolIds.Num() != ToolCallIds.Num())
			{
				return Fail(TEXT("Gemini Interactions could not retain its stateless tool continuation."),
								 TEXT("gemini_continuation_invalid"), OutError);
			}
			Completed.Continuation = MakeShared<FUnrealAIGeminiInteractionsContinuation, ESPMode::ThreadSafe>(
				MakeBinding(Request), HistoryFingerprint, MoveTemp(StepsJson), MoveTemp(RetainedToolIds));
			if (!Completed.Continuation->IsValid())
			{
				return Fail(TEXT("Gemini Interactions produced an invalid tool continuation."),
								 TEXT("gemini_continuation_invalid"), OutError);
			}
		}
		bTerminal = true;
		bCompleted = true;
		Emit(MoveTemp(Completed));
		return true;
	}

	bool ProcessProviderError(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		const TSharedPtr<FJsonObject> *ErrorObject = nullptr;
		FString CodeText;
		if (!Root->TryGetObjectField(TEXT("error"), ErrorObject) || ErrorObject == nullptr || !ErrorObject->IsValid() ||
									 !(*ErrorObject)->TryGetStringField(TEXT("code"), CodeText) || CodeText.IsEmpty())
		{
			return Fail(TEXT("Gemini Interactions error event is malformed."), TEXT("gemini_error_event_invalid"),
																					OutError);
		}
		CodeText.ToLowerInline();
		EUnrealAIErrorCategory Category = EUnrealAIErrorCategory::Provider;
		FName Code(TEXT("gemini_provider_error"));
		bool bRetryable = false;
		if (CodeText == TEXT("resource_exhausted") || CodeText == TEXT("rate_limit_exceeded"))
		{
			Category = EUnrealAIErrorCategory::RateLimited;
			Code = TEXT("gemini_rate_limited");
			bRetryable = true;
		}
		else if (CodeText == TEXT("gateway_timeout") || CodeText == TEXT("deadline_exceeded") ||
																		 CodeText == TEXT("unavailable"))
		{
			Code = TEXT("gemini_unavailable");
			bRetryable = true;
		}
		else if (CodeText == TEXT("unauthenticated"))
		{
			Category = EUnrealAIErrorCategory::NotAuthorized;
			Code = TEXT("gemini_not_authorized");
		}
		else if (CodeText == TEXT("permission_denied"))
		{
			Category = EUnrealAIErrorCategory::PolicyDenied;
			Code = TEXT("gemini_permission_denied");
		}
		else if (CodeText == TEXT("invalid_argument"))
		{
			Category = EUnrealAIErrorCategory::InvalidArgument;
			Code = TEXT("gemini_invalid_request");
		}
		if (!bStarted)
		{
			bStarted = true;
			Emit(MakeEvent(EUnrealAIModelEventKind::Started));
		}
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::Failed);
		Event.Error = MakeGeminiError(Category, Code,
									  TEXT("Gemini could not complete the request."),
										   TEXT("Gemini Interactions emitted a terminal provider error."), bRetryable);
		bTerminal = true;
		Emit(MoveTemp(Event));
		OutError = TEXT("Gemini Interactions emitted a terminal provider error.");
		return false;
	}

	FUnrealAIModelEvent MakeEvent(const EUnrealAIModelEventKind Kind)
	{
		FUnrealAIModelEvent Event;
		Event.RequestId = Request.RequestId;
		Event.Kind = Kind;
		Event.SequenceNumber = ++Sequence;
		if (!ResponseId.IsEmpty())
		{
			FUnrealAIModelProviderRequestId Id;
			Id.Kind = EUnrealAIModelProviderRequestIdKind::Response;
			Id.Value = ResponseId;
			Event.ProviderRequestIds.Add(MoveTemp(Id));
		}
		return Event;
	}

	void Emit(FUnrealAIModelEvent &&Event)
	{
		if (Sink)
		{
			Sink(MoveTemp(Event));
		}
	}

	bool Fail(const TCHAR *Diagnostic, const FName Code, FString &OutError)
	{
		OutError = Diagnostic;
		if (bTerminal)
		{
			return false;
		}
		if (!bStarted)
		{
			bStarted = true;
			Emit(MakeEvent(EUnrealAIModelEventKind::Started));
		}
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::Failed);
		Event.Error = MakeGeminiError(EUnrealAIErrorCategory::ProviderProtocol, Code,
									  TEXT("The Gemini response was invalid."), Diagnostic);
		bTerminal = true;
		Emit(MoveTemp(Event));
		return false;
	}

	FUnrealAIModelRequest Request;
	FEventSink Sink;
	FIgnoredEventObserver IgnoredEventObserver;
	TArray<uint8> Buffer;
	TMap<int32, FGeminiInteractionStep> Steps;
	TSet<FString> ToolCallIds;
	FString HistoryFingerprint;
	FString ResponseId;
	int32 Head = 0;
	int32 TotalStreamBytes = 0;
	int32 NextToolOutputIndex = 0;
	int64 Sequence = 0;
	bool bStarted = false;
	bool bHasUsableOutput = false;
	bool bUsageEmitted = false;
	bool bTerminal = false;
	bool bCompleted = false;
	bool bDoneReceived = false;
};

FUnrealAIGeminiInteractionsStreamDecoder::FUnrealAIGeminiInteractionsStreamDecoder(
	const FUnrealAIModelRequest &Request, FEventSink InSink, FIgnoredEventObserver InIgnoredEventObserver)
	: Impl(MakeUnique<FImpl>(Request, MoveTemp(InSink), MoveTemp(InIgnoredEventObserver)))
{
}

FUnrealAIGeminiInteractionsStreamDecoder::~FUnrealAIGeminiInteractionsStreamDecoder() = default;

bool FUnrealAIGeminiInteractionsStreamDecoder::PushBytes(const TConstArrayView<uint8> Bytes, FString &OutError)
{
	return Impl.IsValid() && Impl->PushBytes(Bytes, OutError);
}

bool FUnrealAIGeminiInteractionsStreamDecoder::Finish(FString &OutError)
{
	return Impl.IsValid() && Impl->Finish(OutError);
}

void FUnrealAIGeminiInteractionsStreamDecoder::Cancel()
{
	if (Impl.IsValid())
	{
		Impl->Cancel();
	}
}

bool FUnrealAIGeminiInteractionsStreamDecoder::IsTerminal() const
{
	return Impl.IsValid() && Impl->IsTerminal();
}
