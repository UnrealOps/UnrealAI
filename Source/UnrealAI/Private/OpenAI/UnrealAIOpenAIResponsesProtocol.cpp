// Copyright EngineWorks. All Rights Reserved.

#include "OpenAI/UnrealAIOpenAIResponsesProtocol.h"

#include "HAL/CriticalSection.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/UnrealAIJsonValidation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
constexpr int32 MaxContinuationBytes = 2 * 1024 * 1024;
constexpr int32 MaxContinuationHistoryBytes = FUnrealAIOpenAIResponsesWireRequest::MaxBodyBytes;
constexpr double MaxExactJsonInteger = 9007199254740991.0;
const FName OpenAIResponsesProtocolProviderName(TEXT("openai.responses"));
constexpr TCHAR OpenAIErrorCodePrefix[] = TEXT("openai_");

bool IsBoundedUtf8(const FString &Value, const int32 MaximumBytes, const bool bAllowEmpty = true)
{
	FTCHARToUTF8 Utf8(*Value);
	return (bAllowEmpty || !Value.IsEmpty()) && Utf8.Length() >= 0 && Utf8.Length() <= MaximumBytes;
}

void SecureZero(TArray<uint8> &Bytes)
{
	volatile uint8 *Data = Bytes.GetData();
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		Data[Index] = 0;
	}
	Bytes.Empty();
}

FUnrealAIModelError MakeOpenAIError(const EUnrealAIErrorCategory Category, const FName Code, const TCHAR *UserMessage,
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
	FString PreflightError;
	if (!PreflightUnrealAIJson(Json, Limits, PreflightError))
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

bool ParseJsonObjectUtf8(const TConstArrayView<uint8> Bytes, const int32 MaximumBytes,
						 TSharedPtr<FJsonObject> &OutObject)
{
	if (Bytes.IsEmpty() || Bytes.Num() > MaximumBytes || !IsValidUtf8(Bytes))
	{
		return false;
	}
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
	if (Text.Length() <= 0)
	{
		return false;
	}
	return ParseJsonObject(FString(Text.Length(), Text.Get()), MaximumBytes, OutObject);
}

FString RoleToOpenAI(const EUnrealAIModelRole Role)
{
	switch (Role)
	{
	case EUnrealAIModelRole::System:
		return TEXT("system");
	case EUnrealAIModelRole::Developer:
		return TEXT("developer");
	case EUnrealAIModelRole::User:
		return TEXT("user");
	case EUnrealAIModelRole::Assistant:
		return TEXT("assistant");
	default:
		return FString();
	}
}

bool TryGetMessagePartText(const FUnrealAIModelContentPart &Part, FString &OutPartText, const TCHAR *ErrorContext,
						   FUnrealAIModelError &OutError)
{
	OutPartText.Reset();
	if (Part.Type == EUnrealAIContentType::Text)
	{
		OutPartText = Part.Text;
		return true;
	}
	if (Part.Type == EUnrealAIContentType::StructuredJson)
	{
		OutPartText = Part.Json;
		return true;
	}
	OutError =
		MakeOpenAIError(EUnrealAIErrorCategory::UnsupportedCapability,
						TEXT("openai_content_type_unsupported"),
							 TEXT("This content type is not supported by the OpenAI Responses adapter."), ErrorContext);
	return false;
}

bool BuildInlineImagePart(const FUnrealAIModelContentPart &Part, TSharedPtr<FJsonValue> &OutValue,
						  FUnrealAIModelError &OutError)
{
	OutValue.Reset();
	if (Part.Type != EUnrealAIContentType::Image || Part.InlineBytes.IsEmpty() ||
		Part.InlineBytes.Num() > FUnrealAIModelContentPart::MaxInlineBytes || !Part.Text.IsEmpty() ||
		!Part.Json.IsEmpty() || (Part.MimeType != TEXT("image/jpeg") && Part.MimeType != TEXT("image/png")))
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::Vision,
								   TEXT("openai_image_inline_invalid"),
										TEXT("The image is not valid for the OpenAI Responses adapter."),
											 TEXT("The shared public Responses contract requires one bounded inline JPEG or PNG with matching encoded bytes and MIME type."));
		return false;
	}
	FString ImageError;
	if (!Part.Validate(ImageError))
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::Vision,
								   TEXT("openai_image_inline_invalid"),
										TEXT("The inline image is invalid."),
											 TEXT("Inline image byte/MIME validation failed."));
		return false;
	}
	const FString DataUrl =
		FString::Printf(TEXT("data:%s;base64,%s"), *Part.MimeType, *FBase64::Encode(Part.InlineBytes));
	if (!IsBoundedUtf8(DataUrl, FUnrealAIOpenAIResponsesWireRequest::MaxBodyBytes, false))
	{
		OutError =
			MakeOpenAIError(EUnrealAIErrorCategory::InvalidArgument,
							TEXT("openai_image_too_large"),
								 TEXT("The image exceeds the supported request size."),
									  TEXT("OpenAI Responses inline image data URL exceeded its wire-body bound."));
		return false;
	}
	TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
	Image->SetStringField(TEXT("type"), TEXT("input_image"));
	Image->SetStringField(TEXT("image_url"), DataUrl);
	OutValue = MakeShared<FJsonValueObject>(Image);
	return true;
}

bool BuildMessage(const FUnrealAIModelMessage &Message, TSharedPtr<FJsonValue> &OutValue, FUnrealAIModelError &OutError)
{
	const FString Role = RoleToOpenAI(Message.Role);
	if (Role.IsEmpty())
	{
		OutError =
			MakeOpenAIError(EUnrealAIErrorCategory::UnsupportedCapability,
							TEXT("openai_message_role_unsupported"),
								 TEXT("This message role is not supported by the OpenAI Responses adapter."),
									  TEXT("OpenAI Responses request rejected a provider-neutral message role."));
		return false;
	}

	FString Content;
	const bool bContainsImage = Message.Content.ContainsByPredicate(
		[](const FUnrealAIModelContentPart &Part) { return Part.Type == EUnrealAIContentType::Image; });
	if (bContainsImage)
	{
		if (Message.Role != EUnrealAIModelRole::User)
		{
			OutError =
				MakeOpenAIError(EUnrealAIErrorCategory::UnsupportedCapability,
								TEXT("openai_image_role_unsupported"),
									 TEXT("Images are supported only in user input messages."),
										  TEXT("OpenAI Responses rejected an image attached to a non-user role."));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Parts;
		Parts.Reserve(Message.Content.Num());
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			TSharedPtr<FJsonValue> Value;
			if (Part.Type == EUnrealAIContentType::Image)
			{
				if (!BuildInlineImagePart(Part, Value, OutError))
				{
					return false;
				}
			}
			else
			{
				FString PartText;
				if (!TryGetMessagePartText(Part, PartText,
										   TEXT("OpenAI Responses rejected mixed image message content."), OutError))
				{
					return false;
				}
				TSharedRef<FJsonObject> TextPart = MakeShared<FJsonObject>();
				TextPart->SetStringField(TEXT("type"), TEXT("input_text"));
				TextPart->SetStringField(TEXT("text"), PartText);
				Value = MakeShared<FJsonValueObject>(TextPart);
			}
			Parts.Add(MoveTemp(Value));
		}
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("role"), Role);
		Object->SetArrayField(TEXT("content"), Parts);
		OutValue = MakeShared<FJsonValueObject>(Object);
		return true;
	}
	else
	{
		for (const FUnrealAIModelContentPart &Part : Message.Content)
		{
			FString PartText;
			if (!TryGetMessagePartText(
					Part, PartText, TEXT("OpenAI Responses request rejected an unsupported content part."), OutError))
			{
				return false;
			}
			if (!Content.IsEmpty())
			{
				Content.AppendChar(TEXT('\n'));
			}
			Content += PartText;
		}
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("role"), Role);
	Object->SetStringField(TEXT("content"), Content);
	OutValue = MakeShared<FJsonValueObject>(Object);
	return true;
}

bool BuildCanonicalRequestHistory(const FUnrealAIModelRequest &Request, TArray<uint8> &OutHistory,
								  FUnrealAIModelError &OutError)
{
	SecureZero(OutHistory);
	TArray<TSharedPtr<FJsonValue>> Input;
	Input.Reserve(Request.InputMessages.Num());
	for (const FUnrealAIModelMessage &Message : Request.InputMessages)
	{
		const FUnrealAIModelMessage *HistoryMessage = &Message;
		TSharedPtr<FJsonValue> Value;
		if (!BuildMessage(*HistoryMessage, Value, OutError))
		{
			return false;
		}
		Input.Add(MoveTemp(Value));
	}

	TSharedRef<FJsonObject> History = MakeShared<FJsonObject>();
	History->SetArrayField(TEXT("input"), Input);
	if (!Request.Instructions.IsEmpty())
	{
		History->SetStringField(TEXT("instructions"), Request.Instructions);
	}
	if (Request.OutputContract.IsSet())
	{
		TSharedPtr<FJsonObject> Schema;
		if (!ParseJsonObject(Request.OutputContract->JsonSchema, FUnrealAIModelOutputContract::MaxSchemaUtf8Bytes,
							 Schema))
		{
			OutError = MakeOpenAIError(EUnrealAIErrorCategory::SchemaValidation,
									   TEXT("openai_output_schema_invalid"),
											TEXT("The requested output schema is invalid."),
												 TEXT("OpenAI Responses could not parse a validated output schema while binding continuation history."));
			return false;
		}
		TSharedRef<FJsonObject> Format = MakeShared<FJsonObject>();
		Format->SetStringField(TEXT("type"), TEXT("json_schema"));
		Format->SetStringField(TEXT("name"), Request.OutputContract->Name);
		Format->SetObjectField(TEXT("schema"), Schema);
		Format->SetBoolField(TEXT("strict"), Request.OutputContract->bStrict);
		History->SetObjectField(TEXT("output_format"), Format);
	}
	FString Json;
	if (!SerializeJsonObject(History, Json))
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::ProviderProtocol,
								   TEXT("openai_continuation_history_serialization_failed"),
										TEXT("The model continuation history could not be created."),
											 TEXT("OpenAI Responses continuation history JSON serialization failed."));
		return false;
	}
	FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() <= 0 || Utf8.Length() > MaxContinuationHistoryBytes)
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("openai_continuation_history_too_large"),
										TEXT("The model continuation history exceeds the supported size."),
											 TEXT("OpenAI Responses continuation history exceeded its byte bound."));
		return false;
	}
	OutHistory.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	return true;
}

class FUnrealAIOpenAIContinuation final : public IUnrealAIModelContinuation
{
  public:
	static const void *TypeToken()
	{
		static uint8 Token;
		return &Token;
	}

	static TSharedPtr<const FUnrealAIOpenAIContinuation, ESPMode::ThreadSafe>
	Create(const FUnrealAIModelContinuationBinding &InBinding, const TArray<TSharedPtr<FJsonValue>> &OutputItems,
		   TArray<uint8> &&InOriginalHistory)
	{
		FString Error;
		if (!InBinding.ValidateShape(Error) || OutputItems.IsEmpty() || InOriginalHistory.IsEmpty() ||
			InOriginalHistory.Num() > MaxContinuationHistoryBytes)
		{
			return nullptr;
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("output"), OutputItems);
		FString Json;
		if (!SerializeJsonObject(Root, Json))
		{
			return nullptr;
		}
		FTCHARToUTF8 Utf8(*Json);
		if (Utf8.Length() <= 0 || Utf8.Length() > MaxContinuationBytes)
		{
			return nullptr;
		}
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
		return MakeShared<FUnrealAIOpenAIContinuation, ESPMode::ThreadSafe>(InBinding, MoveTemp(Bytes),
																			MoveTemp(InOriginalHistory));
	}

	FUnrealAIOpenAIContinuation(const FUnrealAIModelContinuationBinding &InBinding, TArray<uint8> &&InOutputJson,
								TArray<uint8> &&InOriginalHistory)
		: Binding(InBinding), OutputJson(MoveTemp(InOutputJson)), OriginalHistory(MoveTemp(InOriginalHistory))
	{
	}

	~FUnrealAIOpenAIContinuation() override
	{
		FScopeLock Lock(&Mutex);
		SecureZero(OutputJson);
		SecureZero(OriginalHistory);
	}

	FName GetProviderName() const override
	{
		return Binding.ProviderName;
	}

	bool IsValid() const override
	{
		FScopeLock Lock(&Mutex);
		return !bConsumed && !OutputJson.IsEmpty() && !OriginalHistory.IsEmpty();
	}

