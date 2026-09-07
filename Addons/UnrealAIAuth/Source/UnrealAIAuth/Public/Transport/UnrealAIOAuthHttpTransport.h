// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Auth/UnrealAIProviderAccess.h"
#include "UnrealAIModelError.h"
#include "Runtime/UnrealAICancellation.h"

/** OAuth authorization/token-endpoint request encoding owned by the transport. */
enum class EUnrealAIOAuthHttpContentType : uint8
{
	Invalid,
	ApplicationJson,
	FormUrlEncoded
};

/** Closed request-method vocabulary. GET exists only for bounded provider metadata discovery. */
enum class EUnrealAIOAuthHttpMethod : uint8
{
	Invalid,
	Get,
	Post
};

/** Closed terminal vocabulary for one OAuth HTTPS exchange. */
enum class EUnrealAIOAuthHttpTerminalKind : uint8
{
	Invalid,
	Succeeded,
	Failed,
	Cancelled,
	TimedOut
};

/** Closed machine error vocabulary. Provider response bodies never become diagnostics. */
enum class EUnrealAIOAuthHttpErrorCode : uint8
{
	None,
	TransportUnavailable,
	TransportFailed,
	RedirectRejected,
	AuthenticationChallengeRejected,
	InvalidResponse,
	ResponseTooLarge,
	Cancelled,
	TimedOut
};

/** Typed, public-safe OAuth transport error. */
struct UNREALAIAUTH_API FUnrealAIOAuthHttpError final
{
	EUnrealAIErrorCategory Category = EUnrealAIErrorCategory::None;
	EUnrealAIOAuthHttpErrorCode Code = EUnrealAIOAuthHttpErrorCode::None;
	bool bRetryable = false;

	bool IsError() const
	{
		return Category != EUnrealAIErrorCategory::None;
	}
	bool ValidateShape(FString &OutError) const;
};

/**
 * Ephemeral consumer for one sensitive request or response body. Implementations must parse or copy only what they
 * require during the call and must not log the byte view. The owning payload is wiped immediately after the call.
 */
class UNREALAIAUTH_API IUnrealAIOAuthHttpSecretConsumer
{
  public:
	virtual ~IUnrealAIOAuthHttpSecretConsumer() = default;

  protected:
	virtual bool ConsumeOAuthHttpSecret(TConstArrayView<uint8> Secret) = 0;

	friend class FUnrealAIOAuthHttpSecretPayload;
};

/**
 * Move-only, single-consumption wrapper around FUnrealAISecretValue. It deliberately exposes no plaintext getter.
 *
 * FUnrealAISecretValue grants this type a narrow friend relationship in AutonomousAgentsCore.
 */
class UNREALAIAUTH_API FUnrealAIOAuthHttpSecretPayload final
{
  public:
	FUnrealAIOAuthHttpSecretPayload() = default;
	~FUnrealAIOAuthHttpSecretPayload();
	FUnrealAIOAuthHttpSecretPayload(const FUnrealAIOAuthHttpSecretPayload &) = delete;
	FUnrealAIOAuthHttpSecretPayload &operator=(const FUnrealAIOAuthHttpSecretPayload &) = delete;
	FUnrealAIOAuthHttpSecretPayload(FUnrealAIOAuthHttpSecretPayload &&Other) noexcept;
	FUnrealAIOAuthHttpSecretPayload &operator=(FUnrealAIOAuthHttpSecretPayload &&Other) noexcept;

	static bool TryCreate(FUnrealAISecretValue &&Secret, FUnrealAIOAuthHttpSecretPayload &OutPayload,
						  FString &OutError);
	bool IsSet() const;
	int32 Num() const;
	FString GetRedactedDisplay() const;
	/** Consumes and wipes the payload whether the consumer succeeds or fails. */
	bool TryConsume(IUnrealAIOAuthHttpSecretConsumer &Consumer);
	void Reset();

  private:
	FUnrealAISecretValue Secret;
};

/**
 * One bounded OAuth HTTPS request. The origin is canonical and exact; RelativePath cannot carry authority, query,
 * fragment, userinfo, or percent escapes. POST owns one move-only secret body. GET owns no body and exists only for
 * provider metadata discovery.
 */
