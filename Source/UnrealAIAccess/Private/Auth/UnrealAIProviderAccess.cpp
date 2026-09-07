// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIProviderAccess.h"

#include "Values/UnrealAIPhysicalAllocationValidationPrivate.h"
#include "Values/UnrealAITextValidationPrivate.h"

#include "Misc/ScopeRWLock.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace UE::UnrealAI::Private
{
class FCredentialFreshnessState final
{
  public:
	mutable FRWLock Lock;
	uint64 Generation = 1;
	bool bShutdown = false;
	std::atomic<bool> bRevocationRequested{false};
	std::atomic<bool> bShutdownRequested{false};
};

thread_local int32 GCredentialDispatchDepth = 0;

class FCredentialDispatchThreadScope final
{
  public:
	FCredentialDispatchThreadScope()
	{
		++GCredentialDispatchDepth;
	}
	~FCredentialDispatchThreadScope()
	{
		--GCredentialDispatchDepth;
	}
};
} // namespace UE::UnrealAI::Private

namespace
{
using namespace UE::UnrealAI;

bool FitsUtf8(const FString &Value, const int32 MaxBytes)
{
	return PhysicalAllocation::Private::HasBoundedStringStorage(Value, MaxBytes) &&
		   TextValidation::Private::IsWellFormedSerializedString(Value) &&
		   TextValidation::Private::Utf8Length(Value) <= MaxBytes;
}

bool IsStableIdentifier(const FName Name)
{
	if (Name.IsNone())
	{
		return false;
	}
	const FString Text = Name.ToString();
	if (!FitsUtf8(Text, FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes) || Text.IsEmpty())
	{
		return false;
	}
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'));
		if (!bAlphaNumeric && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-'))
		{
			return false;
		}
		if ((Index == 0 || Index == Text.Len() - 1) && !bAlphaNumeric)
		{
			return false;
		}
	}
	return true;
}

bool IsAsciiHostCharacter(const TCHAR Character)
{
	return (Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9')) ||
																  Character == TEXT('.') || Character == TEXT('-');
}

bool IsValidDnsOrIpv4Host(const FString &Host)
{
	if (Host.IsEmpty() || Host.Len() > 253 ||
		Host.StartsWith(TEXT(".")) || Host.EndsWith(TEXT(".")) || Host.Contains(TEXT("..")))
	{
		return false;
	}
	for (const TCHAR Character : Host)
	{
		if (!IsAsciiHostCharacter(Character))
		{
			return false;
		}
	}
	TArray<FString> Labels;
	Host.ParseIntoArray(Labels, TEXT("."), false);
	if (Labels.IsEmpty())
	{
		return false;
	}
	bool bNumericOnly = true;
	for (const FString &Label : Labels)
	{
		if (Label.IsEmpty() || Label.Len() > 63 || Label.StartsWith(TEXT("-")) || Label.EndsWith(TEXT("-")))
		{
			return false;
		}
		for (const TCHAR Character : Label)
		{
			bNumericOnly &= Character >= TEXT('0') && Character <= TEXT('9');
		}
	}
	if (!bNumericOnly)
	{
		return true;
	}
	if (Labels.Num() != 4)
	{
		return false;
	}
	for (const FString &Label : Labels)
	{
		if (Label.Len() > 1 && Label[0] == TEXT('0'))
		{
			return false;
		}
		int32 Value = 0;
		for (const TCHAR Character : Label)
		{
			Value = Value * 10 + Character - TEXT('0');
		}
		if (Value > 255)
		{
			return false;
		}
	}
	return true;
}

bool IsValidBracketedIpv6Host(const FString &Host)
{
	// Fail closed until the HTTP transport and policy layer share one standards-based IPv6 parser.
	return Host == TEXT("[::1]");
}

bool IsLoopbackHost(const FString &Host)
{
	if (Host == TEXT("localhost") || Host == TEXT("[::1]"))
	{
		return true;
	}
	TArray<FString> Octets;
	Host.ParseIntoArray(Octets, TEXT("."), false);
	if (Octets.Num() != 4 || Octets[0] != TEXT("127"))
	{
		return false;
	}
	for (const FString &Octet : Octets)
	{
		if (Octet.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : Octet)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
		}
	}
	return true;
}

bool TryParsePort(const FString &Text, int32 &OutPort)
{
	OutPort = 0;
	if (Text.IsEmpty() || Text.Len() > 5)
	{
		return false;
	}
	int32 Port = 0;
	for (const TCHAR Character : Text)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		Port = Port * 10 + Character - TEXT('0');
	}
	if (Port < 1 || Port > 65535)
	{
		return false;
	}
	OutPort = Port;
	return true;
}

bool CredentialDestinationEqual(const FUnrealAICredentialDestination &A, const FUnrealAICredentialDestination &B)
{
	return A.ModelProviderName == B.ModelProviderName && A.AccountAuthProviderName == B.AccountAuthProviderName &&
		   A.AuthProfileId == B.AuthProfileId && A.AccountId == B.AccountId && A.TenantRealm == B.TenantRealm &&
		   A.BillingPrincipalId == B.BillingPrincipalId && A.PayerHandle == B.PayerHandle &&
		   A.AuthScheme == B.AuthScheme && A.BillingMode == B.BillingMode && A.EndpointOrigin == B.EndpointOrigin &&
		   A.Audience == B.Audience && A.ConnectionRevision == B.ConnectionRevision &&
		   A.EndpointPolicyRevision == B.EndpointPolicyRevision;
}

bool AuthBillingPairIsValid(const EUnrealAIAuthScheme AuthScheme, const EUnrealAIBillingMode BillingMode)
{
	switch (AuthScheme)
	{
	case EUnrealAIAuthScheme::Anonymous:
		return BillingMode == EUnrealAIBillingMode::Local;
	case EUnrealAIAuthScheme::ApiKey:
		return BillingMode == EUnrealAIBillingMode::ApiMetered;
	case EUnrealAIAuthScheme::OAuthBearer:
		return BillingMode == EUnrealAIBillingMode::ApiMetered ||
			   BillingMode == EUnrealAIBillingMode::SubscriptionQuota;
	case EUnrealAIAuthScheme::GatewayBearer:
		return BillingMode == EUnrealAIBillingMode::GatewayAccounted;
	case EUnrealAIAuthScheme::Invalid:
	default:
		return false;
	}
}

bool IsKnownAccountState(const EUnrealAIAccountAuthState State)
{
	return State >= EUnrealAIAccountAuthState::SignedOut && State <= EUnrealAIAccountAuthState::Failed;
}

bool IsKnownAuthTerminal(const EUnrealAIAuthEventKind Kind)
{
	return Kind == EUnrealAIAuthEventKind::Succeeded || Kind == EUnrealAIAuthEventKind::Failed ||
		   Kind == EUnrealAIAuthEventKind::Cancelled || Kind == EUnrealAIAuthEventKind::TimedOut;
}

bool IsKnownCredentialTerminal(const EUnrealAICredentialResultKind Kind)
{
	return Kind >= EUnrealAICredentialResultKind::Succeeded && Kind <= EUnrealAICredentialResultKind::TimedOut;
}

bool IsCredentialFailureCode(const EUnrealAIProviderAccessErrorCode Code)
{
	switch (Code)
	{
	case EUnrealAIProviderAccessErrorCode::CredentialFailed:
	case EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied:
	case EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity:
	case EUnrealAIProviderAccessErrorCode::PartnerGated:
	case EUnrealAIProviderAccessErrorCode::AccessProfileNotReady:
	case EUnrealAIProviderAccessErrorCode::UnsupportedCapability:
	case EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext:
	case EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch:
	case EUnrealAIProviderAccessErrorCode::InvalidSecretStoreWrite:
	case EUnrealAIProviderAccessErrorCode::InvalidSecretStoreDelete:
	case EUnrealAIProviderAccessErrorCode::SecretNotFound:
	case EUnrealAIProviderAccessErrorCode::SecretRevisionConflict:
	case EUnrealAIProviderAccessErrorCode::SecretStoreLocked:
	case EUnrealAIProviderAccessErrorCode::SecretStoreDenied:
	case EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable:
	case EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported:
	case EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt:
	case EUnrealAIProviderAccessErrorCode::SecretStoreCapacity:
	case EUnrealAIProviderAccessErrorCode::SecretCopyFailed:
	case EUnrealAIProviderAccessErrorCode::Internal:
		return true;
	case EUnrealAIProviderAccessErrorCode::None:
	case EUnrealAIProviderAccessErrorCode::InvalidRequest:
	case EUnrealAIProviderAccessErrorCode::InvalidConfiguration:
	case EUnrealAIProviderAccessErrorCode::AuthCancelled:
	case EUnrealAIProviderAccessErrorCode::AuthTimedOut:
	case EUnrealAIProviderAccessErrorCode::AuthFailed:
	case EUnrealAIProviderAccessErrorCode::CredentialCancelled:
	case EUnrealAIProviderAccessErrorCode::CredentialTimedOut:
	case EUnrealAIProviderAccessErrorCode::SecretStoreCancelled:
	case EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut:
	case EUnrealAIProviderAccessErrorCode::OperationBusy:
	case EUnrealAIProviderAccessErrorCode::AuthResponseInvalid:
	case EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete:
	case EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient:
	case EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported:
	case EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid:
	case EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed:
	default:
		return false;
	}
}