	bool MatchesOriginalHistory(const FUnrealAIModelRequest &Request, FString &OutError) const
	{
		OutError.Reset();
		FUnrealAIModelError HistoryError;
		TArray<uint8> ExpectedHistory;
		if (!BuildCanonicalRequestHistory(Request, ExpectedHistory, HistoryError))
		{
			OutError = TEXT("Expected model continuation history is invalid or exceeds framework bounds.");
			return false;
		}

		bool bMatches = false;
		{
			FScopeLock Lock(&Mutex);
			bMatches =
				!bConsumed && !OutputJson.IsEmpty() && ExpectedHistory.Num() == OriginalHistory.Num() &&
				(ExpectedHistory.IsEmpty() ||
				 FMemory::Memcmp(ExpectedHistory.GetData(), OriginalHistory.GetData(), ExpectedHistory.Num()) == 0);
		}
		SecureZero(ExpectedHistory);
		if (!bMatches)
		{
			OutError = TEXT("Model continuation requires the exact original instructions and input messages.");
		}
		return bMatches;
	}

	bool TryConsume(const FUnrealAIModelContinuationBinding &ExpectedBinding, FString &OutError) const override
	{
		OutError.Reset();
		FString ShapeError;
		if (!ExpectedBinding.ValidateShape(ShapeError))
		{
			OutError = TEXT("Expected model continuation binding is invalid.");
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (bConsumed || OutputJson.IsEmpty() || OriginalHistory.IsEmpty())
		{
			OutError = TEXT("Model continuation has already been consumed.");
			return false;
		}
		if (ExpectedBinding.ProviderName != Binding.ProviderName ||
			ExpectedBinding.ConnectionAlias != Binding.ConnectionAlias || ExpectedBinding.ModelId != Binding.ModelId ||
			ExpectedBinding.ConnectionBinding != Binding.ConnectionBinding)
		{
			OutError =
				TEXT("Model continuation does not match this provider, connection, model, agent, run, or world.");
			return false;
		}
		bConsumed = true;
		SecureZero(OutputJson);
		SecureZero(OriginalHistory);
		return true;
	}

	const void *GetImplementationTypeToken() const override
	{
		return TypeToken();
	}

	FString GetRedactedDisplay() const override
	{
		return IsValid() ? TEXT("ModelContinuation(Responses, ready)") : TEXT("ModelContinuation(Responses, consumed)");
	}

	bool CopyOutputItems(TArray<TSharedPtr<FJsonValue>> &OutItems) const
	{
		OutItems.Reset();
		TArray<uint8> Bytes;
		{
			FScopeLock Lock(&Mutex);
			if (bConsumed || OutputJson.IsEmpty() || OriginalHistory.IsEmpty())
			{
				return false;
			}
			Bytes = OutputJson;
		}

		FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
		TSharedPtr<FJsonObject> Root;
		const bool bParsed =
			Text.Length() > 0 && ParseJsonObject(FString(Text.Length(), Text.Get()), MaxContinuationBytes, Root);
		SecureZero(Bytes);
		const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
		if (!bParsed || !Root->TryGetArrayField(TEXT("output"), Items) || Items == nullptr || Items->IsEmpty())
		{
			return false;
		}
		OutItems = *Items;
		return true;
	}

  private:
	FUnrealAIModelContinuationBinding Binding;
	mutable FCriticalSection Mutex;
	mutable TArray<uint8> OutputJson;
	mutable TArray<uint8> OriginalHistory;
	mutable bool bConsumed = false;
};

FUnrealAIModelContinuationBinding MakeContinuationBinding(const FName ProviderName,
														  const FUnrealAIModelRequest &Request)
{
	FUnrealAIModelContinuationBinding Binding;
	Binding.ProviderName = ProviderName;
	Binding.ConnectionAlias = Request.ConnectionAlias;
	Binding.ModelId = Request.ModelId;
	Binding.ConnectionBinding = Request.ConnectionBinding;
	return Binding;
}

bool AppendContinuation(const FName ProviderName, const FUnrealAIModelRequest &Request,
						TArray<TSharedPtr<FJsonValue>> &Input,
						TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> &OutContinuation,
						FUnrealAIModelContinuationBinding &OutBinding, FUnrealAIModelError &OutError)
{
	OutContinuation.Reset();
	OutBinding = {};
	if (!Request.Continuation.IsValid())
	{
		return true;
	}

	FUnrealAIModelContinuationAccess ContinuationAccess;
	FString AccessError;
	if (!FUnrealAIModelContinuationAccess::TryAcquire(Request.Continuation.ToSharedRef(), ProviderName,
													  ContinuationAccess, AccessError))
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("openai_continuation_provider_mismatch"),
										TEXT("The model continuation cannot be used by this provider."),
											 TEXT("OpenAI Responses rejected a foreign continuation implementation."));
		return false;
	}

	const IUnrealAIModelContinuation *Implementation = ContinuationAccess.GetImplementation();
	if (Implementation == nullptr ||
		Implementation->GetImplementationTypeToken() != FUnrealAIOpenAIContinuation::TypeToken())
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("openai_continuation_provider_mismatch"),
										TEXT("The model continuation cannot be used by this provider."),
											 TEXT("OpenAI Responses rejected a foreign continuation implementation."));
		return false;
	}

	const FUnrealAIOpenAIContinuation *Continuation = static_cast<const FUnrealAIOpenAIContinuation *>(Implementation);
	FString HistoryError;
	if (!Continuation->MatchesOriginalHistory(Request, HistoryError))
	{
		OutError =
			MakeOpenAIError(EUnrealAIErrorCategory::ResourceConflict,
							TEXT("openai_continuation_history_mismatch"),
								 TEXT("The model continuation requires the exact original conversation history."),
									  TEXT("OpenAI Responses rejected continuation instructions or input messages that were missing or changed."));
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> PriorOutput;
	if (!Continuation->CopyOutputItems(PriorOutput))
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::ProviderProtocol,
								   TEXT("openai_continuation_corrupt"),
										TEXT("The model continuation could not be restored."),
											 TEXT("OpenAI Responses continuation payload was missing or malformed."));
		return false;
	}
	Input.Append(PriorOutput);
	OutContinuation = Request.Continuation;
	OutBinding = MakeContinuationBinding(ProviderName, Request);
	return true;
}

bool ReadNonNegativeInteger(const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, int64 &OutValue,
							const int64 Maximum = FUnrealAIModelUsageSnapshot::MaxTokenCount)
{
	double Number = 0.0;
	if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number) || Number < 0.0 ||
		Number > static_cast<double>(Maximum) || FMath::FloorToDouble(Number) != Number)
	{
		return false;
	}
	OutValue = static_cast<int64>(Number);
	return true;
}

bool ReadBoundedIndex(const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, int32 &OutValue)
{
	double Number = -1.0;
	if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number) || Number < 0.0 ||
		Number > 4096.0 || FMath::FloorToDouble(Number) != Number)
	{
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

int64 Utf8Length(const FString &Value)
{
	return static_cast<int64>(FTCHARToUTF8_Convert::ConvertedLength(*Value, Value.Len()));
}

bool IsProviderOpaqueToken(const FString &Value)
{
	FUnrealAIModelProviderRequestId Probe;
	Probe.Kind = EUnrealAIModelProviderRequestIdKind::Response;
	Probe.Value = Value;
	FString ShapeError;
	return Probe.ValidateShape(ShapeError);
}

} // namespace

