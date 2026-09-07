// Copyright EngineWorks. All Rights Reserved.

#include "Anthropic/UnrealAIAnthropicMessagesProtocol.h"

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
const FName AnthropicProviderName(TEXT("anthropic.messages"));
constexpr TCHAR AnthropicOrigin[] = TEXT("https://api.anthropic.com");
constexpr TCHAR AnthropicAudience[] = TEXT("https://api.anthropic.com/v1/messages");
constexpr int32 MaxFingerprintBytes = 8 * 1024 * 1024;

FUnrealAIModelError MakeAnthropicError(const EUnrealAIErrorCategory Category, const FName Code,
									   const TCHAR *UserMessage, const TCHAR *Diagnostic, const bool bRetryable = false)
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

bool AppendFingerprintField(FString &InOut, const FString &Value)
{
	const FTCHARToUTF8 Utf8(*Value);
	InOut += FString::FromInt(Utf8.Length());
	InOut.AppendChar(TEXT(':'));
	InOut += Value;
	InOut.AppendChar(TEXT(';'));
	return FTCHARToUTF8(*InOut).Length() <= MaxFingerprintBytes;
}

bool BuildHistoryFingerprint(const FUnrealAIModelRequest &Request, FString &OutFingerprint)
{
	OutFingerprint.Reset();
	if (!AppendFingerprintField(OutFingerprint, Request.Instructions) ||
		!AppendFingerprintField(OutFingerprint, FString::FromInt(Request.InputMessages.Num())))
	{
		return false;
	}
	for (const FUnrealAIModelMessage &Message : Request.InputMessages)
	{
		if (!AppendFingerprintField(OutFingerprint, Message.MessageId.ToString()) ||
			!AppendFingerprintField(OutFingerprint, FString::FromInt(static_cast<int32>(Message.Role))) ||
			!AppendFingerprintField(OutFingerprint, Message.Participant.ToString()) ||
			!AppendFingerprintField(OutFingerprint, FString::FromInt(Message.Content.Num())))
		{
			return false;
		}
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			if (!AppendFingerprintField(OutFingerprint, FString::FromInt(static_cast<int32>(Part.Type))) ||
				!AppendFingerprintField(OutFingerprint, Part.Text) ||
				!AppendFingerprintField(OutFingerprint, Part.Json) ||
				!AppendFingerprintField(OutFingerprint, Part.MimeType) ||
				!AppendFingerprintField(OutFingerprint, FBase64::Encode(Part.InlineBytes)))
			{
				return false;
			}
		}
	}
	for (const FUnrealAIModelToolDescriptor &Tool : Request.Tools)
	{
		if (!AppendFingerprintField(OutFingerprint, Tool.StableName.ToString()) ||
			!AppendFingerprintField(OutFingerprint, FString::FromInt(Tool.Version)) ||
			!AppendFingerprintField(OutFingerprint, Tool.InvocationName) ||
			!AppendFingerprintField(OutFingerprint, Tool.InputJsonSchema))
		{
			return false;
		}
	}
	if (Request.OutputContract.IsSet() &&
		(!AppendFingerprintField(OutFingerprint, Request.OutputContract->Name) ||
		 !AppendFingerprintField(OutFingerprint, Request.OutputContract->JsonSchema) ||
		 !AppendFingerprintField(OutFingerprint, Request.OutputContract->bStrict ? TEXT("1") : TEXT("0"))))
	{
		return false;
	}
	return true;
}

FUnrealAIModelContinuationBinding MakeBinding(const FUnrealAIModelRequest &Request)
{
	FUnrealAIModelContinuationBinding Binding;
	Binding.ProviderName = AnthropicProviderName;
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

class FUnrealAIAnthropicMessagesContinuation final : public IUnrealAIModelContinuation
{
  public:
	static const void *TypeToken()
	{
		static const uint8 Token = 0;
		return &Token;
	}

	FUnrealAIAnthropicMessagesContinuation(FUnrealAIModelContinuationBinding InBinding, FString InHistoryFingerprint,
										   FString InAssistantContentJson, TSet<FString> InToolCallIds)
		: Binding(MoveTemp(InBinding)), HistoryFingerprint(MoveTemp(InHistoryFingerprint)),
		  AssistantContentJson(MoveTemp(InAssistantContentJson)), ToolCallIds(MoveTemp(InToolCallIds))
	{
	}

	FName GetProviderName() const override
	{
		return AnthropicProviderName;
	}

	bool IsValid() const override
	{
		FScopeLock Lock(&Mutex);
		return !bConsumed && !AssistantContentJson.IsEmpty() && !ToolCallIds.IsEmpty();
	}

	bool TryConsume(const FUnrealAIModelContinuationBinding &ExpectedBinding, FString &OutError) const override
	{
		OutError.Reset();
		FScopeLock Lock(&Mutex);
		if (bConsumed || !MatchesBinding(Binding, ExpectedBinding))
		{
			OutError = TEXT("Anthropic continuation is consumed or bound to another request context.");
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
		return TEXT("<redacted:anthropic-messages-continuation>");
	}

	bool TryCopyForRequest(const FUnrealAIModelRequest &Request, FString &OutAssistantContentJson,
						   TSet<FString> &OutToolCallIds, FString &OutError) const
	{
		OutAssistantContentJson.Reset();
		OutToolCallIds.Reset();
		FString Fingerprint;
		if (!BuildHistoryFingerprint(Request, Fingerprint))
		{
			OutError = TEXT("Anthropic continuation history exceeds its bound.");
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (bConsumed || !MatchesBinding(Binding, MakeBinding(Request)) || Fingerprint != HistoryFingerprint)
		{
			OutError = TEXT("Anthropic continuation history or binding does not match the request.");
			return false;
		}
		OutAssistantContentJson = AssistantContentJson;
		OutToolCallIds = ToolCallIds;
		return true;
	}

  private:
	FUnrealAIModelContinuationBinding Binding;
	FString HistoryFingerprint;
	FString AssistantContentJson;
	TSet<FString> ToolCallIds;
	mutable FCriticalSection Mutex;
	mutable bool bConsumed = false;
};

bool BuildTextBlock(const FString &Text, TSharedPtr<FJsonValue> &OutValue)
{
	if (Text.IsEmpty())
	{
		return false;
	}
	TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("text"));
	Block->SetStringField(TEXT("text"), Text);
	OutValue = MakeShared<FJsonValueObject>(Block);
	return true;
}

bool BuildImageBlock(const FUnrealAIModelContentPart &Part, const FUnrealAIAnthropicMessagesBuildContext &Context,
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

	TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("type"), TEXT("base64"));
	Source->SetStringField(TEXT("media_type"), Part.MimeType);
	Source->SetStringField(TEXT("data"), FBase64::Encode(Part.InlineBytes));
	TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("image"));
	Block->SetObjectField(TEXT("source"), Source);
	OutValue = MakeShared<FJsonValueObject>(Block);
	return true;
}

