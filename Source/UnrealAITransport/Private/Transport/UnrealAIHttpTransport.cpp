// Copyright UnrealOps. All Rights Reserved.

#include "Transport/UnrealAIHttpTransport.h"

#include "HAL/Platform.h"
#include "Misc/CString.h"

#if PLATFORM_MAC
#include "Transport/UnrealAIMacUrlSessionTransport.h"
#endif

namespace UE::UnrealAI::Transport::Private
{
bool FitsUtf8(const FString &Text, const int32 MaxBytes)
{
	FTCHARToUTF8 Utf8(*Text);
	return Utf8.Length() <= MaxBytes;
}

bool ContainsForbiddenTextCharacter(const FString &Text)
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

bool IsHeaderTokenCharacter(const TCHAR Character)
{
	if ((Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('A') && Character <= TEXT('Z')) ||
															   (Character >= TEXT('0') && Character <= TEXT('9')))
	{
		return true;
	}

	switch (Character)
	{
	case TEXT('!'):
	case TEXT('#'):
	case TEXT('$'):
	case TEXT('%'):
	case TEXT('&'):
	case TEXT('\''):
	case TEXT('*'):
	case TEXT('+'):
	case TEXT('-'):
	case TEXT('.'):
	case TEXT('^'):
	case TEXT('_'):
	case TEXT('`'):
	case TEXT('|'):
	case TEXT('~'):
		return true;
	default:
		return false;
	}
}

bool IsDeniedHeaderName(const FString &Name)
{
	static const TCHAR *DeniedNames[] = {TEXT("authorization"),
		TEXT("proxy-authorization"),
			 TEXT("x-api-key"),
				  TEXT("api-key"),
					   TEXT("x-goog-api-key"),
							TEXT("x-auth-token"),
								 TEXT("cookie"), TEXT("set-cookie"),
													  TEXT("host"), TEXT("content-length"),
																		 TEXT("transfer-encoding"),
																			  TEXT("connection"),
																				   TEXT("proxy-connection"),
																						TEXT("keep-alive"),
																							 TEXT("te"),
																								  TEXT("trailer"),
																									   TEXT("upgrade")};

	for (const TCHAR *DeniedName : DeniedNames)
	{
		if (Name.Equals(DeniedName, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool IsStrictRelativePath(const FString &Path)
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

bool HasDefaultResponseMetadata(const FUnrealAIHttpResponseMetadata &Metadata)
{
	return Metadata.StatusCode == 0 && Metadata.ContentType.IsEmpty() && Metadata.ProviderRequestId.IsEmpty() &&
		   Metadata.RetryAfterSeconds == 0.0f;
}
} // namespace UE::UnrealAI::Transport::Private

bool FUnrealAIHttpRequestHeader::ValidateShape(FString &OutError) const
{
	using namespace UE::UnrealAI::Transport::Private;
	OutError.Reset();
	if (Name.IsEmpty() || !FitsUtf8(Name, MaxNameBytes) || !FitsUtf8(Value, MaxValueBytes) ||
		ContainsForbiddenTextCharacter(Name) || ContainsForbiddenTextCharacter(Value))
	{
		OutError = TEXT("HTTP request header is empty, oversized, or contains a forbidden control character.");
		return false;
	}
	for (const TCHAR Character : Name)
	{
		if (!IsHeaderTokenCharacter(Character))
		{
			OutError = TEXT("HTTP request header name is not a valid ASCII token.");
			return false;
		}
	}
	if (IsDeniedHeaderName(Name))
	{
		OutError = TEXT("HTTP request header is owned by the credential or transport boundary.");
		return false;
	}
	return true;
}

bool FUnrealAIHttpRequest::ValidateShape(FString &OutError) const
{
	using namespace UE::UnrealAI::Transport::Private;
	OutError.Reset();
	FString DestinationError;
	if (!RequestId.IsValid() || !Destination.ValidateShape(DestinationError) || !Destination.EndpointOrigin.IsSecure())
	{
		OutError = TEXT("HTTP request requires a valid ID and exact secure credential destination.");
		return false;
	}
	if (Method != EUnrealAIHttpMethod::Get && Method != EUnrealAIHttpMethod::Post)
	{
		OutError = TEXT("HTTP request method is unsupported.");
		return false;
	}
	if (RelativePath.IsEmpty() || !FitsUtf8(RelativePath, MaxRelativePathBytes) || !IsStrictRelativePath(RelativePath))
	{
		OutError =
			TEXT("HTTP request path must be a bounded absolute path without authority, query, fragment, or escapes.");
		return false;
	}
	const bool bSingleBearer = CredentialPresentation == EUnrealAIHttpCredentialPresentation::AuthorizationBearer &&
							   ProtectedSecondaryHeaderName.IsEmpty() &&
							   Destination.AuthScheme != EUnrealAIAuthScheme::Anonymous &&
							   Destination.AuthScheme != EUnrealAIAuthScheme::Invalid;
	const bool bAnthropicApiKey =
		CredentialPresentation == EUnrealAIHttpCredentialPresentation::AnthropicApiKeyHeader &&
		ProtectedSecondaryHeaderName.IsEmpty() &&
		Destination.ModelProviderName ==
			TEXT("anthropic.messages") && Destination.AuthScheme == EUnrealAIAuthScheme::ApiKey &&
				 Destination.BillingMode == EUnrealAIBillingMode::ApiMetered &&
				 Destination.EndpointOrigin.ToString() == TEXT("https://api.anthropic.com");
	const bool bGeminiApiKey =
		CredentialPresentation == EUnrealAIHttpCredentialPresentation::GeminiApiKeyHeader &&
		ProtectedSecondaryHeaderName.IsEmpty() &&
		Destination.ModelProviderName ==
			TEXT("gemini.interactions") && Destination.AuthScheme == EUnrealAIAuthScheme::ApiKey &&
				 Destination.BillingMode == EUnrealAIBillingMode::ApiMetered &&
				 Destination.EndpointOrigin.ToString() == TEXT("https://generativelanguage.googleapis.com");
	FUnrealAIHttpRequestHeader ProtectedSecondaryHeader;
	ProtectedSecondaryHeader.Name = ProtectedSecondaryHeaderName;
	ProtectedSecondaryHeader.Value = TEXT("protected");
	FString ProtectedSecondaryHeaderError;
	const bool bProtectedSecondaryHeaderValid = !ProtectedSecondaryHeaderName.IsEmpty() &&
												ProtectedSecondaryHeader.ValidateShape(ProtectedSecondaryHeaderError);
	const bool bBearerWithProtectedSecondary =
#if UE_BUILD_SHIPPING || UE_SERVER
		false;
#else
		CredentialPresentation == EUnrealAIHttpCredentialPresentation::AuthorizationBearerWithProtectedSecondary &&
		Destination.AuthScheme == EUnrealAIAuthScheme::OAuthBearer &&
		Destination.BillingMode == EUnrealAIBillingMode::SubscriptionQuota && bProtectedSecondaryHeaderValid;
#endif
	if (!bSingleBearer && !bAnthropicApiKey && !bGeminiApiKey && !bBearerWithProtectedSecondary)
	{
		OutError = TEXT("HTTP request requires a compiled credential presentation compatible with its destination.");
		return false;
	}
	const bool bQueryProfileValid =
		QueryProfile == EUnrealAIHttpQueryProfile::None || (QueryProfile == EUnrealAIHttpQueryProfile::GeminiAltSse &&
															bGeminiApiKey && RelativePath == TEXT("/v1/interactions"));
	if (!bQueryProfileValid)
	{
		OutError = TEXT("HTTP request query profile is unknown or incompatible with its exact provider resource.");
		return false;
	}
	if (Headers.Num() > MaxHeaders || Body.Num() > MaxRequestBodyBytes ||
		(Method == EUnrealAIHttpMethod::Get && !Body.IsEmpty()))
	{
		OutError = TEXT("HTTP request headers or body exceed the admitted shape.");
		return false;
	}
	TSet<FString> NormalizedHeaderNames;
	const FString NormalizedProtectedSecondaryHeaderName = ProtectedSecondaryHeaderName.ToLower();
	for (const FUnrealAIHttpRequestHeader &Header : Headers)
	{
		if (!Header.ValidateShape(OutError))
		{
			return false;
		}
		FString NormalizedName = Header.Name.ToLower();
		if (NormalizedHeaderNames.Contains(NormalizedName))
		{
			OutError = TEXT("HTTP request contains a duplicate header name.");
			return false;
		}
		if (bBearerWithProtectedSecondary && NormalizedName == NormalizedProtectedSecondaryHeaderName)
		{
			OutError = TEXT("HTTP request cannot provide the transport-owned protected secondary header.");
			return false;
		}
		NormalizedHeaderNames.Add(MoveTemp(NormalizedName));
	}
	if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 || TimeoutSeconds > MaxTimeoutSeconds ||
		MaxResponseBodyBytes <= 0 || MaxResponseBodyBytes > MaxResponseBodyBytesLimit || MaxBodyChunkBytes <= 0 ||
		MaxBodyChunkBytes > MaxBodyChunkBytesLimit || MaxBodyChunkBytes > MaxResponseBodyBytes)
	{
		OutError = TEXT("HTTP request timeout or response bounds are invalid.");
		return false;
	}
	return true;
}

bool FUnrealAIHttpResponseMetadata::ValidateShape(FString &OutError) const
{
	using namespace UE::UnrealAI::Transport::Private;
	OutError.Reset();
	if (StatusCode < 100 || StatusCode > 599 || !FitsUtf8(ContentType, MaxContentTypeBytes) ||
		!FitsUtf8(ProviderRequestId, MaxProviderRequestIdBytes) || ContainsForbiddenTextCharacter(ContentType) ||
		ContainsForbiddenTextCharacter(ProviderRequestId) || !FMath::IsFinite(RetryAfterSeconds) ||
		RetryAfterSeconds < 0.0f || RetryAfterSeconds > 86400.0f)
	{
		OutError = TEXT("HTTP response metadata is invalid or exceeds its public-safe bounds.");
		return false;
	}
	return true;
}

bool FUnrealAIHttpEvent::IsTerminal() const
{
	return Kind == EUnrealAIHttpEventKind::Completed || Kind == EUnrealAIHttpEventKind::Failed ||
		   Kind == EUnrealAIHttpEventKind::Cancelled || Kind == EUnrealAIHttpEventKind::TimedOut;
}

bool FUnrealAIHttpEvent::ValidateShape(FString &OutError) const
{
	using namespace UE::UnrealAI::Transport::Private;
	OutError.Reset();
	if (!RequestId.IsValid() || Sequence == 0 || BodyChunk.Num() > FUnrealAIHttpRequest::MaxBodyChunkBytesLimit)
	{
		OutError = TEXT("HTTP event has an invalid identity, sequence, or body bound.");
		return false;
	}

	FString ErrorShape;
	switch (Kind)
	{
	case EUnrealAIHttpEventKind::ResponseStarted:
		if (!BodyChunk.IsEmpty() || Error.IsError() || !Response.ValidateShape(OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("HTTP response-start event contains an invalid payload.");
			}
			return false;
		}
		return true;
	case EUnrealAIHttpEventKind::BodyChunk:
		if (BodyChunk.IsEmpty() || Error.IsError() || !HasDefaultResponseMetadata(Response))
		{
			OutError = TEXT("HTTP body event contains invalid metadata or error state.");
			return false;
		}
		return true;
	case EUnrealAIHttpEventKind::Completed:
		if (!BodyChunk.IsEmpty() || Error.IsError() || !HasDefaultResponseMetadata(Response))
		{
			OutError = TEXT("HTTP completion event contains an invalid payload.");
			return false;
		}
		return true;
	case EUnrealAIHttpEventKind::Failed:
		if (!BodyChunk.IsEmpty() || !HasDefaultResponseMetadata(Response) || !Error.IsError() ||
			!Error.ValidateShape(ErrorShape))
		{
			OutError = TEXT("HTTP failure event contains an invalid error payload.");
			return false;
		}
		return true;
	case EUnrealAIHttpEventKind::Cancelled:
		if (!BodyChunk.IsEmpty() || !HasDefaultResponseMetadata(Response) ||
			Error.Category != EUnrealAIErrorCategory::Cancelled || !Error.ValidateShape(ErrorShape))
		{
			OutError = TEXT("HTTP cancellation event contains an invalid error payload.");
			return false;
		}
		return true;
	case EUnrealAIHttpEventKind::TimedOut:
		if (!BodyChunk.IsEmpty() || !HasDefaultResponseMetadata(Response) ||
			Error.Category != EUnrealAIErrorCategory::Timeout || !Error.ValidateShape(ErrorShape))
		{
			OutError = TEXT("HTTP timeout event contains an invalid error payload.");
			return false;
		}
		return true;
	case EUnrealAIHttpEventKind::Invalid:
	default:
		OutError = TEXT("HTTP event kind is invalid.");
		return false;
	}
}

bool FUnrealAIHttpTransportOptions::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxActiveRequests <= 0 || MaxActiveRequests > MaxActiveRequestsLimit ||
		!FMath::IsFinite(CancellationPollSeconds) || CancellationPollSeconds < MinCancellationPollSeconds ||
		CancellationPollSeconds > MaxCancellationPollSeconds)
	{
		OutError = TEXT("HTTP transport options are outside their supported bounds.");
		return false;
	}
	return true;
}

#if !PLATFORM_MAC
namespace UE::UnrealAI::Transport::Private
{
class FUnavailableAgentHttpTransport final : public IUnrealAIHttpTransport
{
  public:
	explicit FUnavailableAgentHttpTransport(FUnrealAIHttpTransportOptions InOptions) : Options(MoveTemp(InOptions)) {}