class UNREALAIAUTH_API FUnrealAIOAuthHttpRequest final
{
  public:
	static constexpr int32 MaxRelativePathBytes = 2048;
	static constexpr int32 MaxResponseBodyBytesLimit = FUnrealAISecretValue::MaxSecretBytes;
	static constexpr double MaxTimeoutSeconds = 30.0 * 60.0;

	FUnrealAIOAuthHttpRequest() = default;
	~FUnrealAIOAuthHttpRequest() = default;
	FUnrealAIOAuthHttpRequest(const FUnrealAIOAuthHttpRequest &) = delete;
	FUnrealAIOAuthHttpRequest &operator=(const FUnrealAIOAuthHttpRequest &) = delete;
	FUnrealAIOAuthHttpRequest(FUnrealAIOAuthHttpRequest &&Other) noexcept = default;
	FUnrealAIOAuthHttpRequest &operator=(FUnrealAIOAuthHttpRequest &&Other) noexcept = default;

	static bool TryCreate(const FUnrealAIRequestId &RequestId, const FUnrealAIEndpointOrigin &EndpointOrigin,
						  FString RelativePath, EUnrealAIOAuthHttpContentType ContentType, FUnrealAISecretValue &&Body,
						  double TimeoutSeconds, int32 MaxResponseBodyBytes, FUnrealAIOAuthHttpRequest &OutRequest,
						  FString &OutError);
	static bool TryCreateGet(const FUnrealAIRequestId &RequestId, const FUnrealAIEndpointOrigin &EndpointOrigin,
							 FString RelativePath, double TimeoutSeconds, int32 MaxResponseBodyBytes,
							 FUnrealAIOAuthHttpRequest &OutRequest, FString &OutError);

	const FUnrealAIRequestId &GetRequestId() const;
	const FUnrealAIEndpointOrigin &GetEndpointOrigin() const;
	const FString &GetRelativePath() const;
	EUnrealAIOAuthHttpMethod GetMethod() const;
	EUnrealAIOAuthHttpContentType GetContentType() const;
	double GetTimeoutSeconds() const;
	int32 GetMaxResponseBodyBytes() const;
	int32 GetBodyBytes() const;
	/** Consumes and wipes the request body whether the transport applicator succeeds or fails. */
	bool TryConsumeBody(IUnrealAIOAuthHttpSecretConsumer &Consumer);
	bool ValidateShape(FString &OutError) const;

  private:
	FUnrealAIRequestId RequestId;
	FUnrealAIEndpointOrigin EndpointOrigin;
	FString RelativePath;
	EUnrealAIOAuthHttpMethod Method = EUnrealAIOAuthHttpMethod::Invalid;
	EUnrealAIOAuthHttpContentType ContentType = EUnrealAIOAuthHttpContentType::Invalid;
	FUnrealAIOAuthHttpSecretPayload Body;
	double TimeoutSeconds = 0.0;
	int32 MaxResponseBodyBytes = 0;
};

/** Bounded response metadata. Arbitrary response headers and cookies never cross this boundary. */
struct UNREALAIAUTH_API FUnrealAIOAuthHttpResponseMetadata final
{
	static constexpr int32 MaxContentTypeBytes = 512;
	static constexpr int32 MaxProviderRequestIdBytes = 1024;

	int32 StatusCode = 0;
	FString ContentType;
	FString ProviderRequestId;
	float RetryAfterSeconds = 0.0f;

	bool ValidateShape(FString &OutError) const;
};

/** Move-only terminal result. HTTP 4xx/5xx responses are successful transport exchanges with their status retained. */
class UNREALAIAUTH_API FUnrealAIOAuthHttpResult final
{
  public:
	FUnrealAIOAuthHttpResult() = default;
	~FUnrealAIOAuthHttpResult() = default;
	FUnrealAIOAuthHttpResult(const FUnrealAIOAuthHttpResult &) = delete;
	FUnrealAIOAuthHttpResult &operator=(const FUnrealAIOAuthHttpResult &) = delete;
	FUnrealAIOAuthHttpResult(FUnrealAIOAuthHttpResult &&Other) noexcept = default;
	FUnrealAIOAuthHttpResult &operator=(FUnrealAIOAuthHttpResult &&Other) noexcept = default;

