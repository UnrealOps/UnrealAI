// Copyright UnrealOps. All Rights Reserved.
#include "OpenAI/UnrealAIOpenAIResponsesProvider.h"
#include "Execution/UnrealAINativeSseModelProvider.h"
#include "OpenAICompatible/UnrealAIOpenAICompatibleChatProtocol.h"
#include "Misc/ScopeLock.h"
#include "Runtime/UnrealAIPhysicalRequestBudget.h"

namespace
{
const FName OpenAIResponsesProviderName(TEXT("openai.responses"));
constexpr int32 MaxActiveProviderRequests = 256;
bool MatchesResponsesConnection(const FUnrealAIConnectionDescriptor &Connection,
								const FUnrealAIOpenAIResponsesProviderConfig &Config)
{
	return Connection.CredentialDestination.ModelProviderName == Config.ProviderName &&
		   Connection.CredentialDestination.AccountAuthProviderName == Config.AccountAuthProviderName &&
		   Connection.CredentialDestination.AuthScheme == Config.AuthScheme &&
		   Connection.CredentialDestination.BillingMode == Config.BillingMode &&
		   Connection.CredentialDestination.EndpointOrigin == Config.EndpointOrigin &&
		   Connection.CredentialDestination.Audience == Config.Audience;
}
EUnrealAIModelCapability GetNormalizedResponsesCapabilities()
{
	return EUnrealAIModelCapability::Text | EUnrealAIModelCapability::StreamingText |
		   EUnrealAIModelCapability::FunctionTools | EUnrealAIModelCapability::StructuredOutput |
		   EUnrealAIModelCapability::ImageInput | EUnrealAIModelCapability::UsageReporting |
		   EUnrealAIModelCapability::RequestCancellation;
}

EUnrealAIModelCapability GetToolResponsesCapabilities()
{
	return EUnrealAIModelCapability::Text | EUnrealAIModelCapability::StreamingText |
		   EUnrealAIModelCapability::FunctionTools | EUnrealAIModelCapability::UsageReporting |
		   EUnrealAIModelCapability::RequestCancellation;
}

EUnrealAIModelCapability GetConservativeResponsesCapabilities()
{
	return EUnrealAIModelCapability::Text | EUnrealAIModelCapability::StreamingText |
		   EUnrealAIModelCapability::UsageReporting | EUnrealAIModelCapability::RequestCancellation;
}

FUnrealAIModelProfileProjection MakeDefaultModelProjection(const FUnrealAIOpenAIResponsesProviderConfig &Config,
														   const FString &ModelId)
{
	FUnrealAIModelProfileProjection Projection;
	Projection.ModelId = ModelId;
	Projection.Capabilities = GetConservativeResponsesCapabilities();
	Projection.MaximumToolsPerRequest = 0;
	Projection.MaximumToolOutputsPerRequest = 0;
	Projection.MaximumInputMessages = Config.DefaultMaximumInputMessages;
	Projection.MaximumOutputTokens = Config.DefaultMaximumOutputTokens;
	Projection.MaximumParallelToolCalls = 0;
	Projection.MaximumTextOutputBytes = FUnrealAIModelProfileProjection::MaxTextOutputUtf8Bytes;
	Projection.MaximumStructuredOutputBytes = 0;
	Projection.MaximumToolArgumentBytes = 0;
	Projection.MaximumAggregateToolArgumentBytes = 0;
	Projection.MaximumAudioOutputBytes = 0;
	Projection.MaximumTranscriptBytes = 0;
	Projection.MaximumMetadataEntries = 4;
	return Projection;
}

const FUnrealAIModelProfileProjection *
FindConfiguredModelProjection(const FUnrealAIOpenAIResponsesProviderConfig &Config, const FString &ModelId)
{
	return Config.ModelProfiles.FindByPredicate([&ModelId](const FUnrealAIModelProfileProjection &Profile)
												{ return Profile.ModelId == ModelId; });
}

FUnrealAIModelProfileProjection GetConfiguredModelProjection(const FUnrealAIOpenAIResponsesProviderConfig &Config,
															 const FString &ModelId)
{
	if (const FUnrealAIModelProfileProjection *ExactProfile = FindConfiguredModelProjection(Config, ModelId))
	{
		return *ExactProfile;
	}
	if (Config.bRequireConfiguredModelProfile)
	{
		// An invalid projection makes registry preflight fail closed before
		// provider or credential dispatch. Direct starts report the more
		// specific unsupported-model error below.
		FUnrealAIModelProfileProjection MissingProfile;
		MissingProfile.ModelId = ModelId;
		return MissingProfile;
	}
	return MakeDefaultModelProjection(Config, ModelId);
}

bool RequestFitsProjection(const FUnrealAIModelRequest &Request, const FUnrealAIModelProfileProjection &Projection)
{
	const uint32 RequiredBits = static_cast<uint32>(Request.GetEffectiveRequiredCapabilities());
	const uint32 ProjectionBits = static_cast<uint32>(Projection.Capabilities);
	return Request.ModelId == Projection.ModelId && (RequiredBits & ~ProjectionBits) == 0 &&
		   Request.InputMessages.Num() <= Projection.MaximumInputMessages &&
		   Request.Tools.Num() <= Projection.MaximumToolsPerRequest &&
		   Request.ToolOutputs.Num() <= Projection.MaximumToolOutputsPerRequest &&
		   Request.MaxOutputTokens <= Projection.MaximumOutputTokens;
}

FUnrealAIModelProviderDescriptor MakeResponsesDescriptor(const FUnrealAIOpenAIResponsesProviderConfig &Config)
{
	FUnrealAIModelProviderDescriptor Descriptor;
	Descriptor.ProviderName = Config.ProviderName;
	Descriptor.Capabilities = GetConservativeResponsesCapabilities();
	Descriptor.MaximumToolsPerRequest = 0;
	Descriptor.MaximumInputMessages = Config.DefaultMaximumInputMessages;
	Descriptor.MaximumOutputTokens = Config.DefaultMaximumOutputTokens;
	for (const FUnrealAIModelProfileProjection &Profile : Config.ModelProfiles)
	{
		Descriptor.Capabilities |= Profile.Capabilities;
		Descriptor.MaximumToolsPerRequest =
			FMath::Max(Descriptor.MaximumToolsPerRequest, Profile.MaximumToolsPerRequest);
		Descriptor.MaximumInputMessages = FMath::Max(Descriptor.MaximumInputMessages, Profile.MaximumInputMessages);
		Descriptor.MaximumOutputTokens = FMath::Max(Descriptor.MaximumOutputTokens, Profile.MaximumOutputTokens);
	}
	return Descriptor;
}
} // namespace

