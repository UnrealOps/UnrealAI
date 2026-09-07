// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIOAuthLoopbackAuthorizationBrowser.h"

#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
constexpr int32 MaxCallbackParameters = 32;
constexpr int32 MaxCallbackValueBytes = 4096;
constexpr int32 MaxHeaderCount = 64;
constexpr int32 SocketBacklog = 4;

enum class ELoopbackCallbackParseResult : uint8
{
	Ignore,
	Complete
};

struct FExactLoopbackRedirect final
{
	FString AddressLiteral;
	FString HostHeader;
	FString Path;
	int32 Port = 0;
};

FUnrealAIProviderAccessError MakeLoopbackBrowserError(const EUnrealAIErrorCategory Category,
													  const EUnrealAIProviderAccessErrorCode Code,
													  const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

void SecureResetLoopbackBytes(TArray<uint8> &Bytes)
{
	if (!Bytes.IsEmpty())
	{
		FMemory::Memzero(Bytes.GetData(), Bytes.Max());
	}
	Bytes.Empty();
}

void SecureResetLoopbackString(FString &Value)
{
	TArray<TCHAR> &Characters = Value.GetCharArray();
	volatile TCHAR *Wipe = Characters.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Characters.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Value.Empty();
}

void SecureResetLoopbackStrings(TArray<FString> &Values)
{
	for (FString &Value : Values)
	{
		SecureResetLoopbackString(Value);
	}
	Values.Empty();
}

bool ObserveLoopbackContext(const FUnrealAIOAuthAuthorizationOperationContext &Context,
							FUnrealAIProviderAccessError &OutError)
{
	if (Context.IsCancellationRequested())
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::AuthCancelled);
		return false;
	}
	if (Context.IsTimedOut())
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::Timeout,
											EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
		return false;
	}
	return true;
}

bool IsAsciiDigit(const TCHAR Character)
{
	return Character >= TEXT('0') && Character <= TEXT('9');
}

bool TryParseExplicitPort(const FStringView Text, int32 &OutPort)
{
	OutPort = 0;
	if (Text.IsEmpty() || Text.Len() > 5)
	{
		return false;
	}
	for (const TCHAR Character : Text)
	{
		if (!IsAsciiDigit(Character))
		{
			return false;
		}
		OutPort = OutPort * 10 + Character - TEXT('0');
	}
	return OutPort >= 1 && OutPort <= 65535;
}

bool IsNumericIpv4Loopback(const FStringView Host)
{
	TArray<FString> Octets;
	FString(Host).ParseIntoArray(Octets, TEXT("."), false);
	if (Octets.Num() != 4 || Octets[0] != TEXT("127"))
	{
		return false;
	}
	for (const FString &Octet : Octets)
	{
		if (Octet.IsEmpty() || Octet.Len() > 3)
		{
			return false;
		}
		int32 Value = 0;
		for (const TCHAR Character : Octet)
		{
			if (!IsAsciiDigit(Character))
			{
				return false;
			}
			Value = Value * 10 + Character - TEXT('0');
		}
		if (Value > 255 || (Octet.Len() > 1 && Octet[0] == TEXT('0')))
		{
			return false;
		}
	}
	return true;
}

