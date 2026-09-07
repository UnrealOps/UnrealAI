// Copyright EngineWorks. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"

#include "Auth/UnrealAIAccountAuthProviderRegistry.h"
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIDeviceOAuthAccountProvider.h"
#include "Auth/UnrealAIDeviceOAuthCompositionRoot.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "Auth/UnrealAIMemorySecretStore.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

class FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority final
{
  public:
	static bool StartSignIn(FUnrealAIDeviceOAuthAccountProvider &Provider,
							const FUnrealAIInteractiveAuthRequest &Request,
							TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							const FUnrealAICancellationToken &Cancellation,
							TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							FUnrealAIProviderAccessError &OutError)
	{
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider.StartSignIn(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}

	static bool StartSignOut(FUnrealAIDeviceOAuthAccountProvider &Provider, const FUnrealAIAccountAuthRequest &Request,
							 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							 const FUnrealAICancellationToken &Cancellation,
							 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							 FUnrealAIProviderAccessError &OutError)
	{
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider.StartSignOut(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}
};

namespace
{
constexpr TCHAR DeviceOAuthStoreName[] = TEXT("tests.device_oauth.store");
constexpr TCHAR DeviceOAuthProviderName[] = TEXT("tests.device_oauth.auth");
constexpr TCHAR DeviceOAuthModelProviderName[] = TEXT("tests.device_oauth.model");
constexpr TCHAR DeviceOAuthProfileName[] = TEXT("tests.device_oauth.profile");
constexpr TCHAR DeviceOAuthEndpointName[] = TEXT("tests.device_oauth.endpoint");
constexpr TCHAR DeviceOAuthConnectionName[] = TEXT("tests.device_oauth.connection");
constexpr TCHAR DeviceOAuthSelectionName[] = TEXT("tests.device_oauth.account.default");

const FUnrealAIAccessAccountId DeviceOAuthAccountId{FGuid(0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f001)};
const FUnrealAISecretHandle DeviceOAuthSecretHandle{DeviceOAuthStoreName,
													FGuid(0xabcdef01, 0x23456789, 0x13572468, 0x24681357)};

FUnrealAIProviderAccessError MakeDeviceOAuthError(const EUnrealAIErrorCategory Category,
												  const EUnrealAIProviderAccessErrorCode Code,
												  const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

bool MakeDeviceOAuthSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	TArray<uint8> Bytes;
	const int32 Length = FCStringAnsi::Strlen(Text);
	Bytes.Append(reinterpret_cast<const uint8 *>(Text), Length);
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

bool WaitForDeviceOAuthCondition(TFunctionRef<bool()> Predicate, const double TimeoutSeconds = 3.0)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!Predicate() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return Predicate();
}

FUnrealAIDeviceOAuthAccountProviderConfig MakeDeviceOAuthConfig(const bool bRequiresProtectedSecondary = true)
{
	FUnrealAIDeviceOAuthAccountProviderConfig Config;
	Config.ProviderName = DeviceOAuthProviderName;
	Config.ModelProviderName = DeviceOAuthModelProviderName;
	Config.AuthProfileId = DeviceOAuthProfileName;
	Config.AccountId = DeviceOAuthAccountId;
	Config.SecretHandle = DeviceOAuthSecretHandle;
	Config.bRequiresProtectedSecondary = bRequiresProtectedSecondary;
	return Config;
}

FUnrealAIDeviceOAuthCompositionConfig MakeDeviceOAuthCompositionConfig()
{
	FUnrealAIDeviceOAuthCompositionConfig Config;
	Config.AccountProvider = MakeDeviceOAuthConfig();
	Config.AccountCatalog.SelectionAlias = DeviceOAuthSelectionName;
	Config.AccountCatalog.DisplayLabel = TEXT("Device OAuth test account");
	Config.AccountCatalog.ProviderName = DeviceOAuthProviderName;
	Config.AccountCatalog.AuthProfileId = DeviceOAuthProfileName;
	Config.AccountCatalog.AccountId = DeviceOAuthAccountId;
	Config.AccountCatalog.ConnectionAlias = DeviceOAuthConnectionName;
	Config.bStartStoredSessionRecovery = false;
	return Config;
}

FUnrealAIInteractiveAuthRequest MakeDeviceOAuthSignInRequest(const uint32 Seed, const float TimeoutSeconds = 30.0f)
{
	FUnrealAIInteractiveAuthRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.AuthProfileId = DeviceOAuthProfileName;
	Request.Flow = EUnrealAIInteractiveAuthFlow::DeviceCode;
	Request.TimeoutSeconds = TimeoutSeconds;
	return Request;
}

FUnrealAIAccountAuthRequest MakeDeviceOAuthSignOutRequest(const uint32 Seed, const float TimeoutSeconds = 30.0f)
{
	FUnrealAIAccountAuthRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.AuthProfileId = DeviceOAuthProfileName;
	Request.AccountId = DeviceOAuthAccountId;
	Request.TimeoutSeconds = TimeoutSeconds;
	return Request;
}

void SetDeviceOAuthStoreFault(const EUnrealAISecretStoreResult Result, FUnrealAIProviderAccessError &OutError)
{
	switch (Result)
	{
	case EUnrealAISecretStoreResult::Conflict:
		OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::VersionMismatch,
										EUnrealAIProviderAccessErrorCode::SecretRevisionConflict, true);
		break;
	case EUnrealAISecretStoreResult::Locked:
		OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::Busy,
										EUnrealAIProviderAccessErrorCode::SecretStoreLocked, true);
		break;
	case EUnrealAISecretStoreResult::Denied:
		OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::PolicyDenied,
										EUnrealAIProviderAccessErrorCode::SecretStoreDenied);
		break;
	default:
		OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		break;
	}
}

class FDeviceOAuthTestStore final : public IUnrealAISecretStore
{
  public:
	explicit FDeviceOAuthTestStore(const FUnrealAISecretStoreCapabilities &InCapabilities = MakeValidCapabilities())
		: Inner(MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(DeviceOAuthStoreName, 8)),
		  Capabilities(InCapabilities), LoadEntered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseLoad(FPlatformProcess::GetSynchEventFromPool(true)),
		  LoadReturned(FPlatformProcess::GetSynchEventFromPool(true)),
		  StoreEntered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseStore(FPlatformProcess::GetSynchEventFromPool(true)),
		  StoreReturned(FPlatformProcess::GetSynchEventFromPool(true))
	{
		ReleaseLoad->Trigger();
		ReleaseStore->Trigger();
	}

	~FDeviceOAuthTestStore() override
	{
		ReleaseLoad->Trigger();
		ReleaseStore->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(LoadEntered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseLoad);
		FPlatformProcess::ReturnSynchEventToPool(LoadReturned);
		FPlatformProcess::ReturnSynchEventToPool(StoreEntered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseStore);
		FPlatformProcess::ReturnSynchEventToPool(StoreReturned);
	}

	FName GetStoreName() const override
	{
		return DeviceOAuthStoreName;
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		return Capabilities;
	}

