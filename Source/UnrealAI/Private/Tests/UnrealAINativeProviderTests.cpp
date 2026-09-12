// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Containers/Ticker.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "OpenAI/UnrealAIOpenAIResponsesProvider.h"
#include "Runtime/UnrealAIPhysicalRequestBudget.h"
#include "Runtime/UnrealAIFailureClassification.h"
#include "Runtime/UnrealAIProviderReliability.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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
		TArray<uint8> BodyUtf8;
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
		Attempt.BodyUtf8 = Request.Body;
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
	void RejectHttp(int32 Index, int32 StatusCode, const FString &Body)
	{
		Respond(Index, StatusCode, TEXT("application/json"), Body);
	}
	void Respond(int32 Index, int32 StatusCode, const FString &ContentType, const FString &Body)
	{
		FAttempt &Attempt = Attempts[Index];
		const TSharedPtr<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> Sink = MoveTemp(Attempt.Sink);
		FUnrealAIHttpEvent Started;
		Started.RequestId = Attempt.Handle->Id;
		Started.Sequence = ++Attempt.Sequence;
		Started.Kind = EUnrealAIHttpEventKind::ResponseStarted;
		Started.Response.StatusCode = StatusCode;
		Started.Response.ContentType = ContentType;
		Sink->EnqueueHttpEvent(MoveTemp(Started));
		FUnrealAIHttpEvent Chunk;
		Chunk.RequestId = Attempt.Handle->Id;
		Chunk.Sequence = ++Attempt.Sequence;
		Chunk.Kind = EUnrealAIHttpEventKind::BodyChunk;
		FTCHARToUTF8 Utf8(*Body);
		Chunk.BodyChunk.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
		Sink->EnqueueHttpEvent(MoveTemp(Chunk));
		FUnrealAIHttpEvent Completed;
		Completed.RequestId = Attempt.Handle->Id;
		Completed.Sequence = ++Attempt.Sequence;
		Completed.Kind = EUnrealAIHttpEventKind::Completed;
		Sink->EnqueueHttpEvent(MoveTemp(Completed));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeContentTypePolicyTest, "UnrealAI.Native.MissingContentTypePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeContentTypePolicyTest::RunTest(const FString &)
{
	struct FCase
	{
		const TCHAR *ContentType;
		bool bAllowMissing;
		bool bValidStream;
		bool bSuccess;
	};
	const FCase Cases[] = {{TEXT("text/event-stream"), false, true, true}, {TEXT(""), false, true, false},
		{TEXT(""), true, true, true}, {TEXT("application/json"), true, true, false},
		{TEXT("text/html"), true, true, false}, {TEXT(""), true, false, false}};
	for (const FCase &Case : Cases)
	{
		const TSharedRef<FNativeFixtureTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FNativeFixtureTransport, ESPMode::ThreadSafe>();
		FUnrealAIOpenAIResponsesProviderConfig Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
		TestFalse(TEXT("Public API requires a media header by default"), Config.bAllowMissingResponseContentType);
		Config.bDrainEventsSynchronouslyForTesting = true;
		Config.bAllowMissingResponseContentType = Case.bAllowMissing;
		FUnrealAIOpenAIResponsesProvider Provider(NativeFixtureConnections(), Transport, Config);
		FUnrealAIModelRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.ConnectionAlias = TEXT("tests.sdk.connection");
		Request.ModelId = Config.ModelProfiles[0].ModelId;
		FUnrealAIModelMessage Message;
		FUnrealAIModelContentPart Part;
		Part.Text = TEXT("Offline content-type fixture.");
		Message.Content.Add(MoveTemp(Part));
		Request.InputMessages.Add(MoveTemp(Message));
		const TSharedRef<FNativeFixtureSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIModelError Error;
		FUnrealAICancellationSource Cancellation;
		if (!TestTrue(TEXT("Media-header fixture admitted"), Provider.StartRequest(Request,
			MakeShared<FNativeFixtureAccess, ESPMode::ThreadSafe>(), Sink, Cancellation.GetToken(), Handle, Error)))
		{
			return false;
		}
		const FString Stream = TEXT("data: {\"type\":\"response.created\",\"sequence_number\":0,\"response\":{\"id\":\"resp_media\"}}\n\n")
			TEXT("data: {\"type\":\"response.completed\",\"sequence_number\":1,\"response\":{\"id\":\"resp_media\",\"output\":[{\"type\":\"message\",\"id\":\"msg_media\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\",\"annotations\":[]}]}],\"usage\":{\"input_tokens\":1,\"output_tokens\":1,\"total_tokens\":2}}}\n\n");
		Transport->Respond(0, 200, Case.ContentType, Case.bValidStream ? Stream : TEXT("{\"not\":\"an SSE stream\"}"));
		const TArray<FUnrealAIModelEvent> Terminals = Sink->Events.FilterByPredicate(
			[](const FUnrealAIModelEvent &Event) { return Event.IsTerminal(); });
		TestEqual(TEXT("Exactly one media-policy terminal"), Terminals.Num(), 1);
		if (Terminals.Num() == 1)
		{
			TestEqual(TEXT("Only allowed, valid SSE completes"),
				Terminals[0].Kind == EUnrealAIModelEventKind::Completed, Case.bSuccess);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeOutputTokenPolicyTest, "UnrealAI.Native.EndpointOutputTokenPolicy",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeOutputTokenPolicyTest::RunTest(const FString &)
{
	for (const bool bSendLimit : {true, false})
	{
		const TSharedRef<FNativeFixtureTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FNativeFixtureTransport, ESPMode::ThreadSafe>();
		FUnrealAIOpenAIResponsesProviderConfig Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
		TestTrue(TEXT("Public API sends token limit by default"), Config.bSendMaxOutputTokens);
		Config.bDrainEventsSynchronouslyForTesting = true;
		Config.bSendMaxOutputTokens = bSendLimit;
		FUnrealAIOpenAIResponsesProvider Provider(NativeFixtureConnections(), Transport, Config);
		FUnrealAIModelRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.ConnectionAlias = TEXT("tests.sdk.connection");
		Request.ModelId = Config.ModelProfiles[0].ModelId;
		Request.MaxOutputTokens = 512;
		FUnrealAIModelMessage Message;
		FUnrealAIModelContentPart Part;
		Part.Text = TEXT("Offline endpoint wire policy fixture.");
		Message.Content.Add(MoveTemp(Part));
		Request.InputMessages.Add(MoveTemp(Message));
		const TSharedRef<FNativeFixtureSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIModelError Error;
		FUnrealAICancellationSource Cancellation;
		if (!TestTrue(TEXT("Endpoint policy request admitted"), Provider.StartRequest(Request,
			MakeShared<FNativeFixtureAccess, ESPMode::ThreadSafe>(), Sink, Cancellation.GetToken(), Handle, Error)))
		{
			return false;
		}
		const TArray<uint8> &Body = Transport->Attempts[0].BodyUtf8;
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR *>(Body.GetData()), Body.Num());
		const FString Json(Decoded.Length(), Decoded.Get());
		TSharedPtr<FJsonObject> Root;
		if (!TestTrue(TEXT("Transport receives valid JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid()))
		{
			return false;
		}
		TestEqual(TEXT("Endpoint policy controls the token-limit field"), Root->HasField(TEXT("max_output_tokens")), bSendLimit);
		if (bSendLimit)
		{
			TestEqual(TEXT("Requested token limit is preserved"), Root->GetIntegerField(TEXT("max_output_tokens")), 512);
		}
		TestTrue(TEXT("Streaming remains enabled"), Root->GetBoolField(TEXT("stream")));
		TestFalse(TEXT("Storage remains disabled"), Root->GetBoolField(TEXT("store")));
		TestTrue(TEXT("Stateless reasoning continuity remains requested"), Root->HasField(TEXT("include")));
		Handle->Cancel();
		Transport->Settle(0);
		Config.bUseChatCompletions = true;
		FString ShapeError;
		TestEqual(TEXT("Chat conversion requires its token-limit input"), Config.ValidateShape(ShapeError), bSendLimit);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeHttpStatusTest, "UnrealAI.Native.HttpStatusWithoutResponseDisclosure",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeHttpStatusTest::RunTest(const FString &)
{
	for (const int32 Status : {400, 401, 403, 429, 500})
	{
		const TSharedRef<FNativeFixtureTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FNativeFixtureTransport, ESPMode::ThreadSafe>();
		FUnrealAIOpenAIResponsesProviderConfig Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
		Config.bDrainEventsSynchronouslyForTesting = true;
		FUnrealAIOpenAIResponsesProvider Provider(NativeFixtureConnections(), Transport, Config);
		FUnrealAIModelRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.ConnectionAlias = TEXT("tests.sdk.connection");
		Request.ModelId = Config.ModelProfiles[0].ModelId;
		FUnrealAIModelMessage Message;
		FUnrealAIModelContentPart Part;
		Part.Text = TEXT("Offline HTTP diagnostics fixture.");
		Message.Content.Add(MoveTemp(Part));
		Request.InputMessages.Add(MoveTemp(Message));
		const TSharedRef<FNativeFixtureSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIModelError Error;
		FUnrealAICancellationSource Cancellation;
		if (!TestTrue(TEXT("HTTP fixture admitted"), Provider.StartRequest(Request,
			MakeShared<FNativeFixtureAccess, ESPMode::ThreadSafe>(), Sink, Cancellation.GetToken(), Handle, Error)))
		{
			return false;
		}
		Transport->RejectHttp(0, Status, TEXT("{\"error\":{\"message\":\"private fixture content\"}}"));
		const TArray<FUnrealAIModelEvent> Terminals = Sink->Events.FilterByPredicate(
			[](const FUnrealAIModelEvent &Event) { return Event.IsTerminal(); });
		TestEqual(TEXT("One HTTP terminal"), Terminals.Num(), 1);
		if (Terminals.Num() == 1)
		{
			TestEqual(TEXT("Rejected request fails"), Terminals[0].Kind, EUnrealAIModelEventKind::Failed);
			const FUnrealAIModelError &Failure = Terminals[0].Error;
			TestTrue(TEXT("Numeric HTTP status survives public presentation"),
				Failure.UserMessage.ToString().Contains(FString::FromInt(Status)));
			TestFalse(TEXT("Response body is not presented"),
				Failure.UserMessage.ToString().Contains(TEXT("private fixture content")));
			TestFalse(TEXT("Response body is not retained in diagnostics"),
				Failure.DiagnosticMessage.Contains(TEXT("private fixture content")));
			TestEqual(TEXT("Original numeric metadata retained"),
				Failure.Metadata.FindRef(TEXT("http_status")), FString::FromInt(Status));
		}
	}
	const auto Summary = [](const FString &Json)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return UE::UnrealAI::Reliability::GetPublicHttpFailureSummary(
			MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()));
	};
	TestEqual(TEXT("Known rejected parameter has a fixed public description"),
		Summary(TEXT("{\"detail\":\"Unsupported parameter: max_output_tokens\",\"other\":\"private fixture content\"}")),
		FString(TEXT("Unsupported request parameter: max_output_tokens.")));
	TestEqual(TEXT("String error envelope has the same fixed description"),
		Summary(TEXT("{\"error\":\"Unsupported parameter: 'max_output_tokens'\"}")),
		FString(TEXT("Unsupported request parameter: max_output_tokens.")));
	TestEqual(TEXT("Unsupported subscription model is explained without echoing its name"),
		Summary(TEXT("{\"detail\":\"The 'private-fixture-model' model is not supported when using Codex with a ChatGPT account.\"}")),
		FString(TEXT("The selected model is unavailable for this connection.")));
	TestTrue(TEXT("Unknown provider prose is not surfaced"),
		Summary(TEXT("{\"error\":{\"message\":\"private fixture content\"}}")).IsEmpty());
	TestTrue(TEXT("Unknown parameter names are not surfaced"),
		Summary(TEXT("{\"error\":{\"code\":\"unsupported_parameter\",\"param\":\"private_fixture_content\"}}")).IsEmpty());
	TestTrue(TEXT("Malformed JSON is not summarized"), Summary(TEXT("{not json")).IsEmpty());
	TestTrue(TEXT("Duplicate keys cannot select a public explanation"),
		Summary(TEXT("{\"detail\":\"Unsupported parameter: max_output_tokens\",\"detail\":\"private fixture content\"}")).IsEmpty());
	return true;
}

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativeMultiTurnHistoryTest, "UnrealAI.Native.MultiTurnToolHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAINativeMultiTurnHistoryTest::RunTest(const FString &)
{
	for (const bool bMetadataOnly : {false, true})
	{
		const auto Transport = MakeShared<FNativeFixtureTransport, ESPMode::ThreadSafe>();
		FUnrealAIOpenAIResponsesProviderConfig Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
		Config.bDrainEventsSynchronouslyForTesting = true;
		Config.bAllowEmptyTerminalOutput = bMetadataOnly;
		FUnrealAIOpenAIResponsesProvider Provider(NativeFixtureConnections(), Transport, Config);
		FUnrealAIModelRequest Request;
		Request.ConnectionAlias = TEXT("tests.sdk.connection");
		Request.ModelId = Config.ModelProfiles[0].ModelId;
		FUnrealAIModelMessage Message;
		FUnrealAIModelContentPart Part;
		Part.Text = TEXT("Retain every offline tool exchange.");
		Message.Content.Add(Part);
		Request.InputMessages.Add(Message);
		FUnrealAIModelToolDescriptor Tool;
		Tool.StableName = TEXT("tests.echo");
		Tool.InvocationName = TEXT("echo_v1");
		Tool.Description = TEXT("Offline history fixture.");
		Tool.InputJsonSchema = TEXT("{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
		Request.Tools.Add(Tool);
		for (int32 Turn = 0; Turn < 3; ++Turn)
		{
			Request.RequestId.Value = FGuid::NewGuid();
			const auto Sink = MakeShared<FNativeFixtureSink, ESPMode::ThreadSafe>();
			TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Handle;
			FUnrealAIModelError Error;
			FUnrealAICancellationSource Cancellation;
			const auto Previous = Request.Continuation;
			if (!TestTrue(TEXT("A continued native request is admitted"), Provider.StartRequest(Request,
				MakeShared<FNativeFixtureAccess, ESPMode::ThreadSafe>(), Sink, Cancellation.GetToken(), Handle, Error)))
			{
				AddError(Error.Code.ToString());
				return false;
			}
			if (Previous.IsValid())
			{
				TestFalse(TEXT("Admission consumes the preceding continuation exactly once"), Previous->IsValid());
			}
			const auto &Bytes = Transport->Attempts[Turn].BodyUtf8;
			const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
			TSharedPtr<FJsonObject> Body;
			if (!TestTrue(TEXT("Recorded wire request is JSON"), FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(FString(Decoded.Length(), Decoded.Get())), Body)))
			{
				return false;
			}
			const auto &Input = Body->GetArrayField(TEXT("input"));
			if (!TestEqual(TEXT("Original message plus every preceding exchange exactly once"), Input.Num(), 1 + 3 * Turn))
			{
				return false;
			}
			TestEqual(TEXT("Original message is retained once"), Input[0]->AsObject()->GetStringField(TEXT("content")), Part.Text);
			for (int32 Prior = 0; Prior < Turn; ++Prior)
			{
				TestEqual(TEXT("Opaque reasoning is preserved"),
					Input[1 + 3 * Prior]->AsObject()->GetStringField(TEXT("encrypted_content")),
					FString::Printf(TEXT("opaque_fixture_%d"), Prior));
				TestEqual(TEXT("Prior function identity is preserved"),
					Input[2 + 3 * Prior]->AsObject()->GetStringField(TEXT("call_id")),
					FString::Printf(TEXT("call_%d"), Prior));
				TestEqual(TEXT("Prior tool result is preserved"),
					Input[3 + 3 * Prior]->AsObject()->GetStringField(TEXT("output")),
					FString::Printf(TEXT("{\"step\":%d}"), Prior));
			}
			const FString Reasoning = FString::Printf(
				TEXT("{\"type\":\"reasoning\",\"id\":\"rs_%d\",\"summary\":[],\"encrypted_content\":\"opaque_fixture_%d\"}"), Turn, Turn);
			const FString Function = FString::Printf(
				TEXT("{\"type\":\"function_call\",\"id\":\"fc_%d\",\"call_id\":\"call_%d\",\"name\":\"echo_v1\",\"arguments\":\"{}\"}"), Turn, Turn);
			FString Stream = FString::Printf(
				TEXT("data: {\"type\":\"response.created\",\"sequence_number\":0,\"response\":{\"id\":\"resp_%d\"}}\n\n"), Turn);
			Stream += TEXT("data: {\"type\":\"response.output_item.done\",\"sequence_number\":1,\"output_index\":0,\"item\":") + Reasoning + TEXT("}\n\n");
			Stream += TEXT("data: {\"type\":\"response.output_item.done\",\"sequence_number\":2,\"output_index\":1,\"item\":") + Function + TEXT("}\n\n");
			const FString Output = bMetadataOnly ? TEXT("[]") : TEXT("[") + Reasoning + TEXT(",") + Function + TEXT("]");
			Stream += FString::Printf(TEXT("data: {\"type\":\"response.completed\",\"sequence_number\":3,\"response\":{\"id\":\"resp_%d\",\"output\":"), Turn) + Output +
				TEXT(",\"usage\":{\"input_tokens\":10,\"output_tokens\":5,\"total_tokens\":15}}}\n\n");
			Transport->Respond(Turn, 200, TEXT("text/event-stream"), Stream);
			if (!TestTrue(TEXT("The response retains a new continuation"),
				!Sink->Events.IsEmpty() && Sink->Events.Last().Kind == EUnrealAIModelEventKind::Completed &&
				Sink->Events.Last().Continuation.IsValid()))
			{
				return false;
			}
			Request.Continuation = Sink->Events.Last().Continuation;
			Request.ToolOutputs.Reset();
			FUnrealAIModelToolOutput ToolOutput;
			ToolOutput.ProviderCallId = FString::Printf(TEXT("call_%d"), Turn);
			ToolOutput.OutputJson = FString::Printf(TEXT("{\"step\":%d}"), Turn);
			Request.ToolOutputs.Add(ToolOutput);
		}
	}
	return true;
}
#endif
