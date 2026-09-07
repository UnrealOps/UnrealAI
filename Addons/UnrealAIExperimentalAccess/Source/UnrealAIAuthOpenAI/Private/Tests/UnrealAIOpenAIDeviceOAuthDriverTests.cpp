// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "HAL/Event.h"
#include "Misc/Base64.h"
#include "OpenAI/UnrealAIOpenAIDeviceOAuthDriver.h"
#include "Testing/UnrealAITestClock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
FString OpenAIOrigin()
{
	return TEXT("https://auth.") + FString(TEXT("openai.com"));
}

FString DeviceRoot()
{
	return TEXT("/api/accounts/") + FString(TEXT("deviceauth/"));
}

FString TestDeviceRequestPath()
{
	return DeviceRoot() + TEXT("usercode");
}

FString TestDevicePollPath()
{
	return DeviceRoot() + TEXT("token");
}

FString TestTokenPath()
{
	return TEXT("/oauth/") + FString(TEXT("token"));
}

FString VerificationPage()
{
	return OpenAIOrigin() + TEXT("/codex/device");
}

FString RoutingClaimName()
{
	return TEXT("https://api.") + FString(TEXT("openai.com/auth"));
}

void WipeString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

bool TryMakeSecret(const TConstArrayView<uint8> Bytes, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	TArray<uint8> Owned(Bytes);
	return FUnrealAISecretValue::TryCreate(MoveTemp(Owned), OutSecret, OutError);
}

bool TryMakeSecret(const FStringView Value, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	const FString Owned(Value);
	const FTCHARToUTF8 Utf8(*Owned);
	return TryMakeSecret(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), OutSecret,
						 OutError);
}

TArray<uint8> Utf8Bytes(const FStringView Value)
{
	const FString Owned(Value);
	const FTCHARToUTF8 Utf8(*Owned);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

FString Base64Url(const FString &Value)
{
	const FTCHARToUTF8 Utf8(*Value);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	FString Encoded = FBase64::Encode(Bytes, EBase64Mode::UrlSafe);
	Encoded.ReplaceInline(TEXT("="), TEXT(""));
	return Encoded;
}

FString MakeJwtFromClaims(const FString &Claims)
{
	return TEXT("e30.") + Base64Url(Claims) + TEXT(".fixture");
}

FString MakeAccessJwt(const FDateTime &NowUtc, const int64 OffsetSeconds = 3600,
					  const FString &Routing = TEXT("acct-fixture"))
{
	const int64 Expiry = NowUtc.ToUnixTimestamp() + OffsetSeconds;
	const FString Claims = FString::Printf(TEXT("{\"exp\":%lld,\"%s\":{\"chatgpt_account_id\":\"%s\"}}"),
												static_cast<long long>(Expiry), *RoutingClaimName(), *Routing);
	return MakeJwtFromClaims(Claims);
}

FString MakeTokenJson(const FString &AccessToken, const bool bRefresh = true, const bool bId = true,
					  const FString &RoutingAgnosticRefresh = TEXT("fixture-refresh"))
{
	FString Json = FString::Printf(TEXT("{\"access_token\":\"%s\""), *AccessToken);
	if (bRefresh)
	{
		Json += FString::Printf(TEXT(",\"refresh_token\":\"%s\""), *RoutingAgnosticRefresh);
	}
	if (bId)
	{
		Json += TEXT(",\"id_token\":\"fixture-id\"");
	}
	Json += TEXT("}");
	return Json;
}

FString ValidDeviceJson(const bool bUseAlias = false, const TCHAR *Interval = TEXT("1"))
{
	if (bUseAlias)
	{
		return FString::Printf(
			TEXT("{\"device_auth_id\":\"fixture-device\",\"usercode\":\"ABCD-EFGH\",\"interval\":\"%s\"}"), Interval);
	}
	return FString::Printf(
		TEXT("{\"device_auth_id\":\"fixture-device\",\"user_code\":\"ABCD-EFGH\",\"interval\":\"%s\"}"), Interval);
}

FString ValidApprovalJson()
{
	return TEXT("{\"authorization_code\":\"fixture-code\",\"code_challenge\":\"fixture-challenge\",")
		TEXT("\"code_verifier\":\"fixture-verifier\"}");
}

struct FOpenAIFixtureExchange final
{
	FString RelativePath;
	EUnrealAIOAuthHttpContentType ContentType = EUnrealAIOAuthHttpContentType::Invalid;
	int32 StatusCode = 200;
	TArray<uint8> Body;
	float RetryAfterSeconds = 0.0f;
	bool bDeferred = false;
	bool bNeverComplete = false;
	bool bWrongHandleId = false;
	bool bWrongResultId = false;
	bool bInvalidResultShape = false;
	bool bDuplicateTerminal = false;
	int32 DuplicateStatusCode = 500;
};

FOpenAIFixtureExchange MakeExchange(const FString &Path, const EUnrealAIOAuthHttpContentType ContentType,
									const FStringView Body, const int32 StatusCode = 200)
{
	FOpenAIFixtureExchange Exchange;
	Exchange.RelativePath = Path;
	Exchange.ContentType = ContentType;
	Exchange.StatusCode = StatusCode;
	Exchange.Body = Utf8Bytes(Body);
	return Exchange;
}

class FOpenAITestRequestHandle final : public IUnrealAIOAuthHttpRequestHandle
{
  public:
	explicit FOpenAITestRequestHandle(const FUnrealAIRequestId &InRequestId) : RequestId(InRequestId) {}

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

class FCapturingBodyConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FString Value;

	~FCapturingBodyConsumer() override
	{
		WipeString(Value);
	}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Secret.GetData()), Secret.Num());
		if (Converted.Length() <= 0)
		{
			return false;
		}
		Value = FString(Converted.Length(), Converted.Get());
		return true;
	}
};

