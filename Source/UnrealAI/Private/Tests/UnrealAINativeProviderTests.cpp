// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Containers/Ticker.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "OpenAI/UnrealAIOpenAIResponsesProvider.h"
#include "Runtime/UnrealAIPhysicalRequestBudget.h"
#include "Runtime/UnrealAIProviderReliability.h"
#include "Testing/UnrealAITestClock.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FNativeFixtureAccess final : public IUnrealAIProviderAccessContext
{
  public:
	bool IsValid() const override
	{
		return true;
	}
	bool RequiresCredential() const override
	{
		return true;
	}
	bool TryDispatch(IUnrealAICredentialApplicator &, FString &Error) const override
	{
		Error = TEXT("Offline fixture never dispatches credentials.");
		return false;
	}
	FString GetRedactedDisplay() const override
	{
		return TEXT("<offline>");
	}
};

class FNativeFixtureHandle final : public IUnrealAIHttpRequestHandle
{
  public:
	explicit FNativeFixtureHandle(FUnrealAIRequestId InId) : Id(InId) {}
	FUnrealAIRequestId GetRequestId() const override
	{
		return Id;
	}
	void Cancel() override
	{
		++CancellationCount;
	}
	FUnrealAIRequestId Id;
	int32 CancellationCount = 0;
};

class FNativeFixtureTransport final : public IUnrealAIHttpTransport
{
  public:
	struct FAttempt
	{
		TSharedPtr<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> Sink;
		TSharedPtr<FNativeFixtureHandle, ESPMode::ThreadSafe> Handle;
		uint64 Sequence = 0;
	};
	bool StartRequest(const FUnrealAIHttpRequest &Request,
					  const TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &,
					  const TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> &Sink,
					  const FUnrealAICancellationToken &,
					  TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle, FString &Error) override
	{
		Error.Reset();
		if (!bAccept)
		{
			OutHandle.Reset();
			return false;
		}
		FAttempt Attempt;
		Attempt.Sink = Sink;
		Attempt.Handle = MakeShared<FNativeFixtureHandle, ESPMode::ThreadSafe>(Request.RequestId);
		OutHandle = Attempt.Handle;
		Attempts.Add(MoveTemp(Attempt));
		return true;
	}
	void Settle(int32 Index)
	{
		FAttempt &Attempt = Attempts[Index];
		if (!Attempt.Sink.IsValid())
		{
			return;
		}
		FUnrealAIHttpEvent Event;
		Event.RequestId = Attempt.Handle->Id;
		Event.Sequence = ++Attempt.Sequence;
		Event.Kind = EUnrealAIHttpEventKind::Cancelled;
		Event.Error.Category = EUnrealAIErrorCategory::Cancelled;
		Event.Error.Code = TEXT("offline_cancelled");
		Event.Error.UserMessage = FText::FromString(TEXT("Cancelled."));
		const auto Sink = MoveTemp(Attempt.Sink);
		Sink->EnqueueHttpEvent(MoveTemp(Event));
	}
	void BeginShutdown() override
	{
		for (int32 Index = 0; Index < Attempts.Num(); ++Index)
		{
			Settle(Index);
		}
	}
	bool bAccept = true;
	TArray<FAttempt> Attempts;
};

