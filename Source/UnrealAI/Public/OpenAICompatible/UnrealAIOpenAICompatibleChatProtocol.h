// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Models/UnrealAIModelTypes.h"
#include "OpenAI/UnrealAIOpenAIResponsesProtocol.h"

/** Bounded credential-free Chat Completions request body. */
struct UNREALAI_API FUnrealAIOpenAICompatibleChatWireRequest final
{
	static constexpr int32 MaxBodyBytes = 4 * 1024 * 1024;

	TArray<uint8> BodyUtf8;
	/** Exact validated model copied while lowering; avoids reparsing the serialized body at dispatch. */
	FString ModelId;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Lowers the normalized model request to the conservative OpenAI-compatible
 * `/chat/completions` text/tool dialect.
 *
 * Provider execution invokes this only after the shared Responses admission
 * layer has staged its exact continuation transaction. This class deliberately
 * has no model-request overload: a compatibility helper must never stage and
 * then discard a one-shot continuation commit.
 */
class UNREALAI_API FUnrealAIOpenAICompatibleChatRequestBuilder final
{
  public:
	/** Converts one already validated shared Responses body without inspecting credentials. */
	static bool LowerResponsesWire(TConstArrayView<uint8> ResponsesBody,
								   FUnrealAIOpenAICompatibleChatWireRequest &OutWireRequest, FString &OutError);
};

/**
 * Incremental, bounded Chat Completions SSE decoder.
 *
 * Output chunks use the framework's shared normalized Responses event grammar.
 * This keeps lifecycle, continuation, correlation, and terminal validation in
 * the common provider implementation while the compatible wire remains fully
 * isolated in this module.
 */
class UNREALAI_API FUnrealAIOpenAICompatibleChatStreamDecoder final
{
  public:
	using FCanonicalChunkSink = TFunction<void(TArray<uint8> &&)>;

	static constexpr int32 MaxEventBytes = 1024 * 1024;
	static constexpr int32 MaxStreamBytes = 16 * 1024 * 1024;
	static constexpr int32 MaxToolCalls = 64;
	static constexpr int32 MaxTextBytes = FUnrealAIModelProfileProjection::MaxTextOutputUtf8Bytes;
	static constexpr int32 MaxToolArgumentBytes = FUnrealAIModelProfileProjection::MaxToolArgumentUtf8Bytes;
	static constexpr int32 MaxAggregateToolArgumentBytes =
		FUnrealAIModelProfileProjection::MaxAggregateToolArgumentUtf8Bytes;
	/** Matches the normalized event gate, including one reserved terminal event. */
	static constexpr int32 MaxNormalizedEvents = 8192;
	/** Raw compatible events are separately bounded before normalized expansion. */
	static constexpr int32 MaxWireEvents = 4096;

	explicit FUnrealAIOpenAICompatibleChatStreamDecoder(FString ExpectedModelId, FCanonicalChunkSink InSink);
	~FUnrealAIOpenAICompatibleChatStreamDecoder();

	FUnrealAIOpenAICompatibleChatStreamDecoder(const FUnrealAIOpenAICompatibleChatStreamDecoder &) = delete;
	FUnrealAIOpenAICompatibleChatStreamDecoder &operator=(const FUnrealAIOpenAICompatibleChatStreamDecoder &) = delete;

	bool PushBytes(TConstArrayView<uint8> Bytes, FString &OutError);
	bool Finish(FString &OutError);
	bool IsTerminal() const;

  private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
