#pragma once

#include "CoreMinimal.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealAIResponseTypes.h"

namespace UnrealAIResponseJson
{
	using FObject = TSharedPtr<FJsonObject>;
	using FArray = TArray<TSharedPtr<FJsonValue>>;

	inline FObject Object()
	{
		return MakeShared<FJsonObject>();
	}

	inline FObject Parse(const FString& Json)
	{
		FObject Result;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result) ? Result : FObject();
	}

	inline FArray Array(const FObject& Obj, const TCHAR* Key)
	{
		const FArray* Result = nullptr;
		return Obj && Obj->TryGetArrayField(Key, Result) && Result ? *Result : FArray();
	}

	inline FObject Child(const FObject& Obj, const TCHAR* Key)
	{
		const FObject* Result = nullptr;
		return Obj && Obj->TryGetObjectField(Key, Result) && Result ? *Result : FObject();
	}

	inline FObject AsObject(const TSharedPtr<FJsonValue>& Value)
	{
		return Value && Value->Type == EJson::Object ? Value->AsObject() : FObject();
	}

	inline FString String(const FObject& Obj, const TCHAR* Key)
	{
		FString Result;
		if (Obj)
		{
			Obj->TryGetStringField(Key, Result);
		}
		return Result;
	}

	inline int32 Number(const FObject& Obj, const TCHAR* Key, int32 Default = 0)
	{
		int32 Result = Default;
		if (Obj)
		{
			Obj->TryGetNumberField(Key, Result);
		}
		return Result;
	}

	inline bool Bool(const FObject& Obj, const TCHAR* Key)
	{
		bool Result = false;
		if (Obj)
		{
			Obj->TryGetBoolField(Key, Result);
		}
		return Result;
	}

	inline FString Serialize(const FObject& Obj)
	{
		FString Json;
		if (Obj)
		{
			FJsonSerializer::Serialize(Obj.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
		}
		return Json;
	}

	inline FString Serialize(const FArray& Values)
	{
		FString Json;
		FJsonSerializer::Serialize(Values, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}

	inline bool ParseArray(const FString& Json, FArray& Out)
	{
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out);
	}

	inline TSharedPtr<FJsonValue> Value(const FObject& Obj)
	{
		return MakeShared<FJsonValueObject>(Obj);
	}

	inline FObject At(FArray& Values, int32 Index)
	{
		if (Index < 0 || Index > 16384)
		{
			return nullptr;
		}
		while (Values.Num() <= Index)
		{
			Values.Add(Value(Object()));
		}
		return AsObject(Values[Index]);
	}

	inline bool Fail(FUnrealAIError& Error, const TCHAR* Message, const TCHAR* Code = TEXT("invalid_response_request"))
	{
		Error.bIsError = true;
		Error.Type = TEXT("response_error");
		Error.Code = Code;
		Error.Message = Message;
		return false;
	}

	inline int64 Bytes(const FString& Text)
	{
		return FTCHARToUTF8(*Text).Length();
	}

	inline FString Role(EUnrealAIMessageRole Value)
	{
		switch (Value)
		{
		case EUnrealAIMessageRole::System: return TEXT("system");
		case EUnrealAIMessageRole::Developer: return TEXT("developer");
		case EUnrealAIMessageRole::Assistant: return TEXT("assistant");
		case EUnrealAIMessageRole::Tool: return TEXT("tool");
		default: return TEXT("user");
		}
	}
}
