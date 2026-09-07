// Copyright EngineWorks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "Testing/UnrealAITestClock.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"
#include "XAI/UnrealAIXAIDeviceOAuthDriver.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
constexpr const TCHAR *ValidDiscoveryJson =
	TEXT("{\"issuer\":\"https://auth.x.ai\",\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",")
		TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",")
			TEXT("\"device_authorization_endpoint\":\"https://auth.x.ai/oauth2/device/code\"}");
constexpr const TCHAR *ValidDeviceJson = TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
	TEXT("\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",")
		TEXT("\"verification_uri_complete\":\"https://accounts.x.ai/oauth2/device?user_code=ABCD-EFGH\",")
			TEXT("\"expires_in\":1800,\"interval\":1}");
constexpr const TCHAR *ValidTokenJson =
	TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\",")
		TEXT("\"id_token\":\"fixture-id\",\"token_type\":\"Bearer\",\"expires_in\":3600,")
			TEXT("\"scope\":\"openid profile email offline_access grok-cli:access api:access\"}");

struct FXAIFixtureExchange final
{
	EUnrealAIOAuthHttpMethod Method = EUnrealAIOAuthHttpMethod::Invalid;
	FString RelativePath;
	EUnrealAIOAuthHttpContentType ContentType = EUnrealAIOAuthHttpContentType::Invalid;
	int32 StatusCode = 200;
	FString Body;
	float RetryAfterSeconds = 0.0f;
};

bool TryMakeSecret(const FStringView Value, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	const FString Owned(Value);
	const FTCHARToUTF8 Utf8(*Owned);
	TArray<uint8> Bytes;
	if (Utf8.Length() > 0)
	{
		Bytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	}
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

class FXAITestRequestHandle final : public IUnrealAIOAuthHttpRequestHandle
{
  public:
	explicit FXAITestRequestHandle(const FUnrealAIRequestId &InRequestId) : RequestId(InRequestId) {}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}
	void Cancel() override
	{
		++CancelCalls;
	}

	int32 CancelCalls = 0;

  private:
	FUnrealAIRequestId RequestId;
};

class FScriptedXAIOAuthTransport final : public IUnrealAIOAuthHttpTransport, public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	void Enqueue(FXAIFixtureExchange Exchange)
	{
		Script.Add(MoveTemp(Exchange));
	}

	bool StartRequest(FUnrealAIOAuthHttpRequest &&Request,
					  const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FString &OutError) override
	{
		OutHandle.Reset();
		LastConsumedBody.Reset();
		if (bShutdown || Cancellation.IsCancellationRequested() || NextExchange >= Script.Num())
		{
			OutError = TEXT("Scripted xAI OAuth transport exhausted or unavailable.");
			return false;
		}
		const FXAIFixtureExchange &Expected = Script[NextExchange++];
		const FString Origin = Request.GetEndpointOrigin().ToString();
		if (Origin != TEXT("https://auth.x.ai") || Request.GetMethod() != Expected.Method ||
						   Request.GetRelativePath() != Expected.RelativePath ||
						   Request.GetContentType() != Expected.ContentType)
		{
			bAllRequestsMatched = false;
		}
		if (Request.GetMethod() == EUnrealAIOAuthHttpMethod::Post)
		{
			if (!Request.TryConsumeBody(*this))
			{
				OutError = TEXT("Scripted xAI OAuth transport could not consume the request body.");
				return false;
			}
		}
		else if (Request.GetBodyBytes() != 0 || Request.TryConsumeBody(*this))
		{
			bAllRequestsMatched = false;
		}
		CapturedBodies.Add(LastConsumedBody);

		FUnrealAIOAuthHttpResponseMetadata Metadata;
		Metadata.StatusCode = Expected.StatusCode;
		Metadata.ContentType = TEXT("application/json");
		Metadata.ProviderRequestId = TEXT("fixture-request");
		Metadata.RetryAfterSeconds = Expected.RetryAfterSeconds;
		FUnrealAISecretValue ResponseBody;
		if (!Expected.Body.IsEmpty() && !TryMakeSecret(Expected.Body, ResponseBody, OutError))
		{
			return false;
		}
		FUnrealAIOAuthHttpResult Result;
		if (!FUnrealAIOAuthHttpResult::TryCreateResponse(Request.GetRequestId(), Metadata, MoveTemp(ResponseBody),
														 Result, OutError))
		{
			return false;
		}
		OutHandle = MakeShared<FXAITestRequestHandle, ESPMode::ThreadSafe>(Request.GetRequestId());
		CompletionSink->CompleteOAuthHttpRequest(MoveTemp(Result));
		return true;
	}

	void BeginShutdown() override
	{
		bShutdown = true;
	}

	int32 GetRequestCount() const
	{
		return NextExchange;
	}

	bool DidConsumeScript() const
	{
		return NextExchange == Script.Num();
	}

	bool DidAllRequestsMatch() const
	{
		return bAllRequestsMatched;
	}

	const FString &GetCapturedBody(const int32 Index) const
	{
		return CapturedBodies[Index];
	}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Secret.GetData()), Secret.Num());
		if (Converted.Length() <= 0)
		{
			return false;
		}
		LastConsumedBody = FString(Converted.Length(), Converted.Get());
		return true;
	}

  private:
	TArray<FXAIFixtureExchange> Script;
	TArray<FString> CapturedBodies;
	FString LastConsumedBody;
	int32 NextExchange = 0;
	bool bAllRequestsMatched = true;
	bool bShutdown = false;
};

