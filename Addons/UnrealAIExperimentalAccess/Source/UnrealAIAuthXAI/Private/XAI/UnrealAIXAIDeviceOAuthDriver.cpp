// Copyright UnrealOps. All Rights Reserved.

#include "XAI/UnrealAIXAIDeviceOAuthDriver.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"
#include "Misc/Base64.h"
#include "Misc/ScopeLock.h"
#include "Runtime/UnrealAIClock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
const FName XAIDriverAuthProviderName(TEXT("xai.grok.oauth"));

constexpr int32 CompatibilityRevision = 2;
constexpr const TCHAR *AuthOriginText = TEXT("https://auth.x.ai");
constexpr const TCHAR *DiscoveryPath = TEXT("/.well-known/openid-configuration");
constexpr const TCHAR *DeviceCodePath = TEXT("/oauth2/device/code");
constexpr const TCHAR *TokenPath = TEXT("/oauth2/token");
constexpr const TCHAR *ExactIssuer = TEXT("https://auth.x.ai");
constexpr const TCHAR *ExactAuthorizationEndpoint = TEXT("https://auth.x.ai/oauth2/authorize");
constexpr const TCHAR *ExactTokenEndpoint = TEXT("https://auth.x.ai/oauth2/token");
constexpr const TCHAR *ExactDeviceAuthorizationEndpoint = TEXT("https://auth.x.ai/oauth2/device/code");
constexpr const TCHAR *VerificationOriginText = TEXT("https://accounts.x.ai");
constexpr const TCHAR *ExactVerificationUri = TEXT("https://accounts.x.ai/oauth2/device");
// Reviewed provider-controlled Grok public-client registration.
constexpr const TCHAR *CompatibilityClientId = TEXT("b1a00492-073a-47ea-816f-4c329264a828");
constexpr const TCHAR *CompatibilityScope = TEXT("openid profile email offline_access grok-cli:access api:access");
constexpr const TCHAR *DeviceGrantType = TEXT("urn:ietf:params:oauth:grant-type:device_code");
constexpr double MaxSingleExchangeSeconds = 30.0;
constexpr double MaxDeviceLifetimeSeconds = 60.0 * 60.0;
constexpr double MaxAccessLifetimeSeconds = 7.0 * 24.0 * 60.0 * 60.0;
constexpr double MaxPollIntervalSeconds = 30.0;
constexpr int32 MaxJsonFieldBytes = 16 * 1024;
constexpr int32 MaxOAuthErrorCodeBytes = 256;

const TCHAR *const DiscoveryJsonFields[] = {
	TEXT("issuer"), TEXT("authorization_endpoint"), TEXT("token_endpoint"), TEXT("device_authorization_endpoint")};
const TCHAR *const DeviceJsonFields[] = {
	TEXT("device_code"), TEXT("user_code"), TEXT("verification_uri"), TEXT("verification_uri_complete"),
																		   TEXT("expires_in"),	TEXT("interval")};
const TCHAR *const OAuthErrorJsonFields[] = {TEXT("error"), TEXT("error_description"), TEXT("error_uri")};
const TCHAR *const TokenJsonFields
	[] = {TEXT("access_token"), TEXT("refresh_token"),	   TEXT("id_token"),
																TEXT("token_type"),	  TEXT("expires_in"),		 TEXT("scope"),
																													  TEXT("error"),		TEXT("error_description"), TEXT("error_uri")};

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
	FTCHARToUTF8 Utf8(*Value);
	if (Utf8.Length() <= 0 || Utf8.Length() > MaxBytes)
	{
		SecureResetString(Value);
		OutError = TEXT("xAI OAuth response contained missing or oversized sensitive material.");
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

bool AppendFormField(TArray<uint8> &Out, bool &bFirst, const ANSICHAR *Name, const FStringView Value)
{
	if ((!bFirst && !AppendAscii(Out, "&")) || !AppendAscii(Out, Name) || !AppendAscii(Out, "=") ||
		!AppendFormEncoded(Out, Value))
	{
		return false;
	}
	bFirst = false;
	return true;
}

bool TryFormEncodeToString(const FStringView Value, FString &OutEncoded)
{
	OutEncoded.Reset();
	TArray<uint8> Bytes;
	if (!AppendFormEncoded(Bytes, Value))
	{
		SecureResetBytes(Bytes);
		return false;
	}
	if (!Bytes.IsEmpty())
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
		if (Converted.Length() != Bytes.Num())
		{
			SecureResetBytes(Bytes);
			return false;
		}
		OutEncoded = FString(Converted.Length(), Converted.Get());
	}
	SecureResetBytes(Bytes);
	return true;
}