EUnrealAIErrorCategory ExpectedCategoryForAccessCode(const EUnrealAIProviderAccessErrorCode Code)
{
	switch (Code)
	{
	case EUnrealAIProviderAccessErrorCode::None:
		return EUnrealAIErrorCategory::None;
	case EUnrealAIProviderAccessErrorCode::InvalidRequest:
	case EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext:
	case EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch:
	case EUnrealAIProviderAccessErrorCode::InvalidSecretStoreWrite:
	case EUnrealAIProviderAccessErrorCode::InvalidSecretStoreDelete:
		return EUnrealAIErrorCategory::InvalidArgument;
	case EUnrealAIProviderAccessErrorCode::InvalidConfiguration:
		return EUnrealAIErrorCategory::InvalidConfiguration;
	case EUnrealAIProviderAccessErrorCode::UnsupportedCapability:
	case EUnrealAIProviderAccessErrorCode::PartnerGated:
		return EUnrealAIErrorCategory::UnsupportedCapability;
	case EUnrealAIProviderAccessErrorCode::AccessProfileNotReady:
		return EUnrealAIErrorCategory::NotAuthorized;
	case EUnrealAIProviderAccessErrorCode::AuthCancelled:
	case EUnrealAIProviderAccessErrorCode::CredentialCancelled:
	case EUnrealAIProviderAccessErrorCode::SecretStoreCancelled:
		return EUnrealAIErrorCategory::Cancelled;
	case EUnrealAIProviderAccessErrorCode::AuthTimedOut:
	case EUnrealAIProviderAccessErrorCode::CredentialTimedOut:
	case EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut:
		return EUnrealAIErrorCategory::Timeout;
	case EUnrealAIProviderAccessErrorCode::AuthFailed:
	case EUnrealAIProviderAccessErrorCode::CredentialFailed:
	case EUnrealAIProviderAccessErrorCode::AuthResponseInvalid:
	case EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete:
	case EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported:
	case EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid:
		return EUnrealAIErrorCategory::Provider;
	case EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient:
		return EUnrealAIErrorCategory::NotAuthorized;
	case EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied:
		return EUnrealAIErrorCategory::PolicyDenied;
	case EUnrealAIProviderAccessErrorCode::SecretNotFound:
		return EUnrealAIErrorCategory::NotFound;
	case EUnrealAIProviderAccessErrorCode::SecretRevisionConflict:
		return EUnrealAIErrorCategory::VersionMismatch;
	case EUnrealAIProviderAccessErrorCode::SecretStoreLocked:
	case EUnrealAIProviderAccessErrorCode::SecretStoreCapacity:
	case EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity:
	case EUnrealAIProviderAccessErrorCode::OperationBusy:
		return EUnrealAIErrorCategory::Busy;
	case EUnrealAIProviderAccessErrorCode::SecretStoreDenied:
		return EUnrealAIErrorCategory::PolicyDenied;
	case EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable:
	case EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt:
	case EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed:
		return EUnrealAIErrorCategory::Persistence;
	case EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported:
		return EUnrealAIErrorCategory::UnsupportedCapability;
	case EUnrealAIProviderAccessErrorCode::SecretCopyFailed:
	case EUnrealAIProviderAccessErrorCode::Internal:
	default:
		return EUnrealAIErrorCategory::Internal;
	}
}

void WipeString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

bool IsVisibleAscii(const FString &Value)
{
	for (const TCHAR Character : Value)
	{
		if (Character < 0x21 || Character > 0x7e)
		{
			return false;
		}
	}
	return true;
}

bool IsBoundToInteractionOrigin(const FUnrealAIEndpointOrigin &Origin, const FString &Uri)
{
	const FString OriginText = Origin.ToString();
	return Uri == OriginText || Uri.StartsWith(OriginText + TEXT("/")) || Uri.StartsWith(OriginText + TEXT("?"));
}

bool HasExactlyOneNonEmptyQueryParameter(const FString &Uri, const FStringView Name,
										 const TOptional<FStringView> ExactValue = {})
{
	int32 QueryIndex = INDEX_NONE;
	if (!Uri.FindChar(TEXT('?'), QueryIndex) || QueryIndex == Uri.Len() - 1)
	{
		return false;
	}
	TArray<FString> Parameters;
	Uri.Mid(QueryIndex + 1).ParseIntoArray(Parameters, TEXT("&"), false);
	int32 MatchCount = 0;
	FString MatchedValue;
	for (const FString &Parameter : Parameters)
	{
		int32 EqualsIndex = INDEX_NONE;
		const bool bHasEquals = Parameter.FindChar(TEXT('='), EqualsIndex);
		const FStringView Key = bHasEquals ? FStringView(Parameter).Left(EqualsIndex) : FStringView(Parameter);
		if (Key == Name)
		{
			++MatchCount;
			MatchedValue = bHasEquals ? Parameter.Mid(EqualsIndex + 1) : FString{};
		}
	}
	return MatchCount == 1 && !MatchedValue.IsEmpty() && (!ExactValue.IsSet() || MatchedValue == ExactValue.GetValue());
}

bool HasUnambiguousQuerySyntax(const FString &Uri)
{
	int32 QueryIndex = INDEX_NONE;
	if (!Uri.FindChar(TEXT('?'), QueryIndex) || QueryIndex == Uri.Len() - 1)
	{
		return false;
	}
	const FString Query = Uri.Mid(QueryIndex + 1);
	if (Query.Contains(TEXT("?")) || Query.Contains(TEXT(";")))
	{
		return false;
	}
	TArray<FString> Parameters;
	Query.ParseIntoArray(Parameters, TEXT("&"), false);
	TSet<FString> Names;
	for (const FString &Parameter : Parameters)
	{
		int32 EqualsIndex = INDEX_NONE;
		if (!Parameter.FindChar(TEXT('='), EqualsIndex) || EqualsIndex <= 0 || EqualsIndex == Parameter.Len() - 1)
		{
			return false;
		}
		const FString Name = Parameter.Left(EqualsIndex);
		for (const TCHAR Character : Name)
		{
			const bool bAllowed =
				(Character >= TEXT('a') && Character <= TEXT('z')) ||
				 (Character >= TEXT('A') && Character <= TEXT('Z')) ||
				  (Character >= TEXT('0') && Character <= TEXT('9')) ||
				   Character == TEXT('-') || Character == TEXT('.') || Character == TEXT('_') || Character == TEXT('~');
			if (!bAllowed)
			{
				return false;
			}
		}
		if (Names.Contains(Name))
		{
			return false;
		}
		Names.Add(Name);
		const FString Value = Parameter.Mid(EqualsIndex + 1);
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Value[Index] == TEXT('%'))
			{
				const auto IsHex = [](const TCHAR Hex)
				{
					return (Hex >= TEXT('0') && Hex <= TEXT('9')) || (Hex >= TEXT('a') && Hex <= TEXT('f')) ||
																	  (Hex >= TEXT('A') && Hex <= TEXT('F'));
				};
				if (Index + 2 >= Value.Len() || !IsHex(Value[Index + 1]) || !IsHex(Value[Index + 2]))
				{
					return false;
				}
				Index += 2;
			}
		}
	}
	return !Parameters.IsEmpty();
}
} // namespace

bool FUnrealAISecretHandle::IsValid() const
{
	return IsStableIdentifier(StoreName) && Value.IsValid();
}

bool FUnrealAIProviderAccessError::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (static_cast<uint8>(Category) > static_cast<uint8>(EUnrealAIErrorCategory::Internal) ||
		static_cast<uint8>(Code) > static_cast<uint8>(EUnrealAIProviderAccessErrorCode::Internal) ||
		Category != ExpectedCategoryForAccessCode(Code) || !FMath::IsFinite(RetryAfterSeconds) ||
		RetryAfterSeconds < 0.0f || RetryAfterSeconds > MaxRetryAfterSeconds ||
		(!bRetryable && RetryAfterSeconds > 0.0f) ||
		(Category == EUnrealAIErrorCategory::None && (bRetryable || RetryAfterSeconds > 0.0f)))
	{
		OutError = TEXT("Provider-access error requires a known closed code/category and bounded retry policy.");
		return false;
	}
	return true;
}

bool FUnrealAIProviderEndpointAuthority::IsValid() const
{
	return !AuthProviderName.IsNone() && !ModelProviderName.IsNone() &&
		   AuthScheme == EUnrealAIAuthScheme::OAuthBearer && BillingMode == EUnrealAIBillingMode::SubscriptionQuota;
}

bool FUnrealAIProviderEndpointAuthority::Authorizes(const FName InModelProviderName,
													const EUnrealAIAuthScheme InAuthScheme,
													const EUnrealAIBillingMode InBillingMode) const
{
	return IsValid() && ModelProviderName == InModelProviderName && AuthScheme == InAuthScheme &&
		   BillingMode == InBillingMode;
}

bool FUnrealAISecretHandle::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsValid())
	{
		OutError = TEXT("Secret handle requires a stable store name and opaque GUID.");
		return false;
	}
	return true;
}

FUnrealAISecretValue::~FUnrealAISecretValue()
{
	Reset();
}

FUnrealAISecretValue::FUnrealAISecretValue(FUnrealAISecretValue &&Other) noexcept : Bytes(MoveTemp(Other.Bytes)) {}

FUnrealAISecretValue &FUnrealAISecretValue::operator=(FUnrealAISecretValue &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Bytes = MoveTemp(Other.Bytes);
	}
	return *this;
}

bool FUnrealAISecretValue::TryCreate(TArray<uint8> &&InBytes, FUnrealAISecretValue &OutValue, FString &OutError)
{
	OutError.Reset();
	OutValue.Reset();
	if (InBytes.IsEmpty() || InBytes.Num() > MaxSecretBytes || InBytes.GetAllocatedSize() > MaxSecretBytes * 2)
	{
		volatile uint8 *Wipe = InBytes.GetData();
		for (int32 Index = 0; Wipe != nullptr && Index < InBytes.Max(); ++Index)
		{
			Wipe[Index] = 0;
		}
		InBytes.Empty();
		OutError = TEXT("Secret value must be non-empty and within its physical byte bound.");
		return false;
	}
	OutValue.Bytes = MoveTemp(InBytes);
	return true;
}