class FScriptedOpenAIOAuthTransport final : public IUnrealAIOAuthHttpTransport
{
  public:
	~FScriptedOpenAIOAuthTransport() override
	{
		for (FString &Body : CapturedBodies)
		{
			WipeString(Body);
		}
	}

	void Enqueue(FOpenAIFixtureExchange Exchange)
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
		if (bShutdown || Cancellation.IsCancellationRequested() || NextExchange >= Script.Num())
		{
			OutError = TEXT("Scripted OpenAI OAuth transport exhausted or unavailable.");
			return false;
		}
		const FOpenAIFixtureExchange &Expected = Script[NextExchange++];
		if (Request.GetEndpointOrigin().ToString() != OpenAIOrigin() ||
			Request.GetMethod() != EUnrealAIOAuthHttpMethod::Post ||
			Request.GetRelativePath() != Expected.RelativePath || Request.GetContentType() != Expected.ContentType)
		{
			bAllRequestsMatched = false;
		}
		FCapturingBodyConsumer BodyConsumer;
		if (!Request.TryConsumeBody(BodyConsumer))
		{
			OutError = TEXT("Scripted OpenAI OAuth transport could not consume the request body.");
			return false;
		}
		CapturedBodies.Add(BodyConsumer.Value);

		const FUnrealAIRequestId HandleId =
			Expected.bWrongHandleId ? FUnrealAIRequestId{FGuid::NewGuid()} : Request.GetRequestId();
		LastHandle = MakeShared<FOpenAITestRequestHandle, ESPMode::ThreadSafe>(HandleId);
		OutHandle = LastHandle;
		if (Expected.bDeferred || Expected.bNeverComplete)
		{
			PendingSink = CompletionSink;
			PendingRequestId = Request.GetRequestId();
			PendingExchangeIndex = NextExchange - 1;
			return true;
		}
		return Deliver(Expected, Request.GetRequestId(), CompletionSink, OutError);
	}

	void BeginShutdown() override
	{
		bShutdown = true;
	}

	bool HasPending() const
	{
		return PendingSink.IsValid();
	}

	bool PendingCanComplete() const
	{
		return HasPending() && PendingExchangeIndex != INDEX_NONE && !Script[PendingExchangeIndex].bNeverComplete;
	}

	bool CompletePending()
	{
		if (!PendingCanComplete())
		{
			return false;
		}
		FString Error;
		const FOpenAIFixtureExchange &Exchange = Script[PendingExchangeIndex];
		const TSharedPtr<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> Sink = PendingSink;
		PendingSink.Reset();
		PendingExchangeIndex = INDEX_NONE;
		return Deliver(Exchange, PendingRequestId, Sink.ToSharedRef(), Error);
	}

	bool CompletePendingLate()
	{
		if (!HasPending())
		{
			return false;
		}
		FString Error;
		FOpenAIFixtureExchange Exchange = Script[PendingExchangeIndex];
		Exchange.bNeverComplete = false;
		const TSharedPtr<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> Sink = PendingSink;
		PendingSink.Reset();
		PendingExchangeIndex = INDEX_NONE;
		return Deliver(Exchange, PendingRequestId, Sink.ToSharedRef(), Error);
	}

	int32 GetRequestCount() const
	{
		return NextExchange;
	}

	int32 GetLastCancelCalls() const
	{
		return LastHandle.IsValid() ? LastHandle->CancelCalls : 0;
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

	int32 GetDeliveryCount() const
	{
		return DeliveryCount;
	}

  private:
	bool Deliver(const FOpenAIFixtureExchange &Exchange, const FUnrealAIRequestId &RequestId,
				 const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &Sink, FString &OutError)
	{
		const auto DeliverOne =
			[this, &Exchange, &RequestId, &Sink, &OutError](const int32 StatusCode, const bool bInvalidShape)
		{
			++DeliveryCount;
			if (bInvalidShape)
			{
				Sink->CompleteOAuthHttpRequest(FUnrealAIOAuthHttpResult());
				return true;
			}
			FUnrealAIOAuthHttpResponseMetadata Metadata;
			Metadata.StatusCode = StatusCode;
			Metadata.ContentType = TEXT("application/json");
			Metadata.ProviderRequestId = TEXT("fixture-request");
			Metadata.RetryAfterSeconds = Exchange.RetryAfterSeconds;
			FUnrealAISecretValue ResponseBody;
			if (!Exchange.Body.IsEmpty() && !TryMakeSecret(Exchange.Body, ResponseBody, OutError))
			{
				return false;
			}
			const FUnrealAIRequestId ResultId =
				Exchange.bWrongResultId ? FUnrealAIRequestId{FGuid::NewGuid()} : RequestId;
			FUnrealAIOAuthHttpResult Result;
			if (!FUnrealAIOAuthHttpResult::TryCreateResponse(ResultId, Metadata, MoveTemp(ResponseBody), Result,
															 OutError))
			{
				return false;
			}
			Sink->CompleteOAuthHttpRequest(MoveTemp(Result));
			return true;
		};
		if (!DeliverOne(Exchange.StatusCode, Exchange.bInvalidResultShape))
		{
			return false;
		}
		return !Exchange.bDuplicateTerminal || DeliverOne(Exchange.DuplicateStatusCode, false);
	}

	TArray<FOpenAIFixtureExchange> Script;
	TArray<FString> CapturedBodies;
	TSharedPtr<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> PendingSink;
	TSharedPtr<FOpenAITestRequestHandle, ESPMode::ThreadSafe> LastHandle;
	FUnrealAIRequestId PendingRequestId;
	int32 PendingExchangeIndex = INDEX_NONE;
	int32 NextExchange = 0;
	int32 DeliveryCount = 0;
	bool bAllRequestsMatched = true;
	bool bShutdown = false;
};