bool TryParseExactLoopbackRedirect(const FStringView RedirectUri, FExactLoopbackRedirect &OutRedirect)
{
	OutRedirect = {};
	const FString Uri(RedirectUri);
	if (!Uri.StartsWith(TEXT("http://"), ESearchCase::CaseSensitive) ||
						Uri.Contains(TEXT("?")) || Uri.Contains(TEXT("#")) || Uri.Contains(TEXT("\\")))
	{
		return false;
	}
	const int32 AuthorityStart = 7;
	const int32 PathIndex = Uri.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, AuthorityStart);
	if (PathIndex == INDEX_NONE || PathIndex == Uri.Len() - 1)
	{
		return false;
	}
	const FString Authority = Uri.Mid(AuthorityStart, PathIndex - AuthorityStart);
	FString PortText;
	if (Authority.StartsWith(TEXT("[::1]:"), ESearchCase::CaseSensitive))
	{
		OutRedirect.AddressLiteral = TEXT("::1");
		PortText = Authority.Mid(6);
	}
	else
	{
		int32 ColonIndex = INDEX_NONE;
		if (!Authority.FindLastChar(TEXT(':'), ColonIndex) || ColonIndex <= 0)
		{
			return false;
		}
		OutRedirect.AddressLiteral = Authority.Left(ColonIndex);
		PortText = Authority.Mid(ColonIndex + 1);
		if (!IsNumericIpv4Loopback(OutRedirect.AddressLiteral))
		{
			return false;
		}
	}
	if (!TryParseExplicitPort(PortText, OutRedirect.Port))
	{
		return false;
	}
	OutRedirect.HostHeader = Authority;
	OutRedirect.Path = Uri.Mid(PathIndex);
	return true;
}

bool IsLoopbackPeer(const FInternetAddr &Peer)
{
	const TArray<uint8> Bytes = Peer.GetRawIp();
	if (Bytes.Num() == 4)
	{
		return Bytes[0] == 127;
	}
	if (Bytes.Num() != 16)
	{
		return false;
	}
	bool bIpv6Loopback = Bytes[15] == 1;
	for (int32 Index = 0; Index < 15; ++Index)
	{
		bIpv6Loopback &= Bytes[Index] == 0;
	}
	bool bIpv4MappedLoopback = Bytes[10] == 0xff && Bytes[11] == 0xff && Bytes[12] == 127;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		bIpv4MappedLoopback &= Bytes[Index] == 0;
	}
	return bIpv6Loopback || bIpv4MappedLoopback;
}

FTimespan PollDuration(const FUnrealAIOAuthAuthorizationOperationContext &Context, const double PollSeconds)
{
	return FTimespan::FromSeconds(FMath::Max(0.0001, FMath::Min(PollSeconds, Context.RemainingSeconds())));
}

int32 FindHeaderTerminator(const TConstArrayView<uint8> Bytes)
{
	for (int32 Index = 0; Index + 3 < Bytes.Num(); ++Index)
	{
		if (Bytes[Index] == '\r' && Bytes[Index + 1] == '\n' && Bytes[Index + 2] == '\r' && Bytes[Index + 3] == '\n')
		{
			return Index + 4;
		}
	}
	return INDEX_NONE;
}

bool ReadBoundedHeader(FSocket &Socket, const FUnrealAIOAuthAuthorizationOperationContext &Context,
					   const FUnrealAIOAuthLoopbackAuthorizationBrowserConfig &Config, TArray<uint8> &OutHeader,
					   FUnrealAIProviderAccessError &OutError)
{
	OutHeader.Reset();
	while (OutHeader.Num() < Config.MaxRequestHeaderBytes)
	{
		if (!ObserveLoopbackContext(Context, OutError))
		{
			return false;
		}
		if (!Socket.Wait(ESocketWaitConditions::WaitForRead, PollDuration(Context, Config.SocketPollSeconds)))
		{
			continue;
		}
		uint8 Buffer[1024];
		int32 BytesRead = 0;
		const int32 Remaining = Config.MaxRequestHeaderBytes - OutHeader.Num();
		if (!Socket.Recv(Buffer, FMath::Min(Remaining, static_cast<int32>(UE_ARRAY_COUNT(Buffer))), BytesRead) ||
			BytesRead <= 0)
		{
			return false;
		}
		OutHeader.Append(Buffer, BytesRead);
		if (FindHeaderTerminator(OutHeader) != INDEX_NONE)
		{
			return true;
		}
	}
	return false;
}

