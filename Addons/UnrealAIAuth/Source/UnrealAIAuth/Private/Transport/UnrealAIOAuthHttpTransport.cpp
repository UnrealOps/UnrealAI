// Copyright EngineWorks. All Rights Reserved.

#include "Transport/UnrealAIOAuthHttpTransport.h"

#include "Misc/CString.h"

#if PLATFORM_MAC
#include "Transport/UnrealAIMacOAuthHttpTransport.h"
#endif

namespace UE::UnrealAI::Transport::Private
{
bool OAuthFitsUtf8(const FString &Text, const int32 MaxBytes)
{
	FTCHARToUTF8 Utf8(*Text);
	return Utf8.Length() <= MaxBytes;
}

bool OAuthContainsForbiddenTextCharacter(const FString &Text)
{
	for (const TCHAR Character : Text)
	{
		if (Character == TEXT('\r') || Character == TEXT('\n') || Character == TEXT('\0'))
		{
			return true;
		}
	}
	return false;
}

bool IsStrictOAuthRelativePath(const FString &Path)
{
	if (!Path.StartsWith(
			TEXT("/")) ||
			Path.StartsWith(TEXT("//")) ||
							Path.Contains(TEXT("..")) ||
										  Path.Contains(TEXT("%")) ||
														Path.Contains(TEXT("?")) ||
																	  Path.Contains(TEXT("#")) ||
																					Path.Contains(TEXT("\\")))
	{
		return false;
	}

	for (const TCHAR Character : Path)
	{
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('A') && Character <= TEXT('Z')) ||
																   (Character >= TEXT('0') && Character <= TEXT('9'));
		if (!bAlphaNumeric &&
			Character != TEXT('/') && Character != TEXT('-') &&
														Character != TEXT('_') &&
																		  Character != TEXT('.') &&
																							Character != TEXT('~'))
		{
			return false;
		}
	}
	return true;
}

bool ErrorCategoryMatchesCode(const FUnrealAIOAuthHttpError &Error)
{
	switch (Error.Code)
	{
	case EUnrealAIOAuthHttpErrorCode::None:
		return Error.Category == EUnrealAIErrorCategory::None && !Error.bRetryable;
	case EUnrealAIOAuthHttpErrorCode::TransportUnavailable:
		return Error.Category == EUnrealAIErrorCategory::UnsupportedCapability && !Error.bRetryable;
	case EUnrealAIOAuthHttpErrorCode::TransportFailed:
		return Error.Category == EUnrealAIErrorCategory::Transport;
	case EUnrealAIOAuthHttpErrorCode::RedirectRejected:
	case EUnrealAIOAuthHttpErrorCode::AuthenticationChallengeRejected:
	case EUnrealAIOAuthHttpErrorCode::ResponseTooLarge:
		return Error.Category == EUnrealAIErrorCategory::Transport && !Error.bRetryable;
	case EUnrealAIOAuthHttpErrorCode::InvalidResponse:
		return Error.Category == EUnrealAIErrorCategory::ProviderProtocol && !Error.bRetryable;
	case EUnrealAIOAuthHttpErrorCode::Cancelled:
		return Error.Category == EUnrealAIErrorCategory::Cancelled && !Error.bRetryable;
	case EUnrealAIOAuthHttpErrorCode::TimedOut:
		return Error.Category == EUnrealAIErrorCategory::Timeout;
	default:
		return false;
	}
}

#if !PLATFORM_MAC
class FUnavailableAgentOAuthHttpTransport final : public IUnrealAIOAuthHttpTransport
{
  public:
	explicit FUnavailableAgentOAuthHttpTransport(FUnrealAIOAuthHttpTransportOptions InOptions)
		: Options(MoveTemp(InOptions))
	{
	}

	bool StartRequest(FUnrealAIOAuthHttpRequest &&Request,
					  const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FString &OutError) override
	{
		(void)Request;
		(void)CompletionSink;
		(void)Cancellation;
		OutHandle.Reset();
		OutError = TEXT("The native OAuth HTTPS transport is unavailable on this platform.");
		return false;
	}

