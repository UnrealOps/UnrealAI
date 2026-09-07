// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIOAuthLoopbackAuthorizationBrowser.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Runtime/UnrealAIClock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
const FString LoopbackState = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
const FString LoopbackIssuer = TEXT("https://issuer.example.test");

struct FLoopbackBrowserCallResult final
{
	bool bSucceeded = false;
	FUnrealAIOAuthBrowserAuthorizationCallback Callback;
	FUnrealAIProviderAccessError Error;
};

class FRecordingOAuthBrowserLauncher final : public IUnrealAIOAuthSystemBrowserLauncher
{
  public:
	explicit FRecordingOAuthBrowserLauncher(TFunction<bool()> InLaunchProbe = {}) : LaunchProbe(MoveTemp(InLaunchProbe))
	{
		Launched = FPlatformProcess::GetSynchEventFromPool(true);
	}

	~FRecordingOAuthBrowserLauncher() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Launched);
	}

	bool LaunchSystemBrowser(const FStringView AuthorizationUrl, FUnrealAIProviderAccessError &OutError) override
	{
		OutError = {};
		++LaunchCount;
		bSawHttps = AuthorizationUrl.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive);
		bListenerReachableAtLaunch = !LaunchProbe || LaunchProbe();
		Launched->Trigger();
		return true;
	}

	bool WaitUntilLaunched() const
	{
		return Launched->Wait(5000);
	}

	TAtomic<int32> LaunchCount{0};
	TAtomic<bool> bSawHttps{false};
	TAtomic<bool> bListenerReachableAtLaunch{false};

  private:
	TFunction<bool()> LaunchProbe;
	FEvent *Launched = nullptr;
};

TSharedPtr<FInternetAddr> MakeIpv4LoopbackAddress(ISocketSubsystem &SocketSubsystem, const int32 Port)
{
	TSharedPtr<FInternetAddr> Address = SocketSubsystem.GetAddressFromString(TEXT("127.0.0.1"));
	if (Address.IsValid())
	{
		Address->SetPort(Port);
	}
	return Address;
}

bool CanConnectToIpv4Loopback(const int32 Port)
{
	ISocketSubsystem *const SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		return false;
	}
	TSharedPtr<FInternetAddr> Address = MakeIpv4LoopbackAddress(*SocketSubsystem, Port);
	if (!Address.IsValid())
	{
		return false;
	}
	FUniqueSocket Socket = SocketSubsystem->CreateUniqueSocket(NAME_Stream, TEXT("AgentOAuthLoopbackLaunchProbe"),
																				 Address->GetProtocolType());
	return Socket && Socket->Connect(*Address);
}

int32 ReserveEphemeralLoopbackPort()
{
	ISocketSubsystem *const SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		return 0;
	}
	TSharedPtr<FInternetAddr> Address = MakeIpv4LoopbackAddress(*SocketSubsystem, 0);
	if (!Address.IsValid())
	{
		return 0;
	}
	FUniqueSocket Socket = SocketSubsystem->CreateUniqueSocket(NAME_Stream, TEXT("AgentOAuthLoopbackPortProbe"),
																				 Address->GetProtocolType());
	if (!Socket || !Socket->Bind(*Address))
	{
		return 0;
	}
	Socket->GetAddress(*Address);
	return Address->GetPort();
}

bool SendRawLoopbackRequest(const int32 Port, const FString &Request, FString &OutResponse)
{
	OutResponse.Reset();
	ISocketSubsystem *const SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		return false;
	}
	TSharedPtr<FInternetAddr> Address = MakeIpv4LoopbackAddress(*SocketSubsystem, Port);
	if (!Address.IsValid())
	{
		return false;
	}
	FUniqueSocket Socket = SocketSubsystem->CreateUniqueSocket(NAME_Stream, TEXT("AgentOAuthLoopbackTestClient"),
																				 Address->GetProtocolType());
	if (!Socket || !Socket->Connect(*Address))
	{
		return false;
	}
	FTCHARToUTF8 RequestUtf8(*Request);
	int32 Offset = 0;
	while (Offset < RequestUtf8.Length())
	{
		if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(1.0)))
		{
			return false;
		}
		int32 Sent = 0;
		if (!Socket->Send(reinterpret_cast<const uint8 *>(RequestUtf8.Get()) + Offset, RequestUtf8.Length() - Offset,
						  Sent) ||
			Sent <= 0)
		{
			return false;
		}
		Offset += Sent;
	}
	TArray<uint8> ResponseBytes;
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (FPlatformTime::Seconds() < Deadline && ResponseBytes.Num() < 32 * 1024)
	{
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(20)))
		{
			continue;
		}
		uint8 Buffer[1024];
		int32 Read = 0;
		if (!Socket->Recv(Buffer, UE_ARRAY_COUNT(Buffer), Read) || Read <= 0)
		{
			break;
		}
		ResponseBytes.Append(Buffer, Read);
	}
	OutResponse.Reserve(ResponseBytes.Num());
	for (const uint8 Byte : ResponseBytes)
	{
		if (Byte > 0x7f)
		{
			return false;
		}
		OutResponse.AppendChar(static_cast<TCHAR>(Byte));
	}
	return !OutResponse.IsEmpty();
}