bool IsHttpTokenCharacter(const TCHAR Character)
{
	if ((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('A') && Character <= TEXT('Z')) ||
															   (Character >= TEXT('a') && Character <= TEXT('z')))
	{
		return true;
	}
	switch (Character)
	{
	case TEXT('!'):
	case TEXT('#'):
	case TEXT('$'):
	case TEXT('%'):
	case TEXT('&'):
	case TEXT('\''):
	case TEXT('*'):
	case TEXT('+'):
	case TEXT('-'):
	case TEXT('.'):
	case TEXT('^'):
	case TEXT('_'):
	case TEXT('`'):
	case TEXT('|'):
	case TEXT('~'):
		return true;
	default:
		return false;
	}
}

int32 HexValue(const TCHAR Character)
{
	if (Character >= TEXT('0') && Character <= TEXT('9'))
	{
		return Character - TEXT('0');
	}
	if (Character >= TEXT('a') && Character <= TEXT('f'))
	{
		return Character - TEXT('a') + 10;
	}
	if (Character >= TEXT('A') && Character <= TEXT('F'))
	{
		return Character - TEXT('A') + 10;
	}
	return INDEX_NONE;
}

bool DecodeBoundedAsciiQueryValue(const FStringView Encoded, const int32 MaxBytes, TArray<uint8> &OutBytes)
{
	OutBytes.Reset();
	if (Encoded.IsEmpty() || Encoded.Len() > MaxBytes * 3)
	{
		return false;
	}
	for (int32 Index = 0; Index < Encoded.Len(); ++Index)
	{
		uint8 Byte = 0;
		const TCHAR Character = Encoded[Index];
		if (Character == TEXT('%'))
		{
			if (Index + 2 >= Encoded.Len())
			{
				return false;
			}
			const int32 High = HexValue(Encoded[Index + 1]);
			const int32 Low = HexValue(Encoded[Index + 2]);
			if (High == INDEX_NONE || Low == INDEX_NONE)
			{
				return false;
			}
			Byte = static_cast<uint8>((High << 4) | Low);
			Index += 2;
		}
		else
		{
			if (Character > 0x7f)
			{
				return false;
			}
			Byte = Character == TEXT('+') ? static_cast<uint8>(' ') : static_cast<uint8>(Character);
		}
		if (Byte < 0x20 || Byte > 0x7e || OutBytes.Num() >= MaxBytes)
		{
			return false;
		}
		OutBytes.Add(Byte);
	}
	return !OutBytes.IsEmpty();
}

bool DecodeBoundedAsciiQueryString(const FStringView Encoded, const int32 MaxBytes, FString &OutValue)
{
	OutValue.Reset();
	TArray<uint8> Bytes;
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackBytes(Bytes);
	};
	if (!DecodeBoundedAsciiQueryValue(Encoded, MaxBytes, Bytes))
	{
		return false;
	}
	OutValue.Reserve(Bytes.Num());
	for (const uint8 Byte : Bytes)
	{
		OutValue.AppendChar(static_cast<TCHAR>(Byte));
	}
	return true;
}

bool ConstantTimeLoopbackEquals(const FStringView A, const FStringView B)
{
	uint32 Difference = static_cast<uint32>(A.Len() ^ B.Len());
	const int32 Count = FMath::Max(A.Len(), B.Len());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint32 Left = Index < A.Len() ? static_cast<uint32>(A[Index]) : 0u;
		const uint32 Right = Index < B.Len() ? static_cast<uint32>(B[Index]) : 0u;
		Difference |= Left ^ Right;
	}
	return Difference == 0;
}

