// AnimBP2FPEditorModule.cpp - Editor Module Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPEditorModule.h"
#include "AnimNodeExporter.h"
#include "AnimBP2FPSettings.h"
#include "ToolMenus.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "FAnimBP2FPEditorModule"

// ========== 模块生命周期 ==========

void FAnimBP2FPEditorModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Module startup"));
	
	// 注册引擎初始化回调
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
		this, &FAnimBP2FPEditorModule::OnEngineInit
	);
	
	// 注册热重载回调
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(
		this, &FAnimBP2FPEditorModule::OnReloadComplete
	);
	
	// 注册编辑器菜单（延迟到引擎初始化后）
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(
			this, &FAnimBP2FPEditorModule::RegisterMenuExtensions
		)
	);
}

void FAnimBP2FPEditorModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Module shutdown"));
	
	// 移除回调
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
	
	// 注销菜单
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

// ========== 自动生成逻辑 ==========

void FAnimBP2FPEditorModule::OnEngineInit()
{
	const UAnimBP2FPSettings* Settings = GetDefault<UAnimBP2FPSettings>();
	
	if (!Settings->bAutoGenerateStub || !Settings->bGenerateOnStartup)
	{
		return;
	}
	
	if (ShouldRegenerateStub())
	{
		UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Auto-generating stub on startup..."));
		ExportNodes();
	}
}

void FAnimBP2FPEditorModule::OnReloadComplete(EReloadCompleteReason Reason)
{
	const UAnimBP2FPSettings* Settings = GetDefault<UAnimBP2FPSettings>();
	
	if (!Settings->bAutoGenerateStub || !Settings->bGenerateOnReload)
	{
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Regenerating stub after reload..."));
	ExportNodes();
}

bool FAnimBP2FPEditorModule::ShouldRegenerateStub()
{
	FString StubPath = GetStubPath();
	
	// 文件不存在，需要生成
	if (!FPaths::FileExists(StubPath))
	{
		return true;
	}
	
	// 比较时间戳（简化版本，实际应比较编译时间）
	FDateTime StubTime = IFileManager::Get().GetTimeStamp(*StubPath);
	FDateTime Now = FDateTime::Now();
	
	// 超过 1 天，重新生成
	return (Now - StubTime).GetDays() >= 1;
}

FString FAnimBP2FPEditorModule::GetStubPath()
{
	const UAnimBP2FPSettings* Settings = GetDefault<UAnimBP2FPSettings>();
	
	FString Path = Settings->StubOutputPath;
	
	// 相对路径转绝对路径
	if (FPaths::IsRelative(Path))
	{
		Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
	}
	
	return Path;
}

// ========== 编辑器菜单 ==========

void FAnimBP2FPEditorModule::RegisterMenuExtensions()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	
	// 扩展 Tools 菜单
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (Menu == nullptr)
	{
		return;
	}
	
	FToolMenuSection& Section = Menu->FindOrAddSection("AnimBP2FP");
	Section.Label = LOCTEXT("AnimBP2FPSection", "AnimBP2FP");
	
	// 添加导出命令
	Section.AddMenuEntry(
		"ExportAnimLangNodes",
		LOCTEXT("ExportNodes", "Export AnimLang Nodes"),
		LOCTEXT("ExportNodes_Tooltip", "Generate Racket stub file from all animation nodes"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FAnimBP2FPEditorModule::ExportNodes)
		)
	);
}

void FAnimBP2FPEditorModule::ExportNodes()
{
	FString OutputPath = GetStubPath();
	
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Exporting nodes to %s"), *OutputPath);
	
	// 确保目录存在
	FString Directory = FPaths::GetPath(OutputPath);
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}
	
	// 导出
	if (FAnimNodeExporter::ExportAllNodes(OutputPath))
	{
		FText Message = FText::Format(
			LOCTEXT("ExportSuccess", "Successfully exported animation nodes to:\n{0}"),
			FText::FromString(OutputPath)
		);
		
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Export complete"));
	}
	else
	{
		FText Message = LOCTEXT("ExportFailed", "Failed to export animation nodes. See log for details.");
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		UE_LOG(LogTemp, Error, TEXT("AnimBP2FPEditor: Export failed"));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAnimBP2FPEditorModule, AnimBP2FPEditor)