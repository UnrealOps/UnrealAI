// Copyright UnrealOps. All Rights Reserved.

#include "OpenAICompatible/UnrealAIOpenAICompatibleChatProtocol.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
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

bool ParseObject(const TConstArrayView<uint8> Bytes, const int32 MaximumBytes, TSharedPtr<FJsonObject> &OutObject)
{
	OutObject.Reset();
	if (Bytes.IsEmpty() || Bytes.Num() > MaximumBytes || !IsValidUtf8(Bytes))
	{
		return false;
	}
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
	if (Text.Length() <= 0)
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get()));
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool ParseObject(const FString &Text, TSharedPtr<FJsonObject> &OutObject)
{
	OutObject.Reset();
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool SerializeObject(const TSharedRef<FJsonObject> &Object, TArray<uint8> &OutBytes, const int32 MaximumBytes)
{
	OutBytes.Reset();
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() <= 0 || Utf8.Length() > MaximumBytes)
	{
		return false;
	}
	OutBytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	return true;
}

bool FitsUtf8(const FString &Text, const int32 MaximumBytes, const bool bAllowEmpty = false)
{
	if ((!bAllowEmpty && Text.IsEmpty()) || Text.Contains(TEXT("\r")) || Text.Contains(TEXT("\n")))
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	return Utf8.Length() <= MaximumBytes;
}

TSharedRef<FJsonObject> MakeMessage(const FString &Role, const FString &Content)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), Role);
	Message->SetStringField(TEXT("content"), Content);
	return Message;
}

