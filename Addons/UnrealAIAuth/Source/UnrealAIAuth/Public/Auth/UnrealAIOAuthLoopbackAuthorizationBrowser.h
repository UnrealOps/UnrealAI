// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"
#include "CoreMinimal.h"

/** Bounded resource policy for one exact loopback browser authorization. */
struct UNREALAIAUTH_API FUnrealAIOAuthLoopbackAuthorizationBrowserConfig final
{
	static constexpr int32 MaxRequestHeaderBytesLimit = 32 * 1024;
	static constexpr int32 MaxAcceptedConnectionsLimit = 64;

	int32 MaxRequestHeaderBytes = 8 * 1024;
	int32 MaxAcceptedConnections = 16;
	double SocketPollSeconds = 0.01;
	double ResponseWriteTimeoutSeconds = 0.25;

	bool ValidateShape(FString &OutError) const;
};

/** Launches one trusted HTTPS authorization URL in the platform system browser. */
class UNREALAIAUTH_API IUnrealAIOAuthSystemBrowserLauncher
{
  public:
	virtual ~IUnrealAIOAuthSystemBrowserLauncher() = default;
	virtual bool LaunchSystemBrowser(FStringView AuthorizationUrl, FUnrealAIProviderAccessError &OutError) = 0;
};

/** Production FPlatformProcess system-browser launcher. */
class UNREALAIAUTH_API FUnrealAIPlatformOAuthSystemBrowserLauncher final : public IUnrealAIOAuthSystemBrowserLauncher
{
  public:
	bool LaunchSystemBrowser(FStringView AuthorizationUrl, FUnrealAIProviderAccessError &OutError) override;
};

/**
 * Production system-browser and exact loopback callback adapter.
 *
 * The adapter supports only plaintext HTTP callbacks to numeric IPv4/IPv6 loopback literals with an explicit port.
 * It binds before launching the browser, validates the peer, Host header, exact callback path, bounded query syntax,
 * and expected state, and never logs or reflects authorization URLs, codes, state, or provider error text.
 */
class UNREALAIAUTH_API FUnrealAIOAuthLoopbackAuthorizationBrowser final : public IUnrealAIOAuthAuthorizationBrowser
{
  public:
	explicit FUnrealAIOAuthLoopbackAuthorizationBrowser(
		const FUnrealAIOAuthLoopbackAuthorizationBrowserConfig &InConfig = {});
	FUnrealAIOAuthLoopbackAuthorizationBrowser(
		TSharedRef<IUnrealAIOAuthSystemBrowserLauncher, ESPMode::ThreadSafe> InLauncher,
		const FUnrealAIOAuthLoopbackAuthorizationBrowserConfig &InConfig = {});

	bool Authorize(const FUnrealAIOAuthAuthorizationOperationContext &Context,
				   const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch,
				   FUnrealAIOAuthBrowserAuthorizationCallback &OutCallback,
				   FUnrealAIProviderAccessError &OutError) override;

  private:
	TSharedRef<IUnrealAIOAuthSystemBrowserLauncher, ESPMode::ThreadSafe> Launcher;
	FUnrealAIOAuthLoopbackAuthorizationBrowserConfig Config;
};
