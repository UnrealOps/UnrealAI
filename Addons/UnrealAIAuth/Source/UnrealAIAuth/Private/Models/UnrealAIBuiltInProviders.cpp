// Copyright EngineWorks. All Rights Reserved.
#include "Models/UnrealAIBuiltInProviders.h"
#include "Models/UnrealAIProviderCatalog.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "Auth/UnrealAIPlatformSecretStore.h"
#include "Auth/UnrealAIMacKeychainSecretStore.h"
#include "Auth/UnrealAIApiKeyCredentialBroker.h"
#include "Auth/UnrealAIApiKeyProvisioner.h"
#include "OpenAI/UnrealAIOpenAIResponsesProvider.h"
#include "Anthropic/UnrealAIAnthropicMessagesProvider.h"
#include "Gemini/UnrealAIGeminiInteractionsProvider.h"
#include "Transport/UnrealAIHttpTransport.h"

namespace
{
TArray<TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>> Registrations;
struct FPreset
{
	FName Provider, Alias, Endpoint, AuthProvider, AuthProfile;
	const TCHAR *Origin;
	const TCHAR *Audience;
	FUnrealAIAccessAccountId Account;
	FGuid Secret, Billing;
	const TCHAR *Payer;
};
bool RegisterPreset(const FPreset &Preset)
{
	FString Error;
	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	FUnrealAISecretStoreCapabilities Capabilities;
	FUnrealAIProviderAccessError AccessError;
	if (!FUnrealAIPlatformSecretStore::TryCreate(FUnrealAIPlatformSecretStoreSelection::CurrentUserApiKey(false), Store,
												 Capabilities, AccessError) ||
		!Store.IsValid())
	{
		return false;
	}
	FUnrealAIEndpointOrigin Origin;
	if (!FUnrealAIEndpointOrigin::TryParse(Preset.Origin, false, Origin, Error))
	{
		return false;
	}
	FUnrealAIEndpointProfileRegistry Endpoints;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Endpoint;
	if (!Endpoints.RegisterCustomApiEndpoint(Preset.Endpoint, Preset.Provider, Origin, Preset.Audience, 1, Endpoint,
											 Error))
	{
		return false;
	}
	FUnrealAIConnectionRegistry Registry(Endpoints.CreateSnapshot());
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = Preset.Alias;
	Connection.EndpointProfileId = Preset.Endpoint;
	Connection.ConnectionRevision = 1;
	FUnrealAICredentialDestination &Destination = Connection.CredentialDestination;
	Destination.ModelProviderName = Preset.Provider;
	Destination.AccountAuthProviderName = Preset.AuthProvider;
	Destination.AuthProfileId = Preset.AuthProfile;
	Destination.AccountId = Preset.Account;
	Destination.TenantRealm = TEXT("local.machine");
	Destination.BillingPrincipalId.Value = Preset.Billing;
	Destination.PayerHandle = Preset.Payer;
	Destination.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Destination.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Destination.EndpointOrigin = Origin;
	Destination.Audience = Preset.Audience;
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Frozen;
	if (!Registry.Register(Connection, Frozen, Error))
	{
		return false;
	}
	const auto Connections = Registry.CreateSnapshot();
	const auto Clock = MakeShared<FUnrealAISystemClock, ESPMode::ThreadSafe>();
	const auto Broker =
		MakeShared<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe>(Connections, Store.ToSharedRef(), Clock);
	FUnrealAIApiKeyCredentialBinding Binding;
	Binding.ConnectionAlias = Preset.Alias;
	Binding.AuthProfileId = Preset.AuthProfile;
	Binding.AccountId = Preset.Account;
	Binding.SecretHandle = {Store->GetStoreName(), Preset.Secret};
	if (!Broker->RegisterBinding(Binding, Error))
	{
		Broker->BeginShutdown();
		return false;
	}
	FUnrealAIApiKeyProvisionerConfig ProvisionConfig;
	ProvisionConfig.AuthProfileId = Preset.AuthProfile;
	ProvisionConfig.AccountId = Preset.Account;
	ProvisionConfig.SecretHandle = Binding.SecretHandle;
	TSharedPtr<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe> Provisioner;
	if (!FUnrealAIApiKeyProvisioner::TryCreate(ProvisionConfig, Store.ToSharedRef(), Clock, Broker, Provisioner, Error))
	{
		Broker->BeginShutdown();
		return false;
	}
	const auto Transport = CreateUnrealAIPlatformHttpsTransport();
	TSharedPtr<IUnrealAIModelProvider, ESPMode::ThreadSafe> Provider;
	if (Preset.Provider == TEXT("anthropic.messages"))
	{
		FUnrealAIAnthropicMessagesProviderConfig Config;
		Config.Connection = *Frozen;
		for (const TCHAR *Model : {TEXT("claude-sonnet-5"), TEXT("claude-opus-5")})
		{
			auto Profile = Config.MakeMessagesModelProfile(Model);
			Profile.Capabilities &= ~EUnrealAIModelCapability::ImageInput;
			Config.ModelProfiles.Add(MoveTemp(Profile));
		}
		if (!Config.ValidateShape(Error))
		{
			Provisioner->BeginShutdown();
			Broker->BeginShutdown();
			Transport->BeginShutdown();
			return false;
		}
		Provider = MakeShared<FUnrealAIAnthropicMessagesProvider, ESPMode::ThreadSafe>(Connections, Transport, Config);
	}
	else if (Preset.Provider == TEXT("gemini.interactions"))
	{
		FUnrealAIGeminiInteractionsProviderConfig Config;
		Config.Connection = *Frozen;
		auto Profile = Config.MakeInteractionsModelProfile(TEXT("gemini-3.6-flash"));
		Profile.Capabilities &= ~EUnrealAIModelCapability::ImageInput;
		Config.ModelProfiles.Add(MoveTemp(Profile));
		if (!Config.ValidateShape(Error))
		{
			Provisioner->BeginShutdown();
			Broker->BeginShutdown();
			Transport->BeginShutdown();
			return false;
		}
		Provider = MakeShared<FUnrealAIGeminiInteractionsProvider, ESPMode::ThreadSafe>(Connections, Transport, Config);
	}
	else
	{
		auto Config = FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey();
		if (Preset.Provider == TEXT("xai.platform.responses"))
		{
			Config.ProviderName = Preset.Provider;
			Config.AccountAuthProviderName = Preset.AuthProvider;
			Config.EndpointOrigin = Origin;
			Config.Audience = Preset.Audience;
			Config.PublicFaultPolicy.CodePrefix = TEXT("xai");
			Config.PublicFaultPolicy.ProviderDisplayName = TEXT("xAI");
			Config.UnauthorizedErrorCode = TEXT("xai_http_unauthorized");
			Config.ForbiddenErrorCode = TEXT("xai_http_forbidden");
			Config.ModelProfiles.Reset();
			Config.ModelProfiles.Add(Config.MakePublicMultimodalModelProfile(TEXT("grok-4.5")));
			Config.bRequireConfiguredModelProfile = true;
		}
		if (!Config.ValidateShape(Error))
		{
			Provisioner->BeginShutdown();
			Broker->BeginShutdown();
			Transport->BeginShutdown();
			return false;
		}
		Provider = MakeShared<FUnrealAIOpenAIResponsesProvider, ESPMode::ThreadSafe>(Connections, Transport, Config);
	}
	auto Registration = MakeShared<FUnrealAIProviderRegistration, ESPMode::ThreadSafe>();
	Registration->ProviderName = Preset.Provider;
	Registration->DefaultConnectionAlias = Preset.Alias;
	Registration->Provider = Provider;
	Registration->Connections = Connections;
	Registration->CredentialBroker = Broker;
	Registration->ApiKeyProvisioner = Provisioner;
	Registration->Access.ModelProviderName = Preset.Provider;
	Registration->Access.AccountAuthProviderName = Preset.AuthProvider;
	Registration->Access.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Registration->Access.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Registration->Access.SupportClassification =
		Preset.Provider == TEXT("openai.responses")
								? EUnrealAIProviderAccessSupportClassification::Supported
								: EUnrealAIProviderAccessSupportClassification::ImplementationCandidate;
	Registration->Access.Availability = EUnrealAIProviderAccessAvailability::ConfigurationRequired;
	if (!FUnrealAIProviderCatalog::Get().Register(Registration, Error))
	{
		Provider->BeginShutdown();
		Provisioner->BeginShutdown();
		Broker->BeginShutdown();
		return false;
	}
	Registrations.Add(Registration);
	return true;
}
} // namespace
void RegisterUnrealAIBuiltInProviders()
{
	// Preserve the existing development-only local-key routes and stable keychain record identities.
	if (!IsUnrealAIPlatformHttpsTransportSupported())
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	RegisterPreset({TEXT("openai.responses"),
						 TEXT("openai.platform.default"),
							  TEXT("openai.platform.responses"),
								   TEXT("platform.keychain"),
										TEXT("openai.platform.api_key"),
											 TEXT("https://api.openai.com"),
												  TEXT("https://api.openai.com/v1/responses"),
													   {FGuid(0x12614b23, 0x71d54d80, 0xa1112d16, 0x9daf3ef0)},
													   FGuid(0xe5f3b847, 0x94034bcc, 0xb92af8ec, 0x32bd28b5),
													   FGuid(0x0b930786, 0xffb84408, 0xa8f66c10, 0x4a1919d9),
													   TEXT("openai.platform.api")});
#if !UE_SERVER
	RegisterPreset({TEXT("anthropic.messages"),
						 TEXT("anthropic.platform.default"),
							  TEXT("anthropic.platform.messages"),
								   TEXT("platform.secure_store"),
										TEXT("anthropic.platform.api_key"),
											 TEXT("https://api.anthropic.com"),
												  TEXT("https://api.anthropic.com/v1/messages"),
													   {FGuid(0x12101001, 0x12101002, 0x12101003, 0x12101004)},
													   FGuid(0x12102001, 0x12102002, 0x12102003, 0x12102004),
													   FGuid(0x12103001, 0x12103002, 0x12103003, 0x12103004),
													   TEXT("anthropic.platform.api")});
	RegisterPreset({TEXT("gemini.interactions"),
						 TEXT("gemini.platform.default"),
							  TEXT("gemini.platform.interactions"),
								   TEXT("platform.secure_store"),
										TEXT("gemini.platform.api_key"),
											 TEXT("https://generativelanguage.googleapis.com"),
												  TEXT("https://generativelanguage.googleapis.com/v1/interactions"),
													   {FGuid(0x12201001, 0x12201002, 0x12201003, 0x12201004)},
													   FGuid(0x12202001, 0x12202002, 0x12202003, 0x12202004),
													   FGuid(0x12203001, 0x12203002, 0x12203003, 0x12203004),
													   TEXT("gemini.platform.api")});
	RegisterPreset({TEXT("xai.platform.responses"),
						 TEXT("xai.platform.default"),
							  TEXT("xai.platform.responses"),
								   TEXT("platform.secure_store"),
										TEXT("xai.platform.api_key"),
											 TEXT("https://api.x.ai"),
												  TEXT("https://api.x.ai/v1/responses"),
													   {FGuid(0x12381001, 0x12381002, 0x12381003, 0x12381004)},
													   FGuid(0x12382001, 0x12382002, 0x12382003, 0x12382004),
													   FGuid(0x12383001, 0x12383002, 0x12383003, 0x12383004),
													   TEXT("xai.platform.api")});
#endif
#endif
}
void ShutdownUnrealAIBuiltInProviders()
{
	for (const auto &Registration : Registrations)
	{
		FUnrealAIProviderCatalog::Get().Unregister(Registration);
		Registration->Provider->BeginShutdown();
		Registration->ApiKeyProvisioner->BeginShutdown();
		Registration->CredentialBroker->BeginShutdown();
	}
	Registrations.Reset();
}