	void BeginShutdown() override {}

  private:
	FUnrealAIOAuthHttpTransportOptions Options;
};
#endif
} // namespace UE::UnrealAI::Transport::Private

bool FUnrealAIOAuthHttpError::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!UE::UnrealAI::Transport::Private::ErrorCategoryMatchesCode(*this))
	{
		OutError = TEXT("OAuth HTTP error category, code, and retry policy are inconsistent.");
		return false;
	}
	return true;
}

FUnrealAIOAuthHttpSecretPayload::~FUnrealAIOAuthHttpSecretPayload()
{
	Reset();
}

FUnrealAIOAuthHttpSecretPayload::FUnrealAIOAuthHttpSecretPayload(FUnrealAIOAuthHttpSecretPayload &&Other) noexcept
	: Secret(MoveTemp(Other.Secret))
{
}

FUnrealAIOAuthHttpSecretPayload &
FUnrealAIOAuthHttpSecretPayload::operator=(FUnrealAIOAuthHttpSecretPayload &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		this->Secret = MoveTemp(Other.Secret);
	}
	return *this;
}

bool FUnrealAIOAuthHttpSecretPayload::TryCreate(FUnrealAISecretValue &&InSecret,
												FUnrealAIOAuthHttpSecretPayload &OutPayload, FString &OutError)
{
	OutError.Reset();
	OutPayload.Reset();
	if (!InSecret.IsSet() || InSecret.Num() > FUnrealAISecretValue::MaxSecretBytes)
	{
		InSecret.Reset();
		OutError = TEXT("OAuth HTTP secret payload must be non-empty and bounded.");
		return false;
	}
	OutPayload.Secret = MoveTemp(InSecret);
	return true;
}

bool FUnrealAIOAuthHttpSecretPayload::IsSet() const
{
	return Secret.IsSet();
}

int32 FUnrealAIOAuthHttpSecretPayload::Num() const
{
	return Secret.Num();
}

FString FUnrealAIOAuthHttpSecretPayload::GetRedactedDisplay() const
{
	return Secret.GetRedactedDisplay();
}

bool FUnrealAIOAuthHttpSecretPayload::TryConsume(IUnrealAIOAuthHttpSecretConsumer &Consumer)
{
	if (!Secret.IsSet())
	{
		return false;
	}
	FUnrealAISecretValue OneShotSecret = MoveTemp(Secret);
	const bool bConsumed = Consumer.ConsumeOAuthHttpSecret(OneShotSecret.View());
	OneShotSecret.Reset();
	return bConsumed;
}

void FUnrealAIOAuthHttpSecretPayload::Reset()
{
	Secret.Reset();
}

bool FUnrealAIOAuthHttpRequest::TryCreate(const FUnrealAIRequestId &InRequestId,
										  const FUnrealAIEndpointOrigin &InEndpointOrigin, FString InRelativePath,
										  const EUnrealAIOAuthHttpContentType InContentType,
										  FUnrealAISecretValue &&InBody, const double InTimeoutSeconds,
										  const int32 InMaxResponseBodyBytes, FUnrealAIOAuthHttpRequest &OutRequest,
										  FString &OutError)
{
	OutRequest = FUnrealAIOAuthHttpRequest();
	OutError.Reset();

	FUnrealAIOAuthHttpRequest Candidate;
	Candidate.RequestId = InRequestId;
	Candidate.EndpointOrigin = InEndpointOrigin;
	Candidate.RelativePath = MoveTemp(InRelativePath);
	Candidate.Method = EUnrealAIOAuthHttpMethod::Post;
	Candidate.ContentType = InContentType;
	Candidate.TimeoutSeconds = InTimeoutSeconds;
	Candidate.MaxResponseBodyBytes = InMaxResponseBodyBytes;
	if (!FUnrealAIOAuthHttpSecretPayload::TryCreate(MoveTemp(InBody), Candidate.Body, OutError) ||
		!Candidate.ValidateShape(OutError))
	{
		return false;
	}
	OutRequest = MoveTemp(Candidate);
	return true;
}