	static FUnrealAISecretStoreCapabilities MakeValidCapabilities()
	{
		FUnrealAISecretStoreCapabilities Result;
		Result.PersistenceClass = EUnrealAISecretStorePersistenceClass::Persistent;
		Result.ProtectionClass = EUnrealAISecretStoreProtectionClass::PlatformCredentialStore;
		Result.ScopeClass = EUnrealAISecretStoreScopeClass::CurrentUser;
		Result.bAvailableInCurrentBuild = true;
		Result.bPersistent = true;
		Result.bAtomicCompareAndSwap = true;
		Result.bAvailableInShipping = false;
		return Result;
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
									const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
									uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) override
	{
		++LoadCalls;
		const int32 Forced = ForcedLoadResult.Exchange(NoForcedResult);
		if (Forced != NoForcedResult)
		{
			OutValue.Reset();
			OutRevision = 0;
			const EUnrealAISecretStoreResult Result = static_cast<EUnrealAISecretStoreResult>(Forced);
			SetDeviceOAuthStoreFault(Result, OutError);
			return Result;
		}

		const EUnrealAISecretStoreResult Result = Inner->Load(Context, Handle, OutValue, OutRevision, OutError);
		if (bBlockNextLoadAfterRead.Exchange(false))
		{
			LoadEntered->Trigger();
			ReleaseLoad->Wait(5000);
			LoadReturned->Trigger();
		}
		return Result;
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 const uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		++StoreCalls;
		LastStoreExpectedRevision.Store(ExpectedRevision);
		const int32 Forced = ForcedStoreResult.Exchange(NoForcedResult);
		if (Forced != NoForcedResult)
		{
			OutNewRevision = 0;
			const EUnrealAISecretStoreResult Result = static_cast<EUnrealAISecretStoreResult>(Forced);
			SetDeviceOAuthStoreFault(Result, OutError);
			return Result;
		}

		const EUnrealAISecretStoreResult Result =
			Inner->Store(Context, Handle, Value, ExpectedRevision, OutNewRevision, OutError);
		if (Result == EUnrealAISecretStoreResult::Succeeded && bInstallConcurrentWinnerAfterCommit.Exchange(false))
		{
			TUniquePtr<FUnrealAISecretValue> Winner;
			{
				FScopeLock Lock(&FaultMutex);
				Winner = MoveTemp(ConcurrentWinner);
			}
			uint64 WinnerRevision = 0;
			FUnrealAIProviderAccessError WinnerError;
			if (!Winner.IsValid() || Inner->Store(Context, Handle, *Winner, OutNewRevision, WinnerRevision,
												  WinnerError) != EUnrealAISecretStoreResult::Succeeded)
			{
				OutNewRevision = 0;
				OutError = WinnerError;
				return EUnrealAISecretStoreResult::Failed;
			}
			ConcurrentWinnerRevision.Store(WinnerRevision);
			OutNewRevision = 0;
			OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
			return EUnrealAISecretStoreResult::Cancelled;
		}
		if (bBlockNextStoreAfterCommit.Exchange(false))
		{
			StoreEntered->Trigger();
			ReleaseStore->Wait(5000);
			StoreReturned->Trigger();
		}
		if (bReportCancelledAfterCommit.Exchange(false) && Context.IsCancellationRequested())
		{
			OutNewRevision = 0;
			OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
			return EUnrealAISecretStoreResult::Cancelled;
		}
		return Result;
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, const uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override
	{
		++DeleteCalls;
		LastDeleteExpectedRevision.Store(ExpectedRevision);
		const int32 Forced = ForcedDeleteResult.Exchange(NoForcedResult);
		if (Forced != NoForcedResult)
		{
			const EUnrealAISecretStoreResult Result = static_cast<EUnrealAISecretStoreResult>(Forced);
			SetDeviceOAuthStoreFault(Result, OutError);
			return Result;
		}
		return Inner->Delete(Context, Handle, ExpectedRevision, OutError);
	}

	void ForceNextLoad(const EUnrealAISecretStoreResult Result)
	{
		ForcedLoadResult.Store(static_cast<int32>(Result));
	}

	void ForceNextStore(const EUnrealAISecretStoreResult Result)
	{
		ForcedStoreResult.Store(static_cast<int32>(Result));
	}

	void ForceNextDelete(const EUnrealAISecretStoreResult Result)
	{
		ForcedDeleteResult.Store(static_cast<int32>(Result));
	}

	void BlockNextLoadAfterRead()
	{
		LoadEntered->Reset();
		LoadReturned->Reset();
		ReleaseLoad->Reset();
		bBlockNextLoadAfterRead.Store(true);
	}

	void ReleaseBlockedLoad()
	{
		ReleaseLoad->Trigger();
	}

	bool WaitForLoadEntered(const uint32 TimeoutMilliseconds = 3000) const
	{
		return LoadEntered->Wait(TimeoutMilliseconds);
	}

	bool WaitForLoadReturned(const uint32 TimeoutMilliseconds = 3000) const
	{
		return LoadReturned->Wait(TimeoutMilliseconds);
	}

	void BlockNextStoreAfterCommit()
	{
		StoreEntered->Reset();
		StoreReturned->Reset();
		ReleaseStore->Reset();
		bBlockNextStoreAfterCommit.Store(true);
	}

	void ReportCancelledAfterNextCommit()
	{
		bReportCancelledAfterCommit.Store(true);
	}

	void InstallConcurrentWinnerAndReportCancelledAfterNextCommit(FUnrealAISecretValue &&Winner)
	{
		{
			FScopeLock Lock(&FaultMutex);
			ConcurrentWinner = MakeUnique<FUnrealAISecretValue>(MoveTemp(Winner));
		}
		bInstallConcurrentWinnerAfterCommit.Store(true);
	}

	void ReleaseBlockedStore()
	{
		ReleaseStore->Trigger();
	}

	bool WaitForStoreEntered(const uint32 TimeoutMilliseconds = 3000) const
	{
		return StoreEntered->Wait(TimeoutMilliseconds);
	}

	bool WaitForStoreReturned(const uint32 TimeoutMilliseconds = 3000) const
	{
		return StoreReturned->Wait(TimeoutMilliseconds);
	}

	int32 Num() const
	{
		return Inner->Num();
	}

	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> GetInner() const
	{
		return Inner;
	}

	TAtomic<int32> LoadCalls{0};
	TAtomic<int32> StoreCalls{0};
	TAtomic<int32> DeleteCalls{0};
	TAtomic<uint64> LastStoreExpectedRevision{0};
	TAtomic<uint64> LastDeleteExpectedRevision{0};
	TAtomic<uint64> ConcurrentWinnerRevision{0};

  private:
	static constexpr int32 NoForcedResult = -1;

	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Inner;
	const FUnrealAISecretStoreCapabilities Capabilities;
	FEvent *LoadEntered = nullptr;
	FEvent *ReleaseLoad = nullptr;
	FEvent *LoadReturned = nullptr;
	FEvent *StoreEntered = nullptr;
	FEvent *ReleaseStore = nullptr;
	FEvent *StoreReturned = nullptr;
	TAtomic<int32> ForcedLoadResult{NoForcedResult};
	TAtomic<int32> ForcedStoreResult{NoForcedResult};
	TAtomic<int32> ForcedDeleteResult{NoForcedResult};
	TAtomic<bool> bBlockNextLoadAfterRead{false};
	TAtomic<bool> bBlockNextStoreAfterCommit{false};
	TAtomic<bool> bReportCancelledAfterCommit{false};
	TAtomic<bool> bInstallConcurrentWinnerAfterCommit{false};
	FCriticalSection FaultMutex;
	TUniquePtr<FUnrealAISecretValue> ConcurrentWinner;
};

class FDeviceOAuthTestDriver final : public IUnrealAIDeviceOAuthAuthorizationDriver
{
  public:
	FDeviceOAuthTestDriver(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
						   const bool bInBlockAuthorize = false, const bool bInIgnoreCancellation = false,
						   const bool bInPublishDuplicateInteraction = false, const bool bInSucceed = true,
						   const bool bInOmitRefreshToken = false, const bool bInOmitAccountRouting = false)
		: Clock(MoveTemp(InClock)), bBlockAuthorize(bInBlockAuthorize), bIgnoreCancellation(bInIgnoreCancellation),
		  bPublishDuplicateInteraction(bInPublishDuplicateInteraction), bSucceed(bInSucceed),
		  bOmitRefreshToken(bInOmitRefreshToken), bOmitAccountRouting(bInOmitAccountRouting),
		  Entered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseAuthorize(FPlatformProcess::GetSynchEventFromPool(true)),
		  Exited(FPlatformProcess::GetSynchEventFromPool(true))
	{
		if (!bBlockAuthorize)
		{
			ReleaseAuthorize->Trigger();
		}
	}

	~FDeviceOAuthTestDriver() override
	{
		ReleaseAuthorize->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseAuthorize);
		FPlatformProcess::ReturnSynchEventToPool(Exited);
	}

	FName GetProviderName() const override
	{
		return DeviceOAuthProviderName;
	}

	bool Authorize(const double, const FUnrealAICancellationToken &Cancellation,
				   IUnrealAIDeviceOAuthInteractionPublisher &InteractionPublisher, FUnrealAIOAuthTokenSet &OutTokens,
				   FUnrealAIProviderAccessError &OutError) override
	{
		++AuthorizeCalls;
		OutTokens.Reset();
		OutError = {};

		FirstInteractionAccepted.Store(PublishInteraction(InteractionPublisher, 1));
		if (bPublishDuplicateInteraction)
		{
			SecondInteractionAccepted.Store(PublishInteraction(InteractionPublisher, 2));
		}
		Entered->Trigger();
		ReleaseAuthorize->Wait(5000);

		if (Cancellation.IsCancellationRequested() && !bIgnoreCancellation)
		{
			OutError = MakeDeviceOAuthError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::AuthCancelled);
			Exited->Trigger();
			return false;
		}
		if (!bSucceed)
		{
			OutError = bHasAuthorizeFailure ? AuthorizeFailure
											: MakeDeviceOAuthError(EUnrealAIErrorCategory::Provider,
																   EUnrealAIProviderAccessErrorCode::AuthFailed, true);
			Exited->Trigger();
			return false;
		}

		FString SecretError;
		if (!MakeDeviceOAuthSecret("fixture-access", OutTokens.AccessToken, SecretError) ||
			(!bOmitRefreshToken && !MakeDeviceOAuthSecret("fixture-refresh", OutTokens.RefreshToken, SecretError)) ||
			(!bOmitAccountRouting &&
			 !MakeDeviceOAuthSecret("fixture-routing", OutTokens.AccountRoutingValue, SecretError)))
		{
			OutTokens.Reset();
			OutError =
				MakeDeviceOAuthError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
			Exited->Trigger();
			return false;
		}
		OutTokens.AccessTokenExpiresAtUtc = Clock->UtcNow() + FTimespan::FromHours(1);
		Exited->Trigger();
		return true;
	}

	bool Refresh(FUnrealAIOAuthTokenEnvelope &, const double, const FUnrealAICancellationToken &,
				 FUnrealAIProviderAccessError &OutError) override
	{
		++RefreshCalls;
		OutError =
			MakeDeviceOAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
		return false;
	}

	void SetAuthorizeFailure(const FUnrealAIProviderAccessError &InFailure)
	{
		AuthorizeFailure = InFailure;
		bHasAuthorizeFailure = true;
	}

	void Release()
	{
		ReleaseAuthorize->Trigger();
	}

	bool WaitUntilEntered(const uint32 TimeoutMilliseconds = 3000) const
	{
		return Entered->Wait(TimeoutMilliseconds);
	}

	bool WaitUntilExited(const uint32 TimeoutMilliseconds = 3000) const
	{
		return Exited->Wait(TimeoutMilliseconds);
	}

	TAtomic<int32> AuthorizeCalls{0};
	TAtomic<int32> RefreshCalls{0};
	TAtomic<bool> FirstInteractionAccepted{false};
	TAtomic<bool> SecondInteractionAccepted{false};

  private:
	bool PublishInteraction(IUnrealAIDeviceOAuthInteractionPublisher &Publisher, const int32 Attempt) const
	{
		FUnrealAIEndpointOrigin Origin;
		FString InteractionError;
		if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://auth.fixture.example"), false, Origin, InteractionError))
		{
			return false;
		}
		FUnrealAIAuthInteraction Interaction;
		if (!FUnrealAIAuthInteraction::TryCreateDeviceCode(
				Origin, FString(TEXT("https://auth.fixture.example/device")),
								Attempt == 1 ? FString(TEXT("FIXTURE-ONE"))
											 : FString(TEXT("FIXTURE-TWO")), Interaction, InteractionError))
		{
			return false;
		}
		return Publisher.PublishInteraction(MoveTemp(Interaction));
	}

	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	bool bBlockAuthorize = false;
	bool bIgnoreCancellation = false;
	bool bPublishDuplicateInteraction = false;
	bool bSucceed = true;
	bool bOmitRefreshToken = false;
	bool bOmitAccountRouting = false;
	bool bHasAuthorizeFailure = false;
	FUnrealAIProviderAccessError AuthorizeFailure;
	FEvent *Entered = nullptr;
	FEvent *ReleaseAuthorize = nullptr;
	FEvent *Exited = nullptr;
};