bool BuildContentBlocks(const FUnrealAIModelMessage &Message, const FUnrealAIModelRequest &Request,
						const FUnrealAIAnthropicMessagesBuildContext &Context,
						TArray<TSharedPtr<FJsonValue>> &OutBlocks, FUnrealAIModelError &OutError)
{
	OutBlocks.Reset();
	for (const FUnrealAIModelContentPart &Part : Message.Content)
	{
		TSharedPtr<FJsonValue> Block;
		if (Part.Type == EUnrealAIContentType::Text)
		{
			if (!BuildTextBlock(Part.Text, Block))
			{
				OutError = MakeAnthropicError(
					EUnrealAIErrorCategory::InvalidArgument,
					TEXT("anthropic_empty_text_content"),
						 TEXT("Empty message content is not supported."),
							  TEXT("Anthropic Messages rejected an empty normalized text content part."));
				return false;
			}
		}
		else if (Part.Type == EUnrealAIContentType::StructuredJson)
		{
			if (!BuildTextBlock(Part.Json, Block))
			{
				OutError = MakeAnthropicError(
					EUnrealAIErrorCategory::InvalidArgument,
					TEXT("anthropic_empty_structured_content"),
						 TEXT("Empty message content is not supported."),
							  TEXT("Anthropic Messages rejected an empty normalized structured content part."));
				return false;
			}
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
			OutError = MakeAnthropicError(
				EUnrealAIErrorCategory::UnsupportedCapability,
				TEXT("anthropic_content_type_unsupported"),
					 TEXT("This content type is not supported by the Anthropic Messages adapter."),
						  TEXT("Anthropic Messages request rejected a normalized content type or assistant image."));
			return false;
		}
		OutBlocks.Add(MoveTemp(Block));
	}
	return !OutBlocks.IsEmpty();
}

bool ParseContinuationContent(const FString &Json, TArray<TSharedPtr<FJsonValue>> &OutContent)
{
	OutContent.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!ParseJsonObject(Json, FUnrealAIAnthropicMessagesWireRequest::MaxBodyBytes, Root))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>> *Content = nullptr;
	if (!Root->TryGetArrayField(TEXT("content"), Content) || Content == nullptr || Content->IsEmpty())
	{
		return false;
	}
	OutContent = *Content;
	return true;
}