bool TryParseRequestTarget(const FStringView Target, const FExactLoopbackRedirect &Redirect,
						   const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch,
						   FUnrealAIOAuthBrowserAuthorizationCallback &OutCallback)
{
	int32 QueryIndex = INDEX_NONE;
	if (!FString(Target).FindChar(TEXT('?'), QueryIndex) || QueryIndex <= 0 || QueryIndex == Target.Len() - 1 ||
								  FString(Target.Left(QueryIndex)) != Redirect.Path || Target.Contains(TEXT("#")))
	{
		return false;
	}
	TArray<FString> Parameters;
	FString(Target.RightChop(QueryIndex + 1)).ParseIntoArray(Parameters, TEXT("&"), false);
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackStrings(Parameters);
	};
	if (Parameters.IsEmpty() || Parameters.Num() > MaxCallbackParameters)
	{
		return false;
	}
	TSet<FString> SeenNames;
	TArray<uint8> Code;
	FString State;
	FString Error;
	FString Issuer;
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackBytes(Code);
		SecureResetLoopbackString(State);
		SecureResetLoopbackString(Error);
		SecureResetLoopbackString(Issuer);
	};
	for (const FString &Parameter : Parameters)
	{
		int32 EqualsIndex = INDEX_NONE;
		if (!Parameter.FindChar(TEXT('='), EqualsIndex) || EqualsIndex <= 0 || EqualsIndex == Parameter.Len() - 1)
		{
			return false;
		}
		const FString Name = Parameter.Left(EqualsIndex);
		if (SeenNames.Contains(Name))
		{
			return false;
		}
		for (const TCHAR Character : Name)
		{
			if (!IsHttpTokenCharacter(Character))
			{
				return false;
			}
		}
		SeenNames.Add(Name);
		const FStringView EncodedValue = FStringView(Parameter).RightChop(EqualsIndex + 1);
		if (Name == TEXT("code"))
		{
			if (!DecodeBoundedAsciiQueryValue(EncodedValue, MaxCallbackValueBytes, Code))
			{
				return false;
			}
		}
		else if (Name == TEXT("state"))
		{
			if (!DecodeBoundedAsciiQueryString(EncodedValue, 256, State))
			{
				return false;
			}
		}
		else if (Name == TEXT("error"))
		{
			if (!DecodeBoundedAsciiQueryString(EncodedValue, 256, Error))
			{
				return false;
			}
		}
		else if (Name == TEXT("iss"))
		{
			if (!DecodeBoundedAsciiQueryString(EncodedValue, FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes,
											   Issuer))
			{
				return false;
			}
		}
		else
		{
			TArray<uint8> Ignored;
			const bool bBounded = DecodeBoundedAsciiQueryValue(EncodedValue, MaxCallbackValueBytes, Ignored);
			SecureResetLoopbackBytes(Ignored);
			if (!bBounded)
			{
				return false;
			}
		}
	}
	if (State.IsEmpty() || !ConstantTimeLoopbackEquals(State, Launch.ExpectedState) ||
		(!Issuer.IsEmpty() && Issuer != Launch.ExpectedIssuer) || (Code.IsEmpty() == Error.IsEmpty()))
	{
		return false;
	}
	OutCallback.Reset();
	OutCallback.ExactRedirectUri = Launch.ExactRedirectUri;
	OutCallback.Issuer = Launch.ExpectedIssuer;
	OutCallback.State = State;
	if (!Code.IsEmpty())
	{
		FString SecretError;
		if (!FUnrealAISecretValue::TryCreate(MoveTemp(Code), OutCallback.AuthorizationCode, SecretError))
		{
			SecureResetLoopbackString(SecretError);
			OutCallback.Reset();
			return false;
		}
		SecureResetLoopbackString(SecretError);
		OutCallback.Kind = EUnrealAIOAuthBrowserCallbackKind::AuthorizationCode;
	}
	else
	{
		OutCallback.Kind = Error == TEXT("access_denied") ? EUnrealAIOAuthBrowserCallbackKind::Denied
														  : EUnrealAIOAuthBrowserCallbackKind::Failed;
	}
	FString ShapeError;
	const bool bValid = OutCallback.ValidateShape(ShapeError);
	SecureResetLoopbackString(ShapeError);
	if (!bValid)
	{
		OutCallback.Reset();
	}
	return bValid;
}

