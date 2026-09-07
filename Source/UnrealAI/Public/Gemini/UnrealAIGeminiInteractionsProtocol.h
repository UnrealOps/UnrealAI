// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Models/UnrealAIModelTypes.h"

/** Bounded credential-free Gemini Interactions request body. */
struct UNREALAI_API FUnrealAIGeminiInteractionsWireRequest final
{
	static constexpr int32 MaxBodyBytes = 4 * 1024 * 1024;
	static constexpr int32 MaxResolvedImageBytes = 2 * 1024 * 1024;

	TArray<uint8> BodyUtf8;

	bool ValidateShape(FString &OutError) const;
};

/** Destination-bound media policy supplied to Interactions request lowering. */
struct UNREALAI_API FUnrealAIGeminiInteractionsBuildContext final
{
	FUnrealAICredentialDestination Destination;
	int64 MaximumImageBytes = FUnrealAIGeminiInteractionsWireRequest::MaxResolvedImageBytes;

	bool ValidateShape(bool bRequiresImageResolver, FString &OutError) const;
};

/** Staged one-shot stateless history continuation consume. */
class UNREALAI_API FUnrealAIGeminiInteractionsContinuationCommit final
{
  public:
	FUnrealAIGeminiInteractionsContinuationCommit() = default;
	FUnrealAIGeminiInteractionsContinuationCommit(FUnrealAIGeminiInteractionsContinuationCommit &&) = default;
	FUnrealAIGeminiInteractionsContinuationCommit &
	operator=(FUnrealAIGeminiInteractionsContinuationCommit &&) = default;
	FUnrealAIGeminiInteractionsContinuationCommit(const FUnrealAIGeminiInteractionsContinuationCommit &) = delete;
	FUnrealAIGeminiInteractionsContinuationCommit &
	operator=(const FUnrealAIGeminiInteractionsContinuationCommit &) = delete;

	bool IsRequired() const;
	bool TryCommit(FString &OutError);
	void Reset();

  private:
	friend class FUnrealAIGeminiInteractionsRequestBuilder;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation;
	FUnrealAIModelContinuationBinding Binding;
};

/** Strict native serializer for stable POST /v1/interactions. */
class UNREALAI_API FUnrealAIGeminiInteractionsRequestBuilder final
{
  public:
	static bool Build(const FUnrealAIModelRequest &Request, const FUnrealAIGeminiInteractionsBuildContext &Context,
					  FUnrealAIGeminiInteractionsWireRequest &OutWireRequest, FUnrealAIModelError &OutError);
	static bool Stage(const FUnrealAIModelRequest &Request, const FUnrealAIGeminiInteractionsBuildContext &Context,
					  FUnrealAIGeminiInteractionsWireRequest &OutWireRequest,
					  FUnrealAIGeminiInteractionsContinuationCommit &OutContinuationCommit,
					  FUnrealAIModelError &OutError);
};

/** Incremental native Gemini Interactions SSE decoder. */
class UNREALAI_API FUnrealAIGeminiInteractionsStreamDecoder final
{
  public:
	using FEventSink = TFunction<void(FUnrealAIModelEvent &&)>;
	using FIgnoredEventObserver = TFunction<void()>;

	static constexpr int32 MaxEventBytes = 1024 * 1024;
	static constexpr int32 MaxStreamBytes = 16 * 1024 * 1024;
	static constexpr int32 MaxSteps = 256;

	FUnrealAIGeminiInteractionsStreamDecoder(const FUnrealAIModelRequest &Request, FEventSink InSink,
											 FIgnoredEventObserver InIgnoredEventObserver = {});
	~FUnrealAIGeminiInteractionsStreamDecoder();

	FUnrealAIGeminiInteractionsStreamDecoder(const FUnrealAIGeminiInteractionsStreamDecoder &) = delete;
	FUnrealAIGeminiInteractionsStreamDecoder &operator=(const FUnrealAIGeminiInteractionsStreamDecoder &) = delete;

	bool PushBytes(TConstArrayView<uint8> Bytes, FString &OutError);
	bool Finish(FString &OutError);
	void Cancel();
	bool IsTerminal() const;

  private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
