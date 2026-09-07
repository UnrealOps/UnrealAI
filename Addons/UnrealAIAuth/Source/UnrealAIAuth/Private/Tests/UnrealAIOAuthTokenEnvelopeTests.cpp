// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"

#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "Runtime/UnrealAIClock.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
bool MakeSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Text), Length);
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

bool MakeFilledSecret(const int32 Size, const uint8 Fill, FUnrealAISecretValue &OutSecret)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Size);
	FMemory::Memset(Bytes.GetData(), Fill, Size);
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

FUnrealAIOAuthTokenEnvelopeBinding MakeBinding()
{
	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	Binding.ProviderName = TEXT("tests.oauth.provider");
	Binding.AuthProfileId = TEXT("tests.oauth.profile");
	Binding.AccountId.Value = FGuid(0x01020304, 0x11121314, 0x21222324, 0x31323334);
	return Binding;
}

FUnrealAIOAuthTokenSet MakeTokens(const ANSICHAR *Access, const ANSICHAR *Refresh, const ANSICHAR *IdToken,
								  const ANSICHAR *AccountRouting, const FDateTime &ExpiryUtc)
{
	FUnrealAIOAuthTokenSet Tokens;
	MakeSecret(Access, Tokens.AccessToken);
	if (Refresh != nullptr)
	{
		MakeSecret(Refresh, Tokens.RefreshToken);
	}
	if (IdToken != nullptr)
	{
		MakeSecret(IdToken, Tokens.IdToken);
	}
	if (AccountRouting != nullptr)
	{
		MakeSecret(AccountRouting, Tokens.AccountRoutingValue);
	}
	Tokens.AccessTokenExpiresAtUtc = ExpiryUtc;
	return Tokens;
}

FUnrealAICredentialDestination MakeSecretVerificationDestination()
{
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = TEXT("tests.oauth.provider");
	Destination.AccountAuthProviderName = TEXT("tests.oauth.auth");
	Destination.AuthProfileId = TEXT("tests.oauth.profile");
	Destination.AccountId = MakeBinding().AccountId;
	Destination.TenantRealm = TEXT("tests.local_machine");
	Destination.BillingPrincipalId.Value = FGuid(1, 2, 3, 4);
	Destination.PayerHandle = TEXT("tests.oauth.subscription");
	Destination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Destination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Destination.Audience = TEXT("https://oauth.invalid/v1/responses");
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	FString Error;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://oauth.invalid"), false, Destination.EndpointOrigin, Error);
	return Destination;
}

