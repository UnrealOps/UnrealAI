#include "UnrealAISampleGenerateCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Factories/WorldFactory.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIChatCompletionAsyncAction.h"
#include "UnrealAIPackagedChatWidgetExample.h"
#include "UnrealAISampleActor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAISampleGenerator, Log, All);

namespace UnrealAISampleGenerator
{
	const TCHAR* BlueprintPackageName = TEXT("/Game/Blueprints/BP_UnrealAIGettingStarted");
	const FName BlueprintAssetName = TEXT("BP_UnrealAIGettingStarted");
	const TCHAR* BackendWidgetPackageName = TEXT("/Game/Blueprints/WBP_UnrealAIBackendChat");
	const FName BackendWidgetAssetName = TEXT("WBP_UnrealAIBackendChat");
	const TCHAR* MapPackageName = TEXT("/Game/Maps/UnrealAISampleMap");
	const FName MapAssetName = TEXT("UnrealAISampleMap");

	UK2Node_CallFunction* AddFunctionNode(UEdGraph& Graph, UFunction* Function, int32 X, int32 Y)
	{
		check(Function);
		FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(Graph);
		UK2Node_CallFunction* Node = NodeCreator.CreateNode(false);
		Node->SetFromFunction(Function);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		NodeCreator.Finalize();
		return Node;
	}