bool FUnrealAIOpenAIResponsesProviderConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const FString ProviderText = ProviderName.ToString();
	const FString AuthProviderText = AccountAuthProviderName.ToString();
	const FString PublicFaultCodePrefix = PublicFaultPolicy.CodePrefix.ToString();
	FUnrealAIHttpRequestHeader ProtectedSecondaryHeader;
	ProtectedSecondaryHeader.Name = ProtectedSecondaryHeaderName;
	ProtectedSecondaryHeader.Value = TEXT("protected");
	FString ProtectedSecondaryHeaderError;
	FString PublicFaultPolicyError;
	const bool bSingleBearerPresentation =
		CredentialPresentation == EUnrealAIHttpCredentialPresentation::AuthorizationBearer &&
		ProtectedSecondaryHeaderName.IsEmpty();
	const bool bProtectedSecondaryPresentation =
#if UE_BUILD_SHIPPING || UE_SERVER
		false;
#else
		CredentialPresentation == EUnrealAIHttpCredentialPresentation::AuthorizationBearerWithProtectedSecondary &&
		AuthScheme == EUnrealAIAuthScheme::OAuthBearer && BillingMode == EUnrealAIBillingMode::SubscriptionQuota &&
		!ProtectedSecondaryHeaderName.IsEmpty() &&
		ProtectedSecondaryHeader.ValidateShape(ProtectedSecondaryHeaderError);
