// Copyright EngineWorks. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Misc/CoreDelegates.h"
#include "Subscriptions/UnrealAIAccountsEditor.h"
#include "ToolMenus.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "UnrealAIAuthEditorModule"

class FUnrealAIAuthEditorModule final : public IModuleInterface
{
  public:
	void StartupModule() override
	{
		EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FUnrealAIAuthEditorModule::OnEnginePreExit);
		ToolMenuStartupHandle = UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealAIAuthEditorModule::RegisterMenus));
	}

	void ShutdownModule() override
	{
		if (EnginePreExitHandle.IsValid())
		{
			FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
			EnginePreExitHandle.Reset();
		}
		CloseSubscriptionAccountsWindow();
		UToolMenus::UnRegisterStartupCallback(ToolMenuStartupHandle);
		UToolMenus::UnregisterOwner(this);
	}

	bool SupportsDynamicReloading() override
	{
		return false;
	}

	bool SupportsAutomaticShutdown() override
	{
		return false;
	}

  private:
	void OnEnginePreExit()
	{
		CloseSubscriptionAccountsWindow();
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu *Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		if (Menu == nullptr)
		{
			return;
		}
		FToolMenuSection &Section =
			Menu->FindOrAddSection(TEXT("UnrealAI"), LOCTEXT("AutonomousAgentsSection", "UnrealAI"));
		Section.AddMenuEntry(TEXT("UnrealAI.Accounts"), LOCTEXT("SubscriptionAccountsMenu", "AI Accounts"),
			LOCTEXT("SubscriptionAccountsMenuTooltip",
					"Manage accounts provided by enabled UnrealAI authentication integrations."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FUnrealAIAuthEditorModule::OpenSubscriptionAccountsWindow)));
	}

	void OpenSubscriptionAccountsWindow()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		if (const TSharedPtr<SWindow> ExistingWindow = SubscriptionAccountsWindow.Pin())
		{
			ExistingWindow->BringToFront();
			return;
		}

		const TSharedRef<SWindow> Window = SNew(SWindow)
											   .Title(LOCTEXT("SubscriptionAccountsWindowTitle", "UnrealAI Accounts"))
											   .ClientSize(FVector2D(760.0f, 560.0f))
											   .SizingRule(ESizingRule::UserSized)
											   .SupportsMinimize(false)
											   .SupportsMaximize(true);
		SubscriptionAccountsPanel = MakeUnrealAIAccountsPanel();
		Window->SetContent(SubscriptionAccountsPanel.ToSharedRef());
		Window->SetOnWindowClosed(
			FOnWindowClosed::CreateRaw(this, &FUnrealAIAuthEditorModule::OnSubscriptionAccountsWindowClosed));
		SubscriptionAccountsWindow = Window;
		FSlateApplication::Get().AddWindow(Window);
	}

	void OnSubscriptionAccountsWindowClosed(const TSharedRef<SWindow> &Window)
	{
		Window->SetContent(SNullWidget::NullWidget);
		SubscriptionAccountsPanel.Reset();
		SubscriptionAccountsWindow.Reset();
	}

	void CloseSubscriptionAccountsWindow()
	{
		const TSharedPtr<SWindow> Window = SubscriptionAccountsWindow.Pin();
		if (Window.IsValid())
		{
			Window->SetOnWindowClosed(FOnWindowClosed());
			Window->SetContent(SNullWidget::NullWidget);
			Window->RequestDestroyWindow();
		}
		SubscriptionAccountsPanel.Reset();
		SubscriptionAccountsWindow.Reset();
	}

	FDelegateHandle ToolMenuStartupHandle;
	FDelegateHandle EnginePreExitHandle;
	TWeakPtr<SWindow> SubscriptionAccountsWindow;
	TSharedPtr<SWidget> SubscriptionAccountsPanel;
};

IMPLEMENT_MODULE(FUnrealAIAuthEditorModule, UnrealAIAuthEditor)

#undef LOCTEXT_NAMESPACE
