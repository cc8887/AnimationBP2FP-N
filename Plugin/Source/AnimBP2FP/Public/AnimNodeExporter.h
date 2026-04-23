// AnimNodeExporter.h - Export UE Animation Node definitions to AnimLang stub
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"

/**
 * 从 UE 引擎导出动画节点定义为 AnimLang 类型存根
 * 类似于 TypeScript 的 .d.ts 生成
 */
class ANIMBP2FP_API FAnimNodeExporter
{
public:
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
	
	struct FPinInfo
	{
		FString Name;
		EPinType Type;
		bool bOptional;
		FString DefaultValue;
	};
	
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

/**
 * 命令行工具：从命令行导出节点定义
 * 
 * 用法:
 *   UE Editor -> Cmd -> AnimNodeExporter.Export
 *   或者作为 Commandlet 运行:
 *   UnrealEditor-Cmd.exe ProjectName -run=AnimNodeExporter -output=animlang-nodes.rkt
 */
class ANIMBP2FP_API UAnimNodeExporterCommandlet : public UCommandlet
{
	GENERATED_BODY()
	
public:
	UAnimNodeExporterCommandlet();
	
	virtual int32 Main(const FString& Params) override;
};