#endif
	if (ProviderName.IsNone() || AccountAuthProviderName.IsNone() || ProviderText.IsEmpty() ||
		AuthProviderText.IsEmpty() ||
		FTCHARToUTF8(*ProviderText).Length() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes ||
		FTCHARToUTF8(*AuthProviderText).Length() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes ||
		!(ProviderText == PublicFaultCodePrefix ||
		  ProviderText.StartsWith(PublicFaultCodePrefix + TEXT("."), ESearchCase::CaseSensitive)) ||
		  (AuthScheme != EUnrealAIAuthScheme::ApiKey && AuthScheme != EUnrealAIAuthScheme::OAuthBearer &&
		   AuthScheme != EUnrealAIAuthScheme::GatewayBearer) ||
		  (AuthScheme == EUnrealAIAuthScheme::ApiKey && BillingMode != EUnrealAIBillingMode::ApiMetered) ||
		  (AuthScheme == EUnrealAIAuthScheme::GatewayBearer && BillingMode != EUnrealAIBillingMode::GatewayAccounted) ||
		  (AuthScheme == EUnrealAIAuthScheme::OAuthBearer && BillingMode != EUnrealAIBillingMode::SubscriptionQuota) ||
		  !EndpointOrigin.IsValid() || !EndpointOrigin.IsSecure() || Audience.IsEmpty() ||
		  FTCHARToUTF8(*Audience).Length() > FUnrealAICredentialDestination::MaxAudienceUtf8Bytes ||
		  (!bSingleBearerPresentation && !bProtectedSecondaryPresentation) || FixedHeaders.Num() > MaxFixedHeaders ||
		  DefaultMaximumToolsPerRequest < 1 ||
		  DefaultMaximumToolsPerRequest > FUnrealAIOpenAIResponsesStreamDecoder::MaxNormalizedToolCalls ||
		  DefaultMaximumInputMessages < 1 || DefaultMaximumInputMessages > FUnrealAIModelRequest::MaxInputMessages ||
		  DefaultMaximumOutputTokens < 1 || DefaultMaximumOutputTokens > FUnrealAIModelRequest::MaxOutputTokensLimit ||
		  ModelProfiles.Num() > MaxModelProfiles || (bRequireConfiguredModelProfile && ModelProfiles.IsEmpty()) ||
		  (bUseChatCompletions && !bSendMaxOutputTokens) ||
		  UnauthorizedErrorCode.IsNone() || ForbiddenErrorCode.IsNone() ||
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
		  MaximumActiveRequestsForTesting < 1 || MaximumActiveRequestsForTesting > MaxActiveProviderRequests ||
#endif
		  !PublicFaultPolicy.ValidateShape(PublicFaultPolicyError) ||
		  (ForbiddenErrorCategory != EUnrealAIErrorCategory::NotAuthorized &&
		   ForbiddenErrorCategory != EUnrealAIErrorCategory::PolicyDenied) ||
		  FTCHARToUTF8(*UnauthorizedErrorCode.ToString()).Length() > FUnrealAIModelError::MaxCodeUtf8Bytes ||
		  FTCHARToUTF8(*ForbiddenErrorCode.ToString()).Length() > FUnrealAIModelError::MaxCodeUtf8Bytes)
	{
		OutError =
			TEXT("Responses provider configuration has an invalid identity, access, endpoint, or header policy.");
		return false;
	}
	FTCHARToUTF8 RelativePathUtf8(*RelativePath);
	if (!RelativePath.StartsWith(
			TEXT("/")) ||
			RelativePath.StartsWith(
				TEXT("//")) || RelativePathUtf8.Length() <= 0 ||
				RelativePathUtf8.Length() > FUnrealAIHttpRequest::MaxRelativePathBytes ||
				RelativePath.Contains(
					TEXT("://")) ||
					RelativePath.Contains(
						TEXT("?")) ||
						RelativePath.Contains(
							TEXT("#")) ||
							RelativePath.Contains(
								TEXT("\\")) ||
								RelativePath.Contains(
									TEXT("/../")) ||
									RelativePath.EndsWith(TEXT("/..")) ||
														  RelativePath.Contains(TEXT("/./")) ||
																				RelativePath.EndsWith(TEXT("/.")))
	{
		OutError = TEXT("Responses provider configuration requires a strict origin-relative resource path.");
		return false;
	}
	TSet<FString> HeaderNames;
	const FString LowerProtectedSecondaryHeaderName = ProtectedSecondaryHeaderName.ToLower();
	for (const FUnrealAIHttpRequestHeader &Header : FixedHeaders)
	{
		FString HeaderError;
		const FString LowerName = Header.Name.ToLower();
		if (!Header.ValidateShape(HeaderError) || HeaderNames.Contains(LowerName) ||
			LowerName == TEXT("content-type") ||
							  LowerName == TEXT("accept") ||
												LowerName == TEXT("authorization") ||
																  (bProtectedSecondaryPresentation &&
																   LowerName == LowerProtectedSecondaryHeaderName))
		{
			OutError =
				TEXT("Responses provider configuration contains an invalid, duplicate, or transport-owned header.");
			return false;
		}
		HeaderNames.Add(LowerName);
	}

	const FUnrealAIModelProviderDescriptor Descriptor = MakeResponsesDescriptor(*this);
	FString DescriptorError;
	if (!Descriptor.ValidateShape(DescriptorError))
	{
		OutError = TEXT("Responses provider configuration produced an invalid aggregate model descriptor.");
		return false;
	}
	const FUnrealAIModelProfileProjection DefaultProjection =
		MakeDefaultModelProjection(*this, TEXT("responses-default-profile-validation"));
	FString ProjectionError;
	if (!DefaultProjection.ValidateAgainst(Descriptor, ProjectionError))
	{
		OutError = TEXT("Responses provider configuration produced an invalid default model profile.");
		return false;
	}

	const EUnrealAIModelCapability RequiredNormalizedCapabilities =
		EUnrealAIModelCapability::Text | EUnrealAIModelCapability::StreamingText |
		EUnrealAIModelCapability::UsageReporting | EUnrealAIModelCapability::RequestCancellation;
	TSet<FString> ProfileModelIds;
	for (const FUnrealAIModelProfileProjection &Profile : ModelProfiles)
	{
		const uint32 ProfileCapabilityBits = static_cast<uint32>(Profile.Capabilities);
		const uint32 NormalizedCapabilityBits = static_cast<uint32>(GetNormalizedResponsesCapabilities());
		if (ProfileModelIds.Contains(Profile.ModelId) || (ProfileCapabilityBits & ~NormalizedCapabilityBits) != 0 ||
			!EnumHasAllFlags(Profile.Capabilities, RequiredNormalizedCapabilities) ||
			Profile.MaximumMetadataEntries < 3 ||
			Profile.MaximumToolsPerRequest > FUnrealAIOpenAIResponsesStreamDecoder::MaxNormalizedToolCalls ||
			Profile.MaximumToolOutputsPerRequest > FUnrealAIOpenAIResponsesStreamDecoder::MaxNormalizedToolCalls ||
			!Profile.ValidateAgainst(Descriptor, ProjectionError))
		{
			OutError = TEXT("Responses provider configuration contains a duplicate or invalid exact model profile.");
			return false;
		}
		ProfileModelIds.Add(Profile.ModelId);
	}
	return true;
}