FUnrealAIOAuthBrowserAuthorizationLaunch MakeLoopbackLaunch(const int32 Port)
{
	FUnrealAIOAuthBrowserAuthorizationLaunch Launch;
	Launch.AuthorizationUrl = TEXT("https://issuer.example.test/authorize?state=") + LoopbackState;
	Launch.ExactRedirectUri = FString::Printf(TEXT("http://127.0.0.1:%d/callback"), Port);
	Launch.ExpectedIssuer = LoopbackIssuer;
	Launch.ExpectedState = LoopbackState;
	return Launch;
}

bool MakeLoopbackContext(const double TimeoutSeconds, const FUnrealAICancellationToken &Cancellation,
						 FUnrealAIOAuthAuthorizationOperationContext &OutContext)
{
	FString Error;
	return FUnrealAIOAuthAuthorizationOperationContext::TryCreate(
		MakeShared<FUnrealAISystemClock, ESPMode::ThreadSafe>(), TimeoutSeconds, Cancellation, OutContext, Error);
}

FString MakeGetRequest(const int32 Port, const FString &Query, const FString &HostOverride = {})
{
	const FString Host = HostOverride.IsEmpty() ? FString::Printf(TEXT("127.0.0.1:%d"), Port) : HostOverride;
	return FString::Printf(TEXT("GET /callback?%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"), *Query, *Host);
}

