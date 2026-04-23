// AnimBP2FPSettings.h - Project Settings
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FBP2FPMapping.h"
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
	
	// ========== Stub 设置 ==========

	/** Stub 文件输出路径（相对于项目根目录） */
	UPROPERTY(Config, EditAnywhere, Category="Stub",
		meta=(DisplayName="Stub Output Path"))
	FString StubOutputPath;
	
	// ========== 自动同步设置 ==========

	/** Auto-sync direction: None disables sync, BP2FP exports on compile, FP2BP imports on file change */
	UPROPERTY(Config, EditAnywhere, Category="Auto Sync",
		meta=(DisplayName="Auto Sync Mode"))
	EBP2FPSyncMode AutoSyncMode;

	/** DSL output subdirectory under Saved/BP2DSL/ */
	UPROPERTY(Config, EditAnywhere, Category="Auto Sync",
		meta=(DisplayName="DSL Output Category",
		      EditCondition="AutoSyncMode != EBP2FPSyncMode::None"))
	FString DSLOutputCategory;

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