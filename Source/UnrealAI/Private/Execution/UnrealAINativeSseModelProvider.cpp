// Copyright EngineWorks. All Rights Reserved.

#include "Execution/UnrealAINativeSseModelProvider.h"
#include "Containers/Ticker.h"
#include "Runtime/UnrealAIFailureClassification.h"

#include "Algo/AnyOf.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAINativeSseProvider, Log, All);

namespace
{
constexpr int32 MaxPendingHttpEventsPerRequest = 1024;
constexpr int64 MaxPendingHttpEventBytesPerRequest = 2 * 1024 * 1024;
constexpr int32 MaxRejectedResponseBodyBytes = 64 * 1024;

FName MakeProviderCode(const FName Prefix, const TCHAR *Suffix)
{
	return FName(*(Prefix.ToString() + TEXT("_") + Suffix));
}

FUnrealAIModelError MakeProviderError(const FName Prefix, const EUnrealAIErrorCategory Category, const TCHAR *Suffix,
									  const TCHAR *UserMessage, const TCHAR *Diagnostic, const bool bRetryable = false)
{
	FUnrealAIModelError Error;
	Error.Category = Category;
	Error.Code = MakeProviderCode(Prefix, Suffix);
	Error.UserMessage = FText::FromString(UserMessage);
	Error.DiagnosticMessage = Diagnostic;
	Error.bRetryable = bRetryable;
	return Error;
}

bool MatchesDestination(const FUnrealAICredentialDestination &A, const FUnrealAICredentialDestination &B)
{
	return A.ModelProviderName == B.ModelProviderName && A.AccountAuthProviderName == B.AccountAuthProviderName &&
		   A.AuthProfileId == B.AuthProfileId && A.AccountId == B.AccountId && A.TenantRealm == B.TenantRealm &&
		   A.BillingPrincipalId == B.BillingPrincipalId && A.PayerHandle == B.PayerHandle &&
		   A.AuthScheme == B.AuthScheme && A.BillingMode == B.BillingMode && A.EndpointOrigin == B.EndpointOrigin &&
		   A.Audience == B.Audience && A.ConnectionRevision == B.ConnectionRevision &&
		   A.EndpointPolicyRevision == B.EndpointPolicyRevision;
}

bool MatchesConnection(const FUnrealAIConnectionDescriptor &A, const FUnrealAIConnectionDescriptor &B)
{
	return A.SchemaVersion == B.SchemaVersion && A.ConnectionRevision == B.ConnectionRevision &&
		   A.ConnectionAlias == B.ConnectionAlias && A.EndpointProfileId == B.EndpointProfileId &&
		   MatchesDestination(A.CredentialDestination, B.CredentialDestination);
}

FUnrealAIModelProviderDescriptor MakeDescriptor(const FUnrealAINativeSseModelProviderConfig &Config)
{
	FUnrealAIModelProviderDescriptor Descriptor;
	Descriptor.ProviderName = Config.ProviderName;
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

const FUnrealAIModelProfileProjection *FindProjection(const FUnrealAINativeSseModelProviderConfig &Config,
													  const FString &ModelId)
{
	return Config.ModelProfiles.FindByPredicate([&ModelId](const FUnrealAIModelProfileProjection &Profile)
												{ return Profile.ModelId == ModelId; });
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

int64 GetRetainedHttpEventBytes(const FUnrealAIHttpEvent &Event)
{
	int64 Total = static_cast<int64>(sizeof(FUnrealAIHttpEvent));
	const auto AddAllocation = [&Total](const SIZE_T Bytes)
	{
		if (static_cast<uint64>(Bytes) > static_cast<uint64>(MAX_int64) ||
			Total > MAX_int64 - static_cast<int64>(Bytes))
		{
			Total = MAX_int64;
			return false;
		}
		Total += static_cast<int64>(Bytes);
		return true;
	};
	if (!AddAllocation(Event.BodyChunk.GetAllocatedSize()) ||
		!AddAllocation(Event.Response.ContentType.GetAllocatedSize()) ||
		!AddAllocation(Event.Response.ProviderRequestId.GetAllocatedSize()) ||
		!AddAllocation(Event.Error.DiagnosticMessage.GetAllocatedSize()) ||
		!AddAllocation(Event.Error.ProviderRequestId.GetAllocatedSize()) ||
		!AddAllocation(Event.Error.Metadata.GetAllocatedSize()))
	{
		return MAX_int64;
	}
	for (const TPair<FName, FString> &Pair : Event.Error.Metadata)
	{
		if (!AddAllocation(Pair.Value.GetAllocatedSize()))
		{
			return MAX_int64;
		}
	}
	return Total;
}

bool TryMakeProviderRequestId(const EUnrealAIModelProviderRequestIdKind Kind, const FString &Value,
							  FUnrealAIModelProviderRequestId &OutId)
{
	OutId = {};
	if (Value.IsEmpty())
	{
		return false;
	}
	OutId.Kind = Kind;
	OutId.Value = Value;
	FString Error;
	if (!OutId.ValidateShape(Error))
	{
		OutId = {};
		return false;
	}
	return true;
}

class FNativeSseRequestOperation final : public IUnrealAIHttpEventSink,
										 public IUnrealAIModelRequestHandle,
										 public TSharedFromThis<FNativeSseRequestOperation, ESPMode::ThreadSafe>
{
  public:
	FNativeSseRequestOperation(
		const FUnrealAIModelRequest &Request, const FName InErrorPrefix,
		TSharedRef<const IUnrealAINativeSseProtocol, ESPMode::ThreadSafe> Protocol,
		const EUnrealAIErrorCategory InForbiddenErrorCategory,
		TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> InModelSink,
		TFunction<void(const FUnrealAIRequestId &, const FNativeSseRequestOperation *)> InOnLogicalTerminal,
		TFunction<void(const FNativeSseRequestOperation *)> InOnPhysicalSettled,
		const bool bInDrainSynchronouslyForTesting,
		TSharedRef<FUnrealAIPhysicalRequestPermit, ESPMode::ThreadSafe> InPhysicalPermit,
		const FUnrealAINativeSseModelProviderConfig &Config, const FUnrealAIDeadline &InDeadline,
		const FUnrealAICancellationToken &InCancellation)
		: RequestId(Request.RequestId), ErrorPrefix(InErrorPrefix), ForbiddenErrorCategory(InForbiddenErrorCategory),
		  ModelSink(MoveTemp(InModelSink)), OnLogicalTerminal(MoveTemp(InOnLogicalTerminal)),
		  OnPhysicalSettled(MoveTemp(InOnPhysicalSettled)), PhysicalPermit(MoveTemp(InPhysicalPermit))
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
		  ,
		  bDrainSynchronouslyForTesting(bInDrainSynchronouslyForTesting)
#endif
	{
#if !(WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS)
		(void)bInDrainSynchronouslyForTesting;
#endif
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
		BeforeFirstDrainForTesting = Config.BeforeFirstDrainForTesting;
		AfterFirstQueuedDrainBodyForTesting = Config.AfterFirstQueuedDrainBodyForTesting;
#endif
		Clock = Config.Clock;
		Deadline = InDeadline;
		ParentCancellation = InCancellation;
		MapPublicError = Config.MapPublicError;
		MaximumPendingHttpEvents = Config.MaximumPendingHttpEvents;
		UnauthorizedErrorCode = Config.UnauthorizedErrorCode;
		ForbiddenErrorCode = Config.ForbiddenErrorCode;
		Decoder = Protocol->CreateDecoder(
			Request, [this](FUnrealAIModelEvent &&Event) { ForwardDecodedEventLocked(MoveTemp(Event)); },
			[]()
			{
				UE_LOG(LogUnrealAINativeSseProvider, VeryVerbose,
					   TEXT("Ignored one bounded, sequenced, unrecognized native provider event."));
			});
	}

	void SettleRejectedAdmission()
	{
		NotifyLogicalTerminal();
		NotifyPhysicalSettled();
	}

	bool IsReady() const
	{
		return Decoder.IsValid();
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	bool IsLogicallyComplete() const override
	{
		return bLogicalTerminalNotified.load(std::memory_order_acquire);
	}

	bool IsPhysicallySettled() const override
	{
		return bPhysicalSettledNotified.load(std::memory_order_acquire);
	}

	void Cancel() override
	{
		bool bStartDrain = false;
		{
			FScopeLock Lock(&EventMutex);
			EmitTerminalLocked(EUnrealAIModelEventKind::Cancelled,
							   MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::Cancelled,
												 TEXT("cancelled"), TEXT("The model request was cancelled."),
																		 TEXT("The caller cancelled this model turn.")),
													  false);
			if (bHttpAdmissionPublished && !bDraining && !PendingModelEvents.IsEmpty())
			{
				bDraining = true;
				bStartDrain = true;
			}
		}
		// Cancellation is a logical terminal even when the network has not acknowledged it.
		RequestPhysicalCancellation();
		if (bStartDrain)
		{
			verify(ScheduleDrain());
		}
	}

	bool PollDeadline()
	{
		if (IsLogicallyComplete())
		{
			return false;
		}
		if (ParentCancellation.IsCancellationRequested() &&
			ParentCancellation.GetReason() != EUnrealAICancellationReason::Timeout)
		{
			Cancel();
			return false;
		}
		if (!Deadline.IsExpired(*Clock) && !ParentCancellation.IsCancellationRequested())
		{
			return true;
		}
		bool bStartDrain = false;
		{
			FScopeLock Lock(&EventMutex);
			EmitTerminalLocked(
				EUnrealAIModelEventKind::TimedOut,
				MakeProviderError(
					ErrorPrefix, EUnrealAIErrorCategory::Timeout,
					TEXT("timeout"),
						 TEXT("The model request exceeded its deadline."),
							  TEXT("The absolute model deadline includes stream and callback queue time."), true),
						 false);
			if (bHttpAdmissionPublished && !bDraining && !PendingModelEvents.IsEmpty())
			{
				bDraining = true;
				bStartDrain = true;
			}
		}
		RequestPhysicalCancellation();
		if (bStartDrain)
		{
			verify(ScheduleDrain());
		}
		return false;
	}

	bool CommitHttpAdmission(TSharedRef<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> InHandle)
	{
		TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> HandleToCancel;
		{
			FScopeLock Lock(&HandleMutex);
			HttpHandle = InHandle;
			if (bPhysicalCancellationRequested)
			{
				HandleToCancel = HttpHandle;
			}
		}
		{
			FScopeLock Lock(&EventMutex);
			if (bHttpAdmissionAborted || Terminal.IsComplete())
			{
				return false;
			}
			bHttpAdmissionCommitted = true;
		}
		if (HandleToCancel.IsValid())
		{
			HandleToCancel->Cancel();
			NotifyLogicalTerminal();
		}
		return true;
	}

	void PublishHttpAdmission()
	{
		bool bStartDrain = false;
		{
			FScopeLock Lock(&EventMutex);
			check(bHttpAdmissionCommitted && !bHttpAdmissionAborted);
			bHttpAdmissionPublished = true;
			if (!bDraining && (!PendingHttpEvents.IsEmpty() || !PendingModelEvents.IsEmpty()))
			{
				bDraining = true;
				bStartDrain = true;
			}
		}
		if (bStartDrain)
		{
			verify(ScheduleDrain());
		}
		const TWeakPtr<FNativeSseRequestOperation, ESPMode::ThreadSafe> Weak = AsShared();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Weak](float)
			{
				const auto Operation = Weak.Pin();
				return Operation.IsValid() && Operation->PollDeadline();
			}));
	}

