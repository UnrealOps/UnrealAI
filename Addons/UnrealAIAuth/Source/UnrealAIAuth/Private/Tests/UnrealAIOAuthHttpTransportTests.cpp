// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"

#include <limits>

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
bool TryMakeOAuthTestSecret(FUnrealAISecretValue &OutSecret, FString &OutError)
{
	static const ANSICHAR Body[] = "client_id=public-test-client&grant_type=device_code";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Body), static_cast<int32>(sizeof(Body) - 1));
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

FUnrealAIEndpointOrigin MakeOAuthTestOrigin()
{
	FUnrealAIEndpointOrigin Origin;
	FString Error;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://auth.example"), false, Origin, Error);
	return Origin;
}

bool TryMakeOAuthTestRequest(const FUnrealAIRequestId &RequestId, FUnrealAIOAuthHttpRequest &OutRequest,
							 FString &OutError)
{
	FUnrealAISecretValue Body;
	if (!TryMakeOAuthTestSecret(Body, OutError))
	{
		return false;
	}
	return FUnrealAIOAuthHttpRequest::TryCreate(RequestId, MakeOAuthTestOrigin(),
												TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
													 MoveTemp(Body), 30.0, 32 * 1024, OutRequest, OutError);
}

FUnrealAIOAuthHttpError MakeOAuthTestError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIOAuthHttpErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIOAuthHttpError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

class FCountingOAuthSecretConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	bool bAccept = true;
	int32 Calls = 0;
	int32 LastNumBytes = 0;

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		++Calls;
		LastNumBytes = Secret.Num();
		return bAccept && !Secret.IsEmpty();
	}
};

class FImmediateFakeOAuthRequestHandle final : public IUnrealAIOAuthHttpRequestHandle
{
  public:
	explicit FImmediateFakeOAuthRequestHandle(const FUnrealAIRequestId &InRequestId) : RequestId(InRequestId) {}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}
	void Cancel() override {}

  private:
	FUnrealAIRequestId RequestId;
};

class FImmediateFakeOAuthTransport final : public IUnrealAIOAuthHttpTransport, public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	int32 BodyConsumerCalls = 0;
	int32 ConsumedBodyBytes = 0;

	bool StartRequest(FUnrealAIOAuthHttpRequest &&Request,
					  const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FString &OutError) override
	{
		OutHandle.Reset();
		if (!Request.ValidateShape(OutError) || Cancellation.IsCancellationRequested() ||
			!Request.TryConsumeBody(*this))
		{
			return false;
		}

		FUnrealAIOAuthHttpResponseMetadata Metadata;
		Metadata.StatusCode = 200;
		Metadata.ContentType = TEXT("application/json");
		FUnrealAISecretValue EmptyBody;
		FUnrealAIOAuthHttpResult Result;
		if (!FUnrealAIOAuthHttpResult::TryCreateResponse(Request.GetRequestId(), Metadata, MoveTemp(EmptyBody), Result,
														 OutError))
		{
			return false;
		}
		OutHandle = MakeShared<FImmediateFakeOAuthRequestHandle, ESPMode::ThreadSafe>(Request.GetRequestId());
		CompletionSink->CompleteOAuthHttpRequest(MoveTemp(Result));
		return true;
	}

	void BeginShutdown() override {}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		++BodyConsumerCalls;
		ConsumedBodyBytes = Secret.Num();
		return !Secret.IsEmpty();
	}
};

class FImmediateOAuthCompletionSink final : public IUnrealAIOAuthHttpCompletionSink
{
  public:
	int32 CompletionCount = 0;
	int32 StatusCode = 0;

	void CompleteOAuthHttpRequest(FUnrealAIOAuthHttpResult &&Result) override
	{
		++CompletionCount;
		StatusCode = Result.GetResponseMetadata().StatusCode;
	}
};

