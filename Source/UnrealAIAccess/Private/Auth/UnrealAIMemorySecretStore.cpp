// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIMemorySecretStore.h"

#include "Misc/ScopeLock.h"

namespace
{
void SetStoreError(FUnrealAIProviderAccessError &OutError, const EUnrealAIErrorCategory Category,
				   const EUnrealAIProviderAccessErrorCode Code)
{
	OutError = FUnrealAIProviderAccessError{};
	OutError.Category = Category;
	OutError.Code = Code;
}

TOptional<EUnrealAISecretStoreResult> CheckOperation(const FUnrealAISecretStoreOperationContext &Context,
													 FUnrealAIProviderAccessError &OutError)
{
	FString ContextError;
	if (!Context.ValidateShape(ContextError))
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext);
		return EUnrealAISecretStoreResult::Failed;
	}
	if (Context.IsCancellationRequested())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Cancelled,
					  EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
		return EUnrealAISecretStoreResult::Cancelled;
	}
	if (Context.IsTimedOut())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut);
		return EUnrealAISecretStoreResult::TimedOut;
	}
	return {};
}
} // namespace

struct FUnrealAIMemorySecretStore::FImpl
{
	struct FRecord final
	{
		FUnrealAISecretValue Value;
		uint64 Revision = 0;
	};

	FName StoreName;
	int32 MaxRecords = 0;
	mutable FCriticalSection Mutex;
	TMap<FUnrealAISecretHandle, TUniquePtr<FRecord>> Records;
};

FUnrealAIMemorySecretStore::FUnrealAIMemorySecretStore(const FName InStoreName, const int32 InMaxRecords)
	: Impl(MakeUnique<FImpl>())
{
	Impl->StoreName = InStoreName;
	Impl->MaxRecords = FMath::Clamp(InMaxRecords, 1, HardMaxRecords);
}

FUnrealAIMemorySecretStore::~FUnrealAIMemorySecretStore()
{
	Clear();
}

FName FUnrealAIMemorySecretStore::GetStoreName() const
{
	return Impl->StoreName;
}

FUnrealAISecretStoreCapabilities FUnrealAIMemorySecretStore::DescribeCapabilities() const
{
	FUnrealAISecretStoreCapabilities Capabilities;
	Capabilities.PersistenceClass = EUnrealAISecretStorePersistenceClass::Volatile;
	Capabilities.ProtectionClass = EUnrealAISecretStoreProtectionClass::ProcessMemory;
	Capabilities.ScopeClass = EUnrealAISecretStoreScopeClass::Process;
	Capabilities.bAvailableInCurrentBuild = true;
	Capabilities.bPersistent = false;
	Capabilities.bHardwareBackedWhenAvailable = false;
	Capabilities.bAtomicCompareAndSwap = true;
	// The implementation compiles in Shipping for explicitly ephemeral broker state,
	// but IsProductionProtected deliberately remains false.
	Capabilities.bAvailableInShipping = true;
	return Capabilities;
}

EUnrealAISecretStoreResult FUnrealAIMemorySecretStore::Load(const FUnrealAISecretStoreOperationContext &Context,
															const FUnrealAISecretHandle &Handle,
															FUnrealAISecretValue &OutValue, uint64 &OutRevision,
															FUnrealAIProviderAccessError &OutError)
{
	OutValue.Reset();
	OutRevision = 0;
	OutError = FUnrealAIProviderAccessError{};
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	FString ShapeError;
	if (!Handle.ValidateShape(ShapeError) || Handle.StoreName != Impl->StoreName)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch);
		return EUnrealAISecretStoreResult::Failed;
	}

	TArray<uint8> Copy;
	{
		FScopeLock Lock(&Impl->Mutex);
		const TUniquePtr<FImpl::FRecord> *Found = Impl->Records.Find(Handle);
		if (Found == nullptr)
		{
			SetStoreError(OutError, EUnrealAIErrorCategory::NotFound, EUnrealAIProviderAccessErrorCode::SecretNotFound);
			return EUnrealAISecretStoreResult::NotFound;
		}
		const TConstArrayView<uint8> Bytes = ViewSecret((*Found)->Value);
		Copy.Append(Bytes.GetData(), Bytes.Num());
		OutRevision = (*Found)->Revision;
	}

	if (!FUnrealAISecretValue::TryCreate(MoveTemp(Copy), OutValue, ShapeError))
	{
		OutRevision = 0;
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::SecretCopyFailed);
		return EUnrealAISecretStoreResult::Failed;
	}
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		OutValue.Reset();
		OutRevision = 0;
		return Operation.GetValue();
	}
	return EUnrealAISecretStoreResult::Succeeded;
}

