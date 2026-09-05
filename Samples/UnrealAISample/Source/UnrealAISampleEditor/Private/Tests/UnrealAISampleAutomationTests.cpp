#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraph.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIChatCompletionAsyncAction.h"
#include "UnrealAIClient.h"
#include "UnrealAIPackagedChatWidgetExample.h"
#include "UnrealAIPackagedClientFacadeExample.h"
#include "UnrealAIProductionDeploymentExample.h"
#include "UnrealAISampleActor.h"
#include "UnrealAIMultiTurnExample.h"
#include "UnrealAIStreamingExample.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

struct FUnrealAIMultiTurnExampleTestAccess
{
	static void BeginPendingTurn(AUnrealAIMultiTurnExample& Example, const FString& UserText)
	{
		Example.PendingUserMessage.Role = EUnrealAIMessageRole::User;
		Example.PendingUserMessage.Content = UserText;
		Example.bRequestInFlight = true;
		Example.bStreamingTurn = false;
		Example.PendingAssistantText.Reset();
		Example.LastInterruptedAssistantText.Reset();
	}

	static void BeginPendingStream(AUnrealAIMultiTurnExample& Example, const FString& UserText)
	{
		BeginPendingTurn(Example, UserText);
		Example.bStreamingTurn = true;
	}

	static void Complete(AUnrealAIMultiTurnExample& Example, const FString& AssistantText)
	{
		FUnrealAIChatResponse Response;
		FUnrealAIChatChoice Choice;
		Choice.Role = TEXT("assistant");
		Choice.Content = AssistantText;
		Response.Choices.Add(Choice);
		Example.HandleTurnCompleted(Response, FUnrealAIError());
	}

	static void Fail(AUnrealAIMultiTurnExample& Example, const FString& Code)
	{
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.Code = Code;
		Error.Message = TEXT("Synthetic contract failure");
		Example.HandleTurnCompleted(FUnrealAIChatResponse(), Error);
	}

	static void Trim(AUnrealAIMultiTurnExample& Example)
	{
		Example.TrimCompletedHistory();
	}

	static void EmitStreamDelta(AUnrealAIMultiTurnExample& Example, const FString& Text)
	{
		FUnrealAIChatStreamEvent Event;
		Event.Type = EUnrealAIChatStreamEventType::TextDelta;
		Event.TextDelta = Text;
		Example.HandleStreamEvent(Event);
	}

	static void EmitProviderEvent(AUnrealAIMultiTurnExample& Example)
	{
		FUnrealAIChatStreamEvent Event;
		Event.Type = EUnrealAIChatStreamEventType::ProviderEvent;
		Event.RawJson = TEXT("{\"private\":true}");
		Example.HandleStreamEvent(Event);
	}

	static void RetryStream(AUnrealAIMultiTurnExample& Example)
	{
		FUnrealAIRetryEvent Event;
		Event.RetryNumber = 1;
		Event.MaxRetries = 2;
		Event.DelaySeconds = 0.25f;
		Example.HandleTurnRetry(Event);
	}

	static void FinishStream(
		AUnrealAIMultiTurnExample& Example,
		EUnrealAIChatStreamStatus Status,
		const FString& AggregateText = FString())
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = Status;
		if (!AggregateText.IsEmpty())
		{
			FUnrealAIChatChoice Choice;
			Choice.Role = TEXT("assistant");
			Choice.Content = AggregateText;
			Result.Response.Choices.Add(Choice);
		}
		if (Status == EUnrealAIChatStreamStatus::Failed)
		{
			Result.Error.bIsError = true;
			Result.Error.Code = TEXT("synthetic_stream_failure");
			Result.Error.Message = TEXT("Synthetic stream contract failure");
		}
		Example.HandleStreamTerminal(Result);
	}
};

struct FUnrealAIStreamingExampleTestAccess
{
	static void Emit(AUnrealAIStreamingExample& Example, const FUnrealAIChatStreamEvent& Event)
	{
		Example.HandleStreamEvent(Event);
	}

	static void Complete(AUnrealAIStreamingExample& Example, const FString& AggregateText)
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = EUnrealAIChatStreamStatus::Completed;
		FUnrealAIChatChoice Choice;
		Choice.Role = TEXT("assistant");
		Choice.Content = AggregateText;
		Result.Response.Choices.Add(Choice);
		Example.HandleStreamTerminal(Result);
	}
};

struct FUnrealAIPackagedClientFacadeExampleTestAccess
{
	static const FUnrealAIProviderConfig* GetBackendConfig(
		const UUnrealAIPackagedClientFacadeExample& Facade)
	{
		return Facade.BackendClient ? &Facade.BackendClient->GetProviderConfig() : nullptr;
	}

	static bool ValidateMessages(
		const TArray<FUnrealAIChatMessage>& Messages,
		FName& OutFailureReason)
	{
		return UUnrealAIPackagedClientFacadeExample::ValidateMessages(
			Messages,
			OutFailureReason);
	}

	static FUnrealAIChatRequest BuildBackendRequest(
		const FGuid& RequestId,
		const TArray<FUnrealAIChatMessage>& Messages)
	{
		return UUnrealAIPackagedClientFacadeExample::BuildBackendRequest(RequestId, Messages);
	}

	static FName MapHttpFailure(int32 HttpStatus)
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = EUnrealAIChatStreamStatus::Failed;
		Result.Error.bIsError = true;
		Result.Error.HttpStatus = HttpStatus;
		return UUnrealAIPackagedClientFacadeExample::MapFailureReason(Result);
	}

	static void BeginSyntheticStream(
		UUnrealAIPackagedClientFacadeExample& Facade,
		const FGuid& RequestId)
	{
		Facade.bRequestInFlight = true;
		Facade.ActiveRequestId = RequestId;
		Facade.PartialText.Reset();
		Facade.bOutputLimitExceeded = false;
	}

	static void EmitText(
		UUnrealAIPackagedClientFacadeExample& Facade,
		const FString& Text)
	{
		FUnrealAIChatStreamEvent Event;
		Event.Type = EUnrealAIChatStreamEventType::TextDelta;
		Event.TextDelta = Text;
		Facade.HandleStreamEvent(Event);
	}

	static void EmitProviderEvent(UUnrealAIPackagedClientFacadeExample& Facade)
	{
		FUnrealAIChatStreamEvent Event;
		Event.Type = EUnrealAIChatStreamEventType::ProviderEvent;
		Event.RawJson = TEXT("{\"private\":true}");
		Facade.HandleStreamEvent(Event);
	}

	static void EmitRetry(UUnrealAIPackagedClientFacadeExample& Facade)
	{
		FUnrealAIRetryEvent Event;
		Event.RetryNumber = 1;
		Event.MaxRetries = 2;
		Event.DelaySeconds = 0.25f;
		Facade.HandleRetry(Event);
	}

	static FString GetPartialText(const UUnrealAIPackagedClientFacadeExample& Facade)
	{
		return Facade.PartialText;
	}

	static int32 GetTerminalBroadcastCount(
		const UUnrealAIPackagedClientFacadeExample& Facade)
	{
		return Facade.TerminalBroadcastCountForTests;
	}

	static FName GetLastTerminalFailureReason(
		const UUnrealAIPackagedClientFacadeExample& Facade)
	{
		return Facade.LastTerminalFailureReasonForTests;
	}

	static void Finish(
		UUnrealAIPackagedClientFacadeExample& Facade,
		EUnrealAIChatStreamStatus Status,
		const FString& AggregateText = FString(),
		int32 FailedHttpStatus = 503)
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = Status;
		if (!AggregateText.IsEmpty())
		{
			FUnrealAIChatChoice Choice;
			Choice.Role = TEXT("assistant");
			Choice.Content = AggregateText;
			Result.Response.Choices.Add(Choice);
		}
		if (Status == EUnrealAIChatStreamStatus::Failed)
		{
			Result.Error.bIsError = true;
			Result.Error.HttpStatus = FailedHttpStatus;
		}
		Facade.HandleTerminal(Result);
	}
};

struct FUnrealAIPackagedChatWidgetExampleTestAccess
{
	static void BeginSyntheticTurn(
		UUnrealAIPackagedChatWidgetExample& Widget,
		const FGuid& RequestId,
		const FString& UserText)
	{
		Widget.PendingUserText = UserText;
		Widget.PendingAssistantText.Reset();
		Widget.InterruptedAssistantText.Reset();
		Widget.ActiveRequestId = RequestId;
		Widget.RequestState = EUnrealAIPackagedChatState::Starting;
		Widget.bAcceptEvents = true;
	}

	static FString GetRetryUserText(const UUnrealAIPackagedChatWidgetExample& Widget)
	{
		return Widget.RetryUserText;
	}

	static void SetRetryState(
		UUnrealAIPackagedChatWidgetExample& Widget,
		const FString& UserText)
	{
		Widget.ActiveRequestId.Invalidate();
		Widget.PendingUserText.Reset();
		Widget.RetryUserText = UserText;
		Widget.RequestState = EUnrealAIPackagedChatState::Failed;
	}

	static TArray<FUnrealAIChatMessage> BuildRequestHistory(
		const UUnrealAIPackagedChatWidgetExample& Widget,
		const FString& PendingUser)
	{
		TArray<FUnrealAIChatMessage> Result = Widget.CommittedHistory;
		UUnrealAIPackagedChatWidgetExample::TrimHistoryForRequest(Result, PendingUser);
		return Result;
	}

	static int32 CountHistoryCharacters(const TArray<FUnrealAIChatMessage>& History)
	{
		return UUnrealAIPackagedChatWidgetExample::CountHistoryCharacters(
			History);
	}

	static void Destruct(UUnrealAIPackagedChatWidgetExample& Widget)
	{
		Widget.NativeDestruct();
	}

	static void SetStreamTurnHook(
		UUnrealAIPackagedChatWidgetExample& Widget,
		TFunction<bool(const FGuid&, const TArray<FUnrealAIChatMessage>&, FName&)>&& Hook)
	{
		Widget.StreamTurnForTests = MoveTemp(Hook);
	}

	static void ClearStreamTurnHook(UUnrealAIPackagedChatWidgetExample& Widget)
	{
		Widget.StreamTurnForTests = nullptr;
	}