FUnrealAIModelProfileProjection
FUnrealAIOpenAIResponsesProviderConfig::MakeToolCapableModelProfile(const FString &ModelId) const
{
	FUnrealAIModelProfileProjection Profile;
	Profile.ModelId = ModelId;
	Profile.Capabilities = GetToolResponsesCapabilities();
	Profile.MaximumToolsPerRequest = DefaultMaximumToolsPerRequest;
	Profile.MaximumToolOutputsPerRequest = DefaultMaximumToolsPerRequest;
	Profile.MaximumInputMessages = DefaultMaximumInputMessages;
	Profile.MaximumOutputTokens = DefaultMaximumOutputTokens;
	Profile.MaximumParallelToolCalls = 1;
	Profile.MaximumTextOutputBytes = FUnrealAIModelProfileProjection::MaxTextOutputUtf8Bytes;
	Profile.MaximumStructuredOutputBytes = 0;
	Profile.MaximumToolArgumentBytes = FUnrealAIModelProfileProjection::MaxToolArgumentUtf8Bytes;
	Profile.MaximumAggregateToolArgumentBytes = static_cast<int32>(FMath::Min<int64>(
		FUnrealAIModelProfileProjection::MaxAggregateToolArgumentUtf8Bytes,
		static_cast<int64>(DefaultMaximumToolsPerRequest) * FUnrealAIModelProfileProjection::MaxToolArgumentUtf8Bytes));
	Profile.MaximumAudioOutputBytes = 0;
	Profile.MaximumTranscriptBytes = 0;
	Profile.MaximumMetadataEntries = 4;
	return Profile;
}