bool FUnrealAIOpenAIResponsesPublicFaultPolicy::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const FString Prefix = CodePrefix.ToString();
	const FTCHARToUTF8 PrefixUtf8(*Prefix);
	const FTCHARToUTF8 DisplayNameUtf8(*ProviderDisplayName);
	if (CodePrefix.IsNone() || PrefixUtf8.Length() <= 0 || PrefixUtf8.Length() > MaxCodePrefixUtf8Bytes ||
		ProviderDisplayName.IsEmpty() || DisplayNameUtf8.Length() <= 0 ||
		DisplayNameUtf8.Length() > MaxProviderDisplayNameUtf8Bytes ||
		ProviderDisplayName.Contains(TEXT("\r")) || ProviderDisplayName.Contains(TEXT("\n")))
	{
		OutError = TEXT("Responses public fault policy has an invalid code prefix or provider display name.");
		return false;
	}
	for (const TCHAR Character : Prefix)
	{
		if (!((Character >= TEXT('a') && Character <= TEXT('z')) ||
			   (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_')))
		{
			OutError = TEXT("Responses public fault code prefix must contain only lowercase ASCII letters, digits, or underscores.");
			return false;
		}
	}
	return true;
}

void FUnrealAIOpenAIResponsesPublicFaultPolicy::ApplyTo(FUnrealAIModelError &InOutError) const
{
	if (!InOutError.IsError())
	{
		return;
	}
	const FString ExistingCode = InOutError.Code.ToString();
	if (ExistingCode.StartsWith(OpenAIErrorCodePrefix, ESearchCase::IgnoreCase))
	{
		InOutError.Code = FName(CodePrefix.ToString() +
								TEXT("_") + ExistingCode.RightChop(UE_ARRAY_COUNT(OpenAIErrorCodePrefix) - 1));
	}
	const auto Reattribute = [this](const FString &Value)
	{ return Value.Replace(TEXT("OpenAI"), *ProviderDisplayName, ESearchCase::IgnoreCase); };
	InOutError.UserMessage = FText::FromString(Reattribute(InOutError.UserMessage.ToString()));
	InOutError.DiagnosticMessage = Reattribute(InOutError.DiagnosticMessage);
}

FUnrealAIOpenAIResponsesPublicFaultPolicy FUnrealAIOpenAIResponsesPublicFaultPolicy::OpenAI()
{
	return {};
}

bool FUnrealAIOpenAIResponsesWireRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (BodyUtf8.IsEmpty() || BodyUtf8.Num() > MaxBodyBytes)
	{
		OutError = TEXT("Responses request body is empty or exceeds the transport bound.");
		return false;
	}
	TSharedPtr<FJsonObject> Body;
	if (!ParseJsonObjectUtf8(BodyUtf8, MaxBodyBytes, Body))
	{
		OutError = TEXT("Responses request body is not a JSON object.");
		return false;
	}
	return true;
}

bool FUnrealAIOpenAIResponsesRequestBuilder::Build(const FUnrealAIModelRequest &Request,
												   FUnrealAIOpenAIResponsesWireRequest &OutWireRequest,
												   FUnrealAIModelError &OutError)
{
	return BuildForProvider(OpenAIResponsesProtocolProviderName, FUnrealAIOpenAIResponsesPublicFaultPolicy::OpenAI(),
							Request, OutWireRequest, OutError);
}

bool FUnrealAIOpenAIResponsesContinuationCommit::IsRequired() const
{
	return Continuation.IsValid();
}

bool FUnrealAIOpenAIResponsesContinuationCommit::TryCommit(FString &OutError)
{
	OutError.Reset();
	if (!Continuation.IsValid())
	{
		return true;
	}
	const TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> ContinuationToConsume =
		MoveTemp(Continuation);
	if (!ContinuationToConsume->TryConsume(Binding, OutError))
	{
		Continuation = ContinuationToConsume;
		return false;
	}
	Binding = {};
	return true;
}

void FUnrealAIOpenAIResponsesContinuationCommit::Reset()
{
	Continuation.Reset();
	Binding = {};
}

bool FUnrealAIOpenAIResponsesRequestBuilder::BuildForProvider(
	const FName ProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
	const FUnrealAIModelRequest &Request, FUnrealAIOpenAIResponsesWireRequest &OutWireRequest,
	FUnrealAIModelError &OutError)
{
	FUnrealAIOpenAIResponsesContinuationCommit ContinuationCommit;
	if (!StageForProvider(ProviderName, FaultPolicy, Request, OutWireRequest, ContinuationCommit, OutError))
	{
		return false;
	}
	FString CommitError;
	if (!ContinuationCommit.TryCommit(CommitError))
	{
		OutWireRequest.BodyUtf8.Reset();
		OutError = MakeOpenAIError(
			EUnrealAIErrorCategory::ResourceConflict,
			TEXT("openai_continuation_stale"),
				 TEXT("The model continuation is stale or has already been used."),
					  TEXT("OpenAI Responses rejected a stale, replayed, or incorrectly bound continuation."));
		FaultPolicy.ApplyTo(OutError);
		return false;
	}
	return true;
}

bool FUnrealAIOpenAIResponsesRequestBuilder::StageForProvider(
	const FName ProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
	const FUnrealAIModelRequest &Request, FUnrealAIOpenAIResponsesWireRequest &OutWireRequest,
	FUnrealAIOpenAIResponsesContinuationCommit &OutContinuationCommit, FUnrealAIModelError &OutError)
{
	OutWireRequest.BodyUtf8.Reset();
	OutContinuationCommit.Reset();
	OutError = {};
	ON_SCOPE_EXIT
	{
		FaultPolicy.ApplyTo(OutError);
	};
	FString ShapeError;
	FUnrealAIModelContinuationBinding ProviderShape;
	ProviderShape.ProviderName = ProviderName;
	ProviderShape.ConnectionAlias = Request.ConnectionAlias;
	ProviderShape.ModelId = Request.ModelId;
	if (!FaultPolicy.ValidateShape(ShapeError) || !Request.ValidateShape(ShapeError) ||
		!ProviderShape.ValidateShape(ShapeError))
	{
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("openai_request_invalid"),
										TEXT("The model request is invalid."),
											 TEXT("OpenAI Responses request failed provider-neutral validation."));
		return false;
	}
	for (const FUnrealAIModelToolOutput &Output : Request.ToolOutputs)
	{
	}

	TArray<TSharedPtr<FJsonValue>> Input;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> StagedContinuation;
	FUnrealAIModelContinuationBinding StagedBinding;
	for (const FUnrealAIModelMessage &Message : Request.InputMessages)
	{
		TSharedPtr<FJsonValue> Value;
		if (!BuildMessage(Message, Value, OutError))
		{
			return false;
		}
		Input.Add(MoveTemp(Value));
	}
	if (!AppendContinuation(ProviderName, Request, Input, StagedContinuation, StagedBinding, OutError))
	{
		return false;
	}
	OutContinuationCommit.Continuation = MoveTemp(StagedContinuation);
	OutContinuationCommit.Binding = MoveTemp(StagedBinding);
	for (const FUnrealAIModelToolOutput &Output : Request.ToolOutputs)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("type"), TEXT("function_call_output"));
		Object->SetStringField(TEXT("call_id"), Output.ProviderCallId);
		Object->SetStringField(TEXT("output"), Output.OutputJson);
		Input.Add(MakeShared<FJsonValueObject>(Object));
	}

	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const FUnrealAIModelToolDescriptor &Tool : Request.Tools)
	{
		TSharedPtr<FJsonObject> Parameters;
		if (!ParseJsonObject(Tool.InputJsonSchema, FUnrealAIModelToolDescriptor::MaxSchemaUtf8Bytes, Parameters))
		{
			OutContinuationCommit.Reset();
			OutError = MakeOpenAIError(EUnrealAIErrorCategory::SchemaValidation,
									   TEXT("openai_tool_schema_invalid"),
											TEXT("A tool schema is invalid."),
												 TEXT("OpenAI Responses could not parse a validated tool schema."));
			return false;
		}
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("type"), TEXT("function"));
		Object->SetStringField(TEXT("name"), Tool.InvocationName);
		Object->SetStringField(TEXT("description"), Tool.Description);
		Object->SetObjectField(TEXT("parameters"), Parameters);
		Object->SetBoolField(TEXT("strict"), Tool.bStrict);
		Tools.Add(MakeShared<FJsonValueObject>(Object));
	}

	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), Request.ModelId);
	Body->SetArrayField(TEXT("input"), Input);
	if (!Request.Instructions.IsEmpty())
	{
		Body->SetStringField(TEXT("instructions"), Request.Instructions);
	}
	if (!Tools.IsEmpty())
	{
		Body->SetArrayField(TEXT("tools"), Tools);
		Body->SetBoolField(TEXT("parallel_tool_calls"), false);
	}
	if (Request.OutputContract.IsSet())
	{
		TSharedPtr<FJsonObject> Schema;
		if (!ParseJsonObject(Request.OutputContract->JsonSchema, FUnrealAIModelOutputContract::MaxSchemaUtf8Bytes,
							 Schema))
		{
			OutContinuationCommit.Reset();
			OutError = MakeOpenAIError(EUnrealAIErrorCategory::SchemaValidation,
									   TEXT("openai_output_schema_invalid"),
											TEXT("The requested output schema is invalid."),
												 TEXT("OpenAI Responses could not parse a validated output schema."));
			return false;
		}
		TSharedRef<FJsonObject> Format = MakeShared<FJsonObject>();
		Format->SetStringField(TEXT("type"), TEXT("json_schema"));
		Format->SetStringField(TEXT("name"), Request.OutputContract->Name);
		Format->SetObjectField(TEXT("schema"), Schema);
		Format->SetBoolField(TEXT("strict"), Request.OutputContract->bStrict);
		TSharedRef<FJsonObject> Text = MakeShared<FJsonObject>();
		Text->SetObjectField(TEXT("format"), Format);
		Body->SetObjectField(TEXT("text"), Text);
	}
	Body->SetBoolField(TEXT("stream"), true);
	Body->SetBoolField(TEXT("store"), Request.bAllowProviderStorage);
	Body->SetNumberField(TEXT("max_output_tokens"), Request.MaxOutputTokens);
	if (!Request.bAllowProviderStorage)
	{
		TArray<TSharedPtr<FJsonValue>> Include;
		Include.Add(MakeShared<FJsonValueString>(TEXT("reasoning.encrypted_content")));
		Body->SetArrayField(TEXT("include"), Include);
	}

	FString Json;
	if (!SerializeJsonObject(Body, Json))
	{
		OutContinuationCommit.Reset();
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::ProviderProtocol,
								   TEXT("openai_request_serialization_failed"),
										TEXT("The provider request could not be created."),
											 TEXT("OpenAI Responses JSON serialization failed."));
		return false;
	}
	FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() <= 0 || Utf8.Length() > FUnrealAIOpenAIResponsesWireRequest::MaxBodyBytes)
	{
		OutContinuationCommit.Reset();
		OutError = MakeOpenAIError(EUnrealAIErrorCategory::InvalidArgument,
								   TEXT("openai_request_too_large"),
										TEXT("The provider request exceeds the supported size."),
											 TEXT("OpenAI Responses JSON exceeded its body bound."));
		return false;
	}
	OutWireRequest.BodyUtf8.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	return true;
}

class FUnrealAIOpenAIResponsesStreamDecoder::FImpl final
{
  public:
	FImpl(const FName InProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &InFaultPolicy,
		  const FUnrealAIModelRequest &InRequest, FEventSink InSink, FIgnoredEventObserver InIgnoredEventObserver)
		: ProviderName(InProviderName), FaultPolicy(InFaultPolicy), RequestId(InRequest.RequestId),
		  ConnectionBinding(InRequest.ConnectionBinding), ConnectionAlias(InRequest.ConnectionAlias),
		  ModelId(InRequest.ModelId), Sink(MoveTemp(InSink)), IgnoredEventObserver(MoveTemp(InIgnoredEventObserver)),
		  bStructuredOutput(InRequest.OutputContract.IsSet())
	{
		FUnrealAIModelError HistoryError;
		bOriginalHistoryValid = BuildCanonicalRequestHistory(InRequest, OriginalHistory, HistoryError);
		for (const FUnrealAIModelToolDescriptor &Tool : InRequest.Tools)
		{
			Tools.Add(Tool.InvocationName, Tool);
		}
	}

	~FImpl()
	{
		SecureZero(OriginalHistory);
	}

	bool Push(const TConstArrayView<uint8> Bytes, FString &OutError)
	{
		OutError.Reset();
		if (bTerminal)
		{
			return true;
		}
		if (Bytes.Num() > MaxStreamBytes - TotalBytes)
		{
			return ProtocolFailure(TEXT("openai_stream_too_large"),
										TEXT("The provider response exceeded its size limit."),
											 TEXT("OpenAI Responses SSE stream exceeded its aggregate bound."),
												  OutError);
		}
		TotalBytes += Bytes.Num();
		for (const uint8 Byte : Bytes)
		{
			if (bTerminal)
			{
				break;
			}
			if (bIgnoreLfAfterCr)
			{
				bIgnoreLfAfterCr = false;
				if (Byte == static_cast<uint8>('\n'))
				{
					continue;
				}
			}
			if (Byte == static_cast<uint8>('\r'))
			{
				if (!ProcessLine(OutError))
				{
					return false;
				}
				bIgnoreLfAfterCr = true;
			}
			else if (Byte == static_cast<uint8>('\n'))
			{
				if (!ProcessLine(OutError))
				{
					return false;
				}
			}
			else
			{
				if (CurrentLine.Num() >= MaxEventBytes)
				{
					return ProtocolFailure(TEXT("openai_sse_line_too_large"),
												TEXT("The provider response contained an oversized event."),
													 TEXT("OpenAI Responses SSE line exceeded its bound."), OutError);
				}
				CurrentLine.Add(Byte);
			}
		}
		return !bFailed;
	}

	bool Finish(FString &OutError)
	{
		OutError.Reset();
		if (bTerminal)
		{
			return !bFailed;
		}
		if (!CurrentLine.IsEmpty() && !ProcessLine(OutError))
		{
			return false;
		}
		if (!Data.IsEmpty() && !ProcessEvent(OutError))
		{
			return false;
		}
		if (!bTerminal)
		{
			return ProtocolFailure(TEXT("openai_stream_truncated"),
										TEXT("The provider response ended unexpectedly."),
											 TEXT("OpenAI Responses SSE stream ended without a terminal event."),
												  OutError);
		}
		return !bFailed;
	}

	void Cancel()
	{
		if (bTerminal)
		{
			return;
		}
		FUnrealAIModelEvent Event;
		PopulateEnvelope(Event);
		Event.Kind = EUnrealAIModelEventKind::Cancelled;
		Event.SequenceNumber = ++Sequence;
		Event.Error =
			MakeOpenAIError(EUnrealAIErrorCategory::Cancelled,
							TEXT("openai_request_cancelled"), TEXT("The model request was cancelled."),
																   TEXT("OpenAI Responses request cancelled locally."));
		FaultPolicy.ApplyTo(Event.Error);
		bTerminal = true;
		Sink(MoveTemp(Event));
	}

	bool IsTerminal() const
	{
		return bTerminal;
	}

  private:
	struct FToolAssembly final
	{
		int32 ProviderOutputIndex = INDEX_NONE;
		int32 NormalizedToolOutputIndex = INDEX_NONE;
		FString ItemId;
		FString CallId;
		FName StableName;
		int32 Version = 0;
		FString Arguments;
		FString FinalArguments;
		bool bCompleted = false;
	};

	struct FTextAssembly final
	{
		FString ItemId;
		FString Text;
		int64 TextUtf8Bytes = 0;
		bool bDone = false;
	};

	static bool TryMeasureRetainedText(const int64 Utf8Bytes, const int32 CopyCount, const int64 FixedOverheadBytes,
									   int64 &OutPhysicalBytes)
	{
		OutPhysicalBytes = 0;
		if (Utf8Bytes < 0 || CopyCount < 0 || FixedOverheadBytes < 0)
		{
			return false;
		}
		const int64 PerCopyMultiplier = static_cast<int64>(sizeof(TCHAR)) * 2;
		const int64 Multiplier = PerCopyMultiplier * CopyCount;
		const int64 TerminatorBytes = PerCopyMultiplier * CopyCount;
		if (FixedOverheadBytes > MAX_int64 - TerminatorBytes ||
			(Multiplier > 0 && Utf8Bytes > (MAX_int64 - FixedOverheadBytes - TerminatorBytes) / Multiplier))
		{
			return false;
		}
		OutPhysicalBytes = Utf8Bytes * Multiplier + TerminatorBytes + FixedOverheadBytes;
		return true;
	}

	static bool TryMeasureRetainedJsonDom(const int64 JsonUtf8Bytes, int64 &OutPhysicalBytes)
	{
		constexpr int64 JsonDomExpansionFactor = 8;
		constexpr int64 PerItemOverheadBytes = 1024;
		OutPhysicalBytes = 0;
		if (JsonUtf8Bytes < 0 || JsonUtf8Bytes > (MAX_int64 - PerItemOverheadBytes) / JsonDomExpansionFactor)
		{
			return false;
		}
		OutPhysicalBytes = JsonUtf8Bytes * JsonDomExpansionFactor + PerItemOverheadBytes;
		return true;
	}