	UEdGraphPin* RequirePin(UEdGraphNode& Node, FName PinName, EEdGraphPinDirection Direction)
	{
		UEdGraphPin* Pin = Node.FindPin(PinName, Direction);
		if (!Pin)
		{
			FString AvailablePins;
			for (const UEdGraphPin* AvailablePin : Node.Pins)
			{
				if (!AvailablePins.IsEmpty())
				{
					AvailablePins += TEXT(", ");
				}
				AvailablePins += FString::Printf(
					TEXT("%s (%s)"),
					*AvailablePin->PinName.ToString(),
					AvailablePin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			}
			UE_LOG(
				LogUnrealAISampleGenerator,
				Error,
				TEXT("Node '%s' is missing %s pin '%s'. Available pins: %s"),
				*Node.GetNodeTitle(ENodeTitleType::ListView).ToString(),
				Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
				*PinName.ToString(),
				*AvailablePins);
		}
		return Pin;
	}

	bool Connect(UEdGraphPin* OutputPin, UEdGraphPin* InputPin)
	{
		if (!OutputPin || !InputPin)
		{
			return false;
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (!Schema->TryCreateConnection(OutputPin, InputPin))
		{
			UE_LOG(
				LogUnrealAISampleGenerator,
				Error,
				TEXT("Could not connect '%s' to '%s'."),
				*OutputPin->PinName.ToString(),
				*InputPin->PinName.ToString());
			return false;
		}

		return true;
	}

	bool SetDefaultValue(UEdGraphNode& Node, FName PinName, const FString& Value)
	{
		UEdGraphPin* Pin = RequirePin(Node, PinName, EGPD_Input);
		if (!Pin)
		{
			return false;
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		const FString ProposedValueError = Schema->IsPinDefaultValid(
			Pin,
			Value,
			nullptr,
			FText::GetEmpty());
		if (!ProposedValueError.IsEmpty())
		{
			UE_LOG(
				LogUnrealAISampleGenerator,
				Error,
				TEXT("Default '%s' is invalid for pin '%s' on node '%s': %s"),
				*Value,
				*PinName.ToString(),
				*Node.GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*ProposedValueError);
			return false;
		}

		Schema->TrySetDefaultValue(*Pin, Value);
		const FString AppliedValueError = Schema->IsCurrentPinDefaultValid(Pin);
		if (!AppliedValueError.IsEmpty() || Pin->DefaultValue != Value)
		{
			UE_LOG(
				LogUnrealAISampleGenerator,
				Error,
				TEXT("Pin '%s' on node '%s' did not retain default '%s': %s"),
				*PinName.ToString(),
				*Node.GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*Value,
				*AppliedValueError);
			return false;
		}
		return true;
	}

	bool BuildGraph(UBlueprint& Blueprint)
	{
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(&Blueprint);
		if (!EventGraph)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("The Blueprint has no event graph."));
			return false;
		}

		TArray<UEdGraphNode*> ExistingNodes = EventGraph->Nodes;
		for (UEdGraphNode* ExistingNode : ExistingNodes)
		{
			FBlueprintEditorUtils::RemoveNode(&Blueprint, ExistingNode, true);
		}

		FGraphNodeCreator<UK2Node_Event> EventCreator(*EventGraph);
		UK2Node_Event* BeginPlayEvent = EventCreator.CreateNode(false);
		BeginPlayEvent->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
		BeginPlayEvent->bOverrideFunction = true;
		BeginPlayEvent->NodePosX = -1000;
		BeginPlayEvent->NodePosY = 0;
		BeginPlayEvent->NodeComment = TEXT("The sample runs in PIE. UnrealAI loads the API key from the project environment; no credential is stored in this asset.");
		BeginPlayEvent->bCommentBubblePinned = true;
		EventCreator.Finalize();

		UK2Node_CallFunction* MakeRequest = AddFunctionNode(
			*EventGraph,
			UUnrealAIBlueprintLibrary::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(UUnrealAIBlueprintLibrary, MakeSimpleChatRequest)),
			-760,
			160);
		bool bConfigured = SetDefaultValue(
			*MakeRequest,
			TEXT("Prompt"),
			TEXT("Reply with exactly: UnrealAI sample works."));

		UFunction* CompletionFactory = UUnrealAIChatCompletionAsyncAction::StaticClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, CreateChatCompletion));
		FGraphNodeCreator<UK2Node_AsyncAction> AsyncCreator(*EventGraph);
		UK2Node_AsyncAction* CompletionNode = AsyncCreator.CreateNode(false);
		CompletionNode->InitializeProxyFromFunction(CompletionFactory);
		CompletionNode->NodePosX = -420;
		CompletionNode->NodePosY = 0;
		CompletionNode->NodeComment = TEXT("The async node exposes success, retry, failure, and cancellation as separate execution paths.");
		CompletionNode->bCommentBubblePinned = true;
		AsyncCreator.Finalize();
		bConfigured &= SetDefaultValue(*CompletionNode, TEXT("ProviderName"), TEXT("XAI"));

		UK2Node_CallFunction* GetContent = AddFunctionNode(
			*EventGraph,
			UUnrealAIBlueprintLibrary::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(UUnrealAIBlueprintLibrary, GetFirstChoiceContent)),
			0,
			80);
		UK2Node_CallFunction* RecordSuccess = AddFunctionNode(
			*EventGraph,
			AUnrealAISampleActor::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(AUnrealAISampleActor, RecordBlueprintSuccess)),
			360,
			0);
		UK2Node_CallFunction* RecordRetry = AddFunctionNode(
			*EventGraph,
			AUnrealAISampleActor::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(AUnrealAISampleActor, RecordBlueprintRetry)),
			0,
			260);
		UK2Node_CallFunction* RecordFailure = AddFunctionNode(
			*EventGraph,
			AUnrealAISampleActor::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(AUnrealAISampleActor, RecordBlueprintFailure)),
			0,
			440);
		UK2Node_CallFunction* RecordCancellation = AddFunctionNode(
			*EventGraph,
			AUnrealAISampleActor::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(AUnrealAISampleActor, RecordBlueprintCancellation)),
			0,
			620);

		bool bConnected = true;
		bConnected &= Connect(
			RequirePin(*BeginPlayEvent, UEdGraphSchema_K2::PN_Then, EGPD_Output),
			RequirePin(*CompletionNode, UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		bConnected &= Connect(
			RequirePin(*MakeRequest, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output),
			RequirePin(*CompletionNode, TEXT("Request"), EGPD_Input));
		bConnected &= Connect(
			RequirePin(*CompletionNode, TEXT("Completed"), EGPD_Output),
			RequirePin(*RecordSuccess, UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		bConnected &= Connect(
			RequirePin(*CompletionNode, TEXT("Response"), EGPD_Output),
			RequirePin(*GetContent, TEXT("Response"), EGPD_Input));
		bConnected &= Connect(
			RequirePin(*GetContent, UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output),
			RequirePin(*RecordSuccess, TEXT("Content"), EGPD_Input));
		bConnected &= Connect(
			RequirePin(*GetContent, TEXT("bHasContent"), EGPD_Output),
			RequirePin(*RecordSuccess, TEXT("bHasContent"), EGPD_Input));
		bConnected &= Connect(
			RequirePin(*CompletionNode, TEXT("Retrying"), EGPD_Output),
			RequirePin(*RecordRetry, UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		bConnected &= Connect(
			RequirePin(*CompletionNode, TEXT("Failed"), EGPD_Output),
			RequirePin(*RecordFailure, UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		bConnected &= Connect(
			RequirePin(*CompletionNode, TEXT("Error"), EGPD_Output),
			RequirePin(*RecordFailure, TEXT("Error"), EGPD_Input));
		bConnected &= Connect(
			RequirePin(*CompletionNode, TEXT("Cancelled"), EGPD_Output),
			RequirePin(*RecordCancellation, UEdGraphSchema_K2::PN_Execute, EGPD_Input));

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		return bConfigured && bConnected;
	}

	bool BuildBackendWidgetGraph(UWidgetBlueprint& Blueprint)
	{
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(&Blueprint);
		if (!EventGraph)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("The backend Widget Blueprint has no event graph."));
			return false;
		}

		TArray<UEdGraphNode*> ExistingNodes = EventGraph->Nodes;
		for (UEdGraphNode* ExistingNode : ExistingNodes)
		{
			FBlueprintEditorUtils::RemoveNode(&Blueprint, ExistingNode, true);
		}

		int32 Y = 0;
		for (const FName EventName : {
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveBackendReadinessChanged),
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveTextDelta),
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveRetrying),
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveCompleted),
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveFailed),
			GET_FUNCTION_NAME_CHECKED(UUnrealAIPackagedChatWidgetExample, ReceiveCancelled)})
		{
			UFunction* EventFunction = UUnrealAIPackagedChatWidgetExample::StaticClass()->FindFunctionByName(EventName);
			if (!EventFunction)
			{
				UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not resolve backend widget event '%s'."), *EventName.ToString());
				return false;
			}

			FGraphNodeCreator<UK2Node_Event> EventCreator(*EventGraph);
			UK2Node_Event* EventNode = EventCreator.CreateNode(false);
			EventNode->EventReference.SetExternalMember(EventName, UUnrealAIPackagedChatWidgetExample::StaticClass());
			EventNode->bOverrideFunction = true;
			EventNode->NodePosX = 0;
			EventNode->NodePosY = Y;
			EventNode->NodeComment = TEXT("Presentation-only route. Add bounded UI updates here; transport and credentials remain in native code.");
			EventNode->bCommentBubblePinned = true;
			EventCreator.Finalize();
			Y += 220;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		return true;
	}

	bool GenerateBackendWidget()
	{
		UPackage* WidgetPackage = LoadPackage(nullptr, BackendWidgetPackageName, LOAD_None);
		UWidgetBlueprint* WidgetBlueprint = WidgetPackage
			? FindObject<UWidgetBlueprint>(WidgetPackage, *BackendWidgetAssetName.ToString())
			: nullptr;
		if (!WidgetBlueprint)
		{
			WidgetPackage = CreatePackage(BackendWidgetPackageName);
			WidgetBlueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
				UUnrealAIPackagedChatWidgetExample::StaticClass(),
				WidgetPackage,
				BackendWidgetAssetName,
				BPTYPE_Normal,
				UWidgetBlueprint::StaticClass(),
				UWidgetBlueprintGeneratedClass::StaticClass()));
			if (!WidgetBlueprint)
			{
				UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not create %s."), BackendWidgetPackageName);
				return false;
			}
			FAssetRegistryModule::AssetCreated(WidgetBlueprint);
		}

		if (!BuildBackendWidgetGraph(*WidgetBlueprint))
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		if (WidgetBlueprint->Status == BS_Error || !WidgetBlueprint->GeneratedClass)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Widget Blueprint compilation failed for %s."), BackendWidgetPackageName);
			return false;
		}

		WidgetPackage->FullyLoad();
		WidgetPackage->SetDirtyFlag(true);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		SaveArgs.SaveFlags = SAVE_NoError;
		const FString WidgetFilename = FPackageName::LongPackageNameToFilename(
			BackendWidgetPackageName,
			FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(WidgetPackage, WidgetBlueprint, *WidgetFilename, SaveArgs))
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not save %s."), *WidgetFilename);
			return false;
		}
		return true;
	}

	bool BuildStarterMap(UBlueprint& Blueprint)
	{
		UPackage* MapPackage = LoadPackage(nullptr, MapPackageName, LOAD_None);
		UWorld* World = MapPackage ? FindObject<UWorld>(MapPackage, *MapAssetName.ToString()) : nullptr;
		if (!World)
		{
			MapPackage = CreatePackage(MapPackageName);
			UWorldFactory* WorldFactory = NewObject<UWorldFactory>();
			WorldFactory->WorldType = EWorldType::Editor;
			WorldFactory->bCreateWorldPartition = false;
			WorldFactory->bInformEngineOfWorld = false;
			World = Cast<UWorld>(WorldFactory->FactoryCreateNew(
				UWorld::StaticClass(),
				MapPackage,
				MapAssetName,
				RF_Public | RF_Standalone,
				nullptr,
				GWarn));
			if (!World)
			{
				UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not create %s."), MapPackageName);
				return false;
			}
			FAssetRegistryModule::AssetCreated(World);
		}

		if (!Blueprint.GeneratedClass)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("The Blueprint has no generated class for the starter map."));
			return false;
		}
		if (!World->PersistentLevel)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("The starter map has no persistent level."));
			return false;
		}

		AUnrealAISampleActor* SampleActor = nullptr;
		TArray<AActor*> DuplicateActors;
		for (AActor* Actor : World->PersistentLevel->Actors)
		{
			if (Actor && Actor->GetClass() == Blueprint.GeneratedClass.Get())
			{
				if (!SampleActor)
				{
					SampleActor = CastChecked<AUnrealAISampleActor>(Actor);
				}
				else
				{
					DuplicateActors.Add(Actor);
				}
			}
		}
		for (AActor* Actor : DuplicateActors)
		{
			World->DestroyActor(Actor, true);
		}

		if (!SampleActor)
		{
			SampleActor = World->SpawnActor<AUnrealAISampleActor>(
				Blueprint.GeneratedClass.Get(),
				FTransform::Identity);
		}
		if (!SampleActor)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not place the Blueprint example in %s."), MapPackageName);
			return false;
		}
		SampleActor->SetActorLabel(TEXT("UnrealAI Blueprint Getting Started"));

		UE_LOG(LogUnrealAISampleGenerator, Display, TEXT("Updating starter map components."));
		World->UpdateWorldComponents(true, true);
		MapPackage->SetDirtyFlag(true);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		SaveArgs.SaveFlags = SAVE_NoError;
		const FString MapFilename = FPackageName::LongPackageNameToFilename(
			MapPackageName,
			FPackageName::GetMapPackageExtension());
		UE_LOG(LogUnrealAISampleGenerator, Display, TEXT("Saving starter map to %s."), *MapFilename);
		if (!UPackage::SavePackage(MapPackage, World, *MapFilename, SaveArgs))
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not save %s."), *MapFilename);
			return false;
		}
		UE_LOG(LogUnrealAISampleGenerator, Display, TEXT("Saved starter map."));

		return true;
	}
}

