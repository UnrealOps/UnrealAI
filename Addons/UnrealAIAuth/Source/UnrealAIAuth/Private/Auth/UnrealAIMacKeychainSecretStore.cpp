// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIMacKeychainSecretStore.h"

#include "Misc/ScopeLock.h"

#if PLATFORM_APPLE
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace
{
#if PLATFORM_MAC
constexpr TCHAR KeychainStoreName[] = TEXT("macos.keychain");
#else
constexpr TCHAR KeychainStoreName[] = TEXT("apple.keychain");
#endif

void SetStoreError(FUnrealAIProviderAccessError &OutError, const EUnrealAIErrorCategory Category,
				   const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false)
{
	OutError = FUnrealAIProviderAccessError{};
	OutError.Category = Category;
	OutError.Code = Code;
	OutError.bRetryable = bRetryable;
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
		SetStoreError(OutError, EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut,
					  true);
		return EUnrealAISecretStoreResult::TimedOut;
	}
	return {};
}

bool ValidateHandle(const FUnrealAISecretHandle &Handle)
{
	FString ShapeError;
	return Handle.ValidateShape(ShapeError) && Handle.StoreName == FUnrealAIMacKeychainSecretStore::StoreName();
}

#if PLATFORM_APPLE
template <typename Type> class TScopedCF final
{
  public:
	explicit TScopedCF(Type InValue = nullptr) : Value(InValue) {}
	~TScopedCF()
	{
		if (Value != nullptr)
		{
			CFRelease(Value);
		}
	}
	TScopedCF(const TScopedCF &) = delete;
	TScopedCF &operator=(const TScopedCF &) = delete;
	TScopedCF(TScopedCF &&Other) noexcept : Value(Other.Value)
	{
		Other.Value = nullptr;
	}
	TScopedCF &operator=(TScopedCF &&Other) noexcept
	{
		if (this != &Other)
		{
			if (Value != nullptr)
			{
				CFRelease(Value);
			}
			Value = Other.Value;
			Other.Value = nullptr;
		}
		return *this;
	}
	Type Get() const
	{
		return Value;
	}
	Type *Out()
	{
		if (Value != nullptr)
		{
			CFRelease(Value);
			Value = nullptr;
		}
		return &Value;
	}

  private:
	Type Value;
};

CFStringRef MakeCFString(const FString &Value)
{
	FTCHARToUTF8 Utf8(*Value);
	return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(Utf8.Get()), Utf8.Length(),
								   kCFStringEncodingUTF8, false);
}

CFDataRef MakeRevisionData(const uint64 Revision)
{
	uint8 Bytes[8];
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Bytes[Index] = static_cast<uint8>((Revision >> ((7 - Index) * 8)) & 0xff);
	}
	return CFDataCreate(kCFAllocatorDefault, Bytes, UE_ARRAY_COUNT(Bytes));
}

bool ReadRevision(CFTypeRef Value, uint64 &OutRevision)
{
	OutRevision = 0;
	if (Value == nullptr || CFGetTypeID(Value) != CFDataGetTypeID())
	{
		return false;
	}
	const CFDataRef Data = static_cast<CFDataRef>(Value);
	if (CFDataGetLength(Data) != 8)
	{
		return false;
	}
	const UInt8 *Bytes = CFDataGetBytePtr(Data);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		OutRevision = (OutRevision << 8) | Bytes[Index];
	}
	return OutRevision != 0;
}

