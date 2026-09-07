// Copyright EngineWorks. All Rights Reserved.

#include "Subscriptions/UnrealAIAccountsEditor.h"

#include "UnrealAIAuth.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UnrealAIAccountsEditor"

/**
 * The only constructor authority for the common local-auth gesture. Instances are private to this Editor module and
 * are invoked only from local Slate button callbacks for exact catalog entries.
 */
class FUnrealAIDirectSubscriptionEditorGestureAuthority final
{
  public:
	explicit FUnrealAIDirectSubscriptionEditorGestureAuthority(const FName InProviderName, const FName InSelectionAlias)
		: ProviderName(InProviderName), SelectionAlias(InSelectionAlias)
	{
	}

	bool IsAccountRuntimeReady() const
	{
		FUnrealAIAccountCatalogView View;
		return GetProvider().IsValid() && TryGetCatalogView(View) && View.bBindingCurrent;
	}

	bool IsResourceRuntimeReady() const
	{
		FUnrealAIAccountCatalogView Selection;
		FUnrealAIProviderAccessError Error;
		return IUnrealAIAuthModule::IsAvailable() &&
			   IUnrealAIAuthModule::Get().GetAccountCatalogSnapshot()->ResolveReadyExact(SelectionAlias, Selection,
																						 Error);
	}

	EUnrealAIInteractiveAuthFlow GetInteractiveFlow() const
	{
		const TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider = GetProvider();
		if (!Provider.IsValid())
		{
			return EUnrealAIInteractiveAuthFlow::Invalid;
		}
		const FUnrealAIAccountAuthCapabilities Capabilities = Provider->DescribeCapabilities();
		if (Capabilities.bBrowserPkce)
		{
			return EUnrealAIInteractiveAuthFlow::BrowserPkce;
		}
		return Capabilities.bDeviceCode ? EUnrealAIInteractiveAuthFlow::DeviceCode
										: EUnrealAIInteractiveAuthFlow::Invalid;
	}

	FUnrealAIAccountStatus GetStatus(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) const
	{
		const TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider = GetProvider();
		return Provider.IsValid() ? Provider->GetStatus(AuthProfileId, AccountId) : FUnrealAIAccountStatus{};
	}

