#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "K2Node_AsyncAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealAIResponseExample.h"
#include "UnrealAISettings.h"

namespace UnrealAIResponseExampleTests
{
	const TCHAR* BlueprintPaths[] = {
		TEXT("/Game/Blueprints/BP_UnrealAIResponses.BP_UnrealAIResponses"),
		TEXT("/Game/Blueprints/BP_UnrealAIStreamResponses.BP_UnrealAIStreamResponses")
	};

	class FRunLoopbackMatrix : public IAutomationLatentCommand
	{
	public:
		FRunLoopbackMatrix(FAutomationTestBase& InTest, int32 InPort)
			: Test(InTest), Port(InPort)
		{
			SavedProfiles = GetDefault<UUnrealAISettings>()->ProviderProfiles;
		}

		virtual ~FRunLoopbackMatrix() override
		{
			if (Actor)
			{
				Actor->CancelResponses();
			}
			GetMutableDefault<UUnrealAISettings>()->ProviderProfiles = SavedProfiles;
			if (World)
			{
				World->DestroyWorld(true);
				World->SetPhysicsScene(nullptr);
				GEngine->DestroyWorldContext(World);
				World->RemoveFromRoot();
			}
			if (GameInstance)
			{
				GameInstance->RemoveFromRoot();
			}
		}

		virtual bool Update() override
		{
			if (!World && !Initialize())
			{
				return true;
			}
			if (Actor)
			{
				if (!Actor->bDone && FPlatformTime::Seconds() - Started < 20.0)
				{
					return false;
				}
				const FString Label = FString::Printf(TEXT("Loopback case %d"), Case);
				if (Actor->LastResult.Error.bIsError)
				{
					// Fixture-only diagnostics: never print provider bodies or headers.
					Test.AddInfo(Label + TEXT(" error code: ") + Actor->LastResult.Error.Code);
				}
				Test.TestTrue(Label + TEXT(" terminates"), Actor->bDone);
				if (Case == 18)
				{
					Test.TestEqual(Label + TEXT(" truncated stream fails"), Actor->LastResult.Status, EUnrealAIResponseStatus::Failed);
					Test.TestEqual(Label + TEXT(" lifecycle prevents replay"), Actor->RetryCount, 0);
					Test.TestEqual(Label + TEXT(" exactly one terminal"), Actor->TerminalCount, 1);
					Test.TestTrue(Label + TEXT(" saw lifecycle event"), Actor->EventCount > 0);
				}
				else if (Case == 19 || Case == 20)
				{
					Test.TestEqual(Label + TEXT(" cancelled"), Actor->LastResult.Status, EUnrealAIResponseStatus::Cancelled);
					Test.TestFalse(Label + TEXT(" cancellation not failure"), Actor->LastResult.Error.bIsError);
					Test.TestEqual(Label + TEXT(" one terminal"), Actor->TerminalCount, 1);
					Test.TestEqual(Label + TEXT(" no tool executed"), Actor->ExecutedToolCount, 0);
				}
				else if (Case >= 21)
				{
					Test.TestEqual(Label + TEXT(" explicit terminal status"), Actor->LastResult.Status,
						Case <= 22 ? EUnrealAIResponseStatus::Incomplete : EUnrealAIResponseStatus::Failed);
					Test.TestEqual(Label + TEXT(" incomplete is not error"), Actor->LastResult.Error.bIsError, Case >= 23);
					Test.TestEqual(Label + TEXT(" one terminal"), Actor->TerminalCount, 1);
					Test.TestEqual(Label + TEXT(" no tool executed"), Actor->ExecutedToolCount, 0);
					Test.TestTrue(Label + TEXT(" partial output retained"), !Actor->LastResult.Response.Output.IsEmpty());
				}
				else
				{
					Test.TestEqual(Label + TEXT(" completed"), Actor->LastResult.Status, EUnrealAIResponseStatus::Completed);
					Test.TestEqual(Label + TEXT(" tool executed once"), Actor->ExecutedToolCount, 1);
					Test.TestEqual(Label + TEXT(" two terminal callbacks"), Actor->TerminalCount, 2);
					Test.TestEqual(Label + TEXT(" final Unicode text"), Actor->DisplayText, FString(TEXT("Fixture complete \u2713")));
					Test.TestEqual(Label + TEXT(" retry count"), Actor->RetryCount, Case >= 16 ? 1 : 0);
					if ((Case >= 4 && Case < 8) || (Case >= 12 && Case < 16) || Case == 17)
					{
						Test.TestTrue(Label + TEXT(" streams events"), Actor->EventCount > 0);
					}
				}
				Test.TestTrue(Label + TEXT(" callbacks on game thread"), Actor->bCallbacksOnGameThread);
				Actor->CancelResponses(); // Idempotent after every terminal path.
				Actor->Destroy();
				Actor = nullptr;
				if (++Case == 25)
				{
					return true;
				}
			}
			return StartCase();
		}

	private:
		FAutomationTestBase& Test;
		int32 Port;
		int32 Case = 0;
		double Started = 0.0;
		UWorld* World = nullptr;
		UGameInstance* GameInstance = nullptr;
		AUnrealAIResponseExample* Actor = nullptr;
		TArray<FUnrealAIProviderConfig> SavedProfiles;

		bool Initialize()
		{
			if (!Test.TestNotNull(TEXT("Engine available"), GEngine))
			{
				return false;
			}
			World = UWorld::CreateWorld(EWorldType::Game, false,
				MakeUniqueObjectName(GetTransientPackage(), UWorld::StaticClass(), TEXT("ResponseFixtureWorld")));
			if (!Test.TestNotNull(TEXT("Fixture world"), World))
			{
				return false;
			}
			World->AddToRoot();
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->AddToRoot();
			World->SetGameInstance(GameInstance);
			Context.OwningGameInstance = GameInstance;
			World->InitializeActorsForPlay(FURL());
			return true;
		}

		bool StartCase()
		{
			const int32 Protocol = Case < 16 ? Case % 4 : 0;
			const TCHAR* Names[] = {TEXT("native"), TEXT("chat"), TEXT("anthropic"), TEXT("gemini")};
			const FString Scenario = Case >= 23 ? TEXT("failure") : (Case >= 21 ? TEXT("incomplete") :
				(Case == 16 ? TEXT("retry") : (Case == 17 ? TEXT("retry-stream") :
				(Case == 18 ? TEXT("cutoff") : FString::Printf(TEXT("case%d"), Case)))));
			FUnrealAIProviderConfig Config;
			Config.Name = TEXT("ResponseContractFixture");
			Config.BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d/%s/%s"), Port, *Scenario, Names[Protocol]);
			Config.DefaultModel = TEXT("fixture-model");
			Config.BaseUrlEnvironmentVariable.Reset();
			Config.ModelEnvironmentVariable.Reset();
			Config.ApiKeyEnvironmentVariable.Reset();
			Config.ApiKeyOverride.Reset();
			Config.bRequiresApiKey = false;
			Config.TimeoutSeconds = 5.0f;
			Config.Api = Protocol == 2 ? EUnrealAIProviderApi::AnthropicMessages :
				(Protocol == 3 ? EUnrealAIProviderApi::GeminiGenerateContent : EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
			Config.ResponseApi = Protocol == 0 ? EUnrealAIResponseApi::OpenAIResponses : EUnrealAIResponseApi::ChatProtocol;
			Config.RetryPolicy.MaxRetries = 1;
			GetMutableDefault<UUnrealAISettings>()->ProviderProfiles = SavedProfiles;
			GetMutableDefault<UUnrealAISettings>()->ProviderProfiles.Add(Config);
			const bool bBlueprint = (Case >= 8 && Case < 16) || Case == 20 || Case == 22 || Case == 24;
			const bool bStreaming = (Case >= 4 && Case < 8) || (Case >= 12 && Case < 16)
				|| (Case >= 17 && Case <= 20) || Case == 22 || Case == 24;
			UClass* Class = AUnrealAIResponseExample::StaticClass();
			if (bBlueprint)
			{
				UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, BlueprintPaths[bStreaming ? 1 : 0]);
				if (!Test.TestNotNull(TEXT("Generated response Blueprint"), Blueprint)
					|| !Test.TestNotNull(TEXT("Compiled response class"), Blueprint->GeneratedClass.Get()))
				{
					return true;
				}
				Class = Blueprint->GeneratedClass;
			}
			Actor = World->SpawnActor<AUnrealAIResponseExample>(Class);
			if (!Test.TestNotNull(TEXT("Response example actor"), Actor))
			{
				return true;
			}
			Actor->ProviderName = Config.Name;
			Started = FPlatformTime::Seconds();
			if (bBlueprint)
			{
				Actor->StartBlueprintResponses();
			}
			else
			{
				Actor->StartNativeResponses(bStreaming);
			}
			if (Case == 19 || Case == 20)
			{
				Actor->CancelResponses();
				Actor->CancelResponses();
			}
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIResponseExampleAssetsTest, "UnrealAISample.SkillContracts.Responses.BlueprintAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseExampleAssetsTest::RunTest(const FString& Parameters)
{
	for (const TCHAR* Path : UnrealAIResponseExampleTests::BlueprintPaths)
	{
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, Path);
		if (!TestNotNull(TEXT("Response Blueprint saved"), Blueprint))
		{
			continue;
		}
		TestTrue(TEXT("Compiled Blueprint"), Blueprint->GeneratedClass && Blueprint->Status != BS_Error);
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		int32 AsyncCount = 0;
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_AsyncAction* Action = Cast<UK2Node_AsyncAction>(Node))
				{
					++AsyncCount;
					for (const FName Pin : {FName(TEXT("Completed")), FName(TEXT("Incomplete")), FName(TEXT("Failed")), FName(TEXT("Cancelled"))})
					{
						const UEdGraphPin* Output = Action->FindPin(Pin, EGPD_Output);
						TestTrue(TEXT("Every terminal output wired"), Output && !Output->LinkedTo.IsEmpty());
					}
				}
			}
		}
		TestEqual(TEXT("Bounded two-request Blueprint workflow"), AsyncCount, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIResponseExampleLoopbackTest, "UnrealAISample.SkillContracts.Responses.Loopback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseExampleLoopbackTest::RunTest(const FString& Parameters)
{
	int32 Port = 0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("UnrealAIResponseFixturePort="), Port) || Port < 1 || Port > 65535)
	{
		AddError(TEXT("Run Scripts/ci/run_skill_contracts.py to start the credential-free loopback fixture."));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(UnrealAIResponseExampleTests::FRunLoopbackMatrix(*this, Port));
	return true;
}

#endif