TScopedCF<CFMutableDictionaryRef> MakeBaseQuery(const FUnrealAISecretHandle &Handle)
{
	TScopedCF<CFMutableDictionaryRef> Query(CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
	if (Query.Get() == nullptr)
	{
		return Query;
	}
	TScopedCF<CFStringRef> Service(MakeCFString(TEXT("com.unrealops.unrealai")));
	TScopedCF<CFStringRef> Account(MakeCFString(Handle.Value.ToString(EGuidFormats::Digits)));
	if (Service.Get() == nullptr || Account.Get() == nullptr)
	{
		return TScopedCF<CFMutableDictionaryRef>();
	}
	CFDictionarySetValue(Query.Get(), kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(Query.Get(), kSecAttrService, Service.Get());
	CFDictionarySetValue(Query.Get(), kSecAttrAccount, Account.Get());
	// A plugin cannot require every Unreal host to carry the application-identifier entitlement needed by the data
	// protection Keychain. The standard macOS Keychain remains host-access-controlled and accepts the device-only
	// accessibility policy applied when a record is created.
	return Query;
}

EUnrealAISecretStoreResult MapStatus(const OSStatus Status, FUnrealAIProviderAccessError &OutError,
									 const bool bConflictForMissing = false)
{
	switch (Status)
	{
	case errSecSuccess:
		return EUnrealAISecretStoreResult::Succeeded;
	case errSecItemNotFound:
		SetStoreError(OutError,
					  bConflictForMissing ? EUnrealAIErrorCategory::VersionMismatch : EUnrealAIErrorCategory::NotFound,
					  bConflictForMissing ? EUnrealAIProviderAccessErrorCode::SecretRevisionConflict
										  : EUnrealAIProviderAccessErrorCode::SecretNotFound);
		return bConflictForMissing ? EUnrealAISecretStoreResult::Conflict : EUnrealAISecretStoreResult::NotFound;
	case errSecDuplicateItem:
		SetStoreError(OutError, EUnrealAIErrorCategory::VersionMismatch,
					  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
		return EUnrealAISecretStoreResult::Conflict;
	case errSecInteractionNotAllowed:
		SetStoreError(OutError, EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::SecretStoreLocked,
					  true);
		return EUnrealAISecretStoreResult::Locked;
	case errSecAuthFailed:
	case errSecMissingEntitlement:
		SetStoreError(OutError, EUnrealAIErrorCategory::PolicyDenied,
					  EUnrealAIProviderAccessErrorCode::SecretStoreDenied);
		return EUnrealAISecretStoreResult::Denied;
	case errSecUserCanceled:
		SetStoreError(OutError, EUnrealAIErrorCategory::Cancelled,
					  EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
		return EUnrealAISecretStoreResult::Cancelled;
	case errSecNotAvailable:
		SetStoreError(OutError, EUnrealAIErrorCategory::Persistence,
					  EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true);
		return EUnrealAISecretStoreResult::Unavailable;
	case errSecDecode:
		SetStoreError(OutError, EUnrealAIErrorCategory::Persistence,
					  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
		return EUnrealAISecretStoreResult::Corrupt;
	default:
		SetStoreError(OutError, EUnrealAIErrorCategory::Persistence,
					  EUnrealAIProviderAccessErrorCode::CredentialFailed);
		return EUnrealAISecretStoreResult::Failed;
	}
}

#endif
} // namespace

struct FUnrealAIMacKeychainSecretStore::FImpl
{
	FCriticalSection MutationMutex;

#if PLATFORM_APPLE
	OSStatus InvokeSecurityCall(TFunctionRef<OSStatus()> PlatformCall)
	{
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
		TOptional<EAutomationPlatformStatus> StatusOverride;
		TFunction<void()> AfterPlatformCall;
		{
			FScopeLock Lock(&AutomationMutex);
			StatusOverride = AutomationStatusOverride;
			AutomationStatusOverride.Reset();
			AfterPlatformCall = MoveTemp(AutomationAfterPlatformCall);
		}
		const OSStatus Status = StatusOverride.IsSet() ? ToPlatformStatus(StatusOverride.GetValue()) : PlatformCall();
		if (AfterPlatformCall)
		{
			AfterPlatformCall();
		}
		return Status;
#else
		return PlatformCall();
#endif
	}
#endif

#if PLATFORM_APPLE && (WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS)
	static OSStatus ToPlatformStatus(const EAutomationPlatformStatus Status)
	{
		switch (Status)
		{
		case EAutomationPlatformStatus::Succeeded:
			return errSecSuccess;
		case EAutomationPlatformStatus::InteractionNotAllowed:
			return errSecInteractionNotAllowed;
		case EAutomationPlatformStatus::AuthFailed:
			return errSecAuthFailed;
		case EAutomationPlatformStatus::NotAvailable:
			return errSecNotAvailable;
		case EAutomationPlatformStatus::Decode:
			return errSecDecode;
		default:
			return errSecParam;
		}
	}

	void SetAutomationPlatformCallOverride(const EAutomationPlatformStatus Status, TFunction<void()> AfterPlatformCall)
	{
		FScopeLock Lock(&AutomationMutex);
		AutomationStatusOverride = Status;
		AutomationAfterPlatformCall = MoveTemp(AfterPlatformCall);
	}

	FCriticalSection AutomationMutex;
	TOptional<EAutomationPlatformStatus> AutomationStatusOverride;
	TFunction<void()> AutomationAfterPlatformCall;
#endif
};

FUnrealAIMacKeychainSecretStore::FUnrealAIMacKeychainSecretStore() : Impl(MakeUnique<FImpl>()) {}

FUnrealAIMacKeychainSecretStore::~FUnrealAIMacKeychainSecretStore() = default;

FName FUnrealAIMacKeychainSecretStore::StoreName()
{
	return KeychainStoreName;
}

bool FUnrealAIMacKeychainSecretStore::IsPlatformSupported()
{
#if PLATFORM_APPLE
	return true;
#else
	return false;
#endif
}

#if PLATFORM_APPLE && (WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS)
void FUnrealAIMacKeychainSecretStore::SetAutomationPlatformCallOverride(const EAutomationPlatformStatus Status,
																		TFunction<void()> AfterPlatformCall)
{
	Impl->SetAutomationPlatformCallOverride(Status, MoveTemp(AfterPlatformCall));
}
#endif

FName FUnrealAIMacKeychainSecretStore::GetStoreName() const
{
	return StoreName();
}

FUnrealAISecretStoreCapabilities FUnrealAIMacKeychainSecretStore::DescribeCapabilities() const
{
	FUnrealAISecretStoreCapabilities Capabilities;
	if (!IsPlatformSupported())
	{
		return Capabilities;
	}
	Capabilities.PersistenceClass = EUnrealAISecretStorePersistenceClass::Persistent;
	Capabilities.ProtectionClass = EUnrealAISecretStoreProtectionClass::PlatformCredentialStore;
	Capabilities.ScopeClass = EUnrealAISecretStoreScopeClass::CurrentUser;
	Capabilities.bAvailableInCurrentBuild = true;
	Capabilities.bPersistent = true;
	Capabilities.bHardwareBackedWhenAvailable = false;
	Capabilities.bAtomicCompareAndSwap = true;
	Capabilities.bAvailableInShipping = true;
	return Capabilities;
}

EUnrealAISecretStoreResult FUnrealAIMacKeychainSecretStore::Load(const FUnrealAISecretStoreOperationContext &Context,
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
	if (!ValidateHandle(Handle))
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch);
		return EUnrealAISecretStoreResult::Failed;
	}
#if !PLATFORM_APPLE
	SetStoreError(OutError, EUnrealAIErrorCategory::UnsupportedCapability,
				  EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
	return EUnrealAISecretStoreResult::NotSupported;
#else
	TScopedCF<CFMutableDictionaryRef> Query = MakeBaseQuery(Handle);
	if (Query.Get() == nullptr)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return EUnrealAISecretStoreResult::Failed;
	}
	CFDictionarySetValue(Query.Get(), kSecMatchLimit, kSecMatchLimitOne);
	CFDictionarySetValue(Query.Get(), kSecReturnAttributes, kCFBooleanTrue);
	CFDictionarySetValue(Query.Get(), kSecReturnData, kCFBooleanTrue);
	TScopedCF<CFTypeRef> Result;
	const OSStatus Status =
		Impl->InvokeSecurityCall([&Query, &Result]() { return SecItemCopyMatching(Query.Get(), Result.Out()); });
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	if (Status != errSecSuccess)
	{
		return MapStatus(Status, OutError);
	}
	if (Result.Get() == nullptr || CFGetTypeID(Result.Get()) != CFDictionaryGetTypeID())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Persistence,
					  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
		return EUnrealAISecretStoreResult::Corrupt;
	}
	const CFDictionaryRef Attributes = static_cast<CFDictionaryRef>(Result.Get());
	const CFTypeRef DataValue = CFDictionaryGetValue(Attributes, kSecValueData);
	const CFTypeRef RevisionValue = CFDictionaryGetValue(Attributes, kSecAttrGeneric);
	if (DataValue == nullptr || CFGetTypeID(DataValue) != CFDataGetTypeID() ||
		!ReadRevision(RevisionValue, OutRevision))
	{
		OutRevision = 0;
		SetStoreError(OutError, EUnrealAIErrorCategory::Persistence,
					  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
		return EUnrealAISecretStoreResult::Corrupt;
	}
	const CFDataRef Data = static_cast<CFDataRef>(DataValue);
	const CFIndex Length = CFDataGetLength(Data);
	if (Length <= 0 || Length > FUnrealAISecretValue::MaxSecretBytes)
	{
		OutRevision = 0;
		SetStoreError(OutError, EUnrealAIErrorCategory::Persistence,
					  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
		return EUnrealAISecretStoreResult::Corrupt;
	}
	TArray<uint8> Bytes;
	Bytes.Append(CFDataGetBytePtr(Data), static_cast<int32>(Length));
	FString ShapeError;
	if (!FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutValue, ShapeError))
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
#endif
}

EUnrealAISecretStoreResult FUnrealAIMacKeychainSecretStore::Store(const FUnrealAISecretStoreOperationContext &Context,
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
	if (!ValidateHandle(Handle) || !Value.IsSet())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::InvalidSecretStoreWrite);
		return EUnrealAISecretStoreResult::Failed;
	}
#if !PLATFORM_APPLE
	SetStoreError(OutError, EUnrealAIErrorCategory::UnsupportedCapability,
				  EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
	return EUnrealAISecretStoreResult::NotSupported;
#else
	if (ExpectedRevision == TNumericLimits<uint64>::Max())
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::VersionMismatch,
					  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
		return EUnrealAISecretStoreResult::Conflict;
	}
	const TConstArrayView<uint8> Secret = ViewSecret(Value);
	TScopedCF<CFDataRef> SecretData(CFDataCreate(kCFAllocatorDefault, Secret.GetData(), Secret.Num()));
	const uint64 NewRevision = ExpectedRevision == 0 ? 1 : ExpectedRevision + 1;
	TScopedCF<CFDataRef> RevisionData(MakeRevisionData(NewRevision));
	if (SecretData.Get() == nullptr || RevisionData.Get() == nullptr)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return EUnrealAISecretStoreResult::Failed;
	}
	FScopeLock Lock(&Impl->MutationMutex);
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	TScopedCF<CFMutableDictionaryRef> Query = MakeBaseQuery(Handle);
	if (Query.Get() == nullptr)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return EUnrealAISecretStoreResult::Failed;
	}
	OSStatus Status = errSecParam;
	if (ExpectedRevision == 0)
	{
		CFDictionarySetValue(Query.Get(), kSecValueData, SecretData.Get());
		CFDictionarySetValue(Query.Get(), kSecAttrGeneric, RevisionData.Get());
		CFDictionarySetValue(Query.Get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
		Status = Impl->InvokeSecurityCall([&Query]() { return SecItemAdd(Query.Get(), nullptr); });
	}
	else
	{
		TScopedCF<CFDataRef> ExpectedData(MakeRevisionData(ExpectedRevision));
		TScopedCF<CFMutableDictionaryRef> Update(CFDictionaryCreateMutable(
			kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
		if (ExpectedData.Get() == nullptr || Update.Get() == nullptr)
		{
			SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
			return EUnrealAISecretStoreResult::Failed;
		}
		CFDictionarySetValue(Query.Get(), kSecAttrGeneric, ExpectedData.Get());
		CFDictionarySetValue(Update.Get(), kSecValueData, SecretData.Get());
		CFDictionarySetValue(Update.Get(), kSecAttrGeneric, RevisionData.Get());
		Status = Impl->InvokeSecurityCall([&Query, &Update]() { return SecItemUpdate(Query.Get(), Update.Get()); });
	}
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	const EUnrealAISecretStoreResult Mapped = MapStatus(Status, OutError, ExpectedRevision != 0);
	if (Mapped == EUnrealAISecretStoreResult::Succeeded)
	{
		OutNewRevision = NewRevision;
	}
	return Mapped;
#endif
}

EUnrealAISecretStoreResult FUnrealAIMacKeychainSecretStore::Delete(const FUnrealAISecretStoreOperationContext &Context,
																   const FUnrealAISecretHandle &Handle,
																   const uint64 ExpectedRevision,
																   FUnrealAIProviderAccessError &OutError)
{
	OutError = FUnrealAIProviderAccessError{};
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	if (!ValidateHandle(Handle) || ExpectedRevision == 0)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::InvalidArgument,
					  EUnrealAIProviderAccessErrorCode::InvalidSecretStoreDelete);
		return EUnrealAISecretStoreResult::Failed;
	}
#if !PLATFORM_APPLE
	SetStoreError(OutError, EUnrealAIErrorCategory::UnsupportedCapability,
				  EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
	return EUnrealAISecretStoreResult::NotSupported;
#else
	TScopedCF<CFDataRef> ExpectedData(MakeRevisionData(ExpectedRevision));
	if (ExpectedData.Get() == nullptr)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return EUnrealAISecretStoreResult::Failed;
	}
	FScopeLock Lock(&Impl->MutationMutex);
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	TScopedCF<CFMutableDictionaryRef> Query = MakeBaseQuery(Handle);
	if (Query.Get() == nullptr)
	{
		SetStoreError(OutError, EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return EUnrealAISecretStoreResult::Failed;
	}
	CFDictionarySetValue(Query.Get(), kSecAttrGeneric, ExpectedData.Get());
	const OSStatus Status = Impl->InvokeSecurityCall([&Query]() { return SecItemDelete(Query.Get()); });
	if (const TOptional<EUnrealAISecretStoreResult> Operation = CheckOperation(Context, OutError); Operation.IsSet())
	{
		return Operation.GetValue();
	}
	return MapStatus(Status, OutError, true);
#endif
}
