// Copyright EngineWorks. All Rights Reserved.

#include "OpenAI/UnrealAIOpenAIDeviceOAuthDriver.h"

#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Base64.h"
#include "Misc/ScopeLock.h"
#include "Runtime/UnrealAIClock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
const FName OpenAIAuthProviderName(TEXT("openai.codex.oauth"));
constexpr int32 CompatibilityRevision = 2;
constexpr const TCHAR *AuthOriginText = TEXT("https://auth.openai.com");
constexpr const TCHAR *DeviceRequestPath = TEXT("/api/accounts/deviceauth/usercode");
constexpr const TCHAR *DevicePollPath = TEXT("/api/accounts/deviceauth/token");
constexpr const TCHAR *TokenPath = TEXT("/oauth/token");
constexpr const TCHAR *VerificationUri = TEXT("https://auth.openai.com/codex/device");
constexpr const TCHAR *RedirectUri = TEXT("https://auth.openai.com/deviceauth/callback");
// Reviewed provider-controlled Codex public-client registration. It is not an AutonomousAgents registration.
constexpr const TCHAR *CompatibilityClientId = TEXT("app_EMoamEEZ73f0CkXaXp7hrann");
constexpr double MaxSingleExchangeSeconds = 30.0;
constexpr double MaxAuthorizationSeconds = 15.0 * 60.0;
constexpr int32 MaxPollAttempts = 900;
constexpr int32 MaxJsonFieldBytes = 16 * 1024;
constexpr int64 MaxSupportedUnixTimestamp = 253402300799ll;

enum class EOpenAITokenResponseMode : uint8
{
	InitialExchange,
	Refresh
};

void SecureResetString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

void SecureResetBytes(TArray<uint8> &Value)
{
	volatile uint8 *Wipe = Value.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Value.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

bool IsBoundedUtf8(const FString &Value, const int32 MaxBytes, const bool bAllowEmpty = false)
{
	const FTCHARToUTF8 Utf8(*Value);
	return (bAllowEmpty || Utf8.Length() > 0) && Utf8.Length() <= MaxBytes;
}

bool IsStrictUtf8(const TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty() || (Bytes.Num() >= 3 && Bytes[0] == 0xef && Bytes[1] == 0xbb && Bytes[2] == 0xbf))
	{
		return false;
	}
	for (int32 Index = 0; Index < Bytes.Num();)
	{
		const uint8 First = Bytes[Index];
		if (First == 0)
		{
			return false;
		}
		if (First <= 0x7f)
		{
			++Index;
			continue;
		}
		if (First >= 0xc2 && First <= 0xdf)
		{
			if (Index + 1 >= Bytes.Num() || (Bytes[Index + 1] & 0xc0) != 0x80)
			{
				return false;
			}
			Index += 2;
			continue;
		}
		if (First >= 0xe0 && First <= 0xef)
		{
			if (Index + 2 >= Bytes.Num() || (Bytes[Index + 2] & 0xc0) != 0x80)
			{
				return false;
			}
			const uint8 Second = Bytes[Index + 1];
			const bool bValidSecond = (First == 0xe0 && Second >= 0xa0 && Second <= 0xbf) ||
									  (First >= 0xe1 && First <= 0xec && (Second & 0xc0) == 0x80) ||
									  (First == 0xed && Second >= 0x80 && Second <= 0x9f) ||
									  (First >= 0xee && First <= 0xef && (Second & 0xc0) == 0x80);
			if (!bValidSecond)
			{
				return false;
			}
			Index += 3;
			continue;
		}
		if (First >= 0xf0 && First <= 0xf4)
		{
			if (Index + 3 >= Bytes.Num() || (Bytes[Index + 2] & 0xc0) != 0x80 || (Bytes[Index + 3] & 0xc0) != 0x80)
			{
				return false;
			}
			const uint8 Second = Bytes[Index + 1];
			const bool bValidSecond = (First == 0xf0 && Second >= 0x90 && Second <= 0xbf) ||
									  (First >= 0xf1 && First <= 0xf3 && (Second & 0xc0) == 0x80) ||
									  (First == 0xf4 && Second >= 0x80 && Second <= 0x8f);
			if (!bValidSecond)
			{
				return false;
			}
			Index += 4;
			continue;
		}
		return false;
	}
	return true;
}

struct FJsonScope final
{
	bool bObject = false;
	TArray<FString> Keys;
};