bool CopyMessageContent(const TSharedPtr<FJsonObject> &Source, FString &OutContent)
{
	OutContent.Reset();
	if (Source->TryGetStringField(TEXT("content"), OutContent))
	{
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>> *Parts = nullptr;
	if (!Source->TryGetArrayField(TEXT("content"), Parts) || Parts == nullptr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue> &PartValue : *Parts)
	{
		const TSharedPtr<FJsonObject> *Part = nullptr;
		FString Text;
		if (!PartValue.IsValid() || !PartValue->TryGetObject(Part) || Part == nullptr || !Part->IsValid() ||
			(!(*Part)->TryGetStringField(TEXT("text"), Text) && !(*Part)->TryGetStringField(TEXT("refusal"), Text)))
		{
			return false;
		}
		if (!OutContent.IsEmpty())
		{
			OutContent.AppendChar(TEXT('\n'));
		}
		OutContent += Text;
	}
	return true;
}

bool LowerResponsesObject(const TSharedPtr<FJsonObject> &Responses, TSharedRef<FJsonObject> &OutChat, FString &OutError)
{
	OutError.Reset();
	FString Model;
	const TArray<TSharedPtr<FJsonValue>> *Input = nullptr;
	if (!Responses.IsValid() ||
		!Responses->TryGetStringField(TEXT("model"), Model) ||
									  !FitsUtf8(Model, FUnrealAIModelRequest::MaxModelIdUtf8Bytes) ||
									  !Responses->TryGetArrayField(TEXT("input"), Input) || Input == nullptr)
	{
		OutError = TEXT("Compatible request lowering requires a bounded model and input array.");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	FString Instructions;
	if (Responses->TryGetStringField(TEXT("instructions"), Instructions) && !Instructions.IsEmpty())
	{
		Messages.Add(MakeShared<FJsonValueObject>(MakeMessage(TEXT("system"), Instructions)));
	}

	TArray<TSharedPtr<FJsonValue>> PendingToolCalls;
	const auto FlushPendingTools = [&Messages, &PendingToolCalls]()
	{
		if (PendingToolCalls.IsEmpty())
		{
			return;
		}
		TSharedRef<FJsonObject> Assistant = MakeMessage(TEXT("assistant"), TEXT(""));
		Assistant->SetArrayField(TEXT("tool_calls"), MoveTemp(PendingToolCalls));
		Messages.Add(MakeShared<FJsonValueObject>(Assistant));
		PendingToolCalls.Reset();
	};

	for (const TSharedPtr<FJsonValue> &Value : *Input)
	{
		const TSharedPtr<FJsonObject> *ObjectPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || ObjectPtr == nullptr || !ObjectPtr->IsValid())
		{
			OutError = TEXT("Compatible request input contains a non-object item.");
			return false;
		}
		const TSharedPtr<FJsonObject> &Object = *ObjectPtr;
		FString Type;
		Object->TryGetStringField(TEXT("type"), Type);
		FString Role;
		if (Type.IsEmpty() && Object->TryGetStringField(TEXT("role"), Role))
		{
			FString Content;
			if (!CopyMessageContent(Object, Content))
			{
				OutError = TEXT("Compatible request message content is unsupported.");
				return false;
			}
			FlushPendingTools();
			Messages.Add(MakeShared<FJsonValueObject>(MakeMessage(Role, Content)));
		}
		else if (Type == TEXT("message"))
		{
			FString Content;
			if (!Object->TryGetStringField(TEXT("role"), Role) || !CopyMessageContent(Object, Content))
			{
				OutError = TEXT("Compatible continuation message is malformed.");
				return false;
			}
			FlushPendingTools();
			Messages.Add(MakeShared<FJsonValueObject>(MakeMessage(Role, Content)));
		}
		else if (Type == TEXT("function_call"))
		{
			FString CallId;
			FString Name;
			FString Arguments;
			if (!Object->TryGetStringField(
					TEXT("call_id"), CallId) ||
					!Object->TryGetStringField(
						TEXT("name"), Name) ||
						!Object->TryGetStringField(TEXT("arguments"), Arguments) ||
												   PendingToolCalls.Num() >=
													   FUnrealAIOpenAICompatibleChatStreamDecoder::MaxToolCalls)
			{
				OutError = TEXT("Compatible continuation tool call is malformed or exceeds its bound.");
				return false;
			}
			TSharedRef<FJsonObject> Function = MakeShared<FJsonObject>();
			Function->SetStringField(TEXT("name"), Name);
			Function->SetStringField(TEXT("arguments"), Arguments);
			TSharedRef<FJsonObject> ToolCall = MakeShared<FJsonObject>();
			ToolCall->SetStringField(TEXT("id"), CallId);
			ToolCall->SetStringField(TEXT("type"), TEXT("function"));
			ToolCall->SetObjectField(TEXT("function"), Function);
			PendingToolCalls.Add(MakeShared<FJsonValueObject>(ToolCall));
		}
		else if (Type == TEXT("function_call_output"))
		{
			FString CallId;
			FString Output;
			if (!Object->TryGetStringField(TEXT("call_id"), CallId) ||
										   !Object->TryGetStringField(TEXT("output"), Output))
			{
				OutError = TEXT("Compatible continuation tool output is malformed.");
				return false;
			}
			FlushPendingTools();
			TSharedRef<FJsonObject> ToolMessage = MakeMessage(TEXT("tool"), Output);
			ToolMessage->SetStringField(TEXT("tool_call_id"), CallId);
			Messages.Add(MakeShared<FJsonValueObject>(ToolMessage));
		}
		else if (Type != TEXT("reasoning"))
		{
			OutError = TEXT("Compatible request contains an unsupported input item.");
			return false;
		}
	}
	FlushPendingTools();
	if (Messages.IsEmpty())
	{
		OutError = TEXT("Compatible request lowering produced no messages.");
		return false;
	}

	OutChat = MakeShared<FJsonObject>();
	OutChat->SetStringField(TEXT("model"), Model);
	OutChat->SetArrayField(TEXT("messages"), MoveTemp(Messages));
	OutChat->SetBoolField(TEXT("stream"), true);
	TSharedRef<FJsonObject> StreamOptions = MakeShared<FJsonObject>();
	StreamOptions->SetBoolField(TEXT("include_usage"), true);
	OutChat->SetObjectField(TEXT("stream_options"), StreamOptions);

	double MaximumTokens = 0.0;
	if (!Responses->TryGetNumberField(TEXT("max_output_tokens"), MaximumTokens) || !FMath::IsFinite(MaximumTokens) ||
									  MaximumTokens < 1.0 ||
									  MaximumTokens > FUnrealAIModelRequest::MaxOutputTokensLimit ||
									  FMath::FloorToDouble(MaximumTokens) != MaximumTokens)
	{
		OutError = TEXT("Compatible request has an invalid output-token limit.");
		return false;
	}
	OutChat->SetNumberField(TEXT("max_tokens"), MaximumTokens);

	const TArray<TSharedPtr<FJsonValue>> *ResponseTools = nullptr;
	if (Responses->TryGetArrayField(TEXT("tools"), ResponseTools) && ResponseTools != nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> ChatTools;
		ChatTools.Reserve(ResponseTools->Num());
		for (const TSharedPtr<FJsonValue> &ToolValue : *ResponseTools)
		{
			const TSharedPtr<FJsonObject> *Tool = nullptr;
			if (!ToolValue.IsValid() || !ToolValue->TryGetObject(Tool) || Tool == nullptr || !Tool->IsValid())
			{
				OutError = TEXT("Compatible request contains a malformed tool.");
				return false;
			}
			TSharedRef<FJsonObject> Function = MakeShared<FJsonObject>();
			FString Name;
			FString Description;
			const TSharedPtr<FJsonObject> *Parameters = nullptr;
			if (!(*Tool)->TryGetStringField(
					TEXT("name"), Name) ||
					!(*Tool)->TryGetStringField(TEXT("description"), Description) ||
												!(*Tool)->TryGetObjectField(TEXT("parameters"), Parameters) ||
																			Parameters == nullptr ||
																			!Parameters->IsValid())
			{
				OutError = TEXT("Compatible request tool declaration is malformed.");
				return false;
			}
			Function->SetStringField(TEXT("name"), Name);
			Function->SetStringField(TEXT("description"), Description);
			Function->SetObjectField(TEXT("parameters"), *Parameters);
			bool bStrict = false;
			if ((*Tool)->TryGetBoolField(TEXT("strict"), bStrict))
			{
				Function->SetBoolField(TEXT("strict"), bStrict);
			}
			TSharedRef<FJsonObject> ChatTool = MakeShared<FJsonObject>();
			ChatTool->SetStringField(TEXT("type"), TEXT("function"));
			ChatTool->SetObjectField(TEXT("function"), Function);
			ChatTools.Add(MakeShared<FJsonValueObject>(ChatTool));
		}
		OutChat->SetArrayField(TEXT("tools"), MoveTemp(ChatTools));
		OutChat->SetBoolField(TEXT("parallel_tool_calls"), false);
	}
	return true;
}

bool ReadInteger(const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, int64 &OutValue)
{
	double Value = -1.0;
	if (!Object.IsValid() || !Object->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value) || Value < 0.0 ||
		Value > FUnrealAIModelUsageSnapshot::MaxTokenCount || FMath::FloorToDouble(Value) != Value)
	{
		return false;
	}
	OutValue = static_cast<int64>(Value);
	return true;
}