ELoopbackCallbackParseResult ParseLoopbackRequest(const TConstArrayView<uint8> Header,
												  const FExactLoopbackRedirect &Redirect,
												  const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch,
												  FUnrealAIOAuthBrowserAuthorizationCallback &OutCallback)
{
	const int32 HeaderEnd = FindHeaderTerminator(Header);
	if (HeaderEnd == INDEX_NONE)
	{
		return ELoopbackCallbackParseResult::Ignore;
	}
	FString Text;
	Text.Reserve(HeaderEnd);
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackString(Text);
	};
	for (int32 Index = 0; Index < HeaderEnd - 4; ++Index)
	{
		const uint8 Byte = Header[Index];
		if (Byte != '\r' && Byte != '\n' && (Byte < 0x20 || Byte > 0x7e))
		{
			return ELoopbackCallbackParseResult::Ignore;
		}
		Text.AppendChar(static_cast<TCHAR>(Byte));
	}
	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\r\n"), false);
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackStrings(Lines);
	};
	if (Lines.IsEmpty() || Lines.Num() > MaxHeaderCount + 1)
	{
		return ELoopbackCallbackParseResult::Ignore;
	}
	TArray<FString> RequestParts;
	Lines[0].ParseIntoArray(RequestParts, TEXT(" "), false);
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackStrings(RequestParts);
	};
	if (RequestParts.Num() != 3 ||
		RequestParts[0] != TEXT("GET") || RequestParts[2] != TEXT("HTTP/1.1") || !RequestParts[1].StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		return ELoopbackCallbackParseResult::Ignore;
	}
	bool bSawHost = false;
	TSet<FString> SeenHeaderNames;
	for (int32 Index = 1; Index < Lines.Num(); ++Index)
	{
		const FString &Line = Lines[Index];
		if (Line.IsEmpty() || Line.StartsWith(TEXT(" ")) || Line.StartsWith(TEXT("\t")))
		{
			return ELoopbackCallbackParseResult::Ignore;
		}
		int32 ColonIndex = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), ColonIndex) || ColonIndex <= 0)
		{
			return ELoopbackCallbackParseResult::Ignore;
		}
		const FString Name = Line.Left(ColonIndex);
		for (const TCHAR Character : Name)
		{
			if (!IsHttpTokenCharacter(Character))
			{
				return ELoopbackCallbackParseResult::Ignore;
			}
		}
		const FString CanonicalName = Name.ToLower();
		if (SeenHeaderNames.Contains(CanonicalName))
		{
			return ELoopbackCallbackParseResult::Ignore;
		}
		SeenHeaderNames.Add(CanonicalName);
		FString Value = Line.Mid(ColonIndex + 1);
		Value.TrimStartAndEndInline();
		if (Name.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
		{
			if (bSawHost || !Value.Equals(Redirect.HostHeader, ESearchCase::IgnoreCase))
			{
				return ELoopbackCallbackParseResult::Ignore;
			}
			bSawHost = true;
		}
		else if (Name.Equals(TEXT("Transfer-Encoding"), ESearchCase::IgnoreCase) ||
							 (Name.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase) && Value != TEXT("0")))
		{
			return ELoopbackCallbackParseResult::Ignore;
		}
	}
	return bSawHost && TryParseRequestTarget(RequestParts[1], Redirect, Launch, OutCallback)
			   ? ELoopbackCallbackParseResult::Complete
			   : ELoopbackCallbackParseResult::Ignore;
}