class FDeterministicOpenAIWaiter final : public IUnrealAIOpenAIOAuthWaiter
{
  public:
	FDeterministicOpenAIWaiter(const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> &InClock,
							   const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> &InTransport)
		: Clock(InClock), Transport(InTransport)
	{
	}

	bool WaitForExchange(FEvent &CompletionEvent, const double MaxWaitSeconds) override
	{
		++ExchangeWaitCalls;
		if (Transport->PendingCanComplete())
		{
			Transport->CompletePending();
		}
		if (CompletionEvent.Wait(0))
		{
			return true;
		}
		if (CancellationSource != nullptr && ExchangeWaitCalls == CancelOnExchangeWait)
		{
			CancellationSource->Cancel(EUnrealAICancellationReason::Requested);
		}
		Clock->Advance(FTimespan::FromSeconds(MaxWaitSeconds));
		return CompletionEvent.Wait(0);
	}

	bool WaitForPoll(const double Seconds, const double OverallDeadlineSeconds, const IUnrealAIClock &InClock,
					 const FUnrealAICancellationToken &Cancellation) override
	{
		(void)InClock;
		PollWaitSeconds.Add(Seconds);
		if (CancellationSource != nullptr && PollWaitSeconds.Num() == CancelOnPollWait)
		{
			CancellationSource->Cancel(EUnrealAICancellationReason::Requested);
		}
		Clock->Advance(FTimespan::FromSeconds(Seconds));
		return !Cancellation.IsCancellationRequested() && Clock->MonotonicSeconds() < OverallDeadlineSeconds;
	}