FUnrealAIModelProfileProjection
FUnrealAIOpenAIResponsesProviderConfig::MakePublicMultimodalModelProfile(const FString &ModelId) const
{
	FUnrealAIModelProfileProjection Profile = MakeToolCapableModelProfile(ModelId);
	Profile.Capabilities |= EUnrealAIModelCapability::StructuredOutput | EUnrealAIModelCapability::ImageInput;
	Profile.MaximumStructuredOutputBytes = FUnrealAIModelProfileProjection::MaxStructuredOutputUtf8Bytes;
	return Profile;
}

FUnrealAIOpenAIResponsesProviderConfig FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey()
{
	FUnrealAIOpenAIResponsesProviderConfig Config;
	Config.ProviderName = OpenAIResponsesProviderName;
	Config.AccountAuthProviderName = TEXT("platform.keychain");
	Config.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Config.BillingMode = EUnrealAIBillingMode::ApiMetered;
	FString OriginError;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.openai.com"), false, Config.EndpointOrigin, OriginError);
	Config.Audience = TEXT("https://api.openai.com/v1/responses");
	Config.RelativePath = TEXT("/v1/responses");
	Config.CredentialPresentation = EUnrealAIHttpCredentialPresentation::AuthorizationBearer;
	Config.ModelProfiles.Add(Config.MakePublicMultimodalModelProfile(TEXT("gpt-5.6")));
	Config.bRequireConfiguredModelProfile = true;
	return Config;
}

namespace UE::UnrealAI::Responses::Private
{
class FContinuationCommit final : public IUnrealAINativeSseContinuationCommit
{
  public:
	explicit FContinuationCommit(FUnrealAIOpenAIResponsesContinuationCommit &&InCommit) : Commit(MoveTemp(InCommit)) {}
	bool IsRequired() const override
	{
		return Commit.IsRequired();
	}
	bool TryCommit(FString &OutError) override
	{
		return Commit.TryCommit(OutError);
	}

  private:
	FUnrealAIOpenAIResponsesContinuationCommit Commit;
};

class FDecoder final : public IUnrealAINativeSseDecoder
{
  public:
	FDecoder(FName ProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
			 const FUnrealAIModelRequest &Request, IUnrealAINativeSseProtocol::FEventSink Sink,
			 IUnrealAINativeSseProtocol::FIgnoredEventObserver Ignored, bool bCompatible, bool bAllowEmptyTerminalOutput)
		: Responses(ProviderName, FaultPolicy, Request, MoveTemp(Sink), MoveTemp(Ignored), bAllowEmptyTerminalOutput)
	{
		if (bCompatible)
		{
			Compatible =
				MakeUnique<FUnrealAIOpenAICompatibleChatStreamDecoder>(Request.ModelId,
																	   [this](TArray<uint8> &&Bytes)
																	   {
																		   if (BridgeError.IsEmpty())
																		   {
																			   Responses.PushBytes(Bytes, BridgeError);
																		   }
																	   });
		}
	}
	bool PushBytes(TConstArrayView<uint8> Bytes, FString &OutError) override
	{
		if (!Compatible.IsValid())
		{
			return Responses.PushBytes(Bytes, OutError);
		}
		const bool bSucceeded = Compatible->PushBytes(Bytes, OutError);
		if (!BridgeError.IsEmpty())
		{
			OutError = BridgeError;
			return false;
		}
		return bSucceeded;
	}
	bool Finish(FString &OutError) override
	{
		if (Compatible.IsValid() && !Compatible->Finish(OutError))
		{
			return false;
		}
		if (!BridgeError.IsEmpty())
		{
			OutError = BridgeError;
			return false;
		}
		return Responses.Finish(OutError);
	}
	void Cancel() override
	{
		Responses.Cancel();
	}
	bool IsTerminal() const override
	{
		return Responses.IsTerminal();
	}

