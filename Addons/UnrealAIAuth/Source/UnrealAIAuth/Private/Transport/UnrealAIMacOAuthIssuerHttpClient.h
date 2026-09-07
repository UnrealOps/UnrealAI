// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Transport/UnrealAIOAuthIssuerHttpClient.h"

#if PLATFORM_MAC
TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe>
CreateAgentMacOAuthIssuerHttpClient(const FUnrealAIOAuthIssuerHttpClientOptions &Options);
#endif