	TArray<double> PollWaitSeconds;
	const FUnrealAICancellationSource *CancellationSource = nullptr;
	int32 CancelOnExchangeWait = MAX_int32;
	int32 CancelOnPollWait = MAX_int32;
	int32 ExchangeWaitCalls = 0;

  private:
	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport;
};

class FCapturingOpenAIInteractionPublisher final : public IUnrealAIDeviceOAuthInteractionPublisher
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

void EnqueueSuccessfulApproval(FScriptedOpenAIOAuthTransport &Transport, const FString &DeviceJson,
							   const FString &ApprovalJson)
{
	Transport.Enqueue(
		MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, DeviceJson));
	Transport.Enqueue(MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ApprovalJson));
}

void EnqueueSuccessfulFlow(FScriptedOpenAIOAuthTransport &Transport, const FDateTime &NowUtc,
						   const FString &DeviceJson = ValidDeviceJson(),
						   const FString &ApprovalJson = ValidApprovalJson())
{
	EnqueueSuccessfulApproval(Transport, DeviceJson, ApprovalJson);
	const FString TokenJson = MakeTokenJson(MakeAccessJwt(NowUtc));
	Transport.Enqueue(MakeExchange(TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded, TokenJson));
}

bool ContainsOneNonEmptyClientField(const FString &Body)
{
	int32 Count = 0;
	int32 SearchFrom = 0;
	while ((SearchFrom = Body.Find(TEXT("client_id="), ESearchCase::CaseSensitive, ESearchDir::FromStart,
										SearchFrom)) != INDEX_NONE)
	{
		++Count;
		SearchFrom += 10;
	}
	return Count == 1 && !Body.Contains(TEXT("client_id=&"));
}

bool DeviceJsonHasOneNonEmptyClient(const FString &Body)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	FString ClientId;
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid() && Object->Values.Num() == 1 &&
		   Object->TryGetStringField(TEXT("client_id"), ClientId) && !ClientId.IsEmpty();
}