	static bool AreAllFacadeDelegatesBound(
		const UUnrealAIPackagedClientFacadeExample& Facade,
		const UUnrealAIPackagedChatWidgetExample& Widget)
	{
		return Facade.OnTextDelta.Contains(
			&Widget,
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleTextDelta))
			&& Facade.OnRetrying.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleRetrying))
			&& Facade.OnCompleted.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleCompleted))
			&& Facade.OnFailed.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleFailed))
			&& Facade.OnCancelled.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleCancelled));
	}

	static bool IsAnyFacadeDelegateBound(
		const UUnrealAIPackagedClientFacadeExample& Facade,
		const UUnrealAIPackagedChatWidgetExample& Widget)
	{
		return Facade.OnTextDelta.Contains(
			&Widget,
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleTextDelta))
			|| Facade.OnRetrying.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleRetrying))
			|| Facade.OnCompleted.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleCompleted))
			|| Facade.OnFailed.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleFailed))
			|| Facade.OnCancelled.Contains(
				&Widget,
				GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, HandleCancelled));
	}

	static void SimulateTerminalDuringRejectedCancel(
		UUnrealAIPackagedChatWidgetExample& Widget,
		const FGuid& RequestId,
		EUnrealAIPackagedChatState PreviousState)
	{
		Widget.RequestState = EUnrealAIPackagedChatState::Cancelling;
		Widget.HandleFailed(RequestId, TEXT("service_unavailable"), TEXT("drained"));
		Widget.ReconcileRejectedCancel(RequestId, PreviousState);
	}
};

namespace UnrealAISampleTests
{
	enum class ELiveRequestKind : uint8
	{
		CppCompletion,
		CppStreaming,
		BlueprintCompletion
	};

	const TCHAR* BlueprintObjectPath = TEXT("/Game/Blueprints/BP_UnrealAIGettingStarted.BP_UnrealAIGettingStarted");
	const TCHAR* BackendWidgetObjectPath = TEXT("/Game/Blueprints/WBP_UnrealAIBackendChat.WBP_UnrealAIBackendChat");
	const TCHAR* StarterMapObjectPath = TEXT("/Game/Maps/UnrealAISampleMap.UnrealAISampleMap");
	constexpr double LiveTimeoutSeconds = 180.0;

	struct FContractWorld
	{
		UWorld* World = nullptr;
		UGameInstance* GameInstance = nullptr;

		~FContractWorld()
		{
			Cleanup();
		}

		bool Initialize()
		{
			if (!GEngine)
			{
				return false;
			}

			const FName WorldName = MakeUniqueObjectName(
				GetTransientPackage(),
				UWorld::StaticClass(),
				TEXT("UnrealAISampleContractWorld"));
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName, GetTransientPackage());
			if (!World)
			{
				GEngine->DestroyWorldContext(World);
				return false;
			}

			World->AddToRoot();
			WorldContext.SetCurrentWorld(World);
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->AddToRoot();
			World->SetGameInstance(GameInstance);
			WorldContext.OwningGameInstance = GameInstance;
			World->InitializeActorsForPlay(FURL());
			return true;
		}

