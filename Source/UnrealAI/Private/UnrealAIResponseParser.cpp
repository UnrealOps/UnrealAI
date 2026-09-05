#include "UnrealAIResponseAdapter.h"

#include "UnrealAIResponseJson.h"

namespace UnrealAIResponseParserPrivate
{
	using namespace UnrealAIResponseJson;

	FUnrealAIResponsePart Part(const FObject& Block)
	{
		FUnrealAIResponsePart Result;
		Result.ProviderType = String(Block, TEXT("type"));
		Result.RawJson = Serialize(Block);
		if (Result.ProviderType == TEXT("refusal"))
		{
			Result.Type = EUnrealAIResponsePartType::Refusal;
			Result.Text = String(Block, TEXT("refusal"));
		}
		else if (Block && Block->HasField(TEXT("text")) && !Bool(Block, TEXT("thought"))
			&& (Result.ProviderType.IsEmpty() || Result.ProviderType == TEXT("text") || Result.ProviderType == TEXT("output_text")))
		{
			Result.Text = String(Block, TEXT("text"));
		}
		else
		{
			Result.Type = EUnrealAIResponsePartType::ProviderData;
		}
		return Result;
	}

	void AddItem(FUnrealAIResponse& Response, const FObject& Block, const FString& Kind, bool bComplete)
	{
		FUnrealAIResponseItem Item;
		Item.Id = String(Block, TEXT("id"));
		if (Item.Id.IsEmpty())
		{
			Item.Id = FString::Printf(TEXT("item_%d"), Response.Output.Num());
		}
		Item.Role = EUnrealAIMessageRole::Assistant;
		Item.bComplete = bComplete;
		Item.RawJson = Serialize(Block);
		Item.ProviderType = Kind;
		if (Kind == TEXT("function_call") || Kind == TEXT("tool_use") || Kind == TEXT("functionCall") || Kind == TEXT("chat_tool"))
		{
			Item.Type = EUnrealAIResponseItemType::ToolCall;
			FObject Function = Kind == TEXT("functionCall") ? Child(Block, TEXT("functionCall")) :
				(Kind == TEXT("chat_tool") ? Child(Block, TEXT("function")) : Block);
			Item.CallId = Kind == TEXT("function_call") ? String(Block, TEXT("call_id")) :
				String(Kind == TEXT("functionCall") ? Function : Block, TEXT("id"));
			if (Item.CallId.IsEmpty() && Kind == TEXT("functionCall"))
			{
				Item.CallId = TEXT("local_") + Item.Id;
			}
			Item.ToolName = String(Function, TEXT("name"));
			Item.ArgumentsJson = Kind == TEXT("tool_use") ? Serialize(Child(Block, TEXT("input"))) :
				(Kind == TEXT("functionCall") ? Serialize(Child(Function, TEXT("args"))) : String(Function, TEXT("arguments")));
			if (Item.ArgumentsJson.IsEmpty() && (Kind == TEXT("tool_use") || Kind == TEXT("functionCall")))
			{
				Item.ArgumentsJson = TEXT("{}");
			}
		}
		else if (Kind == TEXT("message") || Kind == TEXT("chat_message"))
		{
			const TSharedPtr<FJsonValue> Content = Block ? Block->TryGetField(TEXT("content")) : nullptr;
			if (Content && Content->Type == EJson::String)
			{
				FUnrealAIResponsePart Text;
				Text.Text = Content->AsString();
				Item.Content.Add(Text);
			}
			else
			{
				for (const TSharedPtr<FJsonValue>& Entry : Array(Block, TEXT("content")))
				{
					Item.Content.Add(Part(AsObject(Entry)));
				}
			}
			if (!String(Block, TEXT("refusal")).IsEmpty())
			{
				FUnrealAIResponsePart Refusal;
				Refusal.Type = EUnrealAIResponsePartType::Refusal;
				Refusal.Text = String(Block, TEXT("refusal"));
				Item.Content.Add(Refusal);
			}
		}
		else
		{
			FUnrealAIResponsePart Content = Part(Block);
			if (Content.Type == EUnrealAIResponsePartType::ProviderData)
			{
				Item.Type = EUnrealAIResponseItemType::ProviderData;
			}
			Item.Content.Add(MoveTemp(Content));
		}
		Response.Output.Add(MoveTemp(Item));
	}