	void AbortHttpAdmission(TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> Handle)
	{
		{
			FScopeLock Lock(&EventMutex);
			bHttpAdmissionAborted = true;
			PendingHttpEvents.Reset();
			PendingModelEvents.Reset();
			PendingHttpEventBytes = 0;
			Terminal.TryComplete(EUnrealAITerminalKind::Failed);
		}
		if (Handle.IsValid())
		{
			Handle->Cancel();
		}
		NotifyLogicalTerminal();
	}

	void EnqueueHttpEvent(FUnrealAIHttpEvent &&Event) override
	{
		const int64 RetainedBytes = GetRetainedHttpEventBytes(Event);
		bool bStartDrain = false;
		bool bPhysicalTerminal = false;
		bool bCancelPhysical = false;
		{
			FScopeLock Lock(&EventMutex);
			if (!bPhysicalCallbackClosed)
			{
				bPhysicalTerminal = Event.IsTerminal();
				bPhysicalCallbackClosed = bPhysicalTerminal;
				if (!bHttpAdmissionAborted && !Terminal.IsComplete())
				{
					if (Event.RequestId != RequestId)
					{
						if (bPhysicalTerminal)
						{
							EmitTerminalLocked(
								EUnrealAIModelEventKind::Failed,
								MakeProviderError(
									ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
									TEXT("http_event_invalid"),
										 TEXT("The model transport returned an invalid event."),
											  TEXT("Native SSE provider rejected mismatched transport correlation.")),
										 true);
							bCancelPhysical = true;
						}
					}
					else if (PendingHttpEvents.Num() >= MaximumPendingHttpEvents ||
							 RetainedBytes > MaxPendingHttpEventBytesPerRequest ||
							 PendingHttpEventBytes > MaxPendingHttpEventBytesPerRequest - RetainedBytes)
					{
						PendingHttpEvents.Reset();
						PendingHttpEventBytes = 0;
						EmitTerminalLocked(
							EUnrealAIModelEventKind::Failed,
							MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::Busy,
											  TEXT("event_capacity"),
												   TEXT("The model provider produced too many pending events."),
														TEXT("Native SSE provider bounded retained transport events."),
															 true),
												   true);
						bCancelPhysical = true;
					}
					else
					{
						PendingHttpEvents.Add(MoveTemp(Event));
						PendingHttpEventBytes += RetainedBytes;
					}
					if (bHttpAdmissionPublished && !bDraining &&
						(!PendingHttpEvents.IsEmpty() || !PendingModelEvents.IsEmpty()))
					{
						bDraining = true;
						bStartDrain = true;
					}
				}
			}
		}
		if (bPhysicalTerminal)
		{
			NotifyPhysicalSettled();
		}
		if (bCancelPhysical)
		{
			RequestPhysicalCancellation();
		}
		if (bStartDrain)
		{
			verify(ScheduleDrain());
		}
	}

  private:
	bool ScheduleDrain()
	{
		const TSharedRef<FNativeSseRequestOperation, ESPMode::ThreadSafe> Self = AsShared();
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
		if (bDrainSynchronouslyForTesting)
		{
			Self->DrainHttpEvents();
			return true;
		}
#endif
		(void)Async(EAsyncExecution::ThreadPool,
					[Self]()
					{
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
						TFunction<void()> After;
						if (!Self->bTestDrainObserved.exchange(true))
						{
							if (Self->BeforeFirstDrainForTesting)
							{
								Self->BeforeFirstDrainForTesting();
							}
							After = MoveTemp(Self->AfterFirstQueuedDrainBodyForTesting);
						}
#endif
						Self->DrainHttpEvents();
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
						if (After)
						{
							After();
						}
#endif
					});
		return true;
	}

	void DrainHttpEvents()
	{
		for (;;)
		{
			PollDeadline();
			TArray<FUnrealAIModelEvent> Deliveries;
			bool bCancelPhysical = false;
			{
				FScopeLock Lock(&EventMutex);
				if (PendingModelEvents.IsEmpty() && !Terminal.IsComplete() && !PendingHttpEvents.IsEmpty())
				{
					const int64 RetainedBytes = GetRetainedHttpEventBytes(PendingHttpEvents[0]);
					FUnrealAIHttpEvent Event = MoveTemp(PendingHttpEvents[0]);
					PendingHttpEvents.RemoveAt(0, 1, EAllowShrinking::No);
					PendingHttpEventBytes = FMath::Max<int64>(0, PendingHttpEventBytes - RetainedBytes);
					ProcessHttpEventLocked(MoveTemp(Event));
				}
				if (!PendingModelEvents.IsEmpty())
				{
					Deliveries = MoveTemp(PendingModelEvents);
					PendingModelEvents.Reset();
					bCancelPhysical = bCancelPhysicalAfterTerminal;
					bCancelPhysicalAfterTerminal = false;
				}
				else if (!Terminal.IsComplete() && !PendingHttpEvents.IsEmpty())
				{
					continue;
				}
				else
				{
					if (Terminal.IsComplete())
					{
						PendingHttpEvents.Reset();
						PendingHttpEventBytes = 0;
					}
					bDraining = false;
					return;
				}
			}
			if (bCancelPhysical)
			{
				RequestPhysicalCancellation();
			}
			for (FUnrealAIModelEvent &Delivery : Deliveries)
			{
				if (Delivery.IsTerminal())
				{
					NotifyLogicalTerminal();
				}
				ModelSink->EnqueueModelEvent(MoveTemp(Delivery));
			}
		}
	}

	void ProcessHttpEventLocked(FUnrealAIHttpEvent &&Event)
	{
		FString ShapeError;
		if (!Event.ValidateShape(ShapeError) || Event.Sequence <= LastHttpSequence)
		{
			EmitTerminalLocked(
				EUnrealAIModelEventKind::Failed,
				MakeProviderError(
					ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
					TEXT("http_event_invalid"),
						 TEXT("The model transport returned an invalid event."),
							  TEXT("Native SSE provider rejected an invalid or out-of-order transport event.")),
						 true);
			return;
		}
		LastHttpSequence = Event.Sequence;
		switch (Event.Kind)
		{
		case EUnrealAIHttpEventKind::ResponseStarted:
			if (bResponseStarted)
			{
				EmitTerminalLocked(
					EUnrealAIModelEventKind::Failed,
					MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
									  TEXT("response_started_duplicate"),
										   TEXT("The model transport returned an invalid response."),
												TEXT("Native SSE provider received duplicate response metadata.")),
										   true);
				return;
			}
			bResponseStarted = true;
			Response = Event.Response;
			{
				FUnrealAIModelProviderRequestId SafeTransportId;
				if (!TryMakeProviderRequestId(EUnrealAIModelProviderRequestIdKind::TransportRequest,
											  Response.ProviderRequestId, SafeTransportId))
				{
					Response.ProviderRequestId.Reset();
				}
			}
			if (Response.StatusCode != 200)
			{
				bRejectedHttpResponse = true;
				return;
			}
			if (!Response.ContentType.StartsWith(TEXT("text/event-stream"), ESearchCase::IgnoreCase))
			{
				bRejectedHttpResponse = true;
				EmitTerminalLocked(
					EUnrealAIModelEventKind::Failed,
					MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
									  TEXT("content_type_invalid"),
										   TEXT("The model provider returned an unsupported response format."),
												TEXT("Native SSE provider requires text/event-stream.")),
										   true);
			}
			return;
		case EUnrealAIHttpEventKind::BodyChunk:
			if (!bResponseStarted)
			{
				EmitTerminalLocked(
					EUnrealAIModelEventKind::Failed,
					MakeProviderError(
						ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
						TEXT("body_before_response"),
							 TEXT("The model transport returned an invalid response."),
								  TEXT("Native SSE provider received body bytes before response metadata.")),
							 true);
				return;
			}
			if (bRejectedHttpResponse)
			{
				if (!bRejectedResponseBodyOverflow &&
					Event.BodyChunk.Num() <= MaxRejectedResponseBodyBytes - RejectedResponseBody.Num())
				{
					RejectedResponseBody.Append(Event.BodyChunk);
				}
				else
				{
					RejectedResponseBody.Empty();
					bRejectedResponseBodyOverflow = true;
				}
				return;
			}
			if (Decoder.IsValid())
			{
				FString DecodeError;
				if (!Decoder->PushBytes(Event.BodyChunk, DecodeError) && !Terminal.IsComplete())
				{
					EmitTerminalLocked(
						EUnrealAIModelEventKind::Failed,
						MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
										  TEXT("stream_decode_failed"),
											   TEXT("The model provider returned an invalid stream."),
													TEXT("Native SSE decoder failed without a terminal event.")),
											   true);
				}
			}
			return;
		case EUnrealAIHttpEventKind::Completed:
			if (!bResponseStarted)
			{
				EmitTerminalLocked(
					EUnrealAIModelEventKind::Failed,
					MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
									  TEXT("completion_before_response"),
										   TEXT("The model transport returned an invalid response."),
												TEXT("Native SSE provider completed before response metadata.")),
										   false);
				return;
			}
			if (bRejectedHttpResponse)
			{
				const bool bPermanentQuota = Response.StatusCode == 429 && !bRejectedResponseBodyOverflow &&
											 UE::UnrealAI::Reliability::IsPermanentQuotaResponse(RejectedResponseBody);
				RejectedResponseBody.Empty();
				EUnrealAIErrorCategory Category = EUnrealAIErrorCategory::Provider;
				if (Response.StatusCode == 401)
				{
					Category = EUnrealAIErrorCategory::NotAuthorized;
				}
				else if (Response.StatusCode == 403)
				{
					Category = ForbiddenErrorCategory;
				}
				else if (Response.StatusCode == 429)
				{
					Category = EUnrealAIErrorCategory::RateLimited;
				}
				FUnrealAIModelError Error = MakeProviderError(
					ErrorPrefix, Category,
					TEXT("http_status"), TEXT("The model provider rejected the request."),
											  TEXT("Native SSE provider returned a non-success HTTP status."),
												   !bPermanentQuota && UE::UnrealAI::Reliability::IsRetryableHttpStatus(
																		   Response.StatusCode));
				if (Response.StatusCode == 401 && !UnauthorizedErrorCode.IsNone())
				{
					Error.Code = UnauthorizedErrorCode;
				}
				if (Response.StatusCode == 403 && !ForbiddenErrorCode.IsNone())
				{
					Error.Code = ForbiddenErrorCode;
				}
				if (bPermanentQuota)
				{
					Error.Code = FName(*(ErrorPrefix.ToString() + TEXT("_quota_exhausted")));
				}
				Error.RetryAfterSeconds = bPermanentQuota ? 0.0f : Response.RetryAfterSeconds;
				Error.ProviderRequestId = Response.ProviderRequestId;
				Error.Metadata.Add(TEXT("http_status"), FString::FromInt(Response.StatusCode));
				EmitTerminalLocked(EUnrealAIModelEventKind::Failed, MoveTemp(Error), false);
				return;
			}
			if (Decoder.IsValid() && !Decoder->IsTerminal())
			{
				FString DecodeError;
				if (!Decoder->Finish(DecodeError) && !Terminal.IsComplete())
				{
					EmitTerminalLocked(
						EUnrealAIModelEventKind::Failed,
						MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
										  TEXT("stream_terminal_missing"),
											   TEXT("The model provider returned an incomplete stream."),
													TEXT("Native SSE decoder ended without a terminal event.")),
											   false);
				}
			}
			return;
		case EUnrealAIHttpEventKind::Cancelled:
			EmitTerminalLocked(EUnrealAIModelEventKind::Cancelled, MoveTemp(Event.Error), false);
			return;
		case EUnrealAIHttpEventKind::TimedOut:
			EmitTerminalLocked(EUnrealAIModelEventKind::TimedOut, MoveTemp(Event.Error), false);
			return;
		case EUnrealAIHttpEventKind::Failed:
			EmitTerminalLocked(EUnrealAIModelEventKind::Failed, MoveTemp(Event.Error), false);
			return;
		case EUnrealAIHttpEventKind::Invalid:
		default:
			EmitTerminalLocked(
				EUnrealAIModelEventKind::Failed,
				MakeProviderError(ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
								  TEXT("http_event_invalid"),
									   TEXT("The model transport returned an invalid event."),
											TEXT("Native SSE provider received an unknown transport event kind.")),
									   true);
			return;
		}
	}

	bool AppendTransportRequestIdLocked(FUnrealAIModelEvent &Event) const
	{
		FUnrealAIModelProviderRequestId TransportId;
		if (!TryMakeProviderRequestId(EUnrealAIModelProviderRequestIdKind::TransportRequest, Response.ProviderRequestId,
									  TransportId))
		{
			return true;
		}
		for (const FUnrealAIModelProviderRequestId &Existing : Event.ProviderRequestIds)
		{
			if (Existing.Kind == TransportId.Kind)
			{
				return Existing.Value == TransportId.Value;
			}
		}
		Event.ProviderRequestIds.Add(MoveTemp(TransportId));
		Event.ProviderRequestIds.Sort(
			[](const FUnrealAIModelProviderRequestId &A, const FUnrealAIModelProviderRequestId &B)
			{ return static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind); });
		return true;
	}

	void ForwardDecodedEventLocked(FUnrealAIModelEvent &&Event)
	{
		FString ShapeError;
		if (MapPublicError)
		{
			MapPublicError(Event.Error);
		}
		if (!(Event.RequestId == RequestId) || Event.SequenceNumber <= LastModelSequence ||
			!AppendTransportRequestIdLocked(Event) || !Event.ValidateShape(ShapeError))
		{
			EmitTerminalLocked(
				EUnrealAIModelEventKind::Failed,
				MakeProviderError(
					ErrorPrefix, EUnrealAIErrorCategory::ProviderProtocol,
					TEXT("model_event_invalid"),
						 TEXT("The model provider returned an invalid event."),
							  TEXT("Native SSE decoder emitted an invalid, misbound, or out-of-order event.")),
						 true);
			return;
		}
		KnownProviderRequestIds = Event.ProviderRequestIds;
		if (Event.IsTerminal())
		{
			EUnrealAITerminalKind Kind = EUnrealAITerminalKind::Failed;
			if (Event.Kind == EUnrealAIModelEventKind::Completed)
			{
				Kind = EUnrealAITerminalKind::Succeeded;
			}
			else if (Event.Kind == EUnrealAIModelEventKind::Cancelled)
			{
				Kind = EUnrealAITerminalKind::Cancelled;
			}
			else if (Event.Kind == EUnrealAIModelEventKind::TimedOut)
			{
				Kind = EUnrealAITerminalKind::TimedOut;
			}
			if (!Terminal.TryComplete(Kind))
			{
				return;
			}
			bCancelPhysicalAfterTerminal = true;
		}
		else if (Terminal.IsComplete())
		{
			return;
		}
		if (Event.Kind == EUnrealAIModelEventKind::Failed && !bStartedForwarded)
		{
			QueueStartedLocked();
			Event.SequenceNumber = ++LastModelSequence;
		}
		else
		{
			LastModelSequence = Event.SequenceNumber;
			bStartedForwarded |= Event.Kind == EUnrealAIModelEventKind::Started;
		}
		PendingModelEvents.Add(MoveTemp(Event));
	}

	void PopulateCorrelationLocked(FUnrealAIModelEvent &Event) const
	{
		Event.RequestId = RequestId;
		Event.ProviderRequestIds = KnownProviderRequestIds;
		AppendTransportRequestIdLocked(Event);
	}

	void QueueStartedLocked()
	{
		if (bStartedForwarded)
		{
			return;
		}
		FUnrealAIModelEvent Started;
		PopulateCorrelationLocked(Started);
		Started.Kind = EUnrealAIModelEventKind::Started;
		Started.SequenceNumber = ++LastModelSequence;
		PendingModelEvents.Add(MoveTemp(Started));
		bStartedForwarded = true;
	}

	void EmitTerminalLocked(const EUnrealAIModelEventKind Kind, FUnrealAIModelError Error, const bool bCancelPhysical)
	{
		if (MapPublicError)
		{
			MapPublicError(Error);
		}
		EUnrealAITerminalKind TerminalKind = EUnrealAITerminalKind::Failed;
		if (Kind == EUnrealAIModelEventKind::Cancelled)
		{
			TerminalKind = EUnrealAITerminalKind::Cancelled;
		}
		else if (Kind == EUnrealAIModelEventKind::TimedOut)
		{
			TerminalKind = EUnrealAITerminalKind::TimedOut;
		}
		if (!Terminal.TryComplete(TerminalKind))
		{
			return;
		}
		if (Kind == EUnrealAIModelEventKind::Failed)
		{
			QueueStartedLocked();
		}
		FUnrealAIModelEvent Event;
		PopulateCorrelationLocked(Event);
		Event.Kind = Kind;
		Event.SequenceNumber = ++LastModelSequence;
		Event.Error = MoveTemp(Error);
		PendingModelEvents.Add(MoveTemp(Event));
		bCancelPhysicalAfterTerminal |= bCancelPhysical;
	}

	void RequestPhysicalCancellation()
	{
		TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> HandleToCancel;
		{
			FScopeLock Lock(&HandleMutex);
			if (bPhysicalCancellationRequested)
			{
				return;
			}
			bPhysicalCancellationRequested = true;
			HandleToCancel = HttpHandle;
		}
		if (HandleToCancel.IsValid())
		{
			HandleToCancel->Cancel();
			NotifyLogicalTerminal();
		}
	}

	void NotifyLogicalTerminal()
	{
		if (!bLogicalTerminalNotified.exchange(true, std::memory_order_acq_rel) && OnLogicalTerminal)
		{
			if (bPhysicalSettledNotified.load(std::memory_order_acquire))
			{
				PhysicalPermit->Release();
			}
			OnLogicalTerminal(RequestId, this);
		}
	}

	void NotifyPhysicalSettled()
	{
		if (!bPhysicalSettledNotified.exchange(true, std::memory_order_acq_rel) && OnPhysicalSettled)
		{
			if (bLogicalTerminalNotified.load(std::memory_order_acquire))
			{
				PhysicalPermit->Release();
			}
			ModelSink->OnPhysicalSettled();
			OnPhysicalSettled(this);
		}
	}

	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIDeadline Deadline;
	FUnrealAICancellationToken ParentCancellation;
	FUnrealAIRequestId RequestId;
	FName UnauthorizedErrorCode;
	TFunction<void(FUnrealAIModelError &)> MapPublicError;
	int32 MaximumPendingHttpEvents = MaxPendingHttpEventsPerRequest;
	FName ForbiddenErrorCode;
	FName ErrorPrefix;
	EUnrealAIErrorCategory ForbiddenErrorCategory = EUnrealAIErrorCategory::NotAuthorized;
	TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> ModelSink;
	TFunction<void(const FUnrealAIRequestId &, const FNativeSseRequestOperation *)> OnLogicalTerminal;
	TFunction<void(const FNativeSseRequestOperation *)> OnPhysicalSettled;
	TSharedRef<FUnrealAIPhysicalRequestPermit, ESPMode::ThreadSafe> PhysicalPermit;
	TUniquePtr<IUnrealAINativeSseDecoder> Decoder;
	FUnrealAIHttpResponseMetadata Response;
	TArray<FUnrealAIModelProviderRequestId> KnownProviderRequestIds;
	FUnrealAITerminalGuard Terminal;
	FCriticalSection EventMutex;
	TArray<FUnrealAIHttpEvent> PendingHttpEvents;
	TArray<FUnrealAIModelEvent> PendingModelEvents;
	uint64 LastHttpSequence = 0;
	int64 LastModelSequence = 0;
	int64 PendingHttpEventBytes = 0;
	TArray<uint8> RejectedResponseBody;
	bool bRejectedResponseBodyOverflow = false;
	bool bStartedForwarded = false;
	bool bResponseStarted = false;
	bool bRejectedHttpResponse = false;
	bool bDraining = false;
	bool bHttpAdmissionCommitted = false;
	bool bHttpAdmissionPublished = false;
	bool bHttpAdmissionAborted = false;
	bool bPhysicalCallbackClosed = false;
	bool bCancelPhysicalAfterTerminal = false;
	std::atomic<bool> bLogicalTerminalNotified{false};
	std::atomic<bool> bPhysicalSettledNotified{false};
	FCriticalSection HandleMutex;
	TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> HttpHandle;
	bool bPhysicalCancellationRequested = false;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	bool bDrainSynchronouslyForTesting = false;
	TFunction<void()> BeforeFirstDrainForTesting;
	TFunction<void()> AfterFirstQueuedDrainBodyForTesting;
	std::atomic<bool> bTestDrainObserved{false};