  private:
	FUnrealAIOpenAIResponsesStreamDecoder Responses;
	TUniquePtr<FUnrealAIOpenAICompatibleChatStreamDecoder> Compatible;
	FString BridgeError;
};

class FProtocol final : public IUnrealAINativeSseProtocol
{
  public:
	explicit FProtocol(const FUnrealAIOpenAIResponsesProviderConfig &Config)
		: ProviderName(Config.ProviderName), FaultPolicy(Config.PublicFaultPolicy),
		  bCompatible(Config.bUseChatCompletions), bSendMaxOutputTokens(Config.bSendMaxOutputTokens),
		  bAllowEmptyTerminalOutput(Config.bAllowEmptyTerminalOutput)
	{
	}
	FName GetProviderName() const override
	{
		return ProviderName;
	}
	int32 GetMaximumRequestBodyBytes() const override
	{
		return FUnrealAIOpenAIResponsesWireRequest::MaxBodyBytes;
	}
	int64 GetMaximumStreamBytes() const override
	{
		return FUnrealAIOpenAIResponsesStreamDecoder::MaxStreamBytes;
	}
	bool StageRequest(const FUnrealAIModelRequest &Request, const FUnrealAICredentialDestination &Destination,
					  TArray<uint8> &OutBodyUtf8, TUniquePtr<IUnrealAINativeSseContinuationCommit> &OutCommit,
					  FUnrealAIModelError &OutError) const override
	{
		FUnrealAIOpenAIResponsesWireRequest Wire;
		FUnrealAIOpenAIResponsesContinuationCommit Commit;
		if (!FUnrealAIOpenAIResponsesRequestBuilder::StageForProvider(ProviderName, FaultPolicy, Request, Wire, Commit,
																	  OutError, bSendMaxOutputTokens))
		{
			return false;
		}
		if (bCompatible)
		{
			FUnrealAIOpenAICompatibleChatWireRequest ChatWire;
			FString Error;
			if (!FUnrealAIOpenAICompatibleChatRequestBuilder::LowerResponsesWire(Wire.BodyUtf8, ChatWire, Error))
			{
				OutError.Category = EUnrealAIErrorCategory::UnsupportedCapability;
				OutError.Code = TEXT("chat_request_unsupported");
				OutError.UserMessage =
					FText::FromString(TEXT("The request is not supported by this Chat Completions route."));
				return false;
			}
			OutBodyUtf8 = MoveTemp(ChatWire.BodyUtf8);
		}
		else
		{
			OutBodyUtf8 = MoveTemp(Wire.BodyUtf8);
		}
		OutCommit = MakeUnique<FContinuationCommit>(MoveTemp(Commit));
		return true;
	}
	TUniquePtr<IUnrealAINativeSseDecoder> CreateDecoder(const FUnrealAIModelRequest &Request, FEventSink Sink,
														FIgnoredEventObserver Ignored) const override
	{
			return MakeUnique<FDecoder>(ProviderName, FaultPolicy, Request, MoveTemp(Sink), MoveTemp(Ignored), bCompatible,
				bAllowEmptyTerminalOutput);
	}

  private:
	FName ProviderName;
	FUnrealAIOpenAIResponsesPublicFaultPolicy FaultPolicy;
	bool bCompatible = false;
	bool bSendMaxOutputTokens = true;
	bool bAllowEmptyTerminalOutput = false;
};
} // namespace UE::UnrealAI::Responses::Private