	bool StartRequest(const FUnrealAIHttpRequest &Request,
					  const TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &AccessContext,
					  const TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> &EventSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FString &OutError) override
	{
		OutHandle.Reset();
		OutError = TEXT("The native credentialed HTTPS transport is unavailable on this platform.");
		return false;
	}

	void BeginShutdown() override {}

  private:
	FUnrealAIHttpTransportOptions Options;
};
} // namespace UE::UnrealAI::Transport::Private
#endif

bool IsUnrealAIPlatformHttpsTransportSupported()
{
#if PLATFORM_MAC
	return true;
#else
	return false;
#endif
}

TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe>
CreateUnrealAIPlatformHttpsTransport(const FUnrealAIHttpTransportOptions &Options)
{
	FUnrealAIHttpTransportOptions ResolvedOptions = Options;
	if (!ResolvedOptions.Clock.IsValid())
	{
		ResolvedOptions.Clock = MakeShared<FUnrealAISystemClock, ESPMode::ThreadSafe>();
	}
#if PLATFORM_MAC
	return UE::UnrealAI::Transport::Private::CreateMacUrlSessionTransport(ResolvedOptions);
#else
	return MakeShared<UE::UnrealAI::Transport::Private::FUnavailableAgentHttpTransport, ESPMode::ThreadSafe>(
		ResolvedOptions);
#endif
}