bool TryMakeDeviceCodeBody(FUnrealAISecretValue &OutBody, FString &OutError)
{
	TArray<uint8> Bytes;
	bool bFirst = true;
	if (!AppendFormField(Bytes, bFirst, "client_id", FStringView(CompatibilityClientId)) ||
		!AppendFormField(Bytes, bFirst, "scope", FStringView(CompatibilityScope)))
	{
		SecureResetBytes(Bytes);
		OutError = TEXT("xAI OAuth device form exceeded its compiled bound.");
		return false;
	}
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutBody, OutError);
}

bool TryMakePollBody(const FStringView DeviceCode, FUnrealAISecretValue &OutBody, FString &OutError)
{
	TArray<uint8> Bytes;
	bool bFirst = true;
	if (!AppendFormField(Bytes, bFirst, "grant_type", FStringView(DeviceGrantType)) ||
		!AppendFormField(Bytes, bFirst, "client_id", FStringView(CompatibilityClientId)) ||
		!AppendFormField(Bytes, bFirst, "device_code", DeviceCode))
	{
		SecureResetBytes(Bytes);
		OutError = TEXT("xAI OAuth polling form exceeded its compiled bound.");
		return false;
	}
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutBody, OutError);
}

struct FStrictJsonContainerState final
{
	bool bObject = false;
	TSet<FString> NormalizedKeys;
};

bool HasUnambiguousExactJsonKeys(const FString &Json, const TConstArrayView<const TCHAR *> ExactRootFields)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TArray<FStrictJsonContainerState> Containers;
	EJsonNotation Notation = EJsonNotation::Error;
	while (Reader->ReadNext(Notation))
	{
		if (Notation == EJsonNotation::Error)
		{
			return false;
		}
		if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
		{
			const bool bClosingObject = Notation == EJsonNotation::ObjectEnd;
			if (Containers.IsEmpty() || Containers.Last().bObject != bClosingObject)
			{
				return false;
			}
			Containers.Pop(EAllowShrinking::No);
			continue;
		}
		if (!Containers.IsEmpty() && Containers.Last().bObject)
		{
			const FString Identifier = Reader->GetIdentifier();
			FString Normalized = Identifier.ToLower();
			if (Containers.Last().NormalizedKeys.Contains(Normalized))
			{
				return false;
			}
			Containers.Last().NormalizedKeys.Add(MoveTemp(Normalized));
			if (Containers.Num() == 1)
			{
				for (const TCHAR *ExactField : ExactRootFields)
				{
					if (Identifier.Equals(ExactField, ESearchCase::IgnoreCase) &&
						!Identifier.Equals(ExactField, ESearchCase::CaseSensitive))
					{
						return false;
					}
				}
			}
		}
		if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
		{
			FStrictJsonContainerState State;
			State.bObject = Notation == EJsonNotation::ObjectStart;
			Containers.Add(MoveTemp(State));
		}
	}
	return Containers.IsEmpty();
}

bool ParseJsonObject(const TConstArrayView<uint8> Bytes, TSharedPtr<FJsonObject> &OutObject,
					 const TConstArrayView<const TCHAR *> ExactRootFields = {})
{
	OutObject.Reset();
	if (Bytes.IsEmpty() || Bytes.Num() > FUnrealAISecretValue::MaxSecretBytes)
	{
		return false;
	}
	FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
	if (Converted.Length() <= 0)
	{
		return false;
	}
	FString Json(Converted.Length(), Converted.Get());
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	const bool bParsed = HasUnambiguousExactJsonKeys(Json, ExactRootFields) &&
						 FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	SecureResetString(Json);
	return bParsed;
}

