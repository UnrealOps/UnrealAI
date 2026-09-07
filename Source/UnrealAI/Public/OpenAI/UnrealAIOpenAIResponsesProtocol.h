// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Models/UnrealAIModelTypes.h"

/**
 * Public fault vocabulary for one provider using the shared Responses wire protocol.
 *
 * The wire implementation is intentionally shared, but provider-facing errors must
 * retain the identity of the provider that admitted the request.
 */
struct UNREALAI_API FUnrealAIOpenAIResponsesPublicFaultPolicy final
{
	static constexpr int32 MaxCodePrefixUtf8Bytes = 32;
	static constexpr int32 MaxProviderDisplayNameUtf8Bytes = 64;

	FName CodePrefix = TEXT("openai");
	FString ProviderDisplayName = TEXT("OpenAI");

	bool ValidateShape(FString &OutError) const;
	void ApplyTo(FUnrealAIModelError &InOutError) const;
	static FUnrealAIOpenAIResponsesPublicFaultPolicy OpenAI();
};

/** Bounded credential-free wire request ready for the trusted HTTPS transport. */
struct UNREALAI_API FUnrealAIOpenAIResponsesWireRequest final
{
	static constexpr int32 MaxBodyBytes = 4 * 1024 * 1024;

	TArray<uint8> BodyUtf8;

	bool ValidateShape(FString &OutError) const;
};

/**
 * One staged, exact continuation consume. Wire construction copies the
 * provider-owned payload without revoking it; an adapter commits this token
 * only after every other admission step has succeeded.
 */
class UNREALAI_API FUnrealAIOpenAIResponsesContinuationCommit final
{
  public:
	FUnrealAIOpenAIResponsesContinuationCommit() = default;
	FUnrealAIOpenAIResponsesContinuationCommit(FUnrealAIOpenAIResponsesContinuationCommit &&) = default;
	FUnrealAIOpenAIResponsesContinuationCommit &operator=(FUnrealAIOpenAIResponsesContinuationCommit &&) = default;
	FUnrealAIOpenAIResponsesContinuationCommit(const FUnrealAIOpenAIResponsesContinuationCommit &) = delete;
	FUnrealAIOpenAIResponsesContinuationCommit &operator=(const FUnrealAIOpenAIResponsesContinuationCommit &) = delete;

	bool IsRequired() const;
	/** Exact-binding, one-shot consume. This must be the adapter's final fallible admission step. */
	bool TryCommit(FString &OutError);
	void Reset();

  private:
	friend class FUnrealAIOpenAIResponsesRequestBuilder;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation;
	FUnrealAIModelContinuationBinding Binding;
};

/** Strict, non-registering serializer for the shared Responses wire contract. */
class UNREALAI_API FUnrealAIOpenAIResponsesRequestBuilder final
{
  public:
	static bool Build(const FUnrealAIModelRequest &Request, FUnrealAIOpenAIResponsesWireRequest &OutWireRequest,
					  FUnrealAIModelError &OutError);
	/** Reuses the Responses wire contract while binding continuations to an isolated compatible provider identity. */
	static bool BuildForProvider(FName ProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
								 const FUnrealAIModelRequest &Request,
								 FUnrealAIOpenAIResponsesWireRequest &OutWireRequest, FUnrealAIModelError &OutError);
	/**
	 * Builds a bounded wire copy without consuming its continuation. The caller
	 * must either discard OutContinuationCommit on rejection or TryCommit it
	 * after transport and handle admission have committed.
	 */
	static bool StageForProvider(FName ProviderName, const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
								 const FUnrealAIModelRequest &Request,
								 FUnrealAIOpenAIResponsesWireRequest &OutWireRequest,
								 FUnrealAIOpenAIResponsesContinuationCommit &OutContinuationCommit,
								 FUnrealAIModelError &OutError);
};

/**
 * Incremental SSE decoder for one Responses request. It accepts arbitrary byte
 * split boundaries and emits owned provider-neutral events through Sink.
 */
class UNREALAI_API FUnrealAIOpenAIResponsesStreamDecoder final
{
  public:
	using FEventSink = TFunction<void(FUnrealAIModelEvent &&)>;
	/** Content-free notification that one bounded, sequenced provider event type was ignored. */
	using FIgnoredEventObserver = TFunction<void()>;

	static constexpr int32 MaxEventBytes = 1024 * 1024;
	static constexpr int32 MaxStreamBytes = 16 * 1024 * 1024;
	static constexpr int32 MaxNormalizedToolCalls = 64;
	static constexpr int32 MaxRetainedOutputItems = MaxNormalizedToolCalls + 256;
	static constexpr int32 MaxRetainedTextParts = 256;
	/** Conservative physical memory budget for retained decoded DOM and text assembly state. */
	static constexpr int64 MaxRetainedResponseStateBytes = 8 * 1024 * 1024;

	FUnrealAIOpenAIResponsesStreamDecoder(const FUnrealAIModelRequest &Request, FEventSink InSink,
										  FIgnoredEventObserver InIgnoredEventObserver = {});
	FUnrealAIOpenAIResponsesStreamDecoder(FName ProviderName,
										  const FUnrealAIOpenAIResponsesPublicFaultPolicy &FaultPolicy,
										  const FUnrealAIModelRequest &Request, FEventSink InSink,
										  FIgnoredEventObserver InIgnoredEventObserver = {});
	~FUnrealAIOpenAIResponsesStreamDecoder();

	FUnrealAIOpenAIResponsesStreamDecoder(const FUnrealAIOpenAIResponsesStreamDecoder &) = delete;
	FUnrealAIOpenAIResponsesStreamDecoder &operator=(const FUnrealAIOpenAIResponsesStreamDecoder &) = delete;

	/** Returns false after a terminal protocol failure or when a bound is exceeded. */
	bool PushBytes(TConstArrayView<uint8> Bytes, FString &OutError);
	/** Completes parsing; a stream without a provider terminal fails closed. */
	bool Finish(FString &OutError);
	/** Emits one cancellation terminal if the provider has not already terminated. */
	void Cancel();
	bool IsTerminal() const;

  private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
