// AnimBP2FPSettings.h - Project Settings
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AnimBP2FPSettings.generated.h"

/**
 * AnimBP2FP 项目设置
 * 位置: Edit → Project Settings → Plugins → AnimBP2FP
 */
UCLASS(config=Editor, defaultconfig, meta=(DisplayName="AnimBP2FP"))
class ANIMBP2FPEDITOR_API UAnimBP2FPSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	UAnimBP2FPSettings();
	
	// ========== Stub 生成设置 ==========
	
	/** 是否自动生成 stub 文件 */
	UPROPERTY(Config, EditAnywhere, Category="Stub Generation",
		meta=(DisplayName="Auto Generate Stub"))
	bool bAutoGenerateStub;
	
	/** 是否在编辑器启动时生成 stub */
	UPROPERTY(Config, EditAnywhere, Category="Stub Generation",
		meta=(DisplayName="Generate On Startup",
		      EditCondition="bAutoGenerateStub"))
	bool bGenerateOnStartup;
	
	/** 是否在 C++ 热重载后生成 stub */
	UPROPERTY(Config, EditAnywhere, Category="Stub Generation",
		meta=(DisplayName="Generate On Reload",
		      EditCondition="bAutoGenerateStub"))
	bool bGenerateOnReload;
	
	/** Stub 文件输出路径（相对于项目根目录） */
	UPROPERTY(Config, EditAnywhere, Category="Stub Generation",
		meta=(DisplayName="Stub Output Path"))
	FString StubOutputPath;
	
	// ========== 导出选项 ==========
	
	/** 是否包含已弃用的节点 */
	UPROPERTY(Config, EditAnywhere, Category="Export Options",
		meta=(DisplayName="Include Deprecated Nodes"))
	bool bIncludeDeprecatedNodes;
	
	/** 是否包含实验性节点 */
	UPROPERTY(Config, EditAnywhere, Category="Export Options",
		meta=(DisplayName="Include Experimental Nodes"))
	bool bIncludeExperimentalNodes;
	
	// ========== UDeveloperSettings Interface ==========
	
	virtual FName GetCategoryName() const override
	{
		return TEXT("Plugins");
	}
	
	virtual FText GetSectionText() const override
	{
		return NSLOCTEXT("AnimBP2FPSettings", "Section", "AnimBP2FP");
	}
};