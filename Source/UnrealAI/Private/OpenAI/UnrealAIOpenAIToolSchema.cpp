// Copyright UnrealOps. All Rights Reserved.

#include "OpenAI/UnrealAIOpenAIToolSchema.h"

namespace UE::UnrealAI::OpenAI::Private
{
namespace
{
constexpr int32 MaximumDepth = 64;

bool AllowsNull(const TSharedRef<FJsonObject> &Schema)
{
	const TArray<TSharedPtr<FJsonValue>> *Enum = nullptr;
	if (Schema->TryGetArrayField(TEXT("enum"), Enum) &&
								 !Enum->ContainsByPredicate([](const TSharedPtr<FJsonValue> &Value)
															{ return Value.IsValid() && Value->IsNull(); }))
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Type = Schema->TryGetField(TEXT("type"));
	if (Type.IsValid())
	{
		if (Type->Type == EJson::String)
		{
			return Type->AsString() == TEXT("null");
		}
		if (Type->Type == EJson::Array)
		{
			return Type->AsArray().ContainsByPredicate(
				[](const TSharedPtr<FJsonValue> &Value)
				{ return Value.IsValid() && Value->Type == EJson::String && Value->AsString() == TEXT("null"); });
		}
	}
	// Leave compound/reference schemas unchanged: this adapter does not reinterpret their null semantics.
	return true;
}

TSet<FString> RequiredProperties(const TSharedRef<FJsonObject> &Schema)
{
	TSet<FString> Result;
	const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
	if (Schema->TryGetArrayField(TEXT("required"), Values))
	{
		for (const TSharedPtr<FJsonValue> &Value : *Values)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name))
			{
				Result.Add(Name);
			}
		}
	}
	return Result;
}

bool Project(const TSharedRef<FJsonObject> &Schema, const bool bStrict, bool &bOptional, const int32 Depth)
{
	if (Depth > MaximumDepth)
	{
		return false;
	}
	FString Format;
	if (Schema->TryGetStringField(TEXT("format"), Format))
	{
		static const TSet<FString> Formats = {
			TEXT("date-time"), TEXT("time"), TEXT("date"), TEXT("duration"), TEXT("email"),
																				  TEXT("hostname"),	 TEXT("ipv4"), TEXT("ipv6"), TEXT("uuid")};
		if (!Formats.Contains(Format))
		{
			// Unknown format names are application annotations. Keep patterns and other real constraints.
			Schema->RemoveField(TEXT("format"));
		}
	}
	for (auto It = Schema->Values.CreateIterator(); It; ++It)
	{
		if (It.Key().StartsWith(TEXT("x-"), ESearchCase::CaseSensitive))
		{
			It.RemoveCurrent();
		}
	}
	const TSharedPtr<FJsonObject> *Properties = nullptr;
	if (Schema->TryGetObjectField(TEXT("properties"), Properties))
	{
		const TSet<FString> Required = RequiredProperties(Schema);
		for (const FString &Name : Required)
		{
			if (!(*Properties)->Values.Contains(Name))
			{
				return false;
			}
		}
		TArray<TSharedPtr<FJsonValue>> WireRequired;
		for (TPair<FString, TSharedPtr<FJsonValue>> &Property : (*Properties)->Values)
		{
			const TSharedPtr<FJsonObject> *Child = nullptr;
			if (!Property.Value.IsValid() || !Property.Value->TryGetObject(Child) || !Child->IsValid())
			{
				return false;
			}
			if (bStrict && !Required.Contains(Property.Key))
			{
				// Optional composition/reference semantics cannot be reversibly normalized without a resolver.
				for (const TCHAR *Keyword : {TEXT("$ref"), TEXT("$dynamicRef"), TEXT("anyOf"), TEXT("oneOf"),
					TEXT("allOf"), TEXT("not"), TEXT("if"), TEXT("then"), TEXT("else")})
				{
					if ((*Child)->HasField(Keyword))
					{
						return false;
					}
				}
			}
			const bool bAddNull = bStrict && !Required.Contains(Property.Key) && !AllowsNull(Child->ToSharedRef());
			if (!Project(Child->ToSharedRef(), bStrict, bOptional, Depth + (bAddNull ? 4 : 2)))
			{
				return false;
			}
			if (bAddNull)
			{
				const TSharedRef<FJsonObject> Nullable = MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> Null = MakeShared<FJsonObject>();
				Null->SetStringField(TEXT("type"), TEXT("null"));
				Nullable->SetArrayField(TEXT("anyOf"), {Property.Value, MakeShared<FJsonValueObject>(Null)});
				Property.Value = MakeShared<FJsonValueObject>(Nullable);
				bOptional = true;
			}
			WireRequired.Add(MakeShared<FJsonValueString>(Property.Key));
		}
		if (bStrict)
		{
			Schema->SetArrayField(TEXT("required"), WireRequired);
		}
	}
	const TSharedPtr<FJsonObject> *Items = nullptr;
	if (Schema->TryGetObjectField(TEXT("items"), Items) &&
								  !Project(Items->ToSharedRef(), bStrict, bOptional, Depth + 1))
	{
		return false;
	}
	return true;
}

bool Normalize(const TSharedRef<FJsonObject> &Schema, const TSharedPtr<FJsonValue> &Value, const int32 Depth)
{
	if (Depth > MaximumDepth || !Value.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> *Properties = nullptr;
	if (Value->Type == EJson::Object && Schema->TryGetObjectField(TEXT("properties"), Properties))
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		const TSet<FString> Required = RequiredProperties(Schema);
		for (const TPair<FString, TSharedPtr<FJsonValue>> &Property : (*Properties)->Values)
		{
			const TSharedPtr<FJsonObject> *Child = nullptr;
			const TSharedPtr<FJsonValue> Field = Object->TryGetField(Property.Key);
			if (!Field.IsValid() || !Property.Value.IsValid() || !Property.Value->TryGetObject(Child))
			{
				continue;
			}
			if (Field->IsNull() && !Required.Contains(Property.Key) && !AllowsNull(Child->ToSharedRef()))
			{
				Object->RemoveField(Property.Key);
			}
			else if (!Normalize(Child->ToSharedRef(), Field, Depth + 1))
			{
				return false;
			}
		}
	}
	const TSharedPtr<FJsonObject> *Items = nullptr;
	if (Value->Type == EJson::Array && Schema->TryGetObjectField(TEXT("items"), Items))
	{
		for (const TSharedPtr<FJsonValue> &Item : Value->AsArray())
		{
			if (!Normalize(Items->ToSharedRef(), Item, Depth + 1))
			{
				return false;
			}
		}
	}
	return true;
}
} // namespace

bool ProjectToolSchema(const TSharedRef<FJsonObject> &Schema, const bool bStrict, bool &bOutOptionalProjection)
{
	bOutOptionalProjection = false;
	return Project(Schema, bStrict, bOutOptionalProjection, 0);
}

bool NormalizeToolArguments(const TSharedRef<FJsonObject> &Schema, const TSharedRef<FJsonObject> &Arguments)
{
	return Normalize(Schema, MakeShared<FJsonValueObject>(Arguments), 0);
}
} // namespace UE::UnrealAI::OpenAI::Private
