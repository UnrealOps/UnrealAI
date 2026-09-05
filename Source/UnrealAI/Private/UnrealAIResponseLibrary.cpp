#include "UnrealAIResponseLibrary.h"

#include "UnrealAIResponseAdapter.h"
#include "UnrealAIResponseJson.h"

FUnrealAIResponseItem UUnrealAIResponseLibrary::MakeResponseMessage(const FString& Text, EUnrealAIMessageRole Role)
{
	FUnrealAIResponseItem Item;
	Item.Role = Role;
	FUnrealAIResponsePart Part;
	Part.Text = Text;
	Item.Content.Add(MoveTemp(Part));
	return Item;
}

FUnrealAIResponseRequest UUnrealAIResponseLibrary::MakeResponseRequest(const FString& Prompt)
{
	FUnrealAIResponseRequest Request;
	Request.Input.Add(MakeResponseMessage(Prompt));
	return Request;
}

FUnrealAIToolDefinition UUnrealAIResponseLibrary::MakeToolDefinition(const FString& Name,
	const FString& Description, const FString& ParametersJson, bool bStrict)
{
	FUnrealAIToolDefinition Tool;
	Tool.Name = Name;
	Tool.Description = Description;
	Tool.ParametersJson = ParametersJson;
	Tool.bStrict = bStrict;
	return Tool;
}

FString UUnrealAIResponseLibrary::GetResponseText(const FUnrealAIResponse& Response)
{
	FString Text;
	for (const FUnrealAIResponseItem& Item : Response.Output)
	{
		if (Item.Type == EUnrealAIResponseItemType::Message)
		{
			for (const FUnrealAIResponsePart& Part : Item.Content)
			{
				if (Part.Type == EUnrealAIResponsePartType::Text)
				{
					Text += Part.Text;
				}
			}
		}
	}
	return Text;
}

TArray<FUnrealAIResponseItem> UUnrealAIResponseLibrary::GetResponseToolCalls(const FUnrealAIResponseResult& Result)
{
	TArray<FUnrealAIResponseItem> Calls;
	if (Result.Status != EUnrealAIResponseStatus::Completed || Result.Error.bIsError)
	{
		return Calls;
	}
	for (const FUnrealAIResponseItem& Item : Result.Response.Output)
	{
		if (Item.Type == EUnrealAIResponseItemType::ToolCall && Item.bComplete && !Item.CallId.IsEmpty()
			&& !Item.ToolName.IsEmpty() && UnrealAIResponseJson::Parse(Item.ArgumentsJson)
			&& UnrealAIResponseJson::Bytes(Item.ArgumentsJson) <= UnrealAIResponseAdapters::MaxToolArgumentBytes)
		{
			Calls.Add(Item);
		}
	}
	return Calls;
}

FUnrealAIResponseItem UUnrealAIResponseLibrary::MakeToolResult(const FUnrealAIResponseItem& Call,
	const FString& Output, bool bIsError)
{
	FUnrealAIResponseItem Result;
	Result.Type = EUnrealAIResponseItemType::ToolResult;
	Result.CallId = Call.CallId;
	Result.ToolName = Call.ToolName;
	Result.Output = Output;
	Result.bToolError = bIsError;
	Result.RawJson = Call.RawJson;
	return Result;
}

bool UUnrealAIResponseLibrary::BuildContinuationRequest(const FUnrealAIResponseRequest& PreviousRequest,
	const FUnrealAIResponseResult& PreviousResult, const TArray<FUnrealAIResponseItem>& NewInput,
	FUnrealAIResponseRequest& OutRequest, FUnrealAIError& OutError)
{
	using namespace UnrealAIResponseJson;
	OutRequest = FUnrealAIResponseRequest();
	OutError = FUnrealAIError();
	if (PreviousResult.Status != EUnrealAIResponseStatus::Completed || PreviousResult.Error.bIsError
		|| PreviousResult.Response.Continuation.Binding.IsEmpty() || NewInput.IsEmpty())
	{
		return Fail(OutError, TEXT("Continuation requires a completed response and new input."));
	}
	const TArray<FUnrealAIResponseItem> Calls = GetResponseToolCalls(PreviousResult);
	TSet<FString> Answered;
	TArray<FUnrealAIResponseItem> Results;
	TArray<FUnrealAIResponseItem> Messages;
	for (const FUnrealAIResponseItem& Input : NewInput)
	{
		if (Input.Type == EUnrealAIResponseItemType::ToolResult)
		{
			const FUnrealAIResponseItem* Call = Calls.FindByPredicate([&Input](const FUnrealAIResponseItem& Candidate)
			{
				return Candidate.CallId == Input.CallId;
			});
			if (!Call || Answered.Contains(Input.CallId) || Input.ToolName != Call->ToolName)
			{
				return Fail(OutError, TEXT("Tool results must match pending calls exactly once."));
			}
			Answered.Add(Input.CallId);
			Results.Add(MakeToolResult(*Call, Input.Output, Input.bToolError));
		}
		else if (Input.Type == EUnrealAIResponseItemType::Message)
		{
			Messages.Add(Input);
		}
		else
		{
			return Fail(OutError, TEXT("New continuation input must contain messages or tool results."));
		}
	}
	if (Answered.Num() != Calls.Num())
	{
		return Fail(OutError, TEXT("Return a result for every pending tool call before continuing."));
	}
	if (PreviousRequest.bStore != PreviousResult.Response.bStored)
	{
		return Fail(OutError, TEXT("The request and response use different storage modes."));
	}
	OutRequest = PreviousRequest;
	OutRequest.Input = MoveTemp(Results);
	OutRequest.Input.Append(Messages);
	OutRequest.History = PreviousResult.Response.Continuation;
	OutRequest.PreviousResponseId.Reset();
	if (PreviousResult.Response.bStored)
	{
		if (PreviousResult.Response.Id.IsEmpty())
		{
			OutRequest = FUnrealAIResponseRequest();
			return Fail(OutError, TEXT("Stored continuation requires a provider response ID."));
		}
		OutRequest.PreviousResponseId = PreviousResult.Response.Id;
		OutRequest.History.ItemsJson.Reset();
	}
	else if (OutRequest.History.ItemsJson.IsEmpty())
	{
		OutRequest = FUnrealAIResponseRequest();
		return Fail(OutError, TEXT("The response has no replayable local history."));
	}
	return true;
}