	bool RetainedStateFailure(FString &OutError)
	{
		return ProtocolFailure(
			TEXT("openai_retained_state_too_large"),
				 TEXT("The provider response retained too much intermediate state."),
					  TEXT("OpenAI Responses decoded state exceeded its cumulative retained physical-memory budget."),
						   OutError);
	}

	bool ConsumeRetainedResponseStateBytes(const int64 AddedBytes, FString &OutError)
	{
		if (AddedBytes < 0 || AddedBytes > FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedResponseStateBytes -
											   RetainedResponseStateBytes)
		{
			return RetainedStateFailure(OutError);
		}
		RetainedResponseStateBytes += AddedBytes;
		return true;
	}

	bool ValidateCompletedMessageItem(const TSharedPtr<FJsonObject> &Item, FString &OutItemId, FString &OutRole,
									  const TArray<TSharedPtr<FJsonValue>> *&OutContent, int32 &OutPartCount,
									  FString &OutError)
	{
		OutItemId.Reset();
		OutRole.Reset();
		OutContent = nullptr;
		OutPartCount = 0;
		if (!Item.IsValid() ||
			!Item->TryGetStringField(
				TEXT("id"), OutItemId) || !IsProviderOpaqueToken(OutItemId) ||
				!Item->TryGetStringField(
					TEXT("role"), OutRole) ||
					OutRole != TEXT("assistant") ||
									!Item->TryGetArrayField(
										TEXT("content"), OutContent) || OutContent == nullptr ||
										OutContent->Num() > FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts)
		{
			return ProtocolFailure(
				TEXT("openai_message_item_invalid"),
					 TEXT("The provider returned an invalid completed message."),
						  TEXT("OpenAI Responses completed message omitted bounded identity, role, or content fields."),
							   OutError);
		}

		int64 MessageTextBytes = 0;
		for (const TSharedPtr<FJsonValue> &PartValue : *OutContent)
		{
			if (!PartValue.IsValid() || PartValue->Type != EJson::Object)
			{
				return ProtocolFailure(TEXT("openai_message_item_invalid"),
					TEXT("The provider returned an invalid completed message."),
						 TEXT("OpenAI Responses completed message contained a non-object content part."), OutError);
			}
			const TSharedPtr<FJsonObject> Part = PartValue->AsObject();
			FString PartType;
			if (!Part.IsValid() || !Part->TryGetStringField(TEXT("type"), PartType) ||
															!IsBoundedUtf8(PartType, 64, false))
			{
				return ProtocolFailure(TEXT("openai_message_item_invalid"),
					TEXT("The provider returned an invalid completed message."),
						 TEXT("OpenAI Responses completed message contained an invalid content-part type."), OutError);
			}

			FString Text;
			const TCHAR *TextField = nullptr;
			if (PartType == TEXT("output_text"))
			{
				TextField = TEXT("text");
			}
			else if (PartType == TEXT("refusal"))
			{
				TextField = TEXT("refusal");
			}
			if (TextField != nullptr)
			{
				if (!Part->TryGetStringField(TextField, Text) ||
					!IsBoundedUtf8(Text, FUnrealAIModelEvent::MaxFinalTextUtf8Bytes, true))
				{
					return ProtocolFailure(
						TEXT("openai_message_item_invalid"),
							 TEXT("The provider returned an invalid completed message."),
								  TEXT("OpenAI Responses completed message contained invalid bounded text."), OutError);
				}
				const int64 TextBytes = Utf8Length(Text);
				if (TextBytes > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes - MessageTextBytes)
				{
					return ProtocolFailure(TEXT("openai_message_item_invalid"),
						TEXT("The provider returned an invalid completed message."),
							 TEXT("OpenAI Responses completed message exceeded its cumulative text bound."), OutError);
				}
				MessageTextBytes += TextBytes;
			}
		}
		OutPartCount = OutContent->Num();
		return true;
	}