void SendBrowserResponse(FSocket &Socket, const bool bComplete,
						 const FUnrealAIOAuthAuthorizationOperationContext &Context,
						 const FUnrealAIOAuthLoopbackAuthorizationBrowserConfig &Config)
{
	const ANSICHAR *Body = bComplete ? "<!doctype html><meta charset=utf-8><title>Authorization complete</title>"
									   "<p>Authorization completed. You can close this window.</p>"
									 : "<!doctype html><meta charset=utf-8><title>Invalid callback</title>"
									   "<p>This callback was not accepted.</p>";
	const int32 BodyLength = FCStringAnsi::Strlen(Body);
	const FString Header =
		FString::Printf(TEXT("HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n")
							TEXT("Connection: close\r\nCache-Control: no-store\r\nPragma: no-cache\r\n")
								TEXT("Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n")
									TEXT("Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n\r\n"),
										 bComplete ? TEXT("200 OK") : TEXT("400 Bad Request"), BodyLength);
	FTCHARToUTF8 HeaderUtf8(*Header);
	TArray<uint8> Response;
	Response.Append(reinterpret_cast<const uint8 *>(HeaderUtf8.Get()), HeaderUtf8.Length());
	Response.Append(reinterpret_cast<const uint8 *>(Body), BodyLength);
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackBytes(Response);
	};
	int32 Offset = 0;
	const double StartedAt = FPlatformTime::Seconds();
	while (Offset < Response.Num() && FPlatformTime::Seconds() - StartedAt < Config.ResponseWriteTimeoutSeconds &&
		   !Context.IsCancellationRequested() && !Context.IsTimedOut())
	{
		if (!Socket.Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(10)))
		{
			continue;
		}
		int32 BytesSent = 0;
		if (!Socket.Send(Response.GetData() + Offset, Response.Num() - Offset, BytesSent) || BytesSent <= 0)
		{
			break;
		}
		Offset += BytesSent;
	}
}
} // namespace

bool FUnrealAIOAuthLoopbackAuthorizationBrowserConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxRequestHeaderBytes < 1024 || MaxRequestHeaderBytes > MaxRequestHeaderBytesLimit ||
		MaxAcceptedConnections < 1 || MaxAcceptedConnections > MaxAcceptedConnectionsLimit ||
		!FMath::IsFinite(SocketPollSeconds) || SocketPollSeconds < 0.001 || SocketPollSeconds > 0.25 ||
		!FMath::IsFinite(ResponseWriteTimeoutSeconds) || ResponseWriteTimeoutSeconds < 0.01 ||
		ResponseWriteTimeoutSeconds > 2.0)
	{
		OutError = TEXT("OAuth loopback browser configuration exceeds its bounded socket policy.");
		return false;
	}
	return true;
}

bool FUnrealAIPlatformOAuthSystemBrowserLauncher::LaunchSystemBrowser(const FStringView AuthorizationUrl,
																	  FUnrealAIProviderAccessError &OutError)
{
	OutError = {};
	FString Url(AuthorizationUrl);
	FString LaunchError;
	ON_SCOPE_EXIT
	{
		SecureResetLoopbackString(Url);
		SecureResetLoopbackString(LaunchError);
	};
	if (!Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive) || !FPlatformProcess::CanLaunchURL(*Url))
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::UnsupportedCapability,
											EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}
	FPlatformProcess::LaunchURL(*Url, nullptr, &LaunchError);
	if (!LaunchError.IsEmpty())
	{
		OutError =
			MakeLoopbackBrowserError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
		return false;
	}
	return true;
}

FUnrealAIOAuthLoopbackAuthorizationBrowser::FUnrealAIOAuthLoopbackAuthorizationBrowser(
	const FUnrealAIOAuthLoopbackAuthorizationBrowserConfig &InConfig)
	: FUnrealAIOAuthLoopbackAuthorizationBrowser(
		  MakeShared<FUnrealAIPlatformOAuthSystemBrowserLauncher, ESPMode::ThreadSafe>(), InConfig)
{
}

FUnrealAIOAuthLoopbackAuthorizationBrowser::FUnrealAIOAuthLoopbackAuthorizationBrowser(
	TSharedRef<IUnrealAIOAuthSystemBrowserLauncher, ESPMode::ThreadSafe> InLauncher,
	const FUnrealAIOAuthLoopbackAuthorizationBrowserConfig &InConfig)
	: Launcher(MoveTemp(InLauncher)), Config(InConfig)
{
}

