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
	
	// 检查是否需要重新生成 stub
	bool ShouldRegenerateStub();
	
	// 获取 stub 输出路径
	FString GetStubPath();
	
	// 委托句柄
	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle ReloadCompleteHandle;
};