bool HasOneRootObjectAndUniqueKeys(const FString &Json)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TArray<FJsonScope> Scopes;
	bool bSawRoot = false;
	bool bFinishedRoot = false;
	EJsonNotation Notation = EJsonNotation::Error;
	while (Reader->ReadNext(Notation))
	{
		if (bFinishedRoot)
		{
			return false;
		}
		if (Scopes.IsEmpty())
		{
			if (bSawRoot || Notation != EJsonNotation::ObjectStart)
			{
				return false;
			}
			bSawRoot = true;
		}
		else if (Scopes.Last().bObject && Notation != EJsonNotation::ObjectEnd)
		{
			const FString &Identifier = Reader->GetIdentifier();
			for (const FString &Existing : Scopes.Last().Keys)
			{
				if (Existing.Equals(Identifier, ESearchCase::IgnoreCase))
				{
					return false;
				}
			}
			Scopes.Last().Keys.Add(Identifier);
		}

		switch (Notation)
		{
		case EJsonNotation::ObjectStart:
		{
			FJsonScope Scope;
			Scope.bObject = true;
			Scopes.Add(MoveTemp(Scope));
			break;
		}
		case EJsonNotation::ArrayStart:
			Scopes.Add(FJsonScope());
			break;
		case EJsonNotation::ObjectEnd:
			if (Scopes.IsEmpty() || !Scopes.Last().bObject)
			{
				return false;
			}
			Scopes.Pop();
			bFinishedRoot = Scopes.IsEmpty();
			break;
		case EJsonNotation::ArrayEnd:
			if (Scopes.IsEmpty() || Scopes.Last().bObject)
			{
				return false;
			}
			Scopes.Pop();
			break;
		case EJsonNotation::Boolean:
		case EJsonNotation::String:
		case EJsonNotation::Number:
		case EJsonNotation::Null:
			break;
		default:
			return false;
		}
	}
	return bSawRoot && bFinishedRoot && Scopes.IsEmpty() && Reader->GetErrorMessage().IsEmpty();
}

bool ParseStrictJsonObject(const TConstArrayView<uint8> Bytes, TSharedPtr<FJsonObject> &OutObject)
{
	OutObject.Reset();
	if (Bytes.IsEmpty() || Bytes.Num() > FUnrealAISecretValue::MaxSecretBytes || !IsStrictUtf8(Bytes))
	{
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
	if (Converted.Length() <= 0)
	{
		return false;
	}
	FString Json(Converted.Length(), Converted.Get());
	if (!HasOneRootObjectAndUniqueKeys(Json))
	{
		SecureResetString(Json);
		return false;
	}
	bool bParsed = false;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		bParsed = FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid() &&
				  Reader->GetErrorMessage().IsEmpty();
	}
	SecureResetString(Json);
	return bParsed;
}

bool TryGetExactString(const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, FString &OutValue)
{
	OutValue.Reset();
	return Object.IsValid() && Object->HasTypedField<EJson::String>(Field) &&
		   Object->TryGetStringField(Field, OutValue);
}

bool TryReadOfficialInterval(const TSharedPtr<FJsonObject> &Object, int32 &OutValue)
{
	OutValue = 5;
	FString Text;
	if (!Object->HasField(TEXT("interval")))
	{
		return true;
	}
	if (!TryGetExactString(Object, TEXT("interval"), Text))
	{
		return false;
	}
	Text.TrimStartAndEndInline();
	if (Text.IsEmpty())
	{
		return false;
	}
	uint64 Parsed = 0;
	for (const TCHAR Character : Text)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			SecureResetString(Text);
			return false;
		}
		const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
		if (Parsed > (MAX_uint64 - Digit) / 10)
		{
			SecureResetString(Text);
			return false;
		}
		Parsed = Parsed * 10 + Digit;
	}
	SecureResetString(Text);
	OutValue = static_cast<int32>(FMath::Clamp<uint64>(Parsed, 1, 30));
	return true;
}

FUnrealAIProviderAccessError MakeAuthError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false,
										   const float RetryAfterSeconds = 0.0f)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	Error.RetryAfterSeconds =
		bRetryable ? FMath::Clamp(RetryAfterSeconds, 0.0f, FUnrealAIProviderAccessError::MaxRetryAfterSeconds) : 0.0f;
	return Error;
}

FUnrealAIProviderAccessError MakeCancellationError(const FUnrealAICancellationToken &Cancellation,
												   const bool bCredentialOperation)
{
	if (Cancellation.GetReason() == EUnrealAICancellationReason::Timeout)
	{
		return MakeAuthError(EUnrealAIErrorCategory::Timeout,
							 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialTimedOut
												  : EUnrealAIProviderAccessErrorCode::AuthTimedOut,
							 true);
	}
	return MakeAuthError(EUnrealAIErrorCategory::Cancelled, bCredentialOperation
																? EUnrealAIProviderAccessErrorCode::CredentialCancelled
																: EUnrealAIProviderAccessErrorCode::AuthCancelled);
}

bool TrySecretFromUtf8(FString &Value, const int32 MaxBytes, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	OutSecret.Reset();
	const FTCHARToUTF8 Utf8(*Value);
	if (Utf8.Length() <= 0 || Utf8.Length() > MaxBytes)
	{
		SecureResetString(Value);
		OutError = TEXT("OAuth response contained missing or oversized sensitive material.");
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length());
	SecureResetString(Value);
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

bool AppendAscii(TArray<uint8> &Out, const ANSICHAR *Text)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	if (Length < 0 || Out.Num() > FUnrealAISecretValue::MaxSecretBytes - Length)
	{
		return false;
	}
	Out.Append(reinterpret_cast<const uint8 *>(Text), Length);
	return true;
}

