// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::UnrealAI::OpenAI::Private
{
/** Projects a private schema copy into the strict Responses subset; never mutates the caller's schema text. */
bool ProjectToolSchema(const TSharedRef<FJsonObject> &Schema, bool bStrict, bool &bOutOptionalProjection);
/** Reverses only synthetic optional nulls in inline object/array schemas. Required and originally nullable values stay.
 */
bool NormalizeToolArguments(const TSharedRef<FJsonObject> &Schema, const TSharedRef<FJsonObject> &Arguments);
} // namespace UE::UnrealAI::OpenAI::Private