TArray<uint8> MakeCanonicalSse(const TSharedRef<FJsonObject> &Object)
{
	TArray<uint8> JsonBytes;
	if (!SerializeObject(Object, JsonBytes, FUnrealAIOpenAICompatibleChatStreamDecoder::MaxEventBytes))
	{
		return {};
	}
	TArray<uint8> Result;
	constexpr ANSICHAR Prefix[] = "data: ";
	constexpr ANSICHAR Suffix[] = "\n\n";
	Result.Append(reinterpret_cast<const uint8 *>(Prefix), UE_ARRAY_COUNT(Prefix) - 1);
	Result.Append(JsonBytes);
	Result.Append(reinterpret_cast<const uint8 *>(Suffix), UE_ARRAY_COUNT(Suffix) - 1);
	return Result;
}
} // namespace

bool FUnrealAIOpenAICompatibleChatWireRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	TSharedPtr<FJsonObject> Object;
	FString SerializedModel;
	if (!FitsUtf8(ModelId, FUnrealAIModelRequest::MaxModelIdUtf8Bytes) ||
		!ParseObject(BodyUtf8, MaxBodyBytes, Object) ||
		!Object->TryGetStringField(TEXT("model"), SerializedModel) || SerializedModel != ModelId)
	{
		OutError =
			TEXT("Compatible Chat Completions request is empty, oversized, malformed, or changed its exact model.");
		return false;
	}
	return true;
}

bool FUnrealAIOpenAICompatibleChatRequestBuilder::LowerResponsesWire(
	const TConstArrayView<uint8> ResponsesBody, FUnrealAIOpenAICompatibleChatWireRequest &OutWireRequest,
	FString &OutError)
{
	OutWireRequest.BodyUtf8.Reset();
	OutWireRequest.ModelId.Reset();
	OutError.Reset();
	TSharedPtr<FJsonObject> Responses;
	if (!ParseObject(ResponsesBody, FUnrealAIOpenAIResponsesWireRequest::MaxBodyBytes, Responses))
	{
		OutError = TEXT("Compatible request lowering received malformed staged wire data.");
		return false;
	}
	TSharedRef<FJsonObject> Chat = MakeShared<FJsonObject>();
	if (!LowerResponsesObject(Responses, Chat, OutError) ||
		!Chat->TryGetStringField(TEXT("model"), OutWireRequest.ModelId) ||
								 !FitsUtf8(OutWireRequest.ModelId, FUnrealAIModelRequest::MaxModelIdUtf8Bytes) ||
								 !SerializeObject(Chat, OutWireRequest.BodyUtf8,
												  FUnrealAIOpenAICompatibleChatWireRequest::MaxBodyBytes))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Compatible Chat Completions request exceeded its serialization bound.");
		}
		OutWireRequest.BodyUtf8.Reset();
		OutWireRequest.ModelId.Reset();
		return false;
	}
	return true;
}

class FUnrealAIOpenAICompatibleChatStreamDecoder::FImpl final
{
  public:
	enum class EFinishReason : uint8
	{
		None,
		Stop,
		ToolCalls,
		Length
	};

	struct FToolAssembly final
	{
		int32 Index = INDEX_NONE;
		FString CallId;
		FString Name;
		FString Arguments;
		int32 ArgumentUtf8Bytes = 0;
		bool bAdded = false;
	};

	FImpl(FString InExpectedModelId, FCanonicalChunkSink InSink)
		: ExpectedModelId(MoveTemp(InExpectedModelId)), Sink(MoveTemp(InSink))
	{
	}

