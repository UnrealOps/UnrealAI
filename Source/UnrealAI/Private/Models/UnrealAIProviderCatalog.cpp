// Copyright UnrealOps. All Rights Reserved.

#include "Models/UnrealAIProviderCatalog.h"
#include "Misc/ScopeLock.h"

bool FUnrealAIProviderRegistration::Validate(FString &OutError) const
{
	OutError.Reset();
	if (ProviderName.IsNone() || DefaultConnectionAlias.IsNone() || !Provider.IsValid() || !Connections.IsValid() ||
		!CredentialBroker.IsValid() || Provider->GetProviderName() != ProviderName || !Access.ValidateShape(OutError) ||
		Access.ModelProviderName != ProviderName)
	{
		OutError = TEXT("Provider registration requires a matching native provider, broker, and connection snapshot.");
		return false;
	}
	const auto Connection = Connections->Find(DefaultConnectionAlias);
	if (!Connection.IsValid() || Connection->CredentialDestination.ModelProviderName != ProviderName ||
		!Connection->ValidateShape(OutError) || !Provider->Describe().ValidateShape(OutError))
	{
		OutError = TEXT("Provider registration does not contain its exact default connection.");
		return false;
	}
	return true;
}

FUnrealAIProviderCatalog &FUnrealAIProviderCatalog::Get()
{
	// Native services can outlive UObject owners; the module and catalog remain resident until process exit.
	static FUnrealAIProviderCatalog *Catalog = new FUnrealAIProviderCatalog();
	return *Catalog;
}

bool FUnrealAIProviderCatalog::Register(
	TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Registration, FString &OutError)
{
	if (!Registration->Validate(OutError))
	{
		return false;
	}
	FScopeLock Lock(&Mutex);
	if (Entries.Num() >= MaximumProviders || Entries.Contains(Registration->ProviderName))
	{
		OutError = TEXT("The provider catalog is full or that provider is already registered.");
		return false;
	}
	const FName Name = Registration->ProviderName;
	Entries.Add(Name, FEntry{Registration,
							 MakeShared<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>(*Registration)});
	return true;
}

bool FUnrealAIProviderCatalog::Unregister(
	TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Registration)
{
	TSharedPtr<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Removed;
	{
		FScopeLock Lock(&Mutex);
		for (auto Iterator = Entries.CreateIterator(); Iterator; ++Iterator)
		{
			if (&Iterator.Value().Owner.Get() == &Registration.Get())
			{
				Removed = Iterator.Value().Value;
				Iterator.RemoveCurrent();
				break;
			}
		}
		if (!Removed.IsValid())
		{
			return false;
		}
	}
	// Provider/credential destructors may reenter the catalog.
	Removed.Reset();
	return true;
}

TSharedPtr<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>
FUnrealAIProviderCatalog::Find(FName ProviderName) const
{
	FScopeLock Lock(&Mutex);
	const auto *Entry = Entries.Find(ProviderName);
	return Entry ? TSharedPtr<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>(Entry->Value) : nullptr;
}

TArray<TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>> FUnrealAIProviderCatalog::Snapshot() const
{
	FScopeLock Lock(&Mutex);
	TArray<TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>> Result;
	for (const auto &Pair : Entries)
	{
		Result.Add(Pair.Value.Value);
	}
	Result.Sort([](const auto &A, const auto &B) { return A->ProviderName.LexicalLess(B->ProviderName); });
	return Result;
}