bool CreateEnvelope(const FDateTime &NowUtc, const FString &Routing, FUnrealAIOAuthTokenEnvelope &OutEnvelope)
{
	FString Error;
	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	Binding.ProviderName = TEXT("openai.codex.oauth");
	Binding.AuthProfileId = TEXT("openai.codex.subscription");
	Binding.AccountId.Value = FGuid(81, 82, 83, 84);
	FUnrealAIOAuthTokenSet Tokens;
	FString Access = MakeAccessJwt(NowUtc, 3600, Routing);
	if (!TryMakeSecret(Access, Tokens.AccessToken, Error) ||
		!TryMakeSecret(TEXT("old-refresh"), Tokens.RefreshToken, Error) ||
					   !TryMakeSecret(TEXT("old-id"), Tokens.IdToken, Error) ||
									  !TryMakeSecret(Routing, Tokens.AccountRoutingValue, Error))
	{
		return false;
	}
	Tokens.AccessTokenExpiresAtUtc = NowUtc + FTimespan::FromHours(1);
	return FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), NowUtc, OutEnvelope, Error);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOpenAIDeviceContractTest,
								 "UnrealAI.Experimental.AuthOpenAI.ExactDeviceContractAndPendingStatuses",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOpenAIDeviceContractTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 100.0);
	const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
		MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
	const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
		MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
	Transport->Enqueue(MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson,
									ValidDeviceJson(true, TEXT("0"))));
	Transport->Enqueue(
		MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, TEXT("{}"), 403));
	Transport->Enqueue(
		MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, TEXT("{}"), 404));
	Transport->Enqueue(
		MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidApprovalJson()));
	Transport->Enqueue(MakeExchange(TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
									MakeTokenJson(MakeAccessJwt(Clock->UtcNow()))));

	FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
	FUnrealAICancellationSource Cancellation;
	FCapturingOpenAIInteractionPublisher Interaction;
	FUnrealAIOAuthTokenSet Tokens;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("The exact reviewed OpenAI device flow succeeds"),
				  Driver.Authorize(60.0, Cancellation.GetToken(), Interaction, Tokens, Error));
	TestFalse(TEXT("Success has no public error"), Error.IsError());
	TestEqual(TEXT("The pinned behavior is compatibility revision 2"),
				   FUnrealAIOpenAIDeviceOAuthDriver::GetCompatibilityRevision(), 2);
	TestTrue(TEXT("Initial authorization requires and returns access material"), Tokens.AccessToken.IsSet());
	TestTrue(TEXT("Initial authorization requires and returns refresh material"), Tokens.RefreshToken.IsSet());
	TestTrue(TEXT("Initial authorization requires and returns ID material"), Tokens.IdToken.IsSet());
	TestTrue(TEXT("The access JWT contributes protected account routing"), Tokens.AccountRoutingValue.IsSet());
	TestEqual(TEXT("The trusted interaction publishes exactly once"), Interaction.Calls, 1);
	TestEqual(TEXT("The trusted interaction uses the fixed verification page"), Interaction.LaunchUri,
				   VerificationPage());
	TestEqual(TEXT("The trusted interaction carries the accepted upstream alias"), Interaction.UserCode,
				   FString(TEXT("ABCD-EFGH")));
	TestEqual(TEXT("Only 403 and 404 caused pending waits"), Waiter->PollWaitSeconds.Num(), 2);
	TestEqual(TEXT("An upstream zero interval is normalized to one second"), Waiter->PollWaitSeconds[0], 1.0);
	TestEqual(TEXT("The normalized interval remains stable"), Waiter->PollWaitSeconds[1], 1.0);
	TestTrue(TEXT("All exact method, origin, path, and content-type constraints matched"),
				  Transport->DidAllRequestsMatch());
	TestTrue(TEXT("The complete fixture script was consumed"), Transport->DidConsumeScript());
	TestTrue(TEXT("The device request owns exactly one nonempty compiled client field"),
				  DeviceJsonHasOneNonEmptyClient(Transport->GetCapturedBody(0)));
	TestTrue(TEXT("The token exchange owns exactly one nonempty compiled client field"),
				  ContainsOneNonEmptyClientField(Transport->GetCapturedBody(4)));
	TestTrue(TEXT("The token exchange carries the reviewed verifier"),
				  Transport->GetCapturedBody(4).Contains(TEXT("code_verifier=fixture-verifier")));
	TestFalse(TEXT("The challenge is validated but is not added to the upstream token form"),
				   Transport->GetCapturedBody(4).Contains(TEXT("code_challenge")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOpenAIStrictResponseTest,
								 "UnrealAI.Experimental.AuthOpenAI.StrictUtf8JsonAndApprovalTriple",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOpenAIStrictResponseTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	TArray<TArray<uint8>> InvalidDeviceBodies;
	InvalidDeviceBodies.Add(
		Utf8Bytes(TEXT("{\"device_auth_id\":\"a\",\"device_auth_id\":\"b\",\"user_code\":\"C\",\"interval\":\"1\"}")));
	InvalidDeviceBodies.Add(
		Utf8Bytes(TEXT("{\"device_auth_id\":\"a\",\"Device_Auth_Id\":\"b\",\"user_code\":\"C\",\"interval\":\"1\"}")));
	InvalidDeviceBodies.Add(
		Utf8Bytes(TEXT("{\"device_auth_id\":\"a\",\"user_code\":\"C\",\"usercode\":\"D\",\"interval\":\"1\"}")));
	InvalidDeviceBodies.Add(Utf8Bytes(TEXT("{\"device_auth_id\":\"a\",\"user_code\":\"C\",\"interval\":1}")));
	InvalidDeviceBodies.Add(Utf8Bytes(TEXT("{\"device_auth_id\":\"a\",\"user_code\":\"C\",\"interval\":\"1x\"}")));
	TArray<uint8> BomBody = Utf8Bytes(ValidDeviceJson());
	BomBody.Insert(0xbf, 0);
	BomBody.Insert(0xbb, 0);
	BomBody.Insert(0xef, 0);
	InvalidDeviceBodies.Add(MoveTemp(BomBody));
	TArray<uint8> InvalidUtf8 = Utf8Bytes(TEXT("{\"device_auth_id\":\""));
	InvalidUtf8.Add(0xc0);
	InvalidUtf8.Add(0xaf);
	InvalidUtf8.Append(Utf8Bytes(TEXT("\",\"user_code\":\"C\",\"interval\":\"1\"}")));
	InvalidDeviceBodies.Add(MoveTemp(InvalidUtf8));

	for (int32 Index = 0; Index < InvalidDeviceBodies.Num(); ++Index)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FOpenAIFixtureExchange Exchange;
		Exchange.RelativePath = TestDeviceRequestPath();
		Exchange.ContentType = EUnrealAIOAuthHttpContentType::ApplicationJson;
		Exchange.Body = InvalidDeviceBodies[Index];
		Transport->Enqueue(MoveTemp(Exchange));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(*FString::Printf(TEXT("Strict device response %d fails closed"), Index),
								   Driver.Authorize(10.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("Invalid device material cannot publish an interaction"), Interaction.Calls, 0);
		TestEqual(TEXT("Invalid device material stops after one exchange"), Transport->GetRequestCount(), 1);
		TestFalse(TEXT("Invalid device material releases no access token"), Tokens.AccessToken.IsSet());
	}

	const TArray<FString> InvalidApprovals = {TEXT("{\"authorization_code\":\"a\",\"authorization_code\":\"b\",") TEXT("\"code_challenge\":\"c\",\"code_verifier\":\"v\"}"),
		TEXT("{\"authorization_code\":\"a\",\"Authorization_Code\":\"b\",")
			TEXT("\"code_challenge\":\"c\",\"code_verifier\":\"v\"}"),
				 TEXT("{\"authorization_code\":\"a\",\"code_verifier\":\"v\"}"),
					  TEXT("{\"authorization_code\":\"a\",\"code_challenge\":\"\",\"code_verifier\":\"v\"}")};
	for (int32 Index = 0; Index < InvalidApprovals.Num(); ++Index)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		EnqueueSuccessfulApproval(*Transport, ValidDeviceJson(), InvalidApprovals[Index]);
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(*FString::Printf(TEXT("Strict approval response %d fails closed"), Index),
								   Driver.Authorize(10.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("Invalid approval stops before token exchange"), Transport->GetRequestCount(), 2);
		TestFalse(TEXT("Invalid approval releases no access token"), Tokens.AccessToken.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOpenAITerminalRaceTest,
								 "UnrealAI.Experimental.AuthOpenAI.CorrelationTimeoutCancellationAndTerminalOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOpenAITerminalRaceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const auto RunCorrelationFailure =
		[this](const bool bWrongHandle, const bool bWrongResult, const bool bInvalidShape, const TCHAR *Label)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FOpenAIFixtureExchange Exchange =
			MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidDeviceJson());
		Exchange.bWrongHandleId = bWrongHandle;
		Exchange.bWrongResultId = bWrongResult;
		Exchange.bInvalidResultShape = bInvalidShape;
		Transport->Enqueue(MoveTemp(Exchange));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(Label, Driver.Authorize(10.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("A correlation or result-shape violation cancels its handle once"),
					   Transport->GetLastCancelCalls(), 1);
		TestEqual(TEXT("A correlation violation publishes no interaction"), Interaction.Calls, 0);
		TestFalse(TEXT("A correlation violation publishes no token"), Tokens.AccessToken.IsSet());
	};
	RunCorrelationFailure(true, false, false, TEXT("A wrong handle ID fails closed"));
	RunCorrelationFailure(false, true, false, TEXT("A wrong result ID fails closed"));
	RunCorrelationFailure(false, false, true, TEXT("An invalid terminal shape fails closed"));

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FOpenAIFixtureExchange Device =
			MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidDeviceJson());
		Device.bDeferred = true;
		Device.bDuplicateTerminal = true;
		Device.DuplicateStatusCode = 500;
		Transport->Enqueue(MoveTemp(Device));
		Transport->Enqueue(
			MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidApprovalJson()));
		Transport->Enqueue(MakeExchange(TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
										MakeTokenJson(MakeAccessJwt(Clock->UtcNow()))));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("The first deferred terminal wins over a duplicate terminal"),
					  Driver.Authorize(10.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("The fake delivered both racing terminals"), Transport->GetDeliveryCount(), 4);
		TestEqual(TEXT("Terminal-once still publishes one interaction"), Interaction.Calls, 1);
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FOpenAIFixtureExchange Device =
			MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidDeviceJson());
		Device.bNeverComplete = true;
		Transport->Enqueue(MoveTemp(Device));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("An exchange obeys the injected monotonic deadline"),
					   Driver.Authorize(0.03, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("A deterministic exchange deadline has the auth timeout code"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthTimedOut);
		TestEqual(TEXT("Timeout cancels the outstanding handle once"), Transport->GetLastCancelCalls(), 1);
		TestTrue(TEXT("A late completion is safely absorbed after timeout"), Transport->CompletePendingLate());
		TestEqual(TEXT("A late completion cannot publish an interaction"), Interaction.Calls, 0);
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FOpenAIFixtureExchange Device =
			MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidDeviceJson());
		Device.bNeverComplete = true;
		Transport->Enqueue(MoveTemp(Device));
		FUnrealAICancellationSource Cancellation;
		Waiter->CancellationSource = &Cancellation;
		Waiter->CancelOnExchangeWait = 1;
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("Cancellation during a deferred exchange stops authorization"),
					   Driver.Authorize(10.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("Cancellation retains the closed auth cancellation code"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthCancelled);
		TestEqual(TEXT("Cancellation reaches the outstanding handle once"), Transport->GetLastCancelCalls(), 1);
		TestTrue(TEXT("A late completion is safely absorbed after cancellation"), Transport->CompletePendingLate());
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23), 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		Transport->Enqueue(MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson,
										ValidDeviceJson(false, TEXT("999"))));
		for (int32 Poll = 0; Poll < 30; ++Poll)
		{
			Transport->Enqueue(
				MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, TEXT("{}"), 403));
		}
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("The provider flow caps a longer caller timeout at fifteen minutes"),
					   Driver.Authorize(3600.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("The fifteen-minute cap terminates with an auth timeout"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthTimedOut);
		TestEqual(TEXT("The bounded flow performs only thirty clamped pending waits"), Waiter->PollWaitSeconds.Num(),
					   30);
		TestEqual(TEXT("The bounded flow makes no poll after its deadline"), Transport->GetRequestCount(), 31);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOpenAITokenFreshnessTest,
								 "UnrealAI.Experimental.AuthOpenAI.InitialTokensJwtRoutingAndFreshness",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOpenAITokenFreshnessTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	struct FTokenCase final
	{
		FString AccessToken;
		bool bRefresh = true;
		bool bId = true;
		FString RefreshValue = TEXT("fixture-refresh");
	};
	const FDateTime NowUtc(2026, 7, 23);
	const int64 MaxTimestamp = 253402300799ll;
	const FString DuplicateClaims =
		FString::Printf(TEXT("{\"exp\":%lld,\"Exp\":%lld,\"%s\":{\"chatgpt_account_id\":\"acct\"}}"),
							 static_cast<long long>(NowUtc.ToUnixTimestamp() + 3600),
							 static_cast<long long>(NowUtc.ToUnixTimestamp() + 7200), *RoutingClaimName());
	const FString ControlRoutingClaims =
		FString::Printf(TEXT("{\"exp\":%lld,\"%s\":{\"chatgpt_account_id\":\"acct\\u001f\"}}"),
							 static_cast<long long>(NowUtc.ToUnixTimestamp() + 3600), *RoutingClaimName());
	const TArray<FTokenCase> InvalidCases = {
		{MakeAccessJwt(NowUtc), false, true},
		{MakeAccessJwt(NowUtc), true, false},
		{MakeAccessJwt(NowUtc), true, true, TEXT("")},
		 {TEXT("only.two"), true, true},
		  {TEXT("e30.bad=padding.fixture"), true, true}, {MakeAccessJwt(NowUtc, 0), true, true},
		   {MakeJwtFromClaims(FString::Printf(TEXT("{\"exp\":%lld,\"%s\":{\"chatgpt_account_id\":\"acct\"}}"),
												   static_cast<long long>(MaxTimestamp + 1), *RoutingClaimName())),
							  true, true},
			{MakeJwtFromClaims(DuplicateClaims), true, true}, {MakeJwtFromClaims(ControlRoutingClaims), true, true}};

	for (int32 Index = 0; Index < InvalidCases.Num(); ++Index)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(NowUtc, 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		EnqueueSuccessfulApproval(*Transport, ValidDeviceJson(), ValidApprovalJson());
		Transport->Enqueue(MakeExchange(TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
										MakeTokenJson(InvalidCases[Index].AccessToken, InvalidCases[Index].bRefresh,
													  InvalidCases[Index].bId, InvalidCases[Index].RefreshValue)));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(*FString::Printf(TEXT("Strict initial token case %d fails closed"), Index),
								   Driver.Authorize(10.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestEqual(TEXT("A rejected token response reached exactly the initial three exchanges"),
					   Transport->GetRequestCount(), 3);
		TestFalse(TEXT("Rejected token material is reset"), Tokens.AccessToken.IsSet());
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(NowUtc, 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		Transport->Enqueue(
			MakeExchange(TestDeviceRequestPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, ValidDeviceJson()));
		FOpenAIFixtureExchange RateLimited =
			MakeExchange(TestDevicePollPath(), EUnrealAIOAuthHttpContentType::ApplicationJson, TEXT("{}"), 429);
		RateLimited.RetryAfterSeconds = 7.0f;
		Transport->Enqueue(MoveTemp(RateLimited));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FCapturingOpenAIInteractionPublisher Interaction;
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("HTTP 429 is terminal rather than an official pending outcome"),
					   Driver.Authorize(60.0, Cancellation.GetToken(), Interaction, Tokens, Error));
		TestTrue(TEXT("A terminal 429 remains retryable"), Error.bRetryable);
		TestEqual(TEXT("A terminal 429 preserves bounded retry guidance"), Error.RetryAfterSeconds, 7.0f);
		TestEqual(TEXT("A terminal 429 causes no poll wait"), Waiter->PollWaitSeconds.Num(), 0);
		TestEqual(TEXT("A terminal 429 causes no additional poll"), Transport->GetRequestCount(), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOpenAIRefreshTest,
								 "UnrealAI.Experimental.AuthOpenAI.RefreshRotationErrorsAndRoutingFreshness",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOpenAIRefreshTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FDateTime NowUtc(2026, 7, 23);
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(NowUtc, 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FUnrealAIOAuthTokenEnvelope Envelope;
		TestTrue(TEXT("The refresh fixture envelope is valid"), CreateEnvelope(NowUtc, TEXT("acct-refresh"), Envelope));
		Transport->Enqueue(
			MakeExchange(TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
						 MakeTokenJson(MakeAccessJwt(NowUtc, 7200, TEXT("acct-refresh")), false, false)));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("A refresh may omit unrotated refresh and ID tokens"),
					  Driver.Refresh(Envelope, 10.0, Cancellation.GetToken(), Error));
		TestTrue(TEXT("An omitted refresh token preserves the prior token"), Envelope.HasRefreshToken());
		TestTrue(TEXT("An omitted ID token preserves the prior token"), Envelope.HasIdToken());
		TestTrue(TEXT("Protected routing remains installed"), Envelope.HasAccountRoutingValue());
		TestTrue(TEXT("The refresh form contains one nonempty compiled client field"),
					  ContainsOneNonEmptyClientField(Transport->GetCapturedBody(0)));
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(NowUtc, 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FUnrealAIOAuthTokenEnvelope Envelope;
		TestTrue(TEXT("The changed-routing fixture envelope is valid"),
					  CreateEnvelope(NowUtc, TEXT("acct-original"), Envelope));
		Transport->Enqueue(MakeExchange(
			TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded,
			MakeTokenJson(MakeAccessJwt(NowUtc, 7200, TEXT("acct-other")), true, true, TEXT("new-refresh"))));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("A refresh cannot switch protected account routing"),
					   Driver.Refresh(Envelope, 10.0, Cancellation.GetToken(), Error));
		TestEqual(TEXT("A routing mismatch is a closed credential failure"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::CredentialFailed);
	}

	for (const int32 Status : {400, 401})
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(NowUtc, 10.0);
		const TSharedRef<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe> Transport =
			MakeShared<FScriptedOpenAIOAuthTransport, ESPMode::ThreadSafe>();
		const TSharedRef<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe> Waiter =
			MakeShared<FDeterministicOpenAIWaiter, ESPMode::ThreadSafe>(Clock, Transport);
		FUnrealAIOAuthTokenEnvelope Envelope;
		TestTrue(TEXT("The reauthorization fixture envelope is valid"),
					  CreateEnvelope(NowUtc, TEXT("acct-refresh"), Envelope));
		Transport->Enqueue(
			MakeExchange(TestTokenPath(), EUnrealAIOAuthHttpContentType::FormUrlEncoded, TEXT("{}"), Status));
		FUnrealAIOpenAIDeviceOAuthDriver Driver(Transport, Clock, Waiter);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("Provider refresh rejection requires reauthorization"),
					   Driver.Refresh(Envelope, 10.0, Cancellation.GetToken(), Error));
		TestEqual(TEXT("Refresh 400/401 maps to access profile not ready"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	}
	return true;
}

#endif // defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS
