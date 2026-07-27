// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangVariableCodec.h"

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"

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

	FString QuoteDSLString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
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

FString FAnimLangVariableCodec::ExportPropertyExpression(const FProperty& Property, const void* Value)
{
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
		return Object
			? FString::Printf(TEXT("(asset %s)"), *QuoteDSLString(Object->GetPathName()))
			: TEXT("nil");
	}
	if (const FNameProperty* NameProperty = CastField<FNameProperty>(&Property))
	{
		return QuoteDSLString(NameProperty->GetPropertyValue(Value).ToString());
	}
	if (const FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
	{
		return QuoteDSLString(StringProperty->GetPropertyValue(Value));
	}
	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property))
	{
		return BoolProperty->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
	}

	FString Exported;
	Property.ExportTextItem_Direct(Exported, Value, Value, nullptr, PPF_None);
	if (Property.IsA<FNumericProperty>() || Property.IsA<FEnumProperty>())
	{
		return Exported;
	}
	return FString::Printf(TEXT("(ue-value %s)"), *QuoteDSLString(Exported));
}

bool FAnimLangVariableCodec::ExportMapEntries(
	const UAnimBlueprint& Blueprint,
	FVariableDef& InOutVariable,
	FString& OutError)
{
	InOutVariable.MapEntries.Reset();
	InOutVariable.bHasStructuredMapDefault = false;
	OutError.Reset();

	if (!Blueprint.GeneratedClass)
	{
		const FBPVariableDescription* Description = Blueprint.NewVariables.FindByPredicate(
			[&InOutVariable](const FBPVariableDescription& Candidate)
			{
				return Candidate.VarName.ToString() == InOutVariable.Name;
			});
		if (!Description || Description->DefaultValue.IsEmpty())
		{
			return true;
		}
		OutError = FString::Printf(
			TEXT("map variable '%s' has a default but no generated property"), *InOutVariable.Name);
		return false;
	}

	const FMapProperty* MapProperty = FindFProperty<FMapProperty>(
		Blueprint.GeneratedClass, FName(*InOutVariable.Name));
	if (!MapProperty)
	{
		OutError = FString::Printf(
			TEXT("generated map property '%s' could not be found"), *InOutVariable.Name);
		return false;
	}

	const UObject* Defaults = Blueprint.GeneratedClass->GetDefaultObject(false);
	if (!Defaults)
	{
		OutError = FString::Printf(
			TEXT("class defaults for map variable '%s' are unavailable"), *InOutVariable.Name);
		return false;
	}

	const void* MapAddress = MapProperty->ContainerPtrToValuePtr<void>(Defaults);
	FScriptMapHelper MapHelper(MapProperty, MapAddress);
	for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
	{
		if (!MapHelper.IsValidIndex(Index))
		{
			continue;
		}
		FMapEntryDef& Entry = InOutVariable.MapEntries.AddDefaulted_GetRef();
		Entry.KeyExpression = ExportPropertyExpression(*MapProperty->KeyProp, MapHelper.GetKeyPtr(Index));
		Entry.ValueExpression = ExportPropertyExpression(*MapProperty->ValueProp, MapHelper.GetValuePtr(Index));
	}

	InOutVariable.MapEntries.Sort([](const FMapEntryDef& Left, const FMapEntryDef& Right)
	{
		return Left.KeyExpression == Right.KeyExpression
			? Left.ValueExpression < Right.ValueExpression
			: Left.KeyExpression < Right.KeyExpression;
	});
	InOutVariable.bHasStructuredMapDefault = !InOutVariable.MapEntries.IsEmpty();
	return true;
}

#endif
