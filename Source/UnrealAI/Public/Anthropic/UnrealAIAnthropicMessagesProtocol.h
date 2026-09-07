// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Models/UnrealAIModelTypes.h"

/** Bounded credential-free Anthropic Messages request body. */
struct UNREALAI_API FUnrealAIAnthropicMessagesWireRequest final
{
	static constexpr int32 MaxBodyBytes = 4 * 1024 * 1024;
	static constexpr int32 MaxResolvedImageBytes = 2 * 1024 * 1024;

	TArray<uint8> BodyUtf8;

	bool ValidateShape(FString &OutError) const;
};

/** Destination-bound media policy supplied to request lowering. */
struct UNREALAI_API FUnrealAIAnthropicMessagesBuildContext final
{
	FUnrealAICredentialDestination Destination;
	int64 MaximumImageBytes = FUnrealAIAnthropicMessagesWireRequest::MaxResolvedImageBytes;

	bool ValidateShape(bool bRequiresImageResolver, FString &OutError) const;
};

/** Staged one-shot continuation consume committed only after transport admission. */
class UNREALAI_API FUnrealAIAnthropicMessagesContinuationCommit final
{
  public:
	FUnrealAIAnthropicMessagesContinuationCommit() = default;
	FUnrealAIAnthropicMessagesContinuationCommit(FUnrealAIAnthropicMessagesContinuationCommit &&) = default;
	FUnrealAIAnthropicMessagesContinuationCommit &operator=(FUnrealAIAnthropicMessagesContinuationCommit &&) = default;
	FUnrealAIAnthropicMessagesContinuationCommit(const FUnrealAIAnthropicMessagesContinuationCommit &) = delete;
	FUnrealAIAnthropicMessagesContinuationCommit &
	operator=(const FUnrealAIAnthropicMessagesContinuationCommit &) = delete;

	bool IsRequired() const;
	bool TryCommit(FString &OutError);
	void Reset();

  private:
	friend class FUnrealAIAnthropicMessagesRequestBuilder;
	TSharedPtr<const IUnrealAIModelContinuation, ESPMode::ThreadSafe> Continuation;
	FUnrealAIModelContinuationBinding Binding;
};

/** Strict native serializer for POST /v1/messages. */
class UNREALAI_API FUnrealAIAnthropicMessagesRequestBuilder final
{
  public:
	static bool Build(const FUnrealAIModelRequest &Request, const FUnrealAIAnthropicMessagesBuildContext &Context,
					  FUnrealAIAnthropicMessagesWireRequest &OutWireRequest, FUnrealAIModelError &OutError);
	static bool Stage(const FUnrealAIModelRequest &Request, const FUnrealAIAnthropicMessagesBuildContext &Context,
					  FUnrealAIAnthropicMessagesWireRequest &OutWireRequest,
					  FUnrealAIAnthropicMessagesContinuationCommit &OutContinuationCommit,
					  FUnrealAIModelError &OutError);
};

/** Incremental native Anthropic Messages SSE decoder. */
class UNREALAI_API FUnrealAIAnthropicMessagesStreamDecoder final
{
  public:
	using FEventSink = TFunction<void(FUnrealAIModelEvent &&)>;
	using FIgnoredEventObserver = TFunction<void()>;

	static constexpr int32 MaxEventBytes = 1024 * 1024;
	static constexpr int32 MaxStreamBytes = 16 * 1024 * 1024;
	static constexpr int32 MaxContentBlocks = 256;

	FUnrealAIAnthropicMessagesStreamDecoder(const FUnrealAIModelRequest &Request, FEventSink InSink,
											FIgnoredEventObserver InIgnoredEventObserver = {});
	~FUnrealAIAnthropicMessagesStreamDecoder();

	FUnrealAIAnthropicMessagesStreamDecoder(const FUnrealAIAnthropicMessagesStreamDecoder &) = delete;
	FUnrealAIAnthropicMessagesStreamDecoder &operator=(const FUnrealAIAnthropicMessagesStreamDecoder &) = delete;

	bool PushBytes(TConstArrayView<uint8> Bytes, FString &OutError);
	bool Finish(FString &OutError);
	void Cancel();
	bool IsTerminal() const;

  private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