bool IsFormUnreserved(const uint8 Byte)
{
	return (Byte >= 'a' && Byte <= 'z') || (Byte >= 'A' && Byte <= 'Z') || (Byte >= '0' && Byte <= '9') ||
		   Byte == '-' || Byte == '.' || Byte == '_' || Byte == '~';
}

bool AppendFormEncoded(TArray<uint8> &Out, const FStringView Value)
{
	static constexpr ANSICHAR Hex[] = "0123456789ABCDEF";
	const FString Owned(Value);
	const FTCHARToUTF8 Utf8(*Owned);
	if (Utf8.Length() < 0 ||
		static_cast<int64>(Out.Num()) + static_cast<int64>(Utf8.Length()) * 3 > FUnrealAISecretValue::MaxSecretBytes)
	{
		return false;
	}
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		if (IsFormUnreserved(Byte))
		{
			Out.Add(Byte);
		}
		else
		{
			Out.Add('%');
			Out.Add(Hex[(Byte >> 4) & 0x0f]);
			Out.Add(Hex[Byte & 0x0f]);
		}
	}
	return true;
}

bool TryMakeFormBody(const TArray<TPair<FString, FString>> &Fields, FUnrealAISecretValue &OutBody, FString &OutError)
{
	OutBody.Reset();
	TArray<uint8> Bytes;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		if ((Index > 0 && !AppendAscii(Bytes, "&")) || !AppendFormEncoded(Bytes, Fields[Index].Key) ||
			!AppendAscii(Bytes, "=") || !AppendFormEncoded(Bytes, Fields[Index].Value))
		{
			SecureResetBytes(Bytes);
			OutError = TEXT("OAuth form body exceeded its compiled bound.");
			return false;
		}
	}
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutBody, OutError);
}

bool TryMakeClientJsonBody(FUnrealAISecretValue &OutBody, FString &OutError)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("client_id"), CompatibilityClientId);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Object, Writer) || !Writer->Close())
	{
		SecureResetString(Json);
		OutError = TEXT("OAuth device request serialization failed.");
		return false;
	}
	return TrySecretFromUtf8(Json, FUnrealAISecretValue::MaxSecretBytes, OutBody, OutError);
}

bool TryMakePollJsonBody(FString &DeviceAuthId, FString &UserCode, FUnrealAISecretValue &OutBody, FString &OutError)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("device_auth_id"), DeviceAuthId);
	Object->SetStringField(TEXT("user_code"), UserCode);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	const bool bSerialized = FJsonSerializer::Serialize(Object, Writer) && Writer->Close();
	if (!bSerialized)
	{
		SecureResetString(Json);
		OutError = TEXT("OAuth device poll serialization failed.");
		return false;
	}
	return TrySecretFromUtf8(Json, FUnrealAISecretValue::MaxSecretBytes, OutBody, OutError);
}

class FBlockingOAuthSink final : public IUnrealAIOAuthHttpCompletionSink
{
  public:
	FBlockingOAuthSink() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
	~FBlockingOAuthSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void CompleteOAuthHttpRequest(FUnrealAIOAuthHttpResult &&Result) override
	{
		{
			FScopeLock Lock(&Mutex);
			if (bCompleted)
			{
				return;
			}
			bCompleted = true;
			StoredResult = MakeUnique<FUnrealAIOAuthHttpResult>(MoveTemp(Result));
		}
		Event->Trigger();
	}

	FEvent &GetCompletionEvent() const
	{
		return *Event;
	}

	bool Take(FUnrealAIOAuthHttpResult &OutResult)
	{
		FScopeLock Lock(&Mutex);
		if (!StoredResult.IsValid())
		{
			return false;
		}
		OutResult = MoveTemp(*StoredResult);
		StoredResult.Reset();
		return true;
	}

  private:
	mutable FCriticalSection Mutex;
	FEvent *Event = nullptr;
	TUniquePtr<FUnrealAIOAuthHttpResult> StoredResult;
	bool bCompleted = false;
};

class FDefaultOpenAIOAuthWaiter final : public IUnrealAIOpenAIOAuthWaiter
{
  public:
	bool WaitForExchange(FEvent &CompletionEvent, const double MaxWaitSeconds) override
	{
		const uint32 Milliseconds = static_cast<uint32>(FMath::Clamp(FMath::CeilToInt(MaxWaitSeconds * 1000.0), 1, 50));
		return CompletionEvent.Wait(Milliseconds);
	}

	bool WaitForPoll(const double Seconds, const double OverallDeadlineSeconds, const IUnrealAIClock &Clock,
					 const FUnrealAICancellationToken &Cancellation) override
	{
		const double Now = Clock.MonotonicSeconds();
		if (!FMath::IsFinite(Now) || Now < 0.0)
		{
			return false;
		}
		const double Until = FMath::Min(Now + Seconds, OverallDeadlineSeconds);
		while (Clock.MonotonicSeconds() < Until)
		{
			if (Cancellation.IsCancellationRequested())
			{
				return false;
			}
			const double Remaining = Until - Clock.MonotonicSeconds();
			FPlatformProcess::SleepNoStats(static_cast<float>(FMath::Clamp(Remaining, 0.001, 0.05)));
		}
		return !Cancellation.IsCancellationRequested() && Clock.MonotonicSeconds() < OverallDeadlineSeconds;
	}
};