class FCapturingXAIInteractionPublisher final : public IUnrealAIDeviceOAuthInteractionPublisher
{
  public:
	bool PublishInteraction(FUnrealAIAuthInteraction &&Interaction) override
	{
		++Calls;
		if (Calls != 1 || !Interaction.IsValid())
		{
			return false;
		}
		LaunchUri = Interaction.GetLaunchUri();
		UserCode = FString(Interaction.GetUserCode());
		return true;
	}

	int32 Calls = 0;
	FString LaunchUri;
	FString UserCode;
};

class FAdvancingXAIPollWaiter final : public IUnrealAIXAIOAuthPollWaiter
{
  public:
	explicit FAdvancingXAIPollWaiter(const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> &InClock)
		: Clock(InClock)
	{
	}

	bool Wait(const double Seconds, const double OverallDeadlineSeconds, const IUnrealAIClock &InClock,
			  const FUnrealAICancellationToken &Cancellation) override
	{
		(void)InClock;
		WaitSeconds.Add(Seconds);
		if (ObservedTransport != nullptr)
		{
			RequestCountsAtWait.Add(ObservedTransport->GetRequestCount());
		}
		if (CancellationSource != nullptr && WaitSeconds.Num() == CancelOnWaitNumber)
		{
			CancellationSource->Cancel(EUnrealAICancellationReason::Requested);
		}
		Clock->Advance(FTimespan::FromSeconds(Seconds));
		return !Cancellation.IsCancellationRequested() && Clock->MonotonicSeconds() < OverallDeadlineSeconds;
	}

	TArray<double> WaitSeconds;
	TArray<int32> RequestCountsAtWait;
	const FScriptedXAIOAuthTransport *ObservedTransport = nullptr;
	const FUnrealAICancellationSource *CancellationSource = nullptr;
	int32 CancelOnWaitNumber = MAX_int32;

  private:
	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock;
};

class FCapturingSecretConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FString Value;

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Secret.GetData()), Secret.Num());
		Value = FString(Converted.Length(), Converted.Get());
		return Converted.Length() > 0;
	}
};

FXAIFixtureExchange MakeExchange(const EUnrealAIOAuthHttpMethod Method, const TCHAR *Path,
								 const EUnrealAIOAuthHttpContentType ContentType, const TCHAR *Body,
								 const int32 StatusCode = 200)
{
	FXAIFixtureExchange Exchange;
	Exchange.Method = Method;
	Exchange.RelativePath = Path;
	Exchange.ContentType = ContentType;
	Exchange.StatusCode = StatusCode;
	Exchange.Body = Body;
	return Exchange;
}

void EnqueueDiscoveryAndDevice(FScriptedXAIOAuthTransport &Transport, const TCHAR *DeviceJson = ValidDeviceJson)
{
	Transport.Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Get, TEXT("/.well-known/openid-configuration"),
																	   EUnrealAIOAuthHttpContentType::Invalid,
																	   ValidDiscoveryJson));
	Transport.Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/device/code"),
																		EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		DeviceJson));
}