bool FUnrealAISecretValue::IsSet() const
{
	return !Bytes.IsEmpty();
}

int32 FUnrealAISecretValue::Num() const
{
	return Bytes.Num();
}

FString FUnrealAISecretValue::GetRedactedDisplay() const
{
	return IsSet() ? TEXT("<redacted:credential>") : TEXT("<unset:credential>");
}

void FUnrealAISecretValue::Reset()
{
	volatile uint8 *Wipe = Bytes.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Bytes.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Bytes.Empty();
}

TConstArrayView<uint8> FUnrealAISecretValue::View() const
{
	return MakeArrayView(Bytes);
}

bool FUnrealAIEndpointOrigin::TryParse(const FString &Input, const bool bAllowLoopbackHttp,
									   FUnrealAIEndpointOrigin &OutOrigin, FString &OutError)
{
	OutOrigin = FUnrealAIEndpointOrigin{};
	OutError.Reset();
	if (!FitsUtf8(Input, MaxOriginUtf8Bytes) || Input.IsEmpty() || Input.TrimStartAndEnd() != Input ||
		Input.Contains(TEXT("\\")) || Input.Contains(TEXT("@")) ||
													 Input.Contains(TEXT("?")) ||
																	Input.Contains(TEXT("#")) ||
																				   Input.Contains(TEXT("%")))
	{
		OutError = TEXT("Endpoint origin contains forbidden syntax or exceeds its bound.");
		return false;
	}

	const int32 SchemeSeparator = Input.Find(TEXT("://"), ESearchCase::CaseSensitive);
	if (SchemeSeparator <= 0)
	{
		OutError = TEXT("Endpoint origin requires an explicit HTTP scheme.");
		return false;
	}
	FString Scheme = Input.Left(SchemeSeparator).ToLower();
	if (Scheme != TEXT("https") && Scheme != TEXT("http"))
	{
		OutError = TEXT("Endpoint origin scheme must be HTTPS or approved loopback HTTP.");
		return false;
	}
	FString Authority = Input.Mid(SchemeSeparator + 3);
	if (Authority.EndsWith(TEXT("/")))
	{
		Authority.LeftChopInline(1, EAllowShrinking::No);
	}
	if (Authority.IsEmpty() || Authority.Contains(TEXT("/")))
	{
		OutError = TEXT("Endpoint origin must not contain a path.");
		return false;
	}

	FString Host;
	FString PortText;
	if (Authority.StartsWith(TEXT("[")))
	{
		int32 ClosingBracket = INDEX_NONE;
		if (!Authority.FindChar(TEXT(']'), ClosingBracket))
		{
			OutError = TEXT("Endpoint origin has malformed IPv6 authority.");
			return false;
		}
		Host = Authority.Left(ClosingBracket + 1).ToLower();
		const FString Remainder = Authority.Mid(ClosingBracket + 1);
		if (!Remainder.IsEmpty())
		{
			if (!Remainder.StartsWith(TEXT(":")) || Remainder.Len() == 1)
			{
				OutError = TEXT("Endpoint origin has malformed port syntax.");
				return false;
			}
			PortText = Remainder.Mid(1);
		}
	}
	else
	{
		int32 Colon = INDEX_NONE;
		if (Authority.FindLastChar(TEXT(':'), Colon))
		{
			Host = Authority.Left(Colon).ToLower();
			PortText = Authority.Mid(Colon + 1);
			if (PortText.IsEmpty())
			{
				OutError = TEXT("Endpoint origin has an empty port.");
				return false;
			}
		}
		else
		{
			Host = Authority.ToLower();
		}
	}

	if ((!Host.StartsWith(TEXT("[")) && !IsValidDnsOrIpv4Host(Host)) ||
		 (Host.StartsWith(TEXT("[")) && !IsValidBracketedIpv6Host(Host)))
	{
		OutError = TEXT("Endpoint origin has an invalid ASCII host.");
		return false;
	}
	int32 Port = Scheme == TEXT("https") ? 443 : 80;
	if (!PortText.IsEmpty() && !TryParsePort(PortText, Port))
	{
		OutError = TEXT("Endpoint origin port must be between 1 and 65535.");
		return false;
	}
	const bool bHostIsLoopback = IsLoopbackHost(Host);
	if (Scheme == TEXT("http") && (!bAllowLoopbackHttp || !bHostIsLoopback))
	{
		OutError = TEXT("Plaintext endpoint origins are restricted to explicitly enabled loopback development.");
		return false;
	}

	const bool bDefaultPort = (Scheme == TEXT("https") && Port == 443) || (Scheme == TEXT("http") && Port == 80);
	OutOrigin.CanonicalOrigin = FString::Printf(TEXT("%s://%s%s"), *Scheme, *Host,
													 bDefaultPort ? TEXT("") : *FString::Printf(TEXT(":%d"), Port));
	OutOrigin.bLoopbackHost = bHostIsLoopback;
	OutOrigin.bLoopbackDevelopmentOnly = Scheme == TEXT("http");
	return true;
}

bool FUnrealAIEndpointOrigin::IsValid() const
{
	return !CanonicalOrigin.IsEmpty();
}

bool FUnrealAIEndpointOrigin::IsSecure() const
{
	return CanonicalOrigin.StartsWith(TEXT("https://"));
}

bool FUnrealAIEndpointOrigin::HasLoopbackHost() const
{
	return IsValid() && bLoopbackHost;
}

bool FUnrealAIEndpointOrigin::IsLoopbackDevelopmentOnly() const
{
	return IsValid() && bLoopbackDevelopmentOnly;
}

const FString &FUnrealAIEndpointOrigin::ToString() const
{
	return CanonicalOrigin;
}

bool FUnrealAICredentialDestination::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableIdentifier(ModelProviderName) || !IsStableIdentifier(TenantRealm) || ConnectionRevision == 0 ||
		EndpointPolicyRevision == 0 || !AuthBillingPairIsValid(AuthScheme, BillingMode))
	{
		OutError =
			TEXT("Credential destination has an invalid provider, endpoint origin, auth scheme, or billing mode.");
		return false;
	}
	if (AuthScheme == EUnrealAIAuthScheme::Anonymous)
	{
		if (EndpointOrigin.IsValid() || !AccountAuthProviderName.IsNone() || !AuthProfileId.IsNone() ||
			AccountId.IsValid() || BillingPrincipalId.IsValid() || !PayerHandle.IsNone() || !Audience.IsEmpty())
		{
			OutError = TEXT("Anonymous local destinations cannot select an auth profile, account, payer, or audience.");
			return false;
		}
		return true;
	}
	if (!EndpointOrigin.IsValid() || !IsStableIdentifier(AccountAuthProviderName) ||
		!IsStableIdentifier(AuthProfileId) || !AccountId.IsValid() || !BillingPrincipalId.IsValid() ||
		!IsStableIdentifier(PayerHandle) || Audience.IsEmpty() || !FitsUtf8(Audience, MaxAudienceUtf8Bytes) ||
		Audience.TrimStartAndEnd() != Audience)
	{
		OutError = TEXT(
			"Credentialed destinations require bounded account, tenant, billing principal, payer, and audience bindings.");
		return false;
	}
	if ((AuthScheme == EUnrealAIAuthScheme::OAuthBearer || AuthScheme == EUnrealAIAuthScheme::GatewayBearer) &&
		!EndpointOrigin.IsSecure())
	{
		OutError = TEXT("Bearer credentials require a secure HTTPS endpoint origin.");
		return false;
	}
	return true;
}

FName FUnrealAIEndpointProfileDescriptor::GetProfileId() const
{
	return ProfileId;
}

FName FUnrealAIEndpointProfileDescriptor::GetModelProviderName() const
{
	return ModelProviderName;
}

FName FUnrealAIEndpointProfileDescriptor::GetAuthProviderName() const
{
	return AuthProviderName;
}

EUnrealAIEndpointProfileClass FUnrealAIEndpointProfileDescriptor::GetProfileClass() const
{
	return ProfileClass;
}

EUnrealAIAuthScheme FUnrealAIEndpointProfileDescriptor::GetAuthScheme() const
{
	return AuthScheme;
}

EUnrealAIBillingMode FUnrealAIEndpointProfileDescriptor::GetBillingMode() const
{
	return BillingMode;
}

const FUnrealAIEndpointOrigin &FUnrealAIEndpointProfileDescriptor::GetOrigin() const
{
	return Origin;
}

const FString &FUnrealAIEndpointProfileDescriptor::GetAudience() const
{
	return Audience;
}

uint64 FUnrealAIEndpointProfileDescriptor::GetPolicyRevision() const
{
	return PolicyRevision;
}

bool FUnrealAIEndpointProfileDescriptor::MatchesDestination(const FUnrealAICredentialDestination &Destination) const
{
	const bool bAuthProviderMatches = ProfileClass != EUnrealAIEndpointProfileClass::ProviderSubscriptionResource ||
									  AuthProviderName == Destination.AccountAuthProviderName;
	return bAuthProviderMatches && ModelProviderName == Destination.ModelProviderName &&
		   AuthScheme == Destination.AuthScheme && BillingMode == Destination.BillingMode &&
		   Origin == Destination.EndpointOrigin && Audience == Destination.Audience &&
		   PolicyRevision == Destination.EndpointPolicyRevision;
}

