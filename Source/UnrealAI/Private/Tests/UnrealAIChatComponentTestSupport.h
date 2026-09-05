#pragma once

#if WITH_DEV_AUTOMATION_TESTS

class FAutomationTestBase;

struct FUnrealAIChatComponentTestAccess
{
	static void RunRequestConstructionTests(FAutomationTestBase& Test);
};

#endif
