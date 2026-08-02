// AnimBP2FPEditorModule.cpp - Editor Module Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPEditorModule.h"
#include "AnimNodeExporter.h"
#include "AnimBPExporter.h"
#include "AnimLangRoundTrip.h"
#include "AnimBP2FPSettings.h"
#include "AnimBP2FPCompilerHook.h"
#include "FBP2FPMappingRegistry.h"
#include "ToolMenus.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/MessageDialog.h"
#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/AssetManager.h"

#define LOCTEXT_NAMESPACE "FAnimBP2FPEditorModule"

// ========== 模块生命周期 ==========

void FAnimBP2FPEditorModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Module startup"));
	
	// 注册引擎初始化回调
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
		this, &FAnimBP2FPEditorModule::OnEngineInit
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
	
	// Unregister compiler hook first
	if (CompilerHook.IsValid())
	{
		CompilerHook->Unregister();
		CompilerHook.Reset();
	}
	
	// 移除回调
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	
	// 注销菜单
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

// ========== 自动同步设置 ==========

void FAnimBP2FPEditorModule::OnEngineInit()
{
	// Initialize the BP <-> DSL mapping registry
	InitializeMappingRegistry();

	// Setup auto-sync
	SetupAutoSync();
}

// ========== 编辑器菜单 ==========

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
	
	// 添加 AnimBP -> DSL 导出命令
	Section.AddMenuEntry(
		"ExportAnimBPToDSL",
		LOCTEXT("ExportAnimBP", "Export AnimBP to DSL"),
		LOCTEXT("ExportAnimBP_Tooltip", "Export all Animation Blueprints in the project to AnimLang DSL"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FAnimBP2FPEditorModule::ExportAnimBPToDSL)
		)
	);
	
	// 添加往返验证测试命令
	Section.AddMenuEntry(
		"RunRoundTripTest",
		LOCTEXT("RoundTripTest", "Run Round-Trip Validation"),
		LOCTEXT("RoundTripTest_Tooltip", "Parse exported .animlang files and compare with re-serialized output"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FAnimBP2FPEditorModule::RunRoundTripTest)
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

void FAnimBP2FPEditorModule::ExportAnimBPToDSL()
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Starting AnimBP -> DSL export..."));
	
	// 输出目录: 统一约定 {Project}/Saved/BP2DSL/AnimBP
	FString OutputDir = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / TEXT("AnimBP");
	if (!IFileManager::Get().DirectoryExists(*OutputDir))
	{
		IFileManager::Get().MakeDirectory(*OutputDir, true);
	}
	
	// 使用 AssetRegistry 查找所有动画蓝图
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	TArray<FAssetData> AnimBPAssets;
	FARFilter Filter;
#if ENGINE_MAJOR_VERSION < 5
	Filter.ClassNames.Add(UAnimBlueprint::StaticClass()->GetFName());
#else
	Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
#endif
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	Filter.bRecursivePaths = true;
	AssetRegistry.GetAssets(Filter, AnimBPAssets);
	
	if (AnimBPAssets.Num() == 0)
	{
		FText Message = LOCTEXT("NoAnimBPs", "No Animation Blueprints found in the project.");
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Found %d Animation Blueprints"), AnimBPAssets.Num());
	
	int32 SuccessCount = 0;
	int32 FailCount = 0;
	FString AllOutput;
	
	for (const FAssetData& AssetData : AnimBPAssets)
	{
		FString AssetName = AssetData.AssetName.ToString();
		FString PackagePath = AssetData.PackageName.ToString();
		
		UE_LOG(LogTemp, Log, TEXT("  Processing: %s (%s)"), *AssetName, *PackagePath);
		
		// 加载动画蓝图
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(AssetData.GetAsset());
		if (!AnimBP)
		{
			UE_LOG(LogTemp, Warning, TEXT("  Failed to load: %s"), *AssetName);
			FailCount++;
			continue;
		}
		
		// 导出
		FAnimBPExporter::FExportOptions Options;
		Options.bPrettyPrint = true;
		Options.IndentSize = 2;
		
		FString DSLOutput = FAnimBPExporter::ExportWithOptions(AnimBP, Options);
		
		// 添加文件头注释
		FString FileContent = FString::Printf(
			TEXT(";; AnimLang DSL Export\n")
			TEXT(";; Source: %s\n")
			TEXT(";; Asset: %s\n")
			TEXT(";; Generated by AnimBP2FP Plugin\n")
			TEXT(";;\n\n")
			TEXT("%s\n"),
			*PackagePath, *AssetName, *DSLOutput
		);
		
		// 使用统一路径约定（通过 MappingRegistry）
		FString FullPath = AnimBP->GetPathName();
		FString OutputFilePath = FBP2FPMappingRegistry::BlueprintToDSLPath(FullPath, TEXT("AnimBP"), TEXT(".animlang"));
		if (OutputFilePath.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("  Cannot resolve DSL path for: %s"), *AssetName);
			FailCount++;
			continue;
		}

		// 确保目录存在
		FString Dir = FPaths::GetPath(OutputFilePath);
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			IFileManager::Get().MakeDirectory(*Dir, true);
		}

		if (FFileHelper::SaveStringToFile(FileContent, *OutputFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogTemp, Log, TEXT("  Exported: %s -> %s"), *AssetName, *OutputFilePath);
			SuccessCount++;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("  Failed to write: %s"), *OutputFilePath);
			FailCount++;
		}
		
		// 累积到总输出（用分隔符隔开）
		AllOutput += FString::Printf(TEXT("\n;; ======== %s ========\n"), *AssetName);
		AllOutput += DSLOutput;
		AllOutput += TEXT("\n");
	}
	
	// 写入合并文件
	FString CombinedPath = OutputDir / TEXT("_all_animbp.animlang");
	FString CombinedContent = FString::Printf(
		TEXT(";; AnimLang DSL - All Animation Blueprints\n")
		TEXT(";; Total: %d exported, %d failed\n")
		TEXT(";;\n\n")
		TEXT("%s"),
		SuccessCount, FailCount, *AllOutput
	);
	FFileHelper::SaveStringToFile(CombinedContent, *CombinedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	
	// 显示结果
	FText Message = FText::Format(
		LOCTEXT("ExportAnimBPResult", "AnimBP -> DSL Export Complete!\n\nExported: {0} / {1}\nFailed: {2}\n\nOutput: {3}"),
		FText::AsNumber(SuccessCount),
		FText::AsNumber(AnimBPAssets.Num()),
		FText::AsNumber(FailCount),
		FText::FromString(OutputDir)
	);
	FMessageDialog::Open(EAppMsgType::Ok, Message);
	
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Export complete. %d/%d succeeded. Output: %s"),
		SuccessCount, AnimBPAssets.Num(), *OutputDir);
}