bool FUnrealAIOAuthLoopbackAuthorizationBrowser::Authorize(const FUnrealAIOAuthAuthorizationOperationContext &Context,
														   const FUnrealAIOAuthBrowserAuthorizationLaunch &Launch,
														   FUnrealAIOAuthBrowserAuthorizationCallback &OutCallback,
														   FUnrealAIProviderAccessError &OutError)
{
	OutCallback.Reset();
	OutError = {};
	FString ShapeError;
	FExactLoopbackRedirect Redirect;
	if (!Context.ValidateShape(ShapeError) || !Launch.ValidateShape(ShapeError))
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::InvalidArgument,
											EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	if (!Config.ValidateShape(ShapeError) || !TryParseExactLoopbackRedirect(Launch.ExactRedirectUri, Redirect))
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::InvalidConfiguration,
											EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	if (!ObserveLoopbackContext(Context, OutError))
	{
		return false;
	}

	ISocketSubsystem *const SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::UnsupportedCapability,
											EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}
	TSharedPtr<FInternetAddr> BindAddress = SocketSubsystem->GetAddressFromString(Redirect.AddressLiteral);
	if (!BindAddress.IsValid() || !BindAddress->IsValid() || !BindAddress->IsPortValid(Redirect.Port))
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::InvalidConfiguration,
											EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	BindAddress->SetPort(Redirect.Port);
	FUniqueSocket Listener = SocketSubsystem->CreateUniqueSocket(NAME_Stream, TEXT("AgentOAuthLoopbackListener"),
																				   BindAddress->GetProtocolType());
	if (!Listener || !Listener->SetNonBlocking(true) || !Listener->Bind(*BindAddress) ||
		!Listener->Listen(SocketBacklog))
	{
		OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::Busy,
											EUnrealAIProviderAccessErrorCode::OperationBusy, true);
		return false;
	}
	OutError = {};
	if (!Launcher->LaunchSystemBrowser(Launch.AuthorizationUrl, OutError))
	{
		FString ErrorShape;
		if (!OutError.IsError() || !OutError.ValidateShape(ErrorShape))
		{
			OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::Provider,
												EUnrealAIProviderAccessErrorCode::AuthFailed);
		}
		return false;
	}

	for (int32 AcceptedCount = 0; AcceptedCount < Config.MaxAcceptedConnections;)
	{
		if (!ObserveLoopbackContext(Context, OutError))
		{
			return false;
		}
		bool bPending = false;
		if (!Listener->WaitForPendingConnection(bPending, PollDuration(Context, Config.SocketPollSeconds)))
		{
			OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::Provider,
												EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		if (!bPending)
		{
			continue;
		}
		++AcceptedCount;
		TSharedRef<FInternetAddr> PeerAddress = BindAddress->Clone();
		FUniqueSocket Connection(Listener->Accept(*PeerAddress, TEXT("AgentOAuthLoopbackCallback")),
												  FSocketDeleter(SocketSubsystem));
		if (!Connection || !IsLoopbackPeer(*PeerAddress) || !Connection->SetNonBlocking(true))
		{
			continue;
		}
		TArray<uint8> Header;
		ON_SCOPE_EXIT
		{
			SecureResetLoopbackBytes(Header);
		};
		FUnrealAIProviderAccessError ReadError;
		if (!ReadBoundedHeader(*Connection, Context, Config, Header, ReadError))
		{
			if (ReadError.IsError())
			{
				OutError = ReadError;
				return false;
			}
			SendBrowserResponse(*Connection, false, Context, Config);
			continue;
		}
		const bool bComplete =
			ParseLoopbackRequest(Header, Redirect, Launch, OutCallback) == ELoopbackCallbackParseResult::Complete;
		SendBrowserResponse(*Connection, bComplete, Context, Config);
		if (bComplete)
		{
			OutError = {};
			return true;
		}
	}
	OutError = MakeLoopbackBrowserError(EUnrealAIErrorCategory::Provider,
										EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
	return false;
}