bool BuildMessages(const FUnrealAIModelRequest &Request, const FUnrealAIAnthropicMessagesBuildContext &Context,
				   FString &OutSystem, TArray<TSharedPtr<FJsonValue>> &OutMessages,
				   TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> &OutContinuation,
				   FUnrealAIModelContinuationBinding &OutBinding, FUnrealAIModelError &OutError)
{
	OutSystem = Request.Instructions;
	OutMessages.Reset();
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
					OutError = MakeAnthropicError(
						EUnrealAIErrorCategory::UnsupportedCapability,
						TEXT("anthropic_system_content_unsupported"),
							 TEXT("This system content type is not supported by Anthropic."),
								  TEXT("Anthropic Messages accepts only normalized text/JSON system content."));
					return false;
				}
				if (!OutSystem.IsEmpty())
				{
					OutSystem += TEXT("\n\n");
				}
				OutSystem += Message.Role == EUnrealAIModelRole::Developer ? TEXT("[developer]\n") : TEXT("[system]\n");
				OutSystem += *Text;
			}
			continue;
		}

		FString Role;
		if (Message.Role == EUnrealAIModelRole::User)
		{
			Role = TEXT("user");
		}
		else if (Message.Role == EUnrealAIModelRole::Assistant)
		{
			Role = TEXT("assistant");
		}
		else
		{
			OutError =
				MakeAnthropicError(EUnrealAIErrorCategory::UnsupportedCapability,
								   TEXT("anthropic_message_role_unsupported"),
										TEXT("This message role is not supported by Anthropic."),
											 TEXT("Anthropic Messages rejected a provider-neutral message role."));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Blocks;
		if (!BuildContentBlocks(Message, Request, Context, Blocks, OutError))
		{
			return false;
		}
		TSharedRef<FJsonObject> WireMessage = MakeShared<FJsonObject>();
		WireMessage->SetStringField(TEXT("role"), Role);
		WireMessage->SetArrayField(TEXT("content"), MoveTemp(Blocks));
		OutMessages.Add(MakeShared<FJsonValueObject>(WireMessage));
	}

	if (OutMessages.IsEmpty())
	{
		OutError = MakeAnthropicError(EUnrealAIErrorCategory::InvalidArgument,
									  TEXT("anthropic_messages_empty"),
										   TEXT("Anthropic requires at least one conversational message."),
												TEXT("Anthropic Messages request contained only system instructions."));
		return false;
	}

	if (Request.ToolOutputs.IsEmpty())
	{
		if (Request.Continuation.IsValid())
		{
			OutError = MakeAnthropicError(
				EUnrealAIErrorCategory::InvalidArgument,
				TEXT("anthropic_continuation_without_outputs"),
					 TEXT("The Anthropic continuation requires tool results."),
						  TEXT("Anthropic Messages rejected an opaque continuation without normalized tool outputs."));
			return false;
		}
		return true;
	}

	FUnrealAIModelContinuationAccess Access;
	FString AccessError;
	if (!Request.Continuation.IsValid() ||
		!FUnrealAIModelContinuationAccess::TryAcquire(Request.Continuation.ToSharedRef(), AnthropicProviderName, Access,
													  AccessError) ||
		!Access.IsValid() || Access.GetImplementation() == nullptr ||
		Access.GetImplementation()->GetImplementationTypeToken() != FUnrealAIAnthropicMessagesContinuation::TypeToken())
	{
		OutError = MakeAnthropicError(
			EUnrealAIErrorCategory::InvalidArgument,
			TEXT("anthropic_continuation_invalid"),
				 TEXT("The Anthropic continuation is invalid."),
					  TEXT("Anthropic Messages could not acquire its exact opaque continuation implementation."));
		return false;
	}
	const auto *Continuation = static_cast<const FUnrealAIAnthropicMessagesContinuation *>(Access.GetImplementation());
	FString AssistantContentJson;
	TSet<FString> PendingIds;
	if (!Continuation->TryCopyForRequest(Request, AssistantContentJson, PendingIds, AccessError))
	{
		OutError =
			MakeAnthropicError(EUnrealAIErrorCategory::InvalidArgument,
							   TEXT("anthropic_continuation_binding_mismatch"),
									TEXT("The Anthropic continuation does not match this request."),
										 TEXT("Anthropic Messages rejected continuation binding or history drift."));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> AssistantContent;
	if (!ParseContinuationContent(AssistantContentJson, AssistantContent))
	{
		OutError = MakeAnthropicError(
			EUnrealAIErrorCategory::ProviderProtocol,
			TEXT("anthropic_continuation_payload_invalid"),
				 TEXT("The Anthropic continuation cannot be used."),
					  TEXT("Anthropic Messages continuation retained an invalid assistant content payload."));
		return false;
	}
	TSharedRef<FJsonObject> AssistantMessage = MakeShared<FJsonObject>();
	AssistantMessage->SetStringField(TEXT("role"), TEXT("assistant"));
	AssistantMessage->SetArrayField(TEXT("content"), MoveTemp(AssistantContent));
	OutMessages.Add(MakeShared<FJsonValueObject>(AssistantMessage));

	TArray<TSharedPtr<FJsonValue>> Results;
	TSet<FString> SuppliedIds;
	for (const FUnrealAIModelToolOutput &Output : Request.ToolOutputs)
	{
		if (!PendingIds.Contains(Output.ProviderCallId) || SuppliedIds.Contains(Output.ProviderCallId))
		{
			OutError = MakeAnthropicError(
				EUnrealAIErrorCategory::InvalidArgument,
				TEXT("anthropic_tool_output_mismatch"),
					 TEXT("The Anthropic tool results are invalid."),
						  TEXT("Anthropic Messages rejected secret, duplicate, or unrequested tool output."));
			return false;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("type"), TEXT("tool_result"));
		Result->SetStringField(TEXT("tool_use_id"), Output.ProviderCallId);
		Result->SetStringField(TEXT("content"), Output.OutputJson);
		Results.Add(MakeShared<FJsonValueObject>(Result));
		SuppliedIds.Add(Output.ProviderCallId);
	}
	if (SuppliedIds.Num() != PendingIds.Num())
	{
		OutError = MakeAnthropicError(
			EUnrealAIErrorCategory::InvalidArgument,
			TEXT("anthropic_tool_outputs_incomplete"),
				TEXT("All Anthropic tool results are required."),
					 TEXT("Anthropic Messages continuation requires exactly one result for every retained tool call."));
		return false;
	}
	TSharedRef<FJsonObject> ToolResultMessage = MakeShared<FJsonObject>();
	ToolResultMessage->SetStringField(TEXT("role"), TEXT("user"));
	ToolResultMessage->SetArrayField(TEXT("content"), MoveTemp(Results));
	OutMessages.Add(MakeShared<FJsonValueObject>(ToolResultMessage));
	OutContinuation = Request.Continuation;
	OutBinding = MakeBinding(Request);
	return true;
}
} // namespace

bool FUnrealAIAnthropicMessagesWireRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (BodyUtf8.IsEmpty() || BodyUtf8.Num() > MaxBodyBytes || !IsValidUtf8(BodyUtf8))
	{
		OutError = TEXT("Anthropic Messages wire body is empty, oversized, or invalid UTF-8.");
		return false;
	}
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR *>(BodyUtf8.GetData()), BodyUtf8.Num());
	TSharedPtr<FJsonObject> Root;
	if (Text.Length() <= 0 || !ParseJsonObject(FString(Text.Length(), Text.Get()), MaxBodyBytes, Root))
	{
		OutError = TEXT("Anthropic Messages wire body is not one bounded JSON object.");
		return false;
	}
	return true;
}

bool FUnrealAIAnthropicMessagesBuildContext::ValidateShape(const bool bRequiresImageResolver, FString &OutError) const
{
	OutError.Reset();
	FString DestinationError;
	if (!Destination.ValidateShape(DestinationError) || Destination.ModelProviderName != AnthropicProviderName ||
		Destination.AuthScheme != EUnrealAIAuthScheme::ApiKey ||
		Destination.BillingMode != EUnrealAIBillingMode::ApiMetered ||
		Destination.EndpointOrigin.ToString() != AnthropicOrigin || Destination.Audience != AnthropicAudience ||
		MaximumImageBytes < 1 || MaximumImageBytes > FUnrealAIAnthropicMessagesWireRequest::MaxResolvedImageBytes)
	{
		OutError = TEXT("Anthropic build context requires its exact API-key destination and bounded media resolver.");
		return false;
	}
	return true;
}

bool FUnrealAIAnthropicMessagesContinuationCommit::IsRequired() const
{
	return Continuation.IsValid();
}

bool FUnrealAIAnthropicMessagesContinuationCommit::TryCommit(FString &OutError)
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

void FUnrealAIAnthropicMessagesContinuationCommit::Reset()
{
	Continuation.Reset();
	Binding = {};
}

bool FUnrealAIAnthropicMessagesRequestBuilder::Build(const FUnrealAIModelRequest &Request,
													 const FUnrealAIAnthropicMessagesBuildContext &Context,
													 FUnrealAIAnthropicMessagesWireRequest &OutWireRequest,
													 FUnrealAIModelError &OutError)
{
	FUnrealAIAnthropicMessagesContinuationCommit Commit;
	if (!Stage(Request, Context, OutWireRequest, Commit, OutError))
	{
		return false;
	}
	FString CommitError;
	if (!Commit.TryCommit(CommitError))
	{
		OutWireRequest = {};
		OutError = MakeAnthropicError(
			EUnrealAIErrorCategory::InvalidArgument,
			TEXT("anthropic_continuation_commit_failed"),
				 TEXT("The Anthropic continuation could not be consumed."),
					  TEXT("Anthropic Messages continuation changed before its staged consume committed."));
		return false;
	}
	return true;
}

