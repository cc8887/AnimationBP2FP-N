// AnimNodeExporter.cpp - Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimNodeExporter.h"

#if WITH_EDITOR
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_Slot.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectIterator.h"
#include "EdGraphSchema_K2.h"
#include "Animation/BlendSpace.h"

// ========== 辅助函数：获取引脚类型 ==========

static EPinType GetPinType(UEdGraphPin* Pin)
{
	if (!Pin) return EPinType::Object;

	const FName& PinCategory = Pin->PinType.PinCategory;

	if (PinCategory == UEdGraphSchema_K2::PC_Float || PinCategory == UEdGraphSchema_K2::PC_Real)
		return EPinType::Float;
	if (PinCategory == UEdGraphSchema_K2::PC_Int)
		return EPinType::Int;
	if (PinCategory == UEdGraphSchema_K2::PC_Boolean)
		return EPinType::Bool;
	if (PinCategory == TEXT("struct"))
	{
		if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			if (Struct->GetFName() == NAME_Vector)
				return EPinType::Vector;
			if (Struct->GetFName() == NAME_Rotator)
				return EPinType::Rotator;
			if (Struct->GetFName() == NAME_Transform)
				return EPinType::Transform;
		}
	}
	if (PinCategory == UEdGraphSchema_K2::PC_Name)
		return EPinType::Name;
	// 动画姿势引脚
	if (PinCategory == TEXT("pose") || PinCategory == TEXT("Pose"))
		return EPinType::Pose;

	return EPinType::Object;
}

// ========== 主导出函数 ==========

bool FAnimNodeExporter::ExportAllNodes(const FString& OutputPath)
{
	TArray<FNodeInfo> AllNodes = ScanAllAnimNodes();
	
	FString Output = TEXT(";; Auto-generated AnimLang Node Definitions\n");
	Output += TEXT(";; Generated from Unreal Engine 5.6\n");
	Output += TEXT(";; Do not edit manually\n\n");
	Output += TEXT("#lang typed/racket\n\n");
	Output += TEXT("(require \"animlang-types.rkt\")\n\n");
	
	// 导出每个节点
	for (const FNodeInfo& Node : AllNodes)
	{
		Output += GenerateTypedRacketDefinition(Node);
		Output += TEXT("\n\n");
	}
	
	// 导出列表
	Output += TEXT(";; ========== Available Nodes ==========\n");
	Output += TEXT(";; Total: ") + FString::FromInt(AllNodes.Num()) + TEXT(" nodes\n\n");
	for (const FNodeInfo& Node : AllNodes)
	{
		Output += FString::Printf(TEXT(";; - %s (%s)\n"), *Node.NodeName, *Node.ClassName);
	}
	
	// 写入文件
	return FFileHelper::SaveStringToFile(Output, *OutputPath);
}

// ========== 扫描所有节点 ==========

TArray<FAnimNodeExporter::FNodeInfo> FAnimNodeExporter::ScanAllAnimNodes()
{
	TArray<FNodeInfo> Nodes;
	
	// 遍历所有 UAnimGraphNode_Base 子类
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		
		if (Class->IsChildOf(UAnimGraphNode_Base::StaticClass()) &&
		    !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			FNodeInfo NodeInfo;
			NodeInfo.ClassName = Class->GetName();
			NodeInfo.NodeName = ToKebabCase(Class->GetName().Replace(TEXT("UAnimGraphNode_"), TEXT("")));
			NodeInfo.Description = Class->GetToolTipText().ToString();
			
			// 提取引脚信息
			if (UAnimGraphNode_Base* CDO = Cast<UAnimGraphNode_Base>(Class->GetDefaultObject()))
			{
				// 输入引脚
				for (UEdGraphPin* Pin : CDO->Pins)
				{
					if (Pin->Direction == EGPD_Input)
					{
						FPinInfo PinInfo;
						PinInfo.Name = Pin->PinName.ToString();
						PinInfo.Type = GetPinType(Pin);
						PinInfo.bOptional = !Pin->bNotConnectable;
						PinInfo.DefaultValue = Pin->DefaultValue;
						NodeInfo.InputPins.Add(PinInfo);
					}
				}
			}
			
			// 提取属性
			for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
				{
					NodeInfo.Properties.Add(
						Property->GetName(),
						MapPropertyType(Property)
					);
				}
			}
			
			Nodes.Add(NodeInfo);
		}
	}
	
	return Nodes;
}