	static bool JsonArraysEqual(const TArray<TSharedPtr<FJsonValue>> &Left, const TArray<TSharedPtr<FJsonValue>> &Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (!Left[Index].IsValid() || !Right[Index].IsValid() ||
				!FJsonValue::CompareEqual(*Left[Index], *Right[Index]))
			{
				return false;
			}
		}
		return true;
	}

	bool ReconcileCompletedMessageItem(const TSharedPtr<FJsonObject> &StreamedItem,
									   const TSharedPtr<FJsonObject> &TerminalItem, FString &OutError)
	{
		FString StreamedItemId;
		FString StreamedRole;
		const TArray<TSharedPtr<FJsonValue>> *StreamedContent = nullptr;
		int32 StreamedPartCount = 0;
		if (!ValidateCompletedMessageItem(StreamedItem, StreamedItemId, StreamedRole, StreamedContent,
										  StreamedPartCount, OutError))
		{
			return false;
		}

		FString TerminalItemId;
		FString TerminalRole;
		const TArray<TSharedPtr<FJsonValue>> *TerminalContent = nullptr;
		int32 TerminalPartCount = 0;
		if (!ValidateCompletedMessageItem(TerminalItem, TerminalItemId, TerminalRole, TerminalContent,
										  TerminalPartCount, OutError))
		{
			return false;
		}
		if (StreamedItemId != TerminalItemId || StreamedRole != TerminalRole ||
			StreamedPartCount != TerminalPartCount || StreamedContent == nullptr || TerminalContent == nullptr ||
			!JsonArraysEqual(*StreamedContent, *TerminalContent))
		{
			return ProtocolFailure(TEXT("openai_message_reconciliation_invalid"),
				TEXT("The provider changed a completed message."),
					 TEXT("OpenAI Responses terminal message identity or content disagreed with its completed item."),
						  OutError);
		}
		return true;
	}

	bool ProcessLine(FString &OutError)
	{
		if (CurrentLine.IsEmpty())
		{
			return Data.IsEmpty() || ProcessEvent(OutError);
		}
		constexpr ANSICHAR Prefix[] = "data:";
		const int32 PrefixLength = UE_ARRAY_COUNT(Prefix) - 1;
		bool bDataLine = CurrentLine.Num() >= PrefixLength;
		for (int32 Index = 0; bDataLine && Index < PrefixLength; ++Index)
		{
			bDataLine = CurrentLine[Index] == static_cast<uint8>(Prefix[Index]);
		}
		if (bDataLine)
		{
			int32 Start = PrefixLength;
			if (Start < CurrentLine.Num() && CurrentLine[Start] == static_cast<uint8>(' '))
			{
				++Start;
			}
			const int32 Added = CurrentLine.Num() - Start + (Data.IsEmpty() ? 0 : 1);
			if (Added > MaxEventBytes - Data.Num())
			{
				CurrentLine.Reset();
				return ProtocolFailure(TEXT("openai_sse_event_too_large"),
											TEXT("The provider response contained an oversized event."),
												 TEXT("OpenAI Responses SSE data event exceeded its bound."), OutError);
			}
			if (!Data.IsEmpty())
			{
				Data.Add(static_cast<uint8>('\n'));
			}
			Data.Append(CurrentLine.GetData() + Start, CurrentLine.Num() - Start);
		}
		CurrentLine.Reset();
		return true;
	}

	bool ProcessEvent(FString &OutError)
	{
		TArray<uint8> EventData = MoveTemp(Data);
		Data.Reset();
		if (EventData.Num() == 6 && FMemory::Memcmp(EventData.GetData(), "[DONE]", 6) == 0)
		{
			return true;
		}
		TSharedPtr<FJsonObject> Event;
		if (!ParseJsonObjectUtf8(EventData, MaxEventBytes, Event))
		{
			return ProtocolFailure(TEXT("openai_sse_json_invalid"),
										TEXT("The provider returned an invalid streaming event."),
											 TEXT("OpenAI Responses SSE data was not a bounded JSON object."),
												  OutError);
		}
		FString Type;
		if (!Event->TryGetStringField(TEXT("type"), Type) || !IsBoundedUtf8(Type, 256, false))
		{
			return ProtocolFailure(TEXT("openai_event_type_invalid"),
										TEXT("The provider returned an invalid streaming event."),
											 TEXT("OpenAI Responses event type was absent or invalid."), OutError);
		}
		double ProviderSequence = -1.0;
		if (!Event->TryGetNumberField(TEXT("sequence_number"), ProviderSequence) ||
									  !FMath::IsFinite(ProviderSequence) || ProviderSequence < 0.0 ||
									  ProviderSequence > MaxExactJsonInteger ||
									  FMath::FloorToDouble(ProviderSequence) != ProviderSequence ||
									  ProviderSequence <= static_cast<double>(LastProviderSequence))
		{
			return ProtocolFailure(TEXT("openai_event_sequence_invalid"),
										TEXT("The provider returned an invalid streaming event sequence."),
											 TEXT("OpenAI Responses event sequence_number was absent, non-integral, duplicated, or out of order."),
												  OutError);
		}
		LastProviderSequence = static_cast<int64>(ProviderSequence);
		if (Type == TEXT("response.queued") || Type == TEXT("response.created") || Type == TEXT("response.in_progress"))
		{
			const TSharedPtr<FJsonObject> *Response = nullptr;
			if (!Event->TryGetObjectField(TEXT("response"), Response) || Response == nullptr || !Response->IsValid())
			{
				return ProtocolFailure(TEXT("openai_response_identity_missing"),
											TEXT("The provider returned an invalid response identity."),
												 TEXT("OpenAI Responses lifecycle event omitted its response object."),
													  OutError);
			}
			if (!CaptureResponseId(*Response, OutError))
			{
				return false;
			}
			EmitStarted();
			return true;
		}
		if (Type == TEXT("response.output_text.delta"))
		{
			int32 OutputIndex = INDEX_NONE;
			int32 ContentIndex = INDEX_NONE;
			FString ItemId;
			FString Delta;
			if (!ReadBoundedIndex(Event, TEXT("output_index"), OutputIndex) ||
								  !ReadBoundedIndex(
									  Event, TEXT("content_index"), ContentIndex) ||
									  !Event->TryGetStringField(
										  TEXT("item_id"), ItemId) || !IsProviderOpaqueToken(ItemId) ||
										  !Event->TryGetStringField(
											  TEXT("delta"), Delta) || Delta.IsEmpty() ||
											  !IsBoundedUtf8(Delta, FUnrealAIModelEvent::MaxTextDeltaUtf8Bytes, false))
			{
				return ProtocolFailure(TEXT("openai_text_delta_invalid"),
					TEXT("The provider returned an invalid text event."),
						 TEXT("OpenAI Responses text delta omitted bounded part identity or contained invalid text."),
							  OutError);
			}
			const int64 PartKey = (static_cast<int64>(OutputIndex) << 32) | static_cast<uint32>(ContentIndex);
			FTextAssembly *Assembly = TextAssemblies.Find(PartKey);
			const bool bNewAssembly = Assembly == nullptr;
			const int64 DeltaBytes = Utf8Length(Delta);
			if ((bNewAssembly && TextAssemblies.Num() + RetainedCompletedMessagePartCount >=
									 FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts) ||
				(!bNewAssembly && (Assembly->ItemId != ItemId || Assembly->bDone)) ||
				DeltaBytes > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes - AssembledTextUtf8Bytes ||
				(!bNewAssembly && DeltaBytes > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes - Assembly->TextUtf8Bytes))
			{
				return ProtocolFailure(bNewAssembly && TextAssemblies.Num() + RetainedCompletedMessagePartCount >=
														   FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts
									   ? TEXT("openai_text_parts_too_many")
									   : TEXT("openai_text_delta_lifecycle_invalid"),
											  TEXT("The provider returned an invalid text event."),
												   TEXT("OpenAI Responses text delta changed identity, followed completion, or exceeded cumulative part bounds."),
														OutError);
			}
			int64 RetainedBytesAdded = 0;
			if (!TryMeasureRetainedText(DeltaBytes, 2,
										bNewAssembly ? static_cast<int64>(sizeof(FTextAssembly)) + 128 : 0,
										RetainedBytesAdded))
			{
				return RetainedStateFailure(OutError);
			}
			if (bNewAssembly)
			{
				int64 ItemIdBytes = 0;
				if (!TryMeasureRetainedText(Utf8Length(ItemId), 1, 0, ItemIdBytes) ||
					ItemIdBytes > MAX_int64 - RetainedBytesAdded)
				{
					return RetainedStateFailure(OutError);
				}
				RetainedBytesAdded += ItemIdBytes;
			}
			if (!ConsumeRetainedResponseStateBytes(RetainedBytesAdded, OutError))
			{
				return false;
			}
			if (bNewAssembly)
			{
				FTextAssembly NewAssembly;
				NewAssembly.ItemId = ItemId;
				Assembly = &TextAssemblies.Add(PartKey, MoveTemp(NewAssembly));
			}
			EmitStarted();
			Assembly->Text += Delta;
			Assembly->TextUtf8Bytes += DeltaBytes;
			AssembledTextUtf8Bytes += DeltaBytes;
			AccumulatedText += Delta;
			AccumulatedTextUtf8Bytes += DeltaBytes;
			FUnrealAIModelEvent ModelEvent;
			PopulateEnvelope(ModelEvent);
			ModelEvent.Kind =
				bStructuredOutput ? EUnrealAIModelEventKind::StructuredDelta : EUnrealAIModelEventKind::TextDelta;
			ModelEvent.SequenceNumber = ++Sequence;
			if (bStructuredOutput)
			{
				ModelEvent.StructuredDelta = MoveTemp(Delta);
			}
			else
			{
				ModelEvent.TextDelta = MoveTemp(Delta);
			}
			Sink(MoveTemp(ModelEvent));
			return true;
		}
		if (Type == TEXT("response.output_text.done"))
		{
			int32 OutputIndex = INDEX_NONE;
			int32 ContentIndex = INDEX_NONE;
			FString ItemId;
			FString Text;
			if (!ReadBoundedIndex(Event, TEXT("output_index"), OutputIndex) ||
								  !ReadBoundedIndex(Event, TEXT("content_index"), ContentIndex) ||
													!Event->TryGetStringField(
														TEXT("item_id"), ItemId) || !IsProviderOpaqueToken(ItemId) ||
														!Event->TryGetStringField(
															TEXT("text"), Text) ||
															!IsBoundedUtf8(
																Text, FUnrealAIModelEvent::MaxFinalTextUtf8Bytes, true))
			{
				return ProtocolFailure(TEXT("openai_text_done_invalid"),
					TEXT("The provider returned an invalid text completion event."),
						TEXT("OpenAI Responses output-text completion was missing bounded item, index, or text fields."),
							OutError);
			}
			const int64 PartKey = (static_cast<int64>(OutputIndex) << 32) | static_cast<uint32>(ContentIndex);
			FTextAssembly *Assembly = TextAssemblies.Find(PartKey);
			const bool bNewAssembly = Assembly == nullptr;
			const int64 TextBytes = Utf8Length(Text);
			const int64 NewlyAssembledTextBytes = (bNewAssembly || Assembly->Text.IsEmpty()) ? TextBytes : 0;
			if ((bNewAssembly && TextAssemblies.Num() + RetainedCompletedMessagePartCount >=
									 FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts) ||
				(!bNewAssembly && (Assembly->ItemId != ItemId || Assembly->bDone ||
								   (!Assembly->Text.IsEmpty() && Assembly->Text != Text))) ||
				NewlyAssembledTextBytes > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes - AssembledTextUtf8Bytes ||
				TextBytes > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes - CompletedTextUtf8Bytes)
			{
				return ProtocolFailure(
					bNewAssembly && TextAssemblies.Num() + RetainedCompletedMessagePartCount >=
										FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts
					? TEXT("openai_text_parts_too_many")
					: TEXT("openai_text_done_lifecycle_invalid"),
						   TEXT("The provider returned an invalid text completion event."),
								TEXT("OpenAI Responses output-text completion changed identity, duplicated completion, disagreed with fragments, or exceeded cumulative part bounds."),
									 OutError);
			}
			int64 RetainedBytesAdded = 0;
			if (!TryMeasureRetainedText(NewlyAssembledTextBytes, NewlyAssembledTextBytes > 0 ? 1 : 0,
										bNewAssembly ? static_cast<int64>(sizeof(FTextAssembly)) + 128 : 0,
										RetainedBytesAdded))
			{
				return RetainedStateFailure(OutError);
			}
			if (bNewAssembly)
			{
				int64 ItemIdBytes = 0;
				if (!TryMeasureRetainedText(Utf8Length(ItemId), 1, 0, ItemIdBytes) ||
					ItemIdBytes > MAX_int64 - RetainedBytesAdded)
				{
					return RetainedStateFailure(OutError);
				}
				RetainedBytesAdded += ItemIdBytes;
			}
			if (!ConsumeRetainedResponseStateBytes(RetainedBytesAdded, OutError))
			{
				return false;
			}
			if (bNewAssembly)
			{
				FTextAssembly NewAssembly;
				NewAssembly.ItemId = ItemId;
				Assembly = &TextAssemblies.Add(PartKey, MoveTemp(NewAssembly));
			}
			Assembly->Text = MoveTemp(Text);
			Assembly->TextUtf8Bytes = TextBytes;
			Assembly->bDone = true;
			AssembledTextUtf8Bytes += NewlyAssembledTextBytes;
			CompletedTextUtf8Bytes += TextBytes;
			++CompletedTextPartCount;
			return true;
		}
		if (Type == TEXT("response.output_item.added"))
		{
			const TSharedPtr<FJsonObject> *Item = nullptr;
			int32 OutputIndex = INDEX_NONE;
			if (!Event->TryGetObjectField(TEXT("item"), Item) || Item == nullptr || !Item->IsValid() ||
										  !ReadBoundedIndex(Event, TEXT("output_index"), OutputIndex))
			{
				return ProtocolFailure(TEXT("openai_output_item_invalid"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses output-item start was malformed."), OutError);
			}
			FString ItemType;
			if (!(*Item)->TryGetStringField(TEXT("type"), ItemType))
			{
				return ProtocolFailure(TEXT("openai_output_item_type_missing"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses output-item start omitted its type."),
													  OutError);
			}
			if (ItemType == TEXT("function_call"))
			{
				FString WholeArguments;
				if ((*Item)->TryGetStringField(TEXT("arguments"), WholeArguments) && !WholeArguments.IsEmpty())
				{
					// xAI documents that streamed function calls can arrive as one
					// complete chunk. Accept that bounded full-item form at the
					// normal item-added boundary while retaining the same identity,
					// JSON-shape, aggregate-byte, and terminal reconciliation checks.
					return ReconcileFunctionCall(*Item, OutputIndex, OutError);
				}
				return StartToolCallFromItem(*Item, OutputIndex, OutError);
			}
			return true;
		}
		if (Type == TEXT("response.function_call_arguments.delta"))
		{
			return AppendToolArgumentsDelta(Event, OutError);
		}
		if (Type == TEXT("response.function_call_arguments.done"))
		{
			return CompleteToolArguments(Event, OutError);
		}
		if (Type == TEXT("response.output_item.done"))
		{
			const TSharedPtr<FJsonObject> *Item = nullptr;
			int32 OutputIndex = INDEX_NONE;
			if (!Event->TryGetObjectField(TEXT("item"), Item) || Item == nullptr || !Item->IsValid() ||
										  !ReadBoundedIndex(Event, TEXT("output_index"), OutputIndex))
			{
				return ProtocolFailure(TEXT("openai_output_item_invalid"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses output-item event was malformed."), OutError);
			}
			if (OutputItems.Contains(OutputIndex))
			{
				return ProtocolFailure(TEXT("openai_output_item_duplicate"),
											TEXT("The provider returned a duplicate output item."),
												 TEXT("OpenAI Responses repeated an output-item index."), OutError);
			}
			if (OutputItems.Num() >= FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedOutputItems)
			{
				return ProtocolFailure(
					TEXT("openai_output_items_too_many"),
						 TEXT("The provider returned too many output items."),
							  TEXT("OpenAI Responses output-item retention count exceeded its bound."), OutError);
			}
			FString ItemType;
			if (!(*Item)->TryGetStringField(TEXT("type"), ItemType) || !IsBoundedUtf8(ItemType, 64, false))
			{
				return ProtocolFailure(TEXT("openai_output_item_type_missing"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses output-item completion omitted its type."),
													  OutError);
			}
			int32 MessagePartCount = 0;
			if (ItemType == TEXT("message"))
			{
				FString ItemId;
				FString Role;
				const TArray<TSharedPtr<FJsonValue>> *Content = nullptr;
				if (!ValidateCompletedMessageItem(*Item, ItemId, Role, Content, MessagePartCount, OutError))
				{
					return false;
				}
				if (MessagePartCount > FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts -
										   TextAssemblies.Num() - RetainedCompletedMessagePartCount)
				{
					return ProtocolFailure(
						TEXT("openai_text_parts_too_many"),
							 TEXT("The provider returned too many completed message parts."),
								  TEXT("OpenAI Responses retained message content-part count exceeded its bound."),
									   OutError);
				}
			}

			FString ItemJson;
			if (!SerializeJsonObject((*Item).ToSharedRef(), ItemJson))
			{
				return ProtocolFailure(TEXT("openai_output_item_invalid"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses could not measure a completed output item."),
													  OutError);
			}
			const int64 ItemBytes = Utf8Length(ItemJson);
			if (ItemBytes <= 0 || ItemBytes > FUnrealAIOpenAIResponsesStreamDecoder::MaxEventBytes)
			{
				return ProtocolFailure(
					TEXT("openai_output_item_invalid"),
						 TEXT("The provider returned an invalid output item."),
							  TEXT("OpenAI Responses completed output item exceeded its serialized bound."), OutError);
			}
			int64 RetainedItemBytes = 0;
			if (!TryMeasureRetainedJsonDom(ItemBytes, RetainedItemBytes))
			{
				return RetainedStateFailure(OutError);
			}
			if (!ConsumeRetainedResponseStateBytes(RetainedItemBytes, OutError))
			{
				return false;
			}

			OutputItems.Add(OutputIndex, MakeShared<FJsonValueObject>((*Item).ToSharedRef()));
			RetainedCompletedMessagePartCount += MessagePartCount;
			if (ItemType == TEXT("function_call") && !ReconcileFunctionCall(*Item, OutputIndex, OutError))
			{
				return false;
			}
			return true;
		}
		if (Type == TEXT("response.completed"))
		{
			const TSharedPtr<FJsonObject> *Response = nullptr;
			if (!Event->TryGetObjectField(TEXT("response"), Response) || Response == nullptr || !Response->IsValid())
			{
				return ProtocolFailure(
					TEXT("openai_completed_invalid"),
						 TEXT("The provider returned an invalid completion."),
							  TEXT("OpenAI Responses terminal completion omitted the response object."), OutError);
			}
			return Complete(*Response, OutError);
		}
		if (Type == TEXT("response.failed") || Type == TEXT("response.incomplete") || Type == TEXT("error"))
		{
			const TSharedPtr<FJsonObject> *Response = nullptr;
			const bool bHasResponse =
				Event->TryGetObjectField(TEXT("response"), Response) && Response != nullptr && Response->IsValid();
			if (Type != TEXT("error") && !bHasResponse)
			{
				return ProtocolFailure(TEXT("openai_response_identity_missing"),
											TEXT("The provider returned an invalid response identity."),
												 TEXT("OpenAI Responses terminal failure omitted its response object."),
													  OutError);
			}
			if (bHasResponse && !CaptureResponseId(*Response, OutError))
			{
				return false;
			}
			return ProviderFailure(Type == TEXT("response.incomplete")
								   ? TEXT("openai_response_incomplete")
								   : TEXT("openai_response_failed"), Type == TEXT("response.incomplete"), OutError);
		}
		// Unknown provider events are intentionally tolerated after bounded JSON,
		// type, and sequence validation. The observer receives no untrusted data.
		if (IgnoredEventObserver)
		{
			IgnoredEventObserver();
		}
		return true;
	}

	bool Complete(const TSharedPtr<FJsonObject> &Response, FString &OutError)
	{
		if (bTerminal)
		{
			return true;
		}
		if (!CaptureResponseId(Response, OutError))
		{
			return false;
		}
		EmitStarted();
		TArray<TSharedPtr<FJsonValue>> Items;
		TArray<int32> ItemIndices;
		const TArray<TSharedPtr<FJsonValue>> *ResponseItems = nullptr;
		const bool bHasTerminalOutput =
			Response->TryGetArrayField(TEXT("output"), ResponseItems) && ResponseItems != nullptr;
		if (bHasTerminalOutput)
		{
			if (ResponseItems->Num() > FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedOutputItems)
			{
				return ProtocolFailure(
					TEXT("openai_output_items_too_many"),
						 TEXT("The provider returned too many output items."),
							  TEXT("OpenAI Responses terminal output item count exceeded its bound."), OutError);
			}
			Items = *ResponseItems;
			ItemIndices.Reserve(Items.Num());
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				ItemIndices.Add(Index);
			}
		}
		else
		{
			TArray<int32> Indices;
			OutputItems.GetKeys(Indices);
			Indices.Sort();
			for (const int32 Index : Indices)
			{
				Items.Add(OutputItems.FindChecked(Index));
				ItemIndices.Add(Index);
			}
		}
		if (Items.Num() > FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedOutputItems)
		{
			return ProtocolFailure(TEXT("openai_output_items_too_many"),
										TEXT("The provider returned too many output items."),
											 TEXT("OpenAI Responses output item count exceeded its bound."), OutError);
		}

		if (bHasTerminalOutput)
		{
			for (const TPair<int32, TSharedPtr<FJsonValue>> &Pair : OutputItems)
			{
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				{
					return ProtocolFailure(TEXT("openai_output_item_invalid"),
												TEXT("The provider returned an invalid output item."),
													 TEXT("OpenAI Responses retained output item was not an object."),
														  OutError);
				}
				const TSharedPtr<FJsonObject> StreamedItem = Pair.Value->AsObject();
				FString StreamedType;
				if (!StreamedItem.IsValid() || !StreamedItem->TryGetStringField(TEXT("type"), StreamedType))
				{
					return ProtocolFailure(TEXT("openai_output_item_type_missing"),
												TEXT("The provider returned an invalid output item."),
													 TEXT("OpenAI Responses retained output item omitted its type."),
														  OutError);
				}
				if (StreamedType != TEXT("message"))
				{
					continue;
				}
				if (!Items.IsValidIndex(Pair.Key) || !Items[Pair.Key].IsValid() ||
					Items[Pair.Key]->Type != EJson::Object)
				{
					return ProtocolFailure(TEXT("openai_message_terminal_missing"),
						TEXT("The provider omitted a completed message."),
							 TEXT("OpenAI Responses terminal output omitted a previously completed message index."),
								  OutError);
				}
				const TSharedPtr<FJsonObject> TerminalItem = Items[Pair.Key]->AsObject();
				FString TerminalType;
				if (!TerminalItem.IsValid() || !TerminalItem->TryGetStringField(TEXT("type"), TerminalType) ||
																				TerminalType != TEXT("message"))
				{
					return ProtocolFailure(
						TEXT("openai_message_reconciliation_invalid"),
							 TEXT("The provider changed a completed message."),
								  TEXT("OpenAI Responses terminal output changed a completed message type."), OutError);
				}
				if (!ReconcileCompletedMessageItem(StreamedItem, TerminalItem, OutError))
				{
					return false;
				}
			}
		}

		FString FinalText;
		int64 FinalTextUtf8Bytes = 0;
		int32 TerminalMessagePartCount = 0;
		TSet<int32> CompletionToolIndices;
		for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
		{
			const int32 OutputIndex = ItemIndices[ItemIndex];
			const TSharedPtr<FJsonValue> &Value = Items[ItemIndex];
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				return ProtocolFailure(TEXT("openai_output_item_invalid"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses output array contained a non-object item."),
													  OutError);
			}
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			FString ItemType;
			if (!Item->TryGetStringField(TEXT("type"), ItemType))
			{
				return ProtocolFailure(TEXT("openai_output_item_type_missing"),
											TEXT("The provider returned an invalid output item."),
												 TEXT("OpenAI Responses output item omitted its type."), OutError);
			}
			if (ItemType == TEXT("message"))
			{
				const TArray<TSharedPtr<FJsonValue>> *Content = nullptr;
				if (!Item->TryGetArrayField(TEXT("content"), Content) || Content == nullptr)
				{
					return ProtocolFailure(TEXT("openai_message_content_invalid"),
												TEXT("The provider returned an invalid message."),
													 TEXT("OpenAI Responses message item omitted its content array."),
														  OutError);
				}
				if (Content->Num() >
					FUnrealAIOpenAIResponsesStreamDecoder::MaxRetainedTextParts - TerminalMessagePartCount)
				{
					return ProtocolFailure(
						TEXT("openai_text_parts_too_many"),
							 TEXT("The provider returned too many message content parts."),
								  TEXT("OpenAI Responses terminal message content-part count exceeded its bound."),
									   OutError);
				}
				TerminalMessagePartCount += Content->Num();
				for (const TSharedPtr<FJsonValue> &PartValue : *Content)
				{
					if (!PartValue.IsValid() || PartValue->Type != EJson::Object)
					{
						return ProtocolFailure(
							TEXT("openai_message_part_invalid"),
								 TEXT("The provider returned an invalid message."),
									  TEXT("OpenAI Responses message content contained a non-object part."), OutError);
					}
					const TSharedPtr<FJsonObject> Part = PartValue->AsObject();
					FString PartType;
					FString Text;
					if (!Part->TryGetStringField(TEXT("type"), PartType))
					{
						return ProtocolFailure(
							TEXT("openai_message_part_type_missing"),
								 TEXT("The provider returned an invalid message."),
									  TEXT("OpenAI Responses message content part omitted its type."), OutError);
					}
					if (PartType == TEXT("output_text"))
					{
						if (!Part->TryGetStringField(TEXT("text"), Text) ||
													 !IsBoundedUtf8(Text, FUnrealAIModelEvent::MaxFinalTextUtf8Bytes,
																	true))
						{
							return ProtocolFailure(
								TEXT("openai_message_part_text_invalid"),
									 TEXT("The provider returned invalid message text."),
										  TEXT("OpenAI Responses output-text part omitted bounded text."), OutError);
						}
					}
					else if (PartType == TEXT("refusal"))
					{
						if (!Part->TryGetStringField(TEXT("refusal"), Text) ||
													 !IsBoundedUtf8(Text, FUnrealAIModelEvent::MaxFinalTextUtf8Bytes,
																	true))
						{
							return ProtocolFailure(
								TEXT("openai_message_part_text_invalid"),
									 TEXT("The provider returned invalid message text."),
										  TEXT("OpenAI Responses refusal part omitted bounded text."), OutError);
						}
					}
					if (!Text.IsEmpty())
					{
						const int64 TextBytes = Utf8Length(Text);
						if (TextBytes > FUnrealAIModelEvent::MaxFinalTextUtf8Bytes - FinalTextUtf8Bytes)
						{
							return ProtocolFailure(TEXT("openai_completion_text_too_large"),
														TEXT("The provider returned oversized text output."),
															 TEXT("OpenAI Responses terminal message text exceeded the normalized cumulative bound."),
																  OutError);
						}
						FinalText += Text;
						FinalTextUtf8Bytes += TextBytes;
					}
				}
			}
			else if (ItemType == TEXT("function_call"))
			{
				if (!ReconcileFunctionCall(Item, OutputIndex, OutError))
				{
					return false;
				}
				CompletionToolIndices.Add(OutputIndex);
			}
		}

		for (const auto &Pair : ToolAssembliesByProviderOutputIndex)
		{
			if (!Pair.Value.bCompleted || !CompletionToolIndices.Contains(Pair.Key))
			{
				return ProtocolFailure(
					TEXT("openai_function_terminal_missing"),
						 TEXT("The provider returned an incomplete tool-call lifecycle."),
							  TEXT("OpenAI Responses terminal output omitted or left open a streamed function call."),
								   OutError);
			}
		}

		if (FinalText.IsEmpty())
		{
			FinalText = AccumulatedText;
			FinalTextUtf8Bytes = AccumulatedTextUtf8Bytes;
		}
		FString DoneText;
		DoneText.Reserve(static_cast<int32>(CompletedTextUtf8Bytes));
		TArray<int64> DoneTextKeys;
		TextAssemblies.GetKeys(DoneTextKeys);
		DoneTextKeys.Sort();
		for (const int64 Key : DoneTextKeys)
		{
			const FTextAssembly &Assembly = TextAssemblies.FindChecked(Key);
			if (Assembly.bDone)
			{
				DoneText += Assembly.Text;
			}
		}
		if (!IsBoundedUtf8(FinalText, FUnrealAIModelEvent::MaxFinalTextUtf8Bytes, true) ||
			(!AccumulatedText.IsEmpty() && FinalText != AccumulatedText) ||
			(CompletedTextPartCount > 0 && FinalText != DoneText) || (FinalText.IsEmpty() && CompletedToolCalls == 0))
		{
			return ProtocolFailure(TEXT("openai_completion_reconciliation_invalid"),
				TEXT("The provider returned inconsistent or empty output."),
					 TEXT("OpenAI Responses terminal output did not exactly reconcile its text and function streams."),
						  OutError);
		}

		if (!FinalText.IsEmpty())
		{
			FUnrealAIModelEvent TextCompleted;
			PopulateEnvelope(TextCompleted);
			TextCompleted.Kind = bStructuredOutput ? EUnrealAIModelEventKind::StructuredCompleted
												   : EUnrealAIModelEventKind::TextCompleted;
			TextCompleted.SequenceNumber = ++Sequence;
			if (bStructuredOutput)
			{
				TSharedPtr<FJsonObject> Structured;
				if (!ParseJsonObject(FinalText, FUnrealAIModelProfileProjection::MaxStructuredOutputUtf8Bytes,
									 Structured))
				{
					return ProtocolFailure(
						TEXT("openai_structured_output_invalid"),
							 TEXT("The provider returned invalid structured output."),
								  TEXT("OpenAI Responses structured output was not a bounded JSON object."), OutError);
				}
				TextCompleted.StructuredJson = FinalText;
			}
			else
			{
				TextCompleted.FinalText = FinalText;
			}
			Sink(MoveTemp(TextCompleted));
		}

		TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation;
		if (CompletedToolCalls > 0)
		{
			FUnrealAIModelContinuationBinding Binding;
			Binding.ProviderName = ProviderName;
			Binding.ConnectionAlias = ConnectionAlias;
			Binding.ModelId = ModelId;
			Binding.ConnectionBinding = ConnectionBinding;
			if (bOriginalHistoryValid)
			{
				Continuation = FUnrealAIOpenAIContinuation::Create(Binding, Items, MoveTemp(OriginalHistory));
			}
			if (!Continuation.IsValid())
			{
				return ProtocolFailure(
					TEXT("openai_continuation_too_large"),
						 TEXT("The provider continuation exceeds the supported size."),
							  TEXT("OpenAI Responses could not retain a bounded opaque tool continuation."), OutError);
			}
		}

		if (!EmitMetadataIfPresent(
				Response, TEXT("status"), EUnrealAIModelMetadataKey::ResponseStatus, OutError) ||
				!EmitMetadataIfPresent(
					Response, TEXT("service_tier"), EUnrealAIModelMetadataKey::ServiceTier, OutError) ||
					!EmitMetadataIfPresent(Response, TEXT("model"), EUnrealAIModelMetadataKey::ModelRevision, OutError))
		{
			return false;
		}

		FUnrealAIModelUsageSnapshot Usage;
		if (!ParseFinalUsage(Response, Usage, OutError))
		{
			return false;
		}
		FUnrealAIModelEvent UsageEvent;
		PopulateEnvelope(UsageEvent);
		UsageEvent.Kind = EUnrealAIModelEventKind::UsageUpdated;
		UsageEvent.SequenceNumber = ++Sequence;
		UsageEvent.Usage = Usage;
		Sink(MoveTemp(UsageEvent));

		FUnrealAIModelEvent ModelEvent;
		PopulateEnvelope(ModelEvent);
		ModelEvent.Kind = EUnrealAIModelEventKind::Completed;
		ModelEvent.SequenceNumber = ++Sequence;
		ModelEvent.Continuation = MoveTemp(Continuation);
		bTerminal = true;
		Sink(MoveTemp(ModelEvent));
		return true;
	}

	bool EmitMetadataIfPresent(const TSharedPtr<FJsonObject> &Response, const TCHAR *FieldName,
							   const EUnrealAIModelMetadataKey Key, FString &OutError)
	{
		FString Value;
		if (!Response.IsValid() || !Response->HasField(FieldName))
		{
			return true;
		}
		if (!Response->TryGetStringField(FieldName, Value))
		{
			// Responses-compatible providers may expose a null optional field.
			// Null is absence; every normalized value remains a bounded string.
			const TSharedPtr<FJsonValue> Field = Response->TryGetField(FieldName);
			if (Field.IsValid() && Field->IsNull())
			{
				return true;
			}
			return ProtocolFailure(TEXT("openai_metadata_invalid"),
										TEXT("The provider returned invalid response metadata."),
											 TEXT("OpenAI Responses metadata was neither a bounded string nor null."),
												  OutError);
		}

		FUnrealAIModelMetadata Metadata;
		Metadata.Key = Key;
		Metadata.Value = MoveTemp(Value);
		FString ShapeError;
		if (!Metadata.ValidateShape(ShapeError))
		{
			return ProtocolFailure(TEXT("openai_metadata_invalid"),
				TEXT("The provider returned invalid response metadata."),
					 TEXT("OpenAI Responses metadata violated the normalized allowlist value contract."), OutError);
		}

		FUnrealAIModelEvent MetadataEvent;
		PopulateEnvelope(MetadataEvent);
		MetadataEvent.Kind = EUnrealAIModelEventKind::ProviderMetadata;
		MetadataEvent.SequenceNumber = ++Sequence;
		MetadataEvent.Metadata = MoveTemp(Metadata);
		Sink(MoveTemp(MetadataEvent));
		return true;
	}

	bool CaptureResponseId(const TSharedPtr<FJsonObject> &Response, FString &OutError)
	{
		FString Candidate;
		if (!Response.IsValid() || !Response->TryGetStringField(TEXT("id"), Candidate) ||
																!IsProviderOpaqueToken(Candidate))
		{
			return ProtocolFailure(TEXT("openai_response_id_invalid"),
				TEXT("The provider returned an invalid response identity."),
					 TEXT("OpenAI Responses response ID was missing, malformed, or exceeded its bound."), OutError);
		}
		if (!ResponseId.IsEmpty() && ResponseId != Candidate)
		{
			return ProtocolFailure(TEXT("openai_response_id_mismatch"),
										TEXT("The provider changed its response identity."),
											 TEXT("OpenAI Responses response ID changed within one admitted request."),
												  OutError);
		}
		ResponseId = MoveTemp(Candidate);
		return true;
	}

	bool ParseFinalUsage(const TSharedPtr<FJsonObject> &Response, FUnrealAIModelUsageSnapshot &OutUsage,
						 FString &OutError)
	{
		OutUsage = {};
		const TSharedPtr<FJsonObject> *UsageObject = nullptr;
		if (!Response->TryGetObjectField(
				TEXT("usage"), UsageObject) || UsageObject == nullptr || !UsageObject->IsValid() ||
				!ReadNonNegativeInteger(*UsageObject, TEXT("input_tokens"), OutUsage.InputTokens) ||
										!ReadNonNegativeInteger(*UsageObject, TEXT("output_tokens"),
																				   OutUsage.OutputTokens))
		{
			return ProtocolFailure(
				TEXT("openai_usage_invalid"),
					 TEXT("The provider returned invalid usage data."),
						  TEXT("OpenAI Responses completion omitted bounded integral input or output token usage."),
							   OutError);
		}

		if ((*UsageObject)->HasField(TEXT("total_tokens")))
		{
			if (!ReadNonNegativeInteger(*UsageObject, TEXT("total_tokens"), OutUsage.TotalTokens))
			{
				return ProtocolFailure(TEXT("openai_usage_invalid"),
					TEXT("The provider returned invalid usage data."),
						 TEXT("OpenAI Responses total token usage was malformed or exceeded its bound."), OutError);
			}
		}
		else
		{
			if (OutUsage.OutputTokens > FUnrealAIModelUsageSnapshot::MaxTokenCount - OutUsage.InputTokens)
			{
				return ProtocolFailure(TEXT("openai_usage_invalid"),
					TEXT("The provider returned invalid usage data."),
						 TEXT("OpenAI Responses token usage overflowed the normalized cumulative bound."), OutError);
			}
			OutUsage.TotalTokens = OutUsage.InputTokens + OutUsage.OutputTokens;
		}

		const auto ReadDetails = [this,
								  &OutError](const TSharedPtr<FJsonObject> &Usage, const TCHAR *Field,
											 const TFunctionRef<bool(const TSharedPtr<FJsonObject> &)> Reader) -> bool
		{
			if (!Usage->HasField(Field))
			{
				return true;
			}
			const TSharedPtr<FJsonObject> *Details = nullptr;
			if (!Usage->TryGetObjectField(Field, Details) || Details == nullptr || !Details->IsValid() ||
				!Reader(*Details))
			{
				return ProtocolFailure(
					TEXT("openai_usage_details_invalid"),
						 TEXT("The provider returned invalid usage details."),
							  TEXT("OpenAI Responses usage detail counters were malformed or exceeded their bounds."),
								   OutError);
			}
			return true;
		};
		if (!ReadDetails(*UsageObject,
						 TEXT("input_tokens_details"),
							  [&OutUsage](const TSharedPtr<FJsonObject> &Details)
							  {
								  if (Details->HasField(TEXT("cached_tokens")) &&
														!ReadNonNegativeInteger(
															Details, TEXT("cached_tokens"), OutUsage.CachedInputTokens))
								  {
									  return false;
								  }
								  if (Details->HasField(TEXT("audio_tokens")) &&
														!ReadNonNegativeInteger(
															Details, TEXT("audio_tokens"), OutUsage.AudioInputTokens))
								  {
									  return false;
								  }
								  return true;
							  }) ||
						 !ReadDetails(*UsageObject,
									  TEXT("output_tokens_details"),
										   [&OutUsage](const TSharedPtr<FJsonObject> &Details)
										   {
											   if (Details->HasField(TEXT("reasoning_tokens")) &&
																	 !ReadNonNegativeInteger(
																		 Details, TEXT("reasoning_tokens"),
																					   OutUsage.ReasoningOutputTokens))
											   {
												   return false;
											   }
											   if (Details->HasField(TEXT("audio_tokens")) &&
																	 !ReadNonNegativeInteger(
																		 Details, TEXT("audio_tokens"),
																					   OutUsage.AudioOutputTokens))
											   {
												   return false;
											   }
											   return true;
										   }))
		{
			return false;
		}

		OutUsage.bFinal = true;
		FString ShapeError;
		if (!OutUsage.ValidateShape(ShapeError))
		{
			return ProtocolFailure(
				TEXT("openai_usage_inconsistent"),
					 TEXT("The provider returned inconsistent usage data."),
						  TEXT("OpenAI Responses usage counters violated normalized cumulative invariants."), OutError);
		}
		return true;
	}

	bool ParseFunctionIdentity(const TSharedPtr<FJsonObject> &Item, FString &OutItemId, FString &OutCallId,
							   const FUnrealAIModelToolDescriptor *&OutTool, FString &OutArguments, FString &OutError)
	{
		FString Name;
		if (!Item.IsValid() ||
			!Item->TryGetStringField(
				TEXT("id"), OutItemId) ||
				!Item->TryGetStringField(
					TEXT("call_id"), OutCallId) ||
					!Item->TryGetStringField(
						TEXT("name"), Name) ||
						!Item->TryGetStringField(
							TEXT("arguments"), OutArguments) || !IsProviderOpaqueToken(OutItemId) ||
							!IsProviderOpaqueToken(OutCallId) ||
							!IsBoundedUtf8(OutArguments, FUnrealAIModelToolCall::MaxArgumentsJsonUtf8Bytes, true))
		{
			return ProtocolFailure(
				TEXT("openai_function_call_invalid"),
					 TEXT("The provider returned an invalid tool call."),
						  TEXT("OpenAI Responses function call omitted bounded item, call, name, or argument fields."),
							   OutError);
		}
		OutTool = Tools.Find(Name);
		if (OutTool == nullptr)
		{
			return ProtocolFailure(TEXT("openai_function_unknown"),
				TEXT("The provider requested an unavailable tool."),
					 TEXT("OpenAI Responses requested a function name absent from the explicit tool map."), OutError);
		}
		return true;
	}

	bool StartToolCallFromItem(const TSharedPtr<FJsonObject> &Item, const int32 OutputIndex, FString &OutError)
	{
		FString ItemId;
		FString CallId;
		FString Arguments;
		const FUnrealAIModelToolDescriptor *Tool = nullptr;
		if (!ParseFunctionIdentity(Item, ItemId, CallId, Tool, Arguments, OutError))
		{
			return false;
		}
		if (!Arguments.IsEmpty())
		{
			return ProtocolFailure(
				TEXT("openai_function_start_arguments_invalid"),
					 TEXT("The provider returned an invalid tool-call start."),
						  TEXT("OpenAI Responses function-call start unexpectedly contained assembled arguments."),
							   OutError);
		}
		return StartToolCall(OutputIndex, ItemId, CallId, *Tool, OutError);
	}

	bool StartToolCall(const int32 OutputIndex, const FString &ItemId, const FString &CallId,
					   const FUnrealAIModelToolDescriptor &Tool, FString &OutError)
	{
		EmitStarted();
		if (OutputIndex < LastStartedProviderToolOutputIndex)
		{
			return ProtocolFailure(TEXT("openai_function_start_order_invalid"),
				TEXT("The provider returned an out-of-order tool call."),
					 TEXT("OpenAI Responses function-call start moved backward in provider output order."), OutError);
		}
		if (ToolAssembliesByProviderOutputIndex.Contains(OutputIndex) ||
			ProviderToolOutputIndicesByItemId.Contains(ItemId) || ProviderToolOutputIndicesByCallId.Contains(CallId) ||
			ToolAssembliesByProviderOutputIndex.Num() >= FUnrealAIOpenAIResponsesStreamDecoder::MaxNormalizedToolCalls)
		{
			return ProtocolFailure(TEXT("openai_function_start_duplicate"),
				TEXT("The provider returned a duplicate or excessive tool call."),
					 TEXT("OpenAI Responses function-call start violated output, item, call, or count uniqueness."),
						  OutError);
		}
		FToolAssembly Assembly;
		Assembly.ProviderOutputIndex = OutputIndex;
		Assembly.NormalizedToolOutputIndex = ToolAssembliesByProviderOutputIndex.Num();
		Assembly.ItemId = ItemId;
		Assembly.CallId = CallId;
		Assembly.StableName = Tool.StableName;
		Assembly.Version = Tool.Version;
		const int32 NormalizedToolOutputIndex = Assembly.NormalizedToolOutputIndex;
		ToolAssembliesByProviderOutputIndex.Add(OutputIndex, MoveTemp(Assembly));
		ProviderToolOutputIndicesByItemId.Add(ItemId, OutputIndex);
		ProviderToolOutputIndicesByCallId.Add(CallId, OutputIndex);
		// Responses tool ordinals are defined by monotonically increasing raw
		// output_index arrival. Non-tool items may create gaps, but providers may
		// not reorder an earlier tool after a later tool has already been emitted.
		LastStartedProviderToolOutputIndex = OutputIndex;

		FUnrealAIModelEvent Started;
		PopulateEnvelope(Started);
		Started.Kind = EUnrealAIModelEventKind::ToolCallStarted;
		Started.SequenceNumber = ++Sequence;
		Started.ToolCallId = CallId;
		Started.ToolOutputIndex = NormalizedToolOutputIndex;
		Started.ToolStableName = Tool.StableName;
		Started.ToolVersion = Tool.Version;
		Sink(MoveTemp(Started));
		return true;
	}

	bool AppendToolArgumentsDelta(const TSharedPtr<FJsonObject> &Event, FString &OutError)
	{
		int32 OutputIndex = INDEX_NONE;
		FString ItemId;
		FString Delta;
		if (!ReadBoundedIndex(Event, TEXT("output_index"), OutputIndex) ||
							  !Event->TryGetStringField(
								  TEXT("item_id"), ItemId) || !IsProviderOpaqueToken(ItemId) ||
								  !Event->TryGetStringField(
									  TEXT("delta"), Delta) || Delta.IsEmpty() ||
									  !IsBoundedUtf8(Delta, FUnrealAIModelEvent::MaxToolArgumentsDeltaUtf8Bytes, false))
		{
			return ProtocolFailure(TEXT("openai_function_delta_invalid"),
				TEXT("The provider returned invalid tool arguments."),
					 TEXT("OpenAI Responses function-argument delta omitted bounded item, index, or delta fields."),
						  OutError);
		}
		FToolAssembly *Assembly = ToolAssembliesByProviderOutputIndex.Find(OutputIndex);
		const int64 DeltaBytes = Utf8Length(Delta);
		if (Assembly == nullptr || Assembly->ProviderOutputIndex != OutputIndex || Assembly->ItemId != ItemId ||
			Assembly->bCompleted ||
			DeltaBytes > FUnrealAIModelProfileProjection::MaxToolArgumentUtf8Bytes - Utf8Length(Assembly->Arguments) ||
			DeltaBytes >
				FUnrealAIModelProfileProjection::MaxAggregateToolArgumentUtf8Bytes - AggregateToolArgumentBytes)
		{
			return ProtocolFailure(TEXT("openai_function_delta_lifecycle_invalid"),
				TEXT("The provider returned invalid tool arguments."),
					 TEXT("OpenAI Responses function-argument delta violated open-call identity or cumulative bounds."),
						  OutError);
		}
		Assembly->Arguments += Delta;
		AggregateToolArgumentBytes += DeltaBytes;

		FUnrealAIModelEvent DeltaEvent;
		PopulateEnvelope(DeltaEvent);
		DeltaEvent.Kind = EUnrealAIModelEventKind::ToolCallArgumentsDelta;
		DeltaEvent.SequenceNumber = ++Sequence;
		DeltaEvent.ToolCallId = Assembly->CallId;
		DeltaEvent.ToolOutputIndex = Assembly->NormalizedToolOutputIndex;
		DeltaEvent.ToolArgumentsDelta = MoveTemp(Delta);
		Sink(MoveTemp(DeltaEvent));
		return true;
	}

	bool CompleteToolArguments(const TSharedPtr<FJsonObject> &Event, FString &OutError)
	{
		int32 OutputIndex = INDEX_NONE;
		FString ItemId;
		FString Arguments;
		if (!ReadBoundedIndex(Event, TEXT("output_index"), OutputIndex) ||
							  !Event->TryGetStringField(TEXT("item_id"), ItemId) || !IsProviderOpaqueToken(ItemId) ||
														!Event->TryGetStringField(TEXT("arguments"), Arguments))
		{
			return ProtocolFailure(
				TEXT("openai_function_done_invalid"),
					 TEXT("The provider returned invalid tool arguments."),
						  TEXT("OpenAI Responses function-argument completion omitted item, index, or arguments."),
							   OutError);
		}
		FToolAssembly *Assembly = ToolAssembliesByProviderOutputIndex.Find(OutputIndex);
		if (Assembly == nullptr || Assembly->ProviderOutputIndex != OutputIndex || Assembly->ItemId != ItemId)
		{
			return ProtocolFailure(
				TEXT("openai_function_done_lifecycle_invalid"),
					 TEXT("The provider returned an invalid tool-call completion."),
						  TEXT("OpenAI Responses function-argument completion did not match an open call."), OutError);
		}
		return CompleteToolCall(*Assembly, Arguments, OutError);
	}

	bool ReconcileFunctionCall(const TSharedPtr<FJsonObject> &Item, const int32 OutputIndex, FString &OutError)
	{
		FString ItemId;
		FString CallId;
		FString Arguments;
		const FUnrealAIModelToolDescriptor *Tool = nullptr;
		if (!ParseFunctionIdentity(Item, ItemId, CallId, Tool, Arguments, OutError))
		{
			return false;
		}
		FToolAssembly *Assembly = ToolAssembliesByProviderOutputIndex.Find(OutputIndex);
		if (Assembly == nullptr)
		{
			if (!StartToolCall(OutputIndex, ItemId, CallId, *Tool, OutError))
			{
				return false;
			}
			Assembly = ToolAssembliesByProviderOutputIndex.Find(OutputIndex);
		}
		if (Assembly == nullptr || Assembly->ProviderOutputIndex != OutputIndex || Assembly->ItemId != ItemId ||
			Assembly->CallId != CallId || Assembly->StableName != Tool->StableName ||
			Assembly->Version != Tool->Version)
		{
			return ProtocolFailure(
				TEXT("openai_function_reconciliation_invalid"),
					 TEXT("The provider changed a tool-call identity."),
						  TEXT("OpenAI Responses full function item did not match its streamed start."), OutError);
		}
		return CompleteToolCall(*Assembly, Arguments, OutError);
	}

	bool CompleteToolCall(FToolAssembly &Assembly, const FString &Arguments, FString &OutError)
	{
		FUnrealAIModelToolCall Call;
		Call.ProviderCallId = Assembly.CallId;
		Call.StableName = Assembly.StableName;
		Call.Version = Assembly.Version;
		Call.ArgumentsJson = Arguments;
		FString ShapeError;
		if (!Call.ValidateShape(ShapeError))
		{
			return ProtocolFailure(
				TEXT("openai_function_arguments_invalid"),
					 TEXT("The provider returned invalid tool arguments."),
						  TEXT("OpenAI Responses function arguments failed bounded JSON-object validation."), OutError);
		}
		if (Assembly.bCompleted)
		{
			if (Assembly.FinalArguments != Arguments)
			{
				return ProtocolFailure(TEXT("openai_function_completion_mismatch"),
					TEXT("The provider changed completed tool arguments."),
						 TEXT("OpenAI Responses repeated a function completion with different arguments."), OutError);
			}
			return true;
		}
		if (!Assembly.Arguments.IsEmpty() && Assembly.Arguments != Arguments)
		{
			return ProtocolFailure(
				TEXT("openai_function_assembly_mismatch"),
					 TEXT("The provider returned inconsistent tool arguments."),
						  TEXT("OpenAI Responses assembled function arguments did not equal the streamed fragments."),
							   OutError);
		}
		if (Assembly.Arguments.IsEmpty())
		{
			const int64 ArgumentBytes = Utf8Length(Arguments);
			if (ArgumentBytes > FUnrealAIModelProfileProjection::MaxToolArgumentUtf8Bytes ||
				ArgumentBytes >
					FUnrealAIModelProfileProjection::MaxAggregateToolArgumentUtf8Bytes - AggregateToolArgumentBytes)
			{
				return ProtocolFailure(TEXT("openai_function_arguments_too_large"),
					TEXT("The provider returned oversized tool arguments."),
						 TEXT("OpenAI Responses function arguments exceeded normalized cumulative bounds."), OutError);
			}
			AggregateToolArgumentBytes += ArgumentBytes;
		}

		Assembly.bCompleted = true;
		Assembly.FinalArguments = Arguments;
		++CompletedToolCalls;

		FUnrealAIModelEvent Completed;
		PopulateEnvelope(Completed);
		Completed.Kind = EUnrealAIModelEventKind::ToolCallCompleted;
		Completed.SequenceNumber = ++Sequence;
		Completed.ToolOutputIndex = Assembly.NormalizedToolOutputIndex;
		Completed.ToolCall = MoveTemp(Call);
		Sink(MoveTemp(Completed));
		return true;
	}

	void PopulateEnvelope(FUnrealAIModelEvent &Event) const
	{
		Event.RequestId = RequestId;
		if (!ResponseId.IsEmpty())
		{
			FUnrealAIModelProviderRequestId Id;
			Id.Kind = EUnrealAIModelProviderRequestIdKind::Response;
			Id.Value = ResponseId;
			Event.ProviderRequestIds.Add(MoveTemp(Id));
		}
	}

	void EmitStarted()
	{
		if (bStarted || bTerminal)
		{
			return;
		}
		bStarted = true;
		FUnrealAIModelEvent Event;
		PopulateEnvelope(Event);
		Event.Kind = EUnrealAIModelEventKind::Started;
		Event.SequenceNumber = ++Sequence;
		Sink(MoveTemp(Event));
	}

	bool ProviderFailure(const FName Code, const bool bRetryable, FString &OutError)
	{
		FUnrealAIModelError PublicError =
			MakeOpenAIError(EUnrealAIErrorCategory::Provider, Code,
							TEXT("The model provider could not complete the request."),
								 TEXT("OpenAI Responses emitted a terminal failure event."), bRetryable);
		FaultPolicy.ApplyTo(PublicError);
		OutError = PublicError.DiagnosticMessage;
		if (!bTerminal)
		{
			EmitStarted();
			FUnrealAIModelEvent Event;
			PopulateEnvelope(Event);
			Event.Kind = EUnrealAIModelEventKind::Failed;
			Event.SequenceNumber = ++Sequence;
			Event.Error = MoveTemp(PublicError);
			bTerminal = true;
			bFailed = true;
			Sink(MoveTemp(Event));
		}
		return false;
	}

	bool ProtocolFailure(const FName Code, const TCHAR *UserMessage, const TCHAR *Diagnostic, FString &OutError)
	{
		FUnrealAIModelError PublicError =
			MakeOpenAIError(EUnrealAIErrorCategory::ProviderProtocol, Code, UserMessage, Diagnostic);
		FaultPolicy.ApplyTo(PublicError);
		OutError = PublicError.DiagnosticMessage;
		if (!bTerminal)
		{
			EmitStarted();
			FUnrealAIModelEvent Event;
			PopulateEnvelope(Event);
			Event.Kind = EUnrealAIModelEventKind::Failed;
			Event.SequenceNumber = ++Sequence;
			Event.Error = MoveTemp(PublicError);
			bTerminal = true;
			bFailed = true;
			Sink(MoveTemp(Event));
		}
		return false;
	}

	FName ProviderName;
	FUnrealAIOpenAIResponsesPublicFaultPolicy FaultPolicy;
	FUnrealAIRequestId RequestId;
	FString ConnectionBinding;
	FName ConnectionAlias;
	FString ModelId;
	TMap<FString, FUnrealAIModelToolDescriptor> Tools;
	FEventSink Sink;
	FIgnoredEventObserver IgnoredEventObserver;
	TArray<uint8> OriginalHistory;
	TArray<uint8> CurrentLine;
	TArray<uint8> Data;
	TMap<int32, TSharedPtr<FJsonValue>> OutputItems;
	/** Tool assemblies remain keyed by the provider's raw output_index for exact wire reconciliation. */
	TMap<int32, FToolAssembly> ToolAssembliesByProviderOutputIndex;
	TMap<FString, int32> ProviderToolOutputIndicesByItemId;
	TMap<FString, int32> ProviderToolOutputIndicesByCallId;
	TMap<int64, FTextAssembly> TextAssemblies;
	FString ResponseId;
	FString AccumulatedText;
	int64 RetainedResponseStateBytes = 0;
	int64 AssembledTextUtf8Bytes = 0;
	int64 AccumulatedTextUtf8Bytes = 0;
	int64 CompletedTextUtf8Bytes = 0;
	int64 AggregateToolArgumentBytes = 0;
	int32 CompletedToolCalls = 0;
	int32 LastStartedProviderToolOutputIndex = INDEX_NONE;
	int32 CompletedTextPartCount = 0;
	int32 RetainedCompletedMessagePartCount = 0;
	int32 TotalBytes = 0;
	int64 Sequence = 0;
	int64 LastProviderSequence = -1;
	bool bIgnoreLfAfterCr = false;
	bool bStarted = false;
	bool bTerminal = false;
	bool bFailed = false;
	bool bOriginalHistoryValid = false;
	bool bStructuredOutput = false;
};

FUnrealAIOpenAIResponsesStreamDecoder::FUnrealAIOpenAIResponsesStreamDecoder(
	const FUnrealAIModelRequest &Request, FEventSink InSink, FIgnoredEventObserver InIgnoredEventObserver)
	: Impl(MakeUnique<FImpl>(OpenAIResponsesProtocolProviderName, FUnrealAIOpenAIResponsesPublicFaultPolicy::OpenAI(),
							 Request, MoveTemp(InSink), MoveTemp(InIgnoredEventObserver)))
{
}

FUnrealAIOpenAIResponsesStreamDecoder::FUnrealAIOpenAIResponsesStreamDecoder(
	const FName ProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
	const FUnrealAIModelRequest &Request, FEventSink InSink, FIgnoredEventObserver InIgnoredEventObserver)
	: Impl(MakeUnique<FImpl>(ProviderName, FaultPolicy, Request, MoveTemp(InSink), MoveTemp(InIgnoredEventObserver)))
{
}

FUnrealAIOpenAIResponsesStreamDecoder::~FUnrealAIOpenAIResponsesStreamDecoder() = default;

bool FUnrealAIOpenAIResponsesStreamDecoder::PushBytes(const TConstArrayView<uint8> Bytes, FString &OutError)
{
	return Impl.IsValid() && Impl->Push(Bytes, OutError);
}

bool FUnrealAIOpenAIResponsesStreamDecoder::Finish(FString &OutError)
{
	return Impl.IsValid() && Impl->Finish(OutError);
}

void FUnrealAIOpenAIResponsesStreamDecoder::Cancel()
{
	if (Impl.IsValid())
	{
		Impl->Cancel();
	}
}

bool FUnrealAIOpenAIResponsesStreamDecoder::IsTerminal() const
{
	return Impl.IsValid() && Impl->IsTerminal();
}
