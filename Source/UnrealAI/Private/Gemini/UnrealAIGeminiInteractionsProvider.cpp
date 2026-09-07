// Copyright EngineWorks. All Rights Reserved.

#include "Gemini/UnrealAIGeminiInteractionsProvider.h"

#include "Gemini/UnrealAIGeminiInteractionsProtocol.h"
#include "Execution/UnrealAINativeSseModelProvider.h"

namespace
{
const FName GeminiStrategyProviderName(TEXT("gemini.interactions"));

class FGeminiContinuationCommit final : public IUnrealAINativeSseContinuationCommit
{
  public:
	explicit FGeminiContinuationCommit(FUnrealAIGeminiInteractionsContinuationCommit &&InCommit)
		: Commit(MoveTemp(InCommit))
	{
	}

	bool IsRequired() const override
	{
		return Commit.IsRequired();
	}

	bool TryCommit(FString &OutError) override
	{
		return Commit.TryCommit(OutError);
	}

  private:
	FUnrealAIGeminiInteractionsContinuationCommit Commit;
};

class FGeminiDecoder final : public IUnrealAINativeSseDecoder
{
  public:
	FGeminiDecoder(const FUnrealAIModelRequest &Request, IUnrealAINativeSseProtocol::FEventSink Sink,
				   IUnrealAINativeSseProtocol::FIgnoredEventObserver IgnoredEventObserver)
		: Decoder(Request, MoveTemp(Sink), MoveTemp(IgnoredEventObserver))
	{
	}

	bool PushBytes(const TConstArrayView<uint8> Bytes, FString &OutError) override
	{
		return Decoder.PushBytes(Bytes, OutError);
	}

	bool Finish(FString &OutError) override
	{
		return Decoder.Finish(OutError);
	}

	void Cancel() override
	{
		Decoder.Cancel();
	}

	bool IsTerminal() const override
	{
		return Decoder.IsTerminal();
	}

  private:
	FUnrealAIGeminiInteractionsStreamDecoder Decoder;
};

class FGeminiProtocolStrategy final : public IUnrealAINativeSseProtocol
{
  public:
	FName GetProviderName() const override
	{
		return GeminiStrategyProviderName;
	}

	int32 GetMaximumRequestBodyBytes() const override
	{
		return FUnrealAIGeminiInteractionsWireRequest::MaxBodyBytes;
	}

	int64 GetMaximumStreamBytes() const override
	{
		return FUnrealAIGeminiInteractionsStreamDecoder::MaxStreamBytes;
	}

	bool StageRequest(const FUnrealAIModelRequest &Request, const FUnrealAICredentialDestination &Destination,
					  TArray<uint8> &OutBodyUtf8,
					  TUniquePtr<IUnrealAINativeSseContinuationCommit> &OutContinuationCommit,
					  FUnrealAIModelError &OutError) const override
	{
		OutBodyUtf8.Reset();
		OutContinuationCommit.Reset();
		FUnrealAIGeminiInteractionsBuildContext Context;
		Context.Destination = Destination;
		FUnrealAIGeminiInteractionsWireRequest Wire;
		FUnrealAIGeminiInteractionsContinuationCommit Commit;
		if (!FUnrealAIGeminiInteractionsRequestBuilder::Stage(Request, Context, Wire, Commit, OutError))
		{
			return false;
		}
		OutBodyUtf8 = MoveTemp(Wire.BodyUtf8);
		OutContinuationCommit = MakeUnique<FGeminiContinuationCommit>(MoveTemp(Commit));
		return true;
	}

	TUniquePtr<IUnrealAINativeSseDecoder> CreateDecoder(const FUnrealAIModelRequest &Request, FEventSink Sink,
														FIgnoredEventObserver IgnoredEventObserver) const override
	{
		return MakeUnique<FGeminiDecoder>(Request, MoveTemp(Sink), MoveTemp(IgnoredEventObserver));
	}