bool FUnrealAIEndpointProfileDescriptor::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableIdentifier(ProfileId) || !IsStableIdentifier(ModelProviderName) || PolicyRevision == 0)
	{
		OutError = TEXT("Endpoint profile has invalid identifiers or policy revision.");
		return false;
	}
	if (ProfileClass == EUnrealAIEndpointProfileClass::LocalInProcess)
	{
		if (!AuthProviderName.IsNone() || AuthScheme != EUnrealAIAuthScheme::Anonymous ||
			BillingMode != EUnrealAIBillingMode::Local || Origin.IsValid() || !Audience.IsEmpty())
		{
			OutError = TEXT("Local in-process profile requires anonymous local access without a network origin.");
			return false;
		}
		return true;
	}
	if (!Origin.IsValid() || Audience.IsEmpty() ||
		!FitsUtf8(Audience, FUnrealAICredentialDestination::MaxAudienceUtf8Bytes) ||
		Audience.TrimStartAndEnd() != Audience)
	{
		OutError = TEXT("Network endpoint profile has an invalid origin or audience.");
		return false;
	}
	switch (ProfileClass)
	{
	case EUnrealAIEndpointProfileClass::LocalInProcess:
		checkNoEntry();
		break;
	case EUnrealAIEndpointProfileClass::CustomApi:
		if (!AuthProviderName.IsNone() || AuthScheme != EUnrealAIAuthScheme::ApiKey ||
			BillingMode != EUnrealAIBillingMode::ApiMetered || !Origin.IsSecure())
		{
			OutError = TEXT("Custom API endpoint profile requires secure API-key metered access.");
			return false;
		}
		break;
	case EUnrealAIEndpointProfileClass::LocalLoopbackDevelopment:
		if (!AuthProviderName.IsNone() || AuthScheme != EUnrealAIAuthScheme::ApiKey ||
			BillingMode != EUnrealAIBillingMode::ApiMetered || !Origin.IsLoopbackDevelopmentOnly())
		{
			OutError = TEXT("Local development profile requires an explicit loopback HTTP API-key origin.");
			return false;
		}
		break;
	case EUnrealAIEndpointProfileClass::ProjectGateway:
		if (!AuthProviderName.IsNone() || AuthScheme != EUnrealAIAuthScheme::GatewayBearer ||
			BillingMode != EUnrealAIBillingMode::GatewayAccounted || !Origin.IsSecure())
		{
			OutError = TEXT("Project gateway profile requires a secure gateway bearer destination.");
			return false;
		}
		break;
	case EUnrealAIEndpointProfileClass::ProviderSubscriptionResource:
		if (!IsStableIdentifier(AuthProviderName) || AuthScheme != EUnrealAIAuthScheme::OAuthBearer ||
			BillingMode != EUnrealAIBillingMode::SubscriptionQuota || !Origin.IsSecure() || Origin.HasLoopbackHost())
		{
			OutError = TEXT("Provider subscription profile requires provider-authorized secure OAuth access.");
			return false;
		}
		break;
	case EUnrealAIEndpointProfileClass::Invalid:
	default:
		OutError = TEXT("Endpoint profile has an unknown trust class.");
		return false;
	}
	return true;
}

FUnrealAICredentialFreshnessToken::FUnrealAICredentialFreshnessToken(
	TSharedPtr<UE::UnrealAI::Private::FCredentialFreshnessState, ESPMode::ThreadSafe> InState,
	const uint64 InGeneration)
	: State(MoveTemp(InState)), Generation(InGeneration)
{
}

bool FUnrealAICredentialFreshnessToken::IsValid() const
{
	return State.IsValid() && Generation != 0;
}

bool FUnrealAICredentialFreshnessToken::IsCurrent() const
{
	if (!IsValid() || UE::UnrealAI::Private::GCredentialDispatchDepth > 0 ||
		State->bShutdownRequested.load(std::memory_order_acquire) ||
		State->bRevocationRequested.load(std::memory_order_acquire))
	{
		return false;
	}
	FReadScopeLock Permit(State->Lock);
	return !State->bShutdown && !State->bShutdownRequested.load(std::memory_order_acquire) &&
		   !State->bRevocationRequested.load(std::memory_order_acquire) && State->Generation == Generation;
}

FUnrealAICredentialFreshnessSource::FUnrealAICredentialFreshnessSource()
	: State(MakeShared<UE::UnrealAI::Private::FCredentialFreshnessState, ESPMode::ThreadSafe>())
{
}

FUnrealAICredentialFreshnessSource::~FUnrealAICredentialFreshnessSource()
{
	if (!BeginShutdown())
	{
		State->bRevocationRequested.store(true, std::memory_order_release);
	}
}

FUnrealAICredentialFreshnessToken FUnrealAICredentialFreshnessSource::GetToken() const
{
	if (UE::UnrealAI::Private::GCredentialDispatchDepth > 0 ||
		State->bShutdownRequested.load(std::memory_order_acquire) ||
		State->bRevocationRequested.load(std::memory_order_acquire))
	{
		return {};
	}
	FReadScopeLock Permit(State->Lock);
	if (State->bShutdown || State->bShutdownRequested.load(std::memory_order_acquire) ||
		State->bRevocationRequested.load(std::memory_order_acquire))
	{
		return {};
	}
	return FUnrealAICredentialFreshnessToken(State.ToSharedPtr(), State->Generation);
}

bool FUnrealAICredentialFreshnessSource::Invalidate()
{
	if (State->bShutdownRequested.load(std::memory_order_acquire))
	{
		return false;
	}
	State->bRevocationRequested.store(true, std::memory_order_release);
	if (UE::UnrealAI::Private::GCredentialDispatchDepth > 0)
	{
		return false;
	}
	if (!State->Lock.TryWriteLock())
	{
		return false;
	}
	if (State->bShutdown)
	{
		State->Lock.WriteUnlock();
		return false;
	}
	if (State->Generation == TNumericLimits<uint64>::Max())
	{
		State->bShutdown = true;
		State->Lock.WriteUnlock();
		return false;
	}
	++State->Generation;
	if (!State->bShutdownRequested.load(std::memory_order_acquire))
	{
		State->bRevocationRequested.store(false, std::memory_order_release);
	}
	State->Lock.WriteUnlock();
	return true;
}

bool FUnrealAICredentialFreshnessSource::BeginShutdown()
{
	State->bShutdownRequested.store(true, std::memory_order_release);
	State->bRevocationRequested.store(true, std::memory_order_release);
	if (UE::UnrealAI::Private::GCredentialDispatchDepth > 0)
	{
		return false;
	}
	if (!State->Lock.TryWriteLock())
	{
		return false;
	}
	const bool bChanged = !State->bShutdown;
	State->bShutdown = true;
	State->Lock.WriteUnlock();
	return bChanged;
}

bool FUnrealAICredentialFreshnessSource::IsShutdown() const
{
	if (UE::UnrealAI::Private::GCredentialDispatchDepth > 0 ||
		State->bShutdownRequested.load(std::memory_order_acquire) ||
		State->bRevocationRequested.load(std::memory_order_acquire))
	{
		return true;
	}
	FReadScopeLock Permit(State->Lock);
	return State->bShutdown || State->bShutdownRequested.load(std::memory_order_acquire) ||
		   State->bRevocationRequested.load(std::memory_order_acquire);
}

FUnrealAICredentialLease::~FUnrealAICredentialLease()
{
	Reset();
}

FUnrealAICredentialLease::FUnrealAICredentialLease(FUnrealAICredentialLease &&Other) noexcept
	: Binding(MoveTemp(Other.Binding)), Clock(MoveTemp(Other.Clock)), Freshness(MoveTemp(Other.Freshness)),
	  UsableUntil(Other.UsableUntil), CredentialExpiresAtUtc(MoveTemp(Other.CredentialExpiresAtUtc)),
	  Secret(MoveTemp(Other.Secret)), ProtectedSecondary(MoveTemp(Other.ProtectedSecondary)),
	  ReleaseCallback(MoveTemp(Other.ReleaseCallback))
{
	Other.Binding = FUnrealAICredentialDestination{};
	Other.UsableUntil = FUnrealAIDeadline{};
	Other.CredentialExpiresAtUtc.Reset();
}

FUnrealAICredentialLease &FUnrealAICredentialLease::operator=(FUnrealAICredentialLease &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Binding = MoveTemp(Other.Binding);
		Clock = MoveTemp(Other.Clock);
		Freshness = MoveTemp(Other.Freshness);
		UsableUntil = Other.UsableUntil;
		CredentialExpiresAtUtc = MoveTemp(Other.CredentialExpiresAtUtc);
		this->Secret = MoveTemp(Other.Secret);
		ProtectedSecondary = MoveTemp(Other.ProtectedSecondary);
		ReleaseCallback = MoveTemp(Other.ReleaseCallback);
		Other.Binding = FUnrealAICredentialDestination{};
		Other.UsableUntil = FUnrealAIDeadline{};
		Other.CredentialExpiresAtUtc.Reset();
	}
	return *this;
}