	bool StartSignIn(const FUnrealAIInteractiveAuthRequest &Request,
					 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
					 const FUnrealAICancellationToken &Cancellation,
					 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError) const
	{
		const TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider = GetProvider();
		if (!Provider.IsValid())
		{
			return RejectUnavailable(OutHandle, OutError);
		}
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider->StartSignIn(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}

	bool StartSignOut(const FUnrealAIAccountAuthRequest &Request,
					  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) const
	{
		const TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider = GetProvider();
		if (!Provider.IsValid())
		{
			return RejectUnavailable(OutHandle, OutError);
		}
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider->StartSignOut(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}

  private:
	TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> GetProvider() const
	{
		return IUnrealAIAuthModule::IsAvailable()
				   ? IUnrealAIAuthModule::Get().GetAccountAuthProviderRegistry().Find(ProviderName)
				   : nullptr;
	}

	bool TryGetCatalogView(FUnrealAIAccountCatalogView &OutView) const
	{
		OutView = {};
		if (!IUnrealAIAuthModule::IsAvailable())
		{
			return false;
		}
		for (const FUnrealAIAccountCatalogView &View : IUnrealAIAuthModule::Get().GetAccountCatalogSnapshot()->List())
		{
			if (View.Entry.SelectionAlias == SelectionAlias && View.Entry.ProviderName == ProviderName)
			{
				OutView = View;
				return true;
			}
		}
		return false;
	}

	static bool RejectUnavailable(TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
								  FUnrealAIProviderAccessError &OutError)
	{
		OutHandle.Reset();
		OutError.Category = EUnrealAIErrorCategory::InvalidConfiguration;
		OutError.Code = EUnrealAIProviderAccessErrorCode::InvalidConfiguration;
		return false;
	}

	FName ProviderName;
	FName SelectionAlias;
};

namespace
{
constexpr float SignInTimeoutSeconds = 15.0f * 60.0f;
constexpr float SignOutTimeoutSeconds = 30.0f;
constexpr int32 MaxQueuedAuthEvents = 16;

void SecureResetString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

FText StateText(const EUnrealAIAccountAuthState State)
{
	switch (State)
	{
	case EUnrealAIAccountAuthState::SignedOut:
		return LOCTEXT("AccountSignedOut", "Signed out");
	case EUnrealAIAccountAuthState::Authorizing:
		return LOCTEXT("AccountAuthorizing", "Waiting for authorization");
	case EUnrealAIAccountAuthState::Ready:
		return LOCTEXT("AccountReady", "Signed in; subscription route ready");
	case EUnrealAIAccountAuthState::Refreshing:
		return LOCTEXT("AccountRefreshing", "Refreshing credentials");
	case EUnrealAIAccountAuthState::Revoking:
		return LOCTEXT("AccountRevoking", "Signing out");
	case EUnrealAIAccountAuthState::ReauthenticationRequired:
		return LOCTEXT("AccountReauth", "Sign-in is required again");
	case EUnrealAIAccountAuthState::Failed:
		return LOCTEXT("AccountFailed", "The account operation failed");
	case EUnrealAIAccountAuthState::Invalid:
	default:
		return LOCTEXT("AccountUnavailable", "Unavailable");
	}
}

FText AuthenticationFailureText(const FUnrealAIProviderAccessError &Error)
{
	switch (Error.Code)
	{
	case EUnrealAIProviderAccessErrorCode::AuthResponseInvalid:
		return LOCTEXT("AuthResponseInvalid",
					   "Authentication failed: the provider returned a token response that could not be validated.");
	case EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete:
		return LOCTEXT(
			"AuthCredentialIncomplete",
			"Authentication failed: the provider did not issue all credentials required for a renewable session.");
	case EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient:
		return LOCTEXT("AuthScopeInsufficient",
					   "Authentication completed, but the account did not grant the required subscription API scopes.");
	case EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported:
		return LOCTEXT("AuthTokenTypeUnsupported",
					   "Authentication failed: the provider returned an unsupported token type.");
	case EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid:
		return LOCTEXT("AuthExpiryInvalid",
					   "Authentication failed: the provider returned an invalid or unsupported token lifetime.");
	case EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed:
		return LOCTEXT("AuthPersistenceFailed",
					   "Authentication succeeded, but the credential could not be stored in the local secure store.");
	case EUnrealAIProviderAccessErrorCode::AccessProfileNotReady:
		return LOCTEXT("AuthAccessProfileNotReady",
					   "Authentication was not authorized for this account or subscription.");
	case EUnrealAIProviderAccessErrorCode::PartnerGated:
		return LOCTEXT("AuthPartnerGated", "Authentication requires provider or partner access that is not enabled.");
	case EUnrealAIProviderAccessErrorCode::UnsupportedCapability:
		return LOCTEXT("AuthUnsupportedCapability", "This authentication capability is not supported.");
	case EUnrealAIProviderAccessErrorCode::InvalidConfiguration:
		return LOCTEXT("AuthInvalidConfiguration",
					   "Authentication is unavailable because its configuration is invalid.");
	case EUnrealAIProviderAccessErrorCode::AuthFailed:
	default:
		return LOCTEXT("AuthFailed", "Authentication failed. No provider response details were retained.");
	}
}

class FSubscriptionAuthMailbox final : public IUnrealAIAuthEventSink
{
  public:
	bool TryReserveRequest(const FUnrealAIRequestId &RequestId, const FName AuthProfileId,
						   const FUnrealAIAccessAccountId &AccountId, const EUnrealAIAuthOperationKind OperationKind)
	{
		if (!RequestId.IsValid() || AuthProfileId.IsNone() || !AccountId.IsValid() ||
			(OperationKind != EUnrealAIAuthOperationKind::SignIn &&
			 OperationKind != EUnrealAIAuthOperationKind::SignOut))
		{
			return false;
		}

		FScopeLock Lock(&Mutex);
		if (FindReservationLocked(RequestId) != nullptr || Reservations.Num() >= MaxQueuedAuthEvents)
		{
			return false;
		}

		const int32 RequiredProgressEvictions =
			FMath::Max(0, Events.Num() + CountPendingTerminalReservationsLocked() + 1 - MaxQueuedAuthEvents);
		if (RequiredProgressEvictions > CountProgressEventsLocked())
		{
			return false;
		}

		FRequestReservation &Reservation = Reservations.AddDefaulted_GetRef();
		Reservation.RequestId = RequestId;
		Reservation.AuthProfileId = AuthProfileId;
		Reservation.AccountId = AccountId;
		Reservation.OperationKind = OperationKind;
		for (int32 EvictionIndex = 0; EvictionIndex < RequiredProgressEvictions; ++EvictionIndex)
		{
			RemoveOldestProgressEventLocked();
		}
		return true;
	}

	void RetireRequest(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		for (int32 Index = Reservations.Num() - 1; Index >= 0; --Index)
		{
			if (Reservations[Index].RequestId == RequestId)
			{
				Reservations.RemoveAt(Index, 1, EAllowShrinking::No);
			}
		}
		Events.RemoveAll([&RequestId](const FUnrealAIAuthEvent &Event) { return Event.RequestId == RequestId; });
	}

	void EnqueueAuthEvent(FUnrealAIAuthEvent &&Event) override
	{
		FString ShapeError;
		if (!Event.ValidateShape(ShapeError))
		{
			return;
		}
		FScopeLock Lock(&Mutex);
		FRequestReservation *Reservation = FindReservationLocked(Event);
		if (Reservation == nullptr || Reservation->bTerminalAccepted)
		{
			return;
		}

		if (Event.IsTerminal())
		{
			if (Events.Num() >= MaxQueuedAuthEvents)
			{
				if (CountProgressEventsLocked() == 0)
				{
					return;
				}
				RemoveOldestProgressEventLocked();
			}
			Reservation->bTerminalAccepted = true;
			Events.Add(MoveTemp(Event));
			return;
		}

		const int32 ProgressCapacity = MaxQueuedAuthEvents - CountPendingTerminalReservationsLocked();
		if (Events.Num() >= ProgressCapacity)
		{
			if (CountProgressEventsLocked() == 0)
			{
				return;
			}
			RemoveOldestProgressEventLocked();
		}
		Events.Add(MoveTemp(Event));
	}

	void Drain(TArray<FUnrealAIAuthEvent> &OutEvents)
	{
		FScopeLock Lock(&Mutex);
		OutEvents = MoveTemp(Events);
		Events.Reset();
	}

  private:
	struct FRequestReservation final
	{
		FUnrealAIRequestId RequestId;
		FName AuthProfileId;
		FUnrealAIAccessAccountId AccountId;
		EUnrealAIAuthOperationKind OperationKind = EUnrealAIAuthOperationKind::Invalid;
		bool bTerminalAccepted = false;

		bool Matches(const FUnrealAIAuthEvent &Event) const
		{
			return RequestId == Event.RequestId && AuthProfileId == Event.AuthProfileId &&
				   AccountId == Event.AccountId && OperationKind == Event.OperationKind;
		}
	};

	FRequestReservation *FindReservationLocked(const FUnrealAIRequestId &RequestId)
	{
		for (FRequestReservation &Reservation : Reservations)
		{
			if (Reservation.RequestId == RequestId)
			{
				return &Reservation;
			}
		}
		return nullptr;
	}

	FRequestReservation *FindReservationLocked(const FUnrealAIAuthEvent &Event)
	{
		for (FRequestReservation &Reservation : Reservations)
		{
			if (Reservation.Matches(Event))
			{
				return &Reservation;
			}
		}
		return nullptr;
	}

	int32 CountPendingTerminalReservationsLocked() const
	{
		int32 Count = 0;
		for (const FRequestReservation &Reservation : Reservations)
		{
			Count += Reservation.bTerminalAccepted ? 0 : 1;
		}
		return Count;
	}

	int32 CountProgressEventsLocked() const
	{
		int32 Count = 0;
		for (const FUnrealAIAuthEvent &Event : Events)
		{
			Count += Event.IsTerminal() ? 0 : 1;
		}
		return Count;
	}

	void RemoveOldestProgressEventLocked()
	{
		for (int32 Index = 0; Index < Events.Num(); ++Index)
		{
			if (!Events[Index].IsTerminal())
			{
				Events.RemoveAt(Index, 1, EAllowShrinking::No);
				return;
			}
		}
	}

	FCriticalSection Mutex;
	TArray<FUnrealAIAuthEvent> Events;
	TArray<FRequestReservation> Reservations;
};

struct FSubscriptionProviderUiState final
{
	FText DisplayName;
	FName SelectionAlias;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAICredentialDestination Destination;
	EUnrealAIInteractiveAuthFlow AuthFlow = EUnrealAIInteractiveAuthFlow::Invalid;
	TSharedPtr<FUnrealAIDirectSubscriptionEditorGestureAuthority> Authority;
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Operation;
	FUnrealAICancellationSource Cancellation;
	TOptional<FUnrealAIRequestId> ActiveRequestId;
	FString VerificationUri;
	FString UserCode;
	FText LastOutcome;

	bool IsAvailable() const
	{
		return Authority.IsValid() && Authority->IsAccountRuntimeReady();
	}

	bool IsBusy() const
	{
		return ActiveRequestId.IsSet();
	}

	bool CanOpenVerificationPage() const
	{
		return IsBusy() && !VerificationUri.IsEmpty();
	}

	bool MatchesActiveEvent(const FUnrealAIAuthEvent &Event) const
	{
		return ActiveRequestId.IsSet() && ActiveRequestId.GetValue() == Event.RequestId &&
			   AuthProfileId == Event.AuthProfileId && AccountId == Event.AccountId;
	}

	FUnrealAIAccountStatus GetStatus() const
	{
		return Authority.IsValid() ? Authority->GetStatus(AuthProfileId, AccountId) : FUnrealAIAccountStatus{};
	}

	void ClearInteraction()
	{
		SecureResetString(UserCode);
		VerificationUri.Reset();
	}

	void CancelAndReset(const EUnrealAICancellationReason Reason)
	{
		Cancellation.Cancel(Reason);
		if (Operation.IsValid())
		{
			Operation->Cancel();
		}
		Operation.Reset();
		ActiveRequestId.Reset();
		ClearInteraction();
	}
};

bool IsSignInGestureEnabled(const EUnrealAIAccountAuthState State, const bool bAccountRuntimeReady, const bool bBusy,
							const bool bShuttingDown)
{
	if (bShuttingDown || !bAccountRuntimeReady || bBusy)
	{
		return false;
	}
	return State == EUnrealAIAccountAuthState::SignedOut ||
		   State == EUnrealAIAccountAuthState::ReauthenticationRequired || State == EUnrealAIAccountAuthState::Failed;
}

bool IsSignOutGestureEnabled(const EUnrealAIAccountAuthState State, const bool bAccountRuntimeReady, const bool bBusy,
							 const bool bShuttingDown)
{
	if (bShuttingDown || !bAccountRuntimeReady || bBusy)
	{
		return false;
	}
	return State == EUnrealAIAccountAuthState::Ready || State == EUnrealAIAccountAuthState::ReauthenticationRequired ||
		   State == EUnrealAIAccountAuthState::Failed;
}

class SAgentSubscriptionAccountsPanel final : public SCompoundWidget
{
  public:
	SLATE_BEGIN_ARGS(SAgentSubscriptionAccountsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments &)
	{
		Mailbox = MakeShared<FSubscriptionAuthMailbox, ESPMode::ThreadSafe>();
		LoadProviders();

		TSharedRef<SVerticalBox> ProviderRows = SNew(SVerticalBox);
		for (int32 Index = 0; Index < Providers.Num(); ++Index)
		{
			ProviderRows->AddSlot().AutoHeight().Padding(0.0f, 8.0f)[BuildProviderRow(Index)];
		}

		ChildSlot[SNew(SBorder).Padding(
			18.0f)[SNew(SVerticalBox) +
				   SVerticalBox::Slot().AutoHeight()[SNew(STextBlock)
														 .Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
														 .Text(LOCTEXT("SubscriptionHeading",
																	   "Experimental AI Subscription Accounts"))] +
				   SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 8.0f)
					   [SNew(STextBlock)
							.AutoWrapText(true)
							.Text(LOCTEXT("SubscriptionDisclosure",
										  "These native, in-process compatibility routes use the selected platform "
										  "secure store and reviewed browser-PKCE or device authorization. They do not "
										  "launch or require a CLI. They are experimental, Development/Editor-only, "
										  "and are not provider-approved integrations. Authentication does not "
										  "guarantee subscription entitlement, model availability, quota, billing "
										  "treatment, or protocol stability. They never fall back to metered API-key "
										  "billing."))] +
				   SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)[SNew(SSeparator)] +
				   SVerticalBox::Slot().AutoHeight()[ProviderRows]]];
	}

	~SAgentSubscriptionAccountsPanel() override
	{
		Shutdown();
	}

	void Tick(const FGeometry &AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		if (!Mailbox.IsValid())
		{
			return;
		}
		TArray<FUnrealAIAuthEvent> Events;
		Mailbox->Drain(Events);
		for (FUnrealAIAuthEvent &Event : Events)
		{
			const bool bTerminal = Event.IsTerminal();
			FSubscriptionProviderUiState *State = FindProvider(Event.AuthProfileId, Event.AccountId);
			if (State == nullptr || !State->MatchesActiveEvent(Event))
			{
				if (bTerminal)
				{
					Mailbox->RetireRequest(Event.RequestId);
				}
				continue;
			}
			if (Event.Kind == EUnrealAIAuthEventKind::InteractionRequired && Event.Interaction.IsValid())
			{
				State->ClearInteraction();
				State->VerificationUri = Event.Interaction->GetLaunchUri();
				State->UserCode = FString(Event.Interaction->GetUserCode());
				State->LastOutcome = LOCTEXT("DeviceInstructionReady",
											 "Open the verified provider page and enter the code shown below.");
			}
			if (bTerminal)
			{
				switch (Event.Kind)
				{
				case EUnrealAIAuthEventKind::Succeeded:
					State->LastOutcome =
						Event.OperationKind == EUnrealAIAuthOperationKind::SignOut
							? LOCTEXT("SignOutSucceeded", "Signed out and removed the local secure-store session.")
							: LOCTEXT("SignInSucceeded", "Authentication completed and was stored securely.");
					break;
				case EUnrealAIAuthEventKind::Cancelled:
					State->LastOutcome = LOCTEXT("AuthCancelled", "The account operation was cancelled.");
					break;
				case EUnrealAIAuthEventKind::TimedOut:
					State->LastOutcome = LOCTEXT("AuthTimedOut", "The account operation timed out.");
					break;
				case EUnrealAIAuthEventKind::Failed:
				default:
					State->LastOutcome = AuthenticationFailureText(Event.Error);
					break;
				}
				State->Operation.Reset();
				State->ActiveRequestId.Reset();
				State->ClearInteraction();
				Mailbox->RetireRequest(Event.RequestId);
			}
		}
	}

  private:
	void LoadProviders()
	{
		if (!IUnrealAIAuthModule::IsAvailable())
		{
			return;
		}
		for (const FUnrealAIAccountCatalogView &View : IUnrealAIAuthModule::Get().GetAccountCatalogSnapshot()->List())
		{
			FSubscriptionProviderUiState State;
			State.DisplayName = FText::FromString(View.Entry.DisplayLabel);
			State.SelectionAlias = View.Entry.SelectionAlias;
			State.AuthProfileId = View.Entry.AuthProfileId;
			State.AccountId = View.Entry.AccountId;
			State.Destination = View.Destination;
			State.Authority = MakeShared<FUnrealAIDirectSubscriptionEditorGestureAuthority>(View.Entry.ProviderName,
																							View.Entry.SelectionAlias);
			State.AuthFlow = State.Authority->GetInteractiveFlow();
			Providers.Add(MoveTemp(State));
		}
	}

	TSharedRef<SWidget> BuildProviderRow(const int32 Index)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(12.0f)
				[SNew(SVerticalBox) +
				 SVerticalBox::Slot()
					 .AutoHeight()[SNew(STextBlock)
									   .Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
									   .Text_Lambda([this, Index]() { return Providers[Index].DisplayName; })] +
				 SVerticalBox::Slot().AutoHeight().Padding(
					 0.0f, 5.0f)[SNew(STextBlock).Text_Lambda([this, Index]() { return GetStatusText(Index); })] +
				 SVerticalBox::Slot().AutoHeight().Padding(
					 0.0f, 3.0f)[SNew(STextBlock)
									 .AutoWrapText(true)
									 .Text_Lambda([this, Index]() { return GetDestinationDisclosureText(Index); })] +
				 SVerticalBox::Slot().AutoHeight().Padding(
					 0.0f, 3.0f)[SNew(STextBlock)
									 .AutoWrapText(true)
									 .Text_Lambda([this, Index]() { return Providers[Index].LastOutcome; })] +
				 SVerticalBox::Slot().AutoHeight().Padding(
					 0.0f, 6.0f)[SNew(STextBlock)
									 .Visibility_Lambda(
										 [this, Index]() {
											 return Providers[Index].UserCode.IsEmpty() ? EVisibility::Collapsed
																						: EVisibility::Visible;
										 })
									 .Font(FAppStyle::GetFontStyle("Mono"))
									 .Text_Lambda(
										 [this, Index]()
										 {
											 return FText::Format(LOCTEXT("DeviceCodeFormat", "Device code: {0}"),
																  FText::FromString(Providers[Index].UserCode));
										 })] +
				 SVerticalBox::Slot().AutoHeight().Padding(
					 0.0f, 8.0f, 0.0f,
					 0.0f)[SNew(SHorizontalBox) +
						   SHorizontalBox::Slot()
							   .AutoWidth()[SNew(SButton)
												.Text(LOCTEXT("SignIn", "Sign In"))
												.IsEnabled_Lambda([this, Index]() { return CanSignIn(Index); })
												.OnClicked_Lambda([this, Index]() { return SignIn(Index); })] +
						   SHorizontalBox::Slot().AutoWidth().Padding(
							   8.0f, 0.0f)[SNew(SButton)
											   .Text(LOCTEXT("OpenBrowser", "Open Verified Page"))
											   .IsEnabled_Lambda([this, Index]()
																 { return Providers[Index].CanOpenVerificationPage(); })
											   .OnClicked_Lambda([this, Index]() { return OpenBrowser(Index); })] +
						   SHorizontalBox::Slot().AutoWidth().Padding(
							   8.0f, 0.0f)[SNew(SButton)
											   .Text(LOCTEXT("CancelAuth", "Cancel"))
											   .IsEnabled_Lambda([this, Index]() { return Providers[Index].IsBusy(); })
											   .OnClicked_Lambda([this, Index]() { return Cancel(Index); })] +
						   SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SSpacer)] +
						   SHorizontalBox::Slot()
							   .AutoWidth()[SNew(SButton)
												.Text(LOCTEXT("SignOut", "Sign Out"))
												.IsEnabled_Lambda([this, Index]() { return CanSignOut(Index); })
												.OnClicked_Lambda([this, Index]() { return SignOut(Index); })]]];
	}

	FText GetStatusText(const int32 Index) const
	{
		const FSubscriptionProviderUiState &State = Providers[Index];
		if (!State.IsAvailable())
		{
			return LOCTEXT("ProviderUnavailable", "Account runtime: unavailable | Model resource runtime: unavailable");
		}
		return FText::Format(LOCTEXT("ProviderStatusFormat", "Account credential: {0} | Model resource runtime: {1}"),
							 StateText(State.GetStatus().State),
							 State.Authority->IsResourceRuntimeReady()
								 ? LOCTEXT("ResourceRuntimeReady", "ready")
								 : LOCTEXT("ResourceRuntimeUnavailable", "unavailable"));
	}

	FText GetDestinationDisclosureText(const int32 Index) const
	{
		const FSubscriptionProviderUiState &State = Providers[Index];
		const TCHAR *Billing =
			State.Destination.BillingMode == EUnrealAIBillingMode::SubscriptionQuota ? TEXT("subscription quota")
																					 : TEXT("non-subscription billing");
		return FText::Format(
			LOCTEXT("AccountDestinationDisclosureFormat",
					"Selection: {0} | Payer: {1} | Billing: {2} | Tenant: {3} | Destination: {4} | Audience: {5}"),
			FText::FromName(State.SelectionAlias), FText::FromName(State.Destination.PayerHandle),
			FText::FromString(Billing), FText::FromName(State.Destination.TenantRealm),
			FText::FromString(State.Destination.EndpointOrigin.ToString()),
			FText::FromString(State.Destination.Audience));
	}

	bool CanSignIn(const int32 Index) const
	{
		const FSubscriptionProviderUiState &State = Providers[Index];
		return State.AuthFlow != EUnrealAIInteractiveAuthFlow::Invalid &&
			   IsSignInGestureEnabled(State.GetStatus().State, State.IsAvailable(), State.IsBusy(), bShuttingDown);
	}

	bool CanSignOut(const int32 Index) const
	{
		const FSubscriptionProviderUiState &State = Providers[Index];
		return IsSignOutGestureEnabled(State.GetStatus().State, State.IsAvailable(), State.IsBusy(), bShuttingDown);
	}

	FReply SignIn(const int32 Index)
	{
		if (!CanSignIn(Index))
		{
			return FReply::Handled();
		}
		FSubscriptionProviderUiState &State = Providers[Index];
		State.ClearInteraction();
		State.Cancellation = FUnrealAICancellationSource();
		FUnrealAIInteractiveAuthRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.AuthProfileId = State.AuthProfileId;
		Request.Flow = State.AuthFlow;
		Request.TimeoutSeconds = SignInTimeoutSeconds;
		if (!Mailbox->TryReserveRequest(Request.RequestId, State.AuthProfileId, State.AccountId,
										EUnrealAIAuthOperationKind::SignIn))
		{
			State.LastOutcome = LOCTEXT("SignInTerminalCapacityUnavailable",
										"Sign-in could not start because account result capacity is unavailable.");
			return FReply::Handled();
		}
		State.ActiveRequestId = Request.RequestId;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		if (!State.Authority->StartSignIn(Request, Mailbox.ToSharedRef(), State.Cancellation.GetToken(), Handle,
										  Error) ||
			!Handle.IsValid() || !(Handle->GetRequestId() == Request.RequestId))
		{
			State.Cancellation.Cancel(EUnrealAICancellationReason::Requested);
			if (Handle.IsValid())
			{
				Handle->Cancel();
			}
			Mailbox->RetireRequest(Request.RequestId);
			State.ActiveRequestId.Reset();
			State.LastOutcome = AuthenticationFailureText(Error);
			return FReply::Handled();
		}
		State.Operation = MoveTemp(Handle);
		State.LastOutcome =
			State.AuthFlow == EUnrealAIInteractiveAuthFlow::DeviceCode
				? LOCTEXT("DeviceSignInStarted", "Requesting a provider device code...")
				: LOCTEXT("BrowserSignInStarted", "Opening the verified provider authorization page...");
		return FReply::Handled();
	}

	FReply SignOut(const int32 Index)
	{
		if (!CanSignOut(Index))
		{
			return FReply::Handled();
		}
		FSubscriptionProviderUiState &State = Providers[Index];
		State.ClearInteraction();
		State.Cancellation = FUnrealAICancellationSource();
		FUnrealAIAccountAuthRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.AuthProfileId = State.AuthProfileId;
		Request.AccountId = State.AccountId;
		Request.TimeoutSeconds = SignOutTimeoutSeconds;
		if (!Mailbox->TryReserveRequest(Request.RequestId, State.AuthProfileId, State.AccountId,
										EUnrealAIAuthOperationKind::SignOut))
		{
			State.LastOutcome = LOCTEXT("SignOutTerminalCapacityUnavailable",
										"Sign-out could not start because account result capacity is unavailable.");
			return FReply::Handled();
		}
		State.ActiveRequestId = Request.RequestId;
		TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError Error;
		if (!State.Authority->StartSignOut(Request, Mailbox.ToSharedRef(), State.Cancellation.GetToken(), Handle,
										   Error) ||
			!Handle.IsValid() || !(Handle->GetRequestId() == Request.RequestId))
		{
			State.Cancellation.Cancel(EUnrealAICancellationReason::Requested);
			if (Handle.IsValid())
			{
				Handle->Cancel();
			}
			Mailbox->RetireRequest(Request.RequestId);
			State.ActiveRequestId.Reset();
			State.LastOutcome = AuthenticationFailureText(Error);
			return FReply::Handled();
		}
		State.Operation = MoveTemp(Handle);
		State.LastOutcome = LOCTEXT("SignOutStarted", "Revoking access and removing the local secure-store session...");
		return FReply::Handled();
	}

	FReply OpenBrowser(const int32 Index)
	{
		const FString &Uri = Providers[Index].VerificationUri;
		if (!Uri.IsEmpty())
		{
			FPlatformProcess::LaunchURL(*Uri, nullptr, nullptr);
		}
		return FReply::Handled();
	}

	FReply Cancel(const int32 Index)
	{
		FSubscriptionProviderUiState &State = Providers[Index];
		State.Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		if (State.Operation.IsValid())
		{
			State.Operation->Cancel();
		}
		return FReply::Handled();
	}

	FSubscriptionProviderUiState *FindProvider(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId)
	{
		for (FSubscriptionProviderUiState &State : Providers)
		{
			if (State.AuthProfileId == AuthProfileId && State.AccountId == AccountId)
			{
				return &State;
			}
		}
		return nullptr;
	}

	void Shutdown()
	{
		if (bShuttingDown)
		{
			return;
		}
		bShuttingDown = true;
		for (FSubscriptionProviderUiState &State : Providers)
		{
			State.CancelAndReset(EUnrealAICancellationReason::OwnerDestroyed);
		}
		Mailbox.Reset();
	}

	TArray<FSubscriptionProviderUiState> Providers;
	TSharedPtr<FSubscriptionAuthMailbox, ESPMode::ThreadSafe> Mailbox;
	bool bShuttingDown = false;
};
} // namespace

