// Copyright EngineWorks. All Rights Reserved.

#pragma once

/** Native, non-authorizing lifetime observation. Logical cancellation does not imply physical settlement. */
class UNREALAI_API IUnrealAIRequestLifetime
{
  public:
	virtual ~IUnrealAIRequestLifetime() = default;
	virtual bool IsLogicallyComplete() const = 0;
	virtual bool IsPhysicallySettled() const = 0;
};