bool TryReadPositiveInteger(const TSharedPtr<FJsonObject> &Object, const TCHAR *Field, int32 &OutValue,
							const bool bRequired)
{
	OutValue = 0;
	double Number = 0.0;
	if (Object->TryGetNumberField(Field, Number))
	{
		if (!FMath::IsFinite(Number) || Number < 1.0 || Number > MAX_int32 || FMath::FloorToDouble(Number) != Number)
		{
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}
	FString Text;
	if (Object->TryGetStringField(Field, Text))
	{
		const int64 Parsed = FCString::Atoi64(*Text);
		const FString Canonical = FString::Printf(TEXT("%lld"), static_cast<long long>(Parsed));
		const bool bValid = Parsed >= 1 && Parsed <= MAX_int32 && Text == Canonical;
		SecureResetString(Text);
		if (!bValid)
		{
			return false;
		}
		OutValue = static_cast<int32>(Parsed);
		return true;
	}
	return !bRequired;
}

bool HasRequiredCompatibilityScopes(const FString &Scope)
{
	if (!IsBoundedUtf8(Scope, 1024) || Scope.Contains(TEXT("\t")) || Scope.Contains(TEXT("\r")) ||
																					Scope.Contains(TEXT("\n")))
	{
		return false;
	}
	TArray<FString> Values;
	Scope.ParseIntoArray(Values, TEXT(" "), true);
	TSet<FString> Actual;
	for (FString &Value : Values)
	{
		Actual.Add(Value);
		SecureResetString(Value);
	}
	return Actual.Contains(TEXT("grok-cli:access")) && Actual.Contains(TEXT("api:access"));
}

bool TryExtractJwtExpiry(const FString &AccessToken, const FDateTime &NowUtc, FDateTime &OutExpiry)
{
	OutExpiry = {};
	TArray<FString> Parts;
	AccessToken.ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() < 2 || Parts[1].IsEmpty())
	{
		for (FString &Part : Parts)
		{
			SecureResetString(Part);
		}
		return false;
	}
	FString Payload = Parts[1].Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));
	while (Payload.Len() % 4 != 0)
	{
		Payload.AppendChar(TEXT('='));
	}
	TArray<uint8> Decoded;
	const bool bDecoded = FBase64::Decode(Payload, Decoded);
	SecureResetString(Payload);
	for (FString &Part : Parts)
	{
		SecureResetString(Part);
	}
	if (!bDecoded || Decoded.IsEmpty() || Decoded.Num() > FUnrealAISecretValue::MaxSecretBytes)
	{
		SecureResetBytes(Decoded);
		return false;
	}
	TSharedPtr<FJsonObject> Claims;
	const bool bParsed = ParseJsonObject(Decoded, Claims);
	SecureResetBytes(Decoded);
	if (!bParsed)
	{
		return false;
	}
	double ExpiryNumber = 0.0;
	if (!Claims->TryGetNumberField(TEXT("exp"), ExpiryNumber) || !FMath::IsFinite(ExpiryNumber) || ExpiryNumber < 1.0 ||
								   ExpiryNumber > static_cast<double>(MAX_int64) ||
								   FMath::FloorToDouble(ExpiryNumber) != ExpiryNumber)
	{
		return false;
	}
	OutExpiry = FDateTime::FromUnixTimestamp(static_cast<int64>(ExpiryNumber));
	return OutExpiry > NowUtc && (OutExpiry - NowUtc).GetTotalSeconds() <= MaxAccessLifetimeSeconds;
}

bool TryResolveAccessExpiry(const TSharedPtr<FJsonObject> &Object, const FString &AccessToken, const FDateTime &NowUtc,
							FDateTime &OutExpiry)
{
	int32 ExpiresIn = 0;
	if (!TryReadPositiveInteger(Object, TEXT("expires_in"), ExpiresIn, false))
	{
		return false;
	}
	if (ExpiresIn > 0)
	{
		if (ExpiresIn > MaxAccessLifetimeSeconds)
		{
			return false;
		}
		const int64 NowUnixSeconds = NowUtc.ToUnixTimestamp();
		if (NowUnixSeconds > MAX_int64 - ExpiresIn)
		{
			return false;
		}
		OutExpiry = FDateTime::FromUnixTimestamp(NowUnixSeconds + ExpiresIn);
		return OutExpiry > NowUtc;
	}
	return TryExtractJwtExpiry(AccessToken, NowUtc, OutExpiry);
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
			if (StoredResult.IsValid())
			{
				return;
			}
			StoredResult = MakeUnique<FUnrealAIOAuthHttpResult>(MoveTemp(Result));
		}
		Event->Trigger();
	}

	bool Wait(const uint32 Milliseconds) const
	{
		return Event->Wait(Milliseconds);
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
};

