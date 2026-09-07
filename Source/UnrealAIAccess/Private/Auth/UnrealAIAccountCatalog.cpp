// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIAccountCatalog.h"

#include "Misc/ScopeLock.h"

namespace
{
bool IsStableCatalogIdentifier(const FName Name)
{
	if (Name.IsNone())
	{
		return false;
	}
	const FString Text = Name.ToString();
	FTCHARToUTF8 Utf8(*Text);
	if (Utf8.Length() < 1 || Utf8.Length() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'));
		if ((!bAlphaNumeric && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-')) ||
															 ((Index == 0 || Index == Text.Len() - 1) &&
															  !bAlphaNumeric))
		{
			return false;
		}
	}
	return true;
}

bool IsSafeDisplayLabel(const FString &Label)
{
	FTCHARToUTF8 Utf8(*Label);
	if (Label.IsEmpty() || Utf8.Length() < 1 || Utf8.Length() > FUnrealAIAccountCatalogEntry::MaxDisplayLabelUtf8Bytes)
	{
		return false;
	}
	for (const TCHAR Character : Label)
	{
		if (Character < TEXT(' ') || Character == 0x7f)
		{
			return false;
		}
	}
	return true;
}

bool DestinationsEqual(const FUnrealAICredentialDestination &A, const FUnrealAICredentialDestination &B)
{
	return A.ModelProviderName == B.ModelProviderName && A.AccountAuthProviderName == B.AccountAuthProviderName &&
		   A.AuthProfileId == B.AuthProfileId && A.AccountId == B.AccountId && A.TenantRealm == B.TenantRealm &&
		   A.BillingPrincipalId == B.BillingPrincipalId && A.PayerHandle == B.PayerHandle &&
		   A.AuthScheme == B.AuthScheme && A.BillingMode == B.BillingMode && A.EndpointOrigin == B.EndpointOrigin &&
		   A.Audience == B.Audience && A.ConnectionRevision == B.ConnectionRevision &&
		   A.EndpointPolicyRevision == B.EndpointPolicyRevision;
}

bool AccessStaticFieldsEqual(const FUnrealAIProviderAccessDescriptor &A, const FUnrealAIProviderAccessDescriptor &B)
{
	return A.ModelProviderName == B.ModelProviderName && A.AccountAuthProviderName == B.AccountAuthProviderName &&
		   A.AuthScheme == B.AuthScheme && A.BillingMode == B.BillingMode &&
		   A.SupportClassification == B.SupportClassification;
}

FUnrealAIProviderAccessError MakeCatalogError(const EUnrealAIErrorCategory Category,
											  const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}
} // namespace

bool FUnrealAIAccountCatalogEntry::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableCatalogIdentifier(SelectionAlias) || !IsSafeDisplayLabel(DisplayLabel) ||
		!IsStableCatalogIdentifier(ProviderName) || !IsStableCatalogIdentifier(AuthProfileId) || !AccountId.IsValid() ||
		!IsStableCatalogIdentifier(ConnectionAlias))
	{
		OutError = TEXT("Account catalog entry requires bounded stable aliases, a safe product label, and an opaque local account ID.");
		return false;
	}
	return true;
}

struct FUnrealAIAccountCatalogRecord final
{
	FUnrealAIAccountCatalogEntry Entry;
	FUnrealAIProviderAccessDescriptor RegisteredAccess;
	FUnrealAICredentialDestination RegisteredDestination;
	TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider;
	TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection;
	TAtomic<bool> bActive{true};

	FUnrealAIAccountCatalogRecord(const FUnrealAIAccountCatalogEntry &InEntry,
								  const FUnrealAIProviderAccessDescriptor &InRegisteredAccess,
								  const FUnrealAICredentialDestination &InRegisteredDestination,
								  TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> InProvider,
								  TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> InConnection)
		: Entry(InEntry), RegisteredAccess(InRegisteredAccess), RegisteredDestination(InRegisteredDestination),
		  Provider(MoveTemp(InProvider)), Connection(MoveTemp(InConnection))
	{
	}
};

struct FUnrealAIAccountCatalogSnapshot::FImpl
{
	TMap<FName, TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe>> Entries;
};

struct FUnrealAIAccountCatalogRegistry::FImpl
{
	explicit FImpl(FUnrealAIAccountAuthProviderRegistry &InAuthProviders) : AuthProviders(&InAuthProviders) {}