	void ReadUsage(const FObject& Usage, const TCHAR* Input, const TCHAR* Output, const TCHAR* Total, FUnrealAIUsage& Out)
	{
		if (!Usage)
		{
			return;
		}
		Out.PromptTokens = Number(Usage, Input);
		Out.CompletionTokens = Number(Usage, Output);
		Out.TotalTokens = Number(Usage, Total, Out.PromptTokens + Out.CompletionTokens);
	}
}

void UnrealAIResponseAdapters::Normalize(FUnrealAIResponseState& State, bool bComplete)
{
	using namespace UnrealAIResponseJson;
	using namespace UnrealAIResponseParserPrivate;
	FUnrealAIResponse& Response = State.Result.Response;
	Response.Output.Reset();
	Response.Model = State.Context.Model;
	Response.bStored = State.Context.bStore;
	const FObject Root = State.Snapshot;
	if (!Root)
	{
		return;
	}
	Response.RawJson = Serialize(Root);
	State.Result.Status = EUnrealAIResponseStatus::Completed;
	Response.Id = String(Root, TEXT("id"));
	if (!String(Root, TEXT("model")).IsEmpty())
	{
		Response.Model = String(Root, TEXT("model"));
	}
	if (State.Context.bNativeResponses)
	{
		const FString Status = String(Root, TEXT("status"));
		if (Status == TEXT("incomplete"))
		{
			State.Result.Status = EUnrealAIResponseStatus::Incomplete;
			Response.IncompleteReason = String(Child(Root, TEXT("incomplete_details")), TEXT("reason"));
		}
		else if (Status == TEXT("failed"))
		{
			State.Result.Status = EUnrealAIResponseStatus::Failed;
			Fail(State.Result.Error, TEXT("The provider reported a failed response."), TEXT("provider_response_failed"));
			const FObject ProviderError = Child(Root, TEXT("error"));
			if (!String(ProviderError, TEXT("code")).IsEmpty())
			{
				State.Result.Error.Code = String(ProviderError, TEXT("code"));
			}
			if (!String(ProviderError, TEXT("message")).IsEmpty())
			{
				State.Result.Error.Message = String(ProviderError, TEXT("message"));
			}
		}
		else if (Status == TEXT("cancelled"))
		{
			State.Result.Status = EUnrealAIResponseStatus::Cancelled;
		}
		else if (bComplete && Status != TEXT("completed"))
		{
			State.Result.Status = EUnrealAIResponseStatus::Failed;
			Fail(State.Result.Error, TEXT("Foreground response did not reach a terminal status."), TEXT("invalid_response"));
		}
		Response.FinishReason = Status;
		ReadUsage(Child(Root, TEXT("usage")), TEXT("input_tokens"), TEXT("output_tokens"), TEXT("total_tokens"), Response.Usage);
		for (const TSharedPtr<FJsonValue>& Value : Array(Root, TEXT("output")))
		{
			FObject Block = AsObject(Value);
			AddItem(Response, Block, String(Block, TEXT("type")), bComplete && String(Block, TEXT("status")) != TEXT("incomplete"));
		}
	}
	else if (State.Context.Api == EUnrealAIProviderApi::AnthropicMessages)
	{
		Response.FinishReason = String(Root, TEXT("stop_reason"));
		ReadUsage(Child(Root, TEXT("usage")), TEXT("input_tokens"), TEXT("output_tokens"), TEXT("total_tokens"), Response.Usage);
		for (const TSharedPtr<FJsonValue>& Value : Array(Root, TEXT("content")))
		{
			FObject Block = AsObject(Value);
			AddItem(Response, Block, String(Block, TEXT("type")), bComplete);
		}
	}
	else if (State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent)
	{
		Response.Id = String(Root, TEXT("responseId"));
		if (!String(Root, TEXT("modelVersion")).IsEmpty())
		{
			Response.Model = String(Root, TEXT("modelVersion"));
		}
		ReadUsage(Child(Root, TEXT("usageMetadata")), TEXT("promptTokenCount"), TEXT("candidatesTokenCount"), TEXT("totalTokenCount"), Response.Usage);
		const FArray Candidates = Array(Root, TEXT("candidates"));
		if (!Candidates.IsEmpty())
		{
			FObject Candidate = AsObject(Candidates[0]);
			Response.FinishReason = String(Candidate, TEXT("finishReason"));
			for (const TSharedPtr<FJsonValue>& Value : Array(Child(Candidate, TEXT("content")), TEXT("parts")))
			{
				FObject Block = AsObject(Value);
				AddItem(Response, Block, Child(Block, TEXT("functionCall")) ? TEXT("functionCall") : TEXT("part"), bComplete);
			}
		}
		FObject Feedback = Child(Root, TEXT("promptFeedback"));
		if (!String(Feedback, TEXT("blockReason")).IsEmpty())
		{
			FObject Block = Object();
			Block->SetStringField(TEXT("type"), TEXT("refusal"));
			Block->SetStringField(TEXT("refusal"), String(Feedback, TEXT("blockReason")));
			AddItem(Response, Block, TEXT("refusal"), bComplete);
		}
		else if (Response.FinishReason == TEXT("SAFETY") || Response.FinishReason == TEXT("RECITATION")
			|| Response.FinishReason == TEXT("BLOCKLIST") || Response.FinishReason == TEXT("PROHIBITED_CONTENT"))
		{
			FObject Block = Object();
			Block->SetStringField(TEXT("type"), TEXT("refusal"));
			Block->SetStringField(TEXT("refusal"), Response.FinishReason);
			AddItem(Response, Block, TEXT("refusal"), bComplete);
		}
	}
	else
	{
		ReadUsage(Child(Root, TEXT("usage")), TEXT("prompt_tokens"), TEXT("completion_tokens"), TEXT("total_tokens"), Response.Usage);
		FArray Choices = Array(Root, TEXT("choices"));
		if (!Choices.IsEmpty())
		{
			FObject Choice = AsObject(Choices[0]);
			Response.FinishReason = String(Choice, TEXT("finish_reason"));
			FObject Message = Child(Choice, TEXT("message"));
			if (Message)
			{
				AddItem(Response, Message, TEXT("chat_message"), bComplete);
				for (const TSharedPtr<FJsonValue>& Tool : Array(Message, TEXT("tool_calls")))
				{
					AddItem(Response, AsObject(Tool), TEXT("chat_tool"), bComplete);
				}
			}
		}
	}
	if (Response.FinishReason == TEXT("length") || Response.FinishReason == TEXT("max_tokens")
		|| Response.FinishReason == TEXT("MAX_TOKENS") || Response.FinishReason == TEXT("pause_turn"))
	{
		State.Result.Status = EUnrealAIResponseStatus::Incomplete;
		Response.IncompleteReason = Response.FinishReason;
	}
	for (int32 Index = 0; Index < Response.Output.Num(); ++Index)
	{
		FString& StableId = State.StableItemIds.FindOrAdd(Index);
		if (StableId.IsEmpty())
		{
			StableId = Response.Output[Index].Id;
		}
		Response.Output[Index].Id = StableId;
	}
	if (!bComplete && State.Context.Api == EUnrealAIProviderApi::AnthropicMessages)
	{
		for (const TPair<int32, FString>& Pair : State.ArgumentBuffers)
		{
			if (Response.Output.IsValidIndex(Pair.Key) && !State.FinishedItems.Contains(Pair.Key))
			{
				Response.Output[Pair.Key].ArgumentsJson = Pair.Value;
			}
		}
	}
	if (bComplete && State.Result.Status == EUnrealAIResponseStatus::Completed)
	{
		const FArray Choices = Array(Root, State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent ? TEXT("candidates") : TEXT("choices"));
		const bool bValidShape = State.Context.bNativeResponses ? Root->HasTypedField<EJson::Array>(TEXT("output")) :
			(State.Context.Api == EUnrealAIProviderApi::AnthropicMessages
				? Root->HasTypedField<EJson::Array>(TEXT("content")) && !Response.FinishReason.IsEmpty() :
				(State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent
					? (Choices.Num() == 1 && !Response.FinishReason.IsEmpty()) || !String(Child(Root, TEXT("promptFeedback")), TEXT("blockReason")).IsEmpty()
					: Choices.Num() == 1 && Child(AsObject(Choices[0]), TEXT("message")) && !Response.FinishReason.IsEmpty()));
		if (!bValidShape)
		{
			State.Result.Status = EUnrealAIResponseStatus::Failed;
			Fail(State.Result.Error, TEXT("The provider returned a malformed or nonterminal response."), TEXT("invalid_response"));
		}
		TSet<FString> Calls;
		for (const FUnrealAIResponseItem& Item : Response.Output)
		{
			if (Item.Type == EUnrealAIResponseItemType::ToolCall)
			{
				if (!Item.bComplete || Item.CallId.IsEmpty() || Item.ToolName.IsEmpty() || Calls.Contains(Item.CallId)
					|| Bytes(Item.ArgumentsJson) > MaxToolArgumentBytes || !Parse(Item.ArgumentsJson))
				{
					State.Result.Status = EUnrealAIResponseStatus::Failed;
					Fail(State.Result.Error, TEXT("The provider returned an invalid or oversized tool call."), TEXT("invalid_tool_call"));
					break;
				}
				Calls.Add(Item.CallId);
			}
		}
	}
	if (bComplete)
	{
		BuildContinuation(State);
	}
}