#endif
};
} // namespace

bool FUnrealAINativeSseModelProviderConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString ConnectionError;
	const FTCHARToUTF8 PathUtf8(*RelativePath);
	if (MaximumPendingHttpEvents < 1 || MaximumPendingHttpEvents > MaxPendingHttpEventsPerRequest ||
		ProviderName.IsNone() || ErrorCodePrefix.IsNone() || !Connection.ValidateShape(ConnectionError) ||
		Connection.CredentialDestination.ModelProviderName != ProviderName || RelativePath.IsEmpty() ||
		!RelativePath.StartsWith(
			TEXT("/")) ||
			RelativePath.StartsWith(
				TEXT("//")) ||
				RelativePath.Contains(
					TEXT("://")) ||
					RelativePath.Contains(
						TEXT("?")) ||
						RelativePath.Contains(
							TEXT("#")) ||
							RelativePath.Contains(
								TEXT("\\")) || PathUtf8.Length() > FUnrealAIHttpRequest::MaxRelativePathBytes ||
								CredentialPresentation == EUnrealAIHttpCredentialPresentation::Invalid ||

								FixedHeaders.Num() > MaxFixedHeaders || ModelProfiles.IsEmpty() ||
								ModelProfiles.Num() > MaxModelProfiles || !Protocol.IsValid() ||
								Protocol->GetProviderName() != ProviderName ||
								Protocol->GetMaximumRequestBodyBytes() < 1 ||
								Protocol->GetMaximumRequestBodyBytes() > FUnrealAIHttpRequest::MaxRequestBodyBytes ||
								Protocol->GetMaximumStreamBytes() < 1 ||
								Protocol->GetMaximumStreamBytes() > FUnrealAIHttpRequest::MaxResponseBodyBytesLimit ||
								MaximumActiveRequests < 1 || MaximumActiveRequests > MaxActiveRequestsLimit)
	{
		OutError = TEXT("Native SSE provider configuration is invalid or internally inconsistent.");
		return false;
	}
	TSet<FString> HeaderNames;
	for (const FUnrealAIHttpRequestHeader &Header : FixedHeaders)
	{
		FString HeaderError;
		const FString Lower = Header.Name.ToLower();
		if (!Header.ValidateShape(HeaderError) || HeaderNames.Contains(Lower) ||
			Lower == TEXT("content-type") || Lower == TEXT("accept") ||
														   Lower == TEXT("authorization") ||
																		 Lower == TEXT("x-api-key") ||
																					   Lower == TEXT("x-goog-api-key"))
		{
			OutError = TEXT("Native SSE provider contains an invalid, duplicate, or transport-owned header.");
			return false;
		}
		HeaderNames.Add(Lower);
	}
	const FUnrealAIModelProviderDescriptor Descriptor = MakeDescriptor(*this);
	FString DescriptorError;
	if (!Descriptor.ValidateShape(DescriptorError))
	{
		OutError = TEXT("Native SSE provider produced an invalid aggregate descriptor.");
		return false;
	}
	const EUnrealAIModelCapability Required = EUnrealAIModelCapability::Text | EUnrealAIModelCapability::StreamingText |
											  EUnrealAIModelCapability::UsageReporting |
											  EUnrealAIModelCapability::RequestCancellation;
	const EUnrealAIModelCapability Supported =
		Required | EUnrealAIModelCapability::FunctionTools | EUnrealAIModelCapability::ParallelFunctionTools |
		EUnrealAIModelCapability::StructuredOutput | EUnrealAIModelCapability::ImageInput;
	TSet<FString> ModelIds;
	for (const FUnrealAIModelProfileProjection &Profile : ModelProfiles)
	{
		FString ProjectionError;
		const uint32 UnsupportedBits = static_cast<uint32>(Profile.Capabilities) & ~static_cast<uint32>(Supported);
		if (ModelIds.Contains(Profile.ModelId) || UnsupportedBits != 0 ||
			!EnumHasAllFlags(Profile.Capabilities, Required) || !Profile.ValidateAgainst(Descriptor, ProjectionError))
		{
			OutError = TEXT("Native SSE provider contains a duplicate or invalid exact model profile.");
			return false;
		}
		ModelIds.Add(Profile.ModelId);
	}
	return true;
}

