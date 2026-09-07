// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Transport/UnrealAIHttpTransport.h"

#if PLATFORM_MAC
namespace UE::UnrealAI::Transport::Private
{
TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe>
CreateMacUrlSessionTransport(const FUnrealAIHttpTransportOptions &Options);
}
#endif // PLATFORM_MAC