class FUnrealAIOpenAIResponsesProvider::FState final
{
  public:
	FState(TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		   TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
		   const FUnrealAIOpenAIResponsesProviderConfig &InConfig)
		: Connections(MoveTemp(InConnections)), Transport(MoveTemp(InTransport)), Config(InConfig),
		  Budget(MakeShared<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe>(
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
			  InConfig.MaximumActiveRequestsForTesting
#else
			  256
#endif
			  ))
	{
		FString Error;
		bShutdown = !Config.ValidateShape(Error);
	}
	bool Start(const FUnrealAIModelRequest &Request,
			   TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
			   TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink,
			   const FUnrealAICancellationToken &Cancellation,
			   TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIModelError &OutError)
	{
		OutHandle.Reset();
		OutError = {};
		FString ShapeError;
		const auto Reject = [this, &OutError](EUnrealAIErrorCategory Category, FName Code, const TCHAR *Message)
		{
			OutError.Category = Category;
			OutError.Code = Code;
			OutError.UserMessage = FText::FromString(Message);
			Config.PublicFaultPolicy.ApplyTo(OutError);
			return false;
		};
		if (!Request.ValidateShape(ShapeError))
		{
			return Reject(EUnrealAIErrorCategory::InvalidArgument, TEXT("openai_request_invalid"),
																		TEXT("The model request is invalid."));
		}
		const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection =
			Connections->Find(Request.ConnectionAlias);
		if (!Connection.IsValid() || !MatchesResponsesConnection(*Connection, Config))
		{
			return Reject(EUnrealAIErrorCategory::InvalidConfiguration,
						  TEXT("openai_connection_invalid"),
							   TEXT("The model connection is unavailable or incompatible."));
		}
		if (Config.bRequireConfiguredModelProfile && FindConfiguredModelProjection(Config, Request.ModelId) == nullptr)
		{
			return Reject(EUnrealAIErrorCategory::UnsupportedCapability,
						  TEXT("openai_model_not_configured"),
							   TEXT("The selected model has no configured capability profile."));
		}
		const FUnrealAIModelProfileProjection Projection = GetConfiguredModelProjection(Config, Request.ModelId);
		if (!RequestFitsProjection(Request, Projection))
		{
			return Reject(EUnrealAIErrorCategory::UnsupportedCapability,
						  TEXT("openai_capability_unsupported"),
							   TEXT("The configured model cannot serve this request."));
		}
		TSharedPtr<FUnrealAINativeSseModelProvider, ESPMode::ThreadSafe> Native;
		const FString Alias = Request.ConnectionAlias.ToString();
		const FString CacheKey =
			FString::FromInt(Alias.Len()) + TEXT(":") + Alias +
												 (Config.bRequireConfiguredModelProfile ? FString() : Request.ModelId);
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return Reject(EUnrealAIErrorCategory::Provider, TEXT("openai_provider_shutdown"),
																	 TEXT("The model provider is shutting down."));
			}
			for (auto It = Outstanding.CreateIterator(); It; ++It)
			{
				const TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> Handle = It.Value().Pin();
				if (!Handle.IsValid() || Handle->IsLogicallyComplete())
				{
					It.RemoveCurrent();
				}
			}
			const int32 MaximumActive =
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
				Config.MaximumActiveRequestsForTesting;
#else
				256;
#endif
			if (Budget->Num() + Pending.Num() >= MaximumActive || Outstanding.Contains(Request.RequestId.Value) ||
				Pending.Contains(Request.RequestId.Value))
			{
				return Reject(EUnrealAIErrorCategory::Busy,
							  TEXT("openai_provider_capacity"),
								   TEXT("The model provider is at capacity or the request is already active."));
			}
			Native = Providers.FindRef(CacheKey);
			if (!Native.IsValid())
			{
				if (Providers.Num() >= 32)
				{
					return Reject(EUnrealAIErrorCategory::Busy,
								  TEXT("openai_connection_capacity"),
									   TEXT("The provider connection capacity is exhausted."));
				}
				FUnrealAINativeSseModelProviderConfig NativeConfig;
				NativeConfig.ProviderName = Config.ProviderName;
				NativeConfig.Clock = Config.Clock;
				NativeConfig.Connection = *Connection;
				NativeConfig.RelativePath = Config.RelativePath;
				NativeConfig.CredentialPresentation = Config.CredentialPresentation;
				NativeConfig.ProtectedSecondaryHeaderName = Config.ProtectedSecondaryHeaderName;
				NativeConfig.FixedHeaders = Config.FixedHeaders;
				NativeConfig.ModelProfiles = Config.bRequireConfiguredModelProfile
												 ? Config.ModelProfiles
												 : TArray<FUnrealAIModelProfileProjection>{Projection};
				NativeConfig.Protocol =
					MakeShared<UE::UnrealAI::Responses::Private::FProtocol, ESPMode::ThreadSafe>(Config);
				NativeConfig.ErrorCodePrefix = Config.PublicFaultPolicy.CodePrefix;
				NativeConfig.MaximumPendingHttpEvents = Config.bUseChatCompletions ? 256 : 1024;
				NativeConfig.bAllowMissingResponseContentType = Config.bAllowMissingResponseContentType;
				NativeConfig.MapPublicError =
					[Policy = Config.PublicFaultPolicy, bChat = Config.bUseChatCompletions](FUnrealAIModelError &Error)
				{
					if (bChat && Error.Code == FName(TEXT("openai_event_capacity")))
					{
						Error.Code = TEXT("openai_compatible_bridge_capacity");
					}
					Policy.ApplyTo(Error);
				};
				NativeConfig.UnauthorizedErrorCode = Config.UnauthorizedErrorCode;
				NativeConfig.ForbiddenErrorCode = Config.ForbiddenErrorCode;
				NativeConfig.ForbiddenErrorCategory = Config.ForbiddenErrorCategory;
				NativeConfig.MaximumActiveRequests = MaximumActive;
				NativeConfig.PhysicalBudget = Budget;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
				NativeConfig.bDrainEventsSynchronouslyForTesting = Config.bDrainEventsSynchronouslyForTesting;
				NativeConfig.BeforeFirstDrainForTesting = Config.BeforeFirstDrainForTesting;
				NativeConfig.AfterFirstQueuedDrainBodyForTesting = Config.AfterFirstQueuedDrainBodyForTesting;
#endif
				if (!NativeConfig.ValidateShape(ShapeError))
				{
					return Reject(EUnrealAIErrorCategory::InvalidConfiguration,
								  TEXT("openai_native_config_invalid"),
									   TEXT("The model execution configuration is invalid."));
				}
				Native = MakeShared<FUnrealAINativeSseModelProvider, ESPMode::ThreadSafe>(Connections, Transport,
																						  NativeConfig);
				Providers.Add(CacheKey, Native);
			}
			Pending.Add(Request.RequestId.Value);
		}
		const bool bAccepted =
			Native->StartRequest(Request, MoveTemp(AccessContext), MoveTemp(Sink), Cancellation, OutHandle, OutError);
		bool bCancel = false;
		{
			FScopeLock Lock(&Mutex);
			Pending.Remove(Request.RequestId.Value);
			if (OutHandle.IsValid())
			{
				Outstanding.Add(Request.RequestId.Value, OutHandle);
				bCancel = bShutdown;
			}
		}
		if (bCancel)
		{
			OutHandle->Cancel();
		}
		return bAccepted;
	}
	void Shutdown()
	{
		TArray<TSharedPtr<FUnrealAINativeSseModelProvider, ESPMode::ThreadSafe>> Local;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			bShutdown = true;
			Providers.GenerateValueArray(Local);
		}
		for (const TSharedPtr<FUnrealAINativeSseModelProvider, ESPMode::ThreadSafe> &Native : Local)
		{
			Native->BeginShutdown();
		}
	}
	const FUnrealAIOpenAIResponsesProviderConfig &GetConfig() const
	{
		return Config;
	}

  private:
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> Transport;
	FUnrealAIOpenAIResponsesProviderConfig Config;
	TSharedRef<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe> Budget;
	FCriticalSection Mutex;
	TMap<FString, TSharedPtr<FUnrealAINativeSseModelProvider, ESPMode::ThreadSafe>> Providers;
	TMap<FGuid, TWeakPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe>> Outstanding;
	TSet<FGuid> Pending;
	bool bShutdown = false;
};