class FUnrealAINativeSseModelProvider::FState final
	: public TSharedFromThis<FUnrealAINativeSseModelProvider::FState, ESPMode::ThreadSafe>
{
  public:
	FState(TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		   TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
		   const FUnrealAINativeSseModelProviderConfig &InConfig)
		: Connections(MoveTemp(InConnections)), Transport(MoveTemp(InTransport)), Config(InConfig)
	{
		FString Error;
		bShutdown = !Config.ValidateShape(Error);
		if (!Config.Clock.IsValid())
		{
			Config.Clock = MakeShared<FUnrealAISystemClock, ESPMode::ThreadSafe>();
		}
		if (!Config.PhysicalBudget.IsValid())
		{
			Config.PhysicalBudget =
				MakeShared<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe>(Config.MaximumActiveRequests);
		}
	}

	const FUnrealAINativeSseModelProviderConfig &GetConfig() const
	{
		return Config;
	}

	bool Start(const FUnrealAIModelRequest &Request,
			   TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
			   TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink,
			   const FUnrealAICancellationToken &Cancellation,
			   TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIModelError &OutError)
	{
		OutHandle.Reset();
		OutError = {};
		const FUnrealAIDeadline Deadline = FUnrealAIDeadline::FromNow(*Config.Clock, Request.TimeoutSeconds);

		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || !Cancellation.IsValid() || !AccessContext->IsValid() ||
			!AccessContext->RequiresCredential())
		{
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::InvalidArgument,
				TEXT("request_invalid"),
					 TEXT("The model request or credential context is invalid."),
						  TEXT("Native SSE provider rejected request shape, cancellation, or access context."));
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeProviderError(Config.ErrorCodePrefix, EUnrealAIErrorCategory::Cancelled,
										 TEXT("request_cancelled"),
											  TEXT("The model request was cancelled."),
												   TEXT("Native SSE provider request cancelled before admission."));
			return false;
		}
		const FUnrealAIModelProfileProjection *Projection = FindProjection(Config, Request.ModelId);
		if (Projection == nullptr || !RequestFitsProjection(Request, *Projection))
		{
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::UnsupportedCapability,
				TEXT("model_unsupported"),
					 TEXT("The selected model does not support this request."),
						  TEXT("Native SSE provider rejected model allowlist, capabilities, or limits."));
			return false;
		}
		const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection =
			Connections->Find(Request.ConnectionAlias);
		if (!Connection.IsValid() || !MatchesConnection(*Connection, Config.Connection))
		{
			OutError =
				MakeProviderError(Config.ErrorCodePrefix, EUnrealAIErrorCategory::InvalidConfiguration,
								  TEXT("connection_invalid"),
									   TEXT("The model connection is unavailable or invalid."),
											TEXT("Native SSE provider requires its exact frozen API-key destination."));
			return false;
		}

		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || PendingRequestIds.Num() + AllOperations.Num() >= Config.MaximumActiveRequests ||
				PendingRequestIds.Contains(Request.RequestId.Value) ||
				LogicalOperations.Contains(Request.RequestId.Value))
			{
				OutError = MakeProviderError(
					Config.ErrorCodePrefix, bShutdown ? EUnrealAIErrorCategory::Provider : EUnrealAIErrorCategory::Busy,
					bShutdown
					? TEXT("provider_shutdown")
					: TEXT("provider_capacity"), bShutdown
						   ? TEXT("The model provider is shutting down.")
						   : TEXT("The model provider is at capacity or the request is already active."),
								  TEXT("Native SSE provider rejected lifecycle or capacity admission."), !bShutdown);
				return false;
			}
			PendingRequestIds.Add(Request.RequestId.Value);
		}

		TSharedPtr<FUnrealAIPhysicalRequestPermit, ESPMode::ThreadSafe> PhysicalPermit =
			Config.PhysicalBudget->TryAcquire(Request.RequestId);
		if (!PhysicalPermit.IsValid())
		{
			ReleasePending(Request.RequestId);
			OutError =
				MakeProviderError(Config.ErrorCodePrefix, EUnrealAIErrorCategory::Busy,
								  TEXT("provider_capacity"),
									   TEXT("The model provider is at physical request capacity."),
											TEXT("Physical admission remains occupied until callbacks settle."), true);
			return false;
		}
		FUnrealAIModelRequest BoundRequest = Request;
		BoundRequest.ConnectionBinding = MakeUnrealAIConnectionBinding(Connection->CredentialDestination);
		TArray<uint8> Body;
		TUniquePtr<IUnrealAINativeSseContinuationCommit> ContinuationCommit;
		if (!Config.Protocol->StageRequest(BoundRequest, Connection->CredentialDestination, Body, ContinuationCommit,
										   OutError))
		{
			ReleasePending(Request.RequestId);
			return false;
		}
		if (Body.IsEmpty() || Body.Num() > Config.Protocol->GetMaximumRequestBodyBytes())
		{
			ReleasePending(Request.RequestId);
			OutError = MakeProviderError(Config.ErrorCodePrefix, EUnrealAIErrorCategory::ProviderProtocol,
										 TEXT("wire_request_invalid"),
											  TEXT("The model request could not be created."),
												   TEXT("Native SSE strategy returned an empty or oversized body."));
			return false;
		}

		if (Deadline.IsExpired(*Config.Clock))
		{
			ReleasePending(Request.RequestId);
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::Timeout,
				TEXT("timeout"), TEXT("The model request exceeded its deadline."),
									  TEXT("Model preparation exhausted the absolute request deadline."), true);
			return false;
		}
