// Copyright EngineWorks. All Rights Reserved.
#include "Auth/UnrealAIAccountCatalog.h"
#include "Auth/UnrealAIAccountAuthProviderRegistry.h"

FUnrealAIAccountAuthProviderRegistry &FUnrealAIAccountAuthProviderRegistry::GetGlobal()
{
	// Process-lifetime ownership matches non-unloadable native provider and credential callbacks.
	static FUnrealAIAccountAuthProviderRegistry *Registry = new FUnrealAIAccountAuthProviderRegistry();
	return *Registry;
}
FUnrealAIAccountCatalogRegistry &FUnrealAIAccountCatalogRegistry::GetGlobal()
{
	static FUnrealAIAccountCatalogRegistry *Catalog =
		new FUnrealAIAccountCatalogRegistry(FUnrealAIAccountAuthProviderRegistry::GetGlobal());
	return *Catalog;
}
