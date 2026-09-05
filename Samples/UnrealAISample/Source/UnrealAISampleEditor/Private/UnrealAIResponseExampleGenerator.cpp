#include "UnrealAIResponseExampleGenerator.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UnrealAIResponseAsyncAction.h"
#include "UnrealAIResponseExample.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UnrealAISampleGenerator
{
	// Shared checked helpers used by the existing starter graph generator.
	UK2Node_CallFunction* AddFunctionNode(UEdGraph& Graph, UFunction* Function, int32 X, int32 Y);
	UEdGraphPin* RequirePin(UEdGraphNode& Node, FName Name, EEdGraphPinDirection Direction);
	bool Connect(UEdGraphPin* Output, UEdGraphPin* Input);

	bool GenerateResponseExample(bool bStreaming)
	{
		const FName Name = bStreaming ? TEXT("BP_UnrealAIStreamResponses") : TEXT("BP_UnrealAIResponses");
		const FString PackageName = TEXT("/Game/Blueprints/") + Name.ToString();
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		UBlueprint* Blueprint = Package ? FindObject<UBlueprint>(Package, *Name.ToString()) : nullptr;
		if (!Blueprint)
		{
			Package = CreatePackage(*PackageName);
			Blueprint = FKismetEditorUtilities::CreateBlueprint(AUnrealAIResponseExample::StaticClass(), Package, Name,
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (!Blueprint)
			{
				return false;
			}
			FAssetRegistryModule::AssetCreated(Blueprint);
		}
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (!Graph)
		{
			return false;
		}
		const TArray<UEdGraphNode*> Existing = Graph->Nodes;
		for (UEdGraphNode* Node : Existing)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
		}
		auto Call = [Graph](FName Function, int32 X, int32 Y)
		{
			return AddFunctionNode(*Graph, AUnrealAIResponseExample::StaticClass()->FindFunctionByName(Function), X, Y);
		};
		auto Link = [](UEdGraphNode* From, FName Output, UEdGraphNode* To, FName Input)
		{
			return Connect(RequirePin(*From, Output, EGPD_Output), RequirePin(*To, Input, EGPD_Input));
		};
		auto Branch = [Graph](int32 X, int32 Y)
		{
			FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
			UK2Node_IfThenElse* Node = Creator.CreateNode(false);
			Node->NodePosX = X;
			Node->NodePosY = Y;
			Creator.Finalize();
			return Node;
		};
		FGraphNodeCreator<UK2Node_Event> EventCreator(*Graph);
		UK2Node_Event* Start = EventCreator.CreateNode(false);
		Start->EventReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, StartBlueprintResponses),
			AUnrealAIResponseExample::StaticClass());
		Start->bOverrideFunction = true;
		Start->NodePosX = -1200;
		EventCreator.Finalize();
		UK2Node_CallFunction* Prepare = Call(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, PrepareResponseRequest), -950, 0);
		UK2Node_IfThenElse* Ready = Branch(-650, 0);
		UK2Node_CallFunction* Continue = Call(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, PrepareToolContinuation), 100, 0);
		Continue->NodeComment = TEXT("Only after Completed: validate the allowlisted tool, execute once, and build provider-bound continuation. False ends locally.");
		Continue->bCommentBubblePinned = true;
		UK2Node_IfThenElse* CanContinue = Branch(450, 0);
		bool bConnected = Link(Start, UEdGraphSchema_K2::PN_Then, Prepare, UEdGraphSchema_K2::PN_Execute);
		bConnected &= Link(Prepare, UEdGraphSchema_K2::PN_Then, Ready, UEdGraphSchema_K2::PN_Execute);
		bConnected &= Link(Prepare, UEdGraphSchema_K2::PN_ReturnValue, Ready, UEdGraphSchema_K2::PN_Condition);
		bConnected &= Link(Continue, UEdGraphSchema_K2::PN_Then, CanContinue, UEdGraphSchema_K2::PN_Execute);
		bConnected &= Link(Continue, UEdGraphSchema_K2::PN_ReturnValue, CanContinue, UEdGraphSchema_K2::PN_Condition);
		for (int32 Step = 0; Step < 2; ++Step)
		{
			const int32 X = Step == 0 ? -400 : 700;
			FGraphNodeCreator<UK2Node_AsyncAction> Creator(*Graph);
			UK2Node_AsyncAction* Async = Creator.CreateNode(false);
			Async->InitializeProxyFromFunction(UUnrealAIResponseAsyncAction::StaticClass()->FindFunctionByName(bStreaming
				? GET_FUNCTION_NAME_CHECKED(UUnrealAIResponseAsyncAction, StreamResponse)
				: GET_FUNCTION_NAME_CHECKED(UUnrealAIResponseAsyncAction, CreateResponse)));
			Async->NodePosX = X;
			Creator.Finalize();
			UK2Node_CallFunction* Terminal = Call(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, RecordResponseTerminal), X + 400, 400);
			UK2Node_CallFunction* RecordEvent = Call(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, RecordResponseEvent), X + 400, 650);
			UK2Node_CallFunction* RecordRetry = Call(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, RecordResponseRetry), X + 400, 850);
			UK2Node_CallFunction* Retain = Call(GET_FUNCTION_NAME_CHECKED(AUnrealAIResponseExample, RetainResponseAction), X + 400, 1100);
			bConnected &= Link(Step == 0 ? Ready : CanContinue, UEdGraphSchema_K2::PN_Then, Async, UEdGraphSchema_K2::PN_Execute);
			bConnected &= Link(Step == 0 ? Prepare : Continue, TEXT("OutRequest"), Async, TEXT("Request"));
			bConnected &= Link(Prepare, TEXT("OutProvider"), Async, TEXT("ProviderName"));
			bConnected &= Link(Async, TEXT("Completed"), Step == 0 ? Continue : Terminal, UEdGraphSchema_K2::PN_Execute);
			bConnected &= Link(Async, TEXT("Result"), Step == 0 ? Continue : Terminal, TEXT("Result"));
			if (Step == 0)
			{
				bConnected &= Link(Async, TEXT("Result"), Terminal, TEXT("Result"));
			}
			for (const FName Pin : {FName(TEXT("Incomplete")), FName(TEXT("Failed")), FName(TEXT("Cancelled"))})
			{
				bConnected &= Link(Async, Pin, Terminal, UEdGraphSchema_K2::PN_Execute);
			}
			bConnected &= Link(Async, TEXT("Event"), RecordEvent, UEdGraphSchema_K2::PN_Execute);
			bConnected &= Link(Async, TEXT("ResponseEvent"), RecordEvent, TEXT("ResponseEvent"));
			bConnected &= Link(Async, TEXT("Retrying"), RecordRetry, UEdGraphSchema_K2::PN_Execute);
			bConnected &= Link(Async, TEXT("RetryEvent"), RecordRetry, TEXT("RetryEvent"));
			bConnected &= Link(Async, UEdGraphSchema_K2::PN_Then, Retain, UEdGraphSchema_K2::PN_Execute);
			bConnected &= Link(Async, TEXT("AsyncTaskProxy"), Retain, TEXT("Action"));
		}
		if (!bConnected)
		{
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
		{
			return false;
		}
		Package->FullyLoad();
		Package->SetDirtyFlag(true);
		FSavePackageArgs Save;
		Save.TopLevelFlags = RF_Public | RF_Standalone;
		Save.Error = GError;
		Save.SaveFlags = SAVE_NoError;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Package, Blueprint, *Filename, Save);
	}

	bool GenerateResponseExamples()
	{
		return GenerateResponseExample(false) && GenerateResponseExample(true);
	}
}