bool FUnrealAICredentialLease::TryCreate(const FUnrealAICredentialDestination &InBinding,
										 TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
										 const FUnrealAICredentialFreshnessToken &InFreshness,
										 const double LeaseLifetimeSeconds,
										 TOptional<FDateTime> InCredentialExpiresAtUtc, FUnrealAISecretValue &&InSecret,
										 FUnrealAICredentialLease &OutLease, FString &OutError,
										 TUniqueFunction<void()> InReleaseCallback)
{
	OutError.Reset();
	OutLease.Reset();
	FString BindingError;
	if (!InBinding.ValidateShape(BindingError) || InBinding.AuthScheme == EUnrealAIAuthScheme::Anonymous ||
		!InFreshness.IsCurrent())
	{
		InSecret.Reset();
		OutError = !BindingError.IsEmpty() ? BindingError
										   : (InBinding.AuthScheme == EUnrealAIAuthScheme::Anonymous
											  ? TEXT("Anonymous destinations do not require credential leases.")
											  : TEXT("Credential lease requires a current broker freshness token."));
		return false;
	}
	if (!InSecret.IsSet())
	{
		OutError = TEXT("Credential lease requires non-empty secret material.");
		return false;
	}
	if (!FMath::IsFinite(LeaseLifetimeSeconds) || LeaseLifetimeSeconds <= 0.0 ||
		LeaseLifetimeSeconds > MaxLeaseLifetimeSeconds || !FMath::IsFinite(InClock->MonotonicSeconds()) ||
		InClock->MonotonicSeconds() < 0.0)
	{
		InSecret.Reset();
		OutError = TEXT("Credential lease requires a positive bounded lifetime and valid monotonic clock.");
		return false;
	}
	if ((InBinding.AuthScheme == EUnrealAIAuthScheme::OAuthBearer ||
		 InBinding.AuthScheme == EUnrealAIAuthScheme::GatewayBearer) &&
		!InCredentialExpiresAtUtc.IsSet())
	{
		InSecret.Reset();
		OutError = TEXT("Bearer credential leases require an explicit expiry.");
		return false;
	}
	double EffectiveLifetimeSeconds = LeaseLifetimeSeconds;
	if (InCredentialExpiresAtUtc.IsSet())
	{
		const double CredentialRemainingSeconds =
			(InCredentialExpiresAtUtc.GetValue() - InClock->UtcNow()).GetTotalSeconds();
		if (!FMath::IsFinite(CredentialRemainingSeconds) || CredentialRemainingSeconds <= 0.0)
		{
			InSecret.Reset();
			OutError = TEXT("Credential lease rejected already-expired credential material.");
			return false;
		}
		EffectiveLifetimeSeconds = FMath::Min(EffectiveLifetimeSeconds, CredentialRemainingSeconds);
	}
	OutLease.Binding = InBinding;
	OutLease.Clock = InClock.ToSharedPtr();
	OutLease.Freshness = InFreshness;
	OutLease.UsableUntil = FUnrealAIDeadline::FromNow(*InClock, EffectiveLifetimeSeconds);
	OutLease.CredentialExpiresAtUtc = InCredentialExpiresAtUtc;
	OutLease.Secret = MoveTemp(InSecret);
	OutLease.ReleaseCallback = MoveTemp(InReleaseCallback);
	return true;
}

bool FUnrealAICredentialLease::TryCreateWithProtectedSecondary(
	const FUnrealAICredentialDestination &InBinding, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	const FUnrealAICredentialFreshnessToken &InFreshness, const double LeaseLifetimeSeconds,
	TOptional<FDateTime> InCredentialExpiresAtUtc, FUnrealAISecretValue &&InSecret,
	FUnrealAISecretValue &&InProtectedSecondary, FUnrealAICredentialLease &OutLease, FString &OutError,
	TUniqueFunction<void()> InReleaseCallback)
{
	OutLease.Reset();
	OutError.Reset();
	if (InBinding.AuthScheme != EUnrealAIAuthScheme::OAuthBearer ||
		InBinding.BillingMode != EUnrealAIBillingMode::SubscriptionQuota || !InProtectedSecondary.IsSet())
	{
		InSecret.Reset();
		InProtectedSecondary.Reset();
		OutError = TEXT("Protected secondary credentials require a non-empty value bound to an OAuth subscription destination.");
		return false;
	}
	if (!TryCreate(InBinding, MoveTemp(InClock), InFreshness, LeaseLifetimeSeconds, InCredentialExpiresAtUtc,
				   MoveTemp(InSecret), OutLease, OutError, MoveTemp(InReleaseCallback)))
	{
		InProtectedSecondary.Reset();
		return false;
	}
	OutLease.ProtectedSecondary = MoveTemp(InProtectedSecondary);
	return true;
}

bool FUnrealAICredentialLease::IsValid() const
{
	FString Error;
	return Secret.IsSet() && Clock.IsValid() && Freshness.IsCurrent() && Binding.ValidateShape(Error) &&
		   Binding.AuthScheme != EUnrealAIAuthScheme::Anonymous;
}

bool FUnrealAICredentialLease::IsExpired() const
{
	return !Clock.IsValid() || UsableUntil.IsExpired(*Clock);
}

const FUnrealAICredentialDestination &FUnrealAICredentialLease::GetBinding() const
{
	return Binding;
}

TOptional<FDateTime> FUnrealAICredentialLease::GetCredentialExpiryUtc() const
{
	return CredentialExpiresAtUtc;
}

FString FUnrealAICredentialLease::GetRedactedDisplay() const
{
	return Secret.GetRedactedDisplay();
}

void FUnrealAICredentialLease::Reset()
{
	TUniqueFunction<void()> Callback = MoveTemp(ReleaseCallback);
	Secret.Reset();
	ProtectedSecondary.Reset();
	Binding = FUnrealAICredentialDestination{};
	Clock.Reset();
	Freshness = FUnrealAICredentialFreshnessToken{};
	UsableUntil = FUnrealAIDeadline{};
	CredentialExpiresAtUtc.Reset();
	if (Callback)
	{
		Callback();
	}
}

bool FUnrealAICredentialLease::TryApplyTo(IUnrealAICredentialApplicator &Applicator, FString &OutError)
{
	OutError.Reset();
	const FUnrealAICredentialDestination &RequestedDestination = Applicator.GetActualDestination();
	FString DestinationError;
	if (!RequestedDestination.ValidateShape(DestinationError))
	{
		OutError = TEXT("Credential materialization rejected an invalid destination.");
		return false;
	}
	FString LeaseError;
	if (!Secret.IsSet() || !Clock.IsValid() || !Binding.ValidateShape(LeaseError))
	{
		OutError = TEXT("Credential materialization rejected an invalid lease.");
		return false;
	}
	if (!Freshness.IsCurrent())
	{
		OutError = TEXT("Credential materialization rejected a revoked lease.");
		Reset();
		return false;
	}
	if (IsExpired())
	{
		OutError = TEXT("Credential materialization rejected an expired lease.");
		Reset();
		return false;
	}
	if (!CredentialDestinationEqual(Binding, RequestedDestination))
	{
		OutError = TEXT("Credential materialization rejected a destination binding mismatch.");
		return false;
	}
	const TSharedPtr<UE::UnrealAI::Private::FCredentialFreshnessState, ESPMode::ThreadSafe> FreshnessState =
		Freshness.State;
	FReadScopeLock FreshnessPermit(FreshnessState->Lock);
	if (FreshnessState->bShutdown || FreshnessState->bShutdownRequested.load(std::memory_order_acquire) ||
		FreshnessState->bRevocationRequested.load(std::memory_order_acquire) ||
		FreshnessState->Generation != Freshness.Generation)
	{
		OutError = TEXT("Credential materialization rejected a revoked lease.");
		Reset();
		return false;
	}
	if (IsExpired())
	{
		OutError = TEXT("Credential materialization rejected an expired lease.");
		Reset();
		return false;
	}
	const EUnrealAIAuthScheme BoundScheme = Binding.AuthScheme;
	FUnrealAISecretValue OneShotSecret = MoveTemp(Secret);
	FUnrealAISecretValue OneShotProtectedSecondary = MoveTemp(ProtectedSecondary);
	Reset();
	const UE::UnrealAI::Private::FCredentialDispatchThreadScope DispatchScope;
	const bool bApplied = OneShotProtectedSecondary.IsSet()
							  ? Applicator.ApplyCredentialAndProtectedSecondaryAndDispatch(
									BoundScheme, OneShotSecret.View(), OneShotProtectedSecondary.View())
							  : Applicator.ApplyCredentialAndDispatch(BoundScheme, OneShotSecret.View());
	if (!bApplied)
	{
		OutError = TEXT("Credential transport dispatcher rejected the bound credential.");
		return false;
	}
	return true;
}

namespace
{
class FCredentialedProviderAccessContext final : public IUnrealAIProviderAccessContext
{
  public:
	explicit FCredentialedProviderAccessContext(FUnrealAICredentialLease &&InLease) : Lease(MoveTemp(InLease)) {}

	bool IsValid() const override
	{
		if (bClaimed.Load() || !Mutex.TryLock())
		{
			return false;
		}
		const bool bValid = !bClaimed.Load() && Lease.IsValid() && !Lease.IsExpired();
		Mutex.Unlock();
		return bValid;
	}
	bool RequiresCredential() const override
	{
		return true;
	}
	bool TryDispatch(IUnrealAICredentialApplicator &Dispatcher, FString &OutError) const override
	{
		if (bClaimed.Load())
		{
			OutError = TEXT("Provider access context rejected a duplicate dispatch.");
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (bClaimed.Load())
		{
			OutError = TEXT("Provider access context rejected a duplicate dispatch.");
			return false;
		}
		FString DestinationError;
		const FUnrealAICredentialDestination &Destination = Dispatcher.GetActualDestination();
		if (!Destination.ValidateShape(DestinationError) ||
			!CredentialDestinationEqual(Lease.GetBinding(), Destination))
		{
			OutError = TEXT("Credential materialization rejected a destination binding mismatch.");
			return false;
		}
		bClaimed.Store(true);
		FUnrealAICredentialLease OneShotLease = MoveTemp(Lease);
		return OneShotLease.TryApplyTo(Dispatcher, OutError);
	}
	FString GetRedactedDisplay() const override
	{
		return IsValid() ? TEXT("<redacted:provider-access>") : TEXT("<unset:provider-access>");
	}

  private:
	mutable FCriticalSection Mutex;
	mutable TAtomic<bool> bClaimed{false};
	mutable FUnrealAICredentialLease Lease;
};

class FAnonymousProviderAccessContext final : public IUnrealAIProviderAccessContext
{
  public:
	FAnonymousProviderAccessContext(FUnrealAICredentialDestination InBinding,
									FUnrealAICredentialFreshnessToken InFreshness)
		: Binding(MoveTemp(InBinding)), Freshness(MoveTemp(InFreshness))
	{
	}

