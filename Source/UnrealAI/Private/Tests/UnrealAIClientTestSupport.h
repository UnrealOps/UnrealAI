#pragma once

#if WITH_DEV_AUTOMATION_TESTS

class FAutomationTestBase;

struct FUnrealAIClientTestAccess
{
	static void RunRetryCoordinatorTests(FAutomationTestBase& Test);
};

#endif