bool PerformOAuthExchange(IUnrealAIOAuthHttpTransport &Transport, const FUnrealAIEndpointOrigin &Origin,
						  const FString &Path, const EUnrealAIOAuthHttpMethod Method,
						  const EUnrealAIOAuthHttpContentType ContentType, FUnrealAISecretValue &&Body,
						  const double TimeoutSeconds, const FUnrealAICancellationToken &Cancellation,
						  const bool bCredentialOperation, FUnrealAIOAuthHttpResult &OutResult,
						  FUnrealAIProviderAccessError &OutError)
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
	const bool bCreated = Method == EUnrealAIOAuthHttpMethod::Get
							  ? FUnrealAIOAuthHttpRequest::TryCreateGet(
									RequestId, Origin, Path, ExchangeTimeout,
									FUnrealAIOAuthHttpRequest::MaxResponseBodyBytesLimit, Request, RequestError)
							  : Method == EUnrealAIOAuthHttpMethod::Post &&
									FUnrealAIOAuthHttpRequest::TryCreate(
										RequestId, Origin, Path, ContentType, MoveTemp(Body), ExchangeTimeout,
										FUnrealAIOAuthHttpRequest::MaxResponseBodyBytesLimit, Request, RequestError);
	Body.Reset();
	if (!bCreated)
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
	const double PhysicalDeadline = FPlatformTime::Seconds() + ExchangeTimeout + 2.0;
	bool bCancelSent = false;
	while (!Sink->Wait(10))
	{
		if (Cancellation.IsCancellationRequested() && !bCancelSent)
		{
			Handle->Cancel();
			bCancelSent = true;
		}
		if (FPlatformTime::Seconds() >= PhysicalDeadline)
		{
			Handle->Cancel();
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

class FXAIDiscoveryResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		FString Issuer;
		FString AuthorizationEndpoint;
		FString TokenEndpoint;
		FString DeviceAuthorizationEndpoint;
		if (!ParseJsonObject(
				Secret, Object,
				TConstArrayView<const TCHAR *>(DiscoveryJsonFields, UE_ARRAY_COUNT(DiscoveryJsonFields))) ||
			!Object->TryGetStringField(
				TEXT("issuer"), Issuer) ||
				!Object->TryGetStringField(
					TEXT("authorization_endpoint"), AuthorizationEndpoint) ||
					!Object->TryGetStringField(TEXT("token_endpoint"), TokenEndpoint) ||
											   !Object->TryGetStringField(TEXT("device_authorization_endpoint"),
																			   DeviceAuthorizationEndpoint))
		{
			SecureResetString(Issuer);
			SecureResetString(AuthorizationEndpoint);
			SecureResetString(TokenEndpoint);
			SecureResetString(DeviceAuthorizationEndpoint);
			return false;
		}
		const bool bValid =
			Issuer.Equals(ExactIssuer, ESearchCase::CaseSensitive) &&
			AuthorizationEndpoint.Equals(ExactAuthorizationEndpoint, ESearchCase::CaseSensitive) &&
			TokenEndpoint.Equals(ExactTokenEndpoint, ESearchCase::CaseSensitive) &&
			DeviceAuthorizationEndpoint.Equals(ExactDeviceAuthorizationEndpoint, ESearchCase::CaseSensitive);
		SecureResetString(Issuer);
		SecureResetString(AuthorizationEndpoint);
		SecureResetString(TokenEndpoint);
		SecureResetString(DeviceAuthorizationEndpoint);
		return bValid;
	}
};

class FXAIDeviceResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FString DeviceCode;
	FString UserCode;
	int32 ExpiresInSeconds = 0;
	int32 IntervalSeconds = 0;

	~FXAIDeviceResponseConsumer() override
	{
		SecureResetString(DeviceCode);
		SecureResetString(UserCode);
	}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		FString VerificationUri;
		FString VerificationUriComplete;
		if (!ParseJsonObject(Secret, Object,
							 TConstArrayView<const TCHAR *>(DeviceJsonFields, UE_ARRAY_COUNT(DeviceJsonFields))) ||
			!Object->TryGetStringField(
				TEXT("device_code"), DeviceCode) ||
				!Object->TryGetStringField(
					TEXT("user_code"), UserCode) ||
					!Object->TryGetStringField(
						TEXT("verification_uri"), VerificationUri) ||
						!TryReadPositiveInteger(Object, TEXT("expires_in"), ExpiresInSeconds, true) ||
												!TryReadPositiveInteger(Object, TEXT("interval"), IntervalSeconds,
																					 true))
		{
			SecureResetString(VerificationUri);
			SecureResetString(VerificationUriComplete);
			return false;
		}
		const bool bHasCompleteField = Object->HasField(TEXT("verification_uri_complete"));
		const bool bHasCompleteUri =
			Object->TryGetStringField(TEXT("verification_uri_complete"), VerificationUriComplete);
		FString EncodedUserCode;
		const bool bEncoded = TryFormEncodeToString(UserCode, EncodedUserCode);
		FString ExpectedCompleteUri = FString(ExactVerificationUri) + TEXT("?user_code=") + EncodedUserCode;
		const bool bValid =
			IsBoundedUtf8(DeviceCode, MaxJsonFieldBytes) &&
			IsBoundedUtf8(UserCode, FUnrealAIAuthInteraction::MaxUserCodeUtf8Bytes) &&
			VerificationUri.Equals(ExactVerificationUri, ESearchCase::CaseSensitive) && bEncoded &&
			(!bHasCompleteField ||
			 (bHasCompleteUri && VerificationUriComplete.Equals(ExpectedCompleteUri, ESearchCase::CaseSensitive) &&
			  IsBoundedUtf8(VerificationUriComplete, FUnrealAIAuthInteraction::MaxVerificationUriUtf8Bytes))) &&
			ExpiresInSeconds <= MaxDeviceLifetimeSeconds && IntervalSeconds <= MaxPollIntervalSeconds;
		SecureResetString(VerificationUri);
		SecureResetString(VerificationUriComplete);
		SecureResetString(EncodedUserCode);
		SecureResetString(ExpectedCompleteUri);
		return bValid;
	}
};

class FXAIOAuthErrorResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FString ErrorCode;

	~FXAIOAuthErrorResponseConsumer() override
	{
		SecureResetString(ErrorCode);
	}

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		return ParseJsonObject(
				   Secret, Object,
				   TConstArrayView<const TCHAR *>(OAuthErrorJsonFields, UE_ARRAY_COUNT(OAuthErrorJsonFields))) &&
			   Object->TryGetStringField(TEXT("error"), ErrorCode) && IsBoundedUtf8(ErrorCode, MaxOAuthErrorCodeBytes);
	}
};

class FXAITokenResponseConsumer final : public IUnrealAIOAuthHttpSecretConsumer
{
  public:
	FXAITokenResponseConsumer(const FDateTime &InNowUtc, const bool bInRequireRefresh)
		: NowUtc(InNowUtc), bRequireRefresh(bInRequireRefresh)
	{
	}

	FUnrealAIOAuthTokenSet Tokens;
	EUnrealAIProviderAccessErrorCode FailureCode = EUnrealAIProviderAccessErrorCode::AuthResponseInvalid;

  protected:
	bool ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret) override
	{
		TSharedPtr<FJsonObject> Object;
		FString AccessToken;
		FString RefreshToken;
		FString IdToken;
		FString TokenType;
		FString Scope;
		FString Error;
		if (!ParseJsonObject(Secret, Object,
							 TConstArrayView<const TCHAR *>(TokenJsonFields, UE_ARRAY_COUNT(TokenJsonFields))))
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthResponseInvalid;
			return false;
		}
		const bool bHasAccess = Object->TryGetStringField(TEXT("access_token"), AccessToken);
		const bool bHasRefresh = Object->TryGetStringField(TEXT("refresh_token"), RefreshToken);
		const bool bHasId = Object->TryGetStringField(TEXT("id_token"), IdToken);
		const bool bHasTokenType = Object->TryGetStringField(TEXT("token_type"), TokenType);
		const bool bHasScope = Object->TryGetStringField(TEXT("scope"), Scope);
		const bool bTokenTypePresent = Object->HasField(TEXT("token_type"));
		const bool bScopePresent = Object->HasField(TEXT("scope"));
		const bool bRefreshPresent = Object->HasField(TEXT("refresh_token"));
		const bool bIdPresent = Object->HasField(TEXT("id_token"));
		FDateTime Expiry;
		bool bValid = false;
		if (!bHasAccess || AccessToken.IsEmpty())
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete;
		}
		else if (bRequireRefresh && (!bHasRefresh || RefreshToken.IsEmpty()))
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete;
		}
		else if ((bRefreshPresent && !bHasRefresh) || (bIdPresent && !bHasId) ||
				 (bTokenTypePresent && !bHasTokenType) || (bScopePresent && !bHasScope))
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthResponseInvalid;
		}
		else if (!TokenType.IsEmpty() && !TokenType.Equals(TEXT("Bearer"), ESearchCase::IgnoreCase))
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported;
		}
		else if (bHasScope && !HasRequiredCompatibilityScopes(Scope))
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient;
		}
		else if (!TryResolveAccessExpiry(Object, AccessToken, NowUtc, Expiry))
		{
			FailureCode = EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid;
		}
		else
		{
			bValid = true;
		}
		if (bValid)
		{
			bValid =
				TrySecretFromUtf8(AccessToken, FUnrealAIOAuthTokenSet::MaxAccessTokenBytes, Tokens.AccessToken, Error);
		}
		if (bValid && !RefreshToken.IsEmpty())
		{
			bValid = TrySecretFromUtf8(RefreshToken, FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes, Tokens.RefreshToken,
									   Error);
		}
		if (bValid && !IdToken.IsEmpty())
		{
			bValid = TrySecretFromUtf8(IdToken, FUnrealAIOAuthTokenSet::MaxIdTokenBytes, Tokens.IdToken, Error);
		}
		SecureResetString(AccessToken);
		SecureResetString(RefreshToken);
		SecureResetString(IdToken);
		SecureResetString(TokenType);
		SecureResetString(Scope);
		if (!bValid)
		{
			Tokens.Reset();
			if (FailureCode == EUnrealAIProviderAccessErrorCode::None)
			{
				FailureCode = EUnrealAIProviderAccessErrorCode::AuthResponseInvalid;
			}
			return false;
		}
		Tokens.AccessTokenExpiresAtUtc = Expiry;
		FailureCode = EUnrealAIProviderAccessErrorCode::None;
		return true;
	}

  private:
	FDateTime NowUtc;
	bool bRequireRefresh = false;
};