TSharedRef<IUnrealAIOpenAIOAuthWaiter, ESPMode::ThreadSafe>
ResolveWaiter(TSharedPtr<IUnrealAIOpenAIOAuthWaiter, ESPMode::ThreadSafe> Waiter)
{
	if (Waiter.IsValid())
	{
		return Waiter.ToSharedRef();
	}
	return MakeShared<FDefaultOpenAIOAuthWaiter, ESPMode::ThreadSafe>();
}

bool PerformOAuthExchange(IUnrealAIOAuthHttpTransport &Transport, const FUnrealAIEndpointOrigin &Origin,
						  const FString &Path, const EUnrealAIOAuthHttpContentType ContentType,
						  FUnrealAISecretValue &&Body, const double TimeoutSeconds,
						  const FUnrealAICancellationToken &Cancellation, const IUnrealAIClock &Clock,
						  IUnrealAIOpenAIOAuthWaiter &Waiter, const bool bCredentialOperation,
						  FUnrealAIOAuthHttpResult &OutResult, FUnrealAIProviderAccessError &OutError)
{
	OutResult = FUnrealAIOAuthHttpResult();
	OutError = {};
	if (!Cancellation.IsValid())
	{
		Body.Reset();
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
													  : EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}
	if (Cancellation.IsCancellationRequested())
	{
		Body.Reset();
		OutError = MakeCancellationError(Cancellation, bCredentialOperation);
		return false;
	}
	if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0)
	{
		Body.Reset();
		OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialTimedOut
													  : EUnrealAIProviderAccessErrorCode::AuthTimedOut,
								 true);
		return false;
	}

	const FUnrealAIRequestId RequestId{FGuid::NewGuid()};
	FUnrealAIOAuthHttpRequest Request;
	FString RequestError;
	const double ExchangeTimeout = FMath::Min(TimeoutSeconds, MaxSingleExchangeSeconds);
	if (!FUnrealAIOAuthHttpRequest::TryCreate(RequestId, Origin, Path, ContentType, MoveTemp(Body), ExchangeTimeout,
											  FUnrealAIOAuthHttpRequest::MaxResponseBodyBytesLimit, Request,
											  RequestError))
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
													  : EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}

	const TSharedRef<FBlockingOAuthSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FBlockingOAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> Handle;
	if (!Transport.StartRequest(MoveTemp(Request), Sink, Cancellation, Handle, RequestError) || !Handle.IsValid())
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
													  : EUnrealAIProviderAccessErrorCode::AuthFailed,
								 true);
		return false;
	}

	bool bCancelSent = false;
	const auto CancelOnce = [&Handle, &bCancelSent]()
	{
		if (!bCancelSent)
		{
			Handle->Cancel();
			bCancelSent = true;
		}
	};
	if (Handle->GetRequestId() != RequestId)
	{
		CancelOnce();
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
													  : EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}

	const FUnrealAIDeadline Deadline = FUnrealAIDeadline::FromNow(Clock, ExchangeTimeout);
	for (;;)
	{
		const double Remaining = Deadline.RemainingSeconds(Clock);
		if (Remaining <= 0.0)
		{
			CancelOnce();
			OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
									 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialTimedOut
														  : EUnrealAIProviderAccessErrorCode::AuthTimedOut,
									 true);
			return false;
		}
		if (Waiter.WaitForExchange(Sink->GetCompletionEvent(), FMath::Min(Remaining, 0.01)))
		{
			break;
		}
		if (Cancellation.IsCancellationRequested())
		{
			CancelOnce();
			OutError = MakeCancellationError(Cancellation, bCredentialOperation);
			return false;
		}
		if (Deadline.IsExpired(Clock))
		{
			CancelOnce();
			OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
									 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialTimedOut
														  : EUnrealAIProviderAccessErrorCode::AuthTimedOut,
									 true);
			return false;
		}
	}

	if (!Sink->Take(OutResult))
	{
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
													  : EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}
	FString ShapeError;
	if (OutResult.GetRequestId() != RequestId || !OutResult.ValidateShape(ShapeError))
	{
		CancelOnce();
		OutResult = FUnrealAIOAuthHttpResult();
		OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
								 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
													  : EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}
	if (!OutResult.IsTransportSuccess())
	{
		if (OutResult.GetTerminalKind() == EUnrealAIOAuthHttpTerminalKind::Cancelled)
		{
			OutError = MakeCancellationError(Cancellation, bCredentialOperation);
		}
		else if (OutResult.GetTerminalKind() == EUnrealAIOAuthHttpTerminalKind::TimedOut)
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
									 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialTimedOut
														  : EUnrealAIProviderAccessErrorCode::AuthTimedOut,
									 true);
		}
		else
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
									 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
														  : EUnrealAIProviderAccessErrorCode::AuthFailed,
									 OutResult.GetError().bRetryable);
		}
		return false;
	}
	return true;
}

class FOpenAIDeviceResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FString DeviceAuthId;
	FString UserCode;
	int32 IntervalSeconds = 5;

	~FOpenAIDeviceResponseConsumer() override
	{
		SecureResetString(DeviceAuthId);
		SecureResetString(UserCode);
	}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		FString UserCodeAlias;
		if (!ParseStrictJsonObject(Secret, Object) || !TryGetExactString(Object, TEXT("device_auth_id"), DeviceAuthId))
		{
			return false;
		}
		const bool bHasUserCode = TryGetExactString(Object, TEXT("user_code"), UserCode);
		const bool bHasUserCodeAlias = TryGetExactString(Object, TEXT("usercode"), UserCodeAlias);
		if (bHasUserCode == bHasUserCodeAlias || !TryReadOfficialInterval(Object, IntervalSeconds))
		{
			SecureResetString(UserCodeAlias);
			return false;
		}
		if (bHasUserCodeAlias)
		{
			UserCode = MoveTemp(UserCodeAlias);
		}
		SecureResetString(UserCodeAlias);
		return IsBoundedUtf8(DeviceAuthId, MaxJsonFieldBytes) &&
			   IsBoundedUtf8(UserCode, FUnrealAIAuthInteraction::MaxUserCodeUtf8Bytes);
	}
};

class FOpenAIPollResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FString AuthorizationCode;
	FString CodeChallenge;
	FString CodeVerifier;

	~FOpenAIPollResponseConsumer() override
	{
		SecureResetString(AuthorizationCode);
		SecureResetString(CodeChallenge);
		SecureResetString(CodeVerifier);
	}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		return ParseStrictJsonObject(Secret, Object) &&
			   TryGetExactString(Object, TEXT("authorization_code"), AuthorizationCode) &&
								 TryGetExactString(Object, TEXT("code_challenge"), CodeChallenge) &&
												   TryGetExactString(Object, TEXT("code_verifier"), CodeVerifier) &&
																	 IsBoundedUtf8(AuthorizationCode,
																				   MaxJsonFieldBytes) &&
																	 IsBoundedUtf8(CodeChallenge, MaxJsonFieldBytes) &&
																	 IsBoundedUtf8(CodeVerifier, MaxJsonFieldBytes);
	}
};

bool IsStrictUnpaddedBase64Url(const FString &Value)
{
	if (Value.IsEmpty() || Value.Len() > (FUnrealAISecretValue::MaxSecretBytes * 4 / 3 + 4) || Value.Len() % 4 == 1)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!((Character >= TEXT('a') && Character <= TEXT('z')) ||
			   (Character >= TEXT('A') && Character <= TEXT('Z')) ||
				(Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('-') || Character == TEXT('_')))
		{
			return false;
		}
	}
	return true;
}

bool IsValidRoutingValue(const FString &Value)
{
	if (!IsBoundedUtf8(Value, FUnrealAIOAuthTokenSet::MaxAccountRoutingBytes))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (Character < 0x20 || Character == 0x7f)
		{
			return false;
		}
	}
	return true;
}

bool TryExtractOpenAIJwtRoutingAndExpiry(const FString &AccessToken, const FDateTime &NowUtc, FString &OutRouting,
										 FDateTime &OutExpiry)
{
	OutRouting.Reset();
	OutExpiry = FDateTime();
	if (!IsBoundedUtf8(AccessToken, FUnrealAIOAuthTokenSet::MaxAccessTokenBytes))
	{
		return false;
	}
	TArray<FString> Parts;
	AccessToken.ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() != 3 || Parts[0].IsEmpty() || !IsStrictUnpaddedBase64Url(Parts[1]) || Parts[2].IsEmpty())
	{
		for (FString &Part : Parts)
		{
			SecureResetString(Part);
		}
		return false;
	}

	FString PayloadSegment = Parts[1];
	FString Payload = PayloadSegment.Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));
	while (Payload.Len() % 4 != 0)
	{
		Payload.AppendChar(TEXT('='));
	}
	TArray<uint8> Decoded;
	const bool bDecoded = FBase64::Decode(Payload, Decoded);
	SecureResetString(Payload);
	FString CanonicalPayload;
	if (bDecoded)
	{
		CanonicalPayload = FBase64::Encode(Decoded, EBase64Mode::UrlSafe);
		CanonicalPayload.ReplaceInline(TEXT("="), TEXT(""));
	}
	const bool bCanonical = bDecoded && CanonicalPayload == PayloadSegment;
	SecureResetString(CanonicalPayload);
	SecureResetString(PayloadSegment);
	for (FString &Part : Parts)
	{
		SecureResetString(Part);
	}
	if (!bCanonical || Decoded.IsEmpty() || Decoded.Num() > FUnrealAISecretValue::MaxSecretBytes)
	{
		SecureResetBytes(Decoded);
		return false;
	}

	TSharedPtr<FJsonObject> Claims;
	const bool bParsed = ParseStrictJsonObject(Decoded, Claims);
	SecureResetBytes(Decoded);
	if (!bParsed)
	{
		return false;
	}
	double ExpiryNumber = 0.0;
	const TSharedPtr<FJsonObject> *AuthClaims = nullptr;
	const int64 NowUnix = NowUtc.ToUnixTimestamp();
	if (!Claims->HasTypedField<EJson::Number>(
			TEXT("exp")) ||
			!Claims->TryGetNumberField(
				TEXT("exp"), ExpiryNumber) || !FMath::IsFinite(ExpiryNumber) ||
				FMath::FloorToDouble(ExpiryNumber) != ExpiryNumber || ExpiryNumber <= static_cast<double>(NowUnix) ||
				ExpiryNumber > static_cast<double>(MaxSupportedUnixTimestamp) ||
				!Claims->TryGetObjectField(TEXT("https://api.openai.com/auth"), AuthClaims) || AuthClaims == nullptr ||
										   !AuthClaims->IsValid() ||
										   !TryGetExactString(*AuthClaims, TEXT("chatgpt_account_id"), OutRouting) ||
															  !IsValidRoutingValue(OutRouting))
	{
		SecureResetString(OutRouting);
		return false;
	}
	OutExpiry = FDateTime::FromUnixTimestamp(static_cast<int64>(ExpiryNumber));
	return OutExpiry > NowUtc;
}

class FOpenAITokenResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FOpenAITokenResponseConsumer(const FDateTime &InNowUtc, const EOpenAITokenResponseMode InMode)
		: NowUtc(InNowUtc), Mode(InMode)
	{
	}

	FUnrealAIOAuthTokenSet Tokens;

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		FString AccessToken;
		FString RefreshToken;
		FString IdToken;
		FString Routing;
		FString Error;
		if (!ParseStrictJsonObject(Secret, Object) || !TryGetExactString(Object, TEXT("access_token"), AccessToken))
		{
			return false;
		}
		const bool bHasRefresh = TryGetExactString(Object, TEXT("refresh_token"), RefreshToken);
		const bool bHasId = TryGetExactString(Object, TEXT("id_token"), IdToken);
		const bool bRefreshHasWrongType = Object->HasField(TEXT("refresh_token")) && !bHasRefresh;
		const bool bIdHasWrongType = Object->HasField(TEXT("id_token")) && !bHasId;
		const bool bInitial = Mode == EOpenAITokenResponseMode::InitialExchange;
		FDateTime Expiry;
		if (bRefreshHasWrongType || bIdHasWrongType || (bHasRefresh && RefreshToken.IsEmpty()) ||
			(bHasId && IdToken.IsEmpty()) ||
			(bInitial && (!bHasRefresh || RefreshToken.IsEmpty() || !bHasId || IdToken.IsEmpty())) ||
			!TryExtractOpenAIJwtRoutingAndExpiry(AccessToken, NowUtc, Routing, Expiry) ||
			!TrySecretFromUtf8(AccessToken, FUnrealAIOAuthTokenSet::MaxAccessTokenBytes, Tokens.AccessToken, Error) ||
			(bHasRefresh && !RefreshToken.IsEmpty() &&
			 !TrySecretFromUtf8(RefreshToken, FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes, Tokens.RefreshToken,
								Error)) ||
			(bHasId && !IdToken.IsEmpty() &&
			 !TrySecretFromUtf8(IdToken, FUnrealAIOAuthTokenSet::MaxIdTokenBytes, Tokens.IdToken, Error)) ||
			!TrySecretFromUtf8(Routing, FUnrealAIOAuthTokenSet::MaxAccountRoutingBytes, Tokens.AccountRoutingValue,
							   Error))
		{
			SecureResetString(AccessToken);
			SecureResetString(RefreshToken);
			SecureResetString(IdToken);
			SecureResetString(Routing);
			Tokens.Reset();
			return false;
		}
		SecureResetString(RefreshToken);
		SecureResetString(IdToken);
		Tokens.AccessTokenExpiresAtUtc = Expiry;
		return true;
	}

  private:
	FDateTime NowUtc;
	EOpenAITokenResponseMode Mode = EOpenAITokenResponseMode::InitialExchange;
};
} // namespace

class FUnrealAIOpenAIDeviceOAuthDriver::FState final
{
  public:
	FState(TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> InTransport,
		   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		   TSharedPtr<IUnrealAIOpenAIOAuthWaiter, ESPMode::ThreadSafe> InWaiter)
		: Transport(MoveTemp(InTransport)), Clock(MoveTemp(InClock)), Waiter(ResolveWaiter(MoveTemp(InWaiter)))
	{
		FString OriginError;
		bValid = FUnrealAIEndpointOrigin::TryParse(AuthOriginText, false, AuthOrigin, OriginError);
	}