void UnrealAIResponseAdapters::BuildContinuation(FUnrealAIResponseState& State)
{
	using namespace UnrealAIResponseJson;
	FUnrealAIResponse& Response = State.Result.Response;
	Response.Continuation.Binding = State.Context.Binding;
	Response.Continuation.ItemsJson.Reset();
	if (State.Result.Status != EUnrealAIResponseStatus::Completed || State.Context.bStore)
	{
		return;
	}
	FArray History;
	ParseArray(State.Context.InputJson, History);
	FObject Root = State.Snapshot;
	if (State.Context.bNativeResponses)
	{
		History.Append(Array(Root, TEXT("output")));
	}
	else if (State.Context.Api == EUnrealAIProviderApi::AnthropicMessages)
	{
		FObject Message = Object();
		Message->SetStringField(TEXT("role"), TEXT("assistant"));
		Message->SetArrayField(TEXT("content"), Array(Root, TEXT("content")));
		History.Add(Value(Message));
	}
	else
	{
		const bool bGemini = State.Context.Api == EUnrealAIProviderApi::GeminiGenerateContent;
		FArray Choices = Array(Root, bGemini ? TEXT("candidates") : TEXT("choices"));
		if (!Choices.IsEmpty())
		{
			FObject Message = Child(AsObject(Choices[0]), bGemini ? TEXT("content") : TEXT("message"));
			if (Message && (!bGemini || !Array(Message, TEXT("parts")).IsEmpty()))
			{
				History.Add(Value(Message));
			}
		}
	}
	Response.Continuation.ItemsJson = Serialize(History);
}

