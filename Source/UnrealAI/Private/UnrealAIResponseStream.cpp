#include "UnrealAIResponseAdapter.h"

#include "UnrealAIResponseJson.h"
#include "Misc/ScopeExit.h"

namespace UnrealAIResponseStreamPrivate
{
	using namespace UnrealAIResponseJson;

	void Emit(TArray<FUnrealAIResponseEvent>& Events, EUnrealAIResponseEventType Type,
		const FUnrealAIResponseItem& Item, int32 Index, const FString& Delta = FString(), int32 PartIndex = INDEX_NONE)
	{
		FUnrealAIResponseEvent Event;
		Event.Type = Type;
		Event.ItemIndex = Index;
		Event.PartIndex = PartIndex;
		Event.ItemId = Item.Id;
		Event.Delta = Delta;
		if (Type == EUnrealAIResponseEventType::ItemAdded || Type == EUnrealAIResponseEventType::ItemCompleted)
		{
			Event.Item = Item;
		}
		Events.Add(MoveTemp(Event));
	}

	// Used at item boundaries and final snapshots, never for ordinary text/argument deltas.
	bool Refresh(FUnrealAIResponseState& State, TArray<FUnrealAIResponseEvent>& Events, FUnrealAIError& Error, bool bFinal = false)
	{
		TArray<FUnrealAIResponseItem> Before = MoveTemp(State.Result.Response.Output);
		bool bApplied = false;
		ON_SCOPE_EXIT
		{
			if (!bApplied)
			{
				State.Result.Response.Output = MoveTemp(Before);
			}
		};
		UnrealAIResponseAdapters::Normalize(State, bFinal);
		TArray<FUnrealAIResponseItem>& Items = State.Result.Response.Output;
		if (Before.Num() > Items.Num())
		{
			return Fail(Error, TEXT("A final response removed previously streamed items."), TEXT("invalid_stream_snapshot"));
		}
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			FUnrealAIResponseItem& Item = Items[Index];
			const FUnrealAIResponseItem* Old = Before.IsValidIndex(Index) ? &Before[Index] : nullptr;
			if (Old)
			{
				if (Old->Content.Num() > Item.Content.Num())
				{
					return Fail(Error, TEXT("A final response removed previously streamed parts."), TEXT("invalid_stream_snapshot"));
				}
				// Local IDs remain stable even when the provider supplies an ID later.
				Item.Id = Old->Id;
			}
			else
			{
				Emit(Events, EUnrealAIResponseEventType::ItemAdded, Item, Index);
			}
			for (int32 PartIndex = 0; PartIndex < Item.Content.Num(); ++PartIndex)
			{
				const FUnrealAIResponsePart& Part = Item.Content[PartIndex];
				const FUnrealAIResponsePart* OldPart = Old && Old->Content.IsValidIndex(PartIndex) ? &Old->Content[PartIndex] : nullptr;
				if (!OldPart)
				{
					Emit(Events, EUnrealAIResponseEventType::PartAdded, Item, Index, FString(), PartIndex);
				}
				const FString Prefix = OldPart ? OldPart->Text : FString();
				if (!Prefix.IsEmpty() && !Part.Text.StartsWith(Prefix, ESearchCase::CaseSensitive))
				{
					return Fail(Error, TEXT("A final response contradicted previously streamed content."), TEXT("invalid_stream_snapshot"));
				}
				const FString Delta = Part.Text.Mid(Prefix.Len());
				if (!Delta.IsEmpty())
				{
					Emit(Events, Part.Type == EUnrealAIResponsePartType::Refusal ? EUnrealAIResponseEventType::RefusalDelta :
						EUnrealAIResponseEventType::TextDelta, Item, Index, Delta, PartIndex);
				}
			}
			if (Item.Type == EUnrealAIResponseItemType::ToolCall)
			{
				const FString Prefix = Old ? Old->ArgumentsJson : FString();
				if (!Prefix.IsEmpty() && !Item.ArgumentsJson.StartsWith(Prefix, ESearchCase::CaseSensitive))
				{
					return Fail(Error, TEXT("A final tool call contradicted its argument deltas."), TEXT("invalid_stream_snapshot"));
				}
				if (Bytes(Item.ArgumentsJson) > UnrealAIResponseAdapters::MaxToolArgumentBytes)
				{
					return Fail(Error, TEXT("Tool arguments exceeded the 1 MiB limit."), TEXT("tool_argument_overflow"));
				}
				State.ArgumentByteCounts.Add(Index, Bytes(Item.ArgumentsJson));
				if (Item.ArgumentsJson.Len() > Prefix.Len())
				{
					Emit(Events, EUnrealAIResponseEventType::ToolArgumentsDelta, Item, Index, Item.ArgumentsJson.Mid(Prefix.Len()));
				}
			}
			Item.bComplete = bFinal || State.FinishedItems.Contains(Index);
			if (Item.bComplete && (!Old || !Old->bComplete))
			{
				Emit(Events, EUnrealAIResponseEventType::ItemCompleted, Item, Index);
			}
		}
		if (State.Result.Error.bIsError)
		{
			Error = State.Result.Error;
			bApplied = true; // A provider failure may still include useful partial output.
			return false;
		}
		bApplied = true;
		return true;
	}

	bool Delta(FUnrealAIResponseState& State, TArray<FUnrealAIResponseEvent>& Events,
		int32 Index, int32 PartIndex, const FString& Text, bool bArguments, bool bRefusal, FUnrealAIError& Error)
	{
		if (!State.Result.Response.Output.IsValidIndex(Index))
		{
			return Fail(Error, TEXT("A delta referenced an unknown output item."), TEXT("invalid_stream_item"));
		}
		FUnrealAIResponseItem& Item = State.Result.Response.Output[Index];
		if (Item.bComplete)
		{
			return Fail(Error, TEXT("A delta arrived after item completion."), TEXT("invalid_stream_item"));
		}
		if (bArguments)
		{
			int32& ByteCount = State.ArgumentByteCounts.FindOrAdd(Index);
			const int32 DeltaBytes = Bytes(Text);
			if (Item.Type != EUnrealAIResponseItemType::ToolCall)
			{
				return Fail(Error, TEXT("Argument delta referenced a non-tool item."), TEXT("invalid_stream_item"));
			}
			if (DeltaBytes > UnrealAIResponseAdapters::MaxToolArgumentBytes - ByteCount)
			{
				return Fail(Error, TEXT("Tool arguments exceeded the 1 MiB limit."), TEXT("tool_argument_overflow"));
			}
			ByteCount += DeltaBytes;
			Item.ArgumentsJson += Text;
		}
		else
		{
			if (PartIndex < 0 || PartIndex > 16384)
			{
				return Fail(Error, TEXT("Invalid content part index."), TEXT("invalid_stream_item"));
			}
			while (Item.Content.Num() <= PartIndex)
			{
				Item.Content.AddDefaulted();
				Emit(Events, EUnrealAIResponseEventType::PartAdded, Item, Index, FString(), Item.Content.Num() - 1);
			}
			Item.Content[PartIndex].Type = bRefusal ? EUnrealAIResponsePartType::Refusal : EUnrealAIResponsePartType::Text;
			Item.Content[PartIndex].Text += Text;
		}
		if (!Text.IsEmpty())
		{
			Emit(Events, bArguments ? EUnrealAIResponseEventType::ToolArgumentsDelta :
				(bRefusal ? EUnrealAIResponseEventType::RefusalDelta : EUnrealAIResponseEventType::TextDelta),
				Item, Index, Text, bArguments ? INDEX_NONE : PartIndex);
		}
		return true;
	}

	void CopyFields(const FObject& Source, const FObject& Target)
	{
		if (Source && Target)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
			{
				Target->SetField(Pair.Key, Pair.Value);
			}
		}
	}

	bool Native(const FObject& Root, const FString& Type, FUnrealAIResponseState& State,
		TArray<FUnrealAIResponseEvent>& Events, FUnrealAIError& Error)
	{
		if (Type == TEXT("response.created") || Type == TEXT("response.in_progress"))
		{
			FObject Response = Child(Root, TEXT("response"));
			if (Response)
			{
				CopyFields(Response, State.Snapshot);
			}
			return true;
		}
		if (Type == TEXT("response.completed") || Type == TEXT("response.incomplete")
			|| Type == TEXT("response.failed") || Type == TEXT("response.cancelled"))
		{
			FObject Response = Child(Root, TEXT("response"));
			if (!Response)
			{
				return Fail(Error, TEXT("Missing terminal response snapshot."), TEXT("invalid_stream_snapshot"));
			}
			State.Snapshot = Response;
			State.bSawTerminal = true;
			return Refresh(State, Events, Error, true);
		}
		const int32 Index = Number(Root, TEXT("output_index"), INDEX_NONE);
		if (Type == TEXT("response.output_item.added") || Type == TEXT("response.output_item.done"))
		{
			FArray Output = Array(State.Snapshot, TEXT("output"));
			FObject Block = Child(Root, TEXT("item"));
			if (!Block || !At(Output, Index))
			{
				return Fail(Error, TEXT("Invalid output item event."), TEXT("invalid_stream_item"));
			}
			Output[Index] = Value(Block);
			State.Snapshot->SetArrayField(TEXT("output"), Output);
			if (Type == TEXT("response.output_item.done"))
			{
				State.FinishedItems.Add(Index);
			}
			return Refresh(State, Events, Error);
		}
		FArray Output = Array(State.Snapshot, TEXT("output"));
		if (!Type.StartsWith(TEXT("response.")) || !Output.IsValidIndex(Index))
		{
			if (Type == TEXT("response.function_call_arguments.delta") || Type == TEXT("response.output_text.delta")
				|| Type == TEXT("response.refusal.delta") || Type == TEXT("response.content_part.added")
				|| Type == TEXT("response.content_part.done"))
			{
				return Fail(Error, TEXT("A delta referenced an unknown output item."), TEXT("invalid_stream_item"));
			}
			return true; // Unnormalized lifecycle/provider data is emitted by the caller.
		}
		FObject Block = AsObject(Output[Index]);
		if (Type == TEXT("response.function_call_arguments.delta"))
		{
			const FString Text = String(Root, TEXT("delta"));
			if (!Delta(State, Events, Index, INDEX_NONE, Text, true, false, Error))
			{
				return false;
			}
			Block->SetStringField(TEXT("arguments"), State.Result.Response.Output[Index].ArgumentsJson);
		}
		else if (Type == TEXT("response.output_text.delta") || Type == TEXT("response.refusal.delta"))
		{
			const int32 PartIndex = Number(Root, TEXT("content_index"));
			const bool bRefusal = Type == TEXT("response.refusal.delta");
			const FString Text = String(Root, TEXT("delta"));
			if (!Delta(State, Events, Index, PartIndex, Text, false, bRefusal, Error))
			{
				return false;
			}
			FArray Parts = Array(Block, TEXT("content"));
			FObject Part = At(Parts, PartIndex);
			if (!Part)
			{
				return Fail(Error, TEXT("Invalid content index."));
			}
			Part->SetStringField(TEXT("type"), bRefusal ? TEXT("refusal") : TEXT("output_text"));
			Part->SetStringField(bRefusal ? TEXT("refusal") : TEXT("text"), State.Result.Response.Output[Index].Content[PartIndex].Text);
			Block->SetArrayField(TEXT("content"), Parts);
		}
		else if (Type == TEXT("response.content_part.added") || Type == TEXT("response.content_part.done"))
		{
			FArray Parts = Array(Block, TEXT("content"));
			const int32 PartIndex = Number(Root, TEXT("content_index"));
			FObject Part = Child(Root, TEXT("part"));
			if (!Part || !At(Parts, PartIndex))
			{
				return Fail(Error, TEXT("Invalid content part event."));
			}
			Parts[PartIndex] = Value(Part);
			Block->SetArrayField(TEXT("content"), Parts);
			return Refresh(State, Events, Error);
		}
		return true;
	}

	bool Anthropic(const FObject& Root, const FString& Type, FUnrealAIResponseState& State,
		TArray<FUnrealAIResponseEvent>& Events, FUnrealAIError& Error)
	{
		if (Type == TEXT("message_start"))
		{
			State.Snapshot = Child(Root, TEXT("message"));
			return State.Snapshot ? Refresh(State, Events, Error) : Fail(Error, TEXT("Missing Anthropic message."));
		}
		if (Type == TEXT("message_delta"))
		{
			CopyFields(Child(Root, TEXT("delta")), State.Snapshot);
			FObject Usage = Child(State.Snapshot, TEXT("usage"));
			if (!Usage)
			{
				Usage = Object();
				State.Snapshot->SetObjectField(TEXT("usage"), Usage);
			}
			CopyFields(Child(Root, TEXT("usage")), Usage);
			return true;
		}
		if (Type == TEXT("message_stop"))
		{
			State.bSawTerminal = true;
			return Refresh(State, Events, Error, true);
		}
		const int32 Index = Number(Root, TEXT("index"), INDEX_NONE);
		FArray Content = Array(State.Snapshot, TEXT("content"));
		if (Type == TEXT("content_block_start"))
		{
			FObject Block = Child(Root, TEXT("content_block"));
			if (!Block || !At(Content, Index))
			{
				return Fail(Error, TEXT("Invalid Anthropic block."));
			}
			Content[Index] = Value(Block);
			State.Snapshot->SetArrayField(TEXT("content"), Content);
			if (String(Block, TEXT("type")) == TEXT("tool_use"))
			{
				State.ArgumentBuffers.Add(Index, FString());
			}
			if (!Refresh(State, Events, Error))
			{
				return false;
			}
			if (String(Block, TEXT("type")) == TEXT("tool_use"))
			{
				// The start object's empty input is a placeholder, not an argument delta.
				State.Result.Response.Output[Index].ArgumentsJson.Reset();
				State.ArgumentBuffers.Add(Index, FString());
			}
			return true;
		}
		if (!Content.IsValidIndex(Index))
		{
			if (Type == TEXT("content_block_delta") || Type == TEXT("content_block_stop"))
			{
				return Fail(Error, TEXT("A delta referenced an unknown Anthropic block."), TEXT("invalid_stream_item"));
			}
			return true;
		}
		FObject Block = AsObject(Content[Index]);
		if (Type == TEXT("content_block_delta"))
		{
			FObject Change = Child(Root, TEXT("delta"));
			const FString DeltaType = String(Change, TEXT("type"));
			if (DeltaType == TEXT("text_delta"))
			{
				if (!Delta(State, Events, Index, 0, String(Change, TEXT("text")), false, false, Error))
				{
					return false;
				}
				Block->SetStringField(TEXT("text"), State.Result.Response.Output[Index].Content[0].Text);
			}
			else if (DeltaType == TEXT("input_json_delta"))
			{
				if (!Delta(State, Events, Index, INDEX_NONE, String(Change, TEXT("partial_json")), true, false, Error))
				{
					return false;
				}
				State.ArgumentBuffers.FindOrAdd(Index) = State.Result.Response.Output[Index].ArgumentsJson;
			}
			else if (DeltaType == TEXT("thinking_delta") || DeltaType == TEXT("signature_delta"))
			{
				const TCHAR* Key = DeltaType == TEXT("thinking_delta") ? TEXT("thinking") : TEXT("signature");
				Block->SetStringField(Key, String(Block, Key) + String(Change, Key));
			}
		}
		if (Type == TEXT("content_block_stop"))
		{
			if (const FString* Arguments = State.ArgumentBuffers.Find(Index))
			{
				FObject Input = Parse(Arguments->IsEmpty() ? TEXT("{}") : *Arguments);
				if (!Input)
				{
					return Fail(Error, TEXT("Invalid completed tool arguments."), TEXT("invalid_tool_call"));
				}
				Block->SetObjectField(TEXT("input"), Input);
				// JSON serializers may change whitespace, so reconcile the semantic final value.
				State.Result.Response.Output[Index].ArgumentsJson = Serialize(Input);
			}
			State.FinishedItems.Add(Index);
			return Refresh(State, Events, Error);
		}
		return true;
	}

	bool Chat(const FObject& Root, FUnrealAIResponseState& State,
		TArray<FUnrealAIResponseEvent>& Events, FUnrealAIError& Error)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
		{
			if (Pair.Key != TEXT("choices"))
			{
				State.Snapshot->SetField(Pair.Key, Pair.Value);
			}
		}
		FArray Choices = Array(State.Snapshot, TEXT("choices"));
		FObject Choice = At(Choices, 0);
		FObject Message = Child(Choice, TEXT("message"));
		if (!Message)
		{
			Message = Object();
			Message->SetStringField(TEXT("role"), TEXT("assistant"));
			Message->SetStringField(TEXT("content"), TEXT(""));
			Choice->SetObjectField(TEXT("message"), Message);
			State.Snapshot->SetArrayField(TEXT("choices"), Choices);
			if (!Refresh(State, Events, Error))
			{
				return false;
			}
		}
		for (const TSharedPtr<FJsonValue>& Entry : Array(Root, TEXT("choices")))
		{
			FObject Chunk = AsObject(Entry);
			if (Number(Chunk, TEXT("index")) != 0)
			{
				return Fail(Error, TEXT("The response API expects one generation per request."));
			}
			FObject Change = Child(Chunk, TEXT("delta"));
			if (Change)
			{
				const FString Text = String(Change, TEXT("content"));
				if (!Text.IsEmpty())
				{
					if (!Delta(State, Events, 0, 0, Text, false, false, Error))
					{
						return false;
					}
					Message->SetStringField(TEXT("content"), State.Result.Response.Output[0].Content[0].Text);
				}
				const FString Refusal = String(Change, TEXT("refusal"));
				if (!Refusal.IsEmpty())
				{
					if (!Delta(State, Events, 0, 1, Refusal, false, true, Error))
					{
						return false;
					}
					Message->SetStringField(TEXT("refusal"), State.Result.Response.Output[0].Content[1].Text);
				}
				FArray Calls = Array(Message, TEXT("tool_calls"));
				for (const TSharedPtr<FJsonValue>& Value : Array(Change, TEXT("tool_calls")))
				{
					FObject ToolDelta = AsObject(Value);
					const int32 ToolIndex = Number(ToolDelta, TEXT("index"));
					FObject Call = At(Calls, ToolIndex);
					if (!Call)
					{
						return Fail(Error, TEXT("Invalid tool index."));
					}
					FObject Function = Child(Call, TEXT("function"));
					if (!Function)
					{
						Function = Object();
						Function->SetStringField(TEXT("arguments"), TEXT(""));
						Call->SetObjectField(TEXT("function"), Function);
						Call->SetStringField(TEXT("type"), TEXT("function"));
					}
					FObject FunctionDelta = Child(ToolDelta, TEXT("function"));
					if (!String(ToolDelta, TEXT("id")).IsEmpty())
					{
						Call->SetStringField(TEXT("id"), String(ToolDelta, TEXT("id")));
					}
					if (!String(FunctionDelta, TEXT("name")).IsEmpty())
					{
						Function->SetStringField(TEXT("name"), String(FunctionDelta, TEXT("name")));
					}
					Message->SetArrayField(TEXT("tool_calls"), Calls);
					if (!State.Result.Response.Output.IsValidIndex(ToolIndex + 1) && !Refresh(State, Events, Error))
					{
						return false;
					}
					FUnrealAIResponseItem& Item = State.Result.Response.Output[ToolIndex + 1];
					Item.CallId = String(Call, TEXT("id"));
					Item.ToolName = String(Function, TEXT("name"));
					if (!Delta(State, Events, ToolIndex + 1, INDEX_NONE, String(FunctionDelta, TEXT("arguments")), true, false, Error))
					{
						return false;
					}
					Function->SetStringField(TEXT("arguments"), Item.ArgumentsJson);
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Change->Values)
				{
					if (Pair.Key != TEXT("content") && Pair.Key != TEXT("refusal") && Pair.Key != TEXT("tool_calls"))
					{
						if ((Pair.Key == TEXT("reasoning_content") || Pair.Key == TEXT("reasoning")) && Pair.Value->Type == EJson::String)
						{
							Message->SetStringField(Pair.Key, String(Message, *Pair.Key) + Pair.Value->AsString());
						}
						else
						{
							Message->SetField(Pair.Key, Pair.Value);
						}
					}
				}
			}
			if (!String(Chunk, TEXT("finish_reason")).IsEmpty())
			{
				Choice->SetStringField(TEXT("finish_reason"), String(Chunk, TEXT("finish_reason")));
				State.bSawTerminal = true;
				if (!Refresh(State, Events, Error, true))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool Gemini(const FObject& Root, FUnrealAIResponseState& State,
		TArray<FUnrealAIResponseEvent>& Events, FUnrealAIError& Error)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
		{
			if (Pair.Key != TEXT("candidates"))
			{
				State.Snapshot->SetField(Pair.Key, Pair.Value);
			}
		}
		FArray Candidates = Array(State.Snapshot, TEXT("candidates"));
		if (Array(Root, TEXT("candidates")).IsEmpty())
		{
			return true; // Usage-only or prompt-block frames must not manufacture empty model content.
		}
		FObject Candidate = At(Candidates, 0);
		FObject Content = Child(Candidate, TEXT("content"));
		if (!Content)
		{
			Content = Object();
			Content->SetStringField(TEXT("role"), TEXT("model"));
			Candidate->SetObjectField(TEXT("content"), Content);
			State.Snapshot->SetArrayField(TEXT("candidates"), Candidates);
		}
		FArray Parts = Array(Content, TEXT("parts"));
		for (const TSharedPtr<FJsonValue>& Value : Array(Root, TEXT("candidates")))
		{
			FObject Chunk = AsObject(Value);
			if (Number(Chunk, TEXT("index")) != 0)
			{
				return Fail(Error, TEXT("The response API expects one Gemini candidate."));
			}
			for (const TSharedPtr<FJsonValue>& PartValue : Array(Child(Chunk, TEXT("content")), TEXT("parts")))
			{
				FObject Part = AsObject(PartValue);
				if (!Part)
				{
					return Fail(Error, TEXT("Invalid Gemini content part."));
				}
				// Keep each source part intact; signatures must never move to merged text.
				Parts.Add(PartValue);
				FUnrealAIResponseState Single;
				Single.Context = State.Context;
				Single.Snapshot = Object();
				FObject SingleCandidate = Object();
				FObject SingleContent = Object();
				SingleContent->SetArrayField(TEXT("parts"), {PartValue});
				SingleCandidate->SetObjectField(TEXT("content"), SingleContent);
				Single.Snapshot->SetArrayField(TEXT("candidates"), {UnrealAIResponseJson::Value(SingleCandidate)});
				UnrealAIResponseAdapters::Normalize(Single, false);
				FUnrealAIResponseItem Item = Single.Result.Response.Output[0];
				const int32 Index = State.Result.Response.Output.Num();
				Item.Id = FString::Printf(TEXT("item_%d"), Index);
				if (Item.Type == EUnrealAIResponseItemType::ToolCall && Item.CallId.StartsWith(TEXT("local_")))
				{
					Item.CallId = TEXT("local_") + Item.Id;
				}
				Emit(Events, EUnrealAIResponseEventType::ItemAdded, Item, Index);
				for (int32 PartIndex = 0; PartIndex < Item.Content.Num(); ++PartIndex)
				{
					Emit(Events, EUnrealAIResponseEventType::PartAdded, Item, Index, FString(), PartIndex);
					if (Item.Content[PartIndex].Type == EUnrealAIResponsePartType::Text)
					{
						Emit(Events, EUnrealAIResponseEventType::TextDelta, Item, Index, Item.Content[PartIndex].Text, PartIndex);
					}
				}
				if (Item.Type == EUnrealAIResponseItemType::ToolCall)
				{
					if (Bytes(Item.ArgumentsJson) > UnrealAIResponseAdapters::MaxToolArgumentBytes)
					{
						return Fail(Error, TEXT("Tool arguments exceeded the 1 MiB limit."), TEXT("tool_argument_overflow"));
					}
					Emit(Events, EUnrealAIResponseEventType::ToolArgumentsDelta, Item, Index, Item.ArgumentsJson);
				}
				Item.bComplete = true;
				Emit(Events, EUnrealAIResponseEventType::ItemCompleted, Item, Index);
				State.FinishedItems.Add(Index);
				State.Result.Response.Output.Add(MoveTemp(Item));
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Chunk->Values)
			{
				if (Pair.Key != TEXT("content"))
				{
					Candidate->SetField(Pair.Key, Pair.Value);
				}
			}
			if (!String(Chunk, TEXT("finishReason")).IsEmpty())
			{
				State.bSawTerminal = true;
			}
		}
		Content->SetArrayField(TEXT("parts"), Parts);
		return true;
	}
}