	bool Authorize(const double TimeoutSeconds, const FUnrealAICancellationToken &Cancellation,
				   IUnrealAIDeviceOAuthInteractionPublisher &InteractionPublisher, FUnrealAIOAuthTokenSet &OutTokens,
				   FUnrealAIProviderAccessError &OutError)
	{
		OutTokens.Reset();
		OutError = {};
		if (!bValid || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 || !Cancellation.IsValid())
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeCancellationError(Cancellation, false);
			return false;
		}

		const FUnrealAIDeadline OverallDeadline =
			FUnrealAIDeadline::FromNow(*Clock, FMath::Min(TimeoutSeconds, MaxAuthorizationSeconds));
		FUnrealAISecretValue DeviceBody;
		FString SensitiveError;
		if (!TryMakeClientJsonBody(DeviceBody, SensitiveError))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		FUnrealAIOAuthHttpResult DeviceResult;
		if (!PerformOAuthExchange(*Transport, AuthOrigin, DeviceRequestPath,
								  EUnrealAIOAuthHttpContentType::ApplicationJson, MoveTemp(DeviceBody),
								  FMath::Min(OverallDeadline.RemainingSeconds(*Clock), MaxSingleExchangeSeconds),
								  Cancellation, *Clock, *Waiter, false, DeviceResult, OutError) ||
			DeviceResult.GetResponseMetadata().StatusCode != 200)
		{
			if (!OutError.IsError())
			{
				const int32 Status = DeviceResult.GetResponseMetadata().StatusCode;
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
								  Status == 429 || Status >= 500, DeviceResult.GetResponseMetadata().RetryAfterSeconds);
			}
			return false;
		}

		FOpenAIDeviceResponseConsumer DeviceResponse;
		if (!DeviceResult.TryConsumeBody(DeviceResponse))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		FUnrealAIAuthInteraction Interaction;
		FString InteractionError;
		if (!FUnrealAIAuthInteraction::TryCreateDeviceCode(AuthOrigin, FString(VerificationUri),
														   FString(DeviceResponse.UserCode), Interaction,
														   InteractionError) ||
			!InteractionPublisher.PublishInteraction(MoveTemp(Interaction)))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}

		FOpenAIPollResponseConsumer PollResponse;
		bool bApproved = false;
		for (int32 PollAttempt = 0; PollAttempt < MaxPollAttempts; ++PollAttempt)
		{
			if (Cancellation.IsCancellationRequested())
			{
				OutError = MakeCancellationError(Cancellation, false);
				return false;
			}
			const double Remaining = OverallDeadline.RemainingSeconds(*Clock);
			if (Remaining <= 0.0)
			{
				OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
										 EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
				return false;
			}

			FUnrealAISecretValue PollBody;
			FString DeviceIdCopy = DeviceResponse.DeviceAuthId;
			FString UserCodeCopy = DeviceResponse.UserCode;
			const bool bPollBody = TryMakePollJsonBody(DeviceIdCopy, UserCodeCopy, PollBody, SensitiveError);
			SecureResetString(DeviceIdCopy);
			SecureResetString(UserCodeCopy);
			if (!bPollBody)
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
				return false;
			}

			FUnrealAIOAuthHttpResult PollResult;
			if (!PerformOAuthExchange(*Transport, AuthOrigin, DevicePollPath,
									  EUnrealAIOAuthHttpContentType::ApplicationJson, MoveTemp(PollBody),
									  FMath::Min(Remaining, MaxSingleExchangeSeconds), Cancellation, *Clock, *Waiter,
									  false, PollResult, OutError))
			{
				return false;
			}
			const int32 Status = PollResult.GetResponseMetadata().StatusCode;
			if (Status == 200)
			{
				if (!PollResult.TryConsumeBody(PollResponse))
				{
					OutError =
						MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
					return false;
				}
				bApproved = true;
				break;
			}
			if (Status != 403 && Status != 404)
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
								  Status == 429 || Status >= 500, PollResult.GetResponseMetadata().RetryAfterSeconds);
				return false;
			}
			if (!Waiter->WaitForPoll(DeviceResponse.IntervalSeconds, OverallDeadline.AtMonotonicSeconds, *Clock,
									 Cancellation))
			{
				OutError = Cancellation.IsCancellationRequested()
							   ? MakeCancellationError(Cancellation, false)
							   : MakeAuthError(EUnrealAIErrorCategory::Timeout,
											   EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
				return false;
			}
		}
		if (!bApproved)
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
			return false;
		}

		FUnrealAISecretValue ExchangeBody;
		TArray<TPair<FString, FString>> ExchangeFields = {{TEXT("grant_type"), TEXT("authorization_code")},
														   {TEXT("code"), PollResponse.AuthorizationCode},
															{TEXT("redirect_uri"), RedirectUri},
															 {TEXT("client_id"), CompatibilityClientId},
															  {TEXT("code_verifier"), PollResponse.CodeVerifier}};
		const bool bMadeExchangeBody = TryMakeFormBody(ExchangeFields, ExchangeBody, SensitiveError);
		for (TPair<FString, FString> &Field : ExchangeFields)
		{
			SecureResetString(Field.Value);
		}
		SecureResetString(PollResponse.AuthorizationCode);
		SecureResetString(PollResponse.CodeChallenge);
		SecureResetString(PollResponse.CodeVerifier);
		if (!bMadeExchangeBody)
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}

		const double Remaining = OverallDeadline.RemainingSeconds(*Clock);
		FUnrealAIOAuthHttpResult ExchangeResult;
		if (Remaining <= 0.0 ||
			!PerformOAuthExchange(*Transport, AuthOrigin, TokenPath, EUnrealAIOAuthHttpContentType::FormUrlEncoded,
								  MoveTemp(ExchangeBody), FMath::Min(Remaining, MaxSingleExchangeSeconds), Cancellation,
								  *Clock, *Waiter, false, ExchangeResult, OutError) ||
			ExchangeResult.GetResponseMetadata().StatusCode != 200)
		{
			if (!OutError.IsError())
			{
				const int32 Status = ExchangeResult.GetResponseMetadata().StatusCode;
				OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
										 Status == 429 || Status >= 500,
										 ExchangeResult.GetResponseMetadata().RetryAfterSeconds);
			}
			return false;
		}
		FOpenAITokenResponseConsumer TokenConsumer(Clock->UtcNow(), EOpenAITokenResponseMode::InitialExchange);
		if (!ExchangeResult.TryConsumeBody(TokenConsumer))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		OutTokens = MoveTemp(TokenConsumer.Tokens);
		return true;
	}

	bool Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, const double TimeoutSeconds,
				 const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError)
	{
		OutError = {};
		FUnrealAISecretValue RefreshBody;
		FString RefreshError;
		if (!bValid || !InOutEnvelope.TryMintRefreshRequestBody(CompatibilityClientId, RefreshBody, RefreshError))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
									 EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
			return false;
		}
		FUnrealAIOAuthHttpResult Result;
		if (!PerformOAuthExchange(*Transport, AuthOrigin, TokenPath, EUnrealAIOAuthHttpContentType::FormUrlEncoded,
								  MoveTemp(RefreshBody), FMath::Min(TimeoutSeconds, MaxSingleExchangeSeconds),
								  Cancellation, *Clock, *Waiter, true, Result, OutError))
		{
			return false;
		}
		const int32 Status = Result.GetResponseMetadata().StatusCode;
		if (Status != 200)
		{
			OutError = Status == 400 || Status == 401
						   ? MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
										   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady)
						   : MakeAuthError(
								 EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed,
								 Status == 429 || Status >= 500, Result.GetResponseMetadata().RetryAfterSeconds);
			return false;
		}
		FOpenAITokenResponseConsumer TokenConsumer(Clock->UtcNow(), EOpenAITokenResponseMode::Refresh);
		if (!Result.TryConsumeBody(TokenConsumer) ||
			!InOutEnvelope.TryApplyRefresh(MoveTemp(TokenConsumer.Tokens), Clock->UtcNow(), RefreshError))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
			return false;
		}
		return true;
	}

  private:
	TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> Transport;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<IUnrealAIOpenAIOAuthWaiter, ESPMode::ThreadSafe> Waiter;
	FUnrealAIEndpointOrigin AuthOrigin;
	bool bValid = false;
};

