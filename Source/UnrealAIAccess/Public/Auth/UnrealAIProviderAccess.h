// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "UnrealAIAccessTypes.h"
#include "UnrealAIAccessTypes.h"
#include "Runtime/UnrealAICancellation.h"
#include "Runtime/UnrealAIClock.h"
#include "Templates/Function.h"

namespace UE::UnrealAI::Private
{
class FCredentialFreshnessState;
class FDeviceOAuthAccountProviderState;
} // namespace UE::UnrealAI::Private

class IUnrealAIProviderAccessContext;
class FUnrealAIOAuthAuthorizationKernel;
class FUnrealAIOAuthHttpSecretPayload;
class FUnrealAIOAuthTokenEnvelope;
class FUnrealAIOAuthTokenEnvelopeCodec;

/** Authentication presented to a model endpoint. This is intentionally not reflected. */
enum class EUnrealAIAuthScheme : uint8
{
	Invalid,
	Anonymous,
	ApiKey,
	OAuthBearer,
	GatewayBearer
};

/** Who accounts for a connection's model usage. This is intentionally not reflected. */
enum class EUnrealAIBillingMode : uint8
{
	Invalid,
	Local,
	ApiMetered,
	SubscriptionQuota,
	GatewayAccounted
};

/** Public-safe support state for one provider access route. */
enum class EUnrealAIProviderAccessAvailability : uint8
{
	Invalid,
	Available,
	ConfigurationRequired,
	SignedOut,
	ReauthenticationRequired,
	PartnerGated,
	Unsupported,
	Unavailable
};

/** Public-safe support/stability classification for a provider route. */
enum class EUnrealAIProviderAccessSupportClassification : uint8
{
	Invalid,
	Supported,
	ExperimentalDirectSubscriptionCompatibility,
	/** Source may be exercised for development, but the supported release-evidence gate has not closed. */
	ImplementationCandidate
};

/** Trust source for a registered endpoint profile. Callers cannot construct profile descriptors directly. */
enum class EUnrealAIEndpointProfileClass : uint8
{
	Invalid,
	LocalInProcess,
	CustomApi,
	LocalLoopbackDevelopment,
	ProjectGateway,
	ProviderSubscriptionResource
};

/** Public-safe account lifecycle state. Tokens and identity claims are never represented here. */
enum class EUnrealAIAccountAuthState : uint8
{
	Invalid,
	SignedOut,
	Authorizing,
	Ready,
	Refreshing,
	ReauthenticationRequired,
	Revoking,
	Failed
};

/** Interactive public-client authorization flow offered by an account provider. */
enum class EUnrealAIInteractiveAuthFlow : uint8
{
	Invalid,
	BrowserPkce,
	DeviceCode
};

/** Shape of one trusted local-UI interaction. Browser authorization and device verification have different rules. */
enum class EUnrealAIAuthInteractionKind : uint8
{
	Invalid,
	BrowserLaunch,
	DeviceCode
};

/** Operation that owns an auth event, needed to validate successful sign-in versus successful sign-out. */
enum class EUnrealAIAuthOperationKind : uint8
{
	Invalid,
	SignIn,
	SignOut
};

/** Typed secure-store outcome. Callers must not derive diagnostics from secret contents. */
enum class EUnrealAISecretStoreResult : uint8
{
	Succeeded,
	NotFound,
	Conflict,
	Locked,
	Denied,
	Unavailable,
	NotSupported,
	Corrupt,
	Cancelled,
	TimedOut,
	Failed
};

/** Closed persistence guarantee advertised by a secure-store implementation. */
enum class EUnrealAISecretStorePersistenceClass : uint8
{
	Invalid,
	Volatile,
	Persistent
};

/** Closed at-rest protection boundary advertised by a secure-store implementation. */
enum class EUnrealAISecretStoreProtectionClass : uint8
{
	Invalid,
	ProcessMemory,
	PlatformCredentialStore,
	ExternalSecretService
};

/** Closed principal/lifetime scope under which a secure-store record is protected. */
enum class EUnrealAISecretStoreScopeClass : uint8
{
	Invalid,
	Process,
	CurrentUser,
	LocalMachine,
	Service
};

/** Closed machine codes for provider-access control paths; provider text can never enter a permanent FName. */
enum class EUnrealAIProviderAccessErrorCode : uint8
{
	None,
	InvalidRequest,
	InvalidConfiguration,
	UnsupportedCapability,
	PartnerGated,
	AccessProfileNotReady,
	AuthCancelled,
	AuthTimedOut,
	AuthFailed,
	CredentialCancelled,
	CredentialTimedOut,
	CredentialFailed,
	CredentialEntitlementDenied,
	CredentialRefreshCapacity,
	InvalidSecretStoreContext,
	SecretHandleStoreMismatch,
	InvalidSecretStoreWrite,
	InvalidSecretStoreDelete,
	SecretNotFound,
	SecretRevisionConflict,
	SecretStoreLocked,
	SecretStoreDenied,
	SecretStoreUnavailable,
	SecretStoreNotSupported,
	SecretStoreCorrupt,
	SecretStoreCapacity,
	SecretStoreCancelled,
	SecretStoreTimedOut,
	SecretCopyFailed,
	OperationBusy,
	AuthResponseInvalid,
	AuthCredentialIncomplete,
	AuthScopeInsufficient,
	AuthTokenTypeUnsupported,
	AuthExpiryInvalid,
	AuthPersistenceFailed,
	Internal
};

/** Non-reflected, string-free error envelope for auth, credential, and secret-store operations. */
struct UNREALAIACCESS_API FUnrealAIProviderAccessError final
{
	static constexpr float MaxRetryAfterSeconds = 3600.0f;

	EUnrealAIErrorCategory Category = EUnrealAIErrorCategory::None;
	EUnrealAIProviderAccessErrorCode Code = EUnrealAIProviderAccessErrorCode::None;
	bool bRetryable = false;
	float RetryAfterSeconds = 0.0f;

	bool IsError() const
	{
		return Category != EUnrealAIErrorCategory::None;
	}
	bool ValidateShape(FString &OutError) const;
};

/** Locally assigned account identity. Provider subject IDs and email addresses never become framework identity. */
struct UNREALAIACCESS_API FUnrealAIAccessAccountId final
{
	FGuid Value;

	bool IsValid() const
	{
		return Value.IsValid();
	}
	friend bool operator==(const FUnrealAIAccessAccountId &A, const FUnrealAIAccessAccountId &B)
	{
		return A.Value == B.Value;
	}
	friend uint32 GetTypeHash(const FUnrealAIAccessAccountId &Id)
	{
		return GetTypeHash(Id.Value);
	}
};

/** Opaque handle into one secure store. It never contains an account name or secret material. */
struct UNREALAIACCESS_API FUnrealAISecretHandle final
{
	FName StoreName;
	FGuid Value;

	bool IsValid() const;
	bool ValidateShape(FString &OutError) const;

	friend bool operator==(const FUnrealAISecretHandle &A, const FUnrealAISecretHandle &B)
	{
		return A.StoreName == B.StoreName && A.Value == B.Value;
	}
	friend uint32 GetTypeHash(const FUnrealAISecretHandle &Handle)
	{
		return HashCombine(GetTypeHash(Handle.StoreName), GetTypeHash(Handle.Value));
	}
};

/**
 * Move-only secret bytes. The value is non-reflected, non-serializable, has no plaintext accessor, and is wiped on
 * replacement/destruction where the platform permits. Create it only at a credential-entry or secure-store boundary.
 */
class UNREALAIACCESS_API FUnrealAISecretValue final
{
  public:
	static constexpr int32 MaxSecretBytes = 64 * 1024;

	FUnrealAISecretValue() = default;
	~FUnrealAISecretValue();
	FUnrealAISecretValue(const FUnrealAISecretValue &) = delete;
	FUnrealAISecretValue &operator=(const FUnrealAISecretValue &) = delete;
	FUnrealAISecretValue(FUnrealAISecretValue &&Other) noexcept;
	FUnrealAISecretValue &operator=(FUnrealAISecretValue &&Other) noexcept;

	static bool TryCreate(TArray<uint8> &&InBytes, FUnrealAISecretValue &OutValue, FString &OutError);
	bool IsSet() const;
	int32 Num() const;
	FString GetRedactedDisplay() const;
	void Reset();

  private:
	TConstArrayView<uint8> View() const;
	TArray<uint8> Bytes;

	friend class IUnrealAISecretStore;
	friend class FUnrealAICredentialLease;
	friend class FUnrealAIOAuthAuthorizationKernel;
	friend class FUnrealAIOAuthDurableAccountTransaction;
	friend class FUnrealAIOAuthHttpSecretPayload;
	friend class FUnrealAIOAuthTokenEnvelope;
	friend class FUnrealAIOAuthTokenEnvelopeCodec;
	friend class UE::UnrealAI::Private::FDeviceOAuthAccountProviderState;
};

/** Canonical endpoint origin. Paths, query strings, fragments, userinfo, and non-loopback plaintext origins fail. */
class UNREALAIACCESS_API FUnrealAIEndpointOrigin final
{
  public:
	static constexpr int32 MaxOriginUtf8Bytes = 2048;

	static bool TryParse(const FString &Input, bool bAllowLoopbackHttp, FUnrealAIEndpointOrigin &OutOrigin,
						 FString &OutError);
	bool IsValid() const;
	bool IsSecure() const;
	bool HasLoopbackHost() const;
	bool IsLoopbackDevelopmentOnly() const;
	const FString &ToString() const;

	friend bool operator==(const FUnrealAIEndpointOrigin &A, const FUnrealAIEndpointOrigin &B)
	{
		return A.CanonicalOrigin == B.CanonicalOrigin;
	}
	friend bool operator!=(const FUnrealAIEndpointOrigin &A, const FUnrealAIEndpointOrigin &B)
	{
		return !(A == B);
	}
	friend uint32 GetTypeHash(const FUnrealAIEndpointOrigin &Origin)
	{
		return GetTypeHash(Origin.CanonicalOrigin);
	}

  private:
	FString CanonicalOrigin;
	bool bLoopbackHost = false;
	bool bLoopbackDevelopmentOnly = false;
};

/** Exact trusted destination against which a credential lease authorizes materialization. */
struct UNREALAIACCESS_API FUnrealAICredentialDestination final
{
	static constexpr int32 MaxIdentifierUtf8Bytes = 128;
	static constexpr int32 MaxAudienceUtf8Bytes = 2048;

	FName ModelProviderName;
	FName AccountAuthProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FName TenantRealm;
	FUnrealAIBillingPrincipalId BillingPrincipalId;
	FName PayerHandle;
	EUnrealAIAuthScheme AuthScheme = EUnrealAIAuthScheme::Invalid;
	EUnrealAIBillingMode BillingMode = EUnrealAIBillingMode::Invalid;
	FUnrealAIEndpointOrigin EndpointOrigin;
	FString Audience;
	uint64 ConnectionRevision = 0;
	uint64 EndpointPolicyRevision = 0;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Transport-owned credential dispatcher. Implementations bind Destination to the transport's actually parsed request
 * URL and start network admission inside ApplyCredentialAndDispatch; model-provider code never receives a reusable
 * byte view directly.
 */
class UNREALAIACCESS_API IUnrealAICredentialApplicator
{
  public:
	virtual ~IUnrealAICredentialApplicator() = default;
	virtual const FUnrealAICredentialDestination &GetActualDestination() const = 0;

  protected:
	virtual bool ApplyCredentialAndDispatch(EUnrealAIAuthScheme Scheme, TConstArrayView<uint8> Secret) = 0;
	/**
	 * Closed two-secret presentation used only by provider compatibility transports that require an account-routing
	 * value in addition to the bearer. Existing applicators fail closed unless they explicitly opt into this shape.
	 */
	virtual bool ApplyCredentialAndProtectedSecondaryAndDispatch(EUnrealAIAuthScheme, TConstArrayView<uint8>,
																 TConstArrayView<uint8>)
	{
		return false;
	}
	virtual bool DispatchWithoutCredential() = 0;

	friend class FUnrealAICredentialLease;
	friend class IUnrealAIProviderAccessContext;
};

/** Capability minted only by an account-auth provider implementation for its approved resource registrations. */
class UNREALAIACCESS_API FUnrealAIProviderEndpointAuthority final
{
  public:
	FUnrealAIProviderEndpointAuthority() = default;
	bool IsValid() const;
	FName GetAuthProviderName() const
	{
		return AuthProviderName;
	}
	FName GetModelProviderName() const
	{
		return ModelProviderName;
	}
	bool Authorizes(FName InModelProviderName, EUnrealAIAuthScheme InAuthScheme,
					EUnrealAIBillingMode InBillingMode) const;

  private:
	FUnrealAIProviderEndpointAuthority(FName InAuthProviderName, FName InModelProviderName,
									   EUnrealAIAuthScheme InAuthScheme, EUnrealAIBillingMode InBillingMode)
		: AuthProviderName(InAuthProviderName), ModelProviderName(InModelProviderName), AuthScheme(InAuthScheme),
		  BillingMode(InBillingMode)
	{
	}
	FName AuthProviderName;
	FName ModelProviderName;
	EUnrealAIAuthScheme AuthScheme = EUnrealAIAuthScheme::Invalid;
	EUnrealAIBillingMode BillingMode = EUnrealAIBillingMode::Invalid;

	friend class FUnrealAIAccountAuthProviderRegistry;
};

/** Immutable endpoint record created only through the endpoint-profile registry's typed admission methods. */
class UNREALAIACCESS_API FUnrealAIEndpointProfileDescriptor final
{
  public:
	FName GetProfileId() const;
	FName GetModelProviderName() const;
	FName GetAuthProviderName() const;
	EUnrealAIEndpointProfileClass GetProfileClass() const;
	EUnrealAIAuthScheme GetAuthScheme() const;
	EUnrealAIBillingMode GetBillingMode() const;
	const FUnrealAIEndpointOrigin &GetOrigin() const;
	const FString &GetAudience() const;
	uint64 GetPolicyRevision() const;
	bool MatchesDestination(const FUnrealAICredentialDestination &Destination) const;
	bool ValidateShape(FString &OutError) const;

  private:
	FName ProfileId;
	FName ModelProviderName;
	FName AuthProviderName;
	EUnrealAIEndpointProfileClass ProfileClass = EUnrealAIEndpointProfileClass::Invalid;
	EUnrealAIAuthScheme AuthScheme = EUnrealAIAuthScheme::Invalid;
	EUnrealAIBillingMode BillingMode = EUnrealAIBillingMode::Invalid;
	FUnrealAIEndpointOrigin Origin;
	FString Audience;
	uint64 PolicyRevision = 0;

	friend class FUnrealAIEndpointProfileRegistry;
};

/**
 * Copyable generation snapshot carried by a lease. It becomes stale immediately when its broker-owned source
 * invalidates the account or begins shutdown.
 */
class UNREALAIACCESS_API FUnrealAICredentialFreshnessToken final
{
  public:
	FUnrealAICredentialFreshnessToken() = default;
	bool IsValid() const;
	bool IsCurrent() const;

  private:
	FUnrealAICredentialFreshnessToken(
		TSharedPtr<UE::UnrealAI::Private::FCredentialFreshnessState, ESPMode::ThreadSafe> InState, uint64 InGeneration);

	TSharedPtr<UE::UnrealAI::Private::FCredentialFreshnessState, ESPMode::ThreadSafe> State;
	uint64 Generation = 0;

	friend class FUnrealAICredentialFreshnessSource;
	friend class FUnrealAICredentialLease;
	friend class IUnrealAIProviderAccessContext;
};

/** Broker-owned account generation. Invalidation revokes all outstanding leases without retaining their secrets. */
class UNREALAIACCESS_API FUnrealAICredentialFreshnessSource final
{
  public:
	FUnrealAICredentialFreshnessSource();
	~FUnrealAICredentialFreshnessSource();
	FUnrealAICredentialFreshnessSource(const FUnrealAICredentialFreshnessSource &) = delete;
	FUnrealAICredentialFreshnessSource &operator=(const FUnrealAICredentialFreshnessSource &) = delete;
	FUnrealAICredentialFreshnessSource(FUnrealAICredentialFreshnessSource &&) = delete;
	FUnrealAICredentialFreshnessSource &operator=(FUnrealAICredentialFreshnessSource &&) = delete;

	FUnrealAICredentialFreshnessToken GetToken() const;
	/**
	 * Requests revocation immediately, then returns false rather than block when a dispatch read permit is active.
	 * A false result leaves the source fail-closed until the broker retries after dispatch or replaces the source.
	 */
	bool Invalidate();
	/** Requests shutdown immediately and never waits for a hung dispatcher; false still leaves every token fail-closed.
	 */
	bool BeginShutdown();
	bool IsShutdown() const;

  private:
	TSharedRef<UE::UnrealAI::Private::FCredentialFreshnessState, ESPMode::ThreadSafe> State;
};

/**
 * Short-lived, move-only authorization to materialize one secret for one exact destination binding. A model provider
 * cannot retarget this lease to a different account, auth profile, audience, billing principal, or endpoint origin.
 */
class UNREALAIACCESS_API FUnrealAICredentialLease final
{
  public:
	FUnrealAICredentialLease() = default;
	~FUnrealAICredentialLease();
	FUnrealAICredentialLease(const FUnrealAICredentialLease &) = delete;
	FUnrealAICredentialLease &operator=(const FUnrealAICredentialLease &) = delete;
	FUnrealAICredentialLease(FUnrealAICredentialLease &&Other) noexcept;
	FUnrealAICredentialLease &operator=(FUnrealAICredentialLease &&Other) noexcept;

	static constexpr double MaxLeaseLifetimeSeconds = 300.0;

	/**
	 * ReleaseCallback follows the lease through moves and runs exactly once when its owner is reset, destroyed, or
	 * replaced by move assignment.
	 * It must be non-blocking and must not acquire broker or freshness locks because one-shot dispatch resets the lease
	 * while its freshness read permit is held.
	 */
	static bool TryCreate(const FUnrealAICredentialDestination &Binding,
						  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
						  const FUnrealAICredentialFreshnessToken &Freshness, double LeaseLifetimeSeconds,
						  TOptional<FDateTime> CredentialExpiresAtUtc, FUnrealAISecretValue &&Secret,
						  FUnrealAICredentialLease &OutLease, FString &OutError,
						  TUniqueFunction<void()> ReleaseCallback = {});
	/**
	 * Creates the only supported protected-secondary credential shape. It is restricted to OAuth subscription
	 * destinations and remains opaque until a transport applicator consumes both values in one dispatch attempt.
	 */
	static bool
	TryCreateWithProtectedSecondary(const FUnrealAICredentialDestination &Binding,
									TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
									const FUnrealAICredentialFreshnessToken &Freshness, double LeaseLifetimeSeconds,
									TOptional<FDateTime> CredentialExpiresAtUtc, FUnrealAISecretValue &&Secret,
									FUnrealAISecretValue &&ProtectedSecondary, FUnrealAICredentialLease &OutLease,
									FString &OutError, TUniqueFunction<void()> ReleaseCallback = {});

	bool IsValid() const;
	bool IsExpired() const;
	const FUnrealAICredentialDestination &GetBinding() const;
	TOptional<FDateTime> GetCredentialExpiryUtc() const;
	FString GetRedactedDisplay() const;
	void Reset();

	/**
	 * Dispatches once through the transport-owned seam only when the actual destination matches and the lease is
	 * unexpired/current. The callback must synchronously admit the request to its network transport before returning.
	 * The freshness read permit is held through that admission. Invalidation marks tokens fail-closed immediately and
	 * returns false rather than wait when admission is still active; brokers retry or replace that source off the
	 * dispatch stack. The lease wipes itself after the dispatcher receives bytes, whether dispatch succeeds or fails.
	 * The move-only lease has one dispatch owner and must not be reset/applied concurrently.
	 */
	bool TryApplyTo(IUnrealAICredentialApplicator &Applicator, FString &OutError);

  private:
	FUnrealAICredentialDestination Binding;
	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAICredentialFreshnessToken Freshness;
	FUnrealAIDeadline UsableUntil;
	TOptional<FDateTime> CredentialExpiresAtUtc;
	FUnrealAISecretValue Secret;
	FUnrealAISecretValue ProtectedSecondary;
	TUniqueFunction<void()> ReleaseCallback;
};

/**
 * Opaque, one-shot provider seam. Providers never receive a lease, token, key, or auth-profile implementation.
 * Credentialed destination validation happens before materialization and does not consume the frozen context.
 * Credential presentation consumes the context whether transport admission succeeds or fails. Anonymous contexts
 * consume on their first dispatch attempt.
 */
class UNREALAIACCESS_API IUnrealAIProviderAccessContext
{
  public:
	virtual ~IUnrealAIProviderAccessContext() = default;
	virtual bool IsValid() const = 0;
	virtual bool RequiresCredential() const = 0;
	virtual bool TryDispatch(IUnrealAICredentialApplicator &Dispatcher, FString &OutError) const = 0;
	virtual FString GetRedactedDisplay() const = 0;

	static TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe>
	CreateCredentialed(FUnrealAICredentialLease &&Lease);
	static TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe>
	CreateAnonymous(const FUnrealAICredentialDestination &Binding, const FUnrealAICredentialFreshnessToken &Freshness);

  protected:
	static bool DispatchAnonymous(IUnrealAICredentialApplicator &Dispatcher)
	{
		return Dispatcher.DispatchWithoutCredential();
	}
	static bool DispatchAnonymousWhileCurrent(const FUnrealAICredentialFreshnessToken &Freshness,
											  IUnrealAICredentialApplicator &Dispatcher, FString &OutError);
};

/** Provider-neutral trusted runtime connection. Assets select only ConnectionAlias, never an account or key. */
struct UNREALAIACCESS_API FUnrealAIConnectionDescriptor final
{
	static constexpr int32 CurrentSchemaVersion = 1;

	int32 SchemaVersion = CurrentSchemaVersion;
	uint64 ConnectionRevision = 1;
	FName ConnectionAlias;
	FName EndpointProfileId;
	FUnrealAICredentialDestination CredentialDestination;

	bool ValidateShape(FString &OutError) const;
	bool IsSubscriptionConnection() const;
};

/** Safe, bounded status view. AccountId is a local identifier, not an OAuth subject or email address. */
struct UNREALAIACCESS_API FUnrealAIAccountStatus final
{
	FName ProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	EUnrealAIAccountAuthState State = EUnrealAIAccountAuthState::Invalid;
	TOptional<FDateTime> AccessExpiresAtUtc;
	bool bRefreshCredentialPresent = false;

	bool ValidateShape(FString &OutError) const;
};

/** Capabilities are informational; each operation still validates provider policy at admission. */
struct UNREALAIACCESS_API FUnrealAIAccountAuthCapabilities final
{
	bool bBrowserPkce = false;
	bool bDeviceCode = false;
	bool bRefresh = false;
	bool bRevocation = false;
};

/** Safe support disclosure. Partner-gated routes contain no OAuth registration, endpoint, scope, or entitlement data.
 */
struct UNREALAIACCESS_API FUnrealAIProviderAccessDescriptor final
{
	FName ModelProviderName;
	FName AccountAuthProviderName;
	EUnrealAIAuthScheme AuthScheme = EUnrealAIAuthScheme::Invalid;
	EUnrealAIBillingMode BillingMode = EUnrealAIBillingMode::Invalid;
	EUnrealAIProviderAccessAvailability Availability = EUnrealAIProviderAccessAvailability::Invalid;
	EUnrealAIProviderAccessSupportClassification SupportClassification =
		EUnrealAIProviderAccessSupportClassification::Invalid;

	bool ValidateShape(FString &OutError) const;
};

/** Secure-store guarantees advertised to the credential broker. */
struct UNREALAIACCESS_API FUnrealAISecretStoreCapabilities final
{
	EUnrealAISecretStorePersistenceClass PersistenceClass = EUnrealAISecretStorePersistenceClass::Invalid;
	EUnrealAISecretStoreProtectionClass ProtectionClass = EUnrealAISecretStoreProtectionClass::Invalid;
	EUnrealAISecretStoreScopeClass ScopeClass = EUnrealAISecretStoreScopeClass::Invalid;
	bool bAvailableInCurrentBuild = false;

	/** Compatibility mirror. New admission and Shipping validation use PersistenceClass. */
	bool bPersistent = false;

	/** Additional implementation detail only; this never substitutes for ProtectionClass. */
	bool bHardwareBackedWhenAvailable = false;
	bool bAtomicCompareAndSwap = false;
	bool bAvailableInShipping = false;

	bool ValidateShape(FString &OutError) const;
	bool IsProductionProtected() const;
};

/** Cooperative logical deadline for a potentially blocking platform secret-store operation. */
struct UNREALAIACCESS_API FUnrealAISecretStoreOperationContext final
{
	static constexpr double MaxTimeoutSeconds = 60.0;

	static bool TryCreate(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock, double TimeoutSeconds,
						  const FUnrealAICancellationToken &InCancellation,
						  FUnrealAISecretStoreOperationContext &OutContext, FString &OutError);
	bool ValidateShape(FString &OutError) const;
	bool IsCancellationRequested() const;
	bool IsTimedOut() const;

  private:
	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIDeadline Deadline;
	FUnrealAICancellationToken Cancellation;
};

/**
 * Platform/project secure-store contract. Methods may block and must be scheduled away from the game thread by the
 * broker. A platform call may physically outlive logical cancellation; the broker must retain its worker/store state,
 * publish one logical terminal by the context deadline, and never wait indefinitely during shutdown. Revision zero
 * means "must not already exist"; later revisions implement refresh-token rotation by CAS.
 */
class UNREALAIACCESS_API IUnrealAISecretStore
{
  public:
	virtual ~IUnrealAISecretStore() = default;
	virtual FName GetStoreName() const = 0;
	virtual FUnrealAISecretStoreCapabilities DescribeCapabilities() const = 0;
	virtual EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
											const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
											uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) = 0;
	virtual EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
											 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
											 uint64 ExpectedRevision, uint64 &OutNewRevision,
											 FUnrealAIProviderAccessError &OutError) = 0;
	virtual EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
											  const FUnrealAISecretHandle &Handle, uint64 ExpectedRevision,
											  FUnrealAIProviderAccessError &OutError) = 0;

  protected:
	static TConstArrayView<uint8> ViewSecret(const FUnrealAISecretValue &Value);
};

/** Safe account-auth operation request. Provider scopes, client registration, and endpoints are trusted module data. */
struct UNREALAIACCESS_API FUnrealAIAccountAuthRequest final
{
	static constexpr float MaxTimeoutSeconds = 1800.0f;

	FUnrealAIRequestId RequestId;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	float TimeoutSeconds = 0.0f;

	bool ValidateShape(FString &OutError) const;
};

/** Sign-in additionally selects one provider-advertised public-client interaction flow. */
struct UNREALAIACCESS_API FUnrealAIInteractiveAuthRequest final
{
	static constexpr float MaxTimeoutSeconds = 1800.0f;

	FUnrealAIRequestId RequestId;
	FName AuthProfileId;
	EUnrealAIInteractiveAuthFlow Flow = EUnrealAIInteractiveAuthFlow::Invalid;
	float TimeoutSeconds = 0.0f;

	bool ValidateShape(FString &OutError) const;
};

enum class EUnrealAIAuthEventKind : uint8
{
	Invalid,
	StatusChanged,
	InteractionRequired,
	Succeeded,
	Failed,
	Cancelled,
	TimedOut
};

/** Move-only device/browser instruction delivered only to a trusted local account UI or administrator surface. */
class UNREALAIACCESS_API FUnrealAIAuthInteraction final
{
  public:
	static constexpr int32 MaxVerificationUriUtf8Bytes = 2048;
	static constexpr int32 MaxUserCodeUtf8Bytes = 128;

	FUnrealAIAuthInteraction() = default;
	~FUnrealAIAuthInteraction();
	FUnrealAIAuthInteraction(const FUnrealAIAuthInteraction &) = delete;
	FUnrealAIAuthInteraction &operator=(const FUnrealAIAuthInteraction &) = delete;
	FUnrealAIAuthInteraction(FUnrealAIAuthInteraction &&Other) noexcept;
	FUnrealAIAuthInteraction &operator=(FUnrealAIAuthInteraction &&Other) noexcept;

	static bool TryCreateBrowserLaunch(const FUnrealAIEndpointOrigin &ApprovedInteractionOrigin,
									   FString &&AuthorizationUri, FUnrealAIAuthInteraction &OutInteraction,
									   FString &OutError);
	static bool TryCreateDeviceCode(const FUnrealAIEndpointOrigin &ApprovedInteractionOrigin, FString &&VerificationUri,
									FString &&UserCode, FUnrealAIAuthInteraction &OutInteraction, FString &OutError);
	bool IsValid() const;
	EUnrealAIAuthInteractionKind GetKind() const;
	const FString &GetLaunchUri() const;
	FStringView GetUserCode() const;
	FString GetRedactedDisplay() const;
	void Reset();

  private:
	FUnrealAIEndpointOrigin ApprovedOrigin;
	EUnrealAIAuthInteractionKind Kind = EUnrealAIAuthInteractionKind::Invalid;
	FString LaunchUri;
	FString UserCode;
};

/** Owned event. Sensitive interaction data is confined to the move-only local-UI payload; tokens/responses are absent.
 */
struct UNREALAIACCESS_API FUnrealAIAuthEvent final
{
	FUnrealAIRequestId RequestId;
	FName AuthProfileId;
	/** Newly selected account on sign-in success, or exact target on sign-out success. */
	FUnrealAIAccessAccountId AccountId;
	EUnrealAIAuthOperationKind OperationKind = EUnrealAIAuthOperationKind::Invalid;
	EUnrealAIAuthEventKind Kind = EUnrealAIAuthEventKind::Invalid;
	EUnrealAIAccountAuthState State = EUnrealAIAccountAuthState::Invalid;
	TUniquePtr<FUnrealAIAuthInteraction> Interaction;
	FUnrealAIProviderAccessError Error;

	bool IsTerminal() const;
	bool ValidateShape(FString &OutError) const;
};

class UNREALAIACCESS_API IUnrealAIAuthEventSink
{
  public:
	virtual ~IUnrealAIAuthEventSink() = default;
	virtual void EnqueueAuthEvent(FUnrealAIAuthEvent &&Event) = 0;
};

class UNREALAIACCESS_API IUnrealAIAuthOperationHandle
{
  public:
	virtual ~IUnrealAIAuthOperationHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	virtual void Cancel() = 0;
};

/**
 * Opaque capability proving that an account mutation originated in a trusted local account-authoring surface.
 * Normal model, gameplay, and project callers cannot construct, copy, or retain this capability.
 */
class UNREALAIACCESS_API FUnrealAITrustedLocalAuthGesture final
{
  public:
	~FUnrealAITrustedLocalAuthGesture() = default;
	FUnrealAITrustedLocalAuthGesture(const FUnrealAITrustedLocalAuthGesture &) = delete;
	FUnrealAITrustedLocalAuthGesture &operator=(const FUnrealAITrustedLocalAuthGesture &) = delete;
	FUnrealAITrustedLocalAuthGesture(FUnrealAITrustedLocalAuthGesture &&) = delete;
	FUnrealAITrustedLocalAuthGesture &operator=(FUnrealAITrustedLocalAuthGesture &&) = delete;

  private:
	FUnrealAITrustedLocalAuthGesture() = default;
	friend class FUnrealAIAuthOpenAIModule;
	friend class FUnrealAIAuthXAIModule;
	friend class FUnrealAIDirectSubscriptionEditorGestureAuthority;
	friend class FUnrealAIBrowserOAuthAccountProviderTestGestureAuthority;
	friend class FUnrealAIDeviceOAuthAccountProviderTestGestureAuthority;
	friend class FUnrealAIProviderAccessTestGestureAuthority;
};

/** Vendor account lifecycle only. Model request serialization and transport do not belong in this interface. */
class UNREALAIACCESS_API IUnrealAIAccountAuthProvider : public IModularFeature
{
  public:
	virtual ~IUnrealAIAccountAuthProvider() = default;
	static FName GetModularFeatureName()
	{
		return TEXT("AutonomousAgents.AccountAuthProvider");
	}
	virtual FName GetProviderName() const = 0;
	virtual FUnrealAIAccountAuthCapabilities DescribeCapabilities() const = 0;
	virtual FUnrealAIProviderAccessDescriptor DescribeAccess() const = 0;
	virtual FUnrealAIAccountStatus GetStatus(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) const = 0;
	virtual bool StartSignIn(const FUnrealAITrustedLocalAuthGesture &Gesture,
							 const FUnrealAIInteractiveAuthRequest &Request,
							 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							 const FUnrealAICancellationToken &Cancellation,
							 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							 FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool StartSignOut(const FUnrealAITrustedLocalAuthGesture &Gesture,
							  const FUnrealAIAccountAuthRequest &Request,
							  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							  const FUnrealAICancellationToken &Cancellation,
							  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							  FUnrealAIProviderAccessError &OutError) = 0;
};

/** Broker request made immediately before transport dispatch. */
struct UNREALAIACCESS_API FUnrealAICredentialRequest final
{
	static constexpr float MaxTimeoutSeconds = 300.0f;

	FUnrealAIRequestId RequestId;
	FName ConnectionAlias;
	float TimeoutSeconds = 0.0f;

	bool ValidateShape(FString &OutError) const;
};

enum class EUnrealAICredentialResultKind : uint8
{
	Invalid,
	Succeeded,
	NotRequired,
	Failed,
	Cancelled,
	TimedOut
};

/** Move-only terminal result. Exactly one result is delivered for each accepted broker request. */
struct UNREALAIACCESS_API FUnrealAICredentialResult final
{
	FUnrealAICredentialResult() = default;
	FUnrealAICredentialResult(const FUnrealAICredentialResult &) = delete;
	FUnrealAICredentialResult &operator=(const FUnrealAICredentialResult &) = delete;
	FUnrealAICredentialResult(FUnrealAICredentialResult &&) = default;
	FUnrealAICredentialResult &operator=(FUnrealAICredentialResult &&) = default;

	FUnrealAIRequestId RequestId;
	EUnrealAICredentialResultKind Kind = EUnrealAICredentialResultKind::Invalid;
	TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext;
	FUnrealAIProviderAccessError Error;

	bool ValidateShape(FString &OutError) const;
};

class UNREALAIACCESS_API IUnrealAICredentialResultSink
{
  public:
	virtual ~IUnrealAICredentialResultSink() = default;
	virtual void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) = 0;
};

class UNREALAIACCESS_API IUnrealAICredentialRequestHandle
{
  public:
	virtual ~IUnrealAICredentialRequestHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	virtual void Cancel() = 0;
};

/** Provider-neutral resolver/refresh coordinator. It owns per-account single-flight and exactly-once completion. */
class UNREALAIACCESS_API IUnrealAICredentialBroker
{
  public:
	virtual ~IUnrealAICredentialBroker() = default;
	/** Returning true requires a non-null handle whose request ID exactly matches Request.RequestId. */
	virtual bool StartResolve(const FUnrealAICredentialRequest &Request,
							  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
							  const FUnrealAICancellationToken &Cancellation,
							  TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
							  FUnrealAIProviderAccessError &OutError) = 0;
	/** Revokes outstanding leases and cached material for one exact local authorization account. */
	virtual void InvalidateAccount(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) = 0;
	/** Revokes outstanding leases and cached material before sign-out/quarantine for this profile is published. */
	virtual void InvalidateAuthProfile(FName AuthProfileId) = 0;
	/** Revokes outstanding leases when a connection descriptor revision is replaced or quarantined. */
	virtual void InvalidateConnection(FName ConnectionAlias) = 0;
	/** Rejects new work and revokes every outstanding lease before broker-owned stores/workers are released. */
	virtual void BeginShutdown() = 0;
};

/** Optional refresh/re-authentication capability without a dependency on an OAuth implementation. */
class UNREALAIACCESS_API IUnrealAIRefreshableCredentialBroker : public IUnrealAICredentialBroker
{
  public:
	virtual void ForceRefreshAccount(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) = 0;
	virtual void QuarantineAccountForReauthentication(FName AuthProfileId,
													  const FUnrealAIAccessAccountId &AccountId) = 0;
};

/** Exact non-secret connection identity for opaque model history. Empty means invalid input. */
UNREALAIACCESS_API FString MakeUnrealAIConnectionBinding(const FUnrealAICredentialDestination &Destination);