	static bool TryCreateResponse(const FUnrealAIRequestId &RequestId,
								  const FUnrealAIOAuthHttpResponseMetadata &Metadata, FUnrealAISecretValue &&Body,
								  FUnrealAIOAuthHttpResult &OutResult, FString &OutError);
	static bool TryCreateError(const FUnrealAIRequestId &RequestId, EUnrealAIOAuthHttpTerminalKind TerminalKind,
							   const FUnrealAIOAuthHttpError &Error, FUnrealAIOAuthHttpResult &OutResult,
							   FString &OutError);

	const FUnrealAIRequestId &GetRequestId() const;
	EUnrealAIOAuthHttpTerminalKind GetTerminalKind() const;
	const FUnrealAIOAuthHttpResponseMetadata &GetResponseMetadata() const;
	const FUnrealAIOAuthHttpError &GetError() const;
	bool IsTransportSuccess() const;
	bool IsHttpSuccess() const;
	bool HasBody() const;
	int32 GetBodyBytes() const;
	/** Consumes and wipes the response body whether the consumer succeeds or fails. */
	bool TryConsumeBody(IUnrealAIOAuthHttpSecretConsumer &Consumer);
	bool ValidateShape(FString &OutError) const;

  private:
	FUnrealAIRequestId RequestId;
	EUnrealAIOAuthHttpTerminalKind TerminalKind = EUnrealAIOAuthHttpTerminalKind::Invalid;
	FUnrealAIOAuthHttpResponseMetadata Response;
	FUnrealAIOAuthHttpSecretPayload Body;
	FUnrealAIOAuthHttpError Error;
};

/** Thread-safe completion target. It receives exactly one terminal result on a native transport thread. */
class UNREALAIAUTH_API IUnrealAIOAuthHttpCompletionSink
{
  public:
	virtual ~IUnrealAIOAuthHttpCompletionSink() = default;
	virtual void CompleteOAuthHttpRequest(FUnrealAIOAuthHttpResult &&Result) = 0;
};

/** Weak cancellation handle for one admitted OAuth request. */
class UNREALAIAUTH_API IUnrealAIOAuthHttpRequestHandle
{
  public:
	virtual ~IUnrealAIOAuthHttpRequestHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	/**
	 * Requests cancellation without waiting for native or network completion.
	 * Callers may invoke this from any thread and more than once.
	 * Implementations must be thread-safe, idempotent, prompt, bounded, and
	 * nonblocking. Completion remains asynchronous and is reported exactly
	 * once through the request's completion sink.
	 */
	virtual void Cancel() = 0;
};

struct UNREALAIAUTH_API FUnrealAIOAuthHttpTransportOptions final
{
	static constexpr int32 MaxActiveRequestsLimit = 64;
	static constexpr double MinCancellationPollSeconds = 0.01;
	static constexpr double MaxCancellationPollSeconds = 1.0;

	int32 MaxActiveRequests = 8;
	double CancellationPollSeconds = 0.05;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Keeps a native task suspended after admission for deterministic cancel/shutdown race tests. */
	bool bSuspendNativeTaskForTesting = false;
#endif

	bool ValidateShape(FString &OutError) const;
};

/**
 * Injectable OAuth HTTPS seam. Implementations consume the move-only request body on admission and complete exactly
 * once. Provider tests may inject an in-memory fake without using the native implementation.
 */
class UNREALAIAUTH_API IUnrealAIOAuthHttpTransport
{
  public:
	virtual ~IUnrealAIOAuthHttpTransport() = default;

	virtual bool StartRequest(FUnrealAIOAuthHttpRequest &&Request,
							  const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
							  const FUnrealAICancellationToken &Cancellation,
							  TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
							  FString &OutError) = 0;
	virtual void BeginShutdown() = 0;
};

/** Returns whether the current platform has the hardened native OAuth HTTPS implementation. */
UNREALAIAUTH_API bool IsAgentPlatformOAuthHttpsTransportSupported();

/** Creates the hardened native OAuth HTTPS transport, or a fail-closed unavailable implementation. */
UNREALAIAUTH_API TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> CreateAgentPlatformOAuthHttpsTransport(
	const FUnrealAIOAuthHttpTransportOptions &Options = FUnrealAIOAuthHttpTransportOptions());