struct FObservedAuthEvent final
{
	EUnrealAIAuthEventKind Kind = EUnrealAIAuthEventKind::Invalid;
	EUnrealAIAccountAuthState State = EUnrealAIAccountAuthState::Invalid;
	EUnrealAIAuthOperationKind OperationKind = EUnrealAIAuthOperationKind::Invalid;
	EUnrealAIProviderAccessErrorCode ErrorCode = EUnrealAIProviderAccessErrorCode::None;
	bool bHasInteraction = false;
	bool bValidShape = false;
};

class FDeviceOAuthTestAuthSink final : public IUnrealAIAuthEventSink
{
  public:
	void EnqueueAuthEvent(FUnrealAIAuthEvent &&Event) override
	{
		FObservedAuthEvent Observed;
		Observed.Kind = Event.Kind;
		Observed.State = Event.State;
		Observed.OperationKind = Event.OperationKind;
		Observed.ErrorCode = Event.Error.Code;
		Observed.bHasInteraction = Event.Interaction.IsValid();
		FString ShapeError;
		Observed.bValidShape = Event.ValidateShape(ShapeError);
		{
			FScopeLock Lock(&Mutex);
			Events.Add(Observed);
		}
		if (Observed.Kind == EUnrealAIAuthEventKind::InteractionRequired && OnInteraction)
		{
			OnInteraction();
		}
		if ((Observed.Kind == EUnrealAIAuthEventKind::Succeeded || Observed.Kind == EUnrealAIAuthEventKind::Failed ||
			 Observed.Kind == EUnrealAIAuthEventKind::Cancelled || Observed.Kind == EUnrealAIAuthEventKind::TimedOut) &&
			OnTerminal)
		{
			OnTerminal();
		}
	}

	int32 Num() const
	{
		FScopeLock Lock(&Mutex);
		return Events.Num();
	}

	int32 NumTerminals() const
	{
		FScopeLock Lock(&Mutex);
		int32 Count = 0;
		for (const FObservedAuthEvent &Event : Events)
		{
			Count += Event.Kind == EUnrealAIAuthEventKind::Succeeded || Event.Kind == EUnrealAIAuthEventKind::Failed ||
							 Event.Kind == EUnrealAIAuthEventKind::Cancelled ||
							 Event.Kind == EUnrealAIAuthEventKind::TimedOut
						 ? 1
						 : 0;
		}
		return Count;
	}

	FObservedAuthEvent Get(const int32 Index) const
	{
		FScopeLock Lock(&Mutex);
		return Events.IsValidIndex(Index) ? Events[Index] : FObservedAuthEvent{};
	}

	bool WaitForCount(const int32 Count, const double TimeoutSeconds = 3.0) const
	{
		return WaitForDeviceOAuthCondition([this, Count]() { return Num() >= Count; }, TimeoutSeconds);
	}

	TFunction<void()> OnInteraction;
	TFunction<void()> OnTerminal;

  private:
	mutable FCriticalSection Mutex;
	TArray<FObservedAuthEvent> Events;
};

bool StartDeviceOAuthSignIn(FUnrealAIDeviceOAuthAccountProvider &Provider, const uint32 Seed,
							const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> &Sink,
							const FUnrealAICancellationToken &Cancellation,
							TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							FUnrealAIProviderAccessError &OutError, const float TimeoutSeconds = 30.0f)
{
	return FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority::StartSignIn(
		Provider, MakeDeviceOAuthSignInRequest(Seed, TimeoutSeconds), Sink, Cancellation, OutHandle, OutError);
}

bool LoadDeviceOAuthEnvelope(FDeviceOAuthTestStore &Store,
							 const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> &Clock,
							 FUnrealAIOAuthTokenEnvelope &OutEnvelope, uint64 &OutRevision, FString &OutError)
{
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, OutError))
	{
		return false;
	}
	FUnrealAISecretValue Encoded;
	FUnrealAIProviderAccessError StoreError;
	if (Store.Load(Context, DeviceOAuthSecretHandle, Encoded, OutRevision, StoreError) !=
		EUnrealAISecretStoreResult::Succeeded)
	{
		return false;
	}
	const FUnrealAIOAuthTokenEnvelopeBinding Binding{DeviceOAuthProviderName, DeviceOAuthProfileName,
													 DeviceOAuthAccountId};
	return FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(Encoded), Binding, Clock->UtcNow(), OutEnvelope,
													   OutError);
}

bool MakeDeviceOAuthEncodedEnvelope(const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> &Clock,
									const ANSICHAR *AccessToken, const ANSICHAR *RefreshToken,
									const ANSICHAR *RoutingValue, const double LifetimeHours,
									FUnrealAISecretValue &OutEncoded, FString &OutError)
{
	FUnrealAIOAuthTokenSet Tokens;
	if (!MakeDeviceOAuthSecret(AccessToken, Tokens.AccessToken, OutError) ||
		!MakeDeviceOAuthSecret(RefreshToken, Tokens.RefreshToken, OutError) ||
		!MakeDeviceOAuthSecret(RoutingValue, Tokens.AccountRoutingValue, OutError))
	{
		return false;
	}
	Tokens.AccessTokenExpiresAtUtc = Clock->UtcNow() + FTimespan::FromHours(LifetimeHours);
	FUnrealAIOAuthTokenEnvelope Envelope;
	const FUnrealAIOAuthTokenEnvelopeBinding Binding{DeviceOAuthProviderName, DeviceOAuthProfileName,
													 DeviceOAuthAccountId};
	return FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), Clock->UtcNow(), Envelope,
													   OutError) &&
		   FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), Clock->UtcNow(), OutEncoded, OutError);
}

bool StoreDeviceOAuthEnvelope(FDeviceOAuthTestStore &Store,
							  const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> &Clock, FString &OutError)
{
	FUnrealAISecretValue Encoded;
	if (!MakeDeviceOAuthEncodedEnvelope(Clock, "recovery-access", "recovery-refresh", "recovery-routing", 1.0, Encoded,
										OutError))
	{
		return false;
	}
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, OutError))
	{
		return false;
	}
	uint64 Revision = 0;
	FUnrealAIProviderAccessError StoreError;
	return Store.Store(Context, DeviceOAuthSecretHandle, Encoded, 0, Revision, StoreError) ==
			   EUnrealAISecretStoreResult::Succeeded &&
		   Revision == 1;
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
MakeDeviceOAuthConnections(const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> &AccountProvider,
						   FUnrealAICredentialDestination &OutDestination, FString &OutError)
{
	FUnrealAIAccountAuthProviderRegistry AuthProviders;
	FUnrealAIProviderEndpointAuthority Authority;
	if (!AuthProviders.Register(AccountProvider, Authority, OutError))
	{
		FUnrealAIEndpointProfileRegistry EmptyEndpoints;
		FUnrealAIConnectionRegistry EmptyConnections(EmptyEndpoints.CreateSnapshot());
		return EmptyConnections.CreateSnapshot();
	}

	FUnrealAIEndpointOrigin Origin;
	if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://resource.fixture.example"), false, Origin, OutError))
	{
		FUnrealAIEndpointProfileRegistry EmptyEndpoints;
		FUnrealAIConnectionRegistry EmptyConnections(EmptyEndpoints.CreateSnapshot());
		return EmptyConnections.CreateSnapshot();
	}
	const FString Audience(TEXT("https://resource.fixture.example/v1/responses"));
	FUnrealAIEndpointProfileRegistry Endpoints;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	if (!Endpoints.RegisterProviderSubscriptionEndpoint(Authority, DeviceOAuthEndpointName,
														DeviceOAuthModelProviderName, Origin, Audience, 1,
														RegisteredEndpoint, OutError))
	{
		FUnrealAIConnectionRegistry EmptyConnections(Endpoints.CreateSnapshot());
		return EmptyConnections.CreateSnapshot();
	}

	OutDestination.ModelProviderName = DeviceOAuthModelProviderName;
	OutDestination.AccountAuthProviderName = DeviceOAuthProviderName;
	OutDestination.AuthProfileId = DeviceOAuthProfileName;
	OutDestination.AccountId = DeviceOAuthAccountId;
	OutDestination.TenantRealm = TEXT("tests.device_oauth.tenant");
	OutDestination.BillingPrincipalId.Value = FGuid(0x11112222, 0x33334444, 0x55556666, 0x77778888);
	OutDestination.PayerHandle = TEXT("tests.device_oauth.subscription");
	OutDestination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	OutDestination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	OutDestination.EndpointOrigin = Origin;
	OutDestination.Audience = Audience;
	OutDestination.ConnectionRevision = 1;
	OutDestination.EndpointPolicyRevision = 1;

	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = DeviceOAuthConnectionName;
	Connection.EndpointProfileId = DeviceOAuthEndpointName;
	Connection.CredentialDestination = OutDestination;
	FUnrealAIConnectionRegistry Connections(Endpoints.CreateSnapshot());
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> RegisteredConnection;
	Connections.Register(Connection, RegisteredConnection, OutError);
	return Connections.CreateSnapshot();
}

bool ComposeDeviceOAuthConnections(
	const FUnrealAIProviderEndpointAuthority &Authority,
	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections, FString &OutError,
	const FName ConnectionAlias = DeviceOAuthConnectionName)
{
	OutConnections.Reset();
	FUnrealAIEndpointOrigin Origin;
	if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://resource.fixture.example"), false, Origin, OutError))
	{
		return false;
	}
	const FString Audience(TEXT("https://resource.fixture.example/v1/responses"));
	FUnrealAIEndpointProfileRegistry Endpoints;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	if (!Endpoints.RegisterProviderSubscriptionEndpoint(Authority, DeviceOAuthEndpointName,
														DeviceOAuthModelProviderName, Origin, Audience, 1,
														RegisteredEndpoint, OutError))
	{
		return false;
	}
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = ConnectionAlias;
	Connection.EndpointProfileId = DeviceOAuthEndpointName;
	Connection.CredentialDestination.ModelProviderName = DeviceOAuthModelProviderName;
	Connection.CredentialDestination.AccountAuthProviderName = DeviceOAuthProviderName;
	Connection.CredentialDestination.AuthProfileId = DeviceOAuthProfileName;
	Connection.CredentialDestination.AccountId = DeviceOAuthAccountId;
	Connection.CredentialDestination.TenantRealm = TEXT("tests.device_oauth.tenant");
	Connection.CredentialDestination.BillingPrincipalId.Value = FGuid(0x11112222, 0x33334444, 0x55556666, 0x77778888);
	Connection.CredentialDestination.PayerHandle = TEXT("tests.device_oauth.subscription");
	Connection.CredentialDestination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Connection.CredentialDestination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Connection.CredentialDestination.EndpointOrigin = Origin;
	Connection.CredentialDestination.Audience = Audience;
	Connection.CredentialDestination.ConnectionRevision = 1;
	Connection.CredentialDestination.EndpointPolicyRevision = 1;
	FUnrealAIConnectionRegistry Connections(Endpoints.CreateSnapshot());
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> RegisteredConnection;
	if (!Connections.Register(Connection, RegisteredConnection, OutError))
	{
		return false;
	}
	OutConnections = Connections.CreateSnapshot();
	return true;
}