	bool Push(const TConstArrayView<uint8> Bytes, FString &OutError)
	{
		OutError.Reset();
		if (bTerminal || bFailed || Bytes.IsEmpty() || Bytes.Num() > MaxStreamBytes ||
			RetainedStreamBytes > MaxStreamBytes - Bytes.Num())
		{
			OutError = TEXT("Compatible chat stream is terminal, empty, or exceeds its bound.");
			bFailed = true;
			return false;
		}
		RetainedStreamBytes += Bytes.Num();
		Pending.Append(Bytes.GetData(), Bytes.Num());

		int32 Cursor = 0;
		for (;;)
		{
			int32 Boundary = INDEX_NONE;
			int32 DelimiterBytes = 0;
			for (int32 Index = Cursor; Index + 1 < Pending.Num(); ++Index)
			{
				if (Pending[Index] == '\n' && Pending[Index + 1] == '\n')
				{
					Boundary = Index;
					DelimiterBytes = 2;
					break;
				}
				if (Index + 3 < Pending.Num() && Pending[Index] == '\r' && Pending[Index + 1] == '\n' &&
					Pending[Index + 2] == '\r' && Pending[Index + 3] == '\n')
				{
					Boundary = Index;
					DelimiterBytes = 4;
					break;
				}
			}
			if (Boundary == INDEX_NONE)
			{
				const int32 IncompleteBytes = Pending.Num() - Cursor;
				if (IncompleteBytes > MaxEventBytes)
				{
					OutError = TEXT("Compatible chat stream retained an oversized incomplete event.");
					bFailed = true;
					return false;
				}
				// Compact once per pushed native chunk, never once per SSE event.
				if (Cursor > 0)
				{
					Pending.RemoveAt(0, Cursor, EAllowShrinking::No);
				}
				return true;
			}
			const int32 EventLength = Boundary - Cursor;
			if (EventLength > MaxEventBytes)
			{
				OutError = TEXT("Compatible chat stream contains an oversized event.");
				bFailed = true;
				return false;
			}
			if (WireEventCount >= MaxWireEvents)
			{
				OutError = TEXT("Compatible chat stream exceeded its bounded event count.");
				bFailed = true;
				return false;
			}
			++WireEventCount;
			const TConstArrayView<uint8> EventBytes(Pending.GetData() + Cursor, EventLength);
			Cursor = Boundary + DelimiterBytes;
			if (!ProcessEvent(EventBytes, OutError))
			{
				bFailed = true;
				return false;
			}
			if (bTerminal)
			{
				if (Cursor != Pending.Num())
				{
					OutError = TEXT("Compatible chat stream retained bytes after its terminal marker.");
					bFailed = true;
					return false;
				}
				Pending.Reset();
				return true;
			}
		}
	}

	bool Finish(FString &OutError)
	{
		OutError.Reset();
		if (bFailed)
		{
			OutError = TEXT("Compatible chat stream previously failed validation.");
			return false;
		}
		if (bTerminal)
		{
			return true;
		}
		if (!Pending.IsEmpty())
		{
			OutError = TEXT("Compatible chat stream ended with a truncated SSE event.");
			bFailed = true;
			return false;
		}
		OutError = TEXT("Compatible chat stream ended without a terminal marker.");
		bFailed = true;
		return false;
	}

	bool IsTerminal() const
	{
		return bTerminal;
	}

  private:
	bool ProcessEvent(const TConstArrayView<uint8> EventBytes, FString &OutError)
	{
		if (EventBytes.IsEmpty())
		{
			return true;
		}
		if (!IsValidUtf8(EventBytes))
		{
			OutError = TEXT("Compatible chat stream contains invalid UTF-8.");
			return false;
		}
		FUTF8ToTCHAR EventText(reinterpret_cast<const ANSICHAR *>(EventBytes.GetData()), EventBytes.Num());
		if (EventText.Length() <= 0)
		{
			OutError = TEXT("Compatible chat stream contains invalid UTF-8.");
			return false;
		}
		TArray<FString> Lines;
		FString(EventText.Length(), EventText.Get()).ParseIntoArrayLines(Lines, false);
		FString Data;
		for (FString &Line : Lines)
		{
			Line.RemoveFromEnd(TEXT("\r"));
			if (Line.StartsWith(TEXT(":")))
			{
				continue;
			}
			if (!Line.StartsWith(TEXT("data:")))
			{
				OutError = TEXT("Compatible chat stream contains an unsupported SSE field.");
				return false;
			}
			FString Part = Line.RightChop(5);
			Part.RemoveFromStart(TEXT(" "));
			if (!Data.IsEmpty())
			{
				Data.AppendChar(TEXT('\n'));
			}
			Data += Part;
		}
		if (Data.IsEmpty())
		{
			return true;
		}
		if (Data == TEXT("[DONE]"))
		{
			return Complete(OutError);
		}
		TSharedPtr<FJsonObject> Chunk;
		if (!ParseObject(Data, Chunk))
		{
			OutError = TEXT("Compatible chat stream contains malformed JSON.");
			return false;
		}
		return ProcessChunk(Chunk, OutError);
	}