class FExpectedSecretApplicator final : public IUnrealAICredentialApplicator
{
  public:
	FExpectedSecretApplicator(FUnrealAICredentialDestination InDestination, const ANSICHAR *Expected)
		: Destination(MoveTemp(InDestination))
	{
		const int32 Length = FCStringAnsi::Strlen(Expected);
		ExpectedBytes.Append(reinterpret_cast<const uint8 *>(Expected), Length);
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	bool bMatched = false;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret) override
	{
		uint8 Difference = Secret.Num() == ExpectedBytes.Num() ? 0 : 1;
		const int32 Maximum = FMath::Max(Secret.Num(), ExpectedBytes.Num());
		for (int32 Index = 0; Index < Maximum; ++Index)
		{
			const uint8 Actual = Secret.IsValidIndex(Index) ? Secret[Index] : 0;
			const uint8 Expected = ExpectedBytes.IsValidIndex(Index) ? ExpectedBytes[Index] : 0;
			Difference |= Actual ^ Expected;
		}
		bMatched = Scheme == EUnrealAIAuthScheme::OAuthBearer && Difference == 0;
		return bMatched;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
	TArray<uint8> ExpectedBytes;
};

bool VerifySecret(FUnrealAISecretValue &&Secret, const ANSICHAR *Expected)
{
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	FUnrealAICredentialFreshnessSource Freshness;
	FUnrealAICredentialLease Lease;
	FString Error;
	const FDateTime CredentialExpiryUtc = MutableClock->UtcNow() + FTimespan::FromMinutes(1);
	if (!FUnrealAICredentialLease::TryCreate(MakeSecretVerificationDestination(), Clock, Freshness.GetToken(), 10.0,
											 CredentialExpiryUtc, MoveTemp(Secret), Lease, Error))
	{
		return false;
	}
	FExpectedSecretApplicator Applicator(MakeSecretVerificationDestination(), Expected);
	return Lease.TryApplyTo(Applicator, Error) && Applicator.bMatched;
}

bool MakeEncodedEnvelope(const FUnrealAIOAuthTokenEnvelopeBinding &Binding, const FDateTime &NowUtc,
						 const FDateTime &ExpiryUtc, FUnrealAISecretValue &OutEncoded)
{
	FUnrealAIOAuthTokenSet Tokens = MakeTokens("access-one", "refresh-old", "id-one", "account-route", ExpiryUtc);
	FUnrealAIOAuthTokenEnvelope Envelope;
	FString Error;
	return FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), NowUtc, Envelope, Error) &&
		   FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), NowUtc, OutEncoded, Error);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthTokenEnvelopeRoundTripTest,
								 "UnrealAI.Auth.OAuthTokenEnvelope.RoundTripAndSecretPresentation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthTokenEnvelopeRoundTripTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FDateTime NowUtc(2026, 7, 23, 12, 0, 0);
	const FDateTime ExpiryUtc(2026, 7, 23, 13, 0, 0);
	const FUnrealAIOAuthTokenEnvelopeBinding Binding = MakeBinding();
	FUnrealAIOAuthTokenSet Tokens = MakeTokens("access-one", "refresh old/+?", "id-one", "account-route", ExpiryUtc);
	FUnrealAIOAuthTokenEnvelope Envelope;
	FString Error;
	TestTrue(TEXT("A bounded initial token set creates an envelope"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), NowUtc, Envelope, Error));
	TestFalse(TEXT("Creating the envelope consumes and wipes the source access token"), Tokens.AccessToken.IsSet());
	TestFalse(TEXT("Creating the envelope consumes and wipes the source refresh token"), Tokens.RefreshToken.IsSet());
	TestTrue(TEXT("Created envelope is set"), Envelope.IsSet());
	TestTrue(TEXT("Created envelope preserves its exact non-secret binding"), Envelope.GetBinding() == Binding);
	TestEqual(TEXT("Created envelope preserves its explicit UTC expiry"), Envelope.GetAccessTokenExpiresAtUtc(),
				   ExpiryUtc);
	TestTrue(TEXT("Created envelope records refresh-token presence without revealing it"), Envelope.HasRefreshToken());
	TestTrue(TEXT("Created envelope records ID-token presence without revealing it"), Envelope.HasIdToken());
	TestTrue(TEXT("Created envelope records protected routing presence without revealing it"),
				  Envelope.HasAccountRoutingValue());

	FUnrealAISecretValue Encoded;
	TestTrue(TEXT("Envelope encodes into one secret value"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), NowUtc, Encoded, Error));
	TestFalse(TEXT("Encoding consumes and wipes the source envelope"), Envelope.IsSet());
	TestTrue(TEXT("Encoded envelope is represented only as a secret value"), Encoded.IsSet());

	FUnrealAIOAuthTokenEnvelope Decoded;
	TestTrue(TEXT("Encoded secret decodes only for its exact binding"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(Encoded), Binding, NowUtc, Decoded, Error));
	TestFalse(TEXT("Decoding consumes and wipes the encoded secret"), Encoded.IsSet());
	TestEqual(TEXT("Decoded envelope preserves its UTC expiry"), Decoded.GetAccessTokenExpiresAtUtc(), ExpiryUtc);

	FUnrealAISecretValue RefreshBody;
	TestTrue(TEXT("Refresh form is minted without a plaintext token getter"),
				  Decoded.TryMintRefreshRequestBody(TEXT("client.test"), RefreshBody, Error));
	TestTrue(TEXT("Refresh form percent-encodes sensitive bytes deterministically"),
				  VerifySecret(MoveTemp(RefreshBody),
							   "grant_type=refresh_token&refresh_token=refresh%20old%2F%2B%3F&client_id=client.test"));
	FUnrealAISecretValue RevocationCredential;
	TestTrue(TEXT("Revocation prefers and consumes the refresh credential"),
				  Decoded.TryTakeRevocationCredential(RevocationCredential, Error));
	TestTrue(TEXT("Preferred revocation bytes remain opaque and exact"),
				  VerifySecret(MoveTemp(RevocationCredential), "refresh old/+?"));
	TestFalse(TEXT("Taking the revocation credential clears refresh presence"), Decoded.HasRefreshToken());

	FUnrealAISecretValue Bearer;
	FUnrealAISecretValue AccountRouting;
	TestTrue(TEXT("Dispatch takes bearer and protected routing secrets atomically"),
				  Decoded.TryTakeDispatchCredentials(NowUtc, Bearer, AccountRouting, Error));
	TestTrue(TEXT("Taken bearer bytes match without a public plaintext accessor"),
				  VerifySecret(MoveTemp(Bearer), "access-one"));
	TestTrue(TEXT("Taken routing bytes match without a public plaintext accessor"),
				  VerifySecret(MoveTemp(AccountRouting), "account-route"));
	TestFalse(TEXT("Dispatch credentials are one-shot"),
				   Decoded.TryTakeDispatchCredentials(NowUtc, Bearer, AccountRouting, Error));
	TestFalse(TEXT("Failed second take leaves no bearer output"), Bearer.IsSet());
	TestFalse(TEXT("Failed second take leaves no routing output"), AccountRouting.IsSet());
	TestFalse(TEXT("Revocation cannot mint a credential after both token values are consumed"),
				   Decoded.TryTakeRevocationCredential(RevocationCredential, Error));
	TestFalse(TEXT("Failed revocation take leaves no token output"), RevocationCredential.IsSet());

	FUnrealAIOAuthTokenSet AccessOnlyTokens = MakeTokens("access-only", nullptr, nullptr, nullptr, ExpiryUtc);
	FUnrealAIOAuthTokenEnvelope AccessOnlyEnvelope;
	TestTrue(TEXT("An access-only token set creates a bounded envelope"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(AccessOnlyTokens), NowUtc,
															  AccessOnlyEnvelope, Error));
	TestTrue(TEXT("Revocation falls back to the access credential when refresh is absent"),
				  AccessOnlyEnvelope.TryTakeRevocationCredential(RevocationCredential, Error));
	TestTrue(TEXT("Fallback revocation bytes remain opaque and exact"),
				  VerifySecret(MoveTemp(RevocationCredential), "access-only"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthTokenEnvelopeRefreshRotationTest,
								 "UnrealAI.Auth.OAuthTokenEnvelope.RefreshRotationAndOmittedFallback",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthTokenEnvelopeRefreshRotationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FDateTime NowUtc(2026, 7, 23, 12, 0, 0);
	const FUnrealAIOAuthTokenEnvelopeBinding Binding = MakeBinding();
	FUnrealAIOAuthTokenSet Initial =
		MakeTokens("access-one", "refresh-old", "id-one", "account-route", FDateTime(2026, 7, 23, 13, 0, 0));
	FUnrealAIOAuthTokenEnvelope Envelope;
	FString Error;
	if (!TestTrue(
			TEXT("Initial refreshable envelope creates"),
				 FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Initial), NowUtc, Envelope, Error)))
	{
		return false;
	}

	FUnrealAIOAuthTokenSet Rotated =
		MakeTokens("access-two", "refresh-new", nullptr, "account-route", FDateTime(2026, 7, 23, 14, 0, 0));
	TestTrue(TEXT("Present refresh token rotates the prior token"),
				  Envelope.TryApplyRefresh(MoveTemp(Rotated), NowUtc, Error));
	TestFalse(TEXT("Successful rotation consumes the response access token"), Rotated.AccessToken.IsSet());
	TestFalse(TEXT("Successful rotation consumes the response refresh token"), Rotated.RefreshToken.IsSet());
	FUnrealAISecretValue RefreshBody;
	TestTrue(TEXT("Rotated refresh token can mint the next form"),
				  Envelope.TryMintRefreshRequestBody(TEXT("client.test"), RefreshBody, Error));
	TestTrue(TEXT("Next refresh form contains the rotated token"),
				  VerifySecret(MoveTemp(RefreshBody),
							   "grant_type=refresh_token&refresh_token=refresh-new&client_id=client.test"));

	FUnrealAIOAuthTokenSet Omitted =
		MakeTokens("access-three", nullptr, nullptr, "account-route", FDateTime(2026, 7, 23, 15, 0, 0));
	TestTrue(TEXT("Omitted refresh field falls back to the prior rotated token only"),
				  Envelope.TryApplyRefresh(MoveTemp(Omitted), NowUtc, Error));
	TestFalse(TEXT("Omitted-token refresh still consumes its new access token"), Omitted.AccessToken.IsSet());

	FUnrealAISecretValue Persisted;
	TestTrue(TEXT("Refreshed envelope re-encodes atomically"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), NowUtc, Persisted, Error));
	FUnrealAIOAuthTokenEnvelope Reloaded;
	TestTrue(TEXT("Refreshed atomic record decodes"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(Persisted), Binding, NowUtc, Reloaded, Error));
	TestTrue(TEXT("Reloaded envelope retains the prior token when refresh omitted"),
				  Reloaded.TryMintRefreshRequestBody(TEXT("client.test"), RefreshBody, Error));
	TestTrue(TEXT("Omission never restores the superseded old refresh token"),
				  VerifySecret(MoveTemp(RefreshBody),
							   "grant_type=refresh_token&refresh_token=refresh-new&client_id=client.test"));
	FUnrealAISecretValue Bearer;
	FUnrealAISecretValue Routing;
	TestTrue(TEXT("Latest access token and retained routing remain dispatchable"),
				  Reloaded.TryTakeDispatchCredentials(NowUtc, Bearer, Routing, Error));
	TestTrue(TEXT("Latest refreshed bearer is selected"), VerifySecret(MoveTemp(Bearer), "access-three"));
	TestTrue(TEXT("Exact routing field preserves the protected account route"),
				  VerifySecret(MoveTemp(Routing), "account-route"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthTokenEnvelopeRefreshRoutingContinuityTest,
								 "UnrealAI.Auth.OAuthTokenEnvelope.RefreshRoutingContinuityFailsClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthTokenEnvelopeRefreshRoutingContinuityTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FDateTime NowUtc(2026, 7, 23, 12, 0, 0);
	const FUnrealAIOAuthTokenEnvelopeBinding Binding = MakeBinding();
	FString Error;

	FUnrealAIOAuthTokenEnvelope RoutedEnvelope;
	FUnrealAIOAuthTokenSet Initial = MakeTokens("access-original", "refresh-original", "id-original", "account-route",
												FDateTime(2026, 7, 23, 13, 0, 0));
	if (!TestTrue(TEXT("Protected-routing fixture creates"),
					   FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Initial), NowUtc, RoutedEnvelope,
																   Error)))
	{
		return false;
	}

	FUnrealAIOAuthTokenSet OmittedRouting =
		MakeTokens("access-omitted", "refresh-omitted", nullptr, nullptr, FDateTime(2026, 7, 23, 14, 0, 0));
	TestFalse(TEXT("Refresh cannot omit existing protected routing"),
				   RoutedEnvelope.TryApplyRefresh(MoveTemp(OmittedRouting), NowUtc, Error));
	TestFalse(TEXT("Rejected omission wipes returned access material"), OmittedRouting.AccessToken.IsSet());
	TestFalse(TEXT("Rejected omission wipes returned refresh material"), OmittedRouting.RefreshToken.IsSet());

	FUnrealAIOAuthTokenSet MismatchedRouting =
		MakeTokens("access-mismatch", "refresh-mismatch", nullptr, "other-route", FDateTime(2026, 7, 23, 14, 0, 0));
	TestFalse(TEXT("Refresh cannot change protected routing"),
				   RoutedEnvelope.TryApplyRefresh(MoveTemp(MismatchedRouting), NowUtc, Error));
	TestFalse(TEXT("Rejected mismatch wipes returned routing material"), MismatchedRouting.AccountRoutingValue.IsSet());

	FUnrealAISecretValue Persisted;
	TestTrue(TEXT("Rejected refreshes leave the prior envelope encodable"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(RoutedEnvelope), NowUtc, Persisted, Error));
	FUnrealAIOAuthTokenEnvelope Reloaded;
	TestTrue(TEXT("Prior envelope remains bound after rejected refreshes"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(Persisted), Binding, NowUtc, Reloaded, Error));
	FUnrealAISecretValue Bearer;
	FUnrealAISecretValue Routing;
	TestTrue(TEXT("Prior dispatch material remains usable"),
				  Reloaded.TryTakeDispatchCredentials(NowUtc, Bearer, Routing, Error));
	TestTrue(TEXT("Prior access token is unchanged"), VerifySecret(MoveTemp(Bearer), "access-original"));
	TestTrue(TEXT("Prior routing value is unchanged"), VerifySecret(MoveTemp(Routing), "account-route"));

	FUnrealAIOAuthTokenEnvelope UnroutedEnvelope;
	FUnrealAIOAuthTokenSet UnroutedInitial =
		MakeTokens("access-one", "refresh-one", nullptr, nullptr, FDateTime(2026, 7, 23, 13, 0, 0));
	TestTrue(TEXT("Unrouted fixture creates"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(UnroutedInitial), NowUtc,
															  UnroutedEnvelope, Error));
	FUnrealAIOAuthTokenSet UnroutedRefresh =
		MakeTokens("access-two", nullptr, nullptr, nullptr, FDateTime(2026, 7, 23, 14, 0, 0));
	TestTrue(TEXT("Exact absence of routing remains valid"),
				  UnroutedEnvelope.TryApplyRefresh(MoveTemp(UnroutedRefresh), NowUtc, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthTokenEnvelopeFailClosedTest,
								 "UnrealAI.Auth.OAuthTokenEnvelope.MalformedVersionBindingExpiryAndBoundsFailClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthTokenEnvelopeFailClosedTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FDateTime NowUtc(2026, 7, 23, 12, 0, 0);
	const FUnrealAIOAuthTokenEnvelopeBinding Binding = MakeBinding();
	FString Error;
	FUnrealAIOAuthTokenEnvelope Decoded;

	TArray<uint8> MalformedBytes{1, 2, 3, 4};
	FUnrealAISecretValue Malformed;
	TestTrue(TEXT("Malformed fixture enters through the secret boundary"),
				  FUnrealAISecretValue::TryCreate(MoveTemp(MalformedBytes), Malformed, Error));
	TestFalse(TEXT("Malformed envelope fails closed"),
				   FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(Malformed), Binding, NowUtc, Decoded, Error));
	TestFalse(TEXT("Malformed encoded input is consumed and wiped"), Malformed.IsSet());
	TestFalse(TEXT("Malformed input publishes no decoded envelope"), Decoded.IsSet());

	TArray<uint8> UnknownVersionBytes{'A', 'A', 'O', 'A', 'U', 'T', 'H', '\0', 2, 0};
	FUnrealAISecretValue UnknownVersion;
	TestTrue(TEXT("Unknown-version fixture enters through the secret boundary"),
				  FUnrealAISecretValue::TryCreate(MoveTemp(UnknownVersionBytes), UnknownVersion, Error));
	TestFalse(
		TEXT("Unknown envelope version fails closed"),
			 FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(UnknownVersion), Binding, NowUtc, Decoded, Error));
	TestTrue(TEXT("Unknown-version failure is classified without provider data"), Error.Contains(TEXT("version")));
	TestFalse(TEXT("Unknown-version input is consumed and wiped"), UnknownVersion.IsSet());

	for (int32 BindingCase = 0; BindingCase < 3; ++BindingCase)
	{
		FUnrealAISecretValue WrongBindingEncoded;
		TestTrue(TEXT("Valid wrong-binding fixture encodes"),
					  MakeEncodedEnvelope(Binding, NowUtc, FDateTime(2026, 7, 23, 13, 0, 0), WrongBindingEncoded));
		FUnrealAIOAuthTokenEnvelopeBinding WrongBinding = Binding;
		if (BindingCase == 0)
		{
			WrongBinding.ProviderName = TEXT("tests.oauth.other_provider");
		}
		else if (BindingCase == 1)
		{
			WrongBinding.AuthProfileId = TEXT("tests.oauth.other_profile");
		}
		else
		{
			WrongBinding.AccountId.Value.D ^= 1;
		}
		TestFalse(TEXT("Provider, auth-profile, and local-account mismatches fail before token materialization"),
					   FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(WrongBindingEncoded), WrongBinding, NowUtc,
																   Decoded, Error));
		TestFalse(TEXT("Wrong-binding encoded input is consumed and wiped"), WrongBindingEncoded.IsSet());
		TestFalse(TEXT("Wrong binding publishes no decoded envelope"), Decoded.IsSet());
	}

	FUnrealAISecretValue ExpiringEncoded;
	const FDateTime ExpiryUtc(2026, 7, 23, 12, 1, 0);
	TestTrue(TEXT("Future-expiry fixture encodes"), MakeEncodedEnvelope(Binding, NowUtc, ExpiryUtc, ExpiringEncoded));
	TestFalse(TEXT("Envelope is expired at its exact UTC expiry"),
				   FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(ExpiringEncoded), Binding, ExpiryUtc, Decoded,
															   Error));
	TestFalse(TEXT("Expired encoded input is consumed and wiped"), ExpiringEncoded.IsSet());

	FUnrealAIOAuthTokenSet AlreadyExpired = MakeTokens("access-expired", "refresh-expired", nullptr, nullptr, NowUtc);
	FUnrealAIOAuthTokenEnvelope Created;
	TestFalse(
		TEXT("Creation rejects access expiry at the current UTC instant"),
			 FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(AlreadyExpired), NowUtc, Created, Error));
	TestFalse(TEXT("Rejected expired access token is wiped"), AlreadyExpired.AccessToken.IsSet());
	TestFalse(TEXT("Rejected expired refresh token is wiped"), AlreadyExpired.RefreshToken.IsSet());

	struct FOversizedCase final
	{
		const TCHAR *Label;
		int32 Field;
		int32 Size;
	};
	const FOversizedCase OversizedCases[] = {
		{TEXT("access"), 0, FUnrealAIOAuthTokenSet::MaxAccessTokenBytes + 1},
		 {TEXT("refresh"), 1, FUnrealAIOAuthTokenSet::MaxRefreshTokenBytes + 1},
		  {TEXT("id"), 2, FUnrealAIOAuthTokenSet::MaxIdTokenBytes + 1},
		   {TEXT("routing"), 3, FUnrealAIOAuthTokenSet::MaxAccountRoutingBytes + 1},
		  };
	for (const FOversizedCase &Case : OversizedCases)
	{
		FUnrealAIOAuthTokenSet Oversized =
			MakeTokens("valid-access", "valid-refresh", "valid-id", "valid-route", FDateTime(2026, 7, 23, 13, 0, 0));
		FUnrealAISecretValue Replacement;
		TestTrue(
			*FString::Printf(TEXT("%s oversized fixture is representable by the generic secret boundary"), Case.Label),
							 MakeFilledSecret(Case.Size, 0x5a, Replacement));
		switch (Case.Field)
		{
		case 0:
			Oversized.AccessToken = MoveTemp(Replacement);
			break;
		case 1:
			Oversized.RefreshToken = MoveTemp(Replacement);
			break;
		case 2:
			Oversized.IdToken = MoveTemp(Replacement);
			break;
		default:
			Oversized.AccountRoutingValue = MoveTemp(Replacement);
			break;
		}
		TestFalse(*FString::Printf(TEXT("%s field bound fails closed"), Case.Label),
								   FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Oversized), NowUtc,
																			   Created, Error));
		TestFalse(*FString::Printf(TEXT("%s failure wipes access material"), Case.Label),
								   Oversized.AccessToken.IsSet());
		TestFalse(*FString::Printf(TEXT("%s failure wipes refresh material"), Case.Label),
								   Oversized.RefreshToken.IsSet());
		TestFalse(*FString::Printf(TEXT("%s failure wipes ID-token material"), Case.Label), Oversized.IdToken.IsSet());
		TestFalse(*FString::Printf(TEXT("%s failure wipes routing material"), Case.Label),
								   Oversized.AccountRoutingValue.IsSet());
		TestFalse(*FString::Printf(TEXT("%s failure publishes no envelope"), Case.Label), Created.IsSet());
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