	bool IsValid() const override
	{
		return !bConsumed.Load() && Freshness.IsCurrent();
	}
	bool RequiresCredential() const override
	{
		return false;
	}
	bool TryDispatch(IUnrealAICredentialApplicator &Dispatcher, FString &OutError) const override
	{
		OutError.Reset();
		if (bConsumed.Exchange(true))
		{
			OutError = TEXT("Anonymous provider access context rejected a duplicate dispatch.");
			return false;
		}
		FString DestinationError;
		const FUnrealAICredentialDestination &Destination = Dispatcher.GetActualDestination();
		if (!Destination.ValidateShape(DestinationError) || !CredentialDestinationEqual(Binding, Destination))
		{
			OutError = TEXT("Anonymous provider access rejected a destination binding mismatch.");
			return false;
		}
		if (!Freshness.IsCurrent())
		{
			OutError = TEXT("Anonymous provider access context rejected a stale route.");
			return false;
		}
		return DispatchAnonymousWhileCurrent(Freshness, Dispatcher, OutError);
	}
	FString GetRedactedDisplay() const override
	{
		return IsValid() ? TEXT("<anonymous:provider-access>") : TEXT("<unset:provider-access>");
	}

  private:
	FUnrealAICredentialDestination Binding;
	FUnrealAICredentialFreshnessToken Freshness;
	mutable TAtomic<bool> bConsumed{false};
};
} // namespace

bool IUnrealAIProviderAccessContext::DispatchAnonymousWhileCurrent(const FUnrealAICredentialFreshnessToken &Freshness,
																   IUnrealAICredentialApplicator &Dispatcher,
																   FString &OutError)
{
	if (!Freshness.IsValid() || UE::UnrealAI::Private::GCredentialDispatchDepth > 0 ||
		Freshness.State->bShutdownRequested.load(std::memory_order_acquire) ||
		Freshness.State->bRevocationRequested.load(std::memory_order_acquire))
	{
		OutError = TEXT("Anonymous provider access context rejected a stale route.");
		return false;
	}
	FReadScopeLock Permit(Freshness.State->Lock);
	if (Freshness.State->bShutdown || Freshness.State->bShutdownRequested.load(std::memory_order_acquire) ||
		Freshness.State->bRevocationRequested.load(std::memory_order_acquire) ||
		Freshness.State->Generation != Freshness.Generation)
	{
		OutError = TEXT("Anonymous provider access context rejected a stale route.");
		return false;
	}
	const UE::UnrealAI::Private::FCredentialDispatchThreadScope DispatchScope;
	if (!DispatchAnonymous(Dispatcher))
	{
		OutError = TEXT("Anonymous transport dispatcher rejected provider access.");
		return false;
	}
	return true;
}

TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe>
IUnrealAIProviderAccessContext::CreateCredentialed(FUnrealAICredentialLease &&Lease)
{
	if (!Lease.IsValid() || Lease.IsExpired())
	{
		Lease.Reset();
		return nullptr;
	}
	return MakeShared<FCredentialedProviderAccessContext, ESPMode::ThreadSafe>(MoveTemp(Lease));
}

TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe>
IUnrealAIProviderAccessContext::CreateAnonymous(const FUnrealAICredentialDestination &Binding,
												const FUnrealAICredentialFreshnessToken &Freshness)
{
	FString Error;
	if (!Binding.ValidateShape(Error) || Binding.AuthScheme != EUnrealAIAuthScheme::Anonymous ||
		Binding.BillingMode != EUnrealAIBillingMode::Local || !Freshness.IsCurrent())
	{
		return nullptr;
	}
	return MakeShared<FAnonymousProviderAccessContext, ESPMode::ThreadSafe>(Binding, Freshness);
}

bool FUnrealAIConnectionDescriptor::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (SchemaVersion != CurrentSchemaVersion || ConnectionRevision == 0 ||
		ConnectionRevision != CredentialDestination.ConnectionRevision || !IsStableIdentifier(ConnectionAlias) ||
		!IsStableIdentifier(EndpointProfileId))
	{
		OutError = TEXT("Connection descriptor has an unsupported schema or invalid stable alias/profile.");
		return false;
	}
	if (!CredentialDestination.ValidateShape(OutError))
	{
		return false;
	}
	return true;
}

bool FUnrealAIConnectionDescriptor::IsSubscriptionConnection() const
{
	return CredentialDestination.BillingMode == EUnrealAIBillingMode::SubscriptionQuota;
}

bool FUnrealAIAccountStatus::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableIdentifier(ProviderName) || !IsStableIdentifier(AuthProfileId) || !IsKnownAccountState(State))
	{
		OutError = TEXT("Account status has an invalid provider, auth profile, or state.");
		return false;
	}
	if (State == EUnrealAIAccountAuthState::SignedOut)
	{
		if (AccountId.IsValid() || AccessExpiresAtUtc.IsSet() || bRefreshCredentialPresent)
		{
			OutError = TEXT("Signed-out account status cannot retain account or credential metadata.");
			return false;
		}
	}
	else if ((State == EUnrealAIAccountAuthState::Ready || State == EUnrealAIAccountAuthState::Refreshing) &&
			 !AccountId.IsValid())
	{
		OutError = TEXT("Ready account status requires an opaque account handle.");
		return false;
	}
	return true;
}

bool FUnrealAIProviderAccessDescriptor::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const bool bKnownAvailability = Availability >= EUnrealAIProviderAccessAvailability::Available &&
									Availability <= EUnrealAIProviderAccessAvailability::Unavailable;
	const bool bKnownSupport =
		SupportClassification >= EUnrealAIProviderAccessSupportClassification::Supported &&
		SupportClassification <= EUnrealAIProviderAccessSupportClassification::ImplementationCandidate;
	if (!IsStableIdentifier(ModelProviderName) || !bKnownAvailability || !bKnownSupport ||
		!AuthBillingPairIsValid(AuthScheme, BillingMode))
	{
		OutError =
			TEXT("Provider access descriptor has an invalid provider, availability, support, auth, or billing mode.");
		return false;
	}
	if ((AuthScheme == EUnrealAIAuthScheme::Anonymous) != AccountAuthProviderName.IsNone())
	{
		OutError = TEXT("Credentialed provider access requires a stable account-auth provider.");
		return false;
	}
	if (AuthScheme != EUnrealAIAuthScheme::Anonymous && !IsStableIdentifier(AccountAuthProviderName))
	{
		OutError = TEXT("Credentialed provider access requires a stable account-auth provider.");
		return false;
	}
	if (Availability == EUnrealAIProviderAccessAvailability::PartnerGated &&
		(AuthScheme != EUnrealAIAuthScheme::OAuthBearer || BillingMode != EUnrealAIBillingMode::SubscriptionQuota))
	{
		OutError = TEXT("Partner-gated access is reserved for approved subscription OAuth resources.");
		return false;
	}
	if (SupportClassification ==
			EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility &&
		(AuthScheme != EUnrealAIAuthScheme::OAuthBearer || BillingMode != EUnrealAIBillingMode::SubscriptionQuota ||
		 Availability == EUnrealAIProviderAccessAvailability::PartnerGated))
	{
		OutError = TEXT(
			"Experimental direct-subscription compatibility requires a non-partner-gated subscription OAuth route.");
		return false;
	}
	if (SupportClassification == EUnrealAIProviderAccessSupportClassification::ImplementationCandidate &&
		(AuthScheme != EUnrealAIAuthScheme::ApiKey || BillingMode != EUnrealAIBillingMode::ApiMetered))
	{
		OutError =
			TEXT("Implementation-candidate access is reserved for public API-key routes awaiting support evidence.");
		return false;
	}
	return true;
}

bool FUnrealAISecretStoreCapabilities::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	const bool bKnownPersistence = PersistenceClass >= EUnrealAISecretStorePersistenceClass::Volatile &&
								   PersistenceClass <= EUnrealAISecretStorePersistenceClass::Persistent;
	const bool bKnownProtection = ProtectionClass >= EUnrealAISecretStoreProtectionClass::ProcessMemory &&
								  ProtectionClass <= EUnrealAISecretStoreProtectionClass::ExternalSecretService;
	const bool bKnownScope =
		ScopeClass >= EUnrealAISecretStoreScopeClass::Process && ScopeClass <= EUnrealAISecretStoreScopeClass::Service;

	if (!bAvailableInCurrentBuild)
	{
		if (PersistenceClass != EUnrealAISecretStorePersistenceClass::Invalid ||
			ProtectionClass != EUnrealAISecretStoreProtectionClass::Invalid ||
			ScopeClass != EUnrealAISecretStoreScopeClass::Invalid || bPersistent || bHardwareBackedWhenAvailable ||
			bAtomicCompareAndSwap || bAvailableInShipping)
		{
			OutError = TEXT("Unavailable secret-store capabilities cannot advertise storage guarantees.");
			return false;
		}
		return true;
	}

	if (!bKnownPersistence || !bKnownProtection || !bKnownScope ||
		bPersistent != (PersistenceClass == EUnrealAISecretStorePersistenceClass::Persistent))
	{
		OutError = TEXT("Secret-store capabilities have an invalid or inconsistent classification.");
		return false;
	}
	if ((ProtectionClass == EUnrealAISecretStoreProtectionClass::ProcessMemory) !=
		(ScopeClass == EUnrealAISecretStoreScopeClass::Process))
	{
		OutError = TEXT("Process-memory secret storage must have process scope and no wider store may use it.");
		return false;
	}
	if (PersistenceClass == EUnrealAISecretStorePersistenceClass::Volatile &&
		ProtectionClass != EUnrealAISecretStoreProtectionClass::ProcessMemory)
	{
		OutError = TEXT("Volatile secret storage is restricted to the process-memory protection class.");
		return false;
	}
	if (PersistenceClass == EUnrealAISecretStorePersistenceClass::Persistent &&
		ProtectionClass == EUnrealAISecretStoreProtectionClass::ProcessMemory)
	{
		OutError = TEXT("Process-memory secret storage cannot advertise persistent storage.");
		return false;
	}
	if (ProtectionClass == EUnrealAISecretStoreProtectionClass::PlatformCredentialStore &&
		ScopeClass != EUnrealAISecretStoreScopeClass::CurrentUser &&
		ScopeClass != EUnrealAISecretStoreScopeClass::LocalMachine)
	{
		OutError = TEXT("Platform credential stores require a current-user or local-machine protection scope.");
		return false;
	}
	if (ProtectionClass == EUnrealAISecretStoreProtectionClass::ExternalSecretService &&
		ScopeClass != EUnrealAISecretStoreScopeClass::Service)
	{
		OutError = TEXT("External secret services require service protection scope.");
		return false;
	}
	if (bHardwareBackedWhenAvailable && ProtectionClass != EUnrealAISecretStoreProtectionClass::PlatformCredentialStore)
	{
		OutError = TEXT("Hardware-backed capability requires a platform credential-store protection class.");
		return false;
	}
	return true;
}