#if UE_BUILD_SHIPPING && !UE_SERVER
		if (Connection->CredentialDestination.AuthScheme == EUnrealAIAuthScheme::ApiKey ||
			Connection->CredentialDestination.AuthScheme == EUnrealAIAuthScheme::OAuthBearer)
		{
			ReleasePending(Request.RequestId);
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::NotAuthorized,
				TEXT("client_provider_secret_denied"),
					 TEXT("A shipped client must use its trusted backend."),
						  TEXT("Direct provider credentials cannot be admitted by a Shipping client."));
			return false;
		}
#endif
		const TWeakPtr<FState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		auto OnLogicalTerminal =
			[WeakSelf](const FUnrealAIRequestId &CompletedRequest, const FNativeSseRequestOperation *ExpectedOperation)
		{
			if (const TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
			{
				Pinned->ReleaseLogical(CompletedRequest, ExpectedOperation);
			}
		};
		auto OnPhysicalSettled = [WeakSelf](const FNativeSseRequestOperation *ExpectedOperation)
		{
			if (const TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
			{
				Pinned->ReleasePhysical(ExpectedOperation);
			}
		};
		const bool bDrainSynchronouslyForTesting =
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
			Config.bDrainEventsSynchronouslyForTesting;
#else
			false;
#endif
		TSharedPtr<FNativeSseRequestOperation, ESPMode::ThreadSafe> Operation =
			MakeShared<FNativeSseRequestOperation, ESPMode::ThreadSafe>(
				BoundRequest, Config.ErrorCodePrefix, Config.Protocol.ToSharedRef(), Config.ForbiddenErrorCategory,
				Sink, MoveTemp(OnLogicalTerminal), MoveTemp(OnPhysicalSettled), bDrainSynchronouslyForTesting,
				PhysicalPermit.ToSharedRef(), Config, Deadline, Cancellation);
		if (!Operation.IsValid() || !Operation->IsReady())
		{
			ReleasePending(Request.RequestId);
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::Busy,
				TEXT("operation_capacity"),
					 TEXT("The model provider is temporarily at callback capacity."),
						  TEXT("Native SSE provider could not create its managed decoder operation."), true);
			return false;
		}
		bool bRegistered = false;
		{
			FScopeLock Lock(&Mutex);
			if (!bShutdown && PendingRequestIds.Contains(Request.RequestId.Value))
			{
				PendingRequestIds.Remove(Request.RequestId.Value);
				LogicalOperations.Add(Request.RequestId.Value, Operation.Get());
				FOperationOwnership Ownership;
				Ownership.Operation = Operation;
				AllOperations.Add(Operation.Get(), MoveTemp(Ownership));
				bRegistered = true;
			}
			else
			{
				PendingRequestIds.Remove(Request.RequestId.Value);
			}
		}
		if (!bRegistered)
		{
			OutError =
				MakeProviderError(Config.ErrorCodePrefix, EUnrealAIErrorCategory::Provider,
								  TEXT("provider_shutdown"),
									   TEXT("The model provider is shutting down."),
											TEXT("Native SSE provider stopped admission before physical dispatch."));
			return false;
		}

		FUnrealAIHttpRequest HttpRequest;
		HttpRequest.RequestId = Request.RequestId;
		HttpRequest.Destination = Connection->CredentialDestination;
		HttpRequest.Method = EUnrealAIHttpMethod::Post;
		HttpRequest.RelativePath = Config.RelativePath;
		HttpRequest.CredentialPresentation = Config.CredentialPresentation;
		HttpRequest.QueryProfile = Config.QueryProfile;
		HttpRequest.ProtectedSecondaryHeaderName = Config.ProtectedSecondaryHeaderName;
		HttpRequest.Headers.Add({TEXT("Content-Type"), TEXT("application/json")});
		HttpRequest.Headers.Add({TEXT("Accept"), TEXT("text/event-stream")});
		HttpRequest.Headers.Append(Config.FixedHeaders);
		HttpRequest.Body = MoveTemp(Body);
		HttpRequest.TimeoutSeconds = FMath::Max(0.001f, static_cast<float>(Deadline.RemainingSeconds(*Config.Clock)));
		HttpRequest.MaxResponseBodyBytes = Config.Protocol->GetMaximumStreamBytes();
		HttpRequest.MaxBodyChunkBytes = 64 * 1024;

		TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> HttpHandle;
		if (!Transport->StartRequest(HttpRequest, AccessContext, Operation.ToSharedRef(), Cancellation, HttpHandle,
									 ShapeError))
		{
			const bool bPhysicalHandleExists = HttpHandle.IsValid();
			Operation->AbortHttpAdmission(MoveTemp(HttpHandle));
			ReleaseLogical(Request.RequestId, Operation.Get());
			if (!bPhysicalHandleExists)
			{
				Operation->SettleRejectedAdmission();
				ReleasePhysical(Operation.Get());
			}
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::Transport,
				TEXT("transport_admission_failed"),
					 TEXT("The model request could not be started."),
						  TEXT("Native SSE credential-safe transport rejected request admission."), true);
			return false;
		}
		const bool bValidPhysicalHandle = HttpHandle.IsValid() && HttpHandle->GetRequestId() == Request.RequestId;
		if (!bValidPhysicalHandle || !Operation->CommitHttpAdmission(HttpHandle.ToSharedRef()))
		{
			const bool bPhysicalHandleExists = HttpHandle.IsValid();
			Operation->AbortHttpAdmission(MoveTemp(HttpHandle));
			ReleaseLogical(Request.RequestId, Operation.Get());
			if (!bPhysicalHandleExists)
			{
				Operation->SettleRejectedAdmission();
				ReleasePhysical(Operation.Get());
			}
			OutError = MakeProviderError(
				Config.ErrorCodePrefix, EUnrealAIErrorCategory::Internal,
				bValidPhysicalHandle
				? TEXT("transport_admission_invalid")
				: TEXT("transport_handle_invalid"),
					   TEXT("The model transport returned an invalid request handle."),
							TEXT("Native SSE provider could not commit its physical transport handle."));
			return false;
		}
		FString ContinuationError;
		if (ContinuationCommit.IsValid() && !ContinuationCommit->TryCommit(ContinuationError))
		{
			Operation->AbortHttpAdmission(MoveTemp(HttpHandle));
			ReleaseLogical(Request.RequestId, Operation.Get());
			OutError =
				MakeProviderError(Config.ErrorCodePrefix, EUnrealAIErrorCategory::ResourceConflict,
								  TEXT("continuation_stale"),
									   TEXT("The model continuation is stale or already used."),
											TEXT("Native SSE provider lost the staged continuation consume race."));
			return false;
		}
		Operation->PublishHttpAdmission();
		OutHandle = Operation;
		return true;
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FNativeSseRequestOperation, ESPMode::ThreadSafe>> Operations;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			bShutdown = true;
			for (const TPair<const FNativeSseRequestOperation *, FOperationOwnership> &Pair : AllOperations)
			{
				if (!Pair.Value.bPhysicalReleased)
				{
					Operations.Add(Pair.Value.Operation);
				}
			}
		}
		for (const TSharedPtr<FNativeSseRequestOperation, ESPMode::ThreadSafe> &Operation : Operations)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
		Transport->BeginShutdown();
	}

  private:
	void ReleasePending(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		PendingRequestIds.Remove(RequestId.Value);
	}

	void ReleaseLogical(const FUnrealAIRequestId &RequestId, const FNativeSseRequestOperation *Expected)
	{
		FScopeLock Lock(&Mutex);
		const FNativeSseRequestOperation *const *Current = LogicalOperations.Find(RequestId.Value);
		if (Current != nullptr && *Current == Expected)
		{
			LogicalOperations.Remove(RequestId.Value);
		}
		if (FOperationOwnership *Ownership = AllOperations.Find(Expected))
		{
			Ownership->bLogicalReleased = true;
			if (Ownership->bPhysicalReleased)
			{
				AllOperations.Remove(Expected);
			}
		}
	}

	void ReleasePhysical(const FNativeSseRequestOperation *Expected)
	{
		FScopeLock Lock(&Mutex);
		if (FOperationOwnership *Ownership = AllOperations.Find(Expected))
		{
			Ownership->bPhysicalReleased = true;
			if (Ownership->bLogicalReleased)
			{
				AllOperations.Remove(Expected);
			}
		}
	}

	struct FOperationOwnership final
	{
		TSharedPtr<FNativeSseRequestOperation, ESPMode::ThreadSafe> Operation;
		bool bLogicalReleased = false;
		bool bPhysicalReleased = false;
	};

	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> Transport;
	FUnrealAINativeSseModelProviderConfig Config;
	FCriticalSection Mutex;
	TSet<FGuid> PendingRequestIds;
	TMap<FGuid, const FNativeSseRequestOperation *> LogicalOperations;
	TMap<const FNativeSseRequestOperation *, FOperationOwnership> AllOperations;
	bool bShutdown = false;
};