		void Cleanup()
		{
			if (World)
			{
				GEngine->ShutdownWorldNetDriver(World);
				World->DestroyWorld(true);
				World->SetPhysicsScene(nullptr);
				GEngine->DestroyWorldContext(World);
				World->RemoveFromRoot();
				World = nullptr;
			}

			if (GameInstance)
			{
				GameInstance->RemoveFromRoot();
				GameInstance = nullptr;
			}
		}
	};

	struct FLiveRequestState
	{
		UWorld* World = nullptr;
		UGameInstance* GameInstance = nullptr;
		TWeakObjectPtr<AUnrealAISampleActor> Actor;
		double Deadline = 0.0;

		~FLiveRequestState()
		{
			Cleanup();
		}

		bool Initialize(UClass* ActorClass)
		{
			if (!GEngine || !ActorClass)
			{
				return false;
			}

			const FName WorldName = MakeUniqueObjectName(
				GetTransientPackage(),
				UWorld::StaticClass(),
				TEXT("UnrealAISampleLiveWorld"));
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName, GetTransientPackage());
			if (!World)
			{
				GEngine->DestroyWorldContext(World);
				return false;
			}

			World->AddToRoot();
			WorldContext.SetCurrentWorld(World);
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->AddToRoot();
			World->SetGameInstance(GameInstance);
			WorldContext.OwningGameInstance = GameInstance;
			World->InitializeActorsForPlay(FURL());

			Actor = World->SpawnActor<AUnrealAISampleActor>(ActorClass);
			Deadline = FPlatformTime::Seconds() + LiveTimeoutSeconds;
			return Actor.IsValid();
		}

		void Cleanup()
		{
			if (AUnrealAISampleActor* SampleActor = Actor.Get())
			{
				SampleActor->CancelCppRequests();
				SampleActor->Destroy(true);
			}
			Actor.Reset();

			if (World)
			{
				GEngine->ShutdownWorldNetDriver(World);
				World->DestroyWorld(true);
				World->SetPhysicsScene(nullptr);
				GEngine->DestroyWorldContext(World);
				World->RemoveFromRoot();
				World = nullptr;
			}

			if (GameInstance)
			{
				GameInstance->RemoveFromRoot();
				GameInstance = nullptr;
			}
		}
	};

	bool HasLiveXAIKey(FAutomationTestBase& Test)
	{
		if (!FPlatformMisc::GetEnvironmentVariable(TEXT("XAI_API_KEY")).TrimStartAndEnd().IsEmpty())
		{
			return true;
		}

		Test.AddError(TEXT("XAI_API_KEY is required for this opt-in live test."));
		return false;
	}

	void AddLiveWaitCommand(
		FAutomationTestBase& Test,
		const TSharedRef<FLiveRequestState>& State,
		ELiveRequestKind RequestKind)
	{
		Test.AddCommand(new FFunctionLatentCommand([&Test, State, RequestKind]()
		{
			AUnrealAISampleActor* Actor = State->Actor.Get();
			bool bFinished = false;
			if (Actor)
			{
				switch (RequestKind)
				{
				case ELiveRequestKind::CppCompletion:
					bFinished = Actor->bCppCompletionFinished;
					break;
				case ELiveRequestKind::CppStreaming:
					bFinished = Actor->bCppStreamFinished;
					break;
				case ELiveRequestKind::BlueprintCompletion:
					bFinished = Actor->bBlueprintRequestFinished;
					break;
				}
			}
			if (!bFinished && FPlatformTime::Seconds() < State->Deadline)
			{
				return false;
			}

			if (!Actor)
			{
				Test.AddError(TEXT("The live sample actor was destroyed before the request completed."));
			}
			else if (!bFinished)
			{
				Test.AddError(TEXT("The live UnrealAI request timed out."));
			}
			else if (RequestKind == ELiveRequestKind::BlueprintCompletion)
			{
				Test.TestTrue(TEXT("Blueprint request succeeds"), Actor->bBlueprintRequestSucceeded);
				Test.TestFalse(TEXT("Blueprint response has content"), Actor->BlueprintResponseText.IsEmpty());
			}
			else if (RequestKind == ELiveRequestKind::CppCompletion)
			{
				Test.TestTrue(TEXT("C++ request succeeds"), Actor->bCppCompletionSucceeded);
				Test.TestFalse(TEXT("C++ response has content"), Actor->CppCompletionText.IsEmpty());
			}
			else
			{
				Test.TestTrue(TEXT("C++ stream completes"), Actor->bCppStreamSucceeded);
				Test.TestTrue(TEXT("C++ stream emits text events"), Actor->CppStreamTextEventCount > 0);
				Test.TestFalse(TEXT("C++ stream accumulates text"), Actor->CppStreamingText.IsEmpty());
				Test.TestEqual(TEXT("C++ stream emits one terminal callback"), Actor->CppStreamTerminalCount, 1);
			}

			State->Cleanup();
			return true;
		}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleBlueprintAssetContractTest,
	"UnrealAISample.SkillContracts.Blueprint.GeneratedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleBlueprintAssetContractTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAISampleTests;

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, BlueprintObjectPath);
	if (!TestNotNull(TEXT("Blueprint example loads"), Blueprint))
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("Blueprint compiles without errors"), Blueprint->Status != BS_Error);
	TestNotNull(TEXT("Blueprint generated class exists"), Blueprint->GeneratedClass.Get());

	bool bFoundCompletionNode = false;
	bool bHandlesCompleted = false;
	bool bHandlesRetrying = false;
	bool bHandlesFailed = false;
	bool bHandlesCancelled = false;
	bool bRequestInputConnected = false;
	bool bProviderDefaultIsXAI = false;
	bool bFoundRequestHelper = false;
	bool bPromptDefaultIsSet = false;
	bool bFoundResponseHelper = false;
	bool bResponseInputConnected = false;
	bool bContentOutputConnected = false;
	bool bHasContentOutputConnected = false;
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		for (const UEdGraphNode* GraphNode : Graph->Nodes)
		{
			const UK2Node_AsyncAction* AsyncNode = Cast<UK2Node_AsyncAction>(GraphNode);
			if (AsyncNode && AsyncNode->GetFactoryFunction() ==
				UUnrealAIChatCompletionAsyncAction::StaticClass()->FindFunctionByName(
					GET_FUNCTION_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, CreateChatCompletion)))
			{
				bFoundCompletionNode = true;
				const auto IsConnected = [AsyncNode](FName PinName, EEdGraphPinDirection Direction)
				{
					const UEdGraphPin* Pin = AsyncNode->FindPin(PinName, Direction);
					return Pin && Pin->LinkedTo.Num() > 0;
				};
				bHandlesCompleted = IsConnected(TEXT("Completed"), EGPD_Output);
				bHandlesRetrying = IsConnected(TEXT("Retrying"), EGPD_Output);
				bHandlesFailed = IsConnected(TEXT("Failed"), EGPD_Output);
				bHandlesCancelled = IsConnected(TEXT("Cancelled"), EGPD_Output);
				bRequestInputConnected = IsConnected(TEXT("Request"), EGPD_Input);
				if (const UEdGraphPin* ProviderPin = AsyncNode->FindPin(TEXT("ProviderName"), EGPD_Input))
				{
					bProviderDefaultIsXAI = ProviderPin->DefaultValue == TEXT("XAI");
				}
			}

			const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(GraphNode);
			if (!CallNode)
			{
				continue;
			}

			const UFunction* TargetFunction = CallNode->GetTargetFunction();
			if (TargetFunction == UUnrealAIBlueprintLibrary::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(UUnrealAIBlueprintLibrary, MakeSimpleChatRequest)))
			{
				bFoundRequestHelper = true;
				if (const UEdGraphPin* PromptPin = CallNode->FindPin(TEXT("Prompt"), EGPD_Input))
				{
					bPromptDefaultIsSet = PromptPin->DefaultValue == TEXT("Reply with exactly: UnrealAI sample works.");
				}
			}
			else if (TargetFunction == UUnrealAIBlueprintLibrary::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(UUnrealAIBlueprintLibrary, GetFirstChoiceContent)))
			{
				bFoundResponseHelper = true;
				const UEdGraphPin* ResponsePin = CallNode->FindPin(TEXT("Response"), EGPD_Input);
				const UEdGraphPin* ContentPin = CallNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
				const UEdGraphPin* HasContentPin = CallNode->FindPin(TEXT("bHasContent"), EGPD_Output);
				bResponseInputConnected = ResponsePin && ResponsePin->LinkedTo.Num() > 0;
				bContentOutputConnected = ContentPin && ContentPin->LinkedTo.Num() > 0;
				bHasContentOutputConnected = HasContentPin && HasContentPin->LinkedTo.Num() > 0;
			}
		}
	}

	TestTrue(TEXT("Uses Create Chat Completion (UnrealAI)"), bFoundCompletionNode);
	TestTrue(TEXT("Handles Completed"), bHandlesCompleted);
	TestTrue(TEXT("Handles Retrying"), bHandlesRetrying);
	TestTrue(TEXT("Handles Failed"), bHandlesFailed);
	TestTrue(TEXT("Handles Cancelled"), bHandlesCancelled);
	TestTrue(TEXT("The request input is connected"), bRequestInputConnected);
	TestTrue(TEXT("The provider profile default is XAI"), bProviderDefaultIsXAI);
	TestTrue(TEXT("Uses Make Simple Chat Request"), bFoundRequestHelper);
	TestTrue(TEXT("The sample prompt default is set"), bPromptDefaultIsSet);
	TestTrue(TEXT("Uses Get First Choice Content"), bFoundResponseHelper);
	TestTrue(TEXT("The response helper input is connected"), bResponseInputConnected);
	TestTrue(TEXT("The content output is consumed"), bContentOutputConnected);
	TestTrue(TEXT("The Has Content output is consumed"), bHasContentOutputConnected);

	UWorld* StarterMap = LoadObject<UWorld>(nullptr, StarterMapObjectPath);
	if (!TestNotNull(TEXT("Starter map loads"), StarterMap))
	{
		return false;
	}

	bool bFoundPlacedBlueprint = false;
	if (StarterMap->PersistentLevel)
	{
		for (const AActor* Actor : StarterMap->PersistentLevel->Actors)
		{
			if (Actor && Actor->IsA(Blueprint->GeneratedClass.Get()))
			{
				bFoundPlacedBlueprint = true;
				break;
			}
		}
	}
	TestTrue(TEXT("Starter map contains the Blueprint example"), bFoundPlacedBlueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleMultiTurnBlueprintSurfaceTest,
	"UnrealAISample.SkillContracts.Mixed.BlueprintSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleMultiTurnBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
	UClass* ExampleClass = AUnrealAIMultiTurnExample::StaticClass();
	TestNotNull(TEXT("Mixed multi-turn example class exists"), ExampleClass);

	for (const FName FunctionName : {
		GET_FUNCTION_NAME_CHECKED(AUnrealAIMultiTurnExample, ResetConversation),
		GET_FUNCTION_NAME_CHECKED(AUnrealAIMultiTurnExample, SendTurn),
		GET_FUNCTION_NAME_CHECKED(AUnrealAIMultiTurnExample, SendTurnStream),
		GET_FUNCTION_NAME_CHECKED(AUnrealAIMultiTurnExample, CancelTurn)})
	{
		const UFunction* Function = ExampleClass->FindFunctionByName(FunctionName);
		TestNotNull(*FString::Printf(TEXT("%s is reflected"), *FunctionName.ToString()), Function);
		if (Function)
		{
			TestTrue(
				*FString::Printf(TEXT("%s is Blueprint callable"), *FunctionName.ToString()),
				Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		}
	}

	const FProperty* HistoryProperty = FindFProperty<FProperty>(
		ExampleClass,
		GET_MEMBER_NAME_CHECKED(AUnrealAIMultiTurnExample, ConversationHistory));
	TestNotNull(TEXT("Conversation history is reflected"), HistoryProperty);
	if (HistoryProperty)
	{
		TestTrue(TEXT("Conversation history is Blueprint visible"), HistoryProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));
		TestTrue(TEXT("Conversation history is Blueprint read-only"), HistoryProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
	}

	for (const FName EventName : {
		GET_MEMBER_NAME_CHECKED(AUnrealAIMultiTurnExample, OnTurnCompleted),
		GET_MEMBER_NAME_CHECKED(AUnrealAIMultiTurnExample, OnTurnFailed),
		GET_MEMBER_NAME_CHECKED(AUnrealAIMultiTurnExample, OnTurnCancelled),
		GET_MEMBER_NAME_CHECKED(AUnrealAIMultiTurnExample, OnTurnRetrying),
		GET_MEMBER_NAME_CHECKED(AUnrealAIMultiTurnExample, OnTurnTextDelta)})
	{
		const FProperty* EventProperty = FindFProperty<FProperty>(ExampleClass, EventName);
		TestNotNull(*FString::Printf(TEXT("%s is reflected"), *EventName.ToString()), EventProperty);
		if (EventProperty)
		{
			TestTrue(
				*FString::Printf(TEXT("%s is Blueprint assignable"), *EventName.ToString()),
				EventProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleBackendFacadeContractTest,
	"UnrealAISample.SkillContracts.Blueprint.BackendFacade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleBackendFacadeContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UClass* FacadeClass = UUnrealAIPackagedClientFacadeExample::StaticClass();
	TestNotNull(TEXT("The packaged-client facade class exists"), FacadeClass);
	for (const FName FunctionName : {
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, StreamTurn),
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, CancelActiveTurn)})
	{
		const UFunction* Function = FacadeClass->FindFunctionByName(FunctionName);
		TestNotNull(*FString::Printf(TEXT("%s is reflected"), *FunctionName.ToString()), Function);
		if (Function)
		{
			TestTrue(
				*FString::Printf(TEXT("%s is Blueprint callable"), *FunctionName.ToString()),
				Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		}
	}

	enum class EExpectedDelegateParameter : uint8
	{
		Guid,
		String,
		Name,
		Integer,
		Float
	};
	const auto TestEventSignature = [this, FacadeClass](
		FName EventName,
		const TArray<TPair<FName, EExpectedDelegateParameter>>& ExpectedParameters)
	{
		const FMulticastDelegateProperty* EventProperty =
			FindFProperty<FMulticastDelegateProperty>(FacadeClass, EventName);
		TestNotNull(*FString::Printf(TEXT("%s is reflected"), *EventName.ToString()), EventProperty);
		if (!EventProperty)
		{
			return;
		}

		TestTrue(
			*FString::Printf(TEXT("%s is Blueprint assignable"), *EventName.ToString()),
			EventProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable));
		const UFunction* Signature = EventProperty->SignatureFunction.Get();
		if (!TestNotNull(TEXT("The delegate has a reflected signature"), Signature))
		{
			return;
		}

		TArray<const FProperty*> ActualParameters;
		for (TFieldIterator<FProperty> It(Signature); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm)
				&& !It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ActualParameters.Add(*It);
			}
		}
		TestEqual(
			*FString::Printf(TEXT("%s has the documented parameter count"), *EventName.ToString()),
			ActualParameters.Num(),
			ExpectedParameters.Num());
		const int32 ComparableCount = FMath::Min(
			ActualParameters.Num(),
			ExpectedParameters.Num());
		for (int32 Index = 0; Index < ComparableCount; ++Index)
		{
			const FProperty* Actual = ActualParameters[Index];
			const TPair<FName, EExpectedDelegateParameter>& Expected = ExpectedParameters[Index];
			TestEqual(
				*FString::Printf(TEXT("%s parameter %d has the documented name/order"), *EventName.ToString(), Index),
				Actual->GetFName(),
				Expected.Key);

			bool bTypeMatches = false;
			switch (Expected.Value)
			{
			case EExpectedDelegateParameter::Guid:
				if (const FStructProperty* StructProperty = CastField<FStructProperty>(Actual))
				{
					bTypeMatches = StructProperty->Struct == TBaseStructure<FGuid>::Get();
				}
				break;
			case EExpectedDelegateParameter::String:
				bTypeMatches = CastField<FStrProperty>(Actual) != nullptr;
				break;
			case EExpectedDelegateParameter::Name:
				bTypeMatches = CastField<FNameProperty>(Actual) != nullptr;
				break;
			case EExpectedDelegateParameter::Integer:
				bTypeMatches = CastField<FIntProperty>(Actual) != nullptr;
				break;
			case EExpectedDelegateParameter::Float:
				bTypeMatches = CastField<FFloatProperty>(Actual) != nullptr;
				break;
			}
			TestTrue(
				*FString::Printf(TEXT("%s parameter %s has the documented type"), *EventName.ToString(), *Expected.Key.ToString()),
				bTypeMatches);
		}
	};
	TestEventSignature(
		GET_MEMBER_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, OnTextDelta),
		{{TEXT("RequestId"), EExpectedDelegateParameter::Guid},
		 {TEXT("TextDelta"), EExpectedDelegateParameter::String}});
	TestEventSignature(
		GET_MEMBER_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, OnRetrying),
		{{TEXT("RequestId"), EExpectedDelegateParameter::Guid},
		 {TEXT("RetryNumber"), EExpectedDelegateParameter::Integer},
		 {TEXT("MaxRetries"), EExpectedDelegateParameter::Integer},
		 {TEXT("DelaySeconds"), EExpectedDelegateParameter::Float}});
	TestEventSignature(
		GET_MEMBER_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, OnCompleted),
		{{TEXT("RequestId"), EExpectedDelegateParameter::Guid},
		 {TEXT("FinalText"), EExpectedDelegateParameter::String}});
	TestEventSignature(
		GET_MEMBER_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, OnFailed),
		{{TEXT("RequestId"), EExpectedDelegateParameter::Guid},
		 {TEXT("PublicReason"), EExpectedDelegateParameter::Name},
		 {TEXT("PartialText"), EExpectedDelegateParameter::String}});
	TestEventSignature(
		GET_MEMBER_NAME_CHECKED(UUnrealAIPackagedClientFacadeExample, OnCancelled),
		{{TEXT("RequestId"), EExpectedDelegateParameter::Guid},
		 {TEXT("PartialText"), EExpectedDelegateParameter::String}});

	TestNull(
		TEXT("No API key is reflected to Blueprint"),
		FindFProperty<FProperty>(FacadeClass, TEXT("ApiKey")));
	TestNull(
		TEXT("No provider configuration is reflected to Blueprint"),
		FindFProperty<FProperty>(FacadeClass, TEXT("ProviderConfig")));
	TestNull(
		TEXT("Native session-token initialization is not reflected to Blueprint"),
		FacadeClass->FindFunctionByName(TEXT("Initialize")));
	const TSet<FName> ForbiddenBlueprintConfigurationNames = {
		TEXT("ApiKey"), TEXT("Token"), TEXT("BaseUrl"), TEXT("Url"),
		TEXT("Provider"), TEXT("ProviderConfig"), TEXT("Model"), TEXT("Headers"),
		TEXT("AdditionalParametersJson"), TEXT("BackendClient")};
	for (TFieldIterator<FProperty> PropertyIt(FacadeClass); PropertyIt; ++PropertyIt)
	{
		if ((*PropertyIt)->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			TestFalse(
				*FString::Printf(
					TEXT("Blueprint-visible facade property %s is not credential/configuration input"),
					*(*PropertyIt)->GetName()),
				ForbiddenBlueprintConfigurationNames.Contains((*PropertyIt)->GetFName()));
		}
	}

	UUnrealAIPackagedClientFacadeExample* Facade = NewObject<UUnrealAIPackagedClientFacadeExample>();
	FUnrealAIError InitializationError;
	TestFalse(TEXT("An empty game-session token is rejected"), Facade->Initialize(FString(), InitializationError));
	TestTrue(TEXT("The empty-token error is populated"), InitializationError.bIsError);
	TestTrue(
		TEXT("A synthetic short-lived game-session token initializes the facade"),
		Facade->Initialize(TEXT("synthetic-short-lived-session-token"), InitializationError));
	TestFalse(TEXT("Synthetic initialization reports no error"), InitializationError.bIsError);

	const FUnrealAIProviderConfig* BackendConfig =
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetBackendConfig(*Facade);
	if (TestNotNull(TEXT("The facade owns a configured backend client"), BackendConfig))
	{
		TestEqual(TEXT("The facade uses the compatible backend URL"), BackendConfig->BaseUrl, FString(TEXT("https://ai.example.invalid/v1")));
		TestTrue(TEXT("The facade does not read a hosted-provider environment key"), BackendConfig->ApiKeyEnvironmentVariable.IsEmpty());
	}

	TArray<FUnrealAIChatMessage> ValidMessages;
	FUnrealAIChatMessage UserMessage;
	UserMessage.Role = EUnrealAIMessageRole::User;
	UserMessage.Content = TEXT("Hello");
	ValidMessages.Add(UserMessage);
	FName FailureReason;
	TestTrue(
		TEXT("A bounded user message is accepted"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(ValidMessages, FailureReason));
	TArray<FUnrealAIChatMessage> EmptyMessages;
	TestFalse(
		TEXT("An empty conversation is rejected"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(EmptyMessages, FailureReason));
	TestEqual(TEXT("Empty input uses the message-count reason"), FailureReason, FName(TEXT("invalid_message_count")));

	TArray<FUnrealAIChatMessage> TooManyMessages;
	for (int32 Index = 0; Index < 18; ++Index)
	{
		TooManyMessages.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
			Index % 2 == 0 ? EUnrealAIMessageRole::User : EUnrealAIMessageRole::Assistant,
			TEXT("bounded")));
	}
	TestFalse(
		TEXT("More than seventeen messages are rejected before transport"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(TooManyMessages, FailureReason));
	TestEqual(TEXT("Excess input uses the message-count reason"), FailureReason, FName(TEXT("invalid_message_count")));

	TArray<FUnrealAIChatMessage> InvalidContentMessages = ValidMessages;
	InvalidContentMessages[0].Content = TEXT(" \t\r\n ");
	TestFalse(
		TEXT("Whitespace-only message content is rejected"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(InvalidContentMessages, FailureReason));
	TestEqual(TEXT("Blank input uses the content reason"), FailureReason, FName(TEXT("invalid_message_content")));
	InvalidContentMessages[0].Content = FString::ChrN(2049, TCHAR('x'));
	TestFalse(
		TEXT("A message above the per-message character limit is rejected"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(InvalidContentMessages, FailureReason));
	TestEqual(TEXT("Oversized input uses the content reason"), FailureReason, FName(TEXT("invalid_message_content")));
	const FString AstralEmoji(UTF8_TO_TCHAR("\xF0\x9F\x98\x80"));
	TestEqual(TEXT("The astral fixture occupies one UTF-16 surrogate pair"), AstralEmoji.Len(), 2);
	InvalidContentMessages[0].Content = FString::ChrN(2047, TCHAR(0x00E9));
	InvalidContentMessages[0].Content += TCHAR(0x00E9);
	TestTrue(
		TEXT("BMP non-ASCII input is accepted at the exact FString length bound"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(InvalidContentMessages, FailureReason));
	InvalidContentMessages[0].Content = FString::ChrN(2046, TCHAR('x')) + AstralEmoji;
	TestTrue(
		TEXT("An astral scalar is accepted when its full pair fits the length bound"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(InvalidContentMessages, FailureReason));
	InvalidContentMessages[0].Content = FString::ChrN(2047, TCHAR('x')) + AstralEmoji;
	TestFalse(
		TEXT("An astral scalar crossing the length bound is rejected as a whole input"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(InvalidContentMessages, FailureReason));
	TestEqual(TEXT("Astral overflow uses the content reason"), FailureReason, FName(TEXT("invalid_message_content")));

	TArray<FUnrealAIChatMessage> TooLargeConversation;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TooLargeConversation.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
			Index % 2 == 0 ? EUnrealAIMessageRole::User : EUnrealAIMessageRole::Assistant,
			FString::ChrN(2048, TCHAR('x'))));
	}
	TestFalse(
		TEXT("Aggregate message content above the conversation limit is rejected"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(TooLargeConversation, FailureReason));
	TestEqual(TEXT("Aggregate overflow uses the conversation reason"), FailureReason, FName(TEXT("conversation_too_large")));

	ValidMessages[0].Content = TEXT("  Hello  ");
	const FGuid PayloadRequestId = FGuid::NewGuid();
	const FUnrealAIChatRequest BackendRequest =
		FUnrealAIPackagedClientFacadeExampleTestAccess::BuildBackendRequest(PayloadRequestId, ValidMessages);
	TestEqual(TEXT("The facade canonicalizes message text before transport"), BackendRequest.Messages[0].Content, FString(TEXT("Hello")));
	TestEqual(
		TEXT("The stable game request ID crosses the backend boundary"),
		BackendRequest.AdditionalParametersJson,
		FString::Printf(
			TEXT("{\"game_request_id\":\"%s\"}"),
			*PayloadRequestId.ToString(EGuidFormats::DigitsWithHyphensLower)));
	ValidMessages[0].Content = TEXT("Hello");

	TArray<FUnrealAIChatMessage> SystemMessages = ValidMessages;
	SystemMessages[0].Role = EUnrealAIMessageRole::System;
	TestFalse(
		TEXT("A client-authored system message is rejected"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(SystemMessages, FailureReason));
	TestEqual(TEXT("Role rejection uses a stable public reason"), FailureReason, FName(TEXT("invalid_role_order")));

	TArray<FUnrealAIChatMessage> AdvancedMessages = ValidMessages;
	AdvancedMessages[0].AdditionalFieldsJson = TEXT("{\"provider_specific\":true}");
	TestFalse(
		TEXT("Provider-specific message fields are rejected at the facade"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(AdvancedMessages, FailureReason));
	TestEqual(TEXT("Advanced-field rejection uses a stable public reason"), FailureReason, FName(TEXT("unsupported_message_fields")));
	AdvancedMessages = ValidMessages;
	AdvancedMessages[0].ContentJson = TEXT("[{\"type\":\"text\",\"text\":\"hidden\"}]");
	TestFalse(
		TEXT("Structured content is rejected at the facade"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(AdvancedMessages, FailureReason));
	AdvancedMessages = ValidMessages;
	AdvancedMessages[0].Name = TEXT("client-name");
	TestFalse(
		TEXT("Client-supplied message names are rejected at the facade"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(AdvancedMessages, FailureReason));
	AdvancedMessages = ValidMessages;
	AdvancedMessages[0].ToolCallId = TEXT("client-tool-call");
	TestFalse(
		TEXT("Client-supplied tool-call IDs are rejected at the facade"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(AdvancedMessages, FailureReason));
	TestEqual(
		TEXT("HTTP 401 requests native session refresh"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::MapHttpFailure(401),
		FName(TEXT("session_expired")));
	TestEqual(
		TEXT("HTTP 403 remains a distinct authorization failure"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::MapHttpFailure(403),
		FName(TEXT("access_denied")));

	TArray<FUnrealAIChatMessage> AssistantLastMessages = ValidMessages;
	FUnrealAIChatMessage AssistantMessage;
	AssistantMessage.Role = EUnrealAIMessageRole::Assistant;
	AssistantMessage.Content = TEXT("Hello back");
	AssistantLastMessages.Add(AssistantMessage);
	TestFalse(
		TEXT("A conversation ending in an assistant message is rejected"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::ValidateMessages(AssistantLastMessages, FailureReason));
	TestEqual(TEXT("Terminal-role rejection uses a stable public reason"), FailureReason, FName(TEXT("last_message_must_be_user")));

	const FGuid SyntheticRequestId = FGuid::NewGuid();
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, SyntheticRequestId);
	FName ConcurrentReason;
	TestFalse(
		TEXT("The facade rejects a concurrent stream"),
		Facade->StreamTurn(FGuid::NewGuid(), ValidMessages, ConcurrentReason));
	TestEqual(TEXT("Concurrent rejection uses the stable public reason"), ConcurrentReason, FName(TEXT("chat_busy")));
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, TEXT("Backend "));
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitProviderEvent(*Facade);
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, TEXT("text"));
	TestEqual(
		TEXT("The facade retains only normalized text deltas"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetPartialText(*Facade),
		FString(TEXT("Backend text")));
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Failed);
	TestFalse(TEXT("A terminal event clears the facade in-flight flag"), Facade->bRequestInFlight);
	TestFalse(TEXT("A terminal event invalidates the request ID"), Facade->ActiveRequestId.IsValid());

	const FString ExactAstralBoundary = FString::ChrN(2046, TCHAR('x')) + AstralEmoji;
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, FGuid::NewGuid());
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, ExactAstralBoundary);
	TestEqual(
		TEXT("Incremental output retains an astral scalar when its full pair fits"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetPartialText(*Facade),
		ExactAstralBoundary);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Cancelled);

	const FString CrossingAstralBoundary = FString::ChrN(2047, TCHAR('x')) + AstralEmoji;
	const FString SafeAstralPrefix = FString::ChrN(2047, TCHAR('x'));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, FGuid::NewGuid());
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, CrossingAstralBoundary);
	TestEqual(
		TEXT("Incremental output never retains half an astral scalar"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetPartialText(*Facade),
		SafeAstralPrefix);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Cancelled);
	TestEqual(
		TEXT("A scalar crossing the output bound reports the bounded public reason"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetLastTerminalFailureReason(*Facade),
		FName(TEXT("response_too_large")));

	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, FGuid::NewGuid());
	const int32 TerminalCountBeforeOutputLimit =
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetTerminalBroadcastCount(*Facade);
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, FString::ChrN(17000, TCHAR('x')));
	TestEqual(
		TEXT("The facade bounds accumulated output"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetPartialText(*Facade).Len(),
		2048);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Cancelled);
	TestEqual(
		TEXT("Incremental output overflow emits exactly one terminal event"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetTerminalBroadcastCount(*Facade),
		TerminalCountBeforeOutputLimit + 1);
	TestEqual(
		TEXT("Incremental output overflow reports the bounded public reason"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetLastTerminalFailureReason(*Facade),
		FName(TEXT("response_too_large")));
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Cancelled);
	TestEqual(
		TEXT("A duplicate transport terminal cannot emit another public terminal event"),
		FUnrealAIPackagedClientFacadeExampleTestAccess::GetTerminalBroadcastCount(*Facade),
		TerminalCountBeforeOutputLimit + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleGeneratedBackendWidgetContractTest,
	"UnrealAISample.SkillContracts.Blueprint.GeneratedBackendWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleGeneratedBackendWidgetContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UnrealAISampleTests;

	UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, BackendWidgetObjectPath);
	if (!TestNotNull(TEXT("The packaged-chat Widget Blueprint loads"), WidgetBlueprint))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	TestTrue(TEXT("The packaged-chat Widget Blueprint compiles"), WidgetBlueprint->Status != BS_Error);
	TestNotNull(TEXT("The packaged-chat generated class exists"), WidgetBlueprint->GeneratedClass.Get());
	TestEqual(
		TEXT("The Widget Blueprint inherits the credential-free packaged-chat controller"),
		WidgetBlueprint->ParentClass.Get(),
		UUnrealAIPackagedChatWidgetExample::StaticClass());

	TSet<FName> RequiredEvents = {
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveBackendReadinessChanged),
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveTextDelta),
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveRetrying),
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveCompleted),
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveFailed),
		GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveCancelled)};
	for (const UEdGraph* Graph : WidgetBlueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}
		for (const UEdGraphNode* GraphNode : Graph->Nodes)
		{
			if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(GraphNode))
			{
				RequiredEvents.Remove(EventNode->EventReference.GetMemberName());
			}
		}
	}
	TestEqual(TEXT("All six packaged-chat presentation events are generated"), RequiredEvents.Num(), 0);
	if (TestNotNull(TEXT("The generated Widget Blueprint owns a widget tree"), WidgetBlueprint->WidgetTree.Get()))
	{
		TestNull(TEXT("The generated skeleton has no root layout widget"), WidgetBlueprint->WidgetTree->RootWidget.Get());
		TArray<UWidget*> GeneratedWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(GeneratedWidgets);
		TestTrue(TEXT("The generated skeleton contains no layout widgets"), GeneratedWidgets.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISamplePackagedWidgetBehaviorContractTest,
	"UnrealAISample.SkillContracts.Blueprint.PackagedWidgetBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISamplePackagedWidgetBehaviorContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UnrealAISampleTests;
	const FString AstralEmoji(UTF8_TO_TCHAR("\xF0\x9F\x98\x80"));

	FContractWorld ContractWorld;
	if (!TestTrue(TEXT("Creates a packaged-widget contract world"), ContractWorld.Initialize()))
	{
		return false;
	}
	UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, BackendWidgetObjectPath);
	if (!TestNotNull(TEXT("Loads the generated packaged-chat widget"), WidgetBlueprint))
	{
		return false;
	}
	UUnrealAIPackagedChatWidgetExample* Widget = CreateWidget<UUnrealAIPackagedChatWidgetExample>(
		ContractWorld.GameInstance,
		WidgetBlueprint->GeneratedClass.Get());
	if (!TestNotNull(TEXT("Instantiates the generated packaged-chat widget"), Widget))
	{
		return false;
	}

	UUnrealAIPackagedClientFacadeExample* UninitializedFacade =
		NewObject<UUnrealAIPackagedClientFacadeExample>(Widget);
	TestFalse(
		TEXT("The native widget boundary rejects an uninitialized facade"),
		Widget->AttachInitializedFacade(UninitializedFacade));
	UUnrealAIPackagedClientFacadeExample* Facade = NewObject<UUnrealAIPackagedClientFacadeExample>(Widget);
	FUnrealAIError InitializationError;
	TestTrue(
		TEXT("Initializes the widget's synthetic backend facade"),
		Facade->Initialize(TEXT("synthetic-short-lived-session-token"), InitializationError));
	TestTrue(TEXT("The native session boundary attaches the facade"), Widget->AttachInitializedFacade(Facade));
	TestTrue(TEXT("Initial facade attachment marks the backend ready"), Widget->bBackendReady);

	FUnrealAIPackagedChatWidgetExampleTestAccess::SetStreamTurnHook(
		*Widget,
		[Facade](
			const FGuid& RequestId,
			const TArray<FUnrealAIChatMessage>& Messages,
			FName& OutFailureReason)
		{
			(void)Messages;
			OutFailureReason = NAME_None;
			Facade->OnCompleted.Broadcast(RequestId, TEXT("  synchronous answer  "));
			return true;
		});
	FName PublicSubmitReason;
	TestTrue(
		TEXT("The public SubmitTurn path tolerates a synchronous completion"),
		Widget->SubmitTurn(TEXT("  synchronous user  "), PublicSubmitReason));
	TestEqual(
		TEXT("Synchronous completion wins before SubmitTurn returns"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Idle);
	TestEqual(TEXT("Synchronous public completion commits one pair"), Widget->CommittedHistory.Num(), 2);
	if (Widget->CommittedHistory.Num() == 2)
	{
		TestEqual(
			TEXT("Public submission commits canonical user content"),
			Widget->CommittedHistory[0].Content,
			FString(TEXT("synchronous user")));
		TestEqual(
			TEXT("Public synchronous completion commits canonical assistant content"),
			Widget->CommittedHistory[1].Content,
			FString(TEXT("synchronous answer")));
	}

	FUnrealAIPackagedChatWidgetExampleTestAccess::SetStreamTurnHook(
		*Widget,
		[Facade](
			const FGuid& RequestId,
			const TArray<FUnrealAIChatMessage>& Messages,
			FName& OutFailureReason)
		{
			(void)Messages;
			OutFailureReason = NAME_None;
			Facade->OnFailed.Broadcast(
				RequestId,
				TEXT("service_unavailable"),
				TEXT("synchronous partial"));
			return true;
		});
	TestTrue(
		TEXT("A synchronous terminal still means the public request was accepted"),
		Widget->SubmitTurn(TEXT("retry synchronously"), PublicSubmitReason));
	TestEqual(
		TEXT("Synchronous failure is terminal before SubmitTurn returns"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Failed);
	TestEqual(
		TEXT("Synchronous failure preserves canonical retry input"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::GetRetryUserText(*Widget),
		FString(TEXT("retry synchronously")));

	FUnrealAIPackagedChatWidgetExampleTestAccess::SetStreamTurnHook(
		*Widget,
		[Facade](
			const FGuid& RequestId,
			const TArray<FUnrealAIChatMessage>& Messages,
			FName& OutFailureReason)
		{
			(void)Messages;
			OutFailureReason = NAME_None;
			Facade->OnCompleted.Broadcast(RequestId, TEXT("retry completed"));
			return true;
		});
	TestTrue(
		TEXT("RetryLastTurn tolerates a synchronous completion"),
		Widget->RetryLastTurn(PublicSubmitReason));
	TestEqual(
		TEXT("Synchronous retry completion wins before RetryLastTurn returns"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Idle);
	TestEqual(TEXT("Synchronous retry commits one additional pair"), Widget->CommittedHistory.Num(), 4);

	FUnrealAIPackagedChatWidgetExampleTestAccess::SetStreamTurnHook(
		*Widget,
		[](
			const FGuid& RequestId,
			const TArray<FUnrealAIChatMessage>& Messages,
			FName& OutFailureReason)
		{
			(void)RequestId;
			(void)Messages;
			OutFailureReason = TEXT("request_not_started");
			return false;
		});
	TestFalse(
		TEXT("The public SubmitTurn path reports a local transport rejection"),
		Widget->SubmitTurn(TEXT("  locally rejected  "), PublicSubmitReason));
	TestEqual(
		TEXT("Local transport rejection becomes a failed turn"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Failed);
	TestEqual(
		TEXT("Local transport rejection updates the public failure property"),
		Widget->LastPublicFailureReason,
		FName(TEXT("request_not_started")));
	TestEqual(
		TEXT("Local transport rejection preserves canonical retry input"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::GetRetryUserText(*Widget),
		FString(TEXT("locally rejected")));

	FUnrealAIPackagedChatWidgetExampleTestAccess::SetStreamTurnHook(
		*Widget,
		[Facade](
			const FGuid& RequestId,
			const TArray<FUnrealAIChatMessage>& Messages,
			FName& OutFailureReason)
		{
			(void)Messages;
			OutFailureReason = TEXT("request_not_started");
			Facade->OnFailed.Broadcast(
				RequestId,
				TEXT("service_unavailable"),
				TEXT("terminal won"));
			return false;
		});
	TestFalse(
		TEXT("A synchronous terminal does not change the transport's false return"),
		Widget->RetryLastTurn(PublicSubmitReason));
	TestEqual(
		TEXT("A synchronous terminal wins even when the transport returns false"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Failed);
	TestEqual(
		TEXT("The transport return cannot overwrite the synchronous terminal reason"),
		Widget->LastPublicFailureReason,
		FName(TEXT("service_unavailable")));
	TestEqual(
		TEXT("The synchronous retry terminal retains its canonical input"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::GetRetryUserText(*Widget),
		FString(TEXT("locally rejected")));

	FUnrealAIPackagedChatWidgetExampleTestAccess::ClearStreamTurnHook(*Widget);
	Widget->CommittedHistory.Reset();

	const FGuid CompletedId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, CompletedId, TEXT("First user"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, CompletedId);
	Facade->OnTextDelta.Broadcast(FGuid::NewGuid(), TEXT("stale"));
	Facade->OnRetrying.Broadcast(FGuid::NewGuid(), 1, 2, 0.25f);
	Facade->OnCompleted.Broadcast(FGuid::NewGuid(), TEXT("stale terminal"));
	Facade->OnFailed.Broadcast(FGuid::NewGuid(), TEXT("service_unavailable"), TEXT("stale failure"));
	Facade->OnCancelled.Broadcast(FGuid::NewGuid(), TEXT("stale cancellation"));
	TestTrue(TEXT("A stale request ID cannot mutate pending text"), Widget->PendingAssistantText.IsEmpty());
	TestEqual(
		TEXT("Stale lifecycle events cannot change request state"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Starting);
	TestTrue(TEXT("Stale terminal events cannot commit history"), Widget->CommittedHistory.IsEmpty());
	TestEqual(TEXT("Stale terminal events cannot replace the active ID"), Widget->ActiveRequestId, CompletedId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, TEXT("partial "));
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitProviderEvent(*Facade);
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitRetry(*Facade);
	TestEqual(TEXT("Retry progress does not commit history"), Widget->CommittedHistory.Num(), 0);
	TestEqual(TEXT("Retry progress retains partial display text"), Widget->PendingAssistantText, FString(TEXT("partial ")));
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*Facade,
		EUnrealAIChatStreamStatus::Completed,
		TEXT("  final answer  "));
	TestEqual(TEXT("Completion commits one user/assistant pair"), Widget->CommittedHistory.Num(), 2);
	if (Widget->CommittedHistory.Num() == 2)
	{
		TestEqual(TEXT("The committed user is exact"), Widget->CommittedHistory[0].Content, FString(TEXT("First user")));
		TestEqual(TEXT("The committed assistant is canonicalized"), Widget->CommittedHistory[1].Content, FString(TEXT("final answer")));
	}
	Facade->OnCompleted.Broadcast(CompletedId, TEXT("duplicate"));
	TestEqual(TEXT("A duplicate terminal cannot commit twice"), Widget->CommittedHistory.Num(), 2);

	const FGuid FailedId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, FailedId, TEXT("Retry me"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, FailedId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, TEXT("interrupted"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Failed);
	TestEqual(TEXT("Failure leaves committed history unchanged"), Widget->CommittedHistory.Num(), 2);
	TestEqual(TEXT("Failure preserves bounded display-only partial text"), Widget->InterruptedAssistantText, FString(TEXT("interrupted")));
	TestEqual(TEXT("Failure preserves the explicit retry input"), FUnrealAIPackagedChatWidgetExampleTestAccess::GetRetryUserText(*Widget), FString(TEXT("Retry me")));
	Facade->bRequestInFlight = true;
	FName RetryReason;
	TestFalse(
		TEXT("The actual retry path reports a local facade rejection"),
		Widget->RetryLastTurn(RetryReason));
	TestEqual(TEXT("The rejected retry reports chat_busy"), RetryReason, FName(TEXT("chat_busy")));
	TestEqual(
		TEXT("A retry rejected before start preserves the saved input"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::GetRetryUserText(*Widget),
		FString(TEXT("Retry me")));
	Facade->bRequestInFlight = false;
	Facade->ActiveRequestId.Invalidate();

	const FGuid EmptyId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, EmptyId, TEXT("Whitespace"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, EmptyId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*Facade,
		EUnrealAIChatStreamStatus::Completed,
		TEXT("   "));
	TestEqual(TEXT("Whitespace-only completion is rejected"), Widget->RequestState, EUnrealAIPackagedChatState::Failed);
	TestEqual(TEXT("Whitespace-only completion cannot poison history"), Widget->CommittedHistory.Num(), 2);

	const FGuid OversizedId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, OversizedId, TEXT("Oversized"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, OversizedId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*Facade,
		EUnrealAIChatStreamStatus::Completed,
		FString::ChrN(2047, TCHAR('x')) + AstralEmoji);
	TestEqual(TEXT("Oversized completion is rejected"), Widget->RequestState, EUnrealAIPackagedChatState::Failed);
	TestEqual(
		TEXT("Oversized completion exposes the bounded public reason"),
		Widget->LastPublicFailureReason,
		FName(TEXT("response_too_large")));
	TestEqual(
		TEXT("Oversized completion exposes no unpaired surrogate"),
		Widget->InterruptedAssistantText,
		FString::ChrN(2047, TCHAR('x')));
	TestEqual(TEXT("Oversized completion cannot poison history"), Widget->CommittedHistory.Num(), 2);

	const FGuid CancelledId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, CancelledId, TEXT("Cancel me"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, CancelledId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::EmitText(*Facade, TEXT("partial cancellation"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Cancelled);
	TestEqual(TEXT("Cancellation is terminal"), Widget->RequestState, EUnrealAIPackagedChatState::Cancelled);
	TestEqual(TEXT("Cancellation leaves committed history unchanged"), Widget->CommittedHistory.Num(), 2);
	TestEqual(TEXT("Cancellation preserves display-only partial text"), Widget->InterruptedAssistantText, FString(TEXT("partial cancellation")));

	const FGuid ActiveId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, ActiveId, TEXT("Active"));
	TestFalse(TEXT("Cancel reports false when the facade has no matching request"), Widget->CancelTurn());
	TestEqual(
		TEXT("A rejected cancel restores the previous widget state"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Starting);
	FUnrealAIPackagedChatWidgetExampleTestAccess::SimulateTerminalDuringRejectedCancel(
		*Widget,
		ActiveId,
		EUnrealAIPackagedChatState::Starting);
	TestEqual(
		TEXT("A synchronous terminal during cancel is not overwritten by old state"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Failed);
	TestFalse(TEXT("The cancel-race terminal clears the active request ID"), Widget->ActiveRequestId.IsValid());

	const FGuid ConcurrentId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, ConcurrentId, TEXT("Active"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, ConcurrentId);
	FName ConcurrentReason;
	TestFalse(TEXT("The widget rejects a concurrent turn"), Widget->SubmitTurn(TEXT("Second"), ConcurrentReason));
	TestEqual(TEXT("Concurrent rejection uses a stable reason"), ConcurrentReason, FName(TEXT("chat_busy")));
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(*Facade, EUnrealAIChatStreamStatus::Cancelled);

	const FGuid RefreshId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, RefreshId, TEXT("Refresh me"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*Facade, RefreshId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*Facade,
		EUnrealAIChatStreamStatus::Failed,
		FString(),
		401);
	TestEqual(
		TEXT("An expired session is terminal before facade replacement"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Failed);
	TestFalse(TEXT("The expired request ID is cleared"), Widget->ActiveRequestId.IsValid());
	TestFalse(TEXT("Session expiry marks the backend unavailable for retry"), Widget->bBackendReady);
	FName RefreshBlockedReason;
	TestFalse(
		TEXT("Explicit retry is blocked while native token refresh is pending"),
		Widget->RetryLastTurn(RefreshBlockedReason));
	TestEqual(
		TEXT("Refresh-pending retry uses a stable presentation reason"),
		RefreshBlockedReason,
		FName(TEXT("backend_refreshing")));
	TestEqual(
		TEXT("A refresh-pending retry preserves the canonical saved input"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::GetRetryUserText(*Widget),
		FString(TEXT("Refresh me")));
	TestTrue(
		TEXT("All five old-facade lifecycle delegates are initially bound to the widget"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::AreAllFacadeDelegatesBound(*Facade, *Widget));

	UUnrealAIPackagedClientFacadeExample* ReplacementFacade =
		NewObject<UUnrealAIPackagedClientFacadeExample>(Widget);
	TestTrue(
		TEXT("A replacement facade accepts a refreshed synthetic session"),
		ReplacementFacade->Initialize(TEXT("refreshed-synthetic-session-token"), InitializationError));
	TestTrue(
		TEXT("Native session code can attach a replacement after terminal delivery"),
		Widget->AttachInitializedFacade(ReplacementFacade));
	TestTrue(TEXT("Replacement attachment signals backend readiness"), Widget->bBackendReady);
	TestEqual(
		TEXT("Replacement attachment preserves the explicit retry state"),
		Widget->RequestState,
		EUnrealAIPackagedChatState::Failed);
	TestFalse(
		TEXT("Facade replacement removes all five bindings from the old facade"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::IsAnyFacadeDelegateBound(*Facade, *Widget));
	TestTrue(
		TEXT("Facade replacement binds all five lifecycle delegates to the new facade"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::AreAllFacadeDelegatesBound(*ReplacementFacade, *Widget));

	const FGuid AccessDeniedId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, AccessDeniedId, TEXT("Forbidden"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*ReplacementFacade, AccessDeniedId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*ReplacementFacade,
		EUnrealAIChatStreamStatus::Failed,
		FString(),
		403);
	TestTrue(TEXT("Authorization failure does not start a token refresh"), Widget->bBackendReady);
	FName AccessDeniedRetryReason;
	TestFalse(
		TEXT("Authorization failure is not exposed as a retryable turn"),
		Widget->RetryLastTurn(AccessDeniedRetryReason));
	TestEqual(
		TEXT("Authorization failure has no retry payload"),
		AccessDeniedRetryReason,
		FName(TEXT("nothing_to_retry")));

	Widget->CommittedHistory.Reset();
	for (int32 PairIndex = 0; PairIndex < 10; ++PairIndex)
	{
		Widget->CommittedHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
			EUnrealAIMessageRole::User,
			FString::ChrN(600, TCHAR('u'))));
		Widget->CommittedHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
			EUnrealAIMessageRole::Assistant,
			FString::ChrN(600, TCHAR('a'))));
	}
	const TArray<FUnrealAIChatMessage> BoundedRequestHistory =
		FUnrealAIPackagedChatWidgetExampleTestAccess::BuildRequestHistory(*Widget, TEXT("next"));
	TestEqual(
		TEXT("Request-time history pruning does not mutate committed history"),
		Widget->CommittedHistory.Num(),
		20);
	TestTrue(TEXT("Staged history eviction preserves only complete pairs"), BoundedRequestHistory.Num() % 2 == 0);
	TestTrue(TEXT("Staged history eviction reserves the pending request slot"), BoundedRequestHistory.Num() < 16);
	TestTrue(
		TEXT("Staged history eviction enforces the full character limit"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::CountHistoryCharacters(BoundedRequestHistory) + 4 <= 8192);
	for (int32 Index = 0; Index < BoundedRequestHistory.Num(); ++Index)
	{
		TestEqual(
			TEXT("Staged history remains canonical user/assistant pairs"),
			BoundedRequestHistory[Index].Role,
			Index % 2 == 0 ? EUnrealAIMessageRole::User : EUnrealAIMessageRole::Assistant);
	}
	const FGuid BoundedFailureId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, BoundedFailureId, TEXT("next"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*ReplacementFacade, BoundedFailureId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*ReplacementFacade,
		EUnrealAIChatStreamStatus::Failed);
	TestEqual(
		TEXT("Failure at the history bound leaves committed pairs unchanged"),
		Widget->CommittedHistory.Num(),
		20);
	const FGuid BoundedCancelId = FGuid::NewGuid();
	FUnrealAIPackagedChatWidgetExampleTestAccess::BeginSyntheticTurn(*Widget, BoundedCancelId, TEXT("next"));
	FUnrealAIPackagedClientFacadeExampleTestAccess::BeginSyntheticStream(*ReplacementFacade, BoundedCancelId);
	FUnrealAIPackagedClientFacadeExampleTestAccess::Finish(
		*ReplacementFacade,
		EUnrealAIChatStreamStatus::Cancelled);
	TestEqual(
		TEXT("Cancellation at the history bound leaves committed pairs unchanged"),
		Widget->CommittedHistory.Num(),
		20);

	FUnrealAIPackagedChatWidgetExampleTestAccess::Destruct(*Widget);
	TestFalse(
		TEXT("Widget teardown removes all five facade delegates"),
		FUnrealAIPackagedChatWidgetExampleTestAccess::IsAnyFacadeDelegateBound(*ReplacementFacade, *Widget));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleStreamingConversationBehaviorContractTest,
	"UnrealAISample.SkillContracts.Cpp.StreamingConversationBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleStreamingConversationBehaviorContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UnrealAISampleTests;

	FContractWorld ContractWorld;
	if (!TestTrue(TEXT("Creates a streaming-conversation contract world"), ContractWorld.Initialize()))
	{
		return false;
	}

	AUnrealAIMultiTurnExample* Example = ContractWorld.World->SpawnActor<AUnrealAIMultiTurnExample>();
	if (!TestNotNull(TEXT("Spawns the streaming-conversation example"), Example))
	{
		return false;
	}

	Example->SystemPrompt = TEXT("Streaming conversation system prompt");
	TestTrue(TEXT("The streaming conversation resets"), Example->ResetConversation());

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingStream(*Example, TEXT("First user turn"));
	const int32 PendingHistorySize = Example->ConversationHistory.Num();
	FUnrealAIMultiTurnExampleTestAccess::EmitStreamDelta(*Example, TEXT("Incremental "));
	FUnrealAIMultiTurnExampleTestAccess::EmitProviderEvent(*Example);
	FUnrealAIMultiTurnExampleTestAccess::EmitStreamDelta(*Example, TEXT("text"));
	TestEqual(
		TEXT("Only normalized text deltas enter the pending display buffer"),
		Example->PendingAssistantText,
		FString(TEXT("Incremental text")));

	FUnrealAIMultiTurnExampleTestAccess::RetryStream(*Example);
	TestTrue(TEXT("Retry remains non-terminal"), Example->bRequestInFlight);
	TestEqual(TEXT("Retry does not mutate pending history"), Example->ConversationHistory.Num(), PendingHistorySize);

	FUnrealAIMultiTurnExampleTestAccess::FinishStream(
		*Example,
		EUnrealAIChatStreamStatus::Completed,
		TEXT("Normalized aggregate"));
	TestFalse(TEXT("Completion clears the in-flight flag"), Example->bRequestInFlight);
	TestTrue(TEXT("Completion clears the pending display buffer"), Example->PendingAssistantText.IsEmpty());
	TestTrue(TEXT("Completion clears interrupted text"), Example->LastInterruptedAssistantText.IsEmpty());
	TestEqual(TEXT("Completion commits exactly one user/assistant pair"), Example->ConversationHistory.Num(), 3);
	if (Example->ConversationHistory.Num() == 3)
	{
		TestEqual(TEXT("The streamed user role is committed"), Example->ConversationHistory[1].Role, EUnrealAIMessageRole::User);
		TestEqual(TEXT("The aggregate assistant role is committed"), Example->ConversationHistory[2].Role, EUnrealAIMessageRole::Assistant);
		TestEqual(TEXT("The aggregate replaces the delta buffer for history"), Example->ConversationHistory[2].Content, FString(TEXT("Normalized aggregate")));
	}

	int32 CompletedHistorySize = Example->ConversationHistory.Num();
	FUnrealAIMultiTurnExampleTestAccess::FinishStream(
		*Example,
		EUnrealAIChatStreamStatus::Completed,
		TEXT("Duplicate terminal"));
	TestEqual(TEXT("A duplicate terminal cannot commit twice"), Example->ConversationHistory.Num(), CompletedHistorySize);

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingStream(*Example, TEXT("Failed user turn"));
	FUnrealAIMultiTurnExampleTestAccess::EmitStreamDelta(*Example, TEXT("Failed partial"));
	FUnrealAIMultiTurnExampleTestAccess::FinishStream(*Example, EUnrealAIChatStreamStatus::Failed);
	TestFalse(TEXT("Failure clears the in-flight flag"), Example->bRequestInFlight);
	TestEqual(TEXT("Failure rolls back the pending user"), Example->ConversationHistory.Num(), CompletedHistorySize);
	TestEqual(TEXT("Failure preserves partial text for display only"), Example->LastInterruptedAssistantText, FString(TEXT("Failed partial")));

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingStream(*Example, TEXT("Failed user turn"));
	TestEqual(TEXT("Explicit retry keeps committed history unchanged while pending"), Example->ConversationHistory.Num(), CompletedHistorySize);
	FUnrealAIMultiTurnExampleTestAccess::FinishStream(
		*Example,
		EUnrealAIChatStreamStatus::Completed,
		TEXT("Retry aggregate"));
	TestEqual(TEXT("Successful explicit retry commits one complete pair"), Example->ConversationHistory.Num(), CompletedHistorySize + 2);
	CompletedHistorySize = Example->ConversationHistory.Num();

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingStream(*Example, TEXT("Cancelled user turn"));
	FUnrealAIMultiTurnExampleTestAccess::EmitStreamDelta(*Example, TEXT("Cancelled partial"));
	FUnrealAIMultiTurnExampleTestAccess::FinishStream(*Example, EUnrealAIChatStreamStatus::Cancelled);
	TestFalse(TEXT("Cancellation clears the in-flight flag"), Example->bRequestInFlight);
	TestEqual(TEXT("Cancellation rolls back the pending user"), Example->ConversationHistory.Num(), CompletedHistorySize);
	TestEqual(TEXT("Cancellation preserves partial text for display only"), Example->LastInterruptedAssistantText, FString(TEXT("Cancelled partial")));

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingStream(*Example, TEXT("Active user turn"));
	const int32 ActiveHistorySize = Example->ConversationHistory.Num();
	FUnrealAIError ConcurrentError;
	TestFalse(TEXT("A concurrent turn is rejected"), Example->SendTurnStream(TEXT("Second active turn"), ConcurrentError));
	TestTrue(TEXT("Concurrent rejection reports an error"), ConcurrentError.bIsError);
	TestEqual(TEXT("Concurrent rejection uses the stable code"), ConcurrentError.Code, FString(TEXT("turn_in_progress")));
	TestEqual(TEXT("Concurrent rejection does not append history"), Example->ConversationHistory.Num(), ActiveHistorySize);
	FUnrealAIMultiTurnExampleTestAccess::FinishStream(*Example, EUnrealAIChatStreamStatus::Cancelled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleMultiTurnBehaviorContractTest,
	"UnrealAISample.SkillContracts.Cpp.MultiTurnBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleMultiTurnBehaviorContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UnrealAISampleTests;

	FContractWorld ContractWorld;
	if (!TestTrue(TEXT("Creates a contract-test world"), ContractWorld.Initialize()))
	{
		return false;
	}

	AUnrealAIMultiTurnExample* Example = ContractWorld.World->SpawnActor<AUnrealAIMultiTurnExample>();
	if (!TestNotNull(TEXT("Spawns the multi-turn example"), Example))
	{
		return false;
	}

	Example->SystemPrompt = TEXT("Contract system prompt");
	TestTrue(TEXT("The conversation resets"), Example->ResetConversation());
	TestEqual(TEXT("Reset inserts one system message"), Example->ConversationHistory.Num(), 1);
	if (Example->ConversationHistory.Num() == 1)
	{
		TestEqual(TEXT("The first message has the system role"), Example->ConversationHistory[0].Role, EUnrealAIMessageRole::System);
		TestEqual(TEXT("The system prompt is preserved"), Example->ConversationHistory[0].Content, Example->SystemPrompt);
	}

	TestTrue(TEXT("A second reset succeeds"), Example->ResetConversation());
	TestEqual(TEXT("Reset does not duplicate the system message"), Example->ConversationHistory.Num(), 1);

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingTurn(*Example, TEXT("First user turn"));
	FUnrealAIMultiTurnExampleTestAccess::Complete(*Example, TEXT("First assistant turn"));
	TestFalse(TEXT("Completion clears the in-flight flag"), Example->bRequestInFlight);
	TestEqual(TEXT("A successful turn commits one user/assistant pair"), Example->ConversationHistory.Num(), 3);
	if (Example->ConversationHistory.Num() == 3)
	{
		TestEqual(TEXT("The committed user role is preserved"), Example->ConversationHistory[1].Role, EUnrealAIMessageRole::User);
		TestEqual(TEXT("The committed assistant role is preserved"), Example->ConversationHistory[2].Role, EUnrealAIMessageRole::Assistant);
	}

	const int32 CommittedHistorySize = Example->ConversationHistory.Num();
	FUnrealAIMultiTurnExampleTestAccess::BeginPendingTurn(*Example, TEXT("Rejected user turn"));
	FUnrealAIMultiTurnExampleTestAccess::Fail(*Example, TEXT("synthetic_failure"));
	TestFalse(TEXT("Failure clears the in-flight flag"), Example->bRequestInFlight);
	TestEqual(TEXT("Failure rolls back the pending user message"), Example->ConversationHistory.Num(), CommittedHistorySize);

	FUnrealAIMultiTurnExampleTestAccess::BeginPendingTurn(*Example, TEXT("Second user turn"));
	FUnrealAIMultiTurnExampleTestAccess::Complete(*Example, TEXT("Second assistant turn"));
	Example->MaxRetainedTurns = 1;
	FUnrealAIMultiTurnExampleTestAccess::Trim(*Example);
	TestEqual(TEXT("The history bound retains one complete pair and the system prefix"), Example->ConversationHistory.Num(), 3);
	if (Example->ConversationHistory.Num() == 3)
	{
		TestEqual(TEXT("Trimming preserves the system prefix"), Example->ConversationHistory[0].Role, EUnrealAIMessageRole::System);
		TestEqual(TEXT("Trimming begins at a user boundary"), Example->ConversationHistory[1].Role, EUnrealAIMessageRole::User);
		TestEqual(TEXT("Trimming ends at an assistant boundary"), Example->ConversationHistory[2].Role, EUnrealAIMessageRole::Assistant);
		TestEqual(TEXT("Trimming removes the oldest complete pair"), Example->ConversationHistory[1].Content, FString(TEXT("Second user turn")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleStreamingBehaviorContractTest,
	"UnrealAISample.SkillContracts.Cpp.StreamingBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleStreamingBehaviorContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UnrealAISampleTests;

	FContractWorld ContractWorld;
	if (!TestTrue(TEXT("Creates a streaming contract-test world"), ContractWorld.Initialize()))
	{
		return false;
	}

	AUnrealAIStreamingExample* Example = ContractWorld.World->SpawnActor<AUnrealAIStreamingExample>();
	if (!TestNotNull(TEXT("Spawns the streaming example"), Example))
	{
		return false;
	}

	FUnrealAIChatStreamEvent FirstDelta;
	FirstDelta.Type = EUnrealAIChatStreamEventType::TextDelta;
	FirstDelta.TextDelta = TEXT("Hello ");
	FUnrealAIStreamingExampleTestAccess::Emit(*Example, FirstDelta);

	FUnrealAIChatStreamEvent ProviderEvent;
	ProviderEvent.Type = EUnrealAIChatStreamEventType::ProviderEvent;
	ProviderEvent.RawJson = TEXT("{\"private\":true}");
	FUnrealAIStreamingExampleTestAccess::Emit(*Example, ProviderEvent);

	FUnrealAIChatStreamEvent SecondDelta;
	SecondDelta.Type = EUnrealAIChatStreamEventType::TextDelta;
	SecondDelta.TextDelta = TEXT("world");
	FUnrealAIStreamingExampleTestAccess::Emit(*Example, SecondDelta);
	TestEqual(TEXT("Only text deltas are accumulated"), Example->StreamingText, FString(TEXT("Hello world")));

	FUnrealAIStreamingExampleTestAccess::Complete(*Example, TEXT("Normalized aggregate"));
	TestEqual(TEXT("The terminal aggregate replaces the incremental buffer"), Example->StreamingText, FString(TEXT("Normalized aggregate")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleProductionDeploymentContractTest,
	"UnrealAISample.SkillContracts.Deployment.Configuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISampleProductionDeploymentContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UObject* Owner = NewObject<UUnrealAIClient>();
	FUnrealAIError ProxyError;
	UUnrealAIClient* ProxyClient = FUnrealAIProductionDeploymentExample::CreatePackagedClientProxy(
		Owner,
		TEXT("synthetic-short-lived-session-token"),
		ProxyError);
	if (!TestNotNull(TEXT("The packaged-client proxy recipe creates a client"), ProxyClient))
	{
		return false;
	}
	TestFalse(TEXT("The proxy recipe reports no configuration error"), ProxyError.bIsError);

	const FUnrealAIProviderConfig& ProxyConfig = ProxyClient->GetProviderConfig();
	TestEqual(TEXT("The proxy uses the OpenAI-compatible protocol"), ProxyConfig.Api, EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
	TestEqual(TEXT("The proxy base URL is retained"), ProxyConfig.BaseUrl, FString(TEXT("https://ai.example.invalid/v1")));
	TestEqual(TEXT("The backend-owned model alias is retained"), ProxyConfig.DefaultModel, FString(TEXT("game-chat")));
	TestTrue(TEXT("The proxy requires a session token"), ProxyConfig.bRequiresApiKey);
	TestTrue(TEXT("The proxy does not read a hosted-provider environment key"), ProxyConfig.ApiKeyEnvironmentVariable.IsEmpty());
	TestEqual(TEXT("The short-lived session token remains in memory"), ProxyConfig.ApiKeyOverride, FString(TEXT("synthetic-short-lived-session-token")));
	TestEqual(TEXT("The proxy timeout is bounded"), ProxyConfig.TimeoutSeconds, 30.0f);

	FUnrealAIError ServerError;
	UUnrealAIClient* ServerClient = FUnrealAIProductionDeploymentExample::CreateDedicatedServerProvider(
		Owner,
		TEXT("OpenAI"),
		ServerError);
	TestNotNull(TEXT("The dedicated-server recipe resolves a configured provider"), ServerClient);
	TestFalse(TEXT("The dedicated-server recipe reports no configuration error"), ServerError.bIsError);
	if (ServerClient)
	{
		TestEqual(TEXT("The dedicated-server recipe resolves the selected profile"), ServerClient->GetProviderConfig().Name, FName(TEXT("OpenAI")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleCppLiveTest,
	"UnrealAISample.Live.XAI.CppCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)

bool FUnrealAISampleCppLiveTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAISampleTests;
	if (!HasLiveXAIKey(*this))
	{
		return false;
	}

	const TSharedRef<FLiveRequestState> State = MakeShared<FLiveRequestState>();
	if (!TestTrue(TEXT("Creates a live sample world"), State->Initialize(AUnrealAISampleActor::StaticClass())))
	{
		return false;
	}

	State->Actor->RunCppCompletionSample();
	AddLiveWaitCommand(*this, State, ELiveRequestKind::CppCompletion);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleCppStreamingLiveTest,
	"UnrealAISample.Live.XAI.CppStreaming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)

bool FUnrealAISampleCppStreamingLiveTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAISampleTests;
	if (!HasLiveXAIKey(*this))
	{
		return false;
	}

	const TSharedRef<FLiveRequestState> State = MakeShared<FLiveRequestState>();
	if (!TestTrue(TEXT("Creates a live streaming sample world"), State->Initialize(AUnrealAISampleActor::StaticClass())))
	{
		return false;
	}

	State->Actor->RunCppStreamingSample();
	AddLiveWaitCommand(*this, State, ELiveRequestKind::CppStreaming);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAISampleBlueprintLiveTest,
	"UnrealAISample.Live.XAI.BlueprintCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)

bool FUnrealAISampleBlueprintLiveTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAISampleTests;
	if (!HasLiveXAIKey(*this))
	{
		return false;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, BlueprintObjectPath);
	if (!TestNotNull(TEXT("Blueprint example loads"), Blueprint) ||
		!TestNotNull(TEXT("Blueprint generated class exists"), Blueprint ? Blueprint->GeneratedClass.Get() : nullptr))
	{
		return false;
	}

	const TSharedRef<FLiveRequestState> State = MakeShared<FLiveRequestState>();
	if (!TestTrue(TEXT("Creates a live Blueprint sample world"), State->Initialize(Blueprint->GeneratedClass.Get())))
	{
		return false;
	}

	State->Actor->DispatchBeginPlay();
	AddLiveWaitCommand(*this, State, ELiveRequestKind::BlueprintCompletion);
	return true;
}

#endif