bool UnrealAIResponseAdapters::ParseStreamEvent(const FUnrealAISseEvent& Frame, FUnrealAIResponseState& State,
	TArray<FUnrealAIResponseEvent>& OutEvents, FUnrealAIError& OutError)
{
	using namespace UnrealAIResponseJson;
	using namespace UnrealAIResponseStreamPrivate;
	OutError = FUnrealAIError();
	State.bSawData = true;
	if (!State.Snapshot)
	{
		State.Snapshot = Object();
	}
	if (Frame.Data.TrimStartAndEnd() == TEXT("[DONE]"))
	{
		if (!State.Context.bNativeResponses && State.Context.Api == EUnrealAIProviderApi::OpenAICompatibleChatCompletions)
		{
			State.bSawTerminal = true;
			return Refresh(State, OutEvents, OutError, true);
		}
		return true;
	}
	FObject Root = Parse(Frame.Data);
	if (!Root)
	{
		return Fail(OutError, TEXT("Invalid SSE JSON."), TEXT("invalid_stream_json"));
	}
	const FString Type = String(Root, TEXT("type")).IsEmpty() ? Frame.EventType : String(Root, TEXT("type"));
	if (Type == TEXT("error") || (Child(Root, TEXT("error")) && !State.Context.bNativeResponses))
	{
		return Fail(OutError, TEXT("The provider reported an in-stream error."), TEXT("provider_stream_error"));
	}
	FUnrealAIResponseEvent ProviderEvent;
	ProviderEvent.Type = EUnrealAIResponseEventType::ProviderEvent;
	ProviderEvent.ProviderType = Type;
	ProviderEvent.RawJson = Frame.Data;
	OutEvents.Add(MoveTemp(ProviderEvent));
	const bool bSuccess = State.Context.bNativeResponses ? Native(Root, Type, State, OutEvents, OutError) :
		(State.Context.Api == EUnrealAIProviderApi::AnthropicMessages ? Anthropic(Root, Type, State, OutEvents, OutError) :
		(State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent ? Gemini(Root, State, OutEvents, OutError) :
		Chat(Root, State, OutEvents, OutError)));
	if (bSuccess)
	{
		FObject Usage = Child(State.Snapshot, State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent ? TEXT("usageMetadata") : TEXT("usage"));
		if (Usage)
		{
			const bool bGemini = State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent;
			const bool bModern = State.Context.bNativeResponses || State.Context.Api == EUnrealAIProviderApi::AnthropicMessages;
			FUnrealAIUsage NewUsage;
			NewUsage.PromptTokens = Number(Usage, bGemini ? TEXT("promptTokenCount") : (bModern ? TEXT("input_tokens") : TEXT("prompt_tokens")));
			NewUsage.CompletionTokens = Number(Usage, bGemini ? TEXT("candidatesTokenCount") : (bModern ? TEXT("output_tokens") : TEXT("completion_tokens")));
			NewUsage.TotalTokens = Number(Usage, bGemini ? TEXT("totalTokenCount") : TEXT("total_tokens"), NewUsage.PromptTokens + NewUsage.CompletionTokens);
			FUnrealAIResponseEvent Event;
			Event.Type = EUnrealAIResponseEventType::Usage;
			Event.Usage = NewUsage;
			State.Result.Response.Usage = NewUsage;
			OutEvents.Add(MoveTemp(Event));
		}
	}
	return bSuccess;
}

bool UnrealAIResponseAdapters::CanCompleteStream(const FUnrealAIResponseState& State)
{
	return State.bSawData && (State.bSawTerminal ||
		(State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent
			&& !UnrealAIResponseJson::String(UnrealAIResponseJson::Child(State.Snapshot, TEXT("promptFeedback")), TEXT("blockReason")).IsEmpty()));
}