	FUnrealAIAccountAuthProviderRegistry *AuthProviders = nullptr;
	mutable FCriticalSection Mutex;
	TMap<FName, TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe>> Entries;
	uint64 Generation = 0;
};

namespace
{
FUnrealAIAccountCatalogView MakeView(const FUnrealAIAccountCatalogRecord &Record)
{
	FUnrealAIAccountCatalogView View;
	View.Entry = Record.Entry;
	View.Destination = Record.RegisteredDestination;
	if (!Record.bActive.Load())
	{
		return View;
	}

	View.Access = Record.Provider->DescribeAccess();
	View.Status = Record.Provider->GetStatus(Record.Entry.AuthProfileId, Record.Entry.AccountId);
	FString AccessError;
	FString StatusError;
	FString ConnectionError;
	const bool bStatusAccountMatches = View.Status.State == EUnrealAIAccountAuthState::SignedOut
										   ? !View.Status.AccountId.IsValid()
										   : View.Status.AccountId == Record.Entry.AccountId;
	View.bBindingCurrent = Record.bActive.Load() && Record.Provider->GetProviderName() == Record.Entry.ProviderName &&
						   View.Access.ValidateShape(AccessError) && View.Status.ValidateShape(StatusError) &&
						   Record.Connection->ValidateShape(ConnectionError) &&
						   Record.Connection->ConnectionAlias == Record.Entry.ConnectionAlias &&
						   DestinationsEqual(Record.Connection->CredentialDestination, Record.RegisteredDestination) &&
						   AccessStaticFieldsEqual(View.Access, Record.RegisteredAccess) &&
						   View.Status.ProviderName == Record.Entry.ProviderName &&
						   View.Status.AuthProfileId == Record.Entry.AuthProfileId && bStatusAccountMatches;
	return View;
}

bool ViewLess(const FUnrealAIAccountCatalogView &A, const FUnrealAIAccountCatalogView &B)
{
	return A.Entry.SelectionAlias.ToString() < B.Entry.SelectionAlias.ToString();
}
} // namespace

FUnrealAIAccountCatalogSnapshot::FUnrealAIAccountCatalogSnapshot(TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl,
																 const uint64 InGeneration)
	: Impl(MoveTemp(InImpl)), Generation(InGeneration)
{
}

TArray<FUnrealAIAccountCatalogView> FUnrealAIAccountCatalogSnapshot::List() const
{
	TArray<FUnrealAIAccountCatalogView> Result;
	Result.Reserve(Impl->Entries.Num());
	for (const TPair<FName, TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe>> &Pair : Impl->Entries)
	{
		if (Pair.Value->bActive.Load())
		{
			Result.Add(MakeView(Pair.Value.Get()));
		}
	}
	Result.Sort(ViewLess);
	return Result;
}