// ========== 生成类型定义 ==========

FString FAnimNodeExporter::GenerateTypedRacketDefinition(const FNodeInfo& NodeInfo)
{
	FString Output;
	
	// 注释：节点描述
	if (!NodeInfo.Description.IsEmpty())
	{
		Output += FString::Printf(TEXT(";; %s\n"), *NodeInfo.Description);
	}
	Output += FString::Printf(TEXT(";; UE Class: %s\n"), *NodeInfo.ClassName);
	
	// 函数签名
	Output += GenerateNodeSignature(NodeInfo);
	
	return Output;
}

FString FAnimNodeExporter::GenerateNodeSignature(const FNodeInfo& NodeInfo)
{
	FString Sig = FString::Printf(TEXT("(: %s (->*"), *NodeInfo.NodeName);
	
	// 必需参数
	TArray<FString> RequiredParams;
	TArray<FString> OptionalParams;
	
	for (const FPinInfo& Pin : NodeInfo.InputPins)
	{
		FString ParamType = MapPinType(Pin.Type);
		if (Pin.bOptional)
		{
			OptionalParams.Add(FString::Printf(TEXT("#:%s %s"), *ToKebabCase(Pin.Name), *ParamType));
		}
		else
		{
			RequiredParams.Add(ParamType);
		}
	}
	
	// 输出必需参数
	Sig += TEXT(" (");
	for (int32 i = 0; i < RequiredParams.Num(); ++i)
	{
		if (i > 0) Sig += TEXT(" ");
		Sig += RequiredParams[i];
	}
	Sig += TEXT(")");
	
	// 输出可选参数
	if (OptionalParams.Num() > 0)
	{
		Sig += TEXT("\n                        (");
		for (int32 i = 0; i < OptionalParams.Num(); ++i)
		{
			if (i > 0) Sig += TEXT("\n                         ");
			Sig += OptionalParams[i];
		}
		Sig += TEXT(")");
	}
	
	// 返回类型
	Sig += TEXT("\n                        AnimNode))");
	
	return Sig;
}

// ========== 类型映射 ==========

FString FAnimNodeExporter::MapPinType(EPinType UEType)
{
	switch (UEType)
	{
	case EPinType::Pose:      return TEXT("AnimNode");
	case EPinType::Float:     return TEXT("(U Float Symbol)");  // 可以是字面量或参数
	case EPinType::Int:       return TEXT("(U Integer Symbol)");
	case EPinType::Bool:      return TEXT("(U Boolean Symbol)");
	case EPinType::Vector:    return TEXT("(U Vector Symbol)");
	case EPinType::Rotator:   return TEXT("(U Rotator Symbol)");
	case EPinType::Transform: return TEXT("(U Transform Symbol)");
	case EPinType::Name:      return TEXT("String");
	case EPinType::Object:    return TEXT("String");  // 资产路径
	default:                  return TEXT("Any");
	}
}

FString FAnimNodeExporter::MapPropertyType(FProperty* Property)
{
	if (Property->IsA<FFloatProperty>())
		return TEXT("Float");
	if (Property->IsA<FIntProperty>())
		return TEXT("Integer");
	if (Property->IsA<FBoolProperty>())
		return TEXT("Boolean");
	if (Property->IsA<FStrProperty>())
		return TEXT("String");
	if (Property->IsA<FNameProperty>())
		return TEXT("Symbol");
	
	// 复杂类型
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		if (ObjProp->PropertyClass->IsChildOf(UAnimationAsset::StaticClass()))
			return TEXT("AnimSequence");
		if (ObjProp->PropertyClass->IsChildOf(UBlendSpace::StaticClass()))
			return TEXT("BlendSpace");
	}
	
	return TEXT("Any");
}

// ========== 命名转换 ==========

FString FAnimNodeExporter::ToKebabCase(const FString& PascalCase)
{
	FString Result;
	for (int32 i = 0; i < PascalCase.Len(); ++i)
	{
		TCHAR C = PascalCase[i];
		if (FChar::IsUpper(C) && i > 0)
		{
			Result += TEXT("-");
		}
		Result += FChar::ToLower(C);
	}
	return Result;
}

#endif // WITH_EDITOR