// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Transport/UnrealAIHttpTransport.h"

namespace UE::UnrealAI::Transport::Private
{
TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe>
CreateMacUrlSessionTransport(const FUnrealAIHttpTransportOptions &Options);
}