bool FUnrealAIAccountCatalogSnapshot::ResolveReadyExact(const FName SelectionAlias,
														FUnrealAIAccountCatalogView &OutSelection,
														FUnrealAIProviderAccessError &OutError) const
{
	OutSelection = {};
	OutError = {};
	const TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe> *Found = Impl->Entries.Find(SelectionAlias);
	if (Found == nullptr)
	{
		OutError = MakeCatalogError(EUnrealAIErrorCategory::NotFound, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	if (!Found->Get().bActive.Load())
	{
		OutError = MakeCatalogError(EUnrealAIErrorCategory::InvalidConfiguration,
									EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	OutSelection = MakeView(Found->Get());
	if (!OutSelection.bBindingCurrent)
	{
		OutSelection = {};
		OutError = MakeCatalogError(EUnrealAIErrorCategory::InvalidConfiguration,
									EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	if (OutSelection.Status.State != EUnrealAIAccountAuthState::Ready ||
		OutSelection.Access.Availability != EUnrealAIProviderAccessAvailability::Available)
	{
		OutSelection = {};
		OutError = MakeCatalogError(EUnrealAIErrorCategory::NotAuthorized,
									EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
		return false;
	}
	return true;
}

int32 FUnrealAIAccountCatalogSnapshot::Num() const
{
	int32 Count = 0;
	for (const TPair<FName, TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe>> &Pair : Impl->Entries)
	{
		Count += Pair.Value->bActive.Load() ? 1 : 0;
	}
	return Count;
}

FUnrealAIAccountCatalogRegistry::FUnrealAIAccountCatalogRegistry(FUnrealAIAccountAuthProviderRegistry &InAuthProviders)
	: Impl(MakeUnique<FImpl>(InAuthProviders))
{
}

FUnrealAIAccountCatalogRegistry::~FUnrealAIAccountCatalogRegistry()
{
	FScopeLock Lock(&Impl->Mutex);
	for (const TPair<FName, TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe>> &Pair : Impl->Entries)
	{
		Pair.Value->bActive.Store(false);
	}
}

bool FUnrealAIAccountCatalogRegistry::Register(
	const FUnrealAIAccountCatalogEntry &Entry, TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider,
	TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection, FString &OutError)
{
	OutError.Reset();
	FString ShapeError;
	const FUnrealAIProviderAccessDescriptor Access = Provider->DescribeAccess();
	const FUnrealAIAccountStatus Status = Provider->GetStatus(Entry.AuthProfileId, Entry.AccountId);
	const FUnrealAICredentialDestination &Destination = Connection->CredentialDestination;
	const TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> RegisteredProvider =
		Impl->AuthProviders->Find(Entry.ProviderName);
	const bool bExactProvider = RegisteredProvider.IsValid() && RegisteredProvider.Get() == &Provider.Get();
	const bool bStatusAccountMatches = Status.State == EUnrealAIAccountAuthState::SignedOut
										   ? !Status.AccountId.IsValid()
										   : Status.AccountId == Entry.AccountId;
	if (!Entry.ValidateShape(ShapeError) || !Connection->ValidateShape(ShapeError) ||
		!Access.ValidateShape(ShapeError) || !Status.ValidateShape(ShapeError) || !bExactProvider ||
		Provider->GetProviderName() != Entry.ProviderName || Connection->ConnectionAlias != Entry.ConnectionAlias ||
		Destination.AccountAuthProviderName != Entry.ProviderName || Destination.AuthProfileId != Entry.AuthProfileId ||
		!(Destination.AccountId == Entry.AccountId) || Access.ModelProviderName != Destination.ModelProviderName ||
		Access.AccountAuthProviderName != Destination.AccountAuthProviderName ||
		Access.AuthScheme != Destination.AuthScheme || Access.BillingMode != Destination.BillingMode ||
		!bStatusAccountMatches)
	{
		OutError = TEXT("Account catalog rejected provider, account, connection, payer, or destination drift.");
		return false;
	}

	FScopeLock Lock(&Impl->Mutex);
	if (Impl->Entries.Contains(Entry.SelectionAlias))
	{
		OutError = TEXT("Account catalog rejected a duplicate exact selection alias.");
		return false;
	}
	if (Impl->Entries.Num() >= MaxEntries)
	{
		OutError = TEXT("Account catalog reached its bounded capacity.");
		return false;
	}
	Impl->Entries.Add(Entry.SelectionAlias, MakeShared<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe>(
												Entry, Access, Destination, MoveTemp(Provider), MoveTemp(Connection)));
	++Impl->Generation;
	return true;
}

bool FUnrealAIAccountCatalogRegistry::Unregister(const FName SelectionAlias,
												 TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider,
												 FString &OutError)
{
	OutError.Reset();
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<FUnrealAIAccountCatalogRecord, ESPMode::ThreadSafe> *Found = Impl->Entries.Find(SelectionAlias);
	if (Found == nullptr)
	{
		OutError = TEXT("Account catalog cannot unregister an absent selection alias.");
		return false;
	}
	if (&Found->Get().Provider.Get() != &Provider.Get())
	{
		OutError = TEXT("Account catalog rejected removal by a different provider instance.");
		return false;
	}
	Found->Get().bActive.Store(false);
	Impl->Entries.Remove(SelectionAlias);
	++Impl->Generation;
	return true;
}

TSharedRef<const FUnrealAIAccountCatalogSnapshot, ESPMode::ThreadSafe>
FUnrealAIAccountCatalogRegistry::CreateSnapshot() const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<FUnrealAIAccountCatalogSnapshot::FImpl, ESPMode::ThreadSafe> SnapshotImpl =
		MakeShared<FUnrealAIAccountCatalogSnapshot::FImpl, ESPMode::ThreadSafe>();
	SnapshotImpl->Entries = Impl->Entries;
	return MakeShareable(new FUnrealAIAccountCatalogSnapshot(SnapshotImpl, Impl->Generation));
}

int32 FUnrealAIAccountCatalogRegistry::Num() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Entries.Num();
}

uint64 FUnrealAIAccountCatalogRegistry::GetGeneration() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Generation;
}