  private:
};

FUnrealAINativeSseModelProviderConfig MakeNativeConfig(const FUnrealAIGeminiInteractionsProviderConfig &Config)
{
	FUnrealAINativeSseModelProviderConfig Native;
	Native.ProviderName = GeminiStrategyProviderName;
	Native.Connection = Config.Connection;
	Native.RelativePath = TEXT("/v1/interactions");
	Native.CredentialPresentation = EUnrealAIHttpCredentialPresentation::GeminiApiKeyHeader;
	Native.QueryProfile = EUnrealAIHttpQueryProfile::GeminiAltSse;
	Native.ModelProfiles = Config.ModelProfiles;
	Native.Protocol = MakeShared<FGeminiProtocolStrategy, ESPMode::ThreadSafe>();
	Native.ErrorCodePrefix = TEXT("gemini");
	Native.ForbiddenErrorCategory = EUnrealAIErrorCategory::PolicyDenied;
	Native.MaximumActiveRequests = Config.MaximumActiveRequests;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	Native.bDrainEventsSynchronouslyForTesting = Config.bDrainEventsSynchronouslyForTesting;
#endif
	return Native;
}
} // namespace

FUnrealAIModelProfileProjection
FUnrealAIGeminiInteractionsProviderConfig::MakeInteractionsModelProfile(const FString &ModelId)
{
	FUnrealAIModelProfileProjection Profile;
	Profile.ModelId = ModelId;
	Profile.Capabilities = EUnrealAIModelCapability::Text | EUnrealAIModelCapability::StreamingText |
						   EUnrealAIModelCapability::FunctionTools | EUnrealAIModelCapability::ParallelFunctionTools |
						   EUnrealAIModelCapability::StructuredOutput | EUnrealAIModelCapability::ImageInput |
						   EUnrealAIModelCapability::UsageReporting | EUnrealAIModelCapability::RequestCancellation;
	Profile.MaximumToolsPerRequest = 64;
	Profile.MaximumToolOutputsPerRequest = 64;
	Profile.MaximumInputMessages = FUnrealAIModelRequest::MaxInputMessages;
	Profile.MaximumOutputTokens = 64 * 1024;
	Profile.MaximumParallelToolCalls = 64;
	Profile.MaximumTextOutputBytes = FUnrealAIModelProfileProjection::MaxTextOutputUtf8Bytes;
	Profile.MaximumStructuredOutputBytes = FUnrealAIModelProfileProjection::MaxStructuredOutputUtf8Bytes;
	Profile.MaximumToolArgumentBytes = FUnrealAIModelProfileProjection::MaxToolArgumentUtf8Bytes;
	Profile.MaximumAggregateToolArgumentBytes = FUnrealAIModelProfileProjection::MaxAggregateToolArgumentUtf8Bytes;
	Profile.MaximumMetadataEntries = 8;
	return Profile;
}

bool FUnrealAIGeminiInteractionsProviderConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (ModelProfiles.IsEmpty() || ModelProfiles.Num() > MaxModelProfiles)
	{
		OutError = TEXT("Gemini Interactions provider requires an exact bounded model allowlist.");
		return false;
	}
	return MakeNativeConfig(*this).ValidateShape(OutError);
}

FUnrealAIGeminiInteractionsProvider::FUnrealAIGeminiInteractionsProvider(
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
	const FUnrealAIGeminiInteractionsProviderConfig &InConfig)
{
	Implementation = MakeUnique<FUnrealAINativeSseModelProvider>(MoveTemp(InConnections), MoveTemp(InTransport),
																 MakeNativeConfig(InConfig));
}

FUnrealAIGeminiInteractionsProvider::~FUnrealAIGeminiInteractionsProvider()
{
	BeginShutdown();
}

FName FUnrealAIGeminiInteractionsProvider::GetProviderName() const
{
	return Implementation->GetProviderName();
}

FUnrealAIModelProviderDescriptor FUnrealAIGeminiInteractionsProvider::Describe() const
{
	return Implementation->Describe();
}

EUnrealAIModelCapability FUnrealAIGeminiInteractionsProvider::GetCapabilities(const FString &ModelId) const
{
	return Implementation->GetCapabilities(ModelId);
}

FUnrealAIModelProfileProjection FUnrealAIGeminiInteractionsProvider::GetModelProjection(const FString &ModelId) const
{
	return Implementation->GetModelProjection(ModelId);
}

bool FUnrealAIGeminiInteractionsProvider::StartRequest(
	const FUnrealAIModelRequest &Request,
	TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
	TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIModelError &OutError)
{
	return Implementation->StartRequest(Request, MoveTemp(AccessContext), MoveTemp(Sink), Cancellation, OutHandle,
										OutError);
}

void FUnrealAIGeminiInteractionsProvider::BeginShutdown()
{
	if (Implementation.IsValid())
	{
		Implementation->BeginShutdown();
	}
}
