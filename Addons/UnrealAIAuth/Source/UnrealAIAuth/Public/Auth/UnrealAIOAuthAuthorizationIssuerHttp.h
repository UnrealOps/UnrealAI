// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"
#include "CoreMinimal.h"

/** Closed HTTP method vocabulary used by the exact OAuth issuer transport. */
enum class EUnrealAIOAuthIssuerHttpMethod : uint8
{
	Invalid,
	Get,
	PostForm
};

/** One bounded, credential-free or form-secret issuer request. ExactUrl is trusted provider configuration. */
struct UNREALAIAUTH_API FUnrealAIOAuthIssuerHttpRequest final
{
	static constexpr int32 MaxRequestBodyBytes = 64 * 1024;
	static constexpr int32 MaxResponseBodyBytesLimit = 512 * 1024;

	FString ExactUrl;
	EUnrealAIOAuthIssuerHttpMethod Method = EUnrealAIOAuthIssuerHttpMethod::Invalid;
	TArray<uint8> FormBody;
	int32 MaxResponseBodyBytes = 64 * 1024;

	FUnrealAIOAuthIssuerHttpRequest() = default;
	~FUnrealAIOAuthIssuerHttpRequest();
	FUnrealAIOAuthIssuerHttpRequest(const FUnrealAIOAuthIssuerHttpRequest &) = delete;
	FUnrealAIOAuthIssuerHttpRequest &operator=(const FUnrealAIOAuthIssuerHttpRequest &) = delete;
	FUnrealAIOAuthIssuerHttpRequest(FUnrealAIOAuthIssuerHttpRequest &&Other) noexcept;
	FUnrealAIOAuthIssuerHttpRequest &operator=(FUnrealAIOAuthIssuerHttpRequest &&Other) noexcept;

	bool ValidateShape(FString &OutError) const;
	void Reset();
};

/** Bounded raw issuer response. Body and provider headers never cross into account or diagnostic DTOs. */
struct UNREALAIAUTH_API FUnrealAIOAuthIssuerHttpResponse final
{
	FUnrealAIOAuthIssuerHttpResponse() = default;
	~FUnrealAIOAuthIssuerHttpResponse();
	FUnrealAIOAuthIssuerHttpResponse(const FUnrealAIOAuthIssuerHttpResponse &) = delete;
	FUnrealAIOAuthIssuerHttpResponse &operator=(const FUnrealAIOAuthIssuerHttpResponse &) = delete;
	FUnrealAIOAuthIssuerHttpResponse(FUnrealAIOAuthIssuerHttpResponse &&Other) noexcept;
	FUnrealAIOAuthIssuerHttpResponse &operator=(FUnrealAIOAuthIssuerHttpResponse &&Other) noexcept;

	int32 StatusCode = 0;
	FString ExactEffectiveUrl;
	FString ContentType;
	FString CacheControl;
	TArray<uint8> Body;

	bool ValidateShape(const FUnrealAIOAuthIssuerHttpRequest &Request, FString &OutError) const;
	void Reset();
};

/**
 * Exact, redirect-rejecting issuer HTTP boundary.
 *
 * Execute is synchronous only because the retained OAuth coordinator owns a bounded worker. Implementations must
 * observe Context, physically cancel and drain their native request before returning on cancellation/timeout, never
 * follow a redirect, and never log or retain FormBody or response Body bytes.
 */
class UNREALAIAUTH_API IUnrealAIOAuthIssuerHttpClient
{
  public:
	virtual ~IUnrealAIOAuthIssuerHttpClient() = default;
	virtual bool Execute(const FUnrealAIOAuthAuthorizationOperationContext &Context,
						 const FUnrealAIOAuthIssuerHttpRequest &Request, FUnrealAIOAuthIssuerHttpResponse &OutResponse,
						 FUnrealAIProviderAccessError &OutError) = 0;
};

/** Strict response and cache bounds for the production OAuth issuer adapter. */
struct UNREALAIAUTH_API FUnrealAIOAuthAuthorizationIssuerHttpConfig final
{
	int32 MaxDiscoveryResponseBytes = 64 * 1024;
	int32 MaxJwksResponseBytes = 256 * 1024;
	int32 MaxTokenResponseBytes = 64 * 1024;
	int32 MaxRevocationResponseBytes = 8 * 1024;
	int64 MaxAccessTokenLifetimeSeconds = 24 * 60 * 60;

	bool ValidateShape(FString &OutError) const;
};

/** Production strict discovery/JWKS/token/revocation adapter over an exact platform HTTP client. */
class UNREALAIAUTH_API FUnrealAIOAuthAuthorizationIssuerHttp final : public IUnrealAIOAuthAuthorizationIssuer
{
  public:
	FUnrealAIOAuthAuthorizationIssuerHttp(TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe> InHttpClient,
										  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
										  const FUnrealAIOAuthAuthorizationIssuerHttpConfig &InConfig = {});

	bool Discover(const FUnrealAIOAuthAuthorizationOperationContext &Context, FStringView ExactDiscoveryEndpoint,
				  FUnrealAIOidcDiscoveryDocument &OutDocument, FUnrealAIProviderAccessError &OutError) override;
	bool FetchJsonWebKeys(const FUnrealAIOAuthAuthorizationOperationContext &Context, FStringView ExactJwksEndpoint,
						  FUnrealAIOidcJsonWebKeySet &OutKeySet, FUnrealAIProviderAccessError &OutError) override;
	bool ExchangeAuthorizationCode(const FUnrealAIOAuthAuthorizationOperationContext &Context,
								   FStringView ExactTokenEndpoint, TConstArrayView<uint8> FormBody,
								   FUnrealAIOAuthAuthorizationCodeResponse &OutResponse,
								   FUnrealAIProviderAccessError &OutError) override;
	bool Revoke(const FUnrealAIOAuthAuthorizationOperationContext &Context, FStringView ExactRevocationEndpoint,
				TConstArrayView<uint8> FormBody, FUnrealAIProviderAccessError &OutError) override;

  private:
	TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe> HttpClient;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIOAuthAuthorizationIssuerHttpConfig Config;
};