bool FUnrealAIAnthropicMessagesRequestBuilder::Stage(
	const FUnrealAIModelRequest &Request, const FUnrealAIAnthropicMessagesBuildContext &Context,
	FUnrealAIAnthropicMessagesWireRequest &OutWireRequest,
	FUnrealAIAnthropicMessagesContinuationCommit &OutContinuationCommit, FUnrealAIModelError &OutError)
{
	OutWireRequest = {};
	OutContinuationCommit.Reset();
	OutError = {};
	FString ShapeError;
	if (!Request.ValidateShape(ShapeError))
	{
		OutError = MakeAnthropicError(EUnrealAIErrorCategory::InvalidArgument,
									  TEXT("anthropic_request_invalid"),
										   TEXT("The Anthropic request is invalid."),
												TEXT("Anthropic Messages request failed normalized shape validation."));
		return false;
	}
	const bool bHasImages = IsImageRequest(Request);
	if (!Context.ValidateShape(bHasImages, ShapeError))
	{
		OutError = MakeAnthropicError(
			EUnrealAIErrorCategory::InvalidConfiguration,
			TEXT("anthropic_build_context_invalid"),
				 TEXT("The Anthropic connection is invalid."),
					  TEXT("Anthropic Messages request lowering rejected destination or media policy."));
		return false;
	}

	FString System;
	TArray<TSharedPtr<FJsonValue>> Messages;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> StagedContinuation;
	FUnrealAIModelContinuationBinding StagedBinding;
	if (!BuildMessages(Request, Context, System, Messages, StagedContinuation, StagedBinding, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Request.ModelId);
	Root->SetNumberField(TEXT("max_tokens"), Request.MaxOutputTokens);
	Root->SetBoolField(TEXT("stream"), true);
	if (!System.IsEmpty())
	{
		Root->SetStringField(TEXT("system"), System);
	}
	Root->SetArrayField(TEXT("messages"), MoveTemp(Messages));

	if (!Request.Tools.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		Tools.Reserve(Request.Tools.Num());
		for (const FUnrealAIModelToolDescriptor &Tool : Request.Tools)
		{
			TSharedPtr<FJsonObject> Schema;
			if (!ParseJsonObject(Tool.InputJsonSchema, FUnrealAIModelToolDescriptor::MaxSchemaUtf8Bytes, Schema))
			{
				OutError = MakeAnthropicError(
					EUnrealAIErrorCategory::SchemaValidation,
					TEXT("anthropic_tool_schema_invalid"),
						 TEXT("An Anthropic tool schema is invalid."),
							  TEXT("Anthropic Messages could not parse a normalized tool input schema."));
				return false;
			}
			TSharedRef<FJsonObject> WireTool = MakeShared<FJsonObject>();
			WireTool->SetStringField(TEXT("name"), Tool.InvocationName);
			WireTool->SetStringField(TEXT("description"), Tool.Description);
			WireTool->SetObjectField(TEXT("input_schema"), Schema.ToSharedRef());
			WireTool->SetBoolField(TEXT("strict"), Tool.bStrict);
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
			OutError = MakeAnthropicError(
				EUnrealAIErrorCategory::SchemaValidation,
				TEXT("anthropic_output_schema_invalid"),
					 TEXT("The Anthropic output schema is invalid."),
						  TEXT("Anthropic Messages could not parse the normalized output contract."));
			return false;
		}
		TSharedRef<FJsonObject> Format = MakeShared<FJsonObject>();
		Format->SetStringField(TEXT("type"), TEXT("json_schema"));
		Format->SetObjectField(TEXT("schema"), Schema.ToSharedRef());
		TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
		OutputConfig->SetObjectField(TEXT("format"), Format);
		Root->SetObjectField(TEXT("output_config"), OutputConfig);
	}

	FString Json;
	if (!SerializeJsonObject(Root, Json))
	{
		OutError = MakeAnthropicError(EUnrealAIErrorCategory::ProviderProtocol,
									  TEXT("anthropic_request_serialization_failed"),
										   TEXT("The Anthropic request could not be created."),
												TEXT("Anthropic Messages request JSON serialization failed."));
		return false;
	}
	FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() <= 0 || Utf8.Length() > FUnrealAIAnthropicMessagesWireRequest::MaxBodyBytes)
	{
		OutError =
			MakeAnthropicError(EUnrealAIErrorCategory::InvalidArgument,
							   TEXT("anthropic_request_too_large"),
									TEXT("The Anthropic request exceeds the supported size."),
										 TEXT("Anthropic Messages request body exceeded its framework wire bound."));
		return false;
	}
	OutWireRequest.BodyUtf8.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	if (!OutWireRequest.ValidateShape(ShapeError))
	{
		OutWireRequest = {};
		OutError = MakeAnthropicError(EUnrealAIErrorCategory::ProviderProtocol,
									  TEXT("anthropic_wire_request_invalid"),
										   TEXT("The Anthropic request could not be created."),
												TEXT("Anthropic Messages serialized an invalid bounded wire request."));
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

bool TryGetIndexField(const TSharedPtr<FJsonObject> &Object, int32 &OutIndex)
{
	int64 Value = 0;
	if (!TryGetIntegralField(Object, TEXT("index"), Value,
										  FUnrealAIAnthropicMessagesStreamDecoder::MaxContentBlocks - 1))
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

struct FAnthropicContentBlock final
{
	enum class EKind : uint8
	{
		Unknown,
		Text,
		ToolUse
	};

	EKind Kind = EKind::Unknown;
	FString Text;
	FString ToolCallId;
	FString InvocationName;
	FName StableName;
	int32 Version = 1;
	FString Arguments;
	int32 ToolOutputIndex = INDEX_NONE;
	bool bStopped = false;
};

bool SerializeRetainedAssistantContent(const TMap<int32, FAnthropicContentBlock> &Blocks, FString &OutJson,
									   TSet<FString> &OutToolIds)
{
	OutJson.Reset();
	OutToolIds.Reset();
	TArray<int32> Indices;
	Blocks.GenerateKeyArray(Indices);
	Indices.Sort();
	TArray<TSharedPtr<FJsonValue>> Content;
	for (const int32 Index : Indices)
	{
		const FAnthropicContentBlock *Block = Blocks.Find(Index);
		if (Block == nullptr || !Block->bStopped)
		{
			return false;
		}
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		if (Block->Kind == FAnthropicContentBlock::EKind::Unknown)
		{
			continue;
		}
		if (Block->Kind == FAnthropicContentBlock::EKind::Text)
		{
			if (Block->Text.IsEmpty())
			{
				continue;
			}
			Value->SetStringField(TEXT("type"), TEXT("text"));
			Value->SetStringField(TEXT("text"), Block->Text);
		}
		else
		{
			TSharedPtr<FJsonObject> Arguments;
			if (!ParseJsonObject(Block->Arguments, FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes, Arguments) ||
				OutToolIds.Contains(Block->ToolCallId))
			{
				return false;
			}
			Value->SetStringField(TEXT("type"), TEXT("tool_use"));
			Value->SetStringField(TEXT("id"), Block->ToolCallId);
			Value->SetStringField(TEXT("name"), Block->InvocationName);
			Value->SetObjectField(TEXT("input"), Arguments.ToSharedRef());
			OutToolIds.Add(Block->ToolCallId);
		}
		Content.Add(MakeShared<FJsonValueObject>(Value));
	}
	if (OutToolIds.IsEmpty())
	{
		return false;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("content"), MoveTemp(Content));
	return SerializeJsonObject(Root, OutJson) &&
		   FTCHARToUTF8(*OutJson).Length() <= FUnrealAIAnthropicMessagesWireRequest::MaxBodyBytes;
}
} // namespace

class FUnrealAIAnthropicMessagesStreamDecoder::FImpl final
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
		if (bTerminal)
		{
			OutError = TEXT("Anthropic Messages stream is already terminal.");
			return false;
		}
		if (Bytes.IsEmpty())
		{
			return true;
		}
		if (TotalStreamBytes > MaxStreamBytes - Bytes.Num() || Buffer.Num() > MaxStreamBytes - Bytes.Num())
		{
			return Fail(TEXT("Anthropic Messages stream exceeded its byte bound."), TEXT("anthropic_stream_too_large"),
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
					return Fail(TEXT("Anthropic Messages SSE event exceeded its byte bound."),
									 TEXT("anthropic_event_too_large"), OutError);
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
			if (bTerminal)
			{
				return true;
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
			return Fail(TEXT("Anthropic Messages stream ended with an incomplete SSE event."),
							 TEXT("anthropic_stream_truncated"), OutError);
		}
		return Fail(TEXT("Anthropic Messages stream ended without message_stop."), TEXT("anthropic_terminal_missing"),
																						OutError);
	}

	void Cancel()
	{
		if (bTerminal)
		{
			return;
		}
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::Cancelled);
		Event.Error = MakeAnthropicError(EUnrealAIErrorCategory::Cancelled,
										 TEXT("anthropic_request_cancelled"),
											  TEXT("The Anthropic request was cancelled."),
												   TEXT("Anthropic Messages stream cancelled by caller."));
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
			return Fail(TEXT("Anthropic Messages SSE event is invalid UTF-8."), TEXT("anthropic_event_utf8_invalid"),
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
		TSharedPtr<FJsonObject> Root;
		if (!ParseJsonObject(Data, MaxEventBytes, Root))
		{
			return Fail(TEXT("Anthropic Messages SSE data is malformed JSON."), TEXT("anthropic_event_json_invalid"),
																					 OutError);
		}
		FString Type;
		if (!Root->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty() ||
									 (!EventName.IsEmpty() && EventName != Type))
		{
			return Fail(TEXT("Anthropic Messages SSE event type is missing or inconsistent."),
							 TEXT("anthropic_event_type_invalid"), OutError);
		}
		return ProcessTypedEvent(Type, Root, OutError);
	}

	bool ProcessTypedEvent(const FString &Type, const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		if (Type == TEXT("ping"))
		{
			return true;
		}
		if (Type == TEXT("error"))
		{
			return ProcessProviderError(Root, OutError);
		}
		if (Type == TEXT("message_start"))
		{
			return ProcessMessageStart(Root, OutError);
		}
		if (Type == TEXT("content_block_start"))
		{
			return ProcessContentBlockStart(Root, OutError);
		}
		if (Type == TEXT("content_block_delta"))
		{
			return ProcessContentBlockDelta(Root, OutError);
		}
		if (Type == TEXT("content_block_stop"))
		{
			return ProcessContentBlockStop(Root, OutError);
		}
		if (Type == TEXT("message_delta"))
		{
			return ProcessMessageDelta(Root, OutError);
		}
		if (Type == TEXT("message_stop"))
		{
			return ProcessMessageStop(OutError);
		}
		if (IgnoredEventObserver)
		{
			IgnoredEventObserver();
		}
		return true;
	}

	bool ProcessMessageStart(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		if (bStarted)
		{
			return Fail(TEXT("Anthropic Messages emitted duplicate message_start."),
							 TEXT("anthropic_message_start_duplicate"), OutError);
		}
		const TSharedPtr<FJsonObject> *Message = nullptr;
		if (!Root->TryGetObjectField(TEXT("message"), Message) || Message == nullptr || !Message->IsValid() ||
									 !(*Message)->TryGetStringField(TEXT("id"), ResponseId) || ResponseId.IsEmpty())
		{
			return Fail(TEXT("Anthropic Messages message_start is malformed."), TEXT("anthropic_message_start_invalid"),
																					 OutError);
		}
		FUnrealAIModelProviderRequestId Id;
		Id.Kind = EUnrealAIModelProviderRequestIdKind::Response;
		Id.Value = ResponseId;
		FString IdError;
		if (!Id.ValidateShape(IdError))
		{
			return Fail(TEXT("Anthropic Messages response ID is invalid."), TEXT("anthropic_response_id_invalid"),
																				 OutError);
		}
		const TSharedPtr<FJsonObject> *Usage = nullptr;
		if (!(*Message)->TryGetObjectField(TEXT("usage"), Usage) || Usage == nullptr || !Usage->IsValid() ||
										   !TryGetIntegralField(*Usage, TEXT("input_tokens"), InputTokens))
		{
			return Fail(TEXT("Anthropic Messages input usage is malformed."), TEXT("anthropic_input_usage_invalid"),
																				   OutError);
		}
		int64 CacheRead = 0;
		int64 CacheCreation = 0;
		if ((*Usage)->HasField(TEXT("cache_read_input_tokens")) &&
							   !TryGetIntegralField(*Usage, TEXT("cache_read_input_tokens"), CacheRead))
		{
			return Fail(TEXT("Anthropic Messages cached input usage is malformed."),
							 TEXT("anthropic_cache_usage_invalid"), OutError);
		}
		if ((*Usage)->HasField(TEXT("cache_creation_input_tokens")) &&
							   !TryGetIntegralField(*Usage, TEXT("cache_creation_input_tokens"), CacheCreation))
		{
			return Fail(TEXT("Anthropic Messages cache creation usage is malformed."),
							 TEXT("anthropic_cache_usage_invalid"), OutError);
		}
		if (CacheRead > FUnrealAIModelUsageSnapshot::MaxTokenCount - CacheCreation)
		{
			return Fail(TEXT("Anthropic Messages cached usage overflowed."), TEXT("anthropic_cache_usage_invalid"),
																				  OutError);
		}
		CachedInputTokens = CacheRead + CacheCreation;
		bStarted = true;
		Emit(MakeEvent(EUnrealAIModelEventKind::Started));
		return true;
	}

	bool ProcessContentBlockStart(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		int32 Index = INDEX_NONE;
		const TSharedPtr<FJsonObject> *ContentBlock = nullptr;
		FString Type;
		if (!bStarted || !TryGetIndexField(Root, Index) || Blocks.Contains(Index) || Blocks.Num() >= MaxContentBlocks ||
			!Root->TryGetObjectField(TEXT("content_block"), ContentBlock) || ContentBlock == nullptr ||
									 !ContentBlock->IsValid() ||
									 !(*ContentBlock)->TryGetStringField(TEXT("type"), Type))
		{
			return Fail(TEXT("Anthropic Messages content_block_start is malformed or out of order."),
							 TEXT("anthropic_content_block_start_invalid"), OutError);
		}
		FAnthropicContentBlock Block;
		if (Type == TEXT("text"))
		{
			Block.Kind = FAnthropicContentBlock::EKind::Text;
			(*ContentBlock)->TryGetStringField(TEXT("text"), Block.Text);
		}
		else if (Type == TEXT("tool_use"))
		{
			Block.Kind = FAnthropicContentBlock::EKind::ToolUse;
			if (!(*ContentBlock)
					 ->TryGetStringField(TEXT("id"), Block.ToolCallId) || Block.ToolCallId.IsEmpty() ||
										 !(*ContentBlock)
											  ->TryGetStringField(TEXT("name"), Block.InvocationName) ||
																  Block.InvocationName.IsEmpty())
			{
				return Fail(TEXT("Anthropic Messages tool_use block identity is malformed."),
								 TEXT("anthropic_tool_start_invalid"), OutError);
			}
			const FUnrealAIModelToolDescriptor *Tool = FindToolByInvocation(Request, Block.InvocationName);
			if (Tool == nullptr || ToolCallIds.Contains(Block.ToolCallId))
			{
				return Fail(TEXT("Anthropic Messages requested an unknown or duplicate tool."),
								 TEXT("anthropic_tool_not_admitted"), OutError);
			}
			Block.StableName = Tool->StableName;
			Block.Version = Tool->Version;
			Block.ToolOutputIndex = NextToolOutputIndex++;
			const TSharedPtr<FJsonObject> *InitialInput = nullptr;
			if ((*ContentBlock)
					->TryGetObjectField(TEXT("input"), InitialInput) && InitialInput != nullptr &&
										InitialInput->IsValid() && (*InitialInput)->Values.Num() > 0 &&
										!SerializeJsonObject((*InitialInput).ToSharedRef(), Block.Arguments))
			{
				return Fail(TEXT("Anthropic Messages initial tool input is malformed."),
								 TEXT("anthropic_tool_input_invalid"), OutError);
			}
			ToolCallIds.Add(Block.ToolCallId);
			FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::ToolCallStarted);
			Event.ToolCallId = Block.ToolCallId;
			Event.ToolOutputIndex = Block.ToolOutputIndex;
			Event.ToolStableName = Block.StableName;
			Event.ToolVersion = Block.Version;
			Emit(MoveTemp(Event));
		}
		else
		{
			if (IgnoredEventObserver)
			{
				IgnoredEventObserver();
			}
			// Retain an inert block so later events for the same provider index
			// remain sequenced but cannot acquire text or tool authority.
		}
		Blocks.Add(Index, MoveTemp(Block));
		return true;
	}

	bool ProcessContentBlockDelta(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		int32 Index = INDEX_NONE;
		const TSharedPtr<FJsonObject> *Delta = nullptr;
		FString Type;
		if (!TryGetIndexField(Root, Index) || !Root->TryGetObjectField(TEXT("delta"), Delta) || Delta == nullptr ||
																	   !Delta->IsValid() ||
																	   !(*Delta)->TryGetStringField(TEXT("type"), Type))
		{
			return Fail(TEXT("Anthropic Messages content block delta is malformed."),
							 TEXT("anthropic_content_delta_invalid"), OutError);
		}
		FAnthropicContentBlock *Block = Blocks.Find(Index);
		if (Block == nullptr || Block->bStopped)
		{
			return Fail(TEXT("Anthropic Messages content block delta is out of order."),
							 TEXT("anthropic_content_delta_out_of_order"), OutError);
		}
		if (Block->Kind == FAnthropicContentBlock::EKind::Unknown)
		{
			if (IgnoredEventObserver)
			{
				IgnoredEventObserver();
			}
			return true;
		}
		if (Type == TEXT("text_delta") && Block->Kind == FAnthropicContentBlock::EKind::Text)
		{
			FString DeltaText;
			if (!(*Delta)->TryGetStringField(
					TEXT("text"), DeltaText) || DeltaText.IsEmpty() ||
					FTCHARToUTF8(*DeltaText).Length() > FUnrealAIModelEvent::MaxTextDeltaUtf8Bytes ||
					FTCHARToUTF8(*(Block->Text + DeltaText)).Length() > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes)
			{
				return Fail(TEXT("Anthropic Messages text delta is empty or oversized."),
								 TEXT("anthropic_text_delta_invalid"), OutError);
			}
			Block->Text += DeltaText;
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
		if (Type == TEXT("input_json_delta") && Block->Kind == FAnthropicContentBlock::EKind::ToolUse)
		{
			FString Partial;
			if (!(*Delta)->TryGetStringField(TEXT("partial_json"), Partial) || Partial.IsEmpty() ||
											 FTCHARToUTF8(*Partial).Length() >
												 FUnrealAIModelEvent::MaxToolArgumentsDeltaUtf8Bytes ||
											 FTCHARToUTF8(*(Block->Arguments + Partial)).Length() >
												 FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes)
			{
				return Fail(TEXT("Anthropic Messages tool JSON delta is empty or oversized."),
								 TEXT("anthropic_tool_delta_invalid"), OutError);
			}
			Block->Arguments += Partial;
			FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::ToolCallArgumentsDelta);
			Event.ToolCallId = Block->ToolCallId;
			Event.ToolOutputIndex = Block->ToolOutputIndex;
			Event.ToolArgumentsDelta = MoveTemp(Partial);
			Emit(MoveTemp(Event));
			return true;
		}
		if (Type == TEXT("thinking_delta") || Type == TEXT("signature_delta") || Type == TEXT("citations_delta"))
		{
			if (IgnoredEventObserver)
			{
				IgnoredEventObserver();
			}
			return true;
		}
		return Fail(TEXT("Anthropic Messages delta does not match its content block."),
						 TEXT("anthropic_content_delta_mismatch"), OutError);
	}

	bool ProcessContentBlockStop(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		int32 Index = INDEX_NONE;
		if (!TryGetIndexField(Root, Index))
		{
			return Fail(TEXT("Anthropic Messages content block stop is malformed."),
							 TEXT("anthropic_content_stop_invalid"), OutError);
		}
		FAnthropicContentBlock *Block = Blocks.Find(Index);
		if (Block == nullptr || Block->bStopped)
		{
			return Fail(TEXT("Anthropic Messages content block stop is duplicate or out of order."),
							 TEXT("anthropic_content_stop_out_of_order"), OutError);
		}
		Block->bStopped = true;
		if (Block->Kind == FAnthropicContentBlock::EKind::Unknown)
		{
			return true;
		}
		if (Block->Kind == FAnthropicContentBlock::EKind::Text)
		{
			if (Block->Text.IsEmpty())
			{
				return Fail(TEXT("Anthropic Messages completed an empty text block."), TEXT("anthropic_text_empty"),
																							OutError);
			}
			FUnrealAIModelEvent Event =
				MakeEvent(Request.OutputContract.IsSet() ? EUnrealAIModelEventKind::StructuredCompleted
														 : EUnrealAIModelEventKind::TextCompleted);
			if (Request.OutputContract.IsSet())
			{
				TSharedPtr<FJsonObject> Structured;
				if (!ParseJsonObject(Block->Text, FUnrealAIModelEvent::MaxStructuredJsonUtf8Bytes, Structured))
				{
					return Fail(TEXT("Anthropic Messages structured output is not a JSON object."),
									 TEXT("anthropic_structured_output_invalid"), OutError);
				}
				Event.StructuredJson = Block->Text;
			}
			else
			{
				Event.FinalText = Block->Text;
			}
			Emit(MoveTemp(Event));
			bHasUsableOutput = true;
			return true;
		}

		if (Block->Arguments.IsEmpty())
		{
			Block->Arguments = TEXT("{}");
		}
		TSharedPtr<FJsonObject> Arguments;
		if (!ParseJsonObject(Block->Arguments, FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes, Arguments))
		{
			return Fail(TEXT("Anthropic Messages completed malformed tool arguments."),
							 TEXT("anthropic_tool_arguments_invalid"), OutError);
		}
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::ToolCallCompleted);
		Event.ToolOutputIndex = Block->ToolOutputIndex;
		Event.ToolCall.ProviderCallId = Block->ToolCallId;
		Event.ToolCall.StableName = Block->StableName;
		Event.ToolCall.Version = Block->Version;
		Event.ToolCall.ArgumentsJson = Block->Arguments;
		Emit(MoveTemp(Event));
		bHasUsableOutput = true;
		return true;
	}

	bool ProcessMessageDelta(const TSharedPtr<FJsonObject> &Root, FString &OutError)
	{
		if (!bStarted || bUsageEmitted)
		{
			return Fail(TEXT("Anthropic Messages message_delta is duplicate or out of order."),
							 TEXT("anthropic_message_delta_invalid"), OutError);
		}
		const TSharedPtr<FJsonObject> *Delta = nullptr;
		const TSharedPtr<FJsonObject> *Usage = nullptr;
		if (!Root->TryGetObjectField(
				TEXT("delta"), Delta) || Delta == nullptr || !Delta->IsValid() ||
				!Root->TryGetObjectField(TEXT("usage"), Usage) || Usage == nullptr || !Usage->IsValid() ||
										 !TryGetIntegralField(*Usage, TEXT("output_tokens"), OutputTokens) ||
															  InputTokens > FUnrealAIModelUsageSnapshot::MaxTokenCount -
																				OutputTokens)
		{
			return Fail(TEXT("Anthropic Messages final usage is malformed."), TEXT("anthropic_output_usage_invalid"),
																				   OutError);
		}
		FString StopReason;
		if ((*Delta)->TryGetStringField(TEXT("stop_reason"), StopReason) && !StopReason.IsEmpty())
		{
			static const TSet<FString> KnownStopReasons = {
				TEXT("end_turn"), TEXT("max_tokens"), TEXT("stop_sequence"),
														   TEXT("tool_use"), TEXT("pause_turn"), TEXT("refusal")};
			FUnrealAIModelEvent Metadata = MakeEvent(EUnrealAIModelEventKind::ProviderMetadata);
			Metadata.Metadata.Key = EUnrealAIModelMetadataKey::FinishReason;
			Metadata.Metadata.Value = KnownStopReasons.Contains(StopReason) ? StopReason : TEXT("other");
			Emit(MoveTemp(Metadata));
		}
		FUnrealAIModelEvent UsageEvent = MakeEvent(EUnrealAIModelEventKind::UsageUpdated);
		UsageEvent.Usage.InputTokens = InputTokens;
		UsageEvent.Usage.CachedInputTokens = CachedInputTokens;
		UsageEvent.Usage.OutputTokens = OutputTokens;
		UsageEvent.Usage.TotalTokens = InputTokens + OutputTokens;
		UsageEvent.Usage.bFinal = true;
		Emit(MoveTemp(UsageEvent));
		bUsageEmitted = true;
		return true;
	}

	bool ProcessMessageStop(FString &OutError)
	{
		if (!bStarted || !bUsageEmitted || !bHasUsableOutput)
		{
			return Fail(TEXT("Anthropic Messages stopped before output and final usage completed."),
							 TEXT("anthropic_message_stop_incomplete"), OutError);
		}
		for (const TPair<int32, FAnthropicContentBlock> &Pair : Blocks)
		{
			if (!Pair.Value.bStopped)
			{
				return Fail(TEXT("Anthropic Messages stopped with an open content block."),
								 TEXT("anthropic_content_block_open"), OutError);
			}
		}
		FUnrealAIModelEvent Completed = MakeEvent(EUnrealAIModelEventKind::Completed);
		if (!ToolCallIds.IsEmpty())
		{
			FString AssistantContentJson;
			TSet<FString> RetainedToolIds;
			if (HistoryFingerprint.IsEmpty() ||
				!SerializeRetainedAssistantContent(Blocks, AssistantContentJson, RetainedToolIds) ||
				RetainedToolIds.Num() != ToolCallIds.Num())
			{
				return Fail(TEXT("Anthropic Messages could not retain its tool continuation."),
								 TEXT("anthropic_continuation_invalid"), OutError);
			}
			Completed.Continuation = MakeShared<FUnrealAIAnthropicMessagesContinuation, ESPMode::ThreadSafe>(
				MakeBinding(Request), HistoryFingerprint, MoveTemp(AssistantContentJson), MoveTemp(RetainedToolIds));
			if (!Completed.Continuation->IsValid())
			{
				return Fail(TEXT("Anthropic Messages produced an invalid tool continuation."),
								 TEXT("anthropic_continuation_invalid"), OutError);
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
		FString Type;
		if (!Root->TryGetObjectField(TEXT("error"), ErrorObject) || ErrorObject == nullptr || !ErrorObject->IsValid() ||
									 !(*ErrorObject)->TryGetStringField(TEXT("type"), Type))
		{
			return Fail(TEXT("Anthropic Messages error event is malformed."), TEXT("anthropic_error_event_invalid"),
																				   OutError);
		}
		EUnrealAIErrorCategory Category = EUnrealAIErrorCategory::Provider;
		FName Code(TEXT("anthropic_provider_error"));
		bool bRetryable = false;
		if (Type == TEXT("rate_limit_error"))
		{
			Category = EUnrealAIErrorCategory::RateLimited;
			Code = TEXT("anthropic_rate_limited");
			bRetryable = true;
		}
		else if (Type == TEXT("overloaded_error"))
		{
			Code = TEXT("anthropic_overloaded");
			bRetryable = true;
		}
		else if (Type == TEXT("authentication_error"))
		{
			Category = EUnrealAIErrorCategory::NotAuthorized;
			Code = TEXT("anthropic_not_authorized");
		}
		else if (Type == TEXT("permission_error"))
		{
			Category = EUnrealAIErrorCategory::PolicyDenied;
			Code = TEXT("anthropic_permission_denied");
		}
		else if (Type == TEXT("invalid_request_error"))
		{
			Category = EUnrealAIErrorCategory::InvalidArgument;
			Code = TEXT("anthropic_invalid_request");
		}
		if (!bStarted)
		{
			bStarted = true;
			Emit(MakeEvent(EUnrealAIModelEventKind::Started));
		}
		FUnrealAIModelEvent Event = MakeEvent(EUnrealAIModelEventKind::Failed);
		Event.Error =
			MakeAnthropicError(Category, Code,
							   TEXT("Anthropic could not complete the request."),
									TEXT("Anthropic Messages emitted a terminal provider error."), bRetryable);
		bTerminal = true;
		bFailed = true;
		Emit(MoveTemp(Event));
		OutError = TEXT("Anthropic Messages emitted a terminal provider error.");
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
		Event.Error = MakeAnthropicError(EUnrealAIErrorCategory::ProviderProtocol, Code,
										 TEXT("The Anthropic response was invalid."), Diagnostic);
		bTerminal = true;
		bFailed = true;
		Emit(MoveTemp(Event));
		return false;
	}

	FUnrealAIModelRequest Request;
	FEventSink Sink;
	FIgnoredEventObserver IgnoredEventObserver;
	TArray<uint8> Buffer;
	TMap<int32, FAnthropicContentBlock> Blocks;
	TSet<FString> ToolCallIds;
	FString HistoryFingerprint;
	FString ResponseId;
	int32 Head = 0;
	int32 TotalStreamBytes = 0;
	int32 NextToolOutputIndex = 0;
	int64 Sequence = 0;
	int64 InputTokens = 0;
	int64 CachedInputTokens = 0;
	int64 OutputTokens = 0;
	bool bStarted = false;
	bool bHasUsableOutput = false;
	bool bUsageEmitted = false;
	bool bTerminal = false;
	bool bCompleted = false;
	bool bFailed = false;
};

FUnrealAIAnthropicMessagesStreamDecoder::FUnrealAIAnthropicMessagesStreamDecoder(
	const FUnrealAIModelRequest &Request, FEventSink InSink, FIgnoredEventObserver InIgnoredEventObserver)
	: Impl(MakeUnique<FImpl>(Request, MoveTemp(InSink), MoveTemp(InIgnoredEventObserver)))
{
}

FUnrealAIAnthropicMessagesStreamDecoder::~FUnrealAIAnthropicMessagesStreamDecoder() = default;

bool FUnrealAIAnthropicMessagesStreamDecoder::PushBytes(const TConstArrayView<uint8> Bytes, FString &OutError)
{
	return Impl.IsValid() && Impl->PushBytes(Bytes, OutError);
}

bool FUnrealAIAnthropicMessagesStreamDecoder::Finish(FString &OutError)
{
	return Impl.IsValid() && Impl->Finish(OutError);
}

void FUnrealAIAnthropicMessagesStreamDecoder::Cancel()
{
	if (Impl.IsValid())
	{
		Impl->Cancel();
	}
}

bool FUnrealAIAnthropicMessagesStreamDecoder::IsTerminal() const
{
	return Impl.IsValid() && Impl->IsTerminal();
}
