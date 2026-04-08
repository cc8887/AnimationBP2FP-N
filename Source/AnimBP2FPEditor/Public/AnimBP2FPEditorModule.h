// AnimBP2FPEditorModule.h - Editor Module Interface
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * AnimBP2FP 编辑器模块
 * 负责：
 * - 自动生成 AnimLang stub 文件
 * - 注册编辑器菜单命令
 * - 集成项目设置
 * - 管理 Blueprint <-> DSL 自动同步
 */
class FAnimBP2FPEditorModule : public IModuleInterface
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	
private:
	// 引擎初始化时回调
	void OnEngineInit();
	
	// 热重载完成时回调
	void OnReloadComplete(EReloadCompleteReason Reason);
	
	// 注册编辑器菜单
	void RegisterMenuExtensions();
	
	// 导出节点命令
	void ExportNodes();
	
	// 导出 AnimBP 到 DSL
	void ExportAnimBPToDSL();
	
	// 往返验证测试
	void RunRoundTripTest();
	
	// 检查是否需要重新生成 stub
	bool ShouldRegenerateStub();
	
	// 获取 stub 输出路径
	FString GetStubPath();

	// 初始化 BP <-> DSL 映射注册表
	void InitializeMappingRegistry();

	// 根据设置启用/禁用自动同步
	void SetupAutoSync();

	// 委托句柄
	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle ReloadCompleteHandle;

	// Compiler hook for auto-sync (owned by module)
	TUniquePtr<class FAnimBP2FPCompilerHook> CompilerHook;
};