TSharedRef<SWidget> MakeUnrealAIAccountsPanel()
{
	return SNew(SAgentSubscriptionAccountsPanel);
}

#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS

namespace
{
class FEditorAuthTestOperation final : public IUnrealAIAuthOperationHandle
{
  public:
	explicit FEditorAuthTestOperation(const FUnrealAIRequestId InRequestId) : RequestId(InRequestId) {}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	void Cancel() override
	{
		++CancelCount;
	}

	FUnrealAIRequestId RequestId;
	int32 CancelCount = 0;
};

FUnrealAIAuthEvent MakeEditorAuthProgressEvent(const FUnrealAIRequestId &RequestId, const FName AuthProfileId,
											   const FUnrealAIAccessAccountId &AccountId,
											   const EUnrealAIAuthOperationKind OperationKind,
											   const EUnrealAIAccountAuthState State)
{
	FUnrealAIAuthEvent Event;
	Event.RequestId = RequestId;
	Event.AuthProfileId = AuthProfileId;
	Event.AccountId = AccountId;
	Event.OperationKind = OperationKind;
	Event.Kind = EUnrealAIAuthEventKind::StatusChanged;
	Event.State = State;
	return Event;
}

FUnrealAIAuthEvent MakeEditorAuthSuccessEvent(const FUnrealAIRequestId &RequestId, const FName AuthProfileId,
											  const FUnrealAIAccessAccountId &AccountId,
											  const EUnrealAIAuthOperationKind OperationKind)
{
	FUnrealAIAuthEvent Event;
	Event.RequestId = RequestId;
	Event.AuthProfileId = AuthProfileId;
	Event.AccountId = AccountId;
	Event.OperationKind = OperationKind;
	Event.Kind = EUnrealAIAuthEventKind::Succeeded;
	Event.State = OperationKind == EUnrealAIAuthOperationKind::SignIn ? EUnrealAIAccountAuthState::Ready
																	  : EUnrealAIAccountAuthState::SignedOut;
	return Event;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountEditorMailboxSaturationTest,
								 "UnrealAI.Auth.Editor.MailboxSaturationPreservesTerminal",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountEditorMailboxSaturationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FSubscriptionAuthMailbox Mailbox;
	const FUnrealAIRequestId RequestId{FGuid(0xaa066001, 0xaa066002, 0xaa066003, 0xaa066004)};
	const FName AuthProfileId(TEXT("tests.editor.mailbox.saturation"));
	FUnrealAIAccessAccountId AccountId;
	AccountId.Value = FGuid(0xaa066005, 0xaa066006, 0xaa066007, 0xaa066008);
	TestTrue(TEXT("The account operation reserves terminal delivery before progress"),
				  Mailbox.TryReserveRequest(RequestId, AuthProfileId, AccountId, EUnrealAIAuthOperationKind::SignIn));

	for (int32 Index = 0; Index < MaxQueuedAuthEvents; ++Index)
	{
		Mailbox.EnqueueAuthEvent(MakeEditorAuthProgressEvent(
			RequestId, AuthProfileId, AccountId, EUnrealAIAuthOperationKind::SignIn,
			Index == 0 ? EUnrealAIAccountAuthState::SignedOut : EUnrealAIAccountAuthState::Authorizing));
	}
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(RequestId, AuthProfileId, AccountId, EUnrealAIAuthOperationKind::SignIn));
	Mailbox.EnqueueAuthEvent(MakeEditorAuthProgressEvent(RequestId, AuthProfileId, AccountId,
														 EUnrealAIAuthOperationKind::SignIn,
														 EUnrealAIAccountAuthState::Refreshing));

	TArray<FUnrealAIAuthEvent> Events;
	Mailbox.Drain(Events);
	TestEqual(TEXT("Progress saturation retains a bounded queue including its reserved terminal"), Events.Num(),
				   MaxQueuedAuthEvents);
	if (Events.Num() == MaxQueuedAuthEvents)
	{
		for (int32 Index = 0; Index < Events.Num() - 1; ++Index)
		{
			TestEqual(TEXT("Only the oldest progress is discarded while later progress remains ordered"),
						   Events[Index].Kind, EUnrealAIAuthEventKind::StatusChanged);
			TestEqual(TEXT("The oldest distinguishable progress was evicted"), Events[Index].State,
						   EUnrealAIAccountAuthState::Authorizing);
		}
		TestEqual(TEXT("The first terminal remains the final deliverable event"), Events.Last().Kind,
					   EUnrealAIAuthEventKind::Succeeded);
		TestEqual(TEXT("The preserved terminal retains its exact request"), Events.Last().RequestId, RequestId);
	}
	Mailbox.RetireRequest(RequestId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountEditorMailboxTerminalFenceTest,
								 "UnrealAI.Auth.Editor.MailboxTerminalFenceAndDrainOrder",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountEditorMailboxTerminalFenceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FSubscriptionAuthMailbox Mailbox;
	const FName AuthProfileId(TEXT("tests.editor.mailbox.order"));
	const FUnrealAIRequestId SignInRequest{FGuid(0xaa066011, 0xaa066012, 0xaa066013, 0xaa066014)};
	const FUnrealAIRequestId SignOutRequest{FGuid(0xaa066021, 0xaa066022, 0xaa066023, 0xaa066024)};
	FUnrealAIAccessAccountId SignInAccount;
	SignInAccount.Value = FGuid(0xaa066015, 0xaa066016, 0xaa066017, 0xaa066018);
	FUnrealAIAccessAccountId SignOutAccount;
	SignOutAccount.Value = FGuid(0xaa066025, 0xaa066026, 0xaa066027, 0xaa066028);
	TestTrue(TEXT("The sign-in request reserves one terminal"),
				  Mailbox.TryReserveRequest(SignInRequest, AuthProfileId, SignInAccount,
											EUnrealAIAuthOperationKind::SignIn));
	TestTrue(TEXT("A concurrent sign-out request reserves an independent terminal"),
				  Mailbox.TryReserveRequest(SignOutRequest, AuthProfileId, SignOutAccount,
											EUnrealAIAuthOperationKind::SignOut));

	Mailbox.EnqueueAuthEvent(MakeEditorAuthProgressEvent(SignInRequest, AuthProfileId, SignInAccount,
														 EUnrealAIAuthOperationKind::SignIn,
														 EUnrealAIAccountAuthState::Authorizing));
	Mailbox.EnqueueAuthEvent(MakeEditorAuthProgressEvent(SignOutRequest, AuthProfileId, SignOutAccount,
														 EUnrealAIAuthOperationKind::SignOut,
														 EUnrealAIAccountAuthState::Revoking));
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignInRequest, AuthProfileId, SignInAccount, EUnrealAIAuthOperationKind::SignIn));
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignInRequest, AuthProfileId, SignInAccount, EUnrealAIAuthOperationKind::SignIn));
	Mailbox.EnqueueAuthEvent(MakeEditorAuthProgressEvent(SignInRequest, AuthProfileId, SignInAccount,
														 EUnrealAIAuthOperationKind::SignIn,
														 EUnrealAIAccountAuthState::Refreshing));
	Mailbox.EnqueueAuthEvent(MakeEditorAuthProgressEvent(SignOutRequest, AuthProfileId, SignOutAccount,
														 EUnrealAIAuthOperationKind::SignOut,
														 EUnrealAIAccountAuthState::Revoking));
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignOutRequest, AuthProfileId, SignOutAccount, EUnrealAIAuthOperationKind::SignOut));
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignOutRequest, AuthProfileId, SignOutAccount, EUnrealAIAuthOperationKind::SignOut));

	TArray<FUnrealAIAuthEvent> Events;
	Mailbox.Drain(Events);
	TestEqual(TEXT("Duplicate terminals and post-terminal progress are discarded"), Events.Num(), 5);
	if (Events.Num() == 5)
	{
		TestEqual(TEXT("Drain preserves first progress request"), Events[0].RequestId, SignInRequest);
		TestEqual(TEXT("Drain preserves second progress request"), Events[1].RequestId, SignOutRequest);
		TestEqual(TEXT("The first accepted terminal remains in arrival order"), Events[2].RequestId, SignInRequest);
		TestTrue(TEXT("The first accepted terminal is terminal"), Events[2].IsTerminal());
		TestEqual(TEXT("Later progress for the live request retains its order"), Events[3].RequestId, SignOutRequest);
		TestEqual(TEXT("The second accepted terminal remains last"), Events[4].RequestId, SignOutRequest);
		TestTrue(TEXT("The second accepted terminal is terminal"), Events[4].IsTerminal());
	}

	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignInRequest, AuthProfileId, SignInAccount, EUnrealAIAuthOperationKind::SignIn));
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignOutRequest, AuthProfileId, SignOutAccount, EUnrealAIAuthOperationKind::SignOut));
	Mailbox.Drain(Events);
	TestTrue(TEXT("Late terminals remain fenced after the first drain"), Events.IsEmpty());

	Mailbox.RetireRequest(SignInRequest);
	Mailbox.RetireRequest(SignOutRequest);
	Mailbox.EnqueueAuthEvent(
		MakeEditorAuthSuccessEvent(SignInRequest, AuthProfileId, SignInAccount, EUnrealAIAuthOperationKind::SignIn));
	Mailbox.Drain(Events);
	TestTrue(TEXT("Retired request terminals cannot re-enter the mailbox"), Events.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountEditorGestureStateTest,
								 "UnrealAI.Auth.Editor.ExplicitGestureStateMatrix",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountEditorGestureStateTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("Signed-out accounts admit an explicit sign-in gesture"),
				  IsSignInGestureEnabled(EUnrealAIAccountAuthState::SignedOut, true, false, false));
	TestTrue(TEXT("Reauthentication admits an explicit sign-in gesture"),
				  IsSignInGestureEnabled(EUnrealAIAccountAuthState::ReauthenticationRequired, true, false, false));
	TestTrue(TEXT("Failed accounts admit an explicit sign-in gesture"),
				  IsSignInGestureEnabled(EUnrealAIAccountAuthState::Failed, true, false, false));
	TestFalse(TEXT("Ready accounts do not admit another sign-in gesture"),
				   IsSignInGestureEnabled(EUnrealAIAccountAuthState::Ready, true, false, false));
	TestFalse(TEXT("Authorizing accounts do not admit another sign-in gesture"),
				   IsSignInGestureEnabled(EUnrealAIAccountAuthState::Authorizing, true, false, false));
	TestFalse(TEXT("A busy UI state denies sign-in"),
				   IsSignInGestureEnabled(EUnrealAIAccountAuthState::SignedOut, true, true, false));
	TestFalse(TEXT("An unavailable account runtime denies sign-in"),
				   IsSignInGestureEnabled(EUnrealAIAccountAuthState::SignedOut, false, false, false));
	TestFalse(TEXT("Editor shutdown denies sign-in"),
				   IsSignInGestureEnabled(EUnrealAIAccountAuthState::SignedOut, true, false, true));

	TestTrue(TEXT("Ready accounts admit an explicit sign-out gesture"),
				  IsSignOutGestureEnabled(EUnrealAIAccountAuthState::Ready, true, false, false));
	TestTrue(TEXT("Reauthentication admits local credential removal"),
				  IsSignOutGestureEnabled(EUnrealAIAccountAuthState::ReauthenticationRequired, true, false, false));
	TestTrue(TEXT("Failed accounts admit local credential removal"),
				  IsSignOutGestureEnabled(EUnrealAIAccountAuthState::Failed, true, false, false));
	TestFalse(TEXT("Signed-out accounts do not admit sign-out"),
				   IsSignOutGestureEnabled(EUnrealAIAccountAuthState::SignedOut, true, false, false));
	TestFalse(TEXT("A busy UI state denies sign-out"),
				   IsSignOutGestureEnabled(EUnrealAIAccountAuthState::Ready, true, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountEditorSafeAuthDiagnosticsTest,
								 "UnrealAI.Auth.Editor.SafeAuthDiagnosticsAreActionable",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountEditorSafeAuthDiagnosticsTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	struct FDiagnosticRow final
	{
		EUnrealAIProviderAccessErrorCode Code;
		const TCHAR *ExpectedText;
	};
	const FDiagnosticRow Rows[] = {
		{EUnrealAIProviderAccessErrorCode::AuthResponseInvalid,
		 TEXT("Authentication failed: the provider returned a token response that could not be validated.")},
		 {EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete,
		  TEXT("Authentication failed: the provider did not issue all credentials required for a renewable session.")},
		  {EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient,
		   TEXT("Authentication completed, but the account did not grant the required subscription API scopes.")},
		   {EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported,
			TEXT("Authentication failed: the provider returned an unsupported token type.")},
			{EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid,
			 TEXT("Authentication failed: the provider returned an invalid or unsupported token lifetime.")},
			 {EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
			  TEXT("Authentication succeeded, but the credential could not be stored in the local secure store.")}};

	for (const FDiagnosticRow &Row : Rows)
	{
		FUnrealAIProviderAccessError Error;
		Error.Code = Row.Code;
		TestEqual(TEXT("Each string-free auth reason maps to an actionable local message"),
					   AuthenticationFailureText(Error).ToString(), FString(Row.ExpectedText));
	}
	FUnrealAIProviderAccessError Generic;
	Generic.Code = EUnrealAIProviderAccessErrorCode::AuthFailed;
	TestEqual(TEXT("Unknown provider details still fall back to the privacy-preserving message"),
				   AuthenticationFailureText(Generic).ToString(),
				   FString(TEXT("Authentication failed. No provider response details were retained.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountEditorEventFenceTest,
								 "UnrealAI.Auth.Editor.StaleEventsAndPageEnableFailClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountEditorEventFenceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FSubscriptionProviderUiState State;
	State.AuthProfileId = TEXT("tests.editor.auth");
	State.AccountId.Value = FGuid(1, 2, 3, 4);
	State.ActiveRequestId = FUnrealAIRequestId{FGuid(5, 6, 7, 8)};
	State.VerificationUri = TEXT("https://example.invalid/verified");

	FUnrealAIAuthEvent Matching;
	Matching.RequestId = State.ActiveRequestId.GetValue();
	Matching.AuthProfileId = State.AuthProfileId;
	Matching.AccountId = State.AccountId;
	TestTrue(TEXT("Only the exact active account event passes the UI fence"), State.MatchesActiveEvent(Matching));
	TestTrue(TEXT("The verified page is enabled only for an active interaction"), State.CanOpenVerificationPage());

	FUnrealAIAuthEvent StaleRequest;
	StaleRequest.RequestId.Value = FGuid(9, 10, 11, 12);
	StaleRequest.AuthProfileId = State.AuthProfileId;
	StaleRequest.AccountId = State.AccountId;
	TestFalse(TEXT("A stale request event is ignored"), State.MatchesActiveEvent(StaleRequest));
	FUnrealAIAuthEvent WrongAccount;
	WrongAccount.RequestId = State.ActiveRequestId.GetValue();
	WrongAccount.AuthProfileId = State.AuthProfileId;
	WrongAccount.AccountId.Value = FGuid(13, 14, 15, 16);
	TestFalse(TEXT("A cross-account event is ignored"), State.MatchesActiveEvent(WrongAccount));

	State.ActiveRequestId.Reset();
	TestFalse(TEXT("A page URI retained without an active request remains disabled"), State.CanOpenVerificationPage());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountEditorDestructionTest,
								 "UnrealAI.Auth.Editor.DestructionCancelsAndClears",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountEditorDestructionTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FSubscriptionProviderUiState State;
	const FUnrealAIRequestId RequestId{FGuid(17, 18, 19, 20)};
	State.ActiveRequestId = RequestId;
	State.VerificationUri = TEXT("https://example.invalid/verified");
	State.UserCode = TEXT("TRANSIENT");
	const TSharedRef<FEditorAuthTestOperation, ESPMode::ThreadSafe> Operation =
		MakeShared<FEditorAuthTestOperation, ESPMode::ThreadSafe>(RequestId);
	State.Operation = Operation;
	const FUnrealAICancellationToken Token = State.Cancellation.GetToken();

	State.CancelAndReset(EUnrealAICancellationReason::OwnerDestroyed);

	TestTrue(TEXT("Panel destruction cancels the shared operation token"), Token.IsCancellationRequested());
	TestEqual(TEXT("Panel destruction cancels the physical operation handle once"), Operation->CancelCount, 1);
	TestFalse(TEXT("Panel destruction clears active request identity"), State.ActiveRequestId.IsSet());
	TestFalse(TEXT("Panel destruction releases the operation handle"), State.Operation.IsValid());
	TestTrue(TEXT("Panel destruction clears the verification URI"), State.VerificationUri.IsEmpty());
	TestTrue(TEXT("Panel destruction securely clears the transient user code"), State.UserCode.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS

#undef LOCTEXT_NAMESPACE