	bool ProcessChunk(const TSharedPtr<FJsonObject> &Chunk, FString &OutError)
	{
		if (bUsageObserved)
		{
			OutError = TEXT("Compatible chat stream emitted a chunk after final usage.");
			return false;
		}

		FString ChunkId;
		FString ChunkModel;
		if (!Chunk->TryGetStringField(TEXT("id"), ChunkId) || !FitsUtf8(ChunkId, 1024) ||
									  !Chunk->TryGetStringField(TEXT("model"), ChunkModel) ||
																ChunkModel != ExpectedModelId)
		{
			OutError = TEXT("Compatible chat chunk has an invalid response or model identity.");
			return false;
		}
		if (Chunk->HasField(TEXT("object")))
		{
			FString ObjectType;
			if (!Chunk->TryGetStringField(TEXT("object"), ObjectType) || ObjectType != TEXT("chat.completion.chunk"))
			{
				OutError = TEXT("Compatible chat chunk has an invalid object type.");
				return false;
			}
		}
		if (ResponseId.IsEmpty())
		{
			ResponseId = ChunkId;
			if (!EmitCreated())
			{
				OutError = TEXT("Compatible chat response-created event exceeded its bound.");
				return false;
			}
		}
		else if (ChunkId != ResponseId)
		{
			OutError = TEXT("Compatible chat stream changed response identity.");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>> *Choices = nullptr;
		if (!Chunk->TryGetArrayField(TEXT("choices"), Choices) || Choices == nullptr || Choices->Num() > 1)
		{
			OutError = TEXT("Compatible chat chunk requires at most one choices entry.");
			return false;
		}
		if (Choices->Num() == 1)
		{
			if (bFinishObserved)
			{
				OutError = TEXT("Compatible chat stream emitted a choice after its finish reason.");
				return false;
			}
			const TSharedPtr<FJsonObject> *Choice = nullptr;
			double ChoiceIndex = -1.0;
			if (!(*Choices)[0]->TryGetObject(Choice) || Choice == nullptr || !Choice->IsValid() ||
				!(*Choice)->TryGetNumberField(TEXT("index"), ChoiceIndex) || ChoiceIndex != 0.0)
			{
				OutError = TEXT("Compatible chat chunk contains an invalid choice.");
				return false;
			}
			const TSharedPtr<FJsonObject> *Delta = nullptr;
			if (!(*Choice)->TryGetObjectField(TEXT("delta"), Delta) || Delta == nullptr || !Delta->IsValid() ||
											  !ProcessDelta(*Delta, OutError))
			{
				return false;
			}
			if ((*Choice)->HasField(TEXT("finish_reason")))
			{
				const TSharedPtr<FJsonValue> FinishValue = (*Choice)->TryGetField(TEXT("finish_reason"));
				if (!FinishValue.IsValid())
				{
					OutError = TEXT("Compatible chat stream contains an invalid finish reason.");
					return false;
				}
				if (FinishValue->Type == EJson::String)
				{
					const FString FinishReason = FinishValue->AsString();
					if (FinishReason == TEXT("stop"))
					{
						ObservedFinishReason = EFinishReason::Stop;
					}
					else if (FinishReason == TEXT("tool_calls"))
					{
						ObservedFinishReason = EFinishReason::ToolCalls;
					}
					else if (FinishReason == TEXT("length"))
					{
						ObservedFinishReason = EFinishReason::Length;
					}
					else
					{
						OutError = TEXT("Compatible chat stream contains an unsupported finish reason.");
						return false;
					}
					bFinishObserved = true;
				}
				else if (FinishValue->Type != EJson::Null)
				{
					OutError = TEXT("Compatible chat stream contains a non-string finish reason.");
					return false;
				}
			}
		}

		const TSharedPtr<FJsonValue> UsageValue = Chunk->TryGetField(TEXT("usage"));
		if (UsageValue.IsValid() && UsageValue->Type != EJson::Null)
		{
			if (UsageValue->Type != EJson::Object || Choices->Num() != 0 || !bFinishObserved)
			{
				OutError = TEXT("Compatible chat stream emitted final usage before an isolated finish.");
				return false;
			}
			const TSharedPtr<FJsonObject> UsageObject = UsageValue->AsObject();
			if (!UsageObject.IsValid() ||
				!ReadInteger(UsageObject, TEXT("prompt_tokens"), PromptTokens) ||
							 !ReadInteger(UsageObject, TEXT("completion_tokens"), CompletionTokens) ||
										  !ReadInteger(UsageObject, TEXT("total_tokens"), TotalTokens) ||
													   PromptTokens > FUnrealAIModelUsageSnapshot::MaxTokenCount -
																		  CompletionTokens ||
													   PromptTokens + CompletionTokens != TotalTokens)
			{
				OutError = TEXT("Compatible chat stream contains invalid or duplicate usage.");
				return false;
			}
			bUsageObserved = true;
		}
		else if (bFinishObserved && Choices->Num() == 0)
		{
			OutError = TEXT("Compatible chat stream emitted a post-finish chunk without final usage.");
			return false;
		}
		return true;
	}

	bool ProcessDelta(const TSharedPtr<FJsonObject> &Delta, FString &OutError)
	{
		if (Delta->HasField(TEXT("role")))
		{
			FString Role;
			if (!Delta->TryGetStringField(TEXT("role"), Role) || Role != TEXT("assistant"))
			{
				OutError = TEXT("Compatible chat delta has an invalid assistant role.");
				return false;
			}
		}

		FString Content;
		if (const TSharedPtr<FJsonValue> ContentValue = Delta->TryGetField(TEXT("content")); ContentValue.IsValid())
		{
			if (ContentValue->Type == EJson::String)
			{
				Content = ContentValue->AsString();
				if (!Content.IsEmpty())
				{
					if (!Tools.IsEmpty())
					{
						OutError = TEXT("Compatible chat stream mixed text and tool output in one choice.");
						return false;
					}
					const FTCHARToUTF8 Added(*Content);
					if (Added.Length() <= 0 || Added.Length() > FUnrealAIModelEvent::MaxTextDeltaUtf8Bytes ||
						TextUtf8Bytes > MaxTextBytes - Added.Length())
					{
						OutError = TEXT("Compatible chat text exceeded its delta or cumulative bound.");
						return false;
					}
					Text += Content;
					TextUtf8Bytes += Added.Length();
					TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
					Event->SetStringField(TEXT("type"), TEXT("response.output_text.delta"));
					Event->SetNumberField(TEXT("sequence_number"), NextSequence++);
					Event->SetStringField(TEXT("item_id"), TEXT("msg_chat_0"));
					Event->SetNumberField(TEXT("output_index"), 0);
					Event->SetNumberField(TEXT("content_index"), 0);
					Event->SetStringField(TEXT("delta"), Content);
					if (!Emit(Event))
					{
						OutError = TEXT("Compatible chat text event exceeded its bound.");
						return false;
					}
				}
			}
			else if (ContentValue->Type != EJson::Null)
			{
				OutError = TEXT("Compatible chat delta contains non-string content.");
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>> *ToolDeltas = nullptr;
		if (!Delta->HasField(TEXT("tool_calls")))
		{
			return true;
		}
		if (!Delta->TryGetArrayField(TEXT("tool_calls"), ToolDeltas) || ToolDeltas == nullptr)
		{
			OutError = TEXT("Compatible chat delta contains a non-array tool_calls field.");
			return false;
		}
		if (!ToolDeltas->IsEmpty() && !Text.IsEmpty())
		{
			OutError = TEXT("Compatible chat stream mixed tool and text output in one choice.");
			return false;
		}
		for (const TSharedPtr<FJsonValue> &ToolValue : *ToolDeltas)
		{
			const TSharedPtr<FJsonObject> *ToolObject = nullptr;
			double NumericIndex = -1.0;
			if (!ToolValue.IsValid() || !ToolValue->TryGetObject(ToolObject) || ToolObject == nullptr ||
				!ToolObject->IsValid() ||
				!(*ToolObject)
					 ->TryGetNumberField(TEXT("index"), NumericIndex) || NumericIndex < 0.0 ||
										 NumericIndex >= MaxToolCalls ||
										 FMath::FloorToDouble(NumericIndex) != NumericIndex)
			{
				OutError = TEXT("Compatible chat tool delta has an invalid index.");
				return false;
			}
			const int32 Index = static_cast<int32>(NumericIndex);
			if (!Tools.Contains(Index) && Tools.Num() >= MaxToolCalls)
			{
				OutError = TEXT("Compatible chat stream exceeded its tool-call count.");
				return false;
			}
			FToolAssembly &Tool = Tools.FindOrAdd(Index);
			Tool.Index = Index;
			FString Value;
			if ((*ToolObject)->HasField(TEXT("type")))
			{
				if (!(*ToolObject)->TryGetStringField(TEXT("type"), Value) || Value != TEXT("function"))
				{
					OutError = TEXT("Compatible chat tool delta has an invalid tool type.");
					return false;
				}
			}
			if ((*ToolObject)->HasField(TEXT("id")))
			{
				if (!(*ToolObject)
						 ->TryGetStringField(TEXT("id"), Value) || Value.IsEmpty() ||
											 (!Tool.CallId.IsEmpty() && Tool.CallId != Value) || !FitsUtf8(Value, 1024))
				{
					OutError = TEXT("Compatible chat tool delta changed call identity.");
					return false;
				}
				Tool.CallId = Value;
			}
			const TSharedPtr<FJsonObject> *Function = nullptr;
			FString ArgumentsDelta;
			if ((*ToolObject)->HasField(TEXT("function")))
			{
				if (!(*ToolObject)
						 ->TryGetObjectField(TEXT("function"), Function) || Function == nullptr || !Function->IsValid())
				{
					OutError = TEXT("Compatible chat tool delta has a non-object function.");
					return false;
				}
				if ((*Function)->HasField(TEXT("name")))
				{
					if (!(*Function)->TryGetStringField(
							TEXT("name"), Value) || Value.IsEmpty() || (!Tool.Name.IsEmpty() && Tool.Name != Value) ||
							!FitsUtf8(Value, FUnrealAIModelToolDescriptor::MaxInvocationNameUtf8Bytes))
					{
						OutError = TEXT("Compatible chat tool delta changed function identity.");
						return false;
					}
					Tool.Name = Value;
				}
				if ((*Function)->HasField(TEXT("arguments")))
				{
					if (!(*Function)->TryGetStringField(TEXT("arguments"), ArgumentsDelta))
					{
						OutError = TEXT("Compatible chat tool delta contains non-string arguments.");
						return false;
					}
					if (!ArgumentsDelta.IsEmpty())
					{
						const FTCHARToUTF8 Added(*ArgumentsDelta);
						if (Added.Length() <= 0 ||
							Added.Length() > FUnrealAIModelEvent::MaxToolArgumentsDeltaUtf8Bytes ||
							Tool.ArgumentUtf8Bytes > MaxToolArgumentBytes - Added.Length() ||
							AggregateToolArgumentBytes > MaxAggregateToolArgumentBytes - Added.Length())
						{
							OutError = TEXT("Compatible chat tool arguments exceeded their delta or cumulative bound.");
							return false;
						}
						Tool.Arguments += ArgumentsDelta;
						Tool.ArgumentUtf8Bytes += Added.Length();
						AggregateToolArgumentBytes += Added.Length();
					}
				}
			}
			if (!Tool.bAdded)
			{
				if (Tool.CallId.IsEmpty() || Tool.Name.IsEmpty())
				{
					OutError = TEXT("Compatible chat tool delta omitted its initial identity.");
					return false;
				}
				if (!EmitToolAdded(Tool))
				{
					OutError = TEXT("Compatible chat tool-added event exceeded its bound.");
					return false;
				}
				Tool.bAdded = true;
			}
			if (!ArgumentsDelta.IsEmpty())
			{
				if (!EmitToolArguments(Tool, ArgumentsDelta))
				{
					OutError = TEXT("Compatible chat tool-argument event exceeded its bound.");
					return false;
				}
			}
		}
		return true;
	}

	bool Complete(FString &OutError)
	{
		if (!bFinishObserved || !bUsageObserved || ResponseId.IsEmpty())
		{
			OutError = TEXT("Compatible chat terminal is missing finish or usage evidence.");
			return false;
		}
		if (ObservedFinishReason == EFinishReason::Length)
		{
			return EmitIncomplete(OutError);
		}
		if ((ObservedFinishReason == EFinishReason::Stop && !Tools.IsEmpty()) ||
			(ObservedFinishReason == EFinishReason::ToolCalls && Tools.IsEmpty()) ||
			ObservedFinishReason == EFinishReason::None)
		{
			OutError = TEXT("Compatible chat finish reason disagrees with its assembled output.");
			return false;
		}
		TArray<int32> ToolIndices;
		Tools.GetKeys(ToolIndices);
		ToolIndices.Sort();
		for (int32 Position = 0; Position < ToolIndices.Num(); ++Position)
		{
			const int32 Index = ToolIndices[Position];
			const FToolAssembly &Tool = Tools.FindChecked(Index);
			if (Index != Position || !Tool.bAdded || Tool.CallId.IsEmpty() || Tool.Name.IsEmpty() ||
				Tool.Arguments.IsEmpty() || !EmitToolDone(Tool))
			{
				OutError = TEXT("Compatible chat terminal contains an incomplete tool call.");
				return false;
			}
		}

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), ResponseId);
		Response->SetStringField(TEXT("status"), TEXT("completed"));
		Response->SetStringField(TEXT("model"), ExpectedModelId);
		TArray<TSharedPtr<FJsonValue>> Output;
		if (!Text.IsEmpty() || Tools.IsEmpty())
		{
			TSharedRef<FJsonObject> TextPart = MakeShared<FJsonObject>();
			TextPart->SetStringField(TEXT("type"), TEXT("output_text"));
			TextPart->SetStringField(TEXT("text"), Text);
			TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
			Message->SetStringField(TEXT("type"), TEXT("message"));
			Message->SetStringField(TEXT("role"), TEXT("assistant"));
			TArray<TSharedPtr<FJsonValue>> Content;
			Content.Add(MakeShared<FJsonValueObject>(TextPart));
			Message->SetArrayField(TEXT("content"), MoveTemp(Content));
			Output.Add(MakeShared<FJsonValueObject>(Message));
		}
		for (const int32 Index : ToolIndices)
		{
			const FToolAssembly &Tool = Tools.FindChecked(Index);
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("type"), TEXT("function_call"));
			Item->SetStringField(TEXT("id"), FString::Printf(TEXT("fc_chat_%d"), Index));
			Item->SetStringField(TEXT("call_id"), Tool.CallId);
			Item->SetStringField(TEXT("name"), Tool.Name);
			Item->SetStringField(TEXT("arguments"), Tool.Arguments);
			Output.Add(MakeShared<FJsonValueObject>(Item));
		}
		Response->SetArrayField(TEXT("output"), MoveTemp(Output));
		TSharedRef<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), PromptTokens);
		Usage->SetNumberField(TEXT("output_tokens"), CompletionTokens);
		Usage->SetNumberField(TEXT("total_tokens"), TotalTokens);
		Response->SetObjectField(TEXT("usage"), Usage);

		TSharedRef<FJsonObject> Completed = MakeShared<FJsonObject>();
		Completed->SetStringField(TEXT("type"), TEXT("response.completed"));
		Completed->SetNumberField(TEXT("sequence_number"), NextSequence++);
		Completed->SetObjectField(TEXT("response"), Response);
		if (!Emit(Completed, true))
		{
			OutError = TEXT("Compatible chat completion event exceeded its bound.");
			return false;
		}
		bTerminal = true;
		return true;
	}

	bool EmitIncomplete(FString &OutError)
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), ResponseId);
		Response->SetStringField(TEXT("status"), TEXT("incomplete"));
		Response->SetStringField(TEXT("model"), ExpectedModelId);
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("reason"), TEXT("max_output_tokens"));
		Response->SetObjectField(TEXT("incomplete_details"), Details);
		TSharedRef<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), PromptTokens);
		Usage->SetNumberField(TEXT("output_tokens"), CompletionTokens);
		Usage->SetNumberField(TEXT("total_tokens"), TotalTokens);
		Response->SetObjectField(TEXT("usage"), Usage);

		TSharedRef<FJsonObject> Incomplete = MakeShared<FJsonObject>();
		Incomplete->SetStringField(TEXT("type"), TEXT("response.incomplete"));
		Incomplete->SetNumberField(TEXT("sequence_number"), NextSequence++);
		Incomplete->SetObjectField(TEXT("response"), Response);
		if (!Emit(Incomplete, true))
		{
			OutError = TEXT("Compatible chat incomplete terminal exceeded its bound.");
			return false;
		}
		bTerminal = true;
		return true;
	}

	bool EmitCreated()
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("id"), ResponseId);
		TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("type"), TEXT("response.created"));
		Event->SetNumberField(TEXT("sequence_number"), NextSequence++);
		Event->SetObjectField(TEXT("response"), Response);
		return Emit(Event);
	}

	bool EmitToolAdded(const FToolAssembly &Tool)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), TEXT("function_call"));
		Item->SetStringField(TEXT("id"), FString::Printf(TEXT("fc_chat_%d"), Tool.Index));
		Item->SetStringField(TEXT("call_id"), Tool.CallId);
		Item->SetStringField(TEXT("name"), Tool.Name);
		Item->SetStringField(TEXT("arguments"), TEXT(""));
		TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("type"), TEXT("response.output_item.added"));
		Event->SetNumberField(TEXT("sequence_number"), NextSequence++);
		Event->SetNumberField(TEXT("output_index"), Tool.Index);
		Event->SetObjectField(TEXT("item"), Item);
		return Emit(Event);
	}

	bool EmitToolArguments(const FToolAssembly &Tool, const FString &Delta)
	{
		TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("type"), TEXT("response.function_call_arguments.delta"));
		Event->SetNumberField(TEXT("sequence_number"), NextSequence++);
		Event->SetStringField(TEXT("item_id"), FString::Printf(TEXT("fc_chat_%d"), Tool.Index));
		Event->SetNumberField(TEXT("output_index"), Tool.Index);
		Event->SetStringField(TEXT("delta"), Delta);
		return Emit(Event);
	}

	bool EmitToolDone(const FToolAssembly &Tool)
	{
		TSharedRef<FJsonObject> ArgumentsDone = MakeShared<FJsonObject>();
		ArgumentsDone->SetStringField(TEXT("type"), TEXT("response.function_call_arguments.done"));
		ArgumentsDone->SetNumberField(TEXT("sequence_number"), NextSequence++);
		ArgumentsDone->SetStringField(TEXT("item_id"), FString::Printf(TEXT("fc_chat_%d"), Tool.Index));
		ArgumentsDone->SetNumberField(TEXT("output_index"), Tool.Index);
		ArgumentsDone->SetStringField(TEXT("arguments"), Tool.Arguments);
		if (!Emit(ArgumentsDone))
		{
			return false;
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), TEXT("function_call"));
		Item->SetStringField(TEXT("id"), FString::Printf(TEXT("fc_chat_%d"), Tool.Index));
		Item->SetStringField(TEXT("call_id"), Tool.CallId);
		Item->SetStringField(TEXT("name"), Tool.Name);
		Item->SetStringField(TEXT("arguments"), Tool.Arguments);
		TSharedRef<FJsonObject> ItemDone = MakeShared<FJsonObject>();
		ItemDone->SetStringField(TEXT("type"), TEXT("response.output_item.done"));
		ItemDone->SetNumberField(TEXT("sequence_number"), NextSequence++);
		ItemDone->SetNumberField(TEXT("output_index"), Tool.Index);
		ItemDone->SetObjectField(TEXT("item"), Item);
		return Emit(ItemDone);
	}

	bool Emit(const TSharedRef<FJsonObject> &Object, const bool bTerminalEvent = false)
	{
		if (EmittedCanonicalEvents >= MaxNormalizedEvents ||
			(!bTerminalEvent && EmittedCanonicalEvents >= MaxNormalizedEvents - 1))
		{
			return false;
		}
		TArray<uint8> Chunk = MakeCanonicalSse(Object);
		if (Chunk.IsEmpty() || !Sink)
		{
			return false;
		}
		++EmittedCanonicalEvents;
		Sink(MoveTemp(Chunk));
		return true;
	}

	FString ExpectedModelId;
	FCanonicalChunkSink Sink;
	TArray<uint8> Pending;
	int32 RetainedStreamBytes = 0;
	int32 WireEventCount = 0;
	int32 EmittedCanonicalEvents = 0;
	int32 TextUtf8Bytes = 0;
	int32 AggregateToolArgumentBytes = 0;
	uint64 NextSequence = 0;
	FString ResponseId;
	FString Text;
	TMap<int32, FToolAssembly> Tools;
	int64 PromptTokens = 0;
	int64 CompletionTokens = 0;
	int64 TotalTokens = 0;
	EFinishReason ObservedFinishReason = EFinishReason::None;
	bool bFinishObserved = false;
	bool bUsageObserved = false;
	bool bTerminal = false;
	bool bFailed = false;
};

FUnrealAIOpenAICompatibleChatStreamDecoder::FUnrealAIOpenAICompatibleChatStreamDecoder(FString ExpectedModelId,
																					   FCanonicalChunkSink InSink)
	: Impl(MakeUnique<FImpl>(MoveTemp(ExpectedModelId), MoveTemp(InSink)))
{
}

FUnrealAIOpenAICompatibleChatStreamDecoder::~FUnrealAIOpenAICompatibleChatStreamDecoder() = default;

bool FUnrealAIOpenAICompatibleChatStreamDecoder::PushBytes(const TConstArrayView<uint8> Bytes, FString &OutError)
{
	return Impl->Push(Bytes, OutError);
}

bool FUnrealAIOpenAICompatibleChatStreamDecoder::Finish(FString &OutError)
{
	return Impl->Finish(OutError);
}

bool FUnrealAIOpenAICompatibleChatStreamDecoder::IsTerminal() const
{
	return Impl->IsTerminal();
}