void FAnimBP2FPEditorModule::RunRoundTripTest()
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Running round-trip validation..."));

	FString ExportDir = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / TEXT("AnimBP");

	if (!IFileManager::Get().DirectoryExists(*ExportDir))
	{
		FText Message = LOCTEXT("NoExportDir", "Export directory not found. Run 'Export AnimBP to DSL' first.");
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		return;
	}

	TArray<FRoundTripResult> Results = FAnimLangRoundTrip::TestDirectory(ExportDir);

	if (Results.Num() == 0)
	{
		FText Message = LOCTEXT("NoAnimLangFiles", "No .animlang files found in export directory.");
		FMessageDialog::Open(EAppMsgType::Ok, Message);
		return;
	}

	FString Report = FAnimLangRoundTrip::GenerateReport(Results);

	// Save report
	FString ReportPath = ExportDir / TEXT("round_trip_report.txt");
	FFileHelper::SaveStringToFile(Report, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// Count results
	int32 PassCount = 0;
	int32 FailCount = 0;
	for (const FRoundTripResult& R : Results)
	{
		if (R.bSuccess) PassCount++;
		else FailCount++;
	}

	// Show summary dialog
	FText Message = FText::Format(
		LOCTEXT("RoundTripResult", "Round-Trip Validation Complete!\n\nPassed: {0} / {1}\nFailed: {2}\n\nReport: {3}"),
		FText::AsNumber(PassCount),
		FText::AsNumber(Results.Num()),
		FText::AsNumber(FailCount),
		FText::FromString(ReportPath)
	);
	FMessageDialog::Open(EAppMsgType::Ok, Message);
}

// ========== Mapping Registry & Auto Sync ==========

void FAnimBP2FPEditorModule::InitializeMappingRegistry()
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Initializing BP <-> DSL mapping registry..."));
	FBP2FPMappingRegistry::Get().Initialize();
}

void FAnimBP2FPEditorModule::SetupAutoSync()
{
	const UAnimBP2FPSettings* Settings = GetDefault<UAnimBP2FPSettings>();

	if (Settings->AutoSyncMode == EBP2FPSyncMode::BP2FP)
	{
		// Create compiler hook if not already created
		if (!CompilerHook.IsValid())
		{
			CompilerHook = MakeUnique<FAnimBP2FPCompilerHook>();
		}

		if (!CompilerHook->IsRegistered())
		{
			CompilerHook->Register();
		}

		UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Auto-sync BP2FP enabled"));
	}
	else if (Settings->AutoSyncMode == EBP2FPSyncMode::FP2BP)
	{
		// FP2BP: File watcher mode (not yet implemented)
		UE_LOG(LogTemp, Warning, TEXT("AnimBP2FPEditor: FP2BP auto-sync mode is not yet implemented"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("AnimBP2FPEditor: Auto-sync disabled"));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAnimBP2FPEditorModule, AnimBP2FPEditor)
