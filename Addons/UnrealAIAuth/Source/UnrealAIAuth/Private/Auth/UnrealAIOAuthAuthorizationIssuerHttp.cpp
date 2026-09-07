// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthAuthorizationIssuerHttp.h"

#include "Auth/UnrealAIOAuthJwkPrivate.h"
#include "Auth/UnrealAIOAuthStrictJson.h"
#include "Misc/ScopeExit.h"

namespace OAuthJson = UE::UnrealAI::Auth::Private;

namespace
{
constexpr int32 IssuerHttpMaxScopeUtf8Bytes = 256;
const FName IssuerHttpSignatureUse(TEXT("sig"));
const FName IssuerHttpBearerTokenType(TEXT("Bearer"));

FUnrealAIProviderAccessError MakeIssuerError(const EUnrealAIErrorCategory Category,
											 const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

void SetIssuerInvalidResponse(FUnrealAIProviderAccessError &OutError)
{
	OutError = MakeIssuerError(EUnrealAIErrorCategory::ProviderProtocol,
							   EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
}

bool IsIssuerExactHttpsUri(const FString &Value)
{
	FTCHARToUTF8 Utf8(*Value);
	if (Utf8.Length() <= 0 || Utf8.Length() > FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes ||
		!Value.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive) ||
						  Value.Contains(TEXT("\\")) || Value.Contains(TEXT("?")) || Value.Contains(TEXT("#")))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (Character < 0x21 || Character > 0x7e)
		{
			return false;
		}
	}
	const int32 PathIndex = Value.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 8);
	if (PathIndex == INDEX_NONE || PathIndex == Value.Len() - 1)
	{
		return false;
	}
	const FString OriginText = Value.Left(PathIndex);
	if (OriginText.Contains(TEXT("@")))
	{
		return false;
	}
	FUnrealAIEndpointOrigin Origin;
	FString Error;
	return FUnrealAIEndpointOrigin::TryParse(OriginText, false, Origin, Error) && Origin.IsSecure() &&
		   Origin.ToString() == OriginText;
}

bool IsJsonContentType(const FString &Value)
{
	if (Value.IsEmpty() || Value.Len() > 256)
	{
		return false;
	}
	FString MediaType = Value;
	int32 Semicolon = INDEX_NONE;
	if (MediaType.FindChar(TEXT(';'), Semicolon))
	{
		MediaType.LeftInline(Semicolon, EAllowShrinking::No);
	}
	MediaType.TrimStartAndEndInline();
	return MediaType.Equals(TEXT("application/json"), ESearchCase::IgnoreCase) ||
							MediaType.EndsWith(TEXT("+json"), ESearchCase::IgnoreCase);
}

bool TryReadString(const OAuthJson::FStrictJsonValue &Object, const ANSICHAR *Name, FString &OutValue,
				   const bool bRequired = true)
{
	OutValue.Reset();
	const OAuthJson::FStrictJsonValue *Value = Object.FindObjectValue(Name);
	return Value == nullptr ? !bRequired : Value->TryGetString(OutValue);
}

bool IsIssuerOAuthScope(const FString &Scope)
{
	FTCHARToUTF8 Utf8(*Scope);
	if (Utf8.Length() <= 0 || Utf8.Length() > IssuerHttpMaxScopeUtf8Bytes)
	{
		return false;
	}
	for (const TCHAR Character : Scope)
	{
		if (!(Character == 0x21 || (Character >= 0x23 && Character <= 0x5b) ||
			  (Character >= 0x5d && Character <= 0x7e)))
		{
			return false;
		}
	}
	return true;
}

bool TryParseScopes(const OAuthJson::FStrictJsonValue &Value, TArray<FString> &OutScopes)
{
	OutScopes.Reset();
	FString ScopeText;
	if (!Value.TryGetString(ScopeText) ||
		ScopeText.StartsWith(TEXT(" ")) || ScopeText.EndsWith(TEXT(" ")) || ScopeText.Contains(TEXT("  ")))
	{
		return false;
	}
	ScopeText.ParseIntoArray(OutScopes, TEXT(" "), true);
	if (OutScopes.IsEmpty() || OutScopes.Num() > FUnrealAIOAuthBrowserAuthorizationRequest::MaxScopes)
	{
		OutScopes.Reset();
		return false;
	}
	TSet<FString> Seen;
	for (const FString &Scope : OutScopes)
	{
		if (!IsIssuerOAuthScope(Scope) || Seen.Contains(Scope))
		{
			OutScopes.Reset();
			return false;
		}
		Seen.Add(Scope);
	}
	return true;
}