UUnrealAISampleGenerateCommandlet::UUnrealAISampleGenerateCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UUnrealAISampleGenerateCommandlet::Main(const FString& Params)
{
	using namespace UnrealAISampleGenerator;

	UPackage* Package = LoadPackage(nullptr, BlueprintPackageName, LOAD_None);
	UBlueprint* Blueprint = Package ? FindObject<UBlueprint>(Package, *BlueprintAssetName.ToString()) : nullptr;
	if (!Blueprint)
	{
		Package = CreatePackage(BlueprintPackageName);
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AUnrealAISampleActor::StaticClass(),
			Package,
			BlueprintAssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint)
		{
			UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not create %s."), BlueprintPackageName);
			return 1;
		}
		FAssetRegistryModule::AssetCreated(Blueprint);
	}

	if (!BuildGraph(*Blueprint))
	{
		return 1;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error)
	{
		UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Blueprint compilation failed for %s."), BlueprintPackageName);
		return 1;
	}

	Package->SetDirtyFlag(true);
	Package->FullyLoad();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GError;
	SaveArgs.SaveFlags = SAVE_NoError;
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		BlueprintPackageName,
		FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs))
	{
		UE_LOG(LogUnrealAISampleGenerator, Error, TEXT("Could not save %s."), *PackageFilename);
		return 1;
	}
	if (!BuildStarterMap(*Blueprint))
	{
		return 1;
	}
	if (!GenerateBackendWidget())
	{
		return 1;
	}

	UE_LOG(
		LogUnrealAISampleGenerator,
		Display,
		TEXT("Generated and compiled %s, %s, and %s."),
		BlueprintPackageName,
		MapPackageName,
		BackendWidgetPackageName);
	return 0;
}