bool FUnrealAISecretStoreCapabilities::IsProductionProtected() const
{
	FString IgnoredError;
	if (!ValidateShape(IgnoredError) || !bAvailableInCurrentBuild)
	{
		return false;
	}
	return (ProtectionClass == EUnrealAISecretStoreProtectionClass::PlatformCredentialStore ||
			ProtectionClass == EUnrealAISecretStoreProtectionClass::ExternalSecretService) &&
		   ScopeClass != EUnrealAISecretStoreScopeClass::Process &&
		   ScopeClass != EUnrealAISecretStoreScopeClass::Invalid;
}

bool FUnrealAISecretStoreOperationContext::TryCreate(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
													 const double TimeoutSeconds,
													 const FUnrealAICancellationToken &InCancellation,
													 FUnrealAISecretStoreOperationContext &OutContext,
													 FString &OutError)
{
	OutContext = FUnrealAISecretStoreOperationContext{};
	OutError.Reset();
	if (!InCancellation.IsValid() || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0 ||
		TimeoutSeconds > MaxTimeoutSeconds || !FMath::IsFinite(InClock->MonotonicSeconds()) ||
		InClock->MonotonicSeconds() < 0.0)
	{
		OutError = TEXT("Secret-store operation requires a valid cancellation token, clock, and bounded timeout.");
		return false;
	}
	OutContext.Clock = InClock.ToSharedPtr();
	OutContext.Deadline = FUnrealAIDeadline::FromNow(*InClock, TimeoutSeconds);
	OutContext.Cancellation = InCancellation;
	return true;
}

bool FUnrealAISecretStoreOperationContext::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!Clock.IsValid() || !Cancellation.IsValid() || !FMath::IsFinite(Deadline.AtMonotonicSeconds) ||
		Deadline.AtMonotonicSeconds < 0.0)
	{
		OutError = TEXT("Secret-store operation context has an invalid clock, deadline, or cancellation token.");
		return false;
	}
	return true;
}

bool FUnrealAISecretStoreOperationContext::IsCancellationRequested() const
{
	return Cancellation.IsCancellationRequested();
}

bool FUnrealAISecretStoreOperationContext::IsTimedOut() const
{
	return !Clock.IsValid() || Deadline.IsExpired(*Clock);
}

TConstArrayView<uint8> IUnrealAISecretStore::ViewSecret(const FUnrealAISecretValue &Value)
{
	return Value.View();
}

bool FUnrealAIAccountAuthRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!RequestId.IsValid() || !IsStableIdentifier(AuthProfileId) || !AccountId.IsValid() ||
		!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0f || TimeoutSeconds > MaxTimeoutSeconds)
	{
		OutError = TEXT("Account auth request requires valid IDs and a positive finite timeout.");
		return false;
	}
	return true;
}

bool FUnrealAIInteractiveAuthRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!RequestId.IsValid() || !IsStableIdentifier(AuthProfileId) ||
		(Flow != EUnrealAIInteractiveAuthFlow::BrowserPkce && Flow != EUnrealAIInteractiveAuthFlow::DeviceCode) ||
		!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0f || TimeoutSeconds > MaxTimeoutSeconds)
	{
		OutError =
			TEXT("Interactive auth request requires valid IDs, a supported flow, and a positive finite timeout.");
		return false;
	}
	return true;
}

FUnrealAIAuthInteraction::~FUnrealAIAuthInteraction()
{
	Reset();
}

FUnrealAIAuthInteraction::FUnrealAIAuthInteraction(FUnrealAIAuthInteraction &&Other) noexcept
	: ApprovedOrigin(MoveTemp(Other.ApprovedOrigin)), Kind(Other.Kind), LaunchUri(MoveTemp(Other.LaunchUri)),
	  UserCode(MoveTemp(Other.UserCode))
{
	Other.ApprovedOrigin = FUnrealAIEndpointOrigin{};
	Other.Kind = EUnrealAIAuthInteractionKind::Invalid;
}

FUnrealAIAuthInteraction &FUnrealAIAuthInteraction::operator=(FUnrealAIAuthInteraction &&Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		ApprovedOrigin = MoveTemp(Other.ApprovedOrigin);
		Kind = Other.Kind;
		LaunchUri = MoveTemp(Other.LaunchUri);
		UserCode = MoveTemp(Other.UserCode);
		Other.ApprovedOrigin = FUnrealAIEndpointOrigin{};
		Other.Kind = EUnrealAIAuthInteractionKind::Invalid;
	}
	return *this;
}

bool FUnrealAIAuthInteraction::TryCreateBrowserLaunch(const FUnrealAIEndpointOrigin &InApprovedInteractionOrigin,
													  FString &&InAuthorizationUri,
													  FUnrealAIAuthInteraction &OutInteraction, FString &OutError)
{
	OutInteraction.Reset();
	OutError.Reset();
	const bool bValid =
		InApprovedInteractionOrigin.IsSecure() && FitsUtf8(InAuthorizationUri, MaxVerificationUriUtf8Bytes) &&
		InAuthorizationUri.TrimStartAndEnd() == InAuthorizationUri && IsVisibleAscii(InAuthorizationUri) &&
		IsBoundToInteractionOrigin(InApprovedInteractionOrigin, InAuthorizationUri) &&
		!InAuthorizationUri.Contains(
			TEXT("@")) &&
			!InAuthorizationUri.Contains(
				TEXT("#")) &&
				!InAuthorizationUri.Contains(
					TEXT("\\")) && HasUnambiguousQuerySyntax(InAuthorizationUri) &&
					HasExactlyOneNonEmptyQueryParameter(InAuthorizationUri, TEXT("state")) &&
														HasExactlyOneNonEmptyQueryParameter(
															InAuthorizationUri, TEXT("code_challenge")) &&
															HasExactlyOneNonEmptyQueryParameter(
																InAuthorizationUri, TEXT("code_challenge_method"),
																						 FStringView(TEXT("S256")));
	if (!bValid)
	{
		WipeString(InAuthorizationUri);
		OutError = TEXT("Browser auth interaction requires an origin-bound PKCE S256 authorization URI.");
		return false;
	}
	OutInteraction.ApprovedOrigin = InApprovedInteractionOrigin;
	OutInteraction.Kind = EUnrealAIAuthInteractionKind::BrowserLaunch;
	OutInteraction.LaunchUri = MoveTemp(InAuthorizationUri);
	return true;
}

bool FUnrealAIAuthInteraction::TryCreateDeviceCode(const FUnrealAIEndpointOrigin &InApprovedInteractionOrigin,
												   FString &&InVerificationUri, FString &&InUserCode,
												   FUnrealAIAuthInteraction &OutInteraction, FString &OutError)
{
	OutInteraction.Reset();
	OutError.Reset();
	const bool bValid =
		InApprovedInteractionOrigin.IsSecure() && FitsUtf8(InVerificationUri, MaxVerificationUriUtf8Bytes) &&
		InVerificationUri.TrimStartAndEnd() == InVerificationUri && IsVisibleAscii(InVerificationUri) &&
		IsBoundToInteractionOrigin(InApprovedInteractionOrigin, InVerificationUri) &&
		!InVerificationUri.Contains(
			TEXT("@")) &&
			!InVerificationUri.Contains(
				TEXT("?")) &&
				!InVerificationUri.Contains(TEXT("#")) &&
											!InVerificationUri.Contains(TEXT("\\")) && !InUserCode.IsEmpty() &&
																		FitsUtf8(InUserCode, MaxUserCodeUtf8Bytes) &&
																		IsVisibleAscii(InUserCode);
	if (!bValid)
	{
		WipeString(InVerificationUri);
		WipeString(InUserCode);
		OutError = TEXT("Device auth interaction requires an origin-bound verification URI and printable user code.");
		return false;
	}
	OutInteraction.ApprovedOrigin = InApprovedInteractionOrigin;
	OutInteraction.Kind = EUnrealAIAuthInteractionKind::DeviceCode;
	OutInteraction.LaunchUri = MoveTemp(InVerificationUri);
	OutInteraction.UserCode = MoveTemp(InUserCode);
	return true;
}