#if PLATFORM_MAC
class FNativeOAuthCompletionSink final : public IUnrealAIOAuthHttpCompletionSink
{
  public:
	FNativeOAuthCompletionSink() : TerminalEvent(FPlatformProcess::GetSynchEventFromPool(true)) {}
	~FNativeOAuthCompletionSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(TerminalEvent);
	}

	void CompleteOAuthHttpRequest(FUnrealAIOAuthHttpResult &&Result) override
	{
		{
			FScopeLock Lock(&Mutex);
			++CompletionCount;
			RequestId = Result.GetRequestId();
			TerminalKind = Result.GetTerminalKind();
			ErrorCode = Result.GetError().Code;
		}
		TerminalEvent->Trigger();
	}

	bool WaitForTerminal() const
	{
		return TerminalEvent->Wait(5000);
	}

	void Snapshot(int32 &OutCompletionCount, FUnrealAIRequestId &OutRequestId,
				  EUnrealAIOAuthHttpTerminalKind &OutTerminalKind, EUnrealAIOAuthHttpErrorCode &OutErrorCode) const
	{
		FScopeLock Lock(&Mutex);
		OutCompletionCount = CompletionCount;
		OutRequestId = RequestId;
		OutTerminalKind = TerminalKind;
		OutErrorCode = ErrorCode;
	}

  private:
	mutable FCriticalSection Mutex;
	FEvent *TerminalEvent = nullptr;
	int32 CompletionCount = 0;
	FUnrealAIRequestId RequestId;
	EUnrealAIOAuthHttpTerminalKind TerminalKind = EUnrealAIOAuthHttpTerminalKind::Invalid;
	EUnrealAIOAuthHttpErrorCode ErrorCode = EUnrealAIOAuthHttpErrorCode::None;
};
#endif
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthHttpRequestShapeTest, "UnrealAI.Transport.OAuthRequestShape",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthHttpRequestShapeTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FUnrealAIRequestId RequestId{FGuid(31, 32, 33, 34)};
	FString Error;
	FUnrealAIOAuthHttpRequest Valid;
	TestTrue(TEXT("A bounded form POST to an exact HTTPS origin is admitted"),
				  TryMakeOAuthTestRequest(RequestId, Valid, Error));
	TestTrue(TEXT("The admitted OAuth request remains valid"), Valid.ValidateShape(Error));
	TestEqual(TEXT("The request retains only its secret body byte count"), Valid.GetBodyBytes(), 51);
	TestEqual(TEXT("The legacy request factory remains a POST"), Valid.GetMethod(), EUnrealAIOAuthHttpMethod::Post);

	FUnrealAIOAuthHttpRequest DiscoveryGet;
	TestTrue(TEXT("A bounded bodyless metadata GET to an exact HTTPS origin is admitted"),
				  FUnrealAIOAuthHttpRequest::TryCreateGet(RequestId, MakeOAuthTestOrigin(),
														  TEXT("/.well-known/openid-configuration"), 15.0, 32 * 1024,
															   DiscoveryGet, Error));
	TestTrue(TEXT("The metadata GET remains valid"), DiscoveryGet.ValidateShape(Error));
	TestEqual(TEXT("The metadata request is explicitly GET"), DiscoveryGet.GetMethod(), EUnrealAIOAuthHttpMethod::Get);
	TestEqual(TEXT("The metadata GET carries no secret body"), DiscoveryGet.GetBodyBytes(), 0);
	FCountingOAuthSecretConsumer GetConsumer;
	TestFalse(TEXT("A metadata GET cannot materialize a request body"), DiscoveryGet.TryConsumeBody(GetConsumer));
	TestEqual(TEXT("A metadata GET never invokes the secret consumer"), GetConsumer.Calls, 0);
	FUnrealAIOAuthHttpRequest ArbitraryGet;
	TestFalse(TEXT("The bodyless GET capability is closed to well-known provider metadata"),
				   FUnrealAIOAuthHttpRequest::TryCreateGet(
					   RequestId, MakeOAuthTestOrigin(), TEXT("/oauth2/token"), 15.0, 32 * 1024, ArbitraryGet, Error));

	const TArray<FString> UnsafePaths = {TEXT("https://attacker.example/oauth2/token"),
											  TEXT("//attacker.example/token"),
												   TEXT("/oauth2/../token"), TEXT("/oauth2/token?audience=other"),
																				  TEXT("/oauth2/token#fragment"),
																					   TEXT("/oauth2/%2e%2e/token"),
																							TEXT("/oauth2\\token")};
	for (const FString &UnsafePath : UnsafePaths)
	{
		FUnrealAISecretValue Body;
		TestTrue(TEXT("Test body constructs"), TryMakeOAuthTestSecret(Body, Error));
		FUnrealAIOAuthHttpRequest Candidate;
		TestFalse(*FString::Printf(TEXT("Unsafe OAuth path is rejected: %s"), *UnsafePath),
								   FUnrealAIOAuthHttpRequest::TryCreate(RequestId, MakeOAuthTestOrigin(), UnsafePath,
																		EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		MoveTemp(Body), 30.0, 32 * 1024, Candidate,
																		Error));
	}

	FUnrealAIEndpointOrigin LoopbackPlaintext;
	TestTrue(TEXT("Core admits explicitly development-only loopback origins"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("http://127.0.0.1:8080"), true, LoopbackPlaintext, Error));
	FUnrealAISecretValue PlaintextBody;
	TestTrue(TEXT("Test body constructs"), TryMakeOAuthTestSecret(PlaintextBody, Error));
	FUnrealAIOAuthHttpRequest PlaintextRequest;
	TestFalse(TEXT("OAuth token transport rejects plaintext even on loopback"),
				   FUnrealAIOAuthHttpRequest::TryCreate(RequestId, LoopbackPlaintext,
														TEXT("/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
															 MoveTemp(PlaintextBody), 30.0, 32 * 1024, PlaintextRequest,
															 Error));

	FUnrealAISecretValue InvalidTypeBody;
	TestTrue(TEXT("Test body constructs"), TryMakeOAuthTestSecret(InvalidTypeBody, Error));
	FUnrealAIOAuthHttpRequest InvalidTypeRequest;
	TestFalse(TEXT("An uncompiled content type is rejected"),
				   FUnrealAIOAuthHttpRequest::TryCreate(RequestId, MakeOAuthTestOrigin(),
														TEXT("/token"), EUnrealAIOAuthHttpContentType::Invalid,
															 MoveTemp(InvalidTypeBody), 30.0, 32 * 1024,
															 InvalidTypeRequest, Error));

	FUnrealAISecretValue InvalidBoundBody;
	TestTrue(TEXT("Test body constructs"), TryMakeOAuthTestSecret(InvalidBoundBody, Error));
	FUnrealAIOAuthHttpRequest InvalidBoundRequest;
	TestFalse(TEXT("OAuth response bodies cannot exceed the secret-value bound"),
				   FUnrealAIOAuthHttpRequest::TryCreate(
					   RequestId, MakeOAuthTestOrigin(),
					   TEXT("/token"), EUnrealAIOAuthHttpContentType::ApplicationJson, MoveTemp(InvalidBoundBody), 30.0,
							FUnrealAIOAuthHttpRequest::MaxResponseBodyBytesLimit + 1, InvalidBoundRequest, Error));

	FUnrealAISecretValue InvalidTimeoutBody;
	TestTrue(TEXT("Test body constructs"), TryMakeOAuthTestSecret(InvalidTimeoutBody, Error));
	FUnrealAIOAuthHttpRequest InvalidTimeoutRequest;
	TestFalse(TEXT("Non-finite OAuth timeouts are rejected"),
				   FUnrealAIOAuthHttpRequest::TryCreate(
					   RequestId, MakeOAuthTestOrigin(),
					   TEXT("/token"), EUnrealAIOAuthHttpContentType::ApplicationJson, MoveTemp(InvalidTimeoutBody),
							std::numeric_limits<double>::quiet_NaN(), 1024, InvalidTimeoutRequest, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthHttpSecretConsumptionTest,
								 "UnrealAI.Transport.OAuthSecretConsumesExactlyOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthHttpSecretConsumptionTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	FUnrealAISecretValue Secret;
	if (!TestTrue(TEXT("Secret body constructs"), TryMakeOAuthTestSecret(Secret, Error)))
	{
		return false;
	}
	const int32 ExpectedBytes = Secret.Num();
	FUnrealAIOAuthHttpSecretPayload Payload;
	if (!TestTrue(TEXT("Move-only OAuth payload takes ownership"),
					   FUnrealAIOAuthHttpSecretPayload::TryCreate(MoveTemp(Secret), Payload, Error)))
	{
		return false;
	}
	TestEqual(TEXT("OAuth payload reveals only its bounded byte count"), Payload.Num(), ExpectedBytes);
	TestEqual(TEXT("OAuth payload display is redacted"), Payload.GetRedactedDisplay(),
				   FString(TEXT("<redacted:credential>")));

	FCountingOAuthSecretConsumer Consumer;
	TestTrue(TEXT("The secret payload is delivered through the consuming seam"), Payload.TryConsume(Consumer));
	TestFalse(TEXT("The secret payload cannot be consumed twice"), Payload.TryConsume(Consumer));
	TestFalse(TEXT("The secret payload is unset after consumption"), Payload.IsSet());
	TestEqual(TEXT("The consumer was invoked exactly once"), Consumer.Calls, 1);
	TestEqual(TEXT("The consumer saw the expected bounded body"), Consumer.LastNumBytes, ExpectedBytes);

	FUnrealAISecretValue RejectedSecret;
	TestTrue(TEXT("Rejected test body constructs"), TryMakeOAuthTestSecret(RejectedSecret, Error));
	FUnrealAIOAuthHttpSecretPayload RejectedPayload;
	TestTrue(TEXT("Rejected move-only payload takes ownership"),
				  FUnrealAIOAuthHttpSecretPayload::TryCreate(MoveTemp(RejectedSecret), RejectedPayload, Error));
	FCountingOAuthSecretConsumer RejectingConsumer;
	RejectingConsumer.bAccept = false;
	TestFalse(TEXT("A rejecting consumer reports failure"), RejectedPayload.TryConsume(RejectingConsumer));
	TestFalse(TEXT("A rejected payload is still wiped and cannot be retried"), RejectedPayload.IsSet());
	TestFalse(TEXT("A rejected payload cannot be consumed twice"), RejectedPayload.TryConsume(RejectingConsumer));
	TestEqual(TEXT("The rejecting consumer was invoked exactly once"), RejectingConsumer.Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthHttpResultShapeTest, "UnrealAI.Transport.OAuthResultShape",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthHttpResultShapeTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FUnrealAIRequestId RequestId{FGuid(41, 42, 43, 44)};
	FString Error;
	FUnrealAIOAuthHttpResponseMetadata Metadata;
	Metadata.StatusCode = 400;
	Metadata.ContentType = TEXT("application/json");
	Metadata.ProviderRequestId = TEXT("safe-request-id");
	Metadata.RetryAfterSeconds = 2.0f;

	FUnrealAISecretValue ResponseBody;
	if (!TestTrue(TEXT("Response secret constructs"), TryMakeOAuthTestSecret(ResponseBody, Error)))
	{
		return false;
	}
	FUnrealAIOAuthHttpResult Response;
	if (!TestTrue(TEXT("An HTTP error status remains a successful typed transport exchange"),
					   FUnrealAIOAuthHttpResult::TryCreateResponse(RequestId, Metadata, MoveTemp(ResponseBody),
																   Response, Error)))
	{
		return false;
	}
	TestTrue(TEXT("Response result shape is valid"), Response.ValidateShape(Error));
	TestTrue(TEXT("The transport completed successfully"), Response.IsTransportSuccess());
	TestFalse(TEXT("HTTP 400 is not reported as an HTTP success"), Response.IsHttpSuccess());
	TestEqual(TEXT("HTTP status is retained for provider error parsing"), Response.GetResponseMetadata().StatusCode,
				   400);

	FCountingOAuthSecretConsumer Consumer;
	TestTrue(TEXT("Response body is available to one parser"), Response.TryConsumeBody(Consumer));
	TestFalse(TEXT("Response body is unavailable after parser return"), Response.TryConsumeBody(Consumer));
	TestEqual(TEXT("Response parser ran exactly once"), Consumer.Calls, 1);

	const FUnrealAIOAuthHttpError CancelledError =
		MakeOAuthTestError(EUnrealAIErrorCategory::Cancelled, EUnrealAIOAuthHttpErrorCode::Cancelled);
	FUnrealAIOAuthHttpResult Cancelled;
	TestTrue(TEXT("Typed cancellation result is admitted"),
				  FUnrealAIOAuthHttpResult::TryCreateError(RequestId, EUnrealAIOAuthHttpTerminalKind::Cancelled,
														   CancelledError, Cancelled, Error));
	TestTrue(TEXT("Typed cancellation result remains valid"), Cancelled.ValidateShape(Error));

	FUnrealAIOAuthHttpResult Mismatched;
	TestFalse(TEXT("A failed terminal cannot masquerade as cancellation"),
				   FUnrealAIOAuthHttpResult::TryCreateError(RequestId, EUnrealAIOAuthHttpTerminalKind::Failed,
															CancelledError, Mismatched, Error));

	FUnrealAIOAuthHttpError RetryableRedirect =
		MakeOAuthTestError(EUnrealAIErrorCategory::Transport, EUnrealAIOAuthHttpErrorCode::RedirectRejected, true);
	TestFalse(TEXT("Redirect rejection cannot be marked retryable"), RetryableRedirect.ValidateShape(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthHttpTransportOptionsTest, "UnrealAI.Transport.OAuthOptions",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthHttpTransportOptionsTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	FUnrealAIOAuthHttpTransportOptions Options;
	TestTrue(TEXT("Default OAuth transport options are valid"), Options.ValidateShape(Error));
	Options.MaxActiveRequests = 0;
	TestFalse(TEXT("OAuth transport rejects zero active capacity"), Options.ValidateShape(Error));
	Options = FUnrealAIOAuthHttpTransportOptions();
	Options.MaxActiveRequests = FUnrealAIOAuthHttpTransportOptions::MaxActiveRequestsLimit + 1;
	TestFalse(TEXT("OAuth transport rejects unbounded active capacity"), Options.ValidateShape(Error));
	Options = FUnrealAIOAuthHttpTransportOptions();
	Options.CancellationPollSeconds = FUnrealAIOAuthHttpTransportOptions::MinCancellationPollSeconds / 2.0;
	TestFalse(TEXT("OAuth transport rejects busy-loop cancellation polling"), Options.ValidateShape(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthHttpInjectableFakeTest, "UnrealAI.Transport.OAuthInjectableFake",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthHttpInjectableFakeTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FUnrealAIRequestId RequestId{FGuid(45, 46, 47, 48)};
	FUnrealAIOAuthHttpRequest Request;
	FString Error;
	if (!TestTrue(TEXT("Fake transport request constructs"), TryMakeOAuthTestRequest(RequestId, Request, Error)))
	{
		return false;
	}
	const int32 ExpectedBodyBytes = Request.GetBodyBytes();
	const TSharedRef<FImmediateFakeOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FImmediateFakeOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FImmediateOAuthCompletionSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FImmediateOAuthCompletionSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> Handle;
	TestTrue(TEXT("Provider code can inject a deterministic transport fake"),
				  Transport->StartRequest(MoveTemp(Request), Sink, Cancellation.GetToken(), Handle, Error));
	TestTrue(TEXT("Fake transport returns the request handle"), Handle.IsValid());
	TestEqual(TEXT("Fake transport consumes the request body exactly once"), Transport->BodyConsumerCalls, 1);
	TestEqual(TEXT("Fake transport receives the bounded request body"), Transport->ConsumedBodyBytes,
				   ExpectedBodyBytes);
	TestEqual(TEXT("Fake transport publishes one completion"), Sink->CompletionCount, 1);
	TestEqual(TEXT("Fake transport preserves typed HTTP status"), Sink->StatusCode, 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIMacOAuthHttpTransportCancellationTest,
								 "UnrealAI.Transport.MacOAuthCancelExactlyOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIMacOAuthHttpTransportCancellationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
#if PLATFORM_MAC
	TestTrue(TEXT("macOS exposes the native OAuth HTTPS transport"), IsAgentPlatformOAuthHttpsTransportSupported());
	const FUnrealAIRequestId RequestId{FGuid(51, 52, 53, 54)};
	FUnrealAIOAuthHttpRequest Request;
	FString Error;
	if (!TestTrue(TEXT("Native OAuth cancellation request constructs"),
					   TryMakeOAuthTestRequest(RequestId, Request, Error)))
	{
		return false;
	}

	FUnrealAIOAuthHttpTransportOptions Options;
	Options.bSuspendNativeTaskForTesting = true;
	const TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> Transport =
		CreateAgentPlatformOAuthHttpsTransport(Options);
	const TSharedRef<FNativeOAuthCompletionSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FNativeOAuthCompletionSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> Handle;
	if (!TestTrue(TEXT("Native OAuth NSURLSession request is admitted"),
					   Transport->StartRequest(MoveTemp(Request), Sink, Cancellation.GetToken(), Handle, Error)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Native OAuth transport returns its cancellation handle"), Handle.IsValid()))
	{
		return false;
	}
	Handle->Cancel();
	Handle->Cancel();
	Transport->BeginShutdown();
	TestTrue(TEXT("Native OAuth cancellation publishes a terminal"), Sink->WaitForTerminal());

	int32 CompletionCount = 0;
	FUnrealAIRequestId CompletedRequestId;
	EUnrealAIOAuthHttpTerminalKind TerminalKind = EUnrealAIOAuthHttpTerminalKind::Invalid;
	EUnrealAIOAuthHttpErrorCode ErrorCode = EUnrealAIOAuthHttpErrorCode::None;
	Sink->Snapshot(CompletionCount, CompletedRequestId, TerminalKind, ErrorCode);
	TestEqual(TEXT("Caller cancel, duplicate cancel, shutdown, and native completion produce one terminal"),
				   CompletionCount, 1);
	TestTrue(TEXT("Cancellation result retains its exact request binding"), CompletedRequestId == RequestId);
	TestEqual(TEXT("Cancellation retains its closed terminal kind"), TerminalKind,
				   EUnrealAIOAuthHttpTerminalKind::Cancelled);
	TestEqual(TEXT("Cancellation retains its closed error code"), ErrorCode, EUnrealAIOAuthHttpErrorCode::Cancelled);
#else
	TestFalse(TEXT("Non-macOS builds expose no native macOS OAuth transport"),
				   IsAgentPlatformOAuthHttpsTransportSupported());
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIMacOAuthHttpTransportTimeoutTokenTest,
								 "UnrealAI.Transport.MacOAuthTimeoutTokenExactlyOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIMacOAuthHttpTransportTimeoutTokenTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
#if PLATFORM_MAC
	const FUnrealAIRequestId RequestId{FGuid(61, 62, 63, 64)};
	FUnrealAIOAuthHttpRequest Request;
	FString Error;
	if (!TestTrue(TEXT("Native OAuth timeout request constructs"), TryMakeOAuthTestRequest(RequestId, Request, Error)))
	{
		return false;
	}

	FUnrealAIOAuthHttpTransportOptions Options;
	Options.bSuspendNativeTaskForTesting = true;
	const TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> Transport =
		CreateAgentPlatformOAuthHttpsTransport(Options);
	const TSharedRef<FNativeOAuthCompletionSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FNativeOAuthCompletionSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> Handle;
	if (!TestTrue(TEXT("Native OAuth timeout request is admitted"),
					   Transport->StartRequest(MoveTemp(Request), Sink, Cancellation.GetToken(), Handle, Error)))
	{
		return false;
	}
	Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
	TestTrue(TEXT("Timeout cancellation publishes a terminal"), Sink->WaitForTerminal());
	Handle->Cancel();
	Transport->BeginShutdown();

	int32 CompletionCount = 0;
	FUnrealAIRequestId CompletedRequestId;
	EUnrealAIOAuthHttpTerminalKind TerminalKind = EUnrealAIOAuthHttpTerminalKind::Invalid;
	EUnrealAIOAuthHttpErrorCode ErrorCode = EUnrealAIOAuthHttpErrorCode::None;
	Sink->Snapshot(CompletionCount, CompletedRequestId, TerminalKind, ErrorCode);
	TestEqual(TEXT("Timeout, caller cancel, shutdown, and native completion produce one terminal"), CompletionCount, 1);
	TestTrue(TEXT("Timeout result retains its exact request binding"), CompletedRequestId == RequestId);
	TestEqual(TEXT("Timeout cancellation retains its closed terminal kind"), TerminalKind,
				   EUnrealAIOAuthHttpTerminalKind::TimedOut);
	TestEqual(TEXT("Timeout cancellation retains its closed error code"), ErrorCode,
				   EUnrealAIOAuthHttpErrorCode::TimedOut);
#endif
	return true;
}

#endif