EUnrealAISecretStoreResult FUnrealAIMemorySecretStore::Store(const FUnrealAISecretStoreOperationContext &Context,
															 const FUnrealAISecretHandle &Handle,
															 const FUnrealAISecretValue &Value,
															 const uint64 ExpectedRevision, uint64 &OutNewRevision,
															 FUnrealAIProviderAccessError &OutError)
{
	OutNewRevision = 0;
	OutError = FUnrealAIProviderAccessError{};
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	FString ShapeError;
	if (!Handle.ValidateShape(ShapeError) || Handle.StoreName != Impl->StoreName || !Value.IsSet())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::InvalidSecretStoreWrite);
		return EUnrealAISecretStoreResult::Failed;
	}

	TArray<uint8> Copy;
	const TConstArrayView<uint8> Bytes = ViewSecret(Value);
	Copy.Append(Bytes.GetData(), Bytes.Num());
	FUnrealAISecretValue OwnedCopy;
	if (!FUnrealAISecretValue::TryCreate(MoveTemp(Copy), OwnedCopy, ShapeError))
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::SecretCopyFailed);
		return EUnrealAISecretStoreResult::Failed;
	}
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}

	FScopeLock Lock(&Impl->Mutex);
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	TUniquePtr<FImpl::FRecord> *Existing = Impl->Records.Find(Handle);
	if (Existing == nullptr)
	{
		if (ExpectedRevision != 0)
		{
			SetStoreError(OutError, EUnrealAIErrorCategory::VersionMismatch,
						  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
			return EUnrealAISecretStoreResult::Conflict;
		}
		if (Impl->Records.Num() >= Impl->MaxRecords)
		{
			SetStoreError(OutError, EUnrealAIErrorCategory::Busy,
						  EUnrealAIProviderAccessErrorCode::SecretStoreCapacity);
			return EUnrealAISecretStoreResult::Failed;
		}
		TUniquePtr<FImpl::FRecord> Record = MakeUnique<FImpl::FRecord>();
		Record->Value = MoveTemp(OwnedCopy);
		Record->Revision = 1;
		OutNewRevision = Record->Revision;
		Impl->Records.Add(Handle, MoveTemp(Record));
		return EUnrealAISecretStoreResult::Succeeded;
	}

	if (ExpectedRevision == 0 || (*Existing)->Revision != ExpectedRevision ||
		(*Existing)->Revision == TNumericLimits<uint64>::Max())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::VersionMismatch,
					  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
		return EUnrealAISecretStoreResult::Conflict;
	}
	(*Existing)->Value = MoveTemp(OwnedCopy);
	++(*Existing)->Revision;
	OutNewRevision = (*Existing)->Revision;
	return EUnrealAISecretStoreResult::Succeeded;
}

EUnrealAISecretStoreResult FUnrealAIMemorySecretStore::Delete(const FUnrealAISecretStoreOperationContext &Context,
															  const FUnrealAISecretHandle &Handle,
															  const uint64 ExpectedRevision,
															  FUnrealAIProviderAccessError &OutError)
{
	OutError = FUnrealAIProviderAccessError{};
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	FString ShapeError;
	if (!Handle.ValidateShape(ShapeError) || Handle.StoreName != Impl->StoreName || ExpectedRevision == 0)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::InvalidSecretStoreDelete);
		return EUnrealAISecretStoreResult::Failed;
	}

	FScopeLock Lock(&Impl->Mutex);
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	const TUniquePtr<FImpl::FRecord> *Existing = Impl->Records.Find(Handle);
	if (Existing == nullptr)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::NotFound, EUnrealAIProviderAccessErrorCode::SecretNotFound);
		return EUnrealAISecretStoreResult::NotFound;
	}
	if ((*Existing)->Revision != ExpectedRevision)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::VersionMismatch,
					  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
		return EUnrealAISecretStoreResult::Conflict;
	}
	Impl->Records.Remove(Handle);
	return EUnrealAISecretStoreResult::Succeeded;
}

void FUnrealAIMemorySecretStore::Clear()
{
	FScopeLock Lock(&Impl->Mutex);
	Impl->Records.Reset();
}

int32 FUnrealAIMemorySecretStore::Num() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Records.Num();
}