bool FUnrealAIOAuthHttpRequest::TryCreateGet(const FUnrealAIRequestId &InRequestId,
											 const FUnrealAIEndpointOrigin &InEndpointOrigin, FString InRelativePath,
											 const double InTimeoutSeconds, const int32 InMaxResponseBodyBytes,
											 FUnrealAIOAuthHttpRequest &OutRequest, FString &OutError)
{
	OutRequest = FUnrealAIOAuthHttpRequest();
	OutError.Reset();

	FUnrealAIOAuthHttpRequest Candidate;
	Candidate.RequestId = InRequestId;
	Candidate.EndpointOrigin = InEndpointOrigin;
	Candidate.RelativePath = MoveTemp(InRelativePath);
	Candidate.Method = EUnrealAIOAuthHttpMethod::Get;
	Candidate.ContentType = EUnrealAIOAuthHttpContentType::Invalid;
	Candidate.TimeoutSeconds = InTimeoutSeconds;
	Candidate.MaxResponseBodyBytes = InMaxResponseBodyBytes;
	if (!Candidate.ValidateShape(OutError))
	{
		return false;
	}
	OutRequest = MoveTemp(Candidate);
	return true;
}

const FUnrealAIRequestId &FUnrealAIOAuthHttpRequest::GetRequestId() const
{
	return RequestId;
}

const FUnrealAIEndpointOrigin &FUnrealAIOAuthHttpRequest::GetEndpointOrigin() const
{
	return EndpointOrigin;
}

const FString &FUnrealAIOAuthHttpRequest::GetRelativePath() const
{
	return RelativePath;
}

EUnrealAIOAuthHttpMethod FUnrealAIOAuthHttpRequest::GetMethod() const
{
	return Method;
}

EUnrealAIOAuthHttpContentType FUnrealAIOAuthHttpRequest::GetContentType() const
{
	return ContentType;
}

double FUnrealAIOAuthHttpRequest::GetTimeoutSeconds() const
{
	return TimeoutSeconds;
}

int32 FUnrealAIOAuthHttpRequest::GetMaxResponseBodyBytes() const
{
	return MaxResponseBodyBytes;
}

int32 FUnrealAIOAuthHttpRequest::GetBodyBytes() const
{
	return Body.Num();
}

