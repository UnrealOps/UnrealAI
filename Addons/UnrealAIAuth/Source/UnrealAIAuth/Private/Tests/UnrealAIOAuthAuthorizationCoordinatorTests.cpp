// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthAuthorizationCoordinator.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Runtime/UnrealAIClock.h"

namespace
{
FUnrealAIProviderAccessError MakeCoordinatorTestError(const EUnrealAIErrorCategory Category,
													  const EUnrealAIProviderAccessErrorCode Code,
													  const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

bool MakeCoordinatorTestSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret)
{
	TArray<uint8> Bytes;
	for (const ANSICHAR *Cursor = Text; Cursor != nullptr && *Cursor != '\0'; ++Cursor)
	{
		Bytes.Add(static_cast<uint8>(*Cursor));
	}
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

FUnrealAIOAuthTrustedAuthorizationServer MakeCoordinatorTestServer()
{
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	Server.Issuer = TEXT("https://issuer.example.test");
	Server.DiscoveryEndpoint = TEXT("https://issuer.example.test/.well-known/openid-configuration");
	Server.AuthorizationEndpoint = TEXT("https://issuer.example.test/oauth2/authorize");
	Server.TokenEndpoint = TEXT("https://issuer.example.test/oauth2/token");
	Server.JwksEndpoint = TEXT("https://issuer.example.test/oauth2/jwks");
	Server.RevocationEndpoint = TEXT("https://issuer.example.test/oauth2/revoke");
	Server.AllowedSigningAlgorithms = {FName(TEXT("RS256"))};
	return Server;
}

FUnrealAIOAuthBrowserAuthorizationRequest MakeCoordinatorBrowserRequest()
{
	FUnrealAIOAuthBrowserAuthorizationRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.Server = MakeCoordinatorTestServer();
	Request.ClientId = TEXT("test-public-client");
	Request.Audience = TEXT("https://api.example.test/resource");
	Request.ExactRedirectUri = TEXT("http://127.0.0.1:43821/callback");
	Request.RequestedScopes = {TEXT("openid"), TEXT("profile")};
	Request.TimeoutSeconds = 30.0;
	Request.ClockSkewSeconds = 30.0;
	Request.MaxIdentityTokenAgeSeconds = 900.0;
	Request.MaxJwksCacheAgeSeconds = 600.0;
	return Request;
}

FUnrealAIOAuthRevocationRequest MakeCoordinatorRevocationRequest()
{
	FUnrealAIOAuthRevocationRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.Server = MakeCoordinatorTestServer();
	Request.ClientId = TEXT("test-public-client");
	Request.TimeoutSeconds = 30.0;
	return Request;
}

template <typename PredicateType>
bool WaitForCoordinatorCondition(PredicateType &&Predicate, const double Seconds = 5.0)
{
	const double Deadline = FPlatformTime::Seconds() + Seconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (Predicate())
		{
			return true;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return Predicate();
}

class FCoordinatorTestSink final : public IUnrealAIOAuthAuthorizationCompletionSink
{
  public:
	FCoordinatorTestSink()
	{
		Event = FPlatformProcess::GetSynchEventFromPool(true);
	}

	~FCoordinatorTestSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void CompleteOAuthAuthorization(FUnrealAIOAuthAuthorizationCompletion &&Completion) override
	{
		FString ShapeError;
		const bool bShapeValid = Completion.ValidateShape(ShapeError);
		{
			FScopeLock Lock(&Mutex);
			++CompletionCount;
			LastRequestId = Completion.RequestId;
			LastOperationKind = Completion.OperationKind;
			LastTerminalKind = Completion.TerminalKind;
			LastError = Completion.Error;
			bLastShapeValid = bShapeValid;
			bLastHadAccessToken = Completion.AuthorizationResult.Tokens.AccessToken.IsSet();
		}
		Event->Trigger();
	}

	bool Wait(const uint32 Milliseconds = 5000) const
	{
		return Event->Wait(Milliseconds);
	}

	int32 GetCompletionCount() const
	{
		FScopeLock Lock(&Mutex);
		return CompletionCount;
	}

	EUnrealAIOAuthAuthorizationTerminalKind GetTerminalKind() const
	{
		FScopeLock Lock(&Mutex);
		return LastTerminalKind;
	}

	EUnrealAIOAuthAuthorizationOperationKind GetOperationKind() const
	{
		FScopeLock Lock(&Mutex);
		return LastOperationKind;
	}

	EUnrealAIProviderAccessErrorCode GetErrorCode() const
	{
		FScopeLock Lock(&Mutex);
		return LastError.Code;
	}

	bool WasShapeValid() const
	{
		FScopeLock Lock(&Mutex);
		return bLastShapeValid;
	}

	bool HadAccessToken() const
	{
		FScopeLock Lock(&Mutex);
		return bLastHadAccessToken;
	}

  private:
	mutable FCriticalSection Mutex;
	FEvent *Event = nullptr;
	int32 CompletionCount = 0;
	FUnrealAIRequestId LastRequestId;
	EUnrealAIOAuthAuthorizationOperationKind LastOperationKind = EUnrealAIOAuthAuthorizationOperationKind::Invalid;
	EUnrealAIOAuthAuthorizationTerminalKind LastTerminalKind = EUnrealAIOAuthAuthorizationTerminalKind::Invalid;
	FUnrealAIProviderAccessError LastError;
	bool bLastShapeValid = false;
	bool bLastHadAccessToken = false;
};

class FControlledCoordinatorExecutor final : public IUnrealAIOAuthAuthorizationExecutor
{
  public:
	FControlledCoordinatorExecutor()
	{
		Entered = FPlatformProcess::GetSynchEventFromPool(true);
		Release = FPlatformProcess::GetSynchEventFromPool(true);
	}

	~FControlledCoordinatorExecutor() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(Release);
	}

	bool AuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
							  const FUnrealAICancellationToken &Cancellation,
							  FUnrealAIOAuthAuthorizationResult &OutResult,
							  FUnrealAIProviderAccessError &OutError) override
	{
		(void)Request;
		++AuthorizeCalls;
		if (!WaitForRelease(Cancellation, OutError))
		{
			return false;
		}
		if (bFail)
		{
			OutError = MakeCoordinatorTestError(EUnrealAIErrorCategory::Provider,
												EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		OutResult.Reset();
		if (!MakeCoordinatorTestSecret("access-token", OutResult.Tokens.AccessToken) ||
			!MakeCoordinatorTestSecret("refresh-token", OutResult.Tokens.RefreshToken) ||
			!MakeCoordinatorTestSecret("id-token", OutResult.Tokens.IdToken))
		{
			OutResult.Reset();
			OutError =
				MakeCoordinatorTestError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
			return false;
		}
		OutResult.Tokens.AccessTokenExpiresAtUtc = FDateTime(2035, 1, 2, 4, 4, 5);
		OutResult.SubjectFingerprint = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		OutResult.GrantedScopes = {TEXT("openid"), TEXT("profile")};
		OutError = {};
		return true;
	}

	bool Revoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
				const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) override
	{
		(void)Request;
		++RevokeCalls;
		bSawRevocationToken = Token.IsSet();
		ON_SCOPE_EXIT
		{
			Token.Reset();
		};
		if (!WaitForRelease(Cancellation, OutError))
		{
			return false;
		}
		if (bFail)
		{
			OutError = MakeCoordinatorTestError(EUnrealAIErrorCategory::Provider,
												EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		OutError = {};
		return true;
	}

	bool WaitUntilEntered() const
	{
		return Entered->Wait(5000);
	}

	void AllowCompletion() const
	{
		Release->Trigger();
	}

	bool bIgnoreCancellation = false;
	bool bFail = false;
	TAtomic<int32> AuthorizeCalls{0};
	TAtomic<int32> RevokeCalls{0};
	TAtomic<bool> bSawRevocationToken{false};

  private:
	bool WaitForRelease(const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) const
	{
		Entered->Trigger();
		while (!Release->Wait(1))
		{
			if (!bIgnoreCancellation && Cancellation.IsCancellationRequested())
			{
				const bool bTimeout = Cancellation.GetReason() == EUnrealAICancellationReason::Timeout;
				OutError = MakeCoordinatorTestError(bTimeout ? EUnrealAIErrorCategory::Timeout
															 : EUnrealAIErrorCategory::Cancelled,
													bTimeout ? EUnrealAIProviderAccessErrorCode::AuthTimedOut
															 : EUnrealAIProviderAccessErrorCode::AuthCancelled,
													bTimeout);
				return false;
			}
		}
		return true;
	}

	FEvent *Entered = nullptr;
	FEvent *Release = nullptr;
};

FUnrealAIOAuthAuthorizationCoordinatorConfig MakeCoordinatorTestConfig(const int32 MaxActiveOperations = 4)
{
	FUnrealAIOAuthAuthorizationCoordinatorConfig Config;
	Config.MaxActiveOperations = MaxActiveOperations;
	Config.DeadlinePollSeconds = 0.001;
	Config.bDisableBackgroundDeadlineMonitorForTesting = true;
	return Config;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthAuthorizationCoordinatorSuccessTest,
								 "UnrealAI.Auth.OAuthOIDC.AsyncCoordinatorAuthorizeAndRevoke",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthAuthorizationCoordinatorSuccessTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5), 100.0);
		const TSharedRef<FControlledCoordinatorExecutor, ESPMode::ThreadSafe> Executor =
			MakeShared<FControlledCoordinatorExecutor, ESPMode::ThreadSafe>();
		FUnrealAIOAuthAuthorizationCoordinator Coordinator(Executor, Clock, MakeCoordinatorTestConfig());
		const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
		FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("browser authorization is admitted"),
					  Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), Sink,
															Cancellation.GetToken(), Handle, Error));
		TestTrue(TEXT("authorization worker entered executor"), Executor->WaitUntilEntered());
		Executor->AllowCompletion();
		TestTrue(TEXT("authorization terminal delivered"), Sink->Wait());
		TestEqual(TEXT("authorization terminal delivered once"), Sink->GetCompletionCount(), 1);
		TestEqual(TEXT("authorization terminal succeeds"), Sink->GetTerminalKind(),
					   EUnrealAIOAuthAuthorizationTerminalKind::Succeeded);
		TestEqual(TEXT("authorization operation identity retained"), Sink->GetOperationKind(),
					   EUnrealAIOAuthAuthorizationOperationKind::AuthorizeBrowserPkce);
		TestTrue(TEXT("authorization completion shape is valid"), Sink->WasShapeValid());
		TestTrue(TEXT("authorization completion carries token"), Sink->HadAccessToken());
		TestTrue(TEXT("authorization physically settles"),
					  WaitForCoordinatorCondition([&Coordinator]()
												  { return Coordinator.GetPhysicalOperationCount() == 0; }));
	}
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5), 100.0);
		const TSharedRef<FControlledCoordinatorExecutor, ESPMode::ThreadSafe> Executor =
			MakeShared<FControlledCoordinatorExecutor, ESPMode::ThreadSafe>();
		FUnrealAIOAuthAuthorizationCoordinator Coordinator(Executor, Clock, MakeCoordinatorTestConfig());
		const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
		FUnrealAISecretValue Token;
		TestTrue(TEXT("revocation fixture token created"), MakeCoordinatorTestSecret("revoke-me", Token));
		FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("revocation is admitted"),
					  Coordinator.StartRevoke(MakeCoordinatorRevocationRequest(), MoveTemp(Token), Sink,
											  Cancellation.GetToken(), Handle, Error));
		TestFalse(TEXT("coordinator consumes caller revocation token"), Token.IsSet());
		TestTrue(TEXT("revocation worker entered executor"), Executor->WaitUntilEntered());
		Executor->AllowCompletion();
		TestTrue(TEXT("revocation terminal delivered"), Sink->Wait());
		TestEqual(TEXT("revocation terminal delivered once"), Sink->GetCompletionCount(), 1);
		TestEqual(TEXT("revocation operation identity retained"), Sink->GetOperationKind(),
					   EUnrealAIOAuthAuthorizationOperationKind::Revoke);
		TestEqual(TEXT("revocation terminal succeeds"), Sink->GetTerminalKind(),
					   EUnrealAIOAuthAuthorizationTerminalKind::Succeeded);
		TestTrue(TEXT("revocation completion shape is valid"), Sink->WasShapeValid());
		TestTrue(TEXT("executor received revocation token"), Executor->bSawRevocationToken.Load());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthAuthorizationCoordinatorRaceTest,
								 "UnrealAI.Auth.OAuthOIDC.AsyncCoordinatorCancelTimeoutShutdownLateTerminalOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthAuthorizationCoordinatorRaceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5), 100.0);
		const TSharedRef<FControlledCoordinatorExecutor, ESPMode::ThreadSafe> Executor =
			MakeShared<FControlledCoordinatorExecutor, ESPMode::ThreadSafe>();
		Executor->bIgnoreCancellation = true;
		FUnrealAIOAuthAuthorizationCoordinator Coordinator(Executor, Clock, MakeCoordinatorTestConfig());
		const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
		FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("cancellable authorization admitted"),
					  Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), Sink,
															Cancellation.GetToken(), Handle, Error));
		TestTrue(TEXT("cancellable executor entered"), Executor->WaitUntilEntered());
		Handle->Cancel();
		TestTrue(TEXT("cancel terminal delivered promptly"), Sink->Wait());
		TestEqual(TEXT("cancel is typed"), Sink->GetTerminalKind(), EUnrealAIOAuthAuthorizationTerminalKind::Cancelled);
		TestEqual(TEXT("cancel error code is stable"), Sink->GetErrorCode(),
					   EUnrealAIProviderAccessErrorCode::AuthCancelled);
		TestEqual(TEXT("logical operation removed before physical settlement"), Coordinator.GetActiveOperationCount(),
					   0);
		TestEqual(TEXT("late executor remains physically retained"), Coordinator.GetPhysicalOperationCount(), 1);
		Executor->AllowCompletion();
		TestTrue(TEXT("cancelled physical operation drains"),
					  WaitForCoordinatorCondition([&Coordinator]()
												  { return Coordinator.GetPhysicalOperationCount() == 0; }));
		TestEqual(TEXT("late success cannot double-deliver after cancel"), Sink->GetCompletionCount(), 1);
	}
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5), 100.0);
		const TSharedRef<FControlledCoordinatorExecutor, ESPMode::ThreadSafe> Executor =
			MakeShared<FControlledCoordinatorExecutor, ESPMode::ThreadSafe>();
		Executor->bIgnoreCancellation = true;
		FUnrealAIOAuthAuthorizationCoordinator Coordinator(Executor, Clock, MakeCoordinatorTestConfig());
		const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
		FUnrealAIOAuthBrowserAuthorizationRequest Request = MakeCoordinatorBrowserRequest();
		Request.TimeoutSeconds = 1.0;
		FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("timeout authorization admitted"),
					  Coordinator.StartAuthorizeBrowserPkce(Request, Sink, Cancellation.GetToken(), Handle, Error));
		TestTrue(TEXT("timeout executor entered"), Executor->WaitUntilEntered());
		Clock->Advance(FTimespan::FromSeconds(2.0));
		TestEqual(TEXT("deadline pump settles one operation"), Coordinator.PumpDeadlines(), 1);
		TestTrue(TEXT("timeout terminal delivered"), Sink->Wait());
		TestEqual(TEXT("timeout is typed"), Sink->GetTerminalKind(), EUnrealAIOAuthAuthorizationTerminalKind::TimedOut);
		TestEqual(TEXT("timeout error code is stable"), Sink->GetErrorCode(),
					   EUnrealAIProviderAccessErrorCode::AuthTimedOut);
		Executor->AllowCompletion();
		TestTrue(TEXT("timed-out physical operation drains"),
					  WaitForCoordinatorCondition([&Coordinator]()
												  { return Coordinator.GetPhysicalOperationCount() == 0; }));
		TestEqual(TEXT("late success cannot double-deliver after timeout"), Sink->GetCompletionCount(), 1);
	}
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5), 100.0);
		const TSharedRef<FControlledCoordinatorExecutor, ESPMode::ThreadSafe> Executor =
			MakeShared<FControlledCoordinatorExecutor, ESPMode::ThreadSafe>();
		Executor->bIgnoreCancellation = true;
		FUnrealAIOAuthAuthorizationCoordinator Coordinator(Executor, Clock, MakeCoordinatorTestConfig());
		const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
		FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		TestTrue(TEXT("shutdown authorization admitted"),
					  Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), Sink,
															Cancellation.GetToken(), Handle, Error));
		TestTrue(TEXT("shutdown executor entered"), Executor->WaitUntilEntered());
		Coordinator.BeginShutdown();
		TestTrue(TEXT("shutdown terminal delivered"), Sink->Wait());
		TestTrue(TEXT("coordinator reports shutdown"), Coordinator.IsShutdown());
		TestEqual(TEXT("shutdown uses cancellation terminal"), Sink->GetTerminalKind(),
					   EUnrealAIOAuthAuthorizationTerminalKind::Cancelled);
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> RejectedHandle;
		TestFalse(TEXT("shutdown rejects new work"),
					   Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), Sink,
															 Cancellation.GetToken(), RejectedHandle, Error));
		Executor->AllowCompletion();
		TestTrue(TEXT("shutdown physical operation drains"),
					  WaitForCoordinatorCondition([&Coordinator]()
												  { return Coordinator.GetPhysicalOperationCount() == 0; }));
		TestEqual(TEXT("late success cannot revive after shutdown"), Sink->GetCompletionCount(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthAuthorizationCoordinatorCapacityTest,
								 "UnrealAI.Auth.OAuthOIDC.AsyncCoordinatorPhysicalCapacity",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthAuthorizationCoordinatorCapacityTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2035, 1, 2, 3, 4, 5), 100.0);
	const TSharedRef<FControlledCoordinatorExecutor, ESPMode::ThreadSafe> Executor =
		MakeShared<FControlledCoordinatorExecutor, ESPMode::ThreadSafe>();
	Executor->bIgnoreCancellation = true;
	FUnrealAIOAuthAuthorizationCoordinator Coordinator(Executor, Clock, MakeCoordinatorTestConfig(1));
	const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> FirstSink =
		MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> FirstHandle;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("first capacity operation admitted"),
				  Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), FirstSink,
														Cancellation.GetToken(), FirstHandle, Error));
	TestTrue(TEXT("first capacity executor entered"), Executor->WaitUntilEntered());

	const TSharedRef<FCoordinatorTestSink, ESPMode::ThreadSafe> RejectedSink =
		MakeShared<FCoordinatorTestSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> RejectedHandle;
	TestFalse(TEXT("physical capacity rejects concurrent work"),
				   Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), RejectedSink,
														 Cancellation.GetToken(), RejectedHandle, Error));
	TestEqual(TEXT("capacity failure is typed"), Error.Code, EUnrealAIProviderAccessErrorCode::OperationBusy);
	FirstHandle->Cancel();
	TestTrue(TEXT("first capacity cancel delivered"), FirstSink->Wait());
	TestFalse(TEXT("logical cancellation does not free physical capacity"),
				   Coordinator.StartAuthorizeBrowserPkce(MakeCoordinatorBrowserRequest(), RejectedSink,
														 Cancellation.GetToken(), RejectedHandle, Error));
	Executor->AllowCompletion();
	TestTrue(
		TEXT("capacity operation physically drains"),
			 WaitForCoordinatorCondition([&Coordinator]() { return Coordinator.GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("capacity cancellation remains terminal-once"), FirstSink->GetCompletionCount(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
