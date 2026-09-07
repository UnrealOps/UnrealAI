#include "UnrealAI.h"

#define LOCTEXT_NAMESPACE "FUnrealAIModule"

DEFINE_LOG_CATEGORY(LogUnrealAI);

void FUnrealAIModule::StartupModule() {}

void FUnrealAIModule::ShutdownModule()
{
	UE_LOG(LogUnrealAI, Fatal,
		   TEXT("Explicit UnrealAI unload is unsupported while native requests or continuations can be retained."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealAIModule, UnrealAI)
