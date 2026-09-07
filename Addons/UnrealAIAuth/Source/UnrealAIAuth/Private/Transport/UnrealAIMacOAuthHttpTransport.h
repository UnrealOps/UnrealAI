// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Transport/UnrealAIOAuthHttpTransport.h"

namespace UE::UnrealAI::Transport::Private
{
TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe>
CreateMacOAuthHttpTransport(const FUnrealAIOAuthHttpTransportOptions &Options);
}