bool TryCreateSecret(const OAuthJson::FStrictJsonValue &Value, const int32 MaxBytes, FUnrealAISecretValue &OutSecret)
{
	OutSecret.Reset();
	if (Value.Type != OAuthJson::EStrictJsonType::String ||
		!OAuthJson::IsVisibleAsciiBytes(Value.ScalarBytes, MaxBytes, false))
	{
		return false;
	}
	TArray<uint8> Copy = Value.ScalarBytes;
	FString Error;
	if (!FUnrealAISecretValue::TryCreate(MoveTemp(Copy), OutSecret, Error))
	{
		OAuthJson::SecureResetOAuthWireBytes(Copy);
		return false;
	}
	return true;
}

double ParseMaxAge(const FString &CacheControl)
{
	if (CacheControl.IsEmpty())
	{
		return 0.0;
	}
	if (CacheControl.Len() > 1024)
	{
		return -1.0;
	}
	TArray<FString> Directives;
	CacheControl.ParseIntoArray(Directives, TEXT(","), false);
	TOptional<int64> MaxAge;
	for (FString Directive : Directives)
	{
		Directive.TrimStartAndEndInline();
		int32 Equals = INDEX_NONE;
		if (!Directive.FindChar(TEXT('='), Equals))
		{
			continue;
		}
		FString Name = Directive.Left(Equals);
		FString Value = Directive.Mid(Equals + 1);
		Name.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();
		if (!Name.Equals(TEXT("max-age"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (MaxAge.IsSet() || Value.IsEmpty())
		{
			return -1.0;
		}
		int64 Parsed = 0;
		for (const TCHAR Character : Value)
		{
			if (Character <
				TEXT('0') || Character > TEXT('9') || Parsed > (static_cast<int64>(
																	FUnrealAIOidcJsonWebKeySet::MaxCacheAgeSeconds) -
																static_cast<int64>(Character - TEXT('0'))) / 10)
			{
				return -1.0;
			}
			Parsed = Parsed * 10 + (Character - TEXT('0'));
		}
		MaxAge = Parsed;
	}
	return static_cast<double>(MaxAge.Get(0));
}

bool ExecuteJson(IUnrealAIOAuthIssuerHttpClient &Client, const FUnrealAIOAuthAuthorizationOperationContext &Context,
				 FStringView ExactUrl, const EUnrealAIOAuthIssuerHttpMethod Method, TConstArrayView<uint8> FormBody,
				 const int32 MaxResponseBytes, FUnrealAIOAuthIssuerHttpResponse &OutResponse,
				 FUnrealAIProviderAccessError &OutError)
{
	OutResponse.Reset();
	OutError = {};
	FUnrealAIOAuthIssuerHttpRequest Request;
	Request.ExactUrl = FString(ExactUrl);
	Request.Method = Method;
	Request.FormBody.Append(FormBody.GetData(), FormBody.Num());
	Request.MaxResponseBodyBytes = MaxResponseBytes;
	FString ShapeError;
	if (!Request.ValidateShape(ShapeError))
	{
		OutError =
			MakeIssuerError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	if (!Client.Execute(Context, Request, OutResponse, OutError))
	{
		FString ErrorShape;
		if (!OutError.IsError() || !OutError.ValidateShape(ErrorShape))
		{
			OutError =
				MakeIssuerError(EUnrealAIErrorCategory::Transport, EUnrealAIProviderAccessErrorCode::AuthFailed, true);
		}
		OutResponse.Reset();
		return false;
	}
	if (!OutResponse.ValidateShape(Request, ShapeError) || OutResponse.StatusCode < 200 ||
		OutResponse.StatusCode >= 300 || !IsJsonContentType(OutResponse.ContentType))
	{
		OutResponse.Reset();
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	return true;
}
} // namespace

FUnrealAIOAuthIssuerHttpRequest::~FUnrealAIOAuthIssuerHttpRequest()
{
	Reset();
}

FUnrealAIOAuthIssuerHttpRequest::FUnrealAIOAuthIssuerHttpRequest(FUnrealAIOAuthIssuerHttpRequest &&Other) noexcept
	: ExactUrl(MoveTemp(Other.ExactUrl)), Method(Other.Method), FormBody(MoveTemp(Other.FormBody)),
	  MaxResponseBodyBytes(Other.MaxResponseBodyBytes)
{
	Other.Method = EUnrealAIOAuthIssuerHttpMethod::Invalid;
	Other.MaxResponseBodyBytes = 64 * 1024;
}

FUnrealAIOAuthIssuerHttpRequest &
FUnrealAIOAuthIssuerHttpRequest::operator=(FUnrealAIOAuthIssuerHttpRequest &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		ExactUrl = MoveTemp(Other.ExactUrl);
		Method = Other.Method;
		FormBody = MoveTemp(Other.FormBody);
		MaxResponseBodyBytes = Other.MaxResponseBodyBytes;
		Other.Method = EUnrealAIOAuthIssuerHttpMethod::Invalid;
		Other.MaxResponseBodyBytes = 64 * 1024;
	}
	return *this;
}

bool FUnrealAIOAuthIssuerHttpRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const bool bMethodValid =
		Method == EUnrealAIOAuthIssuerHttpMethod::Get || Method == EUnrealAIOAuthIssuerHttpMethod::PostForm;
	if (!bMethodValid || !IsIssuerExactHttpsUri(ExactUrl) || MaxResponseBodyBytes <= 0 ||
		MaxResponseBodyBytes > MaxResponseBodyBytesLimit || FormBody.Num() > MaxRequestBodyBytes ||
		(Method == EUnrealAIOAuthIssuerHttpMethod::Get && !FormBody.IsEmpty()) ||
		(Method == EUnrealAIOAuthIssuerHttpMethod::PostForm && FormBody.IsEmpty()))
	{
		OutError = TEXT("OAuth issuer HTTP request has an invalid exact URL, method, or byte bound.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthIssuerHttpRequest::Reset()
{
	ExactUrl.Reset();
	Method = EUnrealAIOAuthIssuerHttpMethod::Invalid;
	OAuthJson::SecureResetOAuthWireBytes(FormBody);
	MaxResponseBodyBytes = 64 * 1024;
}

FUnrealAIOAuthIssuerHttpResponse::~FUnrealAIOAuthIssuerHttpResponse()
{
	Reset();
}

FUnrealAIOAuthIssuerHttpResponse::FUnrealAIOAuthIssuerHttpResponse(FUnrealAIOAuthIssuerHttpResponse &&Other) noexcept
	: StatusCode(Other.StatusCode), ExactEffectiveUrl(MoveTemp(Other.ExactEffectiveUrl)),
	  ContentType(MoveTemp(Other.ContentType)), CacheControl(MoveTemp(Other.CacheControl)), Body(MoveTemp(Other.Body))
{
	Other.StatusCode = 0;
}

FUnrealAIOAuthIssuerHttpResponse &
FUnrealAIOAuthIssuerHttpResponse::operator=(FUnrealAIOAuthIssuerHttpResponse &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		StatusCode = Other.StatusCode;
		ExactEffectiveUrl = MoveTemp(Other.ExactEffectiveUrl);
		ContentType = MoveTemp(Other.ContentType);
		CacheControl = MoveTemp(Other.CacheControl);
		Body = MoveTemp(Other.Body);
		Other.StatusCode = 0;
	}
	return *this;
}

bool FUnrealAIOAuthIssuerHttpResponse::ValidateShape(const FUnrealAIOAuthIssuerHttpRequest &Request,
													 FString &OutError) const
{
	OutError.Reset();
	if (StatusCode < 100 || StatusCode > 599 || ExactEffectiveUrl != Request.ExactUrl || ContentType.Len() > 256 ||
		CacheControl.Len() > 1024 || Body.Num() > Request.MaxResponseBodyBytes)
	{
		OutError = TEXT("OAuth issuer HTTP response violated its exact URL or byte bounds.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthIssuerHttpResponse::Reset()
{
	StatusCode = 0;
	ExactEffectiveUrl.Reset();
	ContentType.Reset();
	CacheControl.Reset();
	OAuthJson::SecureResetOAuthWireBytes(Body);
}

bool FUnrealAIOAuthAuthorizationIssuerHttpConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const auto IsResponseBound = [](const int32 Value)
	{ return Value > 0 && Value <= FUnrealAIOAuthIssuerHttpRequest::MaxResponseBodyBytesLimit; };
	if (!IsResponseBound(MaxDiscoveryResponseBytes) || !IsResponseBound(MaxJwksResponseBytes) ||
		!IsResponseBound(MaxTokenResponseBytes) || !IsResponseBound(MaxRevocationResponseBytes) ||
		MaxAccessTokenLifetimeSeconds <= 0 || MaxAccessTokenLifetimeSeconds > 24 * 60 * 60)
	{
		OutError = TEXT("OAuth issuer HTTP configuration exceeds a production response or lifetime bound.");
		return false;
	}
	return true;
}

FUnrealAIOAuthAuthorizationIssuerHttp::FUnrealAIOAuthAuthorizationIssuerHttp(
	TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe> InHttpClient,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	const FUnrealAIOAuthAuthorizationIssuerHttpConfig &InConfig)
	: HttpClient(MoveTemp(InHttpClient)), Clock(MoveTemp(InClock)), Config(InConfig)
{
}

bool FUnrealAIOAuthAuthorizationIssuerHttp::Discover(const FUnrealAIOAuthAuthorizationOperationContext &Context,
													 FStringView ExactDiscoveryEndpoint,
													 FUnrealAIOidcDiscoveryDocument &OutDocument,
													 FUnrealAIProviderAccessError &OutError)
{
	OutDocument = {};
	FString ConfigError;
	if (!Config.ValidateShape(ConfigError))
	{
		OutError = MakeIssuerError(EUnrealAIErrorCategory::InvalidConfiguration,
								   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	FUnrealAIOAuthIssuerHttpResponse Response;
	if (!ExecuteJson(*HttpClient, Context, ExactDiscoveryEndpoint, EUnrealAIOAuthIssuerHttpMethod::Get, {},
					 Config.MaxDiscoveryResponseBytes, Response, OutError))
	{
		return false;
	}
	OAuthJson::FStrictJsonValue Root;
	OAuthJson::FStrictJsonParseLimits Limits;
	Limits.MaxTotalBytes = Config.MaxDiscoveryResponseBytes;
	Limits.MaxDepth = 5;
	Limits.MaxNodes = 256;
	Limits.MaxStringBytes = FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes;
	Limits.MaxContainerEntries = 64;
	if (!OAuthJson::ParseStrictJson(Response.Body, Limits, Root) || Root.Type != OAuthJson::EStrictJsonType::Object ||
		!TryReadString(Root, "issuer", OutDocument.Issuer) ||
		!TryReadString(Root, "authorization_endpoint", OutDocument.AuthorizationEndpoint) ||
		!TryReadString(Root, "token_endpoint", OutDocument.TokenEndpoint) ||
		!TryReadString(Root, "jwks_uri", OutDocument.JwksEndpoint) ||
		!TryReadString(Root, "revocation_endpoint", OutDocument.RevocationEndpoint, false))
	{
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	const OAuthJson::FStrictJsonValue *Methods = Root.FindObjectValue("code_challenge_methods_supported");
	if (Methods == nullptr || Methods->Type != OAuthJson::EStrictJsonType::Array || Methods->ArrayValues.IsEmpty() ||
		Methods->ArrayValues.Num() > 16)
	{
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	TSet<FString> Seen;
	for (const OAuthJson::FStrictJsonValue &Method : Methods->ArrayValues)
	{
		FString Value;
		if (!Method.TryGetString(Value) || Seen.Contains(Value))
		{
			SetIssuerInvalidResponse(OutError);
			return false;
		}
		Seen.Add(Value);
		OutDocument.bPkceS256Supported |= Value == TEXT("S256");
	}
	FString ShapeError;
	if (!OutDocument.bPkceS256Supported || !OutDocument.ValidateShape(ShapeError))
	{
		OutDocument = {};
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	return true;
}

bool FUnrealAIOAuthAuthorizationIssuerHttp::FetchJsonWebKeys(const FUnrealAIOAuthAuthorizationOperationContext &Context,
															 FStringView ExactJwksEndpoint,
															 FUnrealAIOidcJsonWebKeySet &OutKeySet,
															 FUnrealAIProviderAccessError &OutError)
{
	OutKeySet = {};
	FString ConfigError;
	if (!Config.ValidateShape(ConfigError))
	{
		OutError = MakeIssuerError(EUnrealAIErrorCategory::InvalidConfiguration,
								   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	FUnrealAIOAuthIssuerHttpResponse Response;
	if (!ExecuteJson(*HttpClient, Context, ExactJwksEndpoint, EUnrealAIOAuthIssuerHttpMethod::Get, {},
					 Config.MaxJwksResponseBytes, Response, OutError))
	{
		return false;
	}
	OAuthJson::FStrictJsonValue Root;
	OAuthJson::FStrictJsonParseLimits Limits;
	Limits.MaxTotalBytes = Config.MaxJwksResponseBytes;
	Limits.MaxDepth = 6;
	Limits.MaxNodes = 1024;
	Limits.MaxStringBytes = 16 * 1024;
	Limits.MaxContainerEntries = 256;
	if (!OAuthJson::ParseStrictJson(Response.Body, Limits, Root) || Root.Type != OAuthJson::EStrictJsonType::Object)
	{
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	const OAuthJson::FStrictJsonValue *Keys = Root.FindObjectValue("keys");
	if (Keys == nullptr || Keys->Type != OAuthJson::EStrictJsonType::Array || Keys->ArrayValues.IsEmpty() ||
		Keys->ArrayValues.Num() > FUnrealAIOidcJsonWebKeySet::MaxKeys)
	{
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	for (const OAuthJson::FStrictJsonValue &Source : Keys->ArrayValues)
	{
		FUnrealAIOidcJsonWebKey Key;
		const OAuthJson::FStrictJsonValue *KeyId = Source.FindObjectValue("kid");
		if (KeyId == nullptr || !KeyId->TryGetString(Key.KeyId) ||
			!OAuthJson::TryEncodeJwkSubjectPublicKeyInfo(Source, Key.KeyType, Key.Algorithm, Key.PublicKeyMaterial))
		{
			OutKeySet = {};
			SetIssuerInvalidResponse(OutError);
			return false;
		}
		Key.Use = IssuerHttpSignatureUse;
		OutKeySet.Keys.Add(MoveTemp(Key));
	}
	OutKeySet.MaxAgeSeconds = ParseMaxAge(Response.CacheControl);
	FString ShapeError;
	if (OutKeySet.MaxAgeSeconds < 0.0 || !OutKeySet.ValidateShape(ShapeError))
	{
		OutKeySet = {};
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	return true;
}

bool FUnrealAIOAuthAuthorizationIssuerHttp::ExchangeAuthorizationCode(
	const FUnrealAIOAuthAuthorizationOperationContext &Context, FStringView ExactTokenEndpoint,
	TConstArrayView<uint8> FormBody, FUnrealAIOAuthAuthorizationCodeResponse &OutResponse,
	FUnrealAIProviderAccessError &OutError)
{
	OutResponse.Reset();
	FString ConfigError;
	if (!Config.ValidateShape(ConfigError))
	{
		OutError = MakeIssuerError(EUnrealAIErrorCategory::InvalidConfiguration,
								   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	FUnrealAIOAuthIssuerHttpResponse Response;
	if (!ExecuteJson(*HttpClient, Context, ExactTokenEndpoint, EUnrealAIOAuthIssuerHttpMethod::PostForm, FormBody,
					 Config.MaxTokenResponseBytes, Response, OutError))
	{
		return false;
	}
	OAuthJson::FStrictJsonValue Root;
	OAuthJson::FStrictJsonParseLimits Limits;
	Limits.MaxTotalBytes = Config.MaxTokenResponseBytes;
	Limits.MaxDepth = 4;
	Limits.MaxNodes = 64;
	Limits.MaxStringBytes = FUnrealAIOAuthTokenSet::MaxIdTokenBytes;
	Limits.MaxContainerEntries = 32;
	if (!OAuthJson::ParseStrictJson(Response.Body, Limits, Root) || Root.Type != OAuthJson::EStrictJsonType::Object)
	{
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	const OAuthJson::FStrictJsonValue *AccessToken = Root.FindObjectValue("access_token");
	const OAuthJson::FStrictJsonValue *RefreshCredential = Root.FindObjectValue("refresh_token");
	const OAuthJson::FStrictJsonValue *IdToken = Root.FindObjectValue("id_token");
	const OAuthJson::FStrictJsonValue *TokenType = Root.FindObjectValue("token_type");
	const OAuthJson::FStrictJsonValue *ExpiresIn = Root.FindObjectValue("expires_in");
	const OAuthJson::FStrictJsonValue *Scope = Root.FindObjectValue("scope");
	int64 LifetimeSeconds = 0;
	if (AccessToken == nullptr || IdToken == nullptr || TokenType == nullptr || ExpiresIn == nullptr ||
		Scope == nullptr || !TokenType->EqualsAscii("Bearer") || !ExpiresIn->TryGetInt64(LifetimeSeconds) ||
		LifetimeSeconds <= 0 || LifetimeSeconds > Config.MaxAccessTokenLifetimeSeconds ||
		!TryCreateSecret(*AccessToken, FUnrealAIOAuthTokenSet::MaxAccessTokenBytes, OutResponse.Tokens.AccessToken) ||
		!TryCreateSecret(*IdToken, FUnrealAIOAuthTokenSet::MaxIdTokenBytes, OutResponse.Tokens.IdToken) ||
		(RefreshCredential != nullptr &&
		 !TryCreateSecret(*RefreshCredential, FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes,
						  OutResponse.Tokens.RefreshToken)) ||
		!TryParseScopes(*Scope, OutResponse.GrantedScopes))
	{
		OutResponse.Reset();
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	OutResponse.TokenType = IssuerHttpBearerTokenType;
	const FDateTime NowUtc = Clock->UtcNow();
	const int64 NowUnixSeconds = NowUtc.ToUnixTimestamp();
	if (NowUnixSeconds < 0 || NowUnixSeconds > 253402300799LL - LifetimeSeconds)
	{
		OutResponse.Reset();
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	OutResponse.Tokens.AccessTokenExpiresAtUtc = FDateTime::FromUnixTimestamp(NowUnixSeconds + LifetimeSeconds);
	FString ShapeError;
	if (!OutResponse.ValidateShape(NowUtc, ShapeError))
	{
		OutResponse.Reset();
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	return true;
}

bool FUnrealAIOAuthAuthorizationIssuerHttp::Revoke(const FUnrealAIOAuthAuthorizationOperationContext &Context,
												   FStringView ExactRevocationEndpoint, TConstArrayView<uint8> FormBody,
												   FUnrealAIProviderAccessError &OutError)
{
	FString ConfigError;
	if (!Config.ValidateShape(ConfigError))
	{
		OutError = MakeIssuerError(EUnrealAIErrorCategory::InvalidConfiguration,
								   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	FUnrealAIOAuthIssuerHttpRequest Request;
	Request.ExactUrl = FString(ExactRevocationEndpoint);
	Request.Method = EUnrealAIOAuthIssuerHttpMethod::PostForm;
	Request.FormBody.Append(FormBody.GetData(), FormBody.Num());
	Request.MaxResponseBodyBytes = Config.MaxRevocationResponseBytes;
	FString ShapeError;
	if (!Request.ValidateShape(ShapeError))
	{
		OutError =
			MakeIssuerError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	FUnrealAIOAuthIssuerHttpResponse Response;
	if (!HttpClient->Execute(Context, Request, Response, OutError))
	{
		FString ErrorShape;
		if (!OutError.IsError() || !OutError.ValidateShape(ErrorShape))
		{
			OutError =
				MakeIssuerError(EUnrealAIErrorCategory::Transport, EUnrealAIProviderAccessErrorCode::AuthFailed, true);
		}
		return false;
	}
	if (!Response.ValidateShape(Request, ShapeError) || Response.StatusCode < 200 || Response.StatusCode >= 300)
	{
		SetIssuerInvalidResponse(OutError);
		return false;
	}
	OutError = {};
	return true;
}