bool FUnrealAIOAuthHttpRequest::ValidateShape(FString &OutError) const
{
	using namespace UE::UnrealAI::Transport::Private;
	OutError.Reset();
	if (!RequestId.IsValid() || !EndpointOrigin.IsValid() || !EndpointOrigin.IsSecure())
	{
		OutError = TEXT("OAuth HTTP request requires a valid request ID and exact HTTPS origin.");
		return false;
	}
	if (RelativePath.IsEmpty() || !OAuthFitsUtf8(RelativePath, MaxRelativePathBytes) ||
		!IsStrictOAuthRelativePath(RelativePath))
	{
		OutError =
			TEXT("OAuth HTTP request path must exclude authority, query, fragment, userinfo, and percent escapes.");
		return false;
	}
	const bool bValidGet =
		Method == EUnrealAIOAuthHttpMethod::Get && ContentType == EUnrealAIOAuthHttpContentType::Invalid &&
		!Body.IsSet() &&
		RelativePath.StartsWith(TEXT("/.well-known/")) && RelativePath.Len() > FCString::Strlen(TEXT("/.well-known/"));
	const bool bValidPost = Method == EUnrealAIOAuthHttpMethod::Post &&
							(ContentType == EUnrealAIOAuthHttpContentType::ApplicationJson ||
							 ContentType == EUnrealAIOAuthHttpContentType::FormUrlEncoded) &&
							Body.IsSet() && Body.Num() <= FUnrealAISecretValue::MaxSecretBytes;
	if (!bValidGet && !bValidPost)
	{
		OutError = TEXT("OAuth HTTP request requires either a bodyless metadata GET or a bounded secret POST.");
		return false;
	}
	if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 || TimeoutSeconds > MaxTimeoutSeconds ||
		MaxResponseBodyBytes <= 0 || MaxResponseBodyBytes > MaxResponseBodyBytesLimit)
	{
		OutError = TEXT("OAuth HTTP request timeout or response bound is invalid.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthHttpRequest::TryConsumeBody(IUnrealAIOAuthHttpSecretConsumer &Consumer)
{
	return Body.TryConsume(Consumer);
}

bool FUnrealAIOAuthHttpResponseMetadata::ValidateShape(FString &OutError) const
{
	using namespace UE::UnrealAI::Transport::Private;
	OutError.Reset();
	if (StatusCode < 100 || StatusCode > 599 || !OAuthFitsUtf8(ContentType, MaxContentTypeBytes) ||
		!OAuthFitsUtf8(ProviderRequestId, MaxProviderRequestIdBytes) ||
		OAuthContainsForbiddenTextCharacter(ContentType) || OAuthContainsForbiddenTextCharacter(ProviderRequestId) ||
		!FMath::IsFinite(RetryAfterSeconds) || RetryAfterSeconds < 0.0f || RetryAfterSeconds > 86400.0f)
	{
		OutError = TEXT("OAuth HTTP response metadata is invalid or exceeds its public-safe bounds.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthHttpResult::TryCreateResponse(const FUnrealAIRequestId &InRequestId,
												 const FUnrealAIOAuthHttpResponseMetadata &Metadata,
												 FUnrealAISecretValue &&InBody, FUnrealAIOAuthHttpResult &OutResult,
												 FString &OutError)
{
	OutResult = FUnrealAIOAuthHttpResult();
	OutError.Reset();
	FUnrealAIOAuthHttpResult Candidate;
	Candidate.RequestId = InRequestId;
	Candidate.TerminalKind = EUnrealAIOAuthHttpTerminalKind::Succeeded;
	Candidate.Response = Metadata;
	if (InBody.IsSet() && !FUnrealAIOAuthHttpSecretPayload::TryCreate(MoveTemp(InBody), Candidate.Body, OutError))
	{
		return false;
	}
	if (!Candidate.ValidateShape(OutError))
	{
		return false;
	}
	OutResult = MoveTemp(Candidate);
	return true;
}

bool FUnrealAIOAuthHttpResult::TryCreateError(const FUnrealAIRequestId &InRequestId,
											  const EUnrealAIOAuthHttpTerminalKind InTerminalKind,
											  const FUnrealAIOAuthHttpError &InError,
											  FUnrealAIOAuthHttpResult &OutResult, FString &OutError)
{
	OutResult = FUnrealAIOAuthHttpResult();
	OutError.Reset();
	FUnrealAIOAuthHttpResult Candidate;
	Candidate.RequestId = InRequestId;
	Candidate.TerminalKind = InTerminalKind;
	Candidate.Error = InError;
	if (!Candidate.ValidateShape(OutError))
	{
		return false;
	}
	OutResult = MoveTemp(Candidate);
	return true;
}

const FUnrealAIRequestId &FUnrealAIOAuthHttpResult::GetRequestId() const
{
	return RequestId;
}

EUnrealAIOAuthHttpTerminalKind FUnrealAIOAuthHttpResult::GetTerminalKind() const
{
	return TerminalKind;
}

const FUnrealAIOAuthHttpResponseMetadata &FUnrealAIOAuthHttpResult::GetResponseMetadata() const
{
	return Response;
}

const FUnrealAIOAuthHttpError &FUnrealAIOAuthHttpResult::GetError() const
{
	return Error;
}

bool FUnrealAIOAuthHttpResult::IsTransportSuccess() const
{
	return TerminalKind == EUnrealAIOAuthHttpTerminalKind::Succeeded;
}

bool FUnrealAIOAuthHttpResult::IsHttpSuccess() const
{
	return IsTransportSuccess() && Response.StatusCode >= 200 && Response.StatusCode <= 299;
}

bool FUnrealAIOAuthHttpResult::HasBody() const
{
	return Body.IsSet();
}

int32 FUnrealAIOAuthHttpResult::GetBodyBytes() const
{
	return Body.Num();
}

bool FUnrealAIOAuthHttpResult::TryConsumeBody(IUnrealAIOAuthHttpSecretConsumer &Consumer)
{
	return Body.TryConsume(Consumer);
}

bool FUnrealAIOAuthHttpResult::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString NestedError;
	if (!RequestId.IsValid())
	{
		OutError = TEXT("OAuth HTTP terminal result requires a valid request ID.");
		return false;
	}
	if (TerminalKind == EUnrealAIOAuthHttpTerminalKind::Succeeded)
	{
		if (!Response.ValidateShape(NestedError) || Error.IsError() || Error.Code != EUnrealAIOAuthHttpErrorCode::None)
		{
			OutError = TEXT("OAuth HTTP response result contains invalid metadata or error state.");
			return false;
		}
		return true;
	}

	if (Response.StatusCode != 0 || !Response.ContentType.IsEmpty() || !Response.ProviderRequestId.IsEmpty() ||
		Response.RetryAfterSeconds != 0.0f || Body.IsSet() || !Error.ValidateShape(NestedError) || !Error.IsError())
	{
		OutError = TEXT("OAuth HTTP failure result contains response data or an invalid typed error.");
		return false;
	}

	switch (TerminalKind)
	{
	case EUnrealAIOAuthHttpTerminalKind::Failed:
		if (Error.Code == EUnrealAIOAuthHttpErrorCode::Cancelled || Error.Code == EUnrealAIOAuthHttpErrorCode::TimedOut)
		{
			OutError = TEXT("OAuth HTTP failure terminal cannot carry cancellation or timeout.");
			return false;
		}
		return true;
	case EUnrealAIOAuthHttpTerminalKind::Cancelled:
		if (Error.Code != EUnrealAIOAuthHttpErrorCode::Cancelled)
		{
			OutError = TEXT("OAuth HTTP cancellation terminal requires the cancellation error.");
			return false;
		}
		return true;
	case EUnrealAIOAuthHttpTerminalKind::TimedOut:
		if (Error.Code != EUnrealAIOAuthHttpErrorCode::TimedOut)
		{
			OutError = TEXT("OAuth HTTP timeout terminal requires the timeout error.");
			return false;
		}
		return true;
	case EUnrealAIOAuthHttpTerminalKind::Invalid:
	case EUnrealAIOAuthHttpTerminalKind::Succeeded:
	default:
		OutError = TEXT("OAuth HTTP terminal kind is invalid.");
		return false;
	}
}

bool FUnrealAIOAuthHttpTransportOptions::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxActiveRequests <= 0 || MaxActiveRequests > MaxActiveRequestsLimit ||
		!FMath::IsFinite(CancellationPollSeconds) || CancellationPollSeconds < MinCancellationPollSeconds ||
		CancellationPollSeconds > MaxCancellationPollSeconds)
	{
		OutError = TEXT("OAuth HTTP transport options are outside their supported bounds.");
		return false;
	}
	return true;
}

bool IsAgentPlatformOAuthHttpsTransportSupported()
{
#if PLATFORM_MAC
	return true;
#else
	return false;
#endif
}

TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe>
CreateAgentPlatformOAuthHttpsTransport(const FUnrealAIOAuthHttpTransportOptions &Options)
{
#if PLATFORM_MAC
	return UE::UnrealAI::Transport::Private::CreateMacOAuthHttpTransport(Options);
#else
	return MakeShared<UE::UnrealAI::Transport::Private::FUnavailableAgentOAuthHttpTransport, ESPMode::ThreadSafe>(
		Options);
#endif
}