class FSystemXAIOAuthPollWaiter final : public IUnrealAIXAIOAuthPollWaiter
{
  public:
	bool Wait(const double Seconds, const double OverallDeadlineSeconds, const IUnrealAIClock &Clock,
			  const FUnrealAICancellationToken &Cancellation) override
	{
		const double Until = FMath::Min(Clock.MonotonicSeconds() + Seconds, OverallDeadlineSeconds);
		while (Clock.MonotonicSeconds() < Until)
		{
			if (Cancellation.IsCancellationRequested())
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.05f);
		}
		return !Cancellation.IsCancellationRequested() && Clock.MonotonicSeconds() < OverallDeadlineSeconds;
	}
};
} // namespace

class FUnrealAIXAIDeviceOAuthDriver::FState final
{
  public:
	FState(TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> InTransport,
		   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		   TSharedPtr<IUnrealAIXAIOAuthPollWaiter, ESPMode::ThreadSafe> InPollWaiter)
		: Transport(MoveTemp(InTransport)), Clock(MoveTemp(InClock)),
		  PollWaiter(InPollWaiter.IsValid() ? MoveTemp(InPollWaiter)
											: MakeShared<FSystemXAIOAuthPollWaiter, ESPMode::ThreadSafe>())
	{
		FString OriginError;
		const bool bAuthOrigin = FUnrealAIEndpointOrigin::TryParse(AuthOriginText, false, AuthOrigin, OriginError);
		const bool bVerificationOrigin =
			FUnrealAIEndpointOrigin::TryParse(VerificationOriginText, false, VerificationOrigin, OriginError);
		bValid = bAuthOrigin && bVerificationOrigin;
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
		const double OverallDeadline = Clock->MonotonicSeconds() + TimeoutSeconds;
		if (!Discover(OverallDeadline, Cancellation, false, OutError))
		{
			return false;
		}

		FUnrealAISecretValue DeviceBody;
		FString SensitiveError;
		if (!TryMakeDeviceCodeBody(DeviceBody, SensitiveError))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		FUnrealAIOAuthHttpResult DeviceResult;
		const double DeviceRemaining = OverallDeadline - Clock->MonotonicSeconds();
		if (!PerformOAuthExchange(*Transport, AuthOrigin, DeviceCodePath, EUnrealAIOAuthHttpMethod::Post,
								  EUnrealAIOAuthHttpContentType::FormUrlEncoded, MoveTemp(DeviceBody), DeviceRemaining,
								  Cancellation, false, DeviceResult, OutError))
		{
			return false;
		}
		const int32 DeviceStatus = DeviceResult.GetResponseMetadata().StatusCode;
		if (DeviceStatus != 200)
		{
			OutError =
				DeviceStatus == 401 || DeviceStatus == 403
					? MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
									EUnrealAIProviderAccessErrorCode::AccessProfileNotReady)
					: MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
									DeviceStatus == 429 || DeviceStatus >= 500,
									DeviceResult.GetResponseMetadata().RetryAfterSeconds);
			return false;
		}
		FXAIDeviceResponseConsumer DeviceResponse;
		if (!DeviceResult.TryConsumeBody(DeviceResponse))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}

		FUnrealAIAuthInteraction Interaction;
		FString InteractionError;
		if (!FUnrealAIAuthInteraction::TryCreateDeviceCode(VerificationOrigin, FString(ExactVerificationUri),
														   FString(DeviceResponse.UserCode), Interaction,
														   InteractionError) ||
			!InteractionPublisher.PublishInteraction(MoveTemp(Interaction)))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}

		const double DeviceDeadline =
			FMath::Min(OverallDeadline, Clock->MonotonicSeconds() + DeviceResponse.ExpiresInSeconds);
		double PollInterval = DeviceResponse.IntervalSeconds;
		for (;;)
		{
			if (Cancellation.IsCancellationRequested())
			{
				OutError = MakeCancellationError(Cancellation, false);
				return false;
			}
			if (!PollWaiter->Wait(PollInterval, DeviceDeadline, *Clock, Cancellation))
			{
				OutError = Cancellation.IsCancellationRequested()
							   ? MakeCancellationError(Cancellation, false)
							   : MakeAuthError(EUnrealAIErrorCategory::Timeout,
											   EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
				return false;
			}
			if (Cancellation.IsCancellationRequested())
			{
				OutError = MakeCancellationError(Cancellation, false);
				return false;
			}
			const double Remaining = DeviceDeadline - Clock->MonotonicSeconds();
			if (Remaining <= 0.0)
			{
				OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
										 EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
				return false;
			}
			FUnrealAISecretValue PollBody;
			if (!TryMakePollBody(DeviceResponse.DeviceCode, PollBody, SensitiveError))
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
				return false;
			}
			FUnrealAIOAuthHttpResult PollResult;
			if (!PerformOAuthExchange(*Transport, AuthOrigin, TokenPath, EUnrealAIOAuthHttpMethod::Post,
									  EUnrealAIOAuthHttpContentType::FormUrlEncoded, MoveTemp(PollBody), Remaining,
									  Cancellation, false, PollResult, OutError))
			{
				return false;
			}
			const int32 Status = PollResult.GetResponseMetadata().StatusCode;
			if (Status == 200)
			{
				FXAITokenResponseConsumer TokenConsumer(Clock->UtcNow(), true);
				if (!PollResult.TryConsumeBody(TokenConsumer))
				{
					const EUnrealAIProviderAccessErrorCode FailureCode =
						TokenConsumer.FailureCode == EUnrealAIProviderAccessErrorCode::None
							? EUnrealAIProviderAccessErrorCode::AuthResponseInvalid
							: TokenConsumer.FailureCode;
					OutError = MakeAuthError(FailureCode == EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient
												 ? EUnrealAIErrorCategory::NotAuthorized
												 : EUnrealAIErrorCategory::Provider,
											 FailureCode);
					return false;
				}
				OutTokens = MoveTemp(TokenConsumer.Tokens);
				return true;
			}

			FXAIOAuthErrorResponseConsumer ErrorConsumer;
			if (!PollResult.TryConsumeBody(ErrorConsumer))
			{
				OutError =
					Status == 401 || Status == 403
						? MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
										EUnrealAIProviderAccessErrorCode::AccessProfileNotReady)
						: MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
										Status == 429 || Status >= 500,
										PollResult.GetResponseMetadata().RetryAfterSeconds);
				return false;
			}
			if (ErrorConsumer.ErrorCode == TEXT("access_denied"))
			{
				OutError = MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
										 EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
				return false;
			}
			if (ErrorConsumer.ErrorCode == TEXT("expired_token"))
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut);
				return false;
			}
			if (ErrorConsumer.ErrorCode == TEXT("slow_down"))
			{
				PollInterval = FMath::Min(PollInterval + 5.0, MaxPollIntervalSeconds);
			}
			else if (ErrorConsumer.ErrorCode != TEXT("authorization_pending"))
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
								  Status == 429 || Status >= 500, PollResult.GetResponseMetadata().RetryAfterSeconds);
				return false;
			}
		}
	}

	bool Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, const double TimeoutSeconds,
				 const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError)
	{
		OutError = {};
		if (!bValid || !InOutEnvelope.IsSet() || !InOutEnvelope.HasRefreshToken() || !FMath::IsFinite(TimeoutSeconds) ||
			TimeoutSeconds <= 0.0 || !Cancellation.IsValid())
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
			return false;
		}
		const double OverallDeadline = Clock->MonotonicSeconds() + TimeoutSeconds;
		if (!Discover(OverallDeadline, Cancellation, true, OutError))
		{
			return false;
		}
		FUnrealAISecretValue RefreshBody;
		FString RefreshError;
		if (!InOutEnvelope.TryMintRefreshRequestBody(CompatibilityClientId, RefreshBody, RefreshError))
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
			return false;
		}
		FUnrealAIOAuthHttpResult Result;
		const double Remaining = OverallDeadline - Clock->MonotonicSeconds();
		if (!PerformOAuthExchange(*Transport, AuthOrigin, TokenPath, EUnrealAIOAuthHttpMethod::Post,
								  EUnrealAIOAuthHttpContentType::FormUrlEncoded, MoveTemp(RefreshBody), Remaining,
								  Cancellation, true, Result, OutError))
		{
			return false;
		}
		const int32 Status = Result.GetResponseMetadata().StatusCode;
		if (Status != 200)
		{
			if (Status == 403)
			{
				OutError = MakeAuthError(EUnrealAIErrorCategory::PolicyDenied,
										 EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied);
			}
			else if (Status == 400 || Status == 401)
			{
				OutError = MakeAuthError(EUnrealAIErrorCategory::NotAuthorized,
										 EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
			}
			else
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed,
								  Status == 429 || Status >= 500, Result.GetResponseMetadata().RetryAfterSeconds);
			}
			return false;
		}
		FXAITokenResponseConsumer TokenConsumer(Clock->UtcNow(), false);
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
	bool Discover(const double OverallDeadline, const FUnrealAICancellationToken &Cancellation,
				  const bool bCredentialOperation, FUnrealAIProviderAccessError &OutError)
	{
		const double Remaining = OverallDeadline - Clock->MonotonicSeconds();
		FUnrealAISecretValue EmptyBody;
		FUnrealAIOAuthHttpResult DiscoveryResult;
		if (Remaining <= 0.0 ||
			!PerformOAuthExchange(*Transport, AuthOrigin, DiscoveryPath, EUnrealAIOAuthHttpMethod::Get,
								  EUnrealAIOAuthHttpContentType::Invalid, MoveTemp(EmptyBody), Remaining, Cancellation,
								  bCredentialOperation, DiscoveryResult, OutError))
		{
			if (!OutError.IsError())
			{
				OutError = MakeAuthError(EUnrealAIErrorCategory::Timeout,
										 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialTimedOut
															  : EUnrealAIProviderAccessErrorCode::AuthTimedOut,
										 true);
			}
			return false;
		}
		const int32 Status = DiscoveryResult.GetResponseMetadata().StatusCode;
		if (Status != 200)
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Provider,
							  bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
												   : EUnrealAIProviderAccessErrorCode::AuthFailed,
							  Status == 429 || Status >= 500, DiscoveryResult.GetResponseMetadata().RetryAfterSeconds);
			return false;
		}
		FXAIDiscoveryResponseConsumer DiscoveryConsumer;
		if (!DiscoveryResult.TryConsumeBody(DiscoveryConsumer))
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Provider,
									 bCredentialOperation ? EUnrealAIProviderAccessErrorCode::CredentialFailed
														  : EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		return true;
	}

	TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> Transport;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedPtr<IUnrealAIXAIOAuthPollWaiter, ESPMode::ThreadSafe> PollWaiter;
	FUnrealAIEndpointOrigin AuthOrigin;
	FUnrealAIEndpointOrigin VerificationOrigin;
	bool bValid = false;
};