FUnrealAIOpenAIResponsesProvider::FUnrealAIOpenAIResponsesProvider(
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport)
	: FUnrealAIOpenAIResponsesProvider(MoveTemp(InConnections), MoveTemp(InTransport),
									   FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey())
{
}
FUnrealAIOpenAIResponsesProvider::FUnrealAIOpenAIResponsesProvider(
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
	const FUnrealAIOpenAIResponsesProviderConfig &Config)
	: State(MakeShared<FState, ESPMode::ThreadSafe>(MoveTemp(InConnections), MoveTemp(InTransport), Config))
{
}
FUnrealAIOpenAIResponsesProvider::~FUnrealAIOpenAIResponsesProvider()
{
	BeginShutdown();
}
FName FUnrealAIOpenAIResponsesProvider::GetProviderName() const
{
	return State->GetConfig().ProviderName;
}
FUnrealAIModelProviderDescriptor FUnrealAIOpenAIResponsesProvider::Describe() const
{
	return MakeResponsesDescriptor(State->GetConfig());
}
EUnrealAIModelCapability FUnrealAIOpenAIResponsesProvider::GetCapabilities(const FString &ModelId) const
{
	return GetModelProjection(ModelId).Capabilities;
}
FUnrealAIModelProfileProjection FUnrealAIOpenAIResponsesProvider::GetModelProjection(const FString &ModelId) const
{
	return GetConfiguredModelProjection(State->GetConfig(), ModelId);
}
void FUnrealAIOpenAIResponsesProvider::BeginShutdown()
{
	State->Shutdown();
}
bool FUnrealAIOpenAIResponsesProvider::StartRequest(
	const FUnrealAIModelRequest &Request,
	TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
	TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIModelError &OutError)
{
	return State->Start(Request, MoveTemp(AccessContext), MoveTemp(Sink), Cancellation, OutHandle, OutError);
}