bool CreateTestEnvelope(const FDateTime &NowUtc, FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError)
{
	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	Binding.ProviderName = TEXT("xai.grok.oauth");
	Binding.AuthProfileId = TEXT("xai.grok.subscription");
	Binding.AccountId.Value = FGuid(71, 72, 73, 74);
	FUnrealAIOAuthTokenSet Tokens;
	if (!TryMakeSecret(TEXT("old-access"), Tokens.AccessToken, OutError) ||
					   !TryMakeSecret(TEXT("old-refresh"), Tokens.RefreshToken, OutError))
	{
		return false;
	}
	Tokens.AccessTokenExpiresAtUtc = NowUtc + FTimespan::FromMinutes(5);
	return FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), NowUtc, OutEnvelope, OutError);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthDeviceFlowTest,
								 "UnrealAI.Experimental.AuthXAI.DevicePendingSlowDownAndSuccess",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthDeviceFlowTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 100.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
	Waiter->ObservedTransport = &*Transport;
	EnqueueDiscoveryAndDevice(*Transport);
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post,
									TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
										 TEXT("{\"error\":\"authorization_pending\"}"), 400));
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
																		 EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		 TEXT("{\"error\":\"slow_down\"}"), 400));
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
																		 EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		 ValidTokenJson));
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FUnrealAICancellationSource Cancellation;
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("The deterministic xAI device flow succeeds"),
				  Driver.Authorize(60.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestFalse(TEXT("A successful xAI authorization has no public error"), Error.IsError());
	TestTrue(TEXT("The access token remains move-only and set"), Tokens.AccessToken.IsSet());
	TestTrue(TEXT("The initial authorization requires a refresh token"), Tokens.RefreshToken.IsSet());
	TestTrue(TEXT("The token expiry comes from the bounded provider lifetime"),
				  Tokens.AccessTokenExpiresAtUtc == Clock->UtcNow() + FTimespan::FromHours(1));
	TestEqual(TEXT("The trusted device instruction publishes once"), Interaction.Calls, 1);
	TestEqual(TEXT("The trusted device instruction uses the compiled verification page"), Interaction.LaunchUri,
				   FString(TEXT("https://accounts.x.ai/oauth2/device")));
	TestEqual(TEXT("The trusted device instruction carries only the printable code"), Interaction.UserCode,
				   FString(TEXT("ABCD-EFGH")));
	TestEqual(TEXT("Every token poll is preceded by a deterministic wait"), Waiter->WaitSeconds.Num(), 3);
	if (Waiter->WaitSeconds.Num() == 3)
	{
		TestEqual(TEXT("The first token poll honors the initial interval"), Waiter->WaitSeconds[0], 1.0);
		TestEqual(TEXT("Pending retains the initial interval"), Waiter->WaitSeconds[1], 1.0);
		TestEqual(TEXT("RFC 8628 slow_down adds five seconds"), Waiter->WaitSeconds[2], 6.0);
	}
	TestEqual(TEXT("Every wait records the requests admitted before it"), Waiter->RequestCountsAtWait.Num(), 3);
	if (Waiter->RequestCountsAtWait.Num() == 3)
	{
		TestEqual(TEXT("No token poll precedes the first interval wait"), Waiter->RequestCountsAtWait[0], 2);
		TestEqual(TEXT("The pending result precedes the second wait"), Waiter->RequestCountsAtWait[1], 3);
		TestEqual(TEXT("The slow-down result precedes the third wait"), Waiter->RequestCountsAtWait[2], 4);
	}
	TestTrue(TEXT("Every scripted exchange was consumed"), Transport->DidConsumeScript());
	TestTrue(TEXT("Every exchange stayed on the exact xAI origin, method, path, and content type"),
				  Transport->DidAllRequestsMatch());
	TestEqual(TEXT("OIDC discovery is a bodyless GET"), Transport->GetCapturedBody(0), FString());
	TestEqual(
		TEXT("The device request contains exactly the reviewed public client and encoded scope set"),
			 Transport->GetCapturedBody(1),
			 FString(TEXT("client_id=b1a00492-073a-47ea-816f-4c329264a828&scope=openid%20profile%20email%20offline_")
						 TEXT("access%20grok-cli%3Aaccess%20api%3Aaccess")));
	TestEqual(TEXT("The token poll contains exactly the RFC device grant, public client, and device identifier"),
				   Transport->GetCapturedBody(2),
				   FString(TEXT("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=")
							   TEXT("b1a00492-073a-47ea-816f-4c329264a828&device_code=fixture-device")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthFractionalClockExpiryTest,
								 "UnrealAI.Experimental.AuthXAI.FractionalClockExpiryCrossesStorageBoundary",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthFractionalClockExpiryTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 34, 56, 789), 100.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
	EnqueueDiscoveryAndDevice(*Transport);
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
																		 EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		 ValidTokenJson));

	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FUnrealAICancellationSource Cancellation;
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("xAI authorization succeeds when the system clock contains fractional seconds"),
				  Driver.Authorize(60.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestEqual(TEXT("Provider-relative expiry is normalized to the token envelope whole-second invariant"),
				   Tokens.AccessTokenExpiresAtUtc.GetTicks() % ETimespan::TicksPerSecond, static_cast<int64>(0));
	TestEqual(TEXT("Expiry preserves the provider lifetime after normalization"), Tokens.AccessTokenExpiresAtUtc,
				   FDateTime::FromUnixTimestamp(Clock->UtcNow().ToUnixTimestamp() + 3600));

	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	Binding.ProviderName = TEXT("xai.grok.oauth");
	Binding.AuthProfileId = TEXT("xai.grok.subscription");
	Binding.AccountId.Value = FGuid(75, 76, 77, 78);
	FUnrealAIOAuthTokenEnvelope Envelope;
	FString EnvelopeError;
	TestTrue(TEXT("The normalized live-style token set crosses the secure storage envelope boundary"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), Clock->UtcNow(), Envelope,
															  EnvelopeError));
	TestTrue(TEXT("The resulting envelope retains a renewable xAI session"), Envelope.HasRefreshToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthDiscoveryPinningTest,
								 "UnrealAI.Experimental.AuthXAI.DiscoveryFailsClosedOffOrigin",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthDiscoveryPinningTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	Transport->Enqueue(MakeExchange(
		EUnrealAIOAuthHttpMethod::Get,
		TEXT("/.well-known/openid-configuration"), EUnrealAIOAuthHttpContentType::Invalid,
			 TEXT("{\"issuer\":\"https://auth.x.ai\",\"authorization_endpoint\":") TEXT("\"https://auth.x.ai/oauth2/authorize\",\"token_endpoint\":\"https://attacker.example/token\"}")));
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock);
	FUnrealAICancellationSource Cancellation;
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("An off-origin discovered token endpoint fails before any credential-bearing POST"),
				   Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestEqual(TEXT("Discovery drift has the closed auth failure code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthFailed);
	TestEqual(TEXT("Only the bodyless discovery request reached the fake transport"), Transport->GetRequestCount(), 1);
	TestEqual(TEXT("No interaction is published after poisoned discovery"), Interaction.Calls, 0);
	TestFalse(TEXT("No token escapes a failed discovery"), Tokens.AccessToken.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthStrictDiscoveryTest,
								 "UnrealAI.Experimental.AuthXAI.DiscoveryRequiresExactFieldsAndUnambiguousKeys",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthStrictDiscoveryTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	struct FDiscoveryFailureRow final
	{
		const TCHAR *Label;
		const TCHAR *Json;
	};
	const FDiscoveryFailureRow Rows[] = {{TEXT("missing device endpoint"),
		TEXT("{\"issuer\":\"https://auth.x.ai\",\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",")
				 TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\"}")},
		{TEXT("wrong issuer"),
			TEXT("{\"issuer\":\"https://attacker.example\",\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",") TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",") TEXT("\"device_authorization_endpoint\":\"https://auth.x.ai/oauth2/device/code\"}")},
			{TEXT("wrong authorization endpoint"),
				TEXT("{\"issuer\":\"https://auth.x.ai\",\"authorization_endpoint\":\"https://attacker.example/authorize\",") TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",") TEXT("\"device_authorization_endpoint\":\"https://auth.x.ai/oauth2/device/code\"}")},
				{TEXT("wrong device endpoint"),
					TEXT("{\"issuer\":\"https://auth.x.ai\",\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",") TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",") TEXT("\"device_authorization_endpoint\":\"https://attacker.example/device\"}")},
					{TEXT("mixed-case issuer"),
						  TEXT("{\"Issuer\":\"https://auth.x.ai\",\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",")
							  TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",") TEXT("\"device_authorization_endpoint\":\"https://auth.x.ai/oauth2/device/code\"}")},
						  {TEXT("duplicate issuer"),
								TEXT("{\"issuer\":\"https://auth.x.ai\",\"issuer\":\"https://auth.x.ai\",") TEXT("\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",")
									TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",") TEXT("\"device_authorization_endpoint\":\"https://auth.x.ai/oauth2/device/code\"}")},
								{TEXT("case-colliding issuer"),
									  TEXT("{\"issuer\":\"https://auth.x.ai\",\"Issuer\":\"https://attacker.example\",")
										  TEXT("\"authorization_endpoint\":\"https://auth.x.ai/oauth2/authorize\",")
											  TEXT("\"token_endpoint\":\"https://auth.x.ai/oauth2/token\",")
												  TEXT("\"device_authorization_endpoint\":\"https://auth.x.ai/oauth2/device/code\"}")}};

	for (const FDiscoveryFailureRow &Row : Rows)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Get, TEXT("/.well-known/openid-configuration"),
																			EUnrealAIOAuthHttpContentType::Invalid,
																			Row.Json));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		const FString Context = FString::Printf(TEXT("Discovery row '%s'"), Row.Label);
		TestFalse(Context + TEXT(" fails closed"),
								 Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(Context + TEXT(" uses the closed auth failure"), Error.Code,
								 EUnrealAIProviderAccessErrorCode::AuthFailed);
		TestEqual(Context + TEXT(" sends only bodyless discovery"), Transport->GetRequestCount(), 1);
		TestEqual(Context + TEXT(" publishes no interaction"), Interaction.Calls, 0);
		TestFalse(Context + TEXT(" publishes no credential"), Tokens.AccessToken.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthStrictDeviceResponseTest,
								 "UnrealAI.Experimental.AuthXAI.DeviceResponseRequiresExactVerificationUri",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthStrictDeviceResponseTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
		EnqueueDiscoveryAndDevice(*Transport, TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
												  TEXT("\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",")
													  TEXT("\"expires_in\":1800,\"interval\":1}"));
		Transport->Enqueue(
			MakeExchange(EUnrealAIOAuthHttpMethod::Post,
						 TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded, ValidTokenJson));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("RFC 8628 permits an omitted verification_uri_complete"),
					  Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("The optional complete URI still publishes the compiled base once"), Interaction.Calls, 1);
		TestTrue(TEXT("The optional complete URI path returns protected tokens"), Tokens.AccessToken.IsSet());
	}

	struct FDeviceFailureRow final
	{
		const TCHAR *Label;
		const TCHAR *Json;
	};
	const FDeviceFailureRow Rows[] = {{TEXT("wrong verification origin"),
		TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
			TEXT("\"verification_uri\":\"https://attacker.example/oauth2/device\",")
				TEXT("\"verification_uri_complete\":\"https://attacker.example/oauth2/device?user_code=ABCD-EFGH\",")
						 TEXT("\"expires_in\":1800,\"interval\":1}")},
		{TEXT("mismatched complete code"),
			TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
				TEXT("\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",")
					TEXT("\"verification_uri_complete\":\"https://accounts.x.ai/oauth2/device?user_code=WXYZ-1234\",")
							 TEXT("\"expires_in\":1800,\"interval\":1}")},
			{TEXT("extra complete query"),
				  TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",") TEXT("\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",")
					  TEXT("\"verification_uri_complete\":") TEXT("\"https://accounts.x.ai/oauth2/device?user_code=ABCD-EFGH&next=https://attacker.example\",")
							  TEXT("\"expires_in\":1800,\"interval\":1}")},
				  {TEXT("fragmented complete URI"),
						TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
							TEXT("\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",")
								TEXT("\"verification_uri_complete\":")
									TEXT("\"https://accounts.x.ai/oauth2/device?user_code=ABCD-EFGH#fragment\",")
											 TEXT("\"expires_in\":1800,\"interval\":1}")},
						{TEXT("mixed-case verification field"),
							  TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
								  TEXT("\"Verification_Uri\":\"https://accounts.x.ai/oauth2/device\",")
									  TEXT("\"verification_uri_complete\":")
										  TEXT("\"https://accounts.x.ai/oauth2/device?user_code=ABCD-EFGH\",")
												   TEXT("\"expires_in\":1800,\"interval\":1}")},
							  {TEXT("case-colliding device code"),
									TEXT("{\"device_code\":\"fixture-device\",\"Device_Code\":\"attacker-device\",")
										TEXT("\"user_code\":\"ABCD-EFGH\",\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",") TEXT("\"verification_uri_complete\":")
											TEXT("\"https://accounts.x.ai/oauth2/device?user_code=ABCD-EFGH\",")
												TEXT("\"expires_in\":1800,\"interval\":1}")}};

	for (const FDeviceFailureRow &Row : Rows)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		EnqueueDiscoveryAndDevice(*Transport, Row.Json);
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		const FString Context = FString::Printf(TEXT("Device row '%s'"), Row.Label);
		TestFalse(Context + TEXT(" fails closed"),
								 Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(Context + TEXT(" consumes discovery and device only"), Transport->GetRequestCount(), 2);
		TestEqual(Context + TEXT(" publishes no interaction"), Interaction.Calls, 0);
		TestFalse(Context + TEXT(" publishes no credential"), Tokens.AccessToken.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthStrictTokenKeysTest,
								 "UnrealAI.Experimental.AuthXAI.TokenAndErrorKeysAreUnambiguous",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthStrictTokenKeysTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	struct FTokenFailureRow final
	{
		const TCHAR *Label;
		const TCHAR *Json;
		int32 StatusCode;
		EUnrealAIProviderAccessErrorCode ExpectedCode;
	};
	const FTokenFailureRow Rows[] = {
		{TEXT("mixed-case access token"),
			  TEXT("{\"Access_Token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\",\"expires_in\":3600}"),
				   200, EUnrealAIProviderAccessErrorCode::AuthResponseInvalid},
		 {TEXT("duplicate access token"),
			   TEXT("{\"access_token\":\"fixture-access\",\"access_token\":\"attacker-access\",")
						TEXT("\"refresh_token\":\"fixture-refresh\",\"expires_in\":3600}"), 200,
							 EUnrealAIProviderAccessErrorCode::AuthResponseInvalid},
			   {TEXT("case-colliding access token"),
					 TEXT("{\"access_token\":\"fixture-access\",\"Access_Token\":\"attacker-access\",")
							  TEXT("\"refresh_token\":\"fixture-refresh\",\"expires_in\":3600}"), 200,
								   EUnrealAIProviderAccessErrorCode::AuthResponseInvalid},
					 {TEXT("mixed-case error"),
						   TEXT("{\"Error\":\"authorization_pending\"}"), 400,
								EUnrealAIProviderAccessErrorCode::AuthFailed},
					  {TEXT("duplicate error"),
							TEXT("{\"error\":\"authorization_pending\",\"error\":\"authorization_pending\"}"), 400,
								 EUnrealAIProviderAccessErrorCode::AuthFailed},
					   {TEXT("case-colliding error"),
							 TEXT("{\"error\":\"authorization_pending\",\"Error\":\"access_denied\"}"), 400,
								  EUnrealAIProviderAccessErrorCode::AuthFailed}};

	for (const FTokenFailureRow &Row : Rows)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
		EnqueueDiscoveryAndDevice(*Transport);
		Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post,
										TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded, Row.Json,
											 Row.StatusCode));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		const FString Context = FString::Printf(TEXT("Token row '%s'"), Row.Label);
		TestFalse(Context + TEXT(" fails closed"),
								 Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(Context + TEXT(" has the expected safe typed auth failure"), Error.Code, Row.ExpectedCode);
		TestEqual(Context + TEXT(" performs exactly one poll"), Transport->GetRequestCount(), 3);
		TestFalse(Context + TEXT(" publishes no access token"), Tokens.AccessToken.IsSet());
		TestFalse(Context + TEXT(" publishes no refresh token"), Tokens.RefreshToken.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthScopeCompatibilityTest,
								 "UnrealAI.Experimental.AuthXAI.ProviderScopeSupersetAndOperationalSubsetAreCompatible",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthScopeCompatibilityTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TCHAR *CompatibleTokenRows[] = {
		TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\",")
			TEXT("\"token_type\":\"Bearer\",\"expires_in\":21600,")
				TEXT("\"scope\":\"openid profile email offline_access grok-cli:access api:access provider:new\"}"),
					 TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\",")
						 TEXT("\"expires_in\":21600,\"scope\":\"grok-cli:access api:access\"}")};

	for (const TCHAR *TokenJson : CompatibleTokenRows)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
		EnqueueDiscoveryAndDevice(*Transport);
		Transport->Enqueue(
			MakeExchange(EUnrealAIOAuthHttpMethod::Post,
						 TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded, TokenJson));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("A response granting both operational scopes is compatible"),
					  Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestTrue(TEXT("Compatible scope handling still requires and returns access material"),
					  Tokens.AccessToken.IsSet());
		TestTrue(TEXT("Compatible scope handling still requires and returns refresh material"),
					  Tokens.RefreshToken.IsSet());
		TestFalse(TEXT("Compatible scope handling emits no public error"), Error.IsError());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthTypedTokenFailureTest,
								 "UnrealAI.Experimental.AuthXAI.TokenValidationFailuresRemainStringFreeAndTyped",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthTypedTokenFailureTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	struct FValidationRow final
	{
		const TCHAR *Label;
		const TCHAR *Json;
		EUnrealAIProviderAccessErrorCode ExpectedCode;
		EUnrealAIErrorCategory ExpectedCategory;
	};
	const FValidationRow Rows[] = {
		{TEXT("malformed JSON"),
			  TEXT("{"), EUnrealAIProviderAccessErrorCode::AuthResponseInvalid, EUnrealAIErrorCategory::Provider},
		 {TEXT("missing refresh token"),
			   TEXT("{\"access_token\":\"fixture-access\",\"expires_in\":3600,"
			  "\"scope\":\"grok-cli:access api:access\"}"),
					EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete, EUnrealAIErrorCategory::Provider},
		  {TEXT("unsupported token type"),
				TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\","
			  "\"token_type\":\"MAC\",\"expires_in\":3600,\"scope\":\"grok-cli:access api:access\"}"),
					 EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported, EUnrealAIErrorCategory::Provider},
		   {TEXT("missing operational scope"),
				 TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\","
			  "\"expires_in\":3600,\"scope\":\"openid profile email offline_access grok-cli:access\"}"),
					  EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient, EUnrealAIErrorCategory::NotAuthorized},
			{TEXT("invalid expiry"), TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\","
			  "\"expires_in\":0,\"scope\":\"grok-cli:access api:access\"}"),
										  EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid,
										  EUnrealAIErrorCategory::Provider}};

	for (const FValidationRow &Row : Rows)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
		EnqueueDiscoveryAndDevice(*Transport);
		Transport->Enqueue(
			MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
															  EUnrealAIOAuthHttpContentType::FormUrlEncoded, Row.Json));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		const FString Context = FString::Printf(TEXT("Validation row '%s'"), Row.Label);
		TestFalse(Context + TEXT(" fails closed"),
								 Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(Context + TEXT(" preserves a safe machine code"), Error.Code, Row.ExpectedCode);
		TestEqual(Context + TEXT(" preserves the canonical safe category"), Error.Category, Row.ExpectedCategory);
		FString ShapeError;
		TestTrue(Context + TEXT(" emits a valid string-free error envelope"), Error.ValidateShape(ShapeError));
		TestFalse(Context + TEXT(" releases no access material"), Tokens.AccessToken.IsSet());
		TestFalse(Context + TEXT(" releases no refresh material"), Tokens.RefreshToken.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthRepeatedSlowDownTest,
								 "UnrealAI.Experimental.AuthXAI.RepeatedSlowDownAccumulatesFromInitialInterval",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthRepeatedSlowDownTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
	EnqueueDiscoveryAndDevice(*Transport);
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post,
									TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
										 TEXT("{\"error\":\"authorization_pending\"}"), 400));
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
																		 EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		 TEXT("{\"error\":\"slow_down\"}"), 400));
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
																		 EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		 TEXT("{\"error\":\"slow_down\"}"), 400));
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post, TEXT("/oauth2/token"),
																		 EUnrealAIOAuthHttpContentType::FormUrlEncoded,
																		 ValidTokenJson));
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FUnrealAICancellationSource Cancellation;
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("Repeated RFC 8628 slow_down responses remain recoverable"),
				  Driver.Authorize(60.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestEqual(TEXT("Four token requests have four preceding waits"), Waiter->WaitSeconds.Num(), 4);
	if (Waiter->WaitSeconds.Num() == 4)
	{
		TestEqual(TEXT("Initial wait uses the provider interval"), Waiter->WaitSeconds[0], 1.0);
		TestEqual(TEXT("Pending retains the provider interval"), Waiter->WaitSeconds[1], 1.0);
		TestEqual(TEXT("First slow_down adds five seconds"), Waiter->WaitSeconds[2], 6.0);
		TestEqual(TEXT("Second slow_down adds another five seconds"), Waiter->WaitSeconds[3], 11.0);
	}
	TestTrue(TEXT("Repeated slow_down consumes the exact script"), Transport->DidConsumeScript());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthDenialAndExpiryTest,
								 "UnrealAI.Experimental.AuthXAI.DenialAndExpiryAreDistinct",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthDenialAndExpiryTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
		EnqueueDiscoveryAndDevice(*Transport);
		Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post,
										TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
											 TEXT("{\"error\":\"access_denied\",\"error_description\":")
												 TEXT("\"fixture body must never become a diagnostic\"}"), 400));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("A provider access_denied terminal fails authorization"),
					   Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("User denial remains a typed not-authorized outcome"), Error.Category,
					   EUnrealAIErrorCategory::NotAuthorized);
		TestEqual(TEXT("User denial does not masquerade as timeout"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	}
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
		EnqueueDiscoveryAndDevice(*Transport);
		Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post,
										TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
											 TEXT("{\"error\":\"expired_token\"}"), 400));
		FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingXAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("An expired device grant fails authorization"),
					   Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("Device expiry remains the closed auth timeout"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthTimedOut);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthCancellationTest,
								 "UnrealAI.Experimental.AuthXAI.PendingCancellationStopsPolling",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthCancellationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
	EnqueueDiscoveryAndDevice(*Transport);
	FUnrealAICancellationSource Cancellation;
	Waiter->CancellationSource = &Cancellation;
	Waiter->CancelOnWaitNumber = 1;
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Cancellation during the initial poll wait terminates authorization"),
				   Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestEqual(TEXT("Initial-wait cancellation retains the closed auth cancellation"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthCancelled);
	TestEqual(TEXT("Cancellation prevents the first token poll"), Transport->GetRequestCount(), 2);
	TestTrue(TEXT("Only discovery and device authorization were consumed"), Transport->DidConsumeScript());
	TestFalse(TEXT("Cancellation publishes no token material"), Tokens.AccessToken.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthDeviceDeadlineTest,
								 "UnrealAI.Experimental.AuthXAI.DeviceDeadlineStopsPolling",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthDeviceDeadlineTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
	EnqueueDiscoveryAndDevice(*Transport,
							  TEXT("{\"device_code\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",")
								  TEXT("\"verification_uri\":\"https://accounts.x.ai/oauth2/device\",")
									  TEXT("\"verification_uri_complete\":")
										  TEXT("\"https://accounts.x.ai/oauth2/device?user_code=ABCD-EFGH\",")
											  TEXT("\"expires_in\":1,\"interval\":1}"));
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FUnrealAICancellationSource Cancellation;
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("The provider device-code lifetime bounds polling even when the caller deadline is longer"),
				   Driver.Authorize(60.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestEqual(TEXT("Device deadline exhaustion has the closed auth timeout"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthTimedOut);
	TestEqual(TEXT("Device deadline exhaustion prevents the first poll"), Transport->GetRequestCount(), 2);
	TestEqual(TEXT("The deadline path records one initial wait"), Waiter->WaitSeconds.Num(), 1);
	if (Waiter->WaitSeconds.Num() == 1)
	{
		TestEqual(TEXT("The fake waiter advanced exactly to the one-second device deadline"), Waiter->WaitSeconds[0],
					   1.0);
	}
	TestTrue(TEXT("Only discovery and device authorization were consumed"), Transport->DidConsumeScript());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthRefreshRotationTest,
								 "UnrealAI.Experimental.AuthXAI.RefreshRediscoversAndRotatesAtomically",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthRefreshRotationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 50.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Get, TEXT("/.well-known/openid-configuration"),
																		EUnrealAIOAuthHttpContentType::Invalid,
																		ValidDiscoveryJson));
	Transport->Enqueue(
		MakeExchange(EUnrealAIOAuthHttpMethod::Post,
					 TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
						  TEXT("{\"access_token\":\"rotated-access\",\"refresh_token\":\"rotated-refresh\",")
							  TEXT("\"token_type\":\"Bearer\",\"expires_in\":7200}")));
	FUnrealAIOAuthTokenEnvelope Envelope;
	FString SetupError;
	if (!TestTrue(TEXT("The starting xAI token envelope constructs"),
					   CreateTestEnvelope(Clock->UtcNow(), Envelope, SetupError)))
	{
		return false;
	}
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock);
	FUnrealAICancellationSource Cancellation;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("A bounded xAI refresh succeeds"), Driver.Refresh(Envelope, 30.0, Cancellation.GetToken(), Error));
	TestTrue(TEXT("Refresh re-runs bodyless exact-issuer discovery"), Transport->GetCapturedBody(0).IsEmpty());
	TestEqual(TEXT("The refresh grant contains exactly the prior credential and reviewed public client"),
				   Transport->GetCapturedBody(1),
				   FString(TEXT("grant_type=refresh_token&refresh_token=old-refresh&client_id=")
							   TEXT("b1a00492-073a-47ea-816f-4c329264a828")));
	TestTrue(TEXT("The refreshed access expiry is applied"),
				  Envelope.GetAccessTokenExpiresAtUtc() == Clock->UtcNow() + FTimespan::FromHours(2));

	FUnrealAISecretValue RotatedForm;
	TestTrue(TEXT("The rotated envelope can mint another refresh request"),
				  Envelope.TryMintRefreshRequestBody(TEXT("fixture-client"), RotatedForm, SetupError));
	FUnrealAIOAuthHttpSecretPayload Payload;
	TestTrue(TEXT("The rotated refresh request remains move-only"),
				  FUnrealAIOAuthHttpSecretPayload::TryCreate(MoveTemp(RotatedForm), Payload, SetupError));
	FCapturingSecretConsumer Consumer;
	TestTrue(TEXT("The rotated refresh request can be consumed once"), Payload.TryConsume(Consumer));
	TestTrue(TEXT("The refresh token rotated atomically"),
				  Consumer.Value.Contains(TEXT("refresh_token=rotated-refresh")));
	TestFalse(TEXT("The stale refresh token is absent after rotation"),
				   Consumer.Value.Contains(TEXT("refresh_token=old-refresh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthRefreshEntitlementTest,
								 "UnrealAI.Experimental.AuthXAI.RefreshEntitlementDeniedPreservesEnvelope",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthRefreshEntitlementTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 50.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Get, TEXT("/.well-known/openid-configuration"),
																		EUnrealAIOAuthHttpContentType::Invalid,
																		ValidDiscoveryJson));
	Transport->Enqueue(MakeExchange(EUnrealAIOAuthHttpMethod::Post,
									TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
										 TEXT("{\"error\":\"permission_denied\",\"error_description\":") TEXT("\"fixture provider body must never cross the typed boundary\"}"), 403));
	FUnrealAIOAuthTokenEnvelope Envelope;
	FString SetupError;
	if (!TestTrue(TEXT("The entitlement fixture envelope constructs"),
					   CreateTestEnvelope(Clock->UtcNow(), Envelope, SetupError)))
	{
		return false;
	}
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock);
	FUnrealAICancellationSource Cancellation;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("A token-endpoint entitlement denial cannot refresh the envelope"),
				   Driver.Refresh(Envelope, 30.0, Cancellation.GetToken(), Error));
	TestEqual(TEXT("Token-endpoint HTTP 403 has the distinct closed entitlement code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied);
	TestEqual(TEXT("Entitlement denial is a non-retryable policy outcome"), Error.Category,
				   EUnrealAIErrorCategory::PolicyDenied);
	TestFalse(TEXT("Entitlement denial is never retried automatically"), Error.bRetryable);
	TestTrue(TEXT("The typed entitlement error remains structurally valid"), Error.ValidateShape(SetupError));

	FUnrealAISecretValue PreservedForm;
	TestTrue(TEXT("The previous refresh generation remains available after entitlement denial"),
				  Envelope.TryMintRefreshRequestBody(TEXT("fixture-client"), PreservedForm, SetupError));
	FUnrealAIOAuthHttpSecretPayload Payload;
	TestTrue(TEXT("The preserved refresh request remains move-only"),
				  FUnrealAIOAuthHttpSecretPayload::TryCreate(MoveTemp(PreservedForm), Payload, SetupError));
	FCapturingSecretConsumer Consumer;
	TestTrue(TEXT("The preserved refresh request can be consumed once"), Payload.TryConsume(Consumer));
	TestTrue(TEXT("The prior refresh token was not overwritten by the denied response"),
				  Consumer.Value.Contains(TEXT("refresh_token=old-refresh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIXAIOAuthMalformedTokenTest,
								 "UnrealAI.Experimental.AuthXAI.MalformedScopeAndExpiryFailClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIXAIOAuthMalformedTokenTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
	const TSharedRef<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedXAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FAdvancingXAIPollWaiter, ESPMode::ThreadSafe>(Clock);
	EnqueueDiscoveryAndDevice(*Transport);
	Transport->Enqueue(
		MakeExchange(EUnrealAIOAuthHttpMethod::Post,
					 TEXT("/oauth2/token"), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
						  TEXT("{\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\",")
							  TEXT("\"expires_in\":0,\"scope\":\"openid profile\"}")));
	FUnrealAIXAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FUnrealAICancellationSource Cancellation;
	FCapturingXAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Lost scopes and invalid expiry cannot produce a ready session"),
				   Driver.Authorize(30.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestEqual(TEXT("Insufficient operational scopes have the closed authorization failure"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient);
	TestFalse(TEXT("Malformed token material is wiped instead of returned"), Tokens.AccessToken.IsSet());
	return true;
}

#endif