TFuture<TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>>
StartLoopbackBrowser(FUnrealAIOAuthLoopbackAuthorizationBrowser &Browser,
					 const FUnrealAIOAuthAuthorizationOperationContext &Context,
					 const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch)
{
	return Async(EAsyncExecution::Thread,
				 [&Browser, &Context, &Launch]()
				 {
					 const TSharedRef<FLoopbackBrowserCallResult, ESPMode::ThreadSafe> Result =
						 MakeShared<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>();
					 Result->bSucceeded = Browser.Authorize(Context, Launch, Result->Callback, Result->Error);
					 return Result.ToSharedPtr();
				 });
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthLoopbackAuthorizationBrowserSuccessTest,
								 "UnrealAI.Auth.OAuthOIDC.ProductionLoopback.SuccessProbeIsolationAndDenial",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthLoopbackAuthorizationBrowserSuccessTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const int32 Port = ReserveEphemeralLoopbackPort();
		TestTrue(TEXT("an ephemeral IPv4 loopback port is available"), Port > 0);
		if (Port <= 0)
		{
			return false;
		}
		const TSharedRef<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe> Launcher =
			MakeShared<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe>([Port]()
																			{ return CanConnectToIpv4Loopback(Port); });
		FUnrealAIOAuthLoopbackAuthorizationBrowser Browser(Launcher);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIOAuthAuthorizationOperationContext Context;
		TestTrue(TEXT("success context is valid"), MakeLoopbackContext(10.0, Cancellation.GetToken(), Context));
		const FUnrealAIOAuthBrowserAuthorizationLaunch Launch = MakeLoopbackLaunch(Port);
		TFuture<TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>> Call =
			StartLoopbackBrowser(Browser, Context, Launch);
		TestTrue(TEXT("listener binds before the system browser is invoked"), Launcher->WaitUntilLaunched());
		TestEqual(TEXT("system browser is launched exactly once"), Launcher->LaunchCount.Load(), 1);
		TestTrue(TEXT("only an HTTPS authorization URL reaches the launcher"), Launcher->bSawHttps.Load());
		TestTrue(TEXT("listener is reachable during the browser-launch call"),
					  Launcher->bListenerReachableAtLaunch.Load());

		FString ProbeResponse;
		TestTrue(TEXT("wrong-host probe receives a bounded response"),
					  SendRawLoopbackRequest(Port, MakeGetRequest(Port, TEXT("code=probe&state=") + LoopbackState,
																			 TEXT("attacker.example.test")),
																  ProbeResponse));
		TestTrue(TEXT("wrong-host probe is rejected"), ProbeResponse.Contains(TEXT("400 Bad Request")));
		TestFalse(TEXT("wrong-host probe cannot settle authorization"), Call.IsReady());

		FString WrongPathResponse;
		TestTrue(TEXT("wrong-path probe receives a bounded response"),
					  SendRawLoopbackRequest(
						  Port,
						  FString::Printf(TEXT("GET /other?code=probe&state=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n"),
											   *LoopbackState, Port),
										  WrongPathResponse));
		TestTrue(TEXT("wrong-path probe is rejected"), WrongPathResponse.Contains(TEXT("400 Bad Request")));
		TestFalse(TEXT("wrong callback path cannot settle authorization"), Call.IsReady());

		FString WrongStateResponse;
		TestTrue(
			TEXT("wrong-state callback receives a bounded response"),
				 SendRawLoopbackRequest(
					 Port, MakeGetRequest(Port, TEXT("code=probe&state=BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB")),
										  WrongStateResponse));
		TestTrue(TEXT("wrong-state callback is rejected"), WrongStateResponse.Contains(TEXT("400 Bad Request")));
		TestFalse(TEXT("wrong state is ignored rather than terminating the operation"), Call.IsReady());

		const FString EncodedIssuer = TEXT("https%3A%2F%2Fissuer.example.test");
		FString SuccessResponse;
		TestTrue(TEXT("valid callback completes over the production loopback socket"),
					  SendRawLoopbackRequest(Port, MakeGetRequest(Port, TEXT("code=abc%2D123&state=") + LoopbackState +
																			 TEXT("&iss=") + EncodedIssuer),
																  SuccessResponse));
		TestTrue(TEXT("valid callback returns a success page"), SuccessResponse.Contains(TEXT("200 OK")));
		TestFalse(TEXT("success page never reflects the authorization code"),
					   SuccessResponse.Contains(TEXT("abc-123")));
		TestFalse(TEXT("success page never reflects transaction state"), SuccessResponse.Contains(LoopbackState));
		TestTrue(TEXT("browser operation settles after the valid callback"), Call.WaitFor(FTimespan::FromSeconds(5.0)));
		const TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe> Result = Call.Get();
		TestTrue(TEXT("valid callback result is retained"), Result.IsValid());
		if (!Result.IsValid())
		{
			return false;
		}
		TestTrue(TEXT("valid callback succeeds"), Result.IsValid() && Result->bSucceeded);
		TestFalse(TEXT("valid callback has no error"), !Result.IsValid() || Result->Error.IsError());
		TestEqual(TEXT("valid callback kind is authorization code"), Result->Callback.Kind,
					   EUnrealAIOAuthBrowserCallbackKind::AuthorizationCode);
		TestEqual(TEXT("exact redirect is retained"), Result->Callback.ExactRedirectUri, Launch.ExactRedirectUri);
		TestEqual(TEXT("issuer context is retained"), Result->Callback.Issuer, LoopbackIssuer);
		TestEqual(TEXT("exact state is retained"), Result->Callback.State, LoopbackState);
		TestTrue(TEXT("authorization code is held as a secret"), Result->Callback.AuthorizationCode.IsSet());
		TestEqual(TEXT("decoded authorization-code size is exact"), Result->Callback.AuthorizationCode.Num(), 7);
	}
	{
		const int32 Port = ReserveEphemeralLoopbackPort();
		TestTrue(TEXT("denial fixture reserves a loopback port"), Port > 0);
		const TSharedRef<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe> Launcher =
			MakeShared<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe>();
		FUnrealAIOAuthLoopbackAuthorizationBrowser Browser(Launcher);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIOAuthAuthorizationOperationContext Context;
		TestTrue(TEXT("denial context is valid"), MakeLoopbackContext(10.0, Cancellation.GetToken(), Context));
		const FUnrealAIOAuthBrowserAuthorizationLaunch Launch = MakeLoopbackLaunch(Port);
		TFuture<TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>> Call =
			StartLoopbackBrowser(Browser, Context, Launch);
		TestTrue(TEXT("denial listener starts"), Launcher->WaitUntilLaunched());
		FString Response;
		TestTrue(TEXT("bounded access-denied callback is accepted"),
					  SendRawLoopbackRequest(
						  Port,
						  MakeGetRequest(Port, TEXT("error=access_denied&error_description=User+declined&error_uri=")
														TEXT("https%3A%2F%2Fissuer.example.test%2Foauth-error&state=") +
															 LoopbackState),
										 Response));
		TestTrue(TEXT("valid denial returns the generic completion page"), Response.Contains(TEXT("200 OK")));
		TestFalse(TEXT("provider denial text is never reflected"), Response.Contains(TEXT("User declined")));
		TestFalse(TEXT("provider denial code is never reflected"), Response.Contains(TEXT("access_denied")));
		TestTrue(TEXT("denial operation settles"), Call.WaitFor(FTimespan::FromSeconds(5.0)));
		const TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe> Result = Call.Get();
		TestTrue(TEXT("denial result is retained"), Result.IsValid());
		if (!Result.IsValid())
		{
			return false;
		}
		TestTrue(TEXT("denial is a valid protocol callback"), Result.IsValid() && Result->bSucceeded);
		TestEqual(TEXT("denial callback is typed"), Result->Callback.Kind, EUnrealAIOAuthBrowserCallbackKind::Denied);
		TestFalse(TEXT("denial callback carries no code"), Result->Callback.AuthorizationCode.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthLoopbackAuthorizationBrowserBoundsTest,
								 "UnrealAI.Auth.OAuthOIDC.ProductionLoopback.CancellationTimeoutAndBoundedFaults",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthLoopbackAuthorizationBrowserBoundsTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const int32 Port = ReserveEphemeralLoopbackPort();
		const TSharedRef<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe> Launcher =
			MakeShared<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe>();
		FUnrealAIOAuthLoopbackAuthorizationBrowser Browser(Launcher);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIOAuthAuthorizationOperationContext Context;
		TestTrue(TEXT("cancellation context is valid"), MakeLoopbackContext(10.0, Cancellation.GetToken(), Context));
		const FUnrealAIOAuthBrowserAuthorizationLaunch Launch = MakeLoopbackLaunch(Port);
		TFuture<TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>> Call =
			StartLoopbackBrowser(Browser, Context, Launch);
		TestTrue(TEXT("cancellable listener starts"), Launcher->WaitUntilLaunched());
		Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		TestTrue(TEXT("cancellation closes the physical listener promptly"), Call.WaitFor(FTimespan::FromSeconds(5.0)));
		const TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe> Result = Call.Get();
		TestTrue(TEXT("cancellation result is retained"), Result.IsValid());
		if (!Result.IsValid())
		{
			return false;
		}
		TestFalse(TEXT("cancelled browser operation fails"), Result.IsValid() && Result->bSucceeded);
		TestEqual(TEXT("cancellation error is typed"), Result->Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthCancelled);
	}
	{
		const int32 Port = ReserveEphemeralLoopbackPort();
		const TSharedRef<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe> Launcher =
			MakeShared<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe>();
		FUnrealAIOAuthLoopbackAuthorizationBrowser Browser(Launcher);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIOAuthAuthorizationOperationContext Context;
		TestTrue(TEXT("timeout context is valid"), MakeLoopbackContext(0.05, Cancellation.GetToken(), Context));
		const FUnrealAIOAuthBrowserAuthorizationLaunch Launch = MakeLoopbackLaunch(Port);
		TFuture<TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>> Call =
			StartLoopbackBrowser(Browser, Context, Launch);
		TestTrue(TEXT("timeout listener starts"), Launcher->WaitUntilLaunched());
		TestTrue(TEXT("deadline closes the physical listener promptly"), Call.WaitFor(FTimespan::FromSeconds(5.0)));
		const TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe> Result = Call.Get();
		TestTrue(TEXT("timeout result is retained"), Result.IsValid());
		if (!Result.IsValid())
		{
			return false;
		}
		TestFalse(TEXT("timed-out browser operation fails"), Result.IsValid() && Result->bSucceeded);
		TestEqual(TEXT("timeout error is typed"), Result->Error.Code, EUnrealAIProviderAccessErrorCode::AuthTimedOut);
	}
	{
		const int32 Port = ReserveEphemeralLoopbackPort();
		FUnrealAIOAuthLoopbackAuthorizationBrowserConfig Config;
		Config.MaxRequestHeaderBytes = 1024;
		Config.MaxAcceptedConnections = 2;
		const TSharedRef<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe> Launcher =
			MakeShared<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe>();
		FUnrealAIOAuthLoopbackAuthorizationBrowser Browser(Launcher, Config);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIOAuthAuthorizationOperationContext Context;
		TestTrue(TEXT("bounded-fault context is valid"), MakeLoopbackContext(10.0, Cancellation.GetToken(), Context));
		const FUnrealAIOAuthBrowserAuthorizationLaunch Launch = MakeLoopbackLaunch(Port);
		TFuture<TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe>> Call =
			StartLoopbackBrowser(Browser, Context, Launch);
		TestTrue(TEXT("bounded-fault listener starts"), Launcher->WaitUntilLaunched());
		FString OversizedResponse;
		const FString OversizedRequest = FString::Printf(
			TEXT("GET /callback?code=probe&state=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Padding: %s\r\n\r\n"),
				 *LoopbackState, Port, *FString::ChrN(Config.MaxRequestHeaderBytes, TEXT('A')));
		TestTrue(TEXT("oversized callback header receives a bounded response"),
					  SendRawLoopbackRequest(Port, OversizedRequest, OversizedResponse));
		TestTrue(TEXT("oversized callback header is rejected"), OversizedResponse.Contains(TEXT("400 Bad Request")));
		FString MalformedResponse;
		TestTrue(TEXT("malformed callback receives a bounded response"),
					  SendRawLoopbackRequest(
						  Port, FString::Printf(TEXT("POST /callback HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n"), Port),
												MalformedResponse));
		TestTrue(TEXT("malformed callback is rejected"), MalformedResponse.Contains(TEXT("400 Bad Request")));
		TestTrue(TEXT("connection budget terminates malformed callback flooding"),
					  Call.WaitFor(FTimespan::FromSeconds(5.0)));
		const TSharedPtr<FLoopbackBrowserCallResult, ESPMode::ThreadSafe> Result = Call.Get();
		TestTrue(TEXT("malformed-flood result is retained"), Result.IsValid());
		if (!Result.IsValid())
		{
			return false;
		}
		TestFalse(TEXT("malformed flood fails"), Result.IsValid() && Result->bSucceeded);
		TestEqual(TEXT("malformed flood has a stable error"), Result->Error.Code,
					   EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
	}
	{
		const int32 Port = ReserveEphemeralLoopbackPort();
		const TSharedRef<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe> Launcher =
			MakeShared<FRecordingOAuthBrowserLauncher, ESPMode::ThreadSafe>();
		FUnrealAIOAuthLoopbackAuthorizationBrowser Browser(Launcher);
		FUnrealAICancellationSource Cancellation;
		FUnrealAIOAuthAuthorizationOperationContext Context;
		TestTrue(TEXT("unsupported-host context is valid"), MakeLoopbackContext(5.0, Cancellation.GetToken(), Context));
		FUnrealAIOAuthBrowserAuthorizationLaunch Launch = MakeLoopbackLaunch(Port);
		Launch.ExactRedirectUri = FString::Printf(TEXT("http://localhost:%d/callback"), Port);
		FLoopbackBrowserCallResult Result;
		Result.bSucceeded = Browser.Authorize(Context, Launch, Result.Callback, Result.Error);
		TestFalse(TEXT("DNS loopback redirect is rejected in favor of an exact numeric literal"), Result.bSucceeded);
		TestEqual(TEXT("unsupported redirect is a configuration error"), Result.Error.Code,
					   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		TestEqual(TEXT("invalid redirect never opens a browser"), Launcher->LaunchCount.Load(), 0);
	}
	{
		FUnrealAIPlatformOAuthSystemBrowserLauncher Launcher;
		FUnrealAIProviderAccessError Error;
		TestFalse(TEXT("production launcher rejects non-HTTPS authorization URLs without opening them"),
					   Launcher.LaunchSystemBrowser(TEXT("http://issuer.example.test/authorize"), Error));
		TestEqual(TEXT("non-HTTPS browser launch is a typed unsupported capability"), Error.Code,
					   EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
