// AnimNodeExporter.h - Export UE Animation Node definitions to AnimLang stub
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"

// 注意：AnimNodeExporter 是编辑器专用功能，仅用于编辑器构建
// 非编辑器构建时，FAnimNodeExporter 和 UAnimNodeExporterCommandlet 不可用

#if WITH_EDITOR

#include "AnimGraphNode_Base.h"

/**
 * 从 UE 引擎导出动画节点定义为 AnimLang 类型存根
 * 类似于 TypeScript 的 .d.ts 生成
 * 
 * 注意：此类仅用于编辑器构建
 */
class ANIMBP2FP_API FAnimNodeExporter
{
public:
	/**
	 * Pin 信息（必须在 FNodeInfo 之前定义）
	 */
	struct FPinInfo
	{
		FString Name;
		EPinType Type;
		bool bOptional;
		FString DefaultValue;
	};

	/**
	 * 从 UE 引擎反射系统提取节点信息
	 */
	struct FNodeInfo
	{
		FString NodeName;          // e.g., "sequence-player"
		FString ClassName;         // e.g., "UAnimGraphNode_SequencePlayer"
		FString Description;       // 节点描述
		TArray<FPinInfo> InputPins;
		TArray<FPinInfo> OutputPins;
		TMap<FString, FString> Properties;  // 属性名 -> 类型
	};

	/**
	 * 扫描所有 UAnimGraphNode 子类并生成类型定义
	 * @param OutputPath 输出路径（.rkt 文件）
	 * @return 是否成功
	 */
	static bool ExportAllNodes(const FString& OutputPath);
	
	/**
	 * 导出单个节点的类型定义
	 * @param NodeClass 节点类
	 * @return Typed Racket 类型定义字符串
	 */
	static FString ExportNodeDefinition(UClass* NodeClass);
	
	/**
	 * 扫描所有动画节点类
	 */
	static TArray<FNodeInfo> ScanAllAnimNodes();
	
private:
	// 类型映射：UE 类型 -> AnimLang 类型
	static FString MapPinType(EPinType UEType);
	static FString MapPropertyType(FProperty* Property);
	
	// 命名转换：SequencePlayer -> sequence-player
	static FString ToKebabCase(const FString& PascalCase);
	
	// 生成 Typed Racket 类型定义
	static FString GenerateTypedRacketDefinition(const FNodeInfo& NodeInfo);
	static FString GenerateNodeSignature(const FNodeInfo& NodeInfo);
};

#endif // WITH_EDITOR