bool FUnrealAIAuthInteraction::IsValid() const
{
	if (!ApprovedOrigin.IsSecure() || LaunchUri.IsEmpty() || !IsBoundToInteractionOrigin(ApprovedOrigin, LaunchUri))
	{
		return false;
	}
	if (Kind == EUnrealAIAuthInteractionKind::BrowserLaunch)
	{
		return UserCode.IsEmpty() && HasUnambiguousQuerySyntax(LaunchUri) &&
			   HasExactlyOneNonEmptyQueryParameter(LaunchUri, TEXT("state")) &&
												   HasExactlyOneNonEmptyQueryParameter(
													   LaunchUri, TEXT("code_challenge")) &&
													   HasExactlyOneNonEmptyQueryParameter(
														   LaunchUri, TEXT("code_challenge_method"),
																		   FStringView(TEXT("S256")));
	}
	return Kind == EUnrealAIAuthInteractionKind::DeviceCode && !UserCode.IsEmpty() && !LaunchUri.Contains(TEXT("?"));
}

EUnrealAIAuthInteractionKind FUnrealAIAuthInteraction::GetKind() const
{
	return Kind;
}

const FString &FUnrealAIAuthInteraction::GetLaunchUri() const
{
	return LaunchUri;
}

FStringView FUnrealAIAuthInteraction::GetUserCode() const
{
	return UserCode;
}

FString FUnrealAIAuthInteraction::GetRedactedDisplay() const
{
	return IsValid() ? TEXT("<redacted:auth-interaction>") : TEXT("<unset:auth-interaction>");
}

void FUnrealAIAuthInteraction::Reset()
{
	WipeString(UserCode);
	WipeString(LaunchUri);
	ApprovedOrigin = FUnrealAIEndpointOrigin{};
	Kind = EUnrealAIAuthInteractionKind::Invalid;
}

bool FUnrealAIAuthEvent::IsTerminal() const
{
	return IsKnownAuthTerminal(Kind);
}

bool FUnrealAIAuthEvent::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!RequestId.IsValid() || !IsStableIdentifier(AuthProfileId) ||
		(OperationKind != EUnrealAIAuthOperationKind::SignIn && OperationKind != EUnrealAIAuthOperationKind::SignOut) ||
		(Kind != EUnrealAIAuthEventKind::StatusChanged && Kind != EUnrealAIAuthEventKind::InteractionRequired &&
		 !IsKnownAuthTerminal(Kind)) ||
		!IsKnownAccountState(State))
	{
		OutError = TEXT("Auth event has an invalid request, profile, kind, or state.");
		return false;
	}
	if (Kind == EUnrealAIAuthEventKind::InteractionRequired)
	{
		if (OperationKind != EUnrealAIAuthOperationKind::SignIn || !Interaction.IsValid() || !Interaction->IsValid() ||
			State != EUnrealAIAccountAuthState::Authorizing)
		{
			OutError = TEXT("Auth interaction events require one valid local-UI payload and Authorizing state.");
			return false;
		}
	}
	else if (Interaction.IsValid())
	{
		OutError = TEXT("Only auth interaction events may carry a local-UI interaction payload.");
		return false;
	}
	FString ErrorShape;
	if (!Error.ValidateShape(ErrorShape))
	{
		OutError = TEXT("Auth event contains an invalid safe error envelope.");
		return false;
	}
	if ((Kind == EUnrealAIAuthEventKind::Failed || Kind == EUnrealAIAuthEventKind::Cancelled ||
		 Kind == EUnrealAIAuthEventKind::TimedOut) != Error.IsError())
	{
		OutError = TEXT("Auth terminal outcome and error envelope disagree.");
		return false;
	}
	if (Kind == EUnrealAIAuthEventKind::Succeeded &&
		(!AccountId.IsValid() ||
		 (OperationKind == EUnrealAIAuthOperationKind::SignIn && State != EUnrealAIAccountAuthState::Ready) ||
		 (OperationKind == EUnrealAIAuthOperationKind::SignOut && State != EUnrealAIAccountAuthState::SignedOut)))
	{
		OutError = TEXT("Successful auth terminal requires an exact account and matching operation state.");
		return false;
	}
	if ((Kind == EUnrealAIAuthEventKind::Cancelled && Error.Category != EUnrealAIErrorCategory::Cancelled) ||
		(Kind == EUnrealAIAuthEventKind::TimedOut && Error.Category != EUnrealAIErrorCategory::Timeout) ||
		(Kind == EUnrealAIAuthEventKind::Failed &&
		 (Error.Category == EUnrealAIErrorCategory::Cancelled || Error.Category == EUnrealAIErrorCategory::Timeout)))
	{
		OutError = TEXT("Auth cancellation and timeout terminals require matching error categories.");
		return false;
	}
	if ((Kind == EUnrealAIAuthEventKind::Cancelled && Error.Code != EUnrealAIProviderAccessErrorCode::AuthCancelled) ||
		(Kind == EUnrealAIAuthEventKind::TimedOut && Error.Code != EUnrealAIProviderAccessErrorCode::AuthTimedOut) ||
		(Kind == EUnrealAIAuthEventKind::Failed && Error.Code != EUnrealAIProviderAccessErrorCode::AuthFailed &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AuthResponseInvalid &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::PartnerGated &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::AccessProfileNotReady &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::UnsupportedCapability))
	{
		OutError = TEXT("Auth terminal outcome requires a closed auth-family error code.");
		return false;
	}
	if (IsTerminal() && Kind != EUnrealAIAuthEventKind::Succeeded && State == EUnrealAIAccountAuthState::Ready)
	{
		OutError = TEXT("Failed, cancelled, or timed-out auth terminals cannot retain Ready state.");
		return false;
	}
	return true;
}

bool FUnrealAICredentialRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!RequestId.IsValid() || !IsStableIdentifier(ConnectionAlias) || !FMath::IsFinite(TimeoutSeconds) ||
		TimeoutSeconds <= 0.0f || TimeoutSeconds > MaxTimeoutSeconds)
	{
		OutError = TEXT("Credential request requires valid IDs and a positive finite timeout.");
		return false;
	}
	return true;
}

bool FUnrealAICredentialResult::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!RequestId.IsValid() || !IsKnownCredentialTerminal(Kind))
	{
		OutError = TEXT("Credential result has an invalid request ID or terminal kind.");
		return false;
	}
	FString ErrorShape;
	if (!Error.ValidateShape(ErrorShape))
	{
		OutError = TEXT("Credential result contains an invalid safe error envelope.");
		return false;
	}
	if (Kind == EUnrealAICredentialResultKind::Succeeded)
	{
		if (!AccessContext.IsValid() || !AccessContext->IsValid() || !AccessContext->RequiresCredential() ||
			Error.IsError())
		{
			OutError = TEXT("Successful credential result requires one valid opaque credential context and no error.");
			return false;
		}
	}
	else if (Kind == EUnrealAICredentialResultKind::NotRequired)
	{
		if (!AccessContext.IsValid() || !AccessContext->IsValid() || AccessContext->RequiresCredential() ||
			Error.IsError())
		{
			OutError = TEXT("Credential-not-required result requires one anonymous opaque context and no error.");
			return false;
		}
	}
	else if (AccessContext.IsValid() || !Error.IsError())
	{
		OutError = TEXT("Failed credential result requires no provider access context and one safe error.");
		return false;
	}
	if ((Kind == EUnrealAICredentialResultKind::Cancelled && Error.Category != EUnrealAIErrorCategory::Cancelled) ||
		(Kind == EUnrealAICredentialResultKind::TimedOut && Error.Category != EUnrealAIErrorCategory::Timeout) ||
		(Kind == EUnrealAICredentialResultKind::Failed &&
		 (Error.Category == EUnrealAIErrorCategory::Cancelled || Error.Category == EUnrealAIErrorCategory::Timeout)))
	{
		OutError = TEXT("Credential cancellation and timeout terminals require matching error categories.");
		return false;
	}
	if ((Kind == EUnrealAICredentialResultKind::Cancelled &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::CredentialCancelled &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::SecretStoreCancelled) ||
		(Kind == EUnrealAICredentialResultKind::TimedOut &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::CredentialTimedOut &&
		 Error.Code != EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut) ||
		(Kind == EUnrealAICredentialResultKind::Failed && !IsCredentialFailureCode(Error.Code)))
	{
		OutError = TEXT("Credential terminal outcome requires a closed credential-family error code.");
		return false;
	}
	return true;
}

FString MakeUnrealAIConnectionBinding(const FUnrealAICredentialDestination &Destination)
{
	FString Error;
	if (!Destination.ValidateShape(Error))
	{
		return FString();
	}
	FString Binding;
	const auto Append = [&Binding](const FString &Value)
	{
		Binding += FString::FromInt(Value.Len());
		Binding.AppendChar(TEXT(':'));
		Binding += Value;
		Binding.AppendChar(TEXT(';'));
	};
	Append(Destination.ModelProviderName.ToString().ToLower());
	Append(Destination.AccountAuthProviderName.ToString().ToLower());
	Append(Destination.AuthProfileId.ToString().ToLower());
	Append(Destination.AccountId.Value.ToString());
	Append(Destination.TenantRealm.ToString().ToLower());
	Append(Destination.BillingPrincipalId.Value.ToString());
	Append(Destination.PayerHandle.ToString().ToLower());
	Append(FString::FromInt(static_cast<int32>(Destination.AuthScheme)));
	Append(FString::FromInt(static_cast<int32>(Destination.BillingMode)));
	Append(Destination.EndpointOrigin.ToString());
	Append(Destination.Audience);
	Append(LexToString(Destination.ConnectionRevision));
	Append(LexToString(Destination.EndpointPolicyRevision));
	return Binding;
}