class FNativeFixtureSink final : public IUnrealAIModelEventSink
{
  public:
	void EnqueueModelEvent(FUnrealAIModelEvent &&Event) override
	{
		Events.Add(MoveTemp(Event));
	}
	void OnPhysicalSettled() override
	{
		++Settlements;
	}
	TArray<FUnrealAIModelEvent> Events;
	int32 Settlements = 0;
};

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> NativeFixtureConnections()
{
	FString Error;
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = TEXT("openai.responses");
	Destination.AccountAuthProviderName = TEXT("platform.keychain");
	Destination.AuthProfileId = TEXT("openai.platform.api_key");
	Destination.AccountId.Value = FGuid(1, 2, 3, 4);
	Destination.TenantRealm = TEXT("tests.sdk");
	Destination.BillingPrincipalId.Value = FGuid(5, 6, 7, 8);
	Destination.PayerHandle = TEXT("openai.platform.api");
	Destination.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Destination.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Destination.Audience = TEXT("https://api.openai.com/v1/responses");
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	verify(FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.openai.com"), false, Destination.EndpointOrigin, Error));
	FUnrealAIEndpointProfileRegistry Endpoints;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Endpoint;
	verify(Endpoints.RegisterCustomApiEndpoint(TEXT("tests.sdk.endpoint"), Destination.ModelProviderName,
													Destination.EndpointOrigin, Destination.Audience, 1, Endpoint,
													Error));
	FUnrealAIConnectionRegistry Connections(Endpoints.CreateSnapshot());
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = TEXT("tests.sdk.connection");
	Connection.EndpointProfileId = TEXT("tests.sdk.endpoint");
	Connection.CredentialDestination = Destination;
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Registered;
	verify(Connections.Register(Connection, Registered, Error));
	return Connections.CreateSnapshot();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeCancellationTest, "UnrealAI.Native.CancellationAndPhysicalCapacity",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeCancellationTest::RunTest(const FString &)
{
	const auto Transport = MakeShared<FNativeFixtureTransport, ESPMode::ThreadSafe>();
	auto Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
	Config.bDrainEventsSynchronouslyForTesting = true;
	Config.MaximumActiveRequestsForTesting = 2;
	FUnrealAIOpenAIResponsesProvider Provider(NativeFixtureConnections(), Transport, Config);
	FUnrealAIModelRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.ConnectionAlias = TEXT("tests.sdk.connection");
	Request.ModelId = Config.ModelProfiles[0].ModelId;
	FUnrealAIModelMessage Message;
	FUnrealAIModelContentPart Part;
	Part.Text = TEXT("Offline lifecycle fixture.");
	Message.Content.Add(MoveTemp(Part));
	Request.InputMessages.Add(MoveTemp(Message));
	const auto Access = MakeShared<FNativeFixtureAccess, ESPMode::ThreadSafe>();
	const auto Sink = MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> First;
	FUnrealAIModelError Error;
	if (!TestTrue(TEXT("Standalone SDK admits a native turn"),
					   Provider.StartRequest(Request, Access, Sink, Cancellation.GetToken(), First, Error)))
	{
		return false;
	}
	First->Cancel();
	First->Cancel();
	TestTrue(TEXT("Cancellation closes logical work immediately"), First->IsLogicallyComplete());
	TestFalse(TEXT("Cancellation does not claim physical settlement"), First->IsPhysicallySettled());
	TestEqual(TEXT("The SDK delivers a terminal without waiting for HTTP cancellation"), Sink->Events.Num(), 1);
	if (!Sink->Events.IsEmpty())
	{
		TestEqual(TEXT("The terminal is cancellation"), Sink->Events.Last().Kind, EUnrealAIModelEventKind::Cancelled);
	}
	const auto NextSink = MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Second;
	TestTrue(TEXT("A logical replacement owns an independent physical permit"),
				  Provider.StartRequest(Request, Access, NextSink, Cancellation.GetToken(), Second, Error));
	Request.RequestId.Value = FGuid::NewGuid();
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Third;
	TestFalse(TEXT("Unsettled cancelled attempts still consume physical capacity"),
				   Provider.StartRequest(Request, Access, NextSink, Cancellation.GetToken(), Third, Error));
	Transport->Settle(0);
	TestTrue(TEXT("Only the terminal callback closes physical work"), First->IsPhysicallySettled());
	TestEqual(TEXT("Late physical cancellation cannot emit a second logical terminal"), Sink->Events.Num(), 1);
	Provider.BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeRetryCircuitTest, "UnrealAI.Native.RetryAndCircuitMechanisms",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeRetryCircuitTest::RunTest(const FString &)
{
	const auto Clock = MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>();
	FUnrealAIProviderRetryPolicy Policy;
	Policy.BreakerFailureThreshold = 1;
	Policy.BreakerOpenSeconds = 2.0;
	FUnrealAIProviderCircuitRegistry Registry(Clock, 2);
	FUnrealAIProviderCircuitKey Key;
	FString Error;
	const auto Connection = NativeFixtureConnections()->Find(TEXT("tests.sdk.connection"));
	TestTrue(TEXT("Circuit key binds an exact connection and capability set"),
				  FUnrealAIProviderCircuitKey::TryCreate(*Connection, TEXT("fixture-model"),
																		   EUnrealAIModelCapability::Text, Key, Error));
	FUnrealAIProviderCircuitPermit Permit;
	EUnrealAIProviderCircuitState State;
	TestTrue(TEXT("Closed circuit admits"), Registry.TryBegin(Key, Policy, Permit, State, Error));
	Permit.ReportTransientFailure();
	TestFalse(TEXT("Open circuit fails before provider work"), Registry.TryBegin(Key, Policy, Permit, State, Error));
	Clock->Advance(FTimespan::FromSeconds(2.1));
	TestTrue(TEXT("Monotonic expiry admits one half-open probe"), Registry.TryBegin(Key, Policy, Permit, State, Error));
	Permit.ReportSuccess();
	TestEqual(TEXT("A successful probe closes the circuit"), Registry.GetState(Key, Policy),
				   EUnrealAIProviderCircuitState::Closed);
	double Delay = 0.0;
	TestTrue(TEXT("Retry math respects an explicit server delay"),
				  FUnrealAIProviderRetryMath::TryComputeDelay(Policy, 1, 42, 2.0, 10.0, Delay));
	TestEqual(TEXT("The server delay is the lower bound"), Delay, 2.0);
	TestFalse(TEXT("Retry cannot start at or after the deadline"),
				   FUnrealAIProviderRetryMath::TryComputeDelay(Policy, 1, 42, 2.0, 2.0, Delay));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeDeadlineTest, "UnrealAI.Native.AbsoluteDeadlineWithoutNetworkTerminal",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeDeadlineTest::RunTest(const FString &)
{
	const auto Transport = MakeShared<FNativeFixtureTransport, ESPMode::ThreadSafe>();
	const auto Clock = MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>();
	auto Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
	Config.Clock = Clock;
	Config.bDrainEventsSynchronouslyForTesting = true;
	FUnrealAIOpenAIResponsesProvider Provider(NativeFixtureConnections(), Transport, Config);
	FUnrealAIModelRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.ConnectionAlias = TEXT("tests.sdk.connection");
	Request.ModelId = Config.ModelProfiles[0].ModelId;
	Request.TimeoutSeconds = 2.0f;
	FUnrealAIModelMessage Message;
	FUnrealAIModelContentPart Part;
	Part.Text = TEXT("Offline deadline fixture.");
	Message.Content.Add(MoveTemp(Part));
	Request.InputMessages.Add(MoveTemp(Message));
	const auto Sink = MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIModelError Error;
	if (!TestTrue(TEXT("Native deadline fixture admitted"),
					   Provider.StartRequest(Request, MakeShared<FNativeFixtureAccess, ESPMode::ThreadSafe>(), Sink,
											 Cancellation.GetToken(), Handle, Error)))
	{
		return false;
	}
	Clock->Advance(FTimespan::FromSeconds(2.1));
	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestTrue(TEXT("Deadline closes logical work even if the transport never sends a terminal"),
				  Handle->IsLogicallyComplete());
	TestFalse(TEXT("Deadline retains physical accounting"), Handle->IsPhysicallySettled());
	TestEqual(TEXT("One deadline terminal"), Sink->Events.Num(), 1);
	if (!Sink->Events.IsEmpty())
	{
		TestEqual(TEXT("Deadline is typed"), Sink->Events[0].Kind, EUnrealAIModelEventKind::TimedOut);
	}
	Transport->Settle(0);
	TestEqual(TEXT("Late cancellation cannot replace deadline outcome"), Sink->Events.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeAggregateBoundTest, "UnrealAI.Native.AggregateInputRetentionBound",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeAggregateBoundTest::RunTest(const FString &)
{
	FUnrealAIModelRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.ConnectionAlias = TEXT("tests.sdk.connection");
	Request.ModelId = TEXT("fixture");
	FUnrealAIModelContentPart Part;
	Part.Type = EUnrealAIContentType::Image;
	Part.MimeType = TEXT("image/png");
	Part.InlineBytes.Init(0, 1024 * 1024);
	const uint8 Signature[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	FMemory::Memcpy(Part.InlineBytes.GetData(), Signature, sizeof(Signature));
	FUnrealAIModelMessage Message;
	Message.Content.Add(Part);
	Request.InputMessages.Add(Message);
	FString Error;
	TestTrue(TEXT("One bounded image validates"), Request.ValidateShape(Error));
	for (int32 Index = 0; Index < 16; ++Index)
	{
		Request.InputMessages.Add(Message);
	}
	TestFalse(TEXT("Individually valid images cannot exceed the aggregate input bound"), Request.ValidateShape(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIGatewayPolicyTest, "UnrealAI.Native.ExplicitGatewayBillingPolicy",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIGatewayPolicyTest::RunTest(const FString &)
{
	auto Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
	Config.ProviderName = TEXT("gateway.responses");
	Config.AccountAuthProviderName = TEXT("game.session");
	Config.PublicFaultPolicy.CodePrefix = TEXT("gateway");
	Config.PublicFaultPolicy.ProviderDisplayName = TEXT("Game backend");
	Config.AuthScheme = EUnrealAIAuthScheme::GatewayBearer;
	Config.BillingMode = EUnrealAIBillingMode::GatewayAccounted;
	FString Error;
	TestTrue(TEXT("Explicit backend origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("https://game.example"), false, Config.EndpointOrigin, Error));
	Config.Audience = TEXT("https://game.example/v1/responses");
	TestTrue(TEXT("An explicit destination-bound gateway policy validates"), Config.ValidateShape(Error));
	Config.BillingMode = EUnrealAIBillingMode::ApiMetered;
	TestFalse(TEXT("Gateway access cannot impersonate provider billing"), Config.ValidateShape(Error));
	return true;
}
#endif
