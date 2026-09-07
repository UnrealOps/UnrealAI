// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "UnrealAIExecutionService.h"
#include "UnrealAIResponseAdapter.h"
#include "UnrealAIResponseLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAINativePreflightTest, "UnrealAI.Execution.NativePreflight",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAINativePreflightTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAIExecutionService, ESPMode::ThreadSafe> Service =
		MakeShared<FUnrealAIExecutionService, ESPMode::ThreadSafe>();
	int32 Completions = 0;
	const FUnrealAIResponseRequest Request = UUnrealAIResponseLibrary::MakeResponseRequest(TEXT("fixture"));
	const FUnrealAIRequestHandle Handle = Service->CreateResponse(
		Request, FUnrealAIResponseNativeDelegate::CreateLambda(
					 [this, &Completions](const FUnrealAIResponseResult &Result)
					 {
						 ++Completions;
						 TestTrue(TEXT("Unconfigured execution returns a structured error"), Result.Error.bIsError);
						 TestTrue(TEXT("Preflight callback is on the game thread"), IsInGameThread());
					 }));
	TestFalse(TEXT("Preflight rejection creates no accepted handle"), Handle.IsValid());
	TestEqual(TEXT("Synchronous rejection completes once"), Completions, 1);
	FUnrealAIProviderConfig Config;
	Config.bRequiresApiKey = false;
	Config.ApiKeyOverride = TEXT("fixture-value");
	Config.AdditionalHeaders.Add(TEXT("Authorization"), TEXT("fixture-value"));
	Config.AdditionalHeaders.Add(TEXT("x-request-tag"), TEXT("safe-label"));
	Service->Configure(Config);
	TestTrue(TEXT("Configuration getter removes secret override"),
				  Service->GetProviderConfig().ApiKeyOverride.IsEmpty());
	TestEqual(TEXT("Configuration getter redacts credential headers"),
				   Service->GetProviderConfig().AdditionalHeaders.FindRef(TEXT("Authorization")),
																		  FString(TEXT("[redacted]")));
	TestEqual(TEXT("Noncredential configuration stays usable"),
				   Service->GetProviderConfig().AdditionalHeaders.FindRef(TEXT("x-request-tag")),
																		  FString(TEXT("safe-label")));
	FUnrealAIExecutionLimits Limits;
	Limits.MaxConcurrentRequests = 0;
	TestFalse(TEXT("An invalid admission bound is rejected"), Service->SetLimits(Limits));
	Config.TimeoutSeconds = -1;
	Service->Configure(Config);
	Service->CreateResponse(Request, FUnrealAIResponseNativeDelegate::CreateLambda(
										 [this](const FUnrealAIResponseResult &Result) {
											 TestEqual(TEXT("Invalid deadlines fail before transport"),
															Result.Error.Code, FString(TEXT("invalid_deadline")));
										 }));
	Service->Shutdown();
	Service->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIInlineImageTest, "UnrealAI.Responses.InlineImageInput",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIInlineImageTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	for (int32 Protocol = 0; Protocol < 4; ++Protocol)
	{
		FUnrealAIProviderConfig Config;
		Config.Name = TEXT("Fixture");
		Config.bRequiresApiKey = false;
		Config.Api = Protocol == 2 ? EUnrealAIProviderApi::AnthropicMessages
								   : (Protocol == 3 ? EUnrealAIProviderApi::GeminiGenerateContent
													: EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
		Config.ResponseApi = Protocol == 0 ? EUnrealAIResponseApi::OpenAIResponses : EUnrealAIResponseApi::ChatProtocol;
		FUnrealAIResponseRequest Request = UUnrealAIResponseLibrary::MakeResponseRequest(TEXT("Describe this view."));
		FUnrealAIResponsePart Part;
		Part.Type = EUnrealAIResponsePartType::Image;
		Part.MimeType = TEXT("image/png");
		Part.ImageBytes = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
		Request.Input[0].Content.Add(Part);
		FUnrealAIHttpRequestData Wire;
		FUnrealAIResponseContext Context;
		FUnrealAIError Error;
		TestTrue(TEXT("Each built-in protocol lowers typed image input"),
					  UnrealAIResponseAdapters::BuildRequest(Config, Request, FString(), EUnrealAIRequestMode::OneShot,
															 Wire, Context, Error));
		const TCHAR *Expected[] = {TEXT("input_image"), TEXT("image_url"), TEXT("media_type"), TEXT("inlineData")};
		TestTrue(TEXT("Image uses the selected protocol representation"), Wire.Body.Contains(Expected[Protocol]));
		Request.Input[0].Content.Last().MimeType = TEXT("image/gif");
		TestFalse(TEXT("Unsupported image encoding fails before transport"),
					   UnrealAIResponseAdapters::BuildRequest(Config, Request, FString(), EUnrealAIRequestMode::OneShot,
															  Wire, Context, Error));
		Request.Input[0].Content.Last().MimeType = TEXT("image/png");
		Request.Input[0].Content.Last().ImageBytes.SetNum(2 * 1024 * 1024 + 1);
		TestFalse(TEXT("Oversized images fail before base64 conversion"),
					   UnrealAIResponseAdapters::BuildRequest(Config, Request, FString(), EUnrealAIRequestMode::OneShot,
															  Wire, Context, Error));
	}
	return true;
}
#endif