void UnrealAIResponseAdapters::ParseResponse(int32 HttpStatus, const FString& Json, FUnrealAIResponseState& State)
{
	using namespace UnrealAIResponseJson;
	State.Result.Error = FUnrealAIError();
	State.Snapshot = Parse(Json);
	State.Result.Response.RawJson = Json;
	const bool bFailedNativeResponse = State.Context.bNativeResponses && String(State.Snapshot, TEXT("status")) == TEXT("failed");
	if (HttpStatus < 200 || HttpStatus >= 300 || !State.Snapshot || (Child(State.Snapshot, TEXT("error")) && !bFailedNativeResponse))
	{
		State.Result.Status = EUnrealAIResponseStatus::Failed;
		Fail(State.Result.Error, TEXT("The provider returned an unsuccessful or malformed response."), TEXT("provider_error"));
		State.Result.Error.HttpStatus = HttpStatus;
		State.Result.Error.RawJson = Json;
		FObject Error = Child(State.Snapshot, TEXT("error"));
		if (Error)
		{
			State.Result.Error.Message = String(Error, TEXT("message"));
			State.Result.Error.Type = String(Error, TEXT("type"));
			State.Result.Error.Code = String(Error, TEXT("code"));
		}
		return;
	}
	Normalize(State, true);
}