FUnrealAINativeSseModelProvider::FUnrealAINativeSseModelProvider(
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
	const FUnrealAINativeSseModelProviderConfig &InConfig)
	: State(MakeShared<FState, ESPMode::ThreadSafe>(MoveTemp(InConnections), MoveTemp(InTransport), InConfig))
{
}

FUnrealAINativeSseModelProvider::~FUnrealAINativeSseModelProvider()
{
	BeginShutdown();
}

FName FUnrealAINativeSseModelProvider::GetProviderName() const
{
	return State->GetConfig().ProviderName;
}

FUnrealAIModelProviderDescriptor FUnrealAINativeSseModelProvider::Describe() const
{
	return MakeDescriptor(State->GetConfig());
}

EUnrealAIModelCapability FUnrealAINativeSseModelProvider::GetCapabilities(const FString &ModelId) const
{
	return GetModelProjection(ModelId).Capabilities;
}

FUnrealAIModelProfileProjection FUnrealAINativeSseModelProvider::GetModelProjection(const FString &ModelId) const
{
	if (const FUnrealAIModelProfileProjection *Projection = FindProjection(State->GetConfig(), ModelId))
	{
		return *Projection;
	}
	FUnrealAIModelProfileProjection Missing;
	Missing.ModelId = ModelId;
	return Missing;
}

bool FUnrealAINativeSseModelProvider::StartRequest(
	const FUnrealAIModelRequest &Request,
	TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
	TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIModelError &OutError)
{
	return State->Start(Request, MoveTemp(AccessContext), MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

void FUnrealAINativeSseModelProvider::BeginShutdown()
{
	State->BeginShutdown();
}