FUnrealAIOpenAIDeviceOAuthDriver::FUnrealAIOpenAIDeviceOAuthDriver(
	TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> InTransport,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	TSharedPtr<IUnrealAIOpenAIOAuthWaiter, ESPMode::ThreadSafe> InWaiter)
	: State(MakeShared<FState, ESPMode::ThreadSafe>(MoveTemp(InTransport), MoveTemp(InClock), MoveTemp(InWaiter)))
{
}

FUnrealAIOpenAIDeviceOAuthDriver::~FUnrealAIOpenAIDeviceOAuthDriver() = default;

FName FUnrealAIOpenAIDeviceOAuthDriver::GetProviderName() const
{
	return OpenAIAuthProviderName;
}

bool FUnrealAIOpenAIDeviceOAuthDriver::Authorize(const double TimeoutSeconds,
												 const FUnrealAICancellationToken &Cancellation,
												 IUnrealAIDeviceOAuthInteractionPublisher &InteractionPublisher,
												 FUnrealAIOAuthTokenSet &OutTokens,
												 FUnrealAIProviderAccessError &OutError)
{
	return State->Authorize(TimeoutSeconds, Cancellation, InteractionPublisher, OutTokens, OutError);
}

bool FUnrealAIOpenAIDeviceOAuthDriver::Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, const double TimeoutSeconds,
											   const FUnrealAICancellationToken &Cancellation,
											   FUnrealAIProviderAccessError &OutError)
{
	return State->Refresh(InOutEnvelope, TimeoutSeconds, Cancellation, OutError);
}

int32 FUnrealAIOpenAIDeviceOAuthDriver::GetCompatibilityRevision()
{
	return CompatibilityRevision;
}
