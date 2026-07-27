// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangVariableCodec.h"

#if WITH_EDITOR

#include "EdGraphSchema_K2.h"

namespace
{
	bool BuildLegacyPinType(const EPinType Type, FEdGraphPinType& OutPinType)
	{
		switch (Type)
		{
		case EPinType::Float:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			return true;
		case EPinType::Int:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		case EPinType::Bool:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		case EPinType::Vector:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			return true;
		case EPinType::Rotator:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			return true;
		case EPinType::Transform:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			return true;
		case EPinType::Name:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		case EPinType::Object:
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			return true;
		default:
			return false;
		}
	}

	bool RequiresTypeObject(const FString& PinCategory)
	{
		return PinCategory == UEdGraphSchema_K2::PC_Struct.ToString()
			|| PinCategory == UEdGraphSchema_K2::PC_Object.ToString()
			|| PinCategory == UEdGraphSchema_K2::PC_Class.ToString()
			|| PinCategory == UEdGraphSchema_K2::PC_SoftObject.ToString()
			|| PinCategory == UEdGraphSchema_K2::PC_SoftClass.ToString()
			|| PinCategory == UEdGraphSchema_K2::PC_Interface.ToString();
	}

	FName OptionalSubCategory(const FString& SubCategory)
	{
		return SubCategory.IsEmpty() || SubCategory.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			? NAME_None
			: FName(*SubCategory);
	}

	bool LoadTypeObject(
		const FString& PinCategory,
		const FString& TypeObjectPath,
		const TCHAR* FieldName,
		UObject*& OutTypeObject,
		FString& OutError)
	{
		OutTypeObject = nullptr;
		if (RequiresTypeObject(PinCategory) && TypeObjectPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("pin category '%s' requires %s"), *PinCategory, FieldName);
			return false;
		}
		if (!TypeObjectPath.IsEmpty())
		{
			OutTypeObject = LoadObject<UObject>(nullptr, *TypeObjectPath);
			if (!OutTypeObject)
			{
				OutError = FString::Printf(TEXT("pin type object '%s' could not be loaded"), *TypeObjectPath);
				return false;
			}
		}
		return true;
	}
}

bool FAnimLangVariableCodec::BuildPinType(
	const FVariableDef& Variable,
	FEdGraphPinType& OutPinType,
	FString& OutError)
{
	OutPinType = FEdGraphPinType();
	OutError.Reset();

	if (Variable.PinCategory.IsEmpty())
	{
		if (Variable.Type == EPinType::Enum)
		{
			UEnum* EnumObject = Variable.TypeObjectPath.IsEmpty()
				? nullptr
				: LoadObject<UEnum>(nullptr, *Variable.TypeObjectPath);
			if (!EnumObject)
			{
				OutError = FString::Printf(TEXT("enum type object '%s' could not be loaded"), *Variable.TypeObjectPath);
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutPinType.PinSubCategoryObject = EnumObject;
		}
		else if (!BuildLegacyPinType(Variable.Type, OutPinType))
		{
			OutError = TEXT("legacy DSL type is unsupported");
			return false;
		}
	}
	else
	{
		OutPinType.PinCategory = FName(*Variable.PinCategory);
		OutPinType.PinSubCategory = OptionalSubCategory(Variable.PinSubCategory);
		UObject* TypeObject = nullptr;
		if (!LoadTypeObject(Variable.PinCategory, Variable.TypeObjectPath, TEXT(":type-object"), TypeObject, OutError))
		{
			return false;
		}
		OutPinType.PinSubCategoryObject = TypeObject;
	}

	const FString Container = Variable.ContainerType.ToLower();
	if (Container.IsEmpty() || Container == TEXT("none"))
	{
		OutPinType.ContainerType = EPinContainerType::None;
	}
	else if (Container == TEXT("array"))
	{
		OutPinType.ContainerType = EPinContainerType::Array;
	}
	else if (Container == TEXT("set"))
	{
		OutPinType.ContainerType = EPinContainerType::Set;
	}
	else if (Container == TEXT("map"))
	{
		if (Variable.ValuePinCategory.IsEmpty())
		{
			OutError = TEXT("map container requires :value-pin-category");
			return false;
		}
		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinValueType.TerminalCategory = FName(*Variable.ValuePinCategory);
		OutPinType.PinValueType.TerminalSubCategory = OptionalSubCategory(Variable.ValuePinSubCategory);

		UObject* ValueTypeObject = nullptr;
		if (!LoadTypeObject(
			Variable.ValuePinCategory,
			Variable.ValueTypeObjectPath,
			TEXT(":value-type-object"),
			ValueTypeObject,
			OutError))
		{
			return false;
		}
		OutPinType.PinValueType.TerminalSubCategoryObject = ValueTypeObject;
	}
	else
	{
		OutError = FString::Printf(TEXT("unknown container '%s'"), *Variable.ContainerType);
		return false;
	}

	OutPinType.bIsReference = Variable.bIsReference;
	OutPinType.bIsConst = Variable.bIsConst;
	OutPinType.bIsWeakPointer = Variable.bIsWeakPointer;
	OutPinType.bIsUObjectWrapper = Variable.bIsUObjectWrapper;
	return true;
}

#endif