FUnrealAIXAIDeviceOAuthDriver::FUnrealAIXAIDeviceOAuthDriver(
	TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> InTransport,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	TSharedPtr<IUnrealAIXAIOAuthPollWaiter, ESPMode::ThreadSafe> InPollWaiter)
	: State(MakeShared<FState, ESPMode::ThreadSafe>(MoveTemp(InTransport), MoveTemp(InClock), MoveTemp(InPollWaiter)))
{
}

FUnrealAIXAIDeviceOAuthDriver::~FUnrealAIXAIDeviceOAuthDriver() = default;

FName FUnrealAIXAIDeviceOAuthDriver::GetProviderName() const
{
	return XAIDriverAuthProviderName;
}

bool FUnrealAIXAIDeviceOAuthDriver::Authorize(const double TimeoutSeconds,
											  const FUnrealAICancellationToken &Cancellation,
											  IUnrealAIDeviceOAuthInteractionPublisher &InteractionPublisher,
											  FUnrealAIOAuthTokenSet &OutTokens, FUnrealAIProviderAccessError &OutError)
{
	return State->Authorize(TimeoutSeconds, Cancellation, InteractionPublisher, OutTokens, OutError);
}

bool FUnrealAIXAIDeviceOAuthDriver::Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, const double TimeoutSeconds,
											const FUnrealAICancellationToken &Cancellation,
											FUnrealAIProviderAccessError &OutError)
{
	return State->Refresh(InOutEnvelope, TimeoutSeconds, Cancellation, OutError);
}

int32 FUnrealAIXAIDeviceOAuthDriver::GetCompatibilityRevision()
{
	return CompatibilityRevision;
}