class FDeviceOAuthCredentialSink final : public IUnrealAICredentialResultSink
{
  public:
	void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) override
	{
		FScopeLock Lock(&Mutex);
		LastResult = MakeUnique<FUnrealAICredentialResult>(MoveTemp(Result));
		++Count;
	}

	bool WaitAndTake(FUnrealAICredentialResult &OutResult, const double TimeoutSeconds = 3.0)
	{
		if (!WaitForDeviceOAuthCondition(
				[this]()
				{
					FScopeLock Lock(&Mutex);
					return LastResult.IsValid();
				},
				TimeoutSeconds))
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		OutResult = MoveTemp(*LastResult);
		LastResult.Reset();
		return true;
	}

  private:
	FCriticalSection Mutex;
	TUniquePtr<FUnrealAICredentialResult> LastResult;
	int32 Count = 0;
};

class FDeviceOAuthCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FDeviceOAuthCredentialApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination))
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	int32 ExposedBytes = 0;

  protected:
	bool ApplyCredentialAndDispatch(EUnrealAIAuthScheme, TConstArrayView<uint8> Secret) override
	{
		ExposedBytes += Secret.Num();
		return true;
	}

	bool ApplyCredentialAndProtectedSecondaryAndDispatch(EUnrealAIAuthScheme, TConstArrayView<uint8> Secret,
														 TConstArrayView<uint8> ProtectedSecondary) override
	{
		ExposedBytes += Secret.Num() + ProtectedSecondary.Num();
		return true;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSecretStoreCapabilitiesTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.SecretStoreCapabilitiesFailClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSecretStoreCapabilitiesTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;

	TArray<FUnrealAISecretStoreCapabilities> InvalidCapabilities;
	FUnrealAISecretStoreCapabilities LegacyBooleanOnly;
	LegacyBooleanOnly.bPersistent = true;
	LegacyBooleanOnly.bAtomicCompareAndSwap = true;
	InvalidCapabilities.Add(LegacyBooleanOnly);

	FUnrealAISecretStoreCapabilities InMemoryCapabilities;
	InMemoryCapabilities.PersistenceClass = EUnrealAISecretStorePersistenceClass::Volatile;
	InMemoryCapabilities.ProtectionClass = EUnrealAISecretStoreProtectionClass::ProcessMemory;
	InMemoryCapabilities.ScopeClass = EUnrealAISecretStoreScopeClass::Process;
	InMemoryCapabilities.bAvailableInCurrentBuild = true;
	InMemoryCapabilities.bPersistent = false;
	InMemoryCapabilities.bAtomicCompareAndSwap = true;
	InvalidCapabilities.Add(InMemoryCapabilities);

	for (int32 Index = 0; Index < InvalidCapabilities.Num(); ++Index)
	{
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>(InvalidCapabilities[Index]);
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		TestEqual(*FString::Printf(TEXT("Invalid store %d never advertises resource access"), Index),
								   Provider->DescribeAccess().Availability,
								   EUnrealAIProviderAccessAvailability::Unavailable);

		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestFalse(*FString::Printf(TEXT("Invalid store %d cannot admit sign-in"), Index),
								   StartDeviceOAuthSignIn(*Provider, 90 + Index, Sink, Cancellation.GetToken(), Handle,
														  AccessError));
		TestFalse(*FString::Printf(TEXT("Invalid store %d creates no operation handle"), Index), Handle.IsValid());
		TestEqual(*FString::Printf(TEXT("Invalid store %d performs no authorization"), Index),
								   Driver->AuthorizeCalls.Load(), 0);
		Provider->BeginShutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSignInCommitRecoveryTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.SignInAtomicCommitRecoveryAndOrdering",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSignInCommitRecoveryTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, false, false, true);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Device sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 100, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Device sign-in publishes interaction and terminal"), Sink->WaitForCount(2));
	TestEqual(TEXT("Exactly one terminal is published"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Interaction is delivered first"), Sink->Get(0).Kind, EUnrealAIAuthEventKind::InteractionRequired);
	TestTrue(TEXT("Interaction event has valid shape"), Sink->Get(0).bValidShape);
	TestTrue(TEXT("Interaction remains confined to the interaction event"), Sink->Get(0).bHasInteraction);
	TestEqual(TEXT("Success is delivered second"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Succeeded);
	TestTrue(TEXT("Success event has valid shape"), Sink->Get(1).bValidShape);
	TestFalse(TEXT("Terminal contains no interaction payload"), Sink->Get(1).bHasInteraction);
	TestTrue(TEXT("First interaction publication succeeds"), Driver->FirstInteractionAccepted.Load());
	TestFalse(TEXT("Duplicate interaction publication is rejected"), Driver->SecondInteractionAccepted.Load());
	TestTrue(TEXT("Accepted operation returns its handle"), Handle.IsValid());
	TestTrue(TEXT("Authorization worker exits"), Driver->WaitUntilExited());
	TestTrue(TEXT("Operation is removed after terminal"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetActiveOperationCount() == 0; }));

	const FUnrealAIAccountStatus ReadyStatus = Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId);
	TestEqual(TEXT("Successful atomic commit makes account ready"), ReadyStatus.State,
				   EUnrealAIAccountAuthState::Ready);
	TestTrue(TEXT("Ready status reports refresh material"), ReadyStatus.bRefreshCredentialPresent);
	TestTrue(TEXT("Ready status reports access expiry"), ReadyStatus.AccessExpiresAtUtc.IsSet());
	TestEqual(TEXT("Exactly one secure-store record is committed"), Store->Num(), 1);
	TestEqual(TEXT("New sign-in uses create-if-absent CAS"), Store->LastStoreExpectedRevision.Load(), uint64(0));

	FString Error;
	FUnrealAIOAuthTokenEnvelope Decoded;
	uint64 Revision = 0;
	TestTrue(TEXT("Committed value decodes as one atomic bound envelope"),
				  LoadDeviceOAuthEnvelope(*Store, Clock, Decoded, Revision, Error));
	TestEqual(TEXT("First atomic envelope revision is one"), Revision, uint64(1));
	TestTrue(TEXT("Atomic envelope retains refresh material"), Decoded.HasRefreshToken());
	TestTrue(TEXT("Atomic envelope retains protected account routing"), Decoded.HasAccountRoutingValue());
	TestEqual(TEXT("Atomic envelope retains exact account binding"), Decoded.GetBinding().AccountId,
				   DeviceOAuthAccountId);

	Handle->Cancel();
	Handle->Cancel();
	TestEqual(TEXT("Late duplicate completion attempts publish no additional event"), Sink->Num(), 2);
	TestEqual(TEXT("Late cancel cannot demote a committed ready session"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::Ready);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> DuplicateSignInSink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource DuplicateSignInCancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> DuplicateSignInHandle;
	TestFalse(TEXT("Ready account requires sign-out before another sign-in"),
				   StartDeviceOAuthSignIn(*Provider, 110, DuplicateSignInSink, DuplicateSignInCancellation.GetToken(),
										  DuplicateSignInHandle, AccessError));
	TestEqual(TEXT("Ready sign-in rejection is operation busy"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::OperationBusy);
	TestFalse(TEXT("Ready sign-in rejection creates no handle"), DuplicateSignInHandle.IsValid());
	Provider->BeginShutdown();

	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> RecoveryDriver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> RecoveryProvider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 RecoveryDriver);
	RecoveryProvider->StartStoredSessionRecovery();
	TestTrue(TEXT("Stored atomic envelope recovers the account"),
				  WaitForDeviceOAuthCondition(
					  [&RecoveryProvider]()
					  {
						  return RecoveryProvider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State ==
								 EUnrealAIAccountAuthState::Ready;
					  }));
	const FUnrealAIAccountStatus Recovered = RecoveryProvider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId);
	TestTrue(TEXT("Recovery reports the persisted refresh credential"), Recovered.bRefreshCredentialPresent);
	TestTrue(TEXT("Recovery reports the persisted expiry"), Recovered.AccessExpiresAtUtc.IsSet());
	RecoveryProvider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthTypedDriverFailureTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.TypedDriverFailureSurvivesTerminalBoundary",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthTypedDriverFailureTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, false, false, false, false);
	Driver->SetAuthorizeFailure(MakeDeviceOAuthError(EUnrealAIErrorCategory::NotAuthorized,
													 EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient));
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Device sign-in with a typed driver failure is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 115, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Typed driver failure publishes interaction and terminal"), Sink->WaitForCount(2));
	TestEqual(TEXT("Typed driver failure produces one failed terminal"), Sink->Get(1).Kind,
				   EUnrealAIAuthEventKind::Failed);
	TestEqual(TEXT("The account provider preserves the string-free validation reason"), Sink->Get(1).ErrorCode,
				   EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient);
	TestTrue(TEXT("The preserved typed terminal remains structurally valid"), Sink->Get(1).bValidShape);
	TestEqual(TEXT("A failed driver never commits credential material"), Store->Num(), 0);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthReentrantInteractionShutdownTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.ReentrantInteractionShutdownIsDeadlockFree",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthReentrantInteractionShutdownTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 15.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	TAtomic<int32> ActiveCountSeenByTerminal{-1};
	Sink->OnInteraction = [Provider]() { Provider->BeginShutdown(); };
	Sink->OnTerminal = [Provider, &ActiveCountSeenByTerminal]()
	{ ActiveCountSeenByTerminal.Store(Provider->GetActiveOperationCount()); };

	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Sign-in is admitted before the reentrant shutdown"),
				  StartDeviceOAuthSignIn(*Provider, 150, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Reentrant interaction callback returns and terminal is delivered"), Sink->WaitForCount(2));
	TestEqual(TEXT("Interaction is delivered before the shutdown terminal"), Sink->Get(0).Kind,
				   EUnrealAIAuthEventKind::InteractionRequired);
	TestEqual(TEXT("Reentrant shutdown owns the terminal"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Cancelled);
	TestEqual(TEXT("Reentrant shutdown publishes exactly one terminal"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Internal active state is cleared before the external terminal callback"),
				   ActiveCountSeenByTerminal.Load(), 0);
	TestTrue(TEXT("Authorization worker exits after reentrant shutdown"), Driver->WaitUntilExited());
	TestTrue(TEXT("Physical operation settles after reentrant shutdown"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Reentrant shutdown persists no credential"), Store->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthCancelAtSuccessClaimTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CancelBeforeSuccessClaimCompensatesCommit",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthCancelAtSuccessClaimTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 15.5);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	Store->BlockNextStoreAfterCommit();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	FUnrealAICredentialDestination Destination;
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeDeviceOAuthConnections(Provider, Destination, Error);
	TestEqual(TEXT("Success-claim fixture registers one subscription connection"), Connections->Num(), 1);
	const TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>(Connections, Store, Clock, Driver);
	FUnrealAIOAuthCredentialBinding Binding;
	Binding.ProviderName = DeviceOAuthProviderName;
	Binding.AuthProfileId = DeviceOAuthProfileName;
	Binding.AccountId = DeviceOAuthAccountId;
	Binding.SecretHandle = DeviceOAuthSecretHandle;
	Binding.bRequiresProtectedSecondary = true;
	TestTrue(TEXT("Success-claim fixture registers its OAuth binding"), Broker->RegisterBinding(Binding, Error));
	TestTrue(TEXT("Success-claim fixture connects the account provider to the broker"),
				  Provider->SetCredentialBroker(Broker, Error));
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Success-claim race sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 155, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Sign-in physically commits before returning from the secure store"), Store->WaitForStoreEntered());
	TestEqual(TEXT("Credential is provisionally persisted before success claims"), Store->Num(), 1);
	Store->BlockNextLoadAfterRead();
	Store->ReleaseBlockedStore();
	TestTrue(TEXT("Committed store call returns into broker verification"), Store->WaitForStoreReturned());
	TestTrue(TEXT("Broker verification reads the exact provisional credential"), Store->WaitForLoadEntered());
	if (Handle.IsValid())
	{
		Handle->Cancel();
	}
	TestTrue(TEXT("Cancellation claims the terminal while broker verification is paused"), Sink->WaitForCount(2));
	TestEqual(TEXT("Cancellation wins the success-claim race"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Cancelled);
	Store->ReleaseBlockedLoad();
	TestTrue(TEXT("Broker verification returns after cancellation"), Store->WaitForLoadReturned());
	TestTrue(TEXT("Losing success path compensates its exact persisted revision"),
				  WaitForDeviceOAuthCondition(
					  [&Store, &Provider]() {
						  return Store->DeleteCalls.Load() == 1 && Store->Num() == 0 &&
								 Provider->GetPhysicalOperationCount() == 0;
					  }));
	TestEqual(TEXT("Success-claim compensation deletes revision one"), Store->LastDeleteExpectedRevision.Load(),
				   uint64(1));
	TestEqual(TEXT("Success-claim race publishes exactly one terminal"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Cancelled committed credential remains quarantined rather than ready"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	Provider->BeginShutdown();
	Broker->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthCancelAtFailureClaimTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CancelBeforeFailureClaimPreservesWinningState",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthCancelAtFailureClaimTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 15.75);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, true, true, false, false);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Failure-claim race sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 157, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Failing driver reaches its deterministic physical barrier"), Driver->WaitUntilEntered());
	if (Handle.IsValid())
	{
		Handle->Cancel();
	}
	TestTrue(TEXT("Cancellation claims the terminal while the physical failure is paused"), Sink->WaitForCount(2));
	TestEqual(TEXT("Cancellation wins the failure-claim race"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Cancelled);
	TestEqual(TEXT("Cancelled failure remains physically fenced until the driver returns"),
				   Provider->GetPhysicalOperationCount(), 1);
	Driver->Release();
	TestTrue(TEXT("Cancellation-ignoring failing driver returns"), Driver->WaitUntilExited());
	TestTrue(TEXT("Losing failure path physically settles"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Losing failure path cannot overwrite the cancellation state"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	TestEqual(TEXT("Failure-claim race persists no credential"), Store->Num(), 0);
	TestEqual(TEXT("Failure-claim race publishes exactly one terminal"), Sink->NumTerminals(), 1);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIDeviceOAuthSuccessfulOutputValidationTest,
	"UnrealAI.Auth.DeviceOAuthAccount.SuccessfulDriverOutputRequiresRefreshAndProtectedRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSuccessfulOutputValidationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	struct FCase
	{
		bool bRequiresProtectedSecondary;
		bool bOmitRefreshToken;
		bool bOmitAccountRouting;
		bool bExpectSuccess;
	};
	const TArray<FCase> Cases{
		{true, true, false, false},
		{true, false, true, false},
		{false, false, true, true},
	};

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FCase &Case = Cases[Index];
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 16.0 + Index);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, false, false, false, true,
																	Case.bOmitRefreshToken, Case.bOmitAccountRouting);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(
				MakeDeviceOAuthConfig(Case.bRequiresProtectedSecondary), Store, Clock, Driver);
		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(*FString::Printf(TEXT("Output-validation case %d is admitted"), Index),
								  StartDeviceOAuthSignIn(*Provider, 160 + Index * 10, Sink, Cancellation.GetToken(),
														 Handle, AccessError));
		TestTrue(*FString::Printf(TEXT("Output-validation case %d reaches one terminal"), Index),
								  Sink->WaitForCount(2));
		TestEqual(*FString::Printf(TEXT("Output-validation case %d publishes once"), Index), Sink->NumTerminals(), 1);
		TestEqual(*FString::Printf(TEXT("Output-validation case %d has exact terminal"), Index), Sink->Get(1).Kind,
								   Case.bExpectSuccess ? EUnrealAIAuthEventKind::Succeeded
													   : EUnrealAIAuthEventKind::Failed);
		TestEqual(*FString::Printf(TEXT("Output-validation case %d has exact persisted count"), Index), Store->Num(),
								   Case.bExpectSuccess ? 1 : 0);
		TestEqual(*FString::Printf(TEXT("Output-validation case %d has exact account state"), Index),
								   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
								   Case.bExpectSuccess ? EUnrealAIAccountAuthState::Ready
													   : EUnrealAIAccountAuthState::Failed);
		TestTrue(*FString::Printf(TEXT("Output-validation case %d physically settles"), Index),
								  WaitForDeviceOAuthCondition([&Provider]()
															  { return Provider->GetPhysicalOperationCount() == 0; }));
		Provider->BeginShutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSignInZeroRevisionTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.SignInRejectsSucceededLoadWithZeroRevision",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSignInZeroRevisionTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 19.5);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	Store->ForceNextLoad(EUnrealAISecretStoreResult::Succeeded);
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Zero-revision sign-in fixture is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 195, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Zero-revision sign-in reaches one terminal"), Sink->WaitForCount(2));
	TestEqual(TEXT("Succeeded load with zero revision fails sign-in"), Sink->Get(1).Kind,
				   EUnrealAIAuthEventKind::Failed);
	TestEqual(TEXT("Zero-revision sign-in publishes exactly once"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Zero-revision load is never used as a store precondition"), Store->StoreCalls.Load(), 0);
	TestEqual(TEXT("Zero-revision sign-in persists no credential"), Store->Num(), 0);
	TestEqual(TEXT("Zero-revision sign-in leaves failed state"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::Failed);
	TestTrue(TEXT("Zero-revision sign-in physically settles"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthCancelTimeoutLateTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CancelTimeoutAndLateAuthorizationStayTerminal",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthCancelTimeoutLateTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 20.0);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, true, true);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(TEXT("Blocked sign-in is admitted"),
					  StartDeviceOAuthSignIn(*Provider, 200, Sink, Cancellation.GetToken(), Handle, AccessError));
		TestTrue(TEXT("Blocked driver publishes the interaction"), Sink->WaitForCount(1));
		TestTrue(TEXT("Blocked driver enters authorization"), Driver->WaitUntilEntered());
		Handle->Cancel();
		TestTrue(TEXT("Cancel publishes its terminal"), Sink->WaitForCount(2));
		TestEqual(TEXT("Cancel wins the terminal race"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Cancelled);
		TestEqual(TEXT("Cancel publishes exactly once"), Sink->NumTerminals(), 1);
		Driver->Release();
		TestTrue(TEXT("Cancellation-ignoring driver eventually returns"), Driver->WaitUntilExited());
		TestTrue(
			TEXT("Cancelled authorization worker finishes provider bookkeeping"),
				 WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
		TestTrue(TEXT("Late authorization result creates no store call"),
					  WaitForDeviceOAuthCondition([&Store]() { return Store->StoreCalls.Load() == 0; }));
		TestEqual(TEXT("Cancelled late result commits no envelope"), Store->Num(), 0);
		TestEqual(TEXT("Cancelled late result cannot revive account"),
					   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
					   EUnrealAIAccountAuthState::SignedOut);
		Handle->Cancel();
		TestEqual(TEXT("Repeated cancel publishes no duplicate terminal"), Sink->NumTerminals(), 1);
		Provider->BeginShutdown();
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 30.0);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, true, true);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(TEXT("Timeout fixture sign-in is admitted"),
					  StartDeviceOAuthSignIn(*Provider, 210, Sink, Cancellation.GetToken(), Handle, AccessError, 1.0f));
		TestTrue(TEXT("Timeout fixture driver enters"), Driver->WaitUntilEntered());
		MutableClock->Advance(FTimespan::FromSeconds(2));
		TestTrue(TEXT("Logical deadline publishes timeout"), Sink->WaitForCount(2));
		TestEqual(TEXT("Timeout wins the terminal race"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::TimedOut);
		TestEqual(TEXT("Timeout publishes exactly once"), Sink->NumTerminals(), 1);
		Driver->Release();
		TestTrue(TEXT("Timeout-ignoring driver eventually returns"), Driver->WaitUntilExited());
		TestTrue(
			TEXT("Timed-out authorization worker finishes provider bookkeeping"),
				 WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
		TestEqual(TEXT("Late post-timeout result commits no envelope"), Store->Num(), 0);
		TestEqual(TEXT("Late post-timeout result cannot make account ready"),
					   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
					   EUnrealAIAccountAuthState::Failed);
		Provider->BeginShutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthLateStoreCompensationTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.LateStoreIsCASCompensatedAfterCancel",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthLateStoreCompensationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 40.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	Store->BlockNextStoreAfterCommit();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Late-store sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 300, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Store physically commits before returning"), Store->WaitForStoreEntered());
	TestEqual(TEXT("Physical late store created one provisional record"), Store->Num(), 1);
	TestTrue(TEXT("Driver returned before secure-store call was released"), Driver->WaitUntilExited());
	Handle->Cancel();
	TestTrue(TEXT("Cancellation publishes before the physical store returns"), Sink->WaitForCount(2));
	TestEqual(TEXT("Cancellation owns the terminal"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Cancelled);
	Store->ReleaseBlockedStore();
	TestTrue(TEXT("Blocked store call returns"), Store->WaitForStoreReturned());
	TestTrue(
		TEXT("Fresh-context compensation deletes the stale commit"),
			 WaitForDeviceOAuthCondition([&Store]() { return Store->DeleteCalls.Load() == 1 && Store->Num() == 0; }));
	TestEqual(TEXT("Compensation retains exact CAS revision"), Store->LastDeleteExpectedRevision.Load(), uint64(1));
	TestEqual(TEXT("Late-store cancel publishes exactly one terminal"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Late store cannot revive account"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthCompensationDeleteFaultTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CompensationDeleteFaultQuarantinesRecovery",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthCompensationDeleteFaultTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 42.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	Store->BlockNextStoreAfterCommit();
	Store->ForceNextDelete(EUnrealAISecretStoreResult::Locked);
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Delete-fault compensation sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 325, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Delete-fault fixture commits before returning"), Store->WaitForStoreEntered());
	if (Handle.IsValid())
	{
		Handle->Cancel();
	}
	TestTrue(TEXT("Cancellation wins before compensation"), Sink->WaitForCount(2));
	Store->ReleaseBlockedStore();
	TestTrue(TEXT("Blocked store returns into compensation"), Store->WaitForStoreReturned());
	TestTrue(TEXT("Failed compensation delete physically settles"),
				  WaitForDeviceOAuthCondition(
					  [&Store, &Provider]()
					  { return Store->DeleteCalls.Load() == 1 && Provider->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Locked delete leaves the exact envelope retained"), Store->Num(), 1);
	TestEqual(TEXT("Failed compensation enters local reauthentication quarantine"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);

	const int32 LoadsBeforeRecovery = Store->LoadCalls.Load();
	Provider->StartStoredSessionRecovery();
	TestTrue(TEXT("Recovery inspects the retained envelope and physically settles"),
				  WaitForDeviceOAuthCondition(
					  [&Store, &Provider, LoadsBeforeRecovery]() {
						  return Store->LoadCalls.Load() > LoadsBeforeRecovery &&
								 Provider->GetPhysicalOperationCount() == 0;
					  }));
	TestEqual(TEXT("Recovery cannot revive a compensation-quarantined envelope"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	TestEqual(TEXT("Delete-fault compensation publishes exactly one terminal"), Sink->NumTerminals(), 1);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthAmbiguousLateStoreReconciliationTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.PostCommitCancellationReconcilesObservedRevision",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthAmbiguousLateStoreReconciliationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 45.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	Store->BlockNextStoreAfterCommit();
	Store->ReportCancelledAfterNextCommit();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Ambiguous post-commit sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 350, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Ambiguous store physically commits before return"), Store->WaitForStoreEntered());
	TestEqual(TEXT("Ambiguous store has one provisional record"), Store->Num(), 1);
	Handle->Cancel();
	TestTrue(TEXT("Cancellation wins logical terminal"), Sink->WaitForCount(2));
	Store->ReleaseBlockedStore();
	TestTrue(TEXT("Post-commit store returns logical cancellation"), Store->WaitForStoreReturned());
	TestTrue(TEXT("Fresh reconciliation observes and removes the newer revision"),
				  WaitForDeviceOAuthCondition(
					  [&Store, &Provider]() {
						  return Store->DeleteCalls.Load() == 1 && Store->Num() == 0 &&
								 Provider->GetPhysicalOperationCount() == 0;
					  }));
	TestEqual(TEXT("Ambiguous reconciliation deletes exact observed revision"),
				   Store->LastDeleteExpectedRevision.Load(), uint64(1));
	TestEqual(TEXT("Ambiguous cancellation publishes one terminal"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Ambiguous late commit cannot revive account"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthAmbiguousConcurrentWinnerTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.AmbiguousStorePreservesConcurrentCASWinner",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthAmbiguousConcurrentWinnerTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 47.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	FUnrealAISecretValue ConcurrentWinner;
	TestTrue(TEXT("Concurrent winner fixture encodes a distinct valid envelope"),
				  MakeDeviceOAuthEncodedEnvelope(Clock, "winner-access", "winner-refresh", "winner-routing", 2.0,
												 ConcurrentWinner, Error));
	Store->InstallConcurrentWinnerAndReportCancelledAfterNextCommit(MoveTemp(ConcurrentWinner));
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Concurrent-winner sign-in is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 375, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Ambiguous concurrent-winner result reaches its terminal"), Sink->WaitForCount(2));
	TestEqual(TEXT("Ambiguous result fails rather than claiming ownership"), Sink->Get(1).Kind,
				   EUnrealAIAuthEventKind::Failed);
	TestTrue(TEXT("Ambiguous concurrent-winner worker physically settles"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Concurrent CAS writer advances to revision two"), Store->ConcurrentWinnerRevision.Load(),
				   uint64(2));
	TestEqual(TEXT("Reconciliation never deletes a newer concurrent winner"), Store->DeleteCalls.Load(), 0);
	TestEqual(TEXT("Concurrent winner remains the sole secure-store record"), Store->Num(), 1);

	FUnrealAIOAuthTokenEnvelope Decoded;
	uint64 Revision = 0;
	TestTrue(TEXT("Concurrent winner remains a valid bound envelope"),
				  LoadDeviceOAuthEnvelope(*Store, Clock, Decoded, Revision, Error));
	TestEqual(TEXT("Concurrent winner retains its newer revision"), Revision, uint64(2));
	TestEqual(TEXT("Concurrent winner retains its distinct expiry"), Decoded.GetAccessTokenExpiresAtUtc(),
				   Clock->UtcNow() + FTimespan::FromHours(2));
	TestEqual(TEXT("Ambiguous result publishes exactly one terminal"), Sink->NumTerminals(), 1);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSignOutInvalidationTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.SignOutInvalidatesBeforeCASDelete",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSignOutInvalidationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 50.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);

	FUnrealAICredentialDestination Destination;
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeDeviceOAuthConnections(Provider, Destination, Error);
	TestEqual(TEXT("Subscription connection fixture is registered"), Connections->Num(), 1);
	const TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>(Connections, Store, Clock, Driver);
	FUnrealAIOAuthCredentialBinding Binding;
	Binding.ProviderName = DeviceOAuthProviderName;
	Binding.AuthProfileId = DeviceOAuthProfileName;
	Binding.AccountId = DeviceOAuthAccountId;
	Binding.SecretHandle = DeviceOAuthSecretHandle;
	Binding.bRequiresProtectedSecondary = true;
	TestTrue(TEXT("OAuth broker binding is registered"), Broker->RegisterBinding(Binding, Error));
	TestTrue(TEXT("Account provider is connected to broker"), Provider->SetCredentialBroker(Broker, Error));

	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> SignInSink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource SignInCancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignInHandle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Sign-in before logout is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 400, SignInSink, SignInCancellation.GetToken(), SignInHandle,
										 AccessError));
	TestTrue(TEXT("Sign-in before logout succeeds"), SignInSink->WaitForCount(2));
	TestEqual(TEXT("Sign-in reaches ready"), SignInSink->Get(1).Kind, EUnrealAIAuthEventKind::Succeeded);
	TestTrue(TEXT("Sign-in physical work settles before sign-out admission"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));

	FUnrealAICredentialRequest ResolveRequest;
	ResolveRequest.RequestId.Value = FGuid(0x40000001, 0x40000002, 0x40000003, 0x40000004);
	ResolveRequest.ConnectionAlias = DeviceOAuthConnectionName;
	ResolveRequest.TimeoutSeconds = 30.0f;
	const TSharedRef<FDeviceOAuthCredentialSink, ESPMode::ThreadSafe> CredentialSink =
		MakeShared<FDeviceOAuthCredentialSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource ResolveCancellation;
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> ResolveHandle;
	TestTrue(TEXT("Fresh credential resolve is admitted"),
				  Broker->StartResolve(ResolveRequest, CredentialSink, ResolveCancellation.GetToken(), ResolveHandle,
									   AccessError));
	FUnrealAICredentialResult CredentialResult;
	TestTrue(TEXT("Fresh credential resolve completes"), CredentialSink->WaitAndTake(CredentialResult));
	TestEqual(TEXT("Fresh credential resolve succeeds"), CredentialResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Fresh credential context is valid before sign-out"),
				  CredentialResult.AccessContext.IsValid() && CredentialResult.AccessContext->IsValid());

	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> SignOutSink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource SignOutCancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignOutHandle;
	TestTrue(TEXT("Sign-out is admitted"), FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority::StartSignOut(
											   *Provider, MakeDeviceOAuthSignOutRequest(410), SignOutSink,
											   SignOutCancellation.GetToken(), SignOutHandle, AccessError));
	FDeviceOAuthCredentialApplicator Applicator(Destination);
	TestFalse(TEXT("Sign-out synchronously invalidates outstanding lease before delete"),
				   CredentialResult.AccessContext->TryDispatch(Applicator, Error));
	TestEqual(TEXT("Invalidated sign-out lease exposes no bytes"), Applicator.ExposedBytes, 0);
	TestTrue(TEXT("Sign-out publishes its terminal"), SignOutSink->WaitForCount(1));
	TestEqual(TEXT("Sign-out succeeds"), SignOutSink->Get(0).Kind, EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Sign-out publishes exactly one terminal"), SignOutSink->NumTerminals(), 1);
	TestTrue(TEXT("Sign-out CAS-deletes persisted envelope"),
				  WaitForDeviceOAuthCondition([&Store]() { return Store->Num() == 0; }));
	TestEqual(TEXT("Sign-out deletes exact stored revision"), Store->LastDeleteExpectedRevision.Load(), uint64(1));
	TestEqual(TEXT("Sign-out status is signed out"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	Provider->BeginShutdown();
	Broker->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSignOutLogicalTerminalCleanupTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CancelledTimedOutSignOutStillDeletesAndFences",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSignOutLogicalTerminalCleanupTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	for (const bool bTimeout : {false, true})
	{
		FString Error;
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0),
																bTimeout ? 56.0 : 55.0);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		if (!TestTrue(TEXT("Sign-out cleanup fixture stores an envelope"),
						   StoreDeviceOAuthEnvelope(*Store, Clock, Error)))
		{
			continue;
		}
		Store->BlockNextLoadAfterRead();
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> SignOutSink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource SignOutCancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignOutHandle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(TEXT("Blocking sign-out is admitted"),
					  FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority::StartSignOut(
						  *Provider, MakeDeviceOAuthSignOutRequest(bTimeout ? 461 : 451, bTimeout ? 1.0f : 30.0f),
						  SignOutSink, SignOutCancellation.GetToken(), SignOutHandle, AccessError));
		TestTrue(TEXT("Sign-out reaches the physical secure-store load"), Store->WaitForLoadEntered());
		if (bTimeout)
		{
			MutableClock->Advance(FTimespan::FromSeconds(2));
		}
		else
		{
			SignOutHandle->Cancel();
		}
		TestTrue(TEXT("Logical sign-out terminal publishes while load remains blocked"), SignOutSink->WaitForCount(1));
		TestEqual(TEXT("Logical sign-out terminal has exact kind"), SignOutSink->Get(0).Kind,
					   bTimeout ? EUnrealAIAuthEventKind::TimedOut : EUnrealAIAuthEventKind::Cancelled);
		TestEqual(TEXT("Logical sign-out terminal retains reauthentication state"), SignOutSink->Get(0).State,
					   EUnrealAIAccountAuthState::ReauthenticationRequired);
		TestEqual(TEXT("Logical sign-out terminal publishes once"), SignOutSink->NumTerminals(), 1);
		TestEqual(TEXT("Logical active count is released"), Provider->GetActiveOperationCount(), 0);
		TestEqual(TEXT("Physical sign-out remains fenced"), Provider->GetPhysicalOperationCount(), 1);
		TestEqual(TEXT("Account remains quarantined until physical cleanup"),
					   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
					   EUnrealAIAccountAuthState::ReauthenticationRequired);

		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> OverlapSink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource OverlapCancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> OverlapHandle;
		TestFalse(TEXT("Overlapping sign-in is rejected by the physical fence"),
					   StartDeviceOAuthSignIn(*Provider, bTimeout ? 471 : 461, OverlapSink,
											  OverlapCancellation.GetToken(), OverlapHandle, AccessError));
		TestEqual(TEXT("Physical fence rejection is busy"), AccessError.Code,
					   EUnrealAIProviderAccessErrorCode::OperationBusy);

		Store->ReleaseBlockedLoad();
		TestTrue(TEXT("Independent cleanup deletes after logical terminal"),
			WaitForDeviceOAuthCondition([&Store, &Provider]()
										{ return Store->Num() == 0 && Provider->GetPhysicalOperationCount() == 0; }));
		TestEqual(TEXT("Successful physical cleanup confirms signed-out state"),
					   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
					   EUnrealAIAccountAuthState::SignedOut);
		TestEqual(TEXT("Late cleanup emits no duplicate terminal"), SignOutSink->NumTerminals(), 1);
		Provider->BeginShutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSignOutZeroRevisionTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.SignOutRejectsSucceededLoadWithZeroRevision",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSignOutZeroRevisionTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 57.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	Store->ForceNextLoad(EUnrealAISecretStoreResult::Succeeded);
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(
		TEXT("Zero-revision sign-out fixture is admitted"),
			 FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority::StartSignOut(
				 *Provider, MakeDeviceOAuthSignOutRequest(475), Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Zero-revision load reaches a terminal"), Sink->WaitForCount(1));
	TestEqual(TEXT("Succeeded load with zero revision is a persistence failure"), Sink->Get(0).Kind,
				   EUnrealAIAuthEventKind::Failed);
	TestEqual(TEXT("Zero-revision sign-out publishes exactly once"), Sink->NumTerminals(), 1);
	TestEqual(TEXT("Zero revision is never used for deletion"), Store->DeleteCalls.Load(), 0);
	TestEqual(TEXT("Zero-revision load cannot confirm signed-out state"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	TestTrue(TEXT("Zero-revision sign-out physically settles"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthSignOutDeleteFaultTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.SignOutDeleteFaultQuarantinesRecovery",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthSignOutDeleteFaultTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 57.5);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	TestTrue(TEXT("Sign-out delete-fault fixture stores a valid envelope"),
				  StoreDeviceOAuthEnvelope(*Store, Clock, Error));
	Store->ForceNextDelete(EUnrealAISecretStoreResult::Locked);
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(
		TEXT("Delete-fault sign-out is admitted"),
			 FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority::StartSignOut(
				 *Provider, MakeDeviceOAuthSignOutRequest(477), Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Delete-fault sign-out reaches one terminal"), Sink->WaitForCount(1));
	TestEqual(TEXT("Locked delete fails sign-out"), Sink->Get(0).Kind, EUnrealAIAuthEventKind::Failed);
	TestTrue(TEXT("Delete-fault sign-out physically settles"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Locked sign-out delete retains the envelope"), Store->Num(), 1);
	TestEqual(TEXT("Locked sign-out delete enters local reauthentication quarantine"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);

	const int32 LoadsBeforeRecovery = Store->LoadCalls.Load();
	Provider->StartStoredSessionRecovery();
	TestTrue(TEXT("Post-sign-out recovery inspects the retained envelope and settles"),
				  WaitForDeviceOAuthCondition(
					  [&Store, &Provider, LoadsBeforeRecovery]() {
						  return Store->LoadCalls.Load() > LoadsBeforeRecovery &&
								 Provider->GetPhysicalOperationCount() == 0;
					  }));
	TestEqual(TEXT("Recovery cannot revive a sign-out-quarantined envelope"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	TestEqual(TEXT("Delete-fault sign-out publishes exactly one terminal"), Sink->NumTerminals(), 1);
	Provider->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthReauthenticationCancellationTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.BrokerQuarantineAndCancelledReauthRemainRequired",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthReauthenticationCancellationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 58.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	TestTrue(TEXT("Reauthentication fixture stores an existing session"),
				  StoreDeviceOAuthEnvelope(*Store, Clock, Error));
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, true, true);
	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																			 Driver);
	FUnrealAICredentialDestination Destination;
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeDeviceOAuthConnections(Provider, Destination, Error);
	const TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>(Connections, Store, Clock, Driver);
	FUnrealAIOAuthCredentialBinding Binding;
	Binding.ProviderName = DeviceOAuthProviderName;
	Binding.AuthProfileId = DeviceOAuthProfileName;
	Binding.AccountId = DeviceOAuthAccountId;
	Binding.SecretHandle = DeviceOAuthSecretHandle;
	Binding.bRequiresProtectedSecondary = true;
	TestTrue(TEXT("Reauthentication broker binding registers"), Broker->RegisterBinding(Binding, Error));
	TestTrue(TEXT("Reauthentication provider connects to broker"), Provider->SetCredentialBroker(Broker, Error));
	Provider->StartStoredSessionRecovery();
	TestTrue(TEXT("Existing session initially recovers ready"),
				  WaitForDeviceOAuthCondition(
					  [&Provider]()
					  {
						  return Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State ==
									 EUnrealAIAccountAuthState::Ready &&
								 Provider->GetPhysicalOperationCount() == 0;
					  }));

	Broker->QuarantineAccountForReauthentication(DeviceOAuthProfileName, DeviceOAuthAccountId);
	TestEqual(TEXT("Broker quarantine overlays a previously ready account"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	const FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Explicit reauthentication is admitted"),
				  StartDeviceOAuthSignIn(*Provider, 481, Sink, Cancellation.GetToken(), Handle, AccessError));
	TestTrue(TEXT("Reauthentication publishes its interaction"), Sink->WaitForCount(1));
	TestTrue(TEXT("Reauthentication driver is physically blocked"), Driver->WaitUntilEntered());
	TestEqual(TEXT("Broker quarantine does not hide an active authorizing transition"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::Authorizing);
	Handle->Cancel();
	TestTrue(TEXT("Reauthentication cancellation publishes"), Sink->WaitForCount(2));
	TestEqual(TEXT("Cancelled reauthentication retains required state"), Sink->Get(1).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	TestEqual(TEXT("Account remains reauthentication-required"),
				   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	TestEqual(TEXT("Cancelled reauthentication remains physically fenced"), Provider->GetPhysicalOperationCount(), 1);
	Driver->Release();
	TestTrue(TEXT("Cancelled reauthentication worker exits"), Driver->WaitUntilExited());
	TestTrue(TEXT("Cancelled reauthentication physical fence settles"),
				  WaitForDeviceOAuthCondition([&Provider]() { return Provider->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Cancelled reauthentication never replaces the stored session"), Store->Num(), 1);
	TestEqual(TEXT("Cancelled reauthentication publishes exactly one terminal"), Sink->NumTerminals(), 1);
	Provider->BeginShutdown();
	Broker->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthStoreFaultTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.LoadStoreAndCASFaultsFailOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthStoreFaultTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TArray<EUnrealAISecretStoreResult> Faults{
		EUnrealAISecretStoreResult::Locked, EUnrealAISecretStoreResult::Conflict, EUnrealAISecretStoreResult::Failed};
	for (int32 Index = 0; Index < Faults.Num(); ++Index)
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 60.0 + Index);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		if (Faults[Index] == EUnrealAISecretStoreResult::Locked)
		{
			Store->ForceNextLoad(Faults[Index]);
		}
		else
		{
			Store->ForceNextStore(Faults[Index]);
		}
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(*FString::Printf(TEXT("Fault fixture %d is admitted"), Index),
								  StartDeviceOAuthSignIn(*Provider, 500 + Index * 10, Sink, Cancellation.GetToken(),
														 Handle, AccessError));
		TestTrue(*FString::Printf(TEXT("Fault fixture %d publishes interaction and terminal"), Index),
								  Sink->WaitForCount(2));
		TestEqual(*FString::Printf(TEXT("Fault fixture %d fails"), Index), Sink->Get(1).Kind,
								   EUnrealAIAuthEventKind::Failed);
		TestEqual(*FString::Printf(TEXT("Fault fixture %d retains the safe persistence diagnosis"), Index),
								   Sink->Get(1).ErrorCode, EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
		TestTrue(*FString::Printf(TEXT("Fault fixture %d publishes a valid typed terminal"), Index),
								  Sink->Get(1).bValidShape);
		TestEqual(*FString::Printf(TEXT("Fault fixture %d publishes one terminal"), Index), Sink->NumTerminals(), 1);
		TestEqual(*FString::Printf(TEXT("Fault fixture %d commits no record"), Index), Store->Num(), 0);
		TestEqual(*FString::Printf(TEXT("Fault fixture %d leaves failed status"), Index),
								   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
								   EUnrealAIAccountAuthState::Failed);
		Handle->Cancel();
		TestEqual(*FString::Printf(TEXT("Fault fixture %d rejects duplicate completion"), Index), Sink->NumTerminals(),
								   1);
		Provider->BeginShutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthRecoveryShutdownRaceTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.RecoveryFencesOverlapsAndShutdownCannotRevive",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthRecoveryShutdownRaceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 70.0);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		TestTrue(TEXT("Stale-recovery fixture envelope is stored"), StoreDeviceOAuthEnvelope(*Store, Clock, Error));
		Store->BlockNextLoadAfterRead();
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		Provider->StartStoredSessionRecovery();
		TestTrue(TEXT("Stored-session recovery blocks after reading the valid envelope"), Store->WaitForLoadEntered());
		TestEqual(TEXT("In-flight recovery exposes refreshing state"),
					   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
					   EUnrealAIAccountAuthState::Refreshing);
		TestEqual(TEXT("In-flight recovery never advertises resource access"), Provider->DescribeAccess().Availability,
					   EUnrealAIProviderAccessAvailability::Unavailable);
		TestEqual(TEXT("In-flight recovery owns one physical-operation fence"), Provider->GetPhysicalOperationCount(),
					   1);

		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestFalse(TEXT("New sign-in cannot supersede an in-flight recovery"),
					   StartDeviceOAuthSignIn(*Provider, 600, Sink, Cancellation.GetToken(), Handle, AccessError));
		TestEqual(TEXT("Recovery overlap rejection is busy"), AccessError.Code,
					   EUnrealAIProviderAccessErrorCode::OperationBusy);
		TestFalse(TEXT("Recovery overlap rejection creates no handle"), Handle.IsValid());
		Store->ReleaseBlockedLoad();
		TestTrue(TEXT("Late recovery load returns"), Store->WaitForLoadReturned());
		TestTrue(TEXT("Recovery commits the stored ready session before releasing its fence"),
					  WaitForDeviceOAuthCondition(
						  [&Provider]()
						  {
							  return Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State ==
										 EUnrealAIAccountAuthState::Ready &&
									 Provider->GetPhysicalOperationCount() == 0;
						  }));
		TestEqual(TEXT("Rejected overlap never invokes the authorization driver"), Driver->AuthorizeCalls.Load(), 0);
		TestEqual(TEXT("Recovery emits no interactive auth event"), Sink->Num(), 0);
		Provider->BeginShutdown();
	}

	{
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 80.0);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
			MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
		const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
			MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock, true, true);
		const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
			MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(MakeDeviceOAuthConfig(), Store, Clock,
																				 Driver);
		const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
		const FUnrealAICancellationSource Cancellation;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(TEXT("Shutdown-race sign-in is admitted"),
					  StartDeviceOAuthSignIn(*Provider, 610, Sink, Cancellation.GetToken(), Handle, AccessError));
		TestTrue(TEXT("Shutdown-race driver enters"), Driver->WaitUntilEntered());
		Provider->BeginShutdown();
		TestTrue(TEXT("Shutdown publishes cancellation terminal"), Sink->WaitForCount(2));
		TestEqual(TEXT("Shutdown cancellation wins"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Cancelled);
		Driver->Release();
		TestTrue(TEXT("Shutdown-ignoring driver exits"), Driver->WaitUntilExited());
		Handle->Cancel();
		TestEqual(TEXT("Shutdown and late completion publish exactly once"), Sink->NumTerminals(), 1);
		TestEqual(TEXT("Shutdown commits no token envelope"), Store->Num(), 0);
		TestEqual(TEXT("Shutdown leaves provider unavailable"), Provider->DescribeAccess().Availability,
					   EUnrealAIProviderAccessAvailability::Unavailable);
		TestEqual(TEXT("Shutdown cannot revive account"),
					   Provider->GetStatus(DeviceOAuthProfileName, DeviceOAuthAccountId).State,
					   EUnrealAIAccountAuthState::SignedOut);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthCompositionRootTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CompositionRootRegistersCatalogAndWithdrawsExactly",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthCompositionRootTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 31, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	FUnrealAIAccountAuthProviderRegistry Providers;
	FUnrealAIAccountCatalogRegistry Catalog(Providers);
	TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> Root;
	FUnrealAIProviderEndpointAuthority Authority;
	FUnrealAIProviderAccessError Error;
	auto Composer = [](const FUnrealAIProviderEndpointAuthority &MintedAuthority,
					   TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
					   FString &OutError)
	{ return ComposeDeviceOAuthConnections(MintedAuthority, OutConnections, OutError); };
	TestTrue(TEXT("Complete device vertical composes"),
				  FUnrealAIDeviceOAuthCompositionRoot::TryCreateAndRegisterWithStore(
					  MakeDeviceOAuthCompositionConfig(), Providers, Catalog, Composer, Driver, Store, Clock, Root,
					  Authority, Error));
	if (!Root.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Composition owns one provider registration"), Providers.Num(), 1);
	TestEqual(TEXT("Composition owns one account-catalog registration"), Catalog.Num(), 1);
	TestTrue(TEXT("Composition returns provider endpoint authority"), Authority.IsValid());

	const TSharedRef<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FDeviceOAuthTestAuthSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	TestTrue(
		TEXT("Composed provider admits device sign-in"),
			 StartDeviceOAuthSignIn(*Root->GetAccountProvider(), 700, Sink, Cancellation.GetToken(), Handle, Error));
	TestTrue(TEXT("Composed provider reaches interaction plus terminal"), Sink->WaitForCount(2));
	TestEqual(TEXT("Composed sign-in succeeds"), Sink->Get(1).Kind, EUnrealAIAuthEventKind::Succeeded);
	FUnrealAIAccountCatalogView Selection;
	FUnrealAIProviderAccessError SelectionError;
	const TSharedRef<const FUnrealAIAccountCatalogSnapshot, ESPMode::ThreadSafe> StaleSnapshot =
		Catalog.CreateSnapshot();
	TestTrue(TEXT("Ready device account resolves by exact alias"),
				  StaleSnapshot->ResolveReadyExact(DeviceOAuthSelectionName, Selection, SelectionError));
	TestEqual(TEXT("Catalog exposes exact subscription billing"), Selection.Destination.BillingMode,
				   EUnrealAIBillingMode::SubscriptionQuota);

	Root->BeginShutdown();
	TestEqual(TEXT("Shutdown withdraws provider registration"), Providers.Num(), 0);
	TestEqual(TEXT("Shutdown withdraws catalog registration"), Catalog.Num(), 0);
	TestFalse(TEXT("Pre-shutdown catalog snapshot is inert"),
				   StaleSnapshot->ResolveReadyExact(DeviceOAuthSelectionName, Selection, SelectionError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIDeviceOAuthCompositionRollbackTest,
								 "UnrealAI.Auth.DeviceOAuthAccount.CompositionRootRollsBackIncompleteConnection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDeviceOAuthCompositionRollbackTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 31, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FDeviceOAuthTestStore, ESPMode::ThreadSafe> Store =
		MakeShared<FDeviceOAuthTestStore, ESPMode::ThreadSafe>();
	const TSharedRef<FDeviceOAuthTestDriver, ESPMode::ThreadSafe> Driver =
		MakeShared<FDeviceOAuthTestDriver, ESPMode::ThreadSafe>(Clock);
	FUnrealAIAccountAuthProviderRegistry Providers;
	FUnrealAIAccountCatalogRegistry Catalog(Providers);
	TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> Root;
	FUnrealAIProviderEndpointAuthority Authority;
	FUnrealAIProviderAccessError Error;
	auto WrongComposer = [](const FUnrealAIProviderEndpointAuthority &MintedAuthority,
							TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
							FString &OutError)
	{
		return ComposeDeviceOAuthConnections(MintedAuthority, OutConnections, OutError,
											 TEXT("tests.device_oauth.connection.wrong"));
	};
	TestFalse(TEXT("Missing exact catalog connection fails composition"),
				   FUnrealAIDeviceOAuthCompositionRoot::TryCreateAndRegisterWithStore(
					   MakeDeviceOAuthCompositionConfig(), Providers, Catalog, WrongComposer, Driver, Store, Clock,
					   Root, Authority, Error));
	TestFalse(TEXT("Failed composition returns no root"), Root.IsValid());
	TestFalse(TEXT("Failed composition returns no authority"), Authority.IsValid());
	TestEqual(TEXT("Failed composition rolls provider registration back"), Providers.Num(), 0);
	TestEqual(TEXT("Failed composition publishes no catalog entry"), Catalog.Num(), 0);
	return true;
}

#endif // defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS
