// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Auth/UnrealAIProviderAccess.h"
#include "UnrealAIModelError.h"
#include "Runtime/UnrealAICancellation.h"
#include "Runtime/UnrealAIClock.h"

/** HTTP methods admitted by the credentialed model transport. */
enum class EUnrealAIHttpMethod : uint8
{
	Invalid,
	Get,
	Post
};

/** Compiled credential-presentation policy. Arbitrary header names are intentionally unsupported. */
enum class EUnrealAIHttpCredentialPresentation : uint8
{
	Invalid,
	AuthorizationBearer,
	/** API-key secret presented only as the fixed literal Anthropic x-api-key header. */
	AnthropicApiKeyHeader,
	/** API-key secret presented only as the fixed literal Google x-goog-api-key header. */
	GeminiApiKeyHeader,
	/** OAuth bearer plus one provider-declared header whose value comes from the protected secondary credential. */
	AuthorizationBearerWithProtectedSecondary
};

/** Closed provider query vocabulary. Arbitrary query strings remain unsupported. */
enum class EUnrealAIHttpQueryProfile : uint8
{
	None,
	/** Appends exactly `alt=sse` for the first-party Gemini Interactions stream. */
	GeminiAltSse
};

/** Closed event vocabulary emitted by an HTTP request. */
enum class EUnrealAIHttpEventKind : uint8
{
	Invalid,
	ResponseStarted,
	BodyChunk,
	Completed,
	Failed,
	Cancelled,
	TimedOut
};

/** One bounded, noncredential request header. Transport-owned and hop-by-hop names are rejected. */
struct UNREALAITRANSPORT_API FUnrealAIHttpRequestHeader final
{
	static constexpr int32 MaxNameBytes = 128;
	static constexpr int32 MaxValueBytes = 8 * 1024;

	FString Name;
	FString Value;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Credential-free request description. The endpoint origin comes from the exact credential destination, while the
 * path is deliberately stricter than a general URL so provider code cannot retarget or smuggle authority components.
 */
struct UNREALAITRANSPORT_API FUnrealAIHttpRequest final
{
	static constexpr int32 MaxHeaders = 32;
	static constexpr int32 MaxRelativePathBytes = 2048;
	static constexpr int32 MaxRequestBodyBytes = 4 * 1024 * 1024;
	static constexpr int64 MaxResponseBodyBytesLimit = 16 * 1024 * 1024;
	static constexpr int32 MaxBodyChunkBytesLimit = 256 * 1024;
	static constexpr double MaxTimeoutSeconds = 30.0 * 60.0;

	FUnrealAIRequestId RequestId;
	FUnrealAICredentialDestination Destination;
	EUnrealAIHttpMethod Method = EUnrealAIHttpMethod::Invalid;
	FString RelativePath;
	EUnrealAIHttpCredentialPresentation CredentialPresentation = EUnrealAIHttpCredentialPresentation::Invalid;
	EUnrealAIHttpQueryProfile QueryProfile = EUnrealAIHttpQueryProfile::None;
	/** Header name only. Its protected value never enters the credential-free request description. */
	FString ProtectedSecondaryHeaderName;
	TArray<FUnrealAIHttpRequestHeader> Headers;
	TArray<uint8> Body;
	double TimeoutSeconds = 60.0;
	int64 MaxResponseBodyBytes = 4 * 1024 * 1024;
	int32 MaxBodyChunkBytes = 64 * 1024;

	bool ValidateShape(FString &OutError) const;
};

/** Bounded response metadata. Cookies and arbitrary response headers never cross the transport boundary. */
struct UNREALAITRANSPORT_API FUnrealAIHttpResponseMetadata final
{
	static constexpr int32 MaxContentTypeBytes = 512;
	static constexpr int32 MaxProviderRequestIdBytes = 1024;

	int32 StatusCode = 0;
	FString ContentType;
	FString ProviderRequestId;
	float RetryAfterSeconds = 0.0f;

	bool ValidateShape(FString &OutError) const;
};

/** One ordered response or terminal event. Event sinks may be invoked from a native transport thread. */
struct UNREALAITRANSPORT_API FUnrealAIHttpEvent final
{
	FUnrealAIRequestId RequestId;
	EUnrealAIHttpEventKind Kind = EUnrealAIHttpEventKind::Invalid;
	uint64 Sequence = 0;
	FUnrealAIHttpResponseMetadata Response;
	TArray<uint8> BodyChunk;
	FUnrealAIModelError Error;

	bool IsTerminal() const;
	bool ValidateShape(FString &OutError) const;
};

/** Thread-safe, non-UObject event target. Implementations must enqueue quickly and must not call back into transport.
 */
class UNREALAITRANSPORT_API IUnrealAIHttpEventSink
{
  public:
	virtual ~IUnrealAIHttpEventSink() = default;
	virtual void EnqueueHttpEvent(FUnrealAIHttpEvent &&Event) = 0;
};

/**
 * Strong ownership handle for one admitted physical transport request.
 *
 * Retaining the handle retains the admitted operation through physical
 * settlement. Releasing it does not cancel implicitly; callers must request
 * cancellation explicitly when they no longer want the operation.
 */
class UNREALAITRANSPORT_API IUnrealAIHttpRequestHandle
{
  public:
	virtual ~IUnrealAIHttpRequestHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	/** True only after every native callback source for this request is closed. */
	virtual bool IsPhysicallySettled() const
	{
		return false;
	}
	/**
	 * Requests cancellation without waiting for native or network completion.
	 * Core may call this from any thread, including retirement and shutdown
	 * callers. Implementations must be thread-safe, idempotent, prompt,
	 * bounded, and nonblocking. Physical completion remains asynchronous and
	 * is reported exactly once through the request's event sink.
	 */
	virtual void Cancel() = 0;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Test-only physical cancellation edge count; unsupported handles return -1. */
	virtual int32 GetPhysicalCancellationCountForTesting() const
	{
		return -1;
	}
	/** Test-only native response-start seam; unsupported handles return false. */
	virtual bool EmitResponseStartedForTesting()
	{
		return false;
	}
	/** Test-only successful native completion seam; unsupported handles return false. */
	virtual bool CompleteSuccessfullyForTesting()
	{
		return false;
	}
#endif
};

struct UNREALAITRANSPORT_API FUnrealAIHttpTransportOptions final
{
	static constexpr int32 MaxActiveRequestsLimit = 256;
	static constexpr double MinCancellationPollSeconds = 0.01;
	static constexpr double MaxCancellationPollSeconds = 1.0;

	int32 MaxActiveRequests = 32;
	double CancellationPollSeconds = 0.05;
	/**
	 * Monotonic clock used for the end-to-end transport deadline. The system
	 * clock is installed by the transport factory when this is null.
	 */
	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Keeps the native task suspended after admission so cancellation ordering can be tested without network timing.
	 */
	bool bSuspendNativeTaskForTesting = false;
	/**
	 * Emits a native invalidation callback after complete preparation but before
	 * admission commit. The delegate portal must stage and replay it against the
	 * exact committed operation.
	 */
	bool bEmitProvisionalInvalidationBeforeCommitForTesting = false;
	/** Barrier hook after the exact state commit and before native activation or staged settlement replay. */
	TSharedPtr<TFunction<void()>, ESPMode::ThreadSafe> AfterAdmissionCommitReservedForTesting;
#endif

	bool ValidateShape(FString &OutError) const;
};

/** Credentialed HTTPS transport. Admission consumes one opaque access context exactly once. */
class UNREALAITRANSPORT_API IUnrealAIHttpTransport
{
  public:
	virtual ~IUnrealAIHttpTransport() = default;

	/**
	 * Attempts exact physical admission. A false result with a null OutHandle
	 * is a complete ownership rollback: the transport must have released
	 * EventSink and must guarantee that no callback can enter it. If any native
	 * work or callback source exists, the transport must instead return true
	 * with an exact, valid handle, even when synchronously staged events will
	 * later make the adapter reject logical admission. Every true admission
	 * closes its callback source with exactly one physical terminal event.
	 */
	virtual bool
	StartRequest(const FUnrealAIHttpRequest &Request,
				 const TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &AccessContext,
				 const TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> &EventSink,
				 const FUnrealAICancellationToken &Cancellation,
				 TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle, FString &OutError) = 0;
	/**
	 * Evaluates admitted request deadlines against the configured clock.
	 *
	 * Native transports retain bounded production polling, while this explicit
	 * seam makes fake-clock deadline tests deterministic. The default no-op
	 * preserves compatibility for custom transports that own no deadlines.
	 */
	virtual void PumpDeadlines() {}
	virtual void BeginShutdown() = 0;
};

/** Creates the native credentialed HTTPS transport for the current platform. */
UNREALAITRANSPORT_API bool IsUnrealAIPlatformHttpsTransportSupported();

/** Creates the native credentialed HTTPS transport, or a fail-closed unavailable implementation. */
UNREALAITRANSPORT_API TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe>
CreateUnrealAIPlatformHttpsTransport(const FUnrealAIHttpTransportOptions &Options = FUnrealAIHttpTransportOptions());
