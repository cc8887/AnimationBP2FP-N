// RigLangImporter.cpp - Staging importer for Rig hierarchy and member variables

#include "RigLangImporter.h"

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#include "AnimLangTokenizer.h"
#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "EdGraph/RigVMEdGraphSchema.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/PackageName.h"
#include "Misc/AutomationTest.h"
#include "RigLangExporter.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMCore/RigVMGraphFunctionDefinition.h"
#include "RigVMCore/RigVMGraphFunctionHost.h"
#include "RigVMCore/RigVMGraphFunctionIdentifier.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMEditorAsset.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectHash.h"

namespace
{
void AddError(FRigLangImportResult& Result, const FString& Message, const FAnimLangSourceLoc& Location = {})
{
	Result.Diagnostics.Add(
		EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::RoundTrip, Message, Location);
}

ERigElementType ElementType(const ERigHierarchyElementKind Kind)
{
	switch (Kind)
	{
	case ERigHierarchyElementKind::Bone: return ERigElementType::Bone;
	case ERigHierarchyElementKind::Control: return ERigElementType::Control;
	case ERigHierarchyElementKind::Null: return ERigElementType::Null;
	case ERigHierarchyElementKind::Curve: return ERigElementType::Curve;
	}
	return ERigElementType::None;
}

const FRigHierarchyTransformAST* FindTransform(
	const FRigHierarchyElementAST& Element, const ERigHierarchyTransformRole Role)
{
	return Element.Transforms.FindByPredicate(
		[Role](const FRigHierarchyTransformAST& Transform) { return Transform.Role == Role; });
}

const FRigHierarchyStateAST* FindState(
	const FRigHierarchyElementAST& Element,
	const ERigHierarchyStateKind Kind,
	const FString& Role = FString())
{
	return Element.States.FindByPredicate([Kind, &Role](const FRigHierarchyStateAST& State)
	{
		return State.Kind == Kind && (Role.IsEmpty() || State.Role == Role);
	});
}

bool IsSupportedScalarType(const FString& CPPType)
{
	static const TSet<FString> Supported = {
		TEXT("bool"), TEXT("float"), TEXT("double"), TEXT("int32"), TEXT("int64"),
		TEXT("uint8"), TEXT("FName"), TEXT("FString"), TEXT("FVector"), TEXT("FVector2D"),
		TEXT("FVector4"), TEXT("FRotator"), TEXT("FQuat"), TEXT("FTransform"),
		TEXT("FEulerTransform"), TEXT("FLinearColor") };
	return Supported.Contains(CPPType);
}

int32 ExpectedControlComponentCount(const ERigControlType Type)
{
	switch (Type)
	{
	case ERigControlType::Bool:
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat:
	case ERigControlType::Integer: return 0;
	case ERigControlType::Vector2D: return 2;
	case ERigControlType::Position:
	case ERigControlType::Scale:
	case ERigControlType::Rotator: return 3;
	case ERigControlType::TransformNoScale: return 7;
	case ERigControlType::EulerTransform: return 9;
	case ERigControlType::Transform: return 10;
	}
	return INDEX_NONE;
}

bool IsArrayCPPType(const FString& CPPType)
{
	return CPPType.StartsWith(TEXT("TArray<")) && CPPType.EndsWith(TEXT(">"));
}

FString BaseCPPType(const FString& CPPType)
{
	return IsArrayCPPType(CPPType) ? CPPType.Mid(7, CPPType.Len() - 8) : CPPType;
}

double NormalizeNearZero(const double Value)
{
	constexpr double SemanticScale = 1000000000000.0;
	constexpr double SemanticZeroTolerance = 0.5 / SemanticScale;
	if (FMath::Abs(Value) < SemanticZeroTolerance) return 0.0;
	return FMath::RoundToDouble(Value * SemanticScale) / SemanticScale;
}

void NormalizeTransform(FTransform& Transform)
{
	FVector Translation = Transform.GetTranslation();
	Translation.X = NormalizeNearZero(Translation.X);
	Translation.Y = NormalizeNearZero(Translation.Y);
	Translation.Z = NormalizeNearZero(Translation.Z);
	FQuat Rotation = Transform.GetRotation();
	Rotation.X = NormalizeNearZero(Rotation.X);
	Rotation.Y = NormalizeNearZero(Rotation.Y);
	Rotation.Z = NormalizeNearZero(Rotation.Z);
	Rotation.W = NormalizeNearZero(Rotation.W);
	FVector Scale = Transform.GetScale3D();
	Scale.X = NormalizeNearZero(Scale.X);
	Scale.Y = NormalizeNearZero(Scale.Y);
	Scale.Z = NormalizeNearZero(Scale.Z);
	Transform.SetTranslation(Translation);
	Transform.SetRotation(Rotation);
	Transform.SetScale3D(Scale);
}

bool ParseTransformDefault(const FString& Value, FTransform& OutTransform)
{
	OutTransform = FTransform::Identity;
	if (TBaseStructure<FTransform>::Get()->ImportText(
		*Value, &OutTransform, nullptr, PPF_None, nullptr, TEXT("FTransform")))
	{
		NormalizeTransform(OutTransform);
		return true;
	}
	OutTransform = FTransform::Identity;
	if (!OutTransform.InitFromString(Value)) return false;
	NormalizeTransform(OutTransform);
	return true;
}

FString BlueprintMemberDefault(const FRigVariableAST& Variable)
{
	if (Variable.Type.CPPType != TEXT("FTransform")
		|| !Variable.Type.ContainerType.IsEmpty())
	{
		return Variable.DefaultValue;
	}
	FTransform Transform;
	return ParseTransformDefault(Variable.DefaultValue, Transform)
		? Transform.ToString() : Variable.DefaultValue;
}

FString CanonicalTypedPinDefault(const FRigPinAST& Pin)
{
	if (Pin.Type.CPPType == TEXT("bool")
		&& (Pin.DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Pin.DefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase)))
	{
		return Pin.DefaultValue.ToBool() ? TEXT("true") : TEXT("false");
	}
	if (Pin.Type.CPPTypeObject.IsEmpty()) return Pin.DefaultValue;
	UObject* TypeObject = FindObject<UObject>(nullptr, *Pin.Type.CPPTypeObject);
	if (!TypeObject) TypeObject = LoadObject<UObject>(nullptr, *Pin.Type.CPPTypeObject);
	UScriptStruct* Struct = Cast<UScriptStruct>(TypeObject);
	if (!Struct) return Pin.DefaultValue;
	FStructOnScope ValueScope(Struct);
	FStructOnScope DefaultScope(Struct);
	const TCHAR* End = Struct->ImportText(
		*Pin.DefaultValue, ValueScope.GetStructMemory(), nullptr,
		PPF_None, nullptr, *Struct->GetName());
	if (!End) return Pin.DefaultValue;
	while (FChar::IsWhitespace(*End)) ++End;
	if (*End != TEXT('\0')) return Pin.DefaultValue;
	FString Canonical;
	Struct->ExportText(Canonical, ValueScope.GetStructMemory(),
		DefaultScope.GetStructMemory(), nullptr, PPF_None, nullptr);
	return Canonical;
}

bool MetadataPayloadHasExpectedShape(const FRigHierarchyMetadataAST& Metadata)
{
	auto Scalar = [](const int32 Count) { return Count == 1; };
	switch (Metadata.Kind)
	{
	case ERigHierarchyMetadataValueKind::Bool: return Scalar(Metadata.BoolValues.Num());
	case ERigHierarchyMetadataValueKind::Integer: return Scalar(Metadata.IntegerValues.Num());
	case ERigHierarchyMetadataValueKind::Float: return Scalar(Metadata.NumberValues.Num());
	case ERigHierarchyMetadataValueKind::Name:
	case ERigHierarchyMetadataValueKind::ElementKey: return Scalar(Metadata.StringValues.Num());
	case ERigHierarchyMetadataValueKind::Vector: return Scalar(Metadata.VectorValues.Num());
	case ERigHierarchyMetadataValueKind::Rotator: return Scalar(Metadata.RotatorValues.Num());
	case ERigHierarchyMetadataValueKind::Quat: return Scalar(Metadata.QuatValues.Num());
	case ERigHierarchyMetadataValueKind::Transform: return Scalar(Metadata.TransformValues.Num());
	case ERigHierarchyMetadataValueKind::LinearColor: return Scalar(Metadata.ColorValues.Num());
	case ERigHierarchyMetadataValueKind::BoolArray:
	case ERigHierarchyMetadataValueKind::IntegerArray:
	case ERigHierarchyMetadataValueKind::FloatArray:
	case ERigHierarchyMetadataValueKind::NameArray:
	case ERigHierarchyMetadataValueKind::VectorArray:
	case ERigHierarchyMetadataValueKind::RotatorArray:
	case ERigHierarchyMetadataValueKind::QuatArray:
	case ERigHierarchyMetadataValueKind::TransformArray:
	case ERigHierarchyMetadataValueKind::LinearColorArray:
	case ERigHierarchyMetadataValueKind::ElementKeyArray: return true;
	}
	return false;
}

FString EffectiveStableId(const FRigHierarchyElementAST& Element)
{
	if (!Element.StableId.IsEmpty()) return Element.StableId;
	return FRigElementKey(FName(*Element.Name), ElementType(Element.Kind)).ToString();
}

bool TryParseRigElementKey(const FString& Text, FRigElementKey& OutKey)
{
	int32 OpenParen = INDEX_NONE;
	if (!Text.FindChar(TEXT('('), OpenParen) || OpenParen <= 0
		|| !Text.EndsWith(TEXT(")"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	const FString TypeText = Text.Left(OpenParen);
	const FString NameText = Text.Mid(OpenParen + 1, Text.Len() - OpenParen - 2);
	if (NameText.IsEmpty() || NameText.Contains(TEXT("(")) || NameText.Contains(TEXT(")")))
		return false;
	const int64 TypeValue = StaticEnum<ERigElementType>()->GetValueByNameString(TypeText);
	if (TypeValue <= static_cast<int64>(ERigElementType::None)
		|| TypeValue >= static_cast<int64>(ERigElementType::All))
	{
		return false;
	}
	OutKey = FRigElementKey(FName(*NameText), static_cast<ERigElementType>(TypeValue));
	return OutKey.IsValid();
}

bool Preflight(
	const FRigModuleAST& Module,
	FRigLangImportResult& Result,
	TArray<int32>& OutOrder)
{
	TMap<FString, int32> ElementByName;
	TMap<FString, int32> ElementByStableId;
	for (int32 Index = 0; Index < Module.Hierarchy.Num(); ++Index)
	{
		const FRigHierarchyElementAST& Element = Module.Hierarchy[Index];
		if (Element.Name.IsEmpty() || ElementByName.Contains(Element.Name))
		{
			AddError(Result, TEXT("Rig hierarchy element names must be non-empty and unique"), Element.Location);
			continue;
		}
		ElementByName.Add(Element.Name, Index);
		const FString StableId = EffectiveStableId(Element);
		if (StableId.IsEmpty() || ElementByStableId.Contains(StableId))
		{
			AddError(Result, TEXT("Rig hierarchy stable IDs must be non-empty and unique"), Element.Location);
			continue;
		}
		ElementByStableId.Add(StableId, Index);
	}
	for (const FRigHierarchyElementAST& Element : Module.Hierarchy)
	{
		if (!Element.ParentName.IsEmpty() && !ElementByName.Contains(Element.ParentName))
		{
			AddError(Result, FString::Printf(
				TEXT("Hierarchy element '%s' has missing parent '%s'"), *Element.Name, *Element.ParentName),
				Element.Location);
		}
		TSet<FString> TypedParents;
		for (const FRigHierarchyParentAST& Parent : Element.Parents)
		{
			if (Parent.StableId.IsEmpty() || TypedParents.Contains(Parent.StableId))
			{
				AddError(Result, FString::Printf(
					TEXT("Hierarchy element '%s' has a duplicate or empty typed parent '%s'"),
					*Element.Name, *Parent.StableId), Parent.Location);
				continue;
			}
			TypedParents.Add(Parent.StableId);
			if (!ElementByStableId.Contains(Parent.StableId))
			{
				AddError(Result, FString::Printf(
					TEXT("Hierarchy element '%s' has missing typed parent '%s'"),
					*Element.Name, *Parent.StableId), Parent.Location);
			}
		}

		TSet<FString> MetadataNames;
		for (const FRigHierarchyMetadataAST& Metadata : Element.Metadata)
		{
			if (Metadata.Name.IsEmpty() || MetadataNames.Contains(Metadata.Name))
			{
				AddError(Result, FString::Printf(
					TEXT("Hierarchy element '%s' has duplicate or empty metadata name '%s'"),
					*Element.Name, *Metadata.Name), Metadata.Location);
				continue;
			}
			MetadataNames.Add(Metadata.Name);
			if (!MetadataPayloadHasExpectedShape(Metadata))
			{
				AddError(Result, FString::Printf(
					TEXT("Metadata '%s' on '%s' has an invalid value shape"),
					*Metadata.Name, *Element.Name), Metadata.Location);
			}
			if (Metadata.Kind == ERigHierarchyMetadataValueKind::ElementKey
				|| Metadata.Kind == ERigHierarchyMetadataValueKind::ElementKeyArray)
			{
				for (const FString& StableId : Metadata.StringValues)
				{
					FRigElementKey ReferencedKey;
					if (!TryParseRigElementKey(StableId, ReferencedKey))
					{
						AddError(Result, FString::Printf(
							TEXT("Metadata '%s' on '%s' has invalid hierarchy key '%s'"),
							*Metadata.Name, *Element.Name, *StableId), Metadata.Location);
					}
				}
			}
		}

		if (Element.Kind == ERigHierarchyElementKind::Bone)
		{
			if (const FRigHierarchyStateAST* State = FindState(Element, ERigHierarchyStateKind::BoneType))
			{
				if (StaticEnum<ERigBoneType>()->GetValueByNameString(State->Type) == INDEX_NONE)
					AddError(Result, FString::Printf(TEXT("Bone '%s' has invalid bone type '%s'"),
						*Element.Name, *State->Type), State->Location);
			}
		}
		if (Element.Kind == ERigHierarchyElementKind::Control)
		{
			TArray<const FRigHierarchyStateAST*> SettingsStates;
			for (const FRigHierarchyStateAST& State : Element.States)
				if (State.Kind == ERigHierarchyStateKind::ControlSettings) SettingsStates.Add(&State);
			FRigControlSettings Settings;
			if (SettingsStates.Num() != 1 || !FRigControlSettings::StaticStruct()->ImportText(
				SettingsStates.Num() == 1 ? *SettingsStates[0]->SerializedValue : TEXT(""),
				&Settings, nullptr, PPF_None, nullptr, TEXT("FRigControlSettings")))
			{
				AddError(Result, FString::Printf(TEXT("Control '%s' must have one valid settings state"), *Element.Name), Element.Location);
			}
			else
			{
				const FString ExpectedType = StaticEnum<ERigControlType>()->GetNameStringByValue(
					static_cast<int64>(Settings.ControlType));
				TSet<FString> ValueRoles;
				bool bHasInitial = false;
				for (const FRigHierarchyStateAST& State : Element.States)
				{
					if (State.Kind == ERigHierarchyStateKind::ControlValue)
					{
						if (State.Type != ExpectedType || State.Components.Num() != ExpectedControlComponentCount(Settings.ControlType)
							|| (State.Role != TEXT("initial") && State.Role != TEXT("current")
								&& State.Role != TEXT("minimum") && State.Role != TEXT("maximum"))
							|| ValueRoles.Contains(State.Role))
						{
							AddError(Result, FString::Printf(TEXT("Control '%s' has an invalid or duplicate '%s' value state"),
								*Element.Name, *State.Role), State.Location);
						}
						ValueRoles.Add(State.Role);
						bHasInitial |= State.Role == TEXT("initial");
					}
					else if (State.Kind == ERigHierarchyStateKind::PreferredEuler)
					{
						if (State.Components.Num() != 3
							|| StaticEnum<EEulerRotationOrder>()->GetValueByNameString(State.Type) == INDEX_NONE)
						{
							AddError(Result, FString::Printf(TEXT("Control '%s' has invalid preferred Euler state"),
								*Element.Name), State.Location);
						}
					}
				}
				if (!bHasInitial)
					AddError(Result, FString::Printf(TEXT("Control '%s' has no initial value"), *Element.Name), Element.Location);
			}
		}
	}

	TSet<int32> Added;
	while (Added.Num() < Module.Hierarchy.Num())
	{
		bool bProgress = false;
		for (int32 Index = 0; Index < Module.Hierarchy.Num(); ++Index)
		{
			if (Added.Contains(Index)) continue;
			const FRigHierarchyElementAST& Element = Module.Hierarchy[Index];
			bool bParentsAdded = Element.ParentName.IsEmpty()
				|| (ElementByName.Contains(Element.ParentName)
					&& Added.Contains(ElementByName.FindChecked(Element.ParentName)));
			for (const FRigHierarchyParentAST& Parent : Element.Parents)
			{
				bParentsAdded &= ElementByStableId.Contains(Parent.StableId)
					&& Added.Contains(ElementByStableId.FindChecked(Parent.StableId));
			}
			if (bParentsAdded)
			{
				Added.Add(Index);
				OutOrder.Add(Index);
				bProgress = true;
			}
		}
		if (!bProgress)
		{
			AddError(Result, TEXT("Rig hierarchy contains a parent cycle"));
			break;
		}
	}

	TSet<FString> VariableNames;
	TSet<FGuid> VariableGuids;
	for (const FRigVariableAST& Variable : Module.Variables)
	{
		if (Variable.Name.IsEmpty() || VariableNames.Contains(Variable.Name))
		{
			AddError(Result, TEXT("Rig variable names must be non-empty and unique"), Variable.Location);
			continue;
		}
		VariableNames.Add(Variable.Name);
		if (Variable.Access == ERigVariableAccess::PublicOutput)
		{
			AddError(Result, FString::Printf(
				TEXT("Rig variable '%s' uses unsupported public-output access"), *Variable.Name), Variable.Location);
		}
		if (!Variable.Type.ContainerType.IsEmpty() && Variable.Type.ContainerType != TEXT("array"))
		{
			AddError(Result, FString::Printf(
				TEXT("Unsupported Rig variable container '%s' for '%s'"),
				*Variable.Type.ContainerType, *Variable.Name), Variable.Location);
		}
		if (Variable.Type.CPPType.IsEmpty())
		{
			AddError(Result, FString::Printf(TEXT("Rig variable '%s' has no CPP type"), *Variable.Name), Variable.Location);
		}
		UObject* TypeObject = nullptr;
		if (!Variable.Type.CPPTypeObject.IsEmpty())
		{
			TypeObject = LoadObject<UObject>(nullptr, *Variable.Type.CPPTypeObject);
			if (!TypeObject)
			{
				AddError(Result, FString::Printf(
					TEXT("Rig variable '%s' has unresolved type object '%s'"),
					*Variable.Name, *Variable.Type.CPPTypeObject), Variable.Location);
			}
		}
		else if (!IsSupportedScalarType(BaseCPPType(Variable.Type.CPPType)))
		{
			AddError(Result, FString::Printf(
				TEXT("Unsupported Rig variable CPP type '%s' for '%s'"),
				*Variable.Type.CPPType, *Variable.Name), Variable.Location);
		}
		if (Variable.Type.ContainerType.IsEmpty() && IsArrayCPPType(Variable.Type.CPPType))
		{
			AddError(Result, FString::Printf(
				TEXT("Rig variable '%s' has array CPP type without array container"), *Variable.Name), Variable.Location);
		}
		FGuid Guid;
		if (FGuid::Parse(Variable.StableId, Guid))
		{
			if (VariableGuids.Contains(Guid))
				AddError(Result, FString::Printf(TEXT("Rig variable '%s' has duplicate Guid"), *Variable.Name), Variable.Location);
			VariableGuids.Add(Guid);
		}
		else if (!Variable.StableId.IsEmpty() && Variable.StableId != Variable.Name)
		{
			AddError(Result, FString::Printf(TEXT("Rig variable '%s' has invalid stable ID '%s'"),
				*Variable.Name, *Variable.StableId), Variable.Location);
		}
	}
	return !Result.Diagnostics.HasErrors();
}

bool ControlValueFromState(
	const FRigHierarchyStateAST* State, const ERigControlType Type, FRigControlValue& OutValue)
{
	if (!State || State->Components.Num() != ExpectedControlComponentCount(Type)) return false;
	auto Component = [State](const int32 Index)
	{
		return static_cast<float>(State->Components[Index]);
	};
	switch (Type)
	{
	case ERigControlType::Bool: OutValue = FRigControlValue::Make(State->bBoolValue); return true;
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat: OutValue = FRigControlValue::Make(static_cast<float>(State->NumberValue)); return true;
	case ERigControlType::Integer: OutValue = FRigControlValue::Make(static_cast<int32>(State->IntegerValue)); return true;
	case ERigControlType::Position:
	case ERigControlType::Scale:
	case ERigControlType::Rotator:
		OutValue = FRigControlValue::Make(FVector3f(Component(0), Component(1), Component(2))); return true;
	case ERigControlType::Vector2D:
		OutValue = FRigControlValue::Make(FVector3f(Component(0), Component(1), 0.0f)); return true;
	case ERigControlType::Transform:
	{
		FRigControlValue::FTransform_Float Value;
		Value.TranslationX = Component(0); Value.TranslationY = Component(1); Value.TranslationZ = Component(2);
		Value.RotationX = Component(3); Value.RotationY = Component(4); Value.RotationZ = Component(5); Value.RotationW = Component(6);
		Value.ScaleX = Component(7); Value.ScaleY = Component(8); Value.ScaleZ = Component(9);
		OutValue = FRigControlValue::Make(Value); return true;
	}
	case ERigControlType::TransformNoScale:
	{
		FRigControlValue::FTransformNoScale_Float Value;
		Value.TranslationX = Component(0); Value.TranslationY = Component(1); Value.TranslationZ = Component(2);
		Value.RotationX = Component(3); Value.RotationY = Component(4); Value.RotationZ = Component(5); Value.RotationW = Component(6);
		OutValue = FRigControlValue::Make(Value); return true;
	}
	case ERigControlType::EulerTransform:
	{
		FRigControlValue::FEulerTransform_Float Value;
		Value.TranslationX = Component(0); Value.TranslationY = Component(1); Value.TranslationZ = Component(2);
		Value.RotationPitch = Component(3); Value.RotationYaw = Component(4); Value.RotationRoll = Component(5);
		Value.ScaleX = Component(6); Value.ScaleY = Component(7); Value.ScaleZ = Component(8);
		OutValue = FRigControlValue::Make(Value); return true;
	}
	}
	return false;
}

bool RestoreMetadata(
	URigHierarchy* Hierarchy,
	const FRigElementKey& Key,
	const FRigHierarchyMetadataAST& Metadata)
{
	const FName Name(*Metadata.Name);
	auto ElementKeys = [&Metadata]()
	{
		TArray<FRigElementKey> Values;
		for (const FString& Value : Metadata.StringValues)
		{
			FRigElementKey Key;
			if (TryParseRigElementKey(Value, Key)) Values.Add(Key);
		}
		return Values;
	};
	switch (Metadata.Kind)
	{
	case ERigHierarchyMetadataValueKind::Bool: return Metadata.BoolValues.Num() == 1 && Hierarchy->SetBoolMetadata(Key, Name, Metadata.BoolValues[0]);
	case ERigHierarchyMetadataValueKind::Integer: return Metadata.IntegerValues.Num() == 1 && Hierarchy->SetInt32Metadata(Key, Name, static_cast<int32>(Metadata.IntegerValues[0]));
	case ERigHierarchyMetadataValueKind::Float: return Metadata.NumberValues.Num() == 1 && Hierarchy->SetFloatMetadata(Key, Name, static_cast<float>(Metadata.NumberValues[0]));
	case ERigHierarchyMetadataValueKind::Name: return Metadata.StringValues.Num() == 1 && Hierarchy->SetNameMetadata(Key, Name, FName(*Metadata.StringValues[0]));
	case ERigHierarchyMetadataValueKind::Vector: return Metadata.VectorValues.Num() == 1 && Hierarchy->SetVectorMetadata(Key, Name, Metadata.VectorValues[0]);
	case ERigHierarchyMetadataValueKind::Rotator: return Metadata.RotatorValues.Num() == 1 && Hierarchy->SetRotatorMetadata(Key, Name, Metadata.RotatorValues[0]);
	case ERigHierarchyMetadataValueKind::Quat: return Metadata.QuatValues.Num() == 1 && Hierarchy->SetQuatMetadata(Key, Name, Metadata.QuatValues[0]);
	case ERigHierarchyMetadataValueKind::Transform: return Metadata.TransformValues.Num() == 1 && Hierarchy->SetTransformMetadata(Key, Name, Metadata.TransformValues[0]);
	case ERigHierarchyMetadataValueKind::LinearColor: return Metadata.ColorValues.Num() == 1 && Hierarchy->SetLinearColorMetadata(Key, Name, Metadata.ColorValues[0]);
	case ERigHierarchyMetadataValueKind::ElementKey:
	{
		FRigElementKey Value;
		return Metadata.StringValues.Num() == 1
			&& TryParseRigElementKey(Metadata.StringValues[0], Value)
			&& Hierarchy->SetRigElementKeyMetadata(Key, Name, Value);
	}
	case ERigHierarchyMetadataValueKind::BoolArray: return Hierarchy->SetBoolArrayMetadata(Key, Name, Metadata.BoolValues);
	case ERigHierarchyMetadataValueKind::IntegerArray:
	{
		TArray<int32> Values; for (const int64 Value : Metadata.IntegerValues) Values.Add(static_cast<int32>(Value));
		return Hierarchy->SetInt32ArrayMetadata(Key, Name, Values);
	}
	case ERigHierarchyMetadataValueKind::FloatArray:
	{
		TArray<float> Values; for (const double Value : Metadata.NumberValues) Values.Add(static_cast<float>(Value));
		return Hierarchy->SetFloatArrayMetadata(Key, Name, Values);
	}
	case ERigHierarchyMetadataValueKind::NameArray:
	{
		TArray<FName> Values; for (const FString& Value : Metadata.StringValues) Values.Add(FName(*Value));
		return Hierarchy->SetNameArrayMetadata(Key, Name, Values);
	}
	case ERigHierarchyMetadataValueKind::VectorArray: return Hierarchy->SetVectorArrayMetadata(Key, Name, Metadata.VectorValues);
	case ERigHierarchyMetadataValueKind::RotatorArray: return Hierarchy->SetRotatorArrayMetadata(Key, Name, Metadata.RotatorValues);
	case ERigHierarchyMetadataValueKind::QuatArray: return Hierarchy->SetQuatArrayMetadata(Key, Name, Metadata.QuatValues);
	case ERigHierarchyMetadataValueKind::TransformArray: return Hierarchy->SetTransformArrayMetadata(Key, Name, Metadata.TransformValues);
	case ERigHierarchyMetadataValueKind::LinearColorArray: return Hierarchy->SetLinearColorArrayMetadata(Key, Name, Metadata.ColorValues);
	case ERigHierarchyMetadataValueKind::ElementKeyArray: return Hierarchy->SetRigElementKeyArrayMetadata(Key, Name, ElementKeys());
	}
	return false;
}

FString HierarchyVariableSemanticSnapshot(
	const FRigModuleAST& Source,
	const TSet<FString>& GeneratedIdentityVariables)
{
	FRigModuleAST Copy = Source;
	Copy.Header = FRigModuleHeaderAST();
	Copy.Imports.Reset();
	Copy.Graphs.Reset();
	Copy.Functions.Reset();
	Copy.Entries.Reset();
	for (FRigHierarchyElementAST& Element : Copy.Hierarchy)
	{
		for (FRigHierarchyTransformAST& Transform : Element.Transforms)
		{
			NormalizeTransform(Transform.Value);
		}
		for (FRigHierarchyStateAST& State : Element.States)
		{
			State.NumberValue = NormalizeNearZero(State.NumberValue);
			for (double& Component : State.Components)
			{
				Component = NormalizeNearZero(Component);
			}
		}
	}
	for (FRigVariableAST& Variable : Copy.Variables)
	{
		if (GeneratedIdentityVariables.Contains(Variable.Name))
		{
			Variable.StableId = Variable.Name;
		}
		Variable.DefaultValue = BlueprintMemberDefault(Variable);
	}
	return Copy.ToCanonicalHashInput();
}

FString UnquoteRigLangProperty(const FString& Value)
{
	if (Value.Len() < 2 || Value[0] != TEXT('"') || Value[Value.Len() - 1] != TEXT('"')) return Value;
	FString Result = Value.Mid(1, Value.Len() - 2);
	Result.ReplaceInline(TEXT("\\\""), TEXT("\""));
	Result.ReplaceInline(TEXT("\\n"), TEXT("\n"));
	Result.ReplaceInline(TEXT("\\r"), TEXT("\r"));
	Result.ReplaceInline(TEXT("\\t"), TEXT("\t"));
	Result.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
	return Result;
}

bool RestoreAndVerifyVariableRemapping(
	URigVMController* Controller,
	URigVMFunctionReferenceNode* Node,
	const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	const FString Encoded = Source.Properties.FindRef(TEXT("variable-remapping"));
	TArray<FAnimLangToken> Tokens;
	TArray<FAnimLangLexError> Errors;
	if (!FAnimLangTokenizer::Tokenize(Encoded.IsEmpty() ? TEXT("()") : Encoded, Tokens, Errors)
		|| Errors.Num() != 0)
	{
		AddError(Result, FString::Printf(TEXT("Malformed variable remapping on Rig call '%s'"),
			*Source.StableId), Source.Location);
		return false;
	}
	int32 Index = 0;
	auto Take = [&Tokens, &Index](const EAnimLangTokenType Type) -> const FAnimLangToken*
	{
		if (!Tokens.IsValidIndex(Index) || Tokens[Index].Type != Type) return nullptr;
		return &Tokens[Index++];
	};
	if (!Take(EAnimLangTokenType::LParen)) return false;
	TMap<FName, FName> Expected;
	while (Tokens.IsValidIndex(Index) && Tokens[Index].Type == EAnimLangTokenType::LParen)
	{
		++Index;
		const FAnimLangToken* Inner = Take(EAnimLangTokenType::String);
		const FAnimLangToken* Outer = Take(EAnimLangTokenType::String);
		URigVMPin* FunctionPin = Inner ? Node->FindRootPinByName(FName(*Inner->Value)) : nullptr;
		const bool bClosedPair = Take(EAnimLangTokenType::RParen) != nullptr;
		const FName* ExistingOuter = Inner ? Node->GetVariableMap().Find(FName(*Inner->Value)) : nullptr;
		const bool bAlreadyBound = ExistingOuter && Outer && *ExistingOuter == FName(*Outer->Value);
		const bool bBound = Inner && Outer && bClosedPair && FunctionPin
			&& (bAlreadyBound
				|| Controller->BindPinToVariable(FunctionPin->GetPinPath(), Outer->Value, false, false));
		if (!bBound)
		{
			TArray<FString> PinNames;
			for (const URigVMPin* Pin : Node->GetPins()) if (Pin) PinNames.Add(Pin->GetName());
			TArray<FString> ExternalNames;
			for (const FRigVMExternalVariable& Variable : Node->GetReferencedFunctionHeader().ExternalVariables)
				ExternalNames.Add(Variable.GetName().ToString());
			AddError(Result, FString::Printf(
				TEXT("Failed variable remapping on Rig call '%s': inner='%s' outer='%s' pin=%s pins=[%s] externals=[%s]"),
				*Source.StableId, Inner ? *Inner->Value : TEXT("<missing>"),
				Outer ? *Outer->Value : TEXT("<missing>"), FunctionPin ? TEXT("yes") : TEXT("no"),
				*FString::Join(PinNames, TEXT(",")), *FString::Join(ExternalNames, TEXT(","))), Source.Location);
			return false;
		}
		Expected.Add(FName(*Inner->Value), FName(*Outer->Value));
	}
	if (!Take(EAnimLangTokenType::RParen) || Node->GetVariableMap().OrderIndependentCompareEqual(Expected) == false)
	{
		AddError(Result, FString::Printf(TEXT("Variable remapping mismatch on Rig call '%s'"),
			*Source.StableId), Source.Location);
		return false;
	}
	return true;
}

FString GraphNameFromAST(const FRigGraphAST& Graph)
{
	auto Normalize = [](FString Name)
	{
		const FString NamedModelPrefix = FString(FRigVMClient::RigVMModelPrefix) + TEXT(" ");
		Name.RemoveFromStart(NamedModelPrefix);
		Name.TrimStartAndEndInline();
		return Name;
	};
	if (const FString* Name = Graph.Properties.Find(TEXT("graph-name")))
	{
		const FString Unquoted = UnquoteRigLangProperty(*Name);
		if (!Unquoted.IsEmpty()) return Normalize(Unquoted);
	}
	FString Name = Graph.StableId;
	int32 Separator = INDEX_NONE;
	if (Name.FindLastChar(TEXT(':'), Separator)) Name = Name.Mid(Separator + 1);
	if (Name.FindLastChar(TEXT('.'), Separator)) Name = Name.Mid(Separator + 1);
	return Normalize(Name);
}

struct FGraphSemanticTokenIndex
{
	TMap<FString, FString> ByStableId;
	TMap<FString, TArray<FString>> StableIdsByToken;

	bool IsUniqueToken(const FString& Token) const
	{
		const TArray<FString>* StableIds = StableIdsByToken.Find(Token);
		return StableIds && StableIds->Num() == 1;
	}

	TArray<FString> CollisionMarkers() const
	{
		TArray<FString> Markers;
		for (const TPair<FString, TArray<FString>>& Pair : StableIdsByToken)
		{
			if (Pair.Value.Num() < 2) continue;
			TArray<FString> StableIds = Pair.Value;
			StableIds.Sort();
			Markers.Add(Pair.Key + TEXT(" => ") + FString::Join(StableIds, TEXT(",")));
		}
		Markers.Sort();
		return Markers;
	}

	TSet<FString> CollisionTokens() const
	{
		TSet<FString> Tokens;
		for (const TPair<FString, TArray<FString>>& Pair : StableIdsByToken)
			if (Pair.Value.Num() > 1) Tokens.Add(Pair.Key);
		return Tokens;
	}
};

FGraphSemanticTokenIndex BuildGraphSemanticTokenIndex(const FRigModuleAST& Module)
{
	FGraphSemanticTokenIndex Result;
	TMap<FString, const FRigGraphAST*> GraphsById;
	for (const FRigGraphAST& Graph : Module.Graphs) GraphsById.Add(Graph.StableId, &Graph);
	TSet<FString> Visiting;
	TFunction<FString(const FRigGraphAST&)> Build = [&](const FRigGraphAST& Graph) -> FString
	{
		if (const FString* Existing = Result.ByStableId.Find(Graph.StableId)) return *Existing;
		if (Visiting.Contains(Graph.StableId))
			return TEXT("$graph-cycle:") + Graph.Role + TEXT(":") + GraphNameFromAST(Graph);
		Visiting.Add(Graph.StableId);
		const FString Self = Graph.Role + TEXT(":") + GraphNameFromAST(Graph);
		FString Token = TEXT("graph/") + Self;
		if (const FRigGraphAST* const* ParentPtr = GraphsById.Find(Graph.ParentStableId))
		{
			const FRigGraphAST& Parent = **ParentPtr;
			TArray<FString> OwnerIdentities;
			for (const FRigNodeAST& Node : Parent.Nodes)
			{
				if (Node.ContainedGraphStableId != Graph.StableId) continue;
				const FString NodeIdentity = Node.StableId.IsEmpty() ? Node.Guid : Node.StableId;
				OwnerIdentities.Add(FString::FromInt(static_cast<int32>(Node.Kind))
					+ TEXT(":") + NodeIdentity);
			}
			OwnerIdentities.Sort();
			const FString Owner = OwnerIdentities.Num() == 1
				? OwnerIdentities[0]
				: TEXT("$owners[") + FString::Join(OwnerIdentities, TEXT(",")) + TEXT("]");
			Token = Build(Parent) + TEXT("/owner/") + Owner + TEXT("/graph/") + Self;
		}
		Visiting.Remove(Graph.StableId);
		Result.ByStableId.Add(Graph.StableId, Token);
		Result.StableIdsByToken.FindOrAdd(Token).Add(Graph.StableId);
		return Token;
	};
	for (const FRigGraphAST& Graph : Module.Graphs) Build(Graph);
	return Result;
}

ERigVMPinDirection ToRigVMPinDirection(const ERigPinDirection Direction)
{
	switch (Direction)
	{
	case ERigPinDirection::Output: return ERigVMPinDirection::Output;
	case ERigPinDirection::IO: return ERigVMPinDirection::IO;
	case ERigPinDirection::Visible: return ERigVMPinDirection::Visible;
	case ERigPinDirection::Hidden: return ERigVMPinDirection::Hidden;
	case ERigPinDirection::Invalid: return ERigVMPinDirection::Invalid;
	default: return ERigVMPinDirection::Input;
	}
}

#if ENGINE_MAJOR_VERSION >= 5
TRigVMTypeIndex TypeIndexForPin(const FRigPinAST& PinAST)
{
	FString CPPType = PinAST.Type.CPPType;
	if (PinAST.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
		CPPType = TEXT("TArray<") + CPPType + TEXT(">");
	UObject* CPPTypeObject = PinAST.Type.CPPTypeObject.IsEmpty()
		? nullptr : LoadObject<UObject>(nullptr, *PinAST.Type.CPPTypeObject);
	return FRigVMRegistry::Get().FindOrAddType(
		FRigVMTemplateArgumentType(FName(*CPPType), CPPTypeObject));
}

bool ResolveTemplateNodeFromSource(
	URigVMController* Controller,
	URigVMNode*& Node,
	const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	URigVMTemplateNode* TemplateNode = Cast<URigVMTemplateNode>(Node);
	if (!TemplateNode) return true;
	const FRigVMTemplate* Template = TemplateNode->GetTemplate();
	if (!Template)
	{
		const bool bRequiresRegisteredTemplate = Source.Kind == ERigNodeKind::Dispatch
			|| (Source.Kind == ERigNodeKind::Unit
				&& Source.Properties.Contains(TEXT("template-notation"))
				&& UnquoteRigLangProperty(Source.Properties.FindRef(TEXT("script-struct"))).IsEmpty());
		if (!bRequiresRegisteredTemplate) return true;
		if (!Controller->UnresolveTemplateNodes(TArray<URigVMNode*>({TemplateNode}), false))
		{
			AddError(Result, TEXT("Failed to recover registered template for: ")
				+ Source.StableId, Source.Location);
			return false;
		}
		Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
		TemplateNode = Cast<URigVMTemplateNode>(Node);
		Template = TemplateNode ? TemplateNode->GetTemplate() : nullptr;
		if (!Template)
		{
			AddError(Result, TEXT("Registered template remains unavailable for: ")
				+ Source.StableId, Source.Location);
			return false;
		}
	}
	FRigVMTemplate::FTypeMap SourceTypes;
	for (const FRigPinAST& PinAST : Source.Pins)
	{
		const FName ArgumentName(*PinAST.Path);
		if (!Template->FindArgument(ArgumentName)
			|| PinAST.Type.CPPType.IsEmpty()
			|| PinAST.Type.CPPType == TEXT("FRigVMUnknownType")) continue;
		SourceTypes.Add(ArgumentName, TypeIndexForPin(PinAST));
	}
	const bool bSourceResolved = Source.Properties.FindRef(TEXT("template-resolved")) == TEXT("true");
	const FString ExpectedFunction = bSourceResolved
		? UnquoteRigLangProperty(Source.Properties.FindRef(TEXT("resolved-function")))
		: FString();
	if (!bSourceResolved)
	{
		for (const FRigPinAST& PinAST : Source.Pins)
		{
			URigVMPin* Pin = TemplateNode->FindPin(PinAST.Path);
			if (!Pin || PinAST.Type.CPPType == TEXT("FRigVMUnknownType")) continue;
			const TRigVMTypeIndex SourceType = TypeIndexForPin(PinAST);
			if (Pin->GetTypeIndex() == SourceType) continue;
			if (!Controller->ResolveWildCardPin(Pin, SourceType, false, false))
			{
				AddError(Result, TEXT("Failed public unresolved template type-map restoration for '")
					+ Source.StableId + TEXT(".") + PinAST.Path + TEXT("'"), PinAST.Location);
				return false;
			}
			Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
			TemplateNode = Cast<URigVMTemplateNode>(Node);
			if (!TemplateNode) return false;
		}
		if (TemplateNode->IsResolved())
		{
			AddError(Result, TEXT("Public type-map restoration unexpectedly resolved template: ")
				+ Source.StableId, Source.Location);
			return false;
		}
		return true;
	}
	const int32 ExactPermutation = Template->FindPermutation(SourceTypes);
	if (ExactPermutation != INDEX_NONE)
	{
		const FRigVMTemplate::FTypeMap PermutationTypes =
			Template->GetTypesForPermutation(ExactPermutation);
		for (const TPair<FName, TRigVMTypeIndex>& SourceType : SourceTypes)
		{
			if (PermutationTypes.FindRef(SourceType.Key) != SourceType.Value)
			{
				AddError(Result, FString::Printf(
					TEXT("Template permutation type mismatch for '%s.%s'"),
					*Source.StableId, *SourceType.Key.ToString()), Source.Location);
				return false;
			}
		}
		if (!Controller->FullyResolveTemplateNode(TemplateNode, ExactPermutation, false))
		{
			AddError(Result, FString::Printf(
				TEXT("Failed exact public template permutation '%s' on '%s'"),
				*ExpectedFunction, *Source.StableId), Source.Location);
			return false;
		}
		Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
		TemplateNode = Cast<URigVMTemplateNode>(Node);
		const FRigVMFunction* ResolvedFunction = TemplateNode
			? TemplateNode->GetResolvedFunction() : nullptr;
		if (TemplateNode && !ResolvedFunction)
		{
			FRigVMTemplate* MutableTemplate = const_cast<FRigVMTemplate*>(Template);
			if (!MutableTemplate->GetOrCreatePermutation(ExactPermutation))
			{
				AddError(Result, TEXT("Failed to materialize public template permutation for: ")
					+ Source.StableId, Source.Location);
				return false;
			}
			if (!Controller->FullyResolveTemplateNode(TemplateNode, ExactPermutation, false))
			{
				AddError(Result, TEXT("Failed to bind materialized public template permutation for: ")
					+ Source.StableId, Source.Location);
				return false;
			}
			Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
			TemplateNode = Cast<URigVMTemplateNode>(Node);
			ResolvedFunction = TemplateNode ? TemplateNode->GetResolvedFunction() : nullptr;
			if (!ResolvedFunction)
			{
				for (const TPair<FName, TRigVMTypeIndex>& SourceType : SourceTypes)
				{
					Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
					TemplateNode = Cast<URigVMTemplateNode>(Node);
					URigVMPin* Pin = TemplateNode
						? TemplateNode->FindPin(SourceType.Key.ToString()) : nullptr;
					if (!Pin || Pin->GetTypeIndex() == SourceType.Value) continue;
					if (!Controller->ResolveWildCardPin(Pin, SourceType.Value, false, false))
					{
						AddError(Result, TEXT("Failed public resolved template type-map restoration for '")
							+ Source.StableId + TEXT(".") + SourceType.Key.ToString() + TEXT("'"), Source.Location);
						return false;
					}
				}
			}
			Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
			TemplateNode = Cast<URigVMTemplateNode>(Node);
			ResolvedFunction = TemplateNode ? TemplateNode->GetResolvedFunction() : nullptr;
		}
		if (ExpectedFunction.IsEmpty() || !ResolvedFunction
			|| ResolvedFunction->Name != ExpectedFunction)
		{
			AddError(Result, FString::Printf(
				TEXT("Resolved template function mismatch for '%s': expected '%s', got '%s'"),
				*Source.StableId, *ExpectedFunction,
				ResolvedFunction ? *ResolvedFunction->Name : TEXT("<none>")), Source.Location);
			return false;
		}
		return true;
	}

	if (!Controller->UnresolveTemplateNodes(TArray<URigVMNode*>({TemplateNode}), false))
	{
		AddError(Result, TEXT("Failed to unresolve template node: ") + Source.StableId,
			Source.Location);
		return false;
	}
	Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
	TemplateNode = Cast<URigVMTemplateNode>(Node);
	if (!TemplateNode) return false;
	for (const FRigPinAST& PinAST : Source.Pins)
	{
		URigVMPin* Pin = TemplateNode->FindPin(PinAST.Path);
		if (!Pin || !Pin->IsWildCard() || PinAST.Type.CPPType == TEXT("FRigVMUnknownType")) continue;
		if (!Controller->ResolveWildCardPin(Pin, TypeIndexForPin(PinAST), false, false))
		{
			AddError(Result, TEXT("Failed public wildcard resolution for '")
				+ Source.StableId + TEXT(".") + PinAST.Path + TEXT("'"), PinAST.Location);
			return false;
		}
		Node = Controller->GetGraph()->FindNodeByName(FName(*Source.StableId));
		TemplateNode = Cast<URigVMTemplateNode>(Node);
		if (!TemplateNode) return false;
	}
	if (!ExpectedFunction.IsEmpty()
		&& (!TemplateNode->GetResolvedFunction()
			|| TemplateNode->GetResolvedFunction()->Name != ExpectedFunction))
	{
		AddError(Result, FString::Printf(
			TEXT("Public wildcard resolution selected wrong permutation for '%s': expected '%s'"),
			*Source.StableId, *ExpectedFunction), Source.Location);
		return false;
	}
	return true;
}
#else
bool ResolveTemplateNodeFromSource(
	URigVMController*,
	URigVMNode*&,
	const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	if (!Source.Properties.Contains(TEXT("template-notation"))
		&& Source.Kind != ERigNodeKind::Dispatch) return true;
	AddError(Result, TEXT("UE4.27 cannot restore UE5 RigVM template type maps for: ")
		+ Source.StableId, Source.Location);
	return false;
}
#endif

bool RestoreAndVerifyPins(
	URigVMController* Controller,
	URigVMNode* Node,
	const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	TFunction<bool(const FRigPinAST&)> Visit = [&](const FRigPinAST& PinAST)
	{
		if (PinAST.Direction == ERigPinDirection::Hidden
			&& PinAST.Type.CPPType == TEXT("FCachedRigElement"))
		{
			return true;
		}
		URigVMPin* Pin = Node->FindPin(PinAST.Path);
		if (!Pin)
		{
			AddError(Result, FString::Printf(TEXT("Rig node '%s' is missing pin '%s'"),
				*Source.StableId, *PinAST.Path), PinAST.Location);
			return false;
		}
		if (Pin->IsWildCard() && PinAST.Type.CPPType != TEXT("FRigVMUnknownType"))
		{
			FString CPPType = PinAST.Type.CPPType;
			if (PinAST.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
				CPPType = TEXT("TArray<") + CPPType + TEXT(">");
			if (!Controller->ResolveWildCardPin(
				Source.StableId + TEXT(".") + PinAST.Path, CPPType,
				FName(*PinAST.Type.CPPTypeObject), false, false))
			{
				AddError(Result, FString::Printf(TEXT("Failed to resolve wildcard pin '%s.%s'"),
					*Source.StableId, *PinAST.Path), PinAST.Location);
				return false;
			}
			Pin = Node->FindPin(PinAST.Path);
		}
		if (!Pin)
		{
			AddError(Result, FString::Printf(TEXT("Rig node '%s' lost pin '%s' after wildcard resolution"),
				*Source.StableId, *PinAST.Path), PinAST.Location);
			return false;
		}
		if (!Pin || Pin->GetCPPType() != PinAST.Type.CPPType
			|| Pin->GetDirection() != ToRigVMPinDirection(PinAST.Direction))
		{
			AddError(Result, FString::Printf(
				TEXT("Rig pin '%s.%s' type or direction mismatch in graph '%s': expected '%s'/%d, actual '%s'/%d"),
				*Source.StableId, *PinAST.Path,
				Node->GetGraph() ? *Node->GetGraph()->GetPathName() : TEXT("<null>"),
				*PinAST.Type.CPPType, static_cast<int32>(ToRigVMPinDirection(PinAST.Direction)),
				*Pin->GetCPPType(), static_cast<int32>(Pin->GetDirection())), PinAST.Location);
			return false;
		}
		if (Pin->IsArray())
		{
			const FString PinPath = Source.StableId + TEXT(".") + PinAST.Path;
			if (Pin->GetSubPins().Num() != PinAST.SubPins.Num()
				&& !Controller->SetArrayPinSize(PinPath, PinAST.SubPins.Num(), TEXT(""), false, false))
			{
				AddError(Result, FString::Printf(TEXT("Failed to materialize %d elements for array pin '%s'"),
					PinAST.SubPins.Num(), *PinPath), PinAST.Location);
				return false;
			}
			Pin = Node->FindPin(PinAST.Path);
			if (!Pin || Pin->GetSubPins().Num() != PinAST.SubPins.Num())
			{
				AddError(Result, FString::Printf(TEXT("Array pin '%s' element count mismatch"), *PinPath), PinAST.Location);
				return false;
			}
		}
		if (!PinAST.bExecuteContext
			&& (PinAST.Direction == ERigPinDirection::Input
				|| PinAST.Direction == ERigPinDirection::IO
				|| PinAST.Direction == ERigPinDirection::Visible)
			&& Pin->GetDefaultValue() != PinAST.DefaultValue
			&& (!Controller->SetPinDefaultValue(
				Source.StableId + TEXT(".") + PinAST.Path,
				PinAST.DefaultValue, true, false, false, false, false)
				|| !(Pin = Node->FindPin(PinAST.Path))
				|| Pin->GetDefaultValue() != PinAST.DefaultValue))
		{
			AddError(Result, FString::Printf(TEXT("Failed to restore default for pin '%s.%s'"),
				*Source.StableId, *PinAST.Path), PinAST.Location);
			return false;
		}
		for (const FRigPinAST& SubPin : PinAST.SubPins)
		{
			if (!Visit(SubPin)) return false;
		}
		return true;
	};
	for (const FRigPinAST& Pin : Source.Pins)
	{
		if (!Visit(Pin)) return false;
	}
	return true;
}

bool VerifyPinsReadOnly(
	URigVMNode* Node,
	const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	TFunction<bool(const FRigPinAST&)> Visit = [&](const FRigPinAST& PinAST)
	{
		if (PinAST.Direction == ERigPinDirection::Hidden
			&& PinAST.Type.CPPType == TEXT("FCachedRigElement")) return true;
		const URigVMPin* Pin = Node ? Node->FindPin(PinAST.Path) : nullptr;
		if (!Pin || Pin->GetCPPType() != PinAST.Type.CPPType
			|| Pin->GetDirection() != ToRigVMPinDirection(PinAST.Direction)
			|| (Pin->IsArray() && Pin->GetSubPins().Num() != PinAST.SubPins.Num()))
		{
			AddError(Result, FString::Printf(
				TEXT("Rig pin '%s.%s' changed after final template resolution"),
				*Source.StableId, *PinAST.Path), PinAST.Location);
			return false;
		}
		if (!PinAST.bExecuteContext
			&& (PinAST.Direction == ERigPinDirection::Input
				|| PinAST.Direction == ERigPinDirection::IO
				|| PinAST.Direction == ERigPinDirection::Visible)
			&& Pin->GetDefaultValue() != PinAST.DefaultValue)
		{
			AddError(Result, FString::Printf(
				TEXT("Rig pin '%s.%s' default changed after final template resolution"),
				*Source.StableId, *PinAST.Path), PinAST.Location);
			return false;
		}
		for (const FRigPinAST& SubPin : PinAST.SubPins)
			if (!Visit(SubPin)) return false;
		return true;
	};
	for (const FRigPinAST& Pin : Source.Pins)
		if (!Visit(Pin)) return false;
	return true;
}

bool RestoreAndVerifyLink(
	URigVMController* Controller,
	URigVMGraph* Graph,
	const FRigLinkAST& Link,
	FRigLangImportResult& Result)
{
	const FString SourcePath = Link.SourceNodeId + TEXT(".") + Link.SourcePinPath;
	const FString TargetPath = Link.TargetNodeId + TEXT(".") + Link.TargetPinPath;
	URigVMPin* SourcePin = Graph ? Graph->FindPin(SourcePath) : nullptr;
	URigVMPin* TargetPin = Graph ? Graph->FindPin(TargetPath) : nullptr;
	FString FailureReason;
	auto ContainsExactLink = [Graph, SourcePin, TargetPin]()
	{
		return Graph && Graph->GetLinks().ContainsByPredicate(
			[SourcePin, TargetPin](const URigVMLink* Candidate)
			{
				return Candidate && Candidate->GetSourcePin() == SourcePin
					&& Candidate->GetTargetPin() == TargetPin;
			});
	};
	if (!Controller || !SourcePin || !TargetPin
		|| (SourcePin->GetDirection() != ERigVMPinDirection::Output
			&& SourcePin->GetDirection() != ERigVMPinDirection::IO)
		|| (TargetPin->GetDirection() != ERigVMPinDirection::Input
			&& TargetPin->GetDirection() != ERigVMPinDirection::IO)
		|| (!ContainsExactLink() && !Controller->AddLink(
			SourcePin, TargetPin, false, ERigVMPinDirection::Invalid,
			false, true, &FailureReason))
		|| !ContainsExactLink())
	{
		AddError(Result, FString::Printf(TEXT("Failed to restore Rig link '%s -> %s'%s%s"),
			*SourcePath, *TargetPath,
			FailureReason.IsEmpty() ? TEXT("") : TEXT(": "), *FailureReason), Link.Location);
		return false;
	}
	return true;
}

FString GraphSemanticSnapshot(const FRigModuleAST& Value, const FRigModuleAST& SourceIdentity)
{
	FRigModuleAST Copy = Value;
	Copy.Header = FRigModuleHeaderAST();
	Copy.Hierarchy.Reset();
	Copy.Variables.Reset();
	const FGraphSemanticTokenIndex SourceGraphTokenIndex =
		BuildGraphSemanticTokenIndex(SourceIdentity);
	const FGraphSemanticTokenIndex ValueGraphTokenIndex =
		BuildGraphSemanticTokenIndex(Value);
	const TArray<FString> GraphTokenCollisions = ValueGraphTokenIndex.CollisionMarkers();
	if (GraphTokenCollisions.Num() != 0)
	{
		FString CollisionText = FString::Join(GraphTokenCollisions, TEXT(" | "));
		CollisionText.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		CollisionText.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Copy.Header.Properties.Add(TEXT("semantic-graph-token-collision"),
			TEXT("\"") + CollisionText + TEXT("\""));
	}
	auto NormalizeTypedPinDefault = [](FRigPinAST& RootPin)
	{
		TFunction<void(FRigPinAST&)> Visit = [&Visit](FRigPinAST& Pin)
		{
			Pin.DefaultValue = CanonicalTypedPinDefault(Pin);
			for (FRigPinAST& SubPin : Pin.SubPins) Visit(SubPin);
		};
		Visit(RootPin);
	};

	TSet<FString> SourceFallbackGraphTokens;
	TSet<FString> SourceNodeKeys;
	TSet<FString> SourceFallbackNodeKeys;
	TSet<FString> SourceLocalKeys;
	TMap<FString, FString> SourceVisibleLocalTokens;
	TSet<FString> SourceInterfaceKeys;
	TMap<FString, FString> SourceGeneratedVariableTokens;
	TSet<FString> SourceFunctionNames;
	TSet<FString> SourceEntryNames;
	TSet<FString> SourceExternalKeys;
	for (const FRigGraphAST& Graph : SourceIdentity.Graphs)
	{
		const FString Token = SourceGraphTokenIndex.ByStableId.FindRef(Graph.StableId);
		if (Graph.Nodes.Num() == 0 || !Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				FGuid EditorGuid;
				return !Node.bInjected && FGuid::Parse(Node.Guid, EditorGuid);
			}))
		{
			SourceFallbackGraphTokens.Add(Token);
		}
		for (const FRigGraphVariableAST& Variable : Graph.LocalVariables)
		{
			SourceLocalKeys.Add(Token + TEXT("|") + Variable.Name);
		}
		for (const FRigNodeAST& Node : Graph.Nodes)
		{
			const FString Key = Token + TEXT("|") + Node.StableId;
			SourceNodeKeys.Add(Key);
			if (Node.Guid.StartsWith(TEXT("model:"))) SourceFallbackNodeKeys.Add(Key);
			if (Node.Properties.Contains(TEXT("interface-pin-guids"))) SourceInterfaceKeys.Add(Key);
			if (Node.Kind == ERigNodeKind::Variable)
			{
				FGuid SourceVariableGuid;
				if (FGuid::Parse(UnquoteRigLangProperty(
					Node.Properties.FindRef(TEXT("variable-guid"))), SourceVariableGuid)
					&& !SourceVariableGuid.IsValid())
				{
					SourceGeneratedVariableTokens.Add(Key, Token + TEXT("|")
						+ UnquoteRigLangProperty(Node.Properties.FindRef(TEXT("variable-name"))));
				}
			}
		}
	}
	TMap<FString, const FRigGraphAST*> SourceGraphsById;
	for (const FRigGraphAST& Graph : SourceIdentity.Graphs)
		SourceGraphsById.Add(Graph.StableId, &Graph);
	for (const FRigGraphAST& Graph : SourceIdentity.Graphs)
	{
		const FRigGraphAST* Scope = &Graph;
		while (Scope && Scope->Role != TEXT("function"))
			Scope = SourceGraphsById.FindRef(Scope->ParentStableId);
		if (!Scope) continue;
		const FString GraphToken = SourceGraphTokenIndex.ByStableId.FindRef(Graph.StableId);
		const FString ScopeToken = SourceGraphTokenIndex.ByStableId.FindRef(Scope->StableId);
		for (const FRigGraphVariableAST& Variable : Scope->LocalVariables)
			SourceVisibleLocalTokens.Add(GraphToken + TEXT("|") + Variable.Name,
				ScopeToken + TEXT("|") + Variable.Name);
	}
	for (const FRigFunctionAST& Function : SourceIdentity.Functions)
	{
		SourceFunctionNames.Add(Function.Name);
		for (const FRigExternalVariableAST& Variable : Function.ExternalVariables)
			SourceExternalKeys.Add(Function.Name + TEXT("|") + Variable.Name);
	}
	for (const FRigEntryAST& Entry : SourceIdentity.Entries) SourceEntryNames.Add(Entry.Name);

	TMap<FString, FString> GraphIds;
	for (const FRigGraphAST& Graph : Copy.Graphs)
	{
		const FString Token = ValueGraphTokenIndex.ByStableId.FindRef(Graph.StableId);
		if (SourceGraphTokenIndex.IsUniqueToken(Token)
			&& ValueGraphTokenIndex.IsUniqueToken(Token))
		{
			GraphIds.Add(Graph.StableId, Token);
		}
	}
	auto RemapGraphId = [&GraphIds](FString& Value)
	{
		if (const FString* Remapped = GraphIds.Find(Value)) Value = *Remapped;
	};
	TMap<FString, FRigFunctionIdentifierAST> LocalFunctionTokens;
	for (const FRigFunctionAST& Function : Copy.Functions)
	{
		if (!SourceFunctionNames.Contains(Function.Name) || !Function.FunctionIdentifier.IsComplete()) continue;
		FRigFunctionIdentifierAST Token;
		Token.HostObject = TEXT("$local");
		Token.LibraryNodePath = Function.Name;
		LocalFunctionTokens.Add(Function.FunctionIdentifier.ToStableId(), Token);
	}
	auto RemapLocalFunction = [&LocalFunctionTokens](FRigFunctionIdentifierAST& Identifier)
	{
		if (const FRigFunctionIdentifierAST* Token = LocalFunctionTokens.Find(Identifier.ToStableId()))
			Identifier = *Token;
	};
	for (FRigGraphAST& Graph : Copy.Graphs)
	{
		const FString OriginalId = Graph.StableId;
		const FString OriginalToken = ValueGraphTokenIndex.ByStableId.FindRef(OriginalId);
		if (const FString* GraphToken = GraphIds.Find(OriginalId))
		{
			Graph.StableId = *GraphToken;
			if (SourceFallbackGraphTokens.Contains(*GraphToken))
				Graph.EditorGuid = TEXT("$fallback-graph-guid:") + *GraphToken;
		}
		RemapGraphId(Graph.ParentStableId);
		for (FRigGraphVariableAST& Variable : Graph.LocalVariables)
		{
			const FString Key = Graph.StableId + TEXT("|") + Variable.Name;
			if (SourceLocalKeys.Contains(Key)) Variable.Guid = TEXT("$generated-local-guid:") + Key;
		}
		for (FRigNodeAST& Node : Graph.Nodes)
		{
			for (FRigPinAST& Pin : Node.Pins) NormalizeTypedPinDefault(Pin);
			Node.Pins.RemoveAll([](const FRigPinAST& Pin)
			{
				return Pin.Direction == ERigPinDirection::Hidden
					&& Pin.Type.CPPType == TEXT("FCachedRigElement");
			});
			const FString Key = Graph.StableId + TEXT("|") + Node.StableId;
			if (SourceFallbackNodeKeys.Contains(Key))
				Node.Guid = TEXT("$fallback-node-guid:") + Key;
			RemapGraphId(Node.ContainedGraphStableId);
			if (SourceInterfaceKeys.Contains(Key)
				&& Node.Properties.Contains(TEXT("interface-pin-guids")))
				Node.Properties[TEXT("interface-pin-guids")] = TEXT("$generated-interface-pin-guids:") + Key;
			if (Node.Kind == ERigNodeKind::Call) RemapLocalFunction(Node.FunctionIdentifier);
			const bool bResolvedUnit = Node.Kind == ERigNodeKind::Unit
				&& Node.ClassPath == TEXT("/Script/RigVMDeveloper.RigVMUnitNode")
				&& !Node.MethodName.IsEmpty()
				&& !UnquoteRigLangProperty(Node.Properties.FindRef(TEXT("script-struct"))).IsEmpty()
				&& !UnquoteRigLangProperty(Node.Properties.FindRef(TEXT("resolved-function"))).IsEmpty()
				&& Node.Properties.FindRef(TEXT("template-resolved")) == TEXT("true")
				&& Node.Properties.FindRef(TEXT("template-types")) != TEXT("()");
			if (bResolvedUnit)
			{
				// AddUnitNode reconstructs the same resolved unit contract but may drop legacy template notation.
				Node.Properties.Add(TEXT("template-notation"), TEXT("\"$resolved-unit-notation\""));
			}
			if (Node.Kind == ERigNodeKind::Variable)
			{
				if (const FString* GeneratedKey = SourceGeneratedVariableTokens.Find(Key))
					Node.Properties.Add(TEXT("variable-guid"),
						FString(TEXT("\"$generated-input-variable-guid:")) + *GeneratedKey + TEXT("\""));
				const FString LocalLookupKey = OriginalToken + TEXT("|")
					+ UnquoteRigLangProperty(Node.Properties.FindRef(TEXT("variable-name")));
				if (const FString* LocalKey = SourceVisibleLocalTokens.Find(LocalLookupKey))
					Node.Properties.Add(TEXT("variable-guid"),
						FString(TEXT("\"$generated-local-guid:")) + *LocalKey + TEXT("\""));
			}
			if (SourceNodeKeys.Contains(Key) && Node.bInjected && !Node.InjectionOwnerPin.IsEmpty())
			{
				int32 Separator = INDEX_NONE;
				if (Node.InjectionOwnerPin.FindLastChar(TEXT('|'), Separator))
					Node.InjectionOwnerPin = Node.InjectionOwnerPin.Mid(Separator + 1);
			}
		}
		Graph.Nodes.Sort([](const FRigNodeAST& A, const FRigNodeAST& B)
		{
			return A.StableId < B.StableId;
		});
	}
	for (FRigFunctionAST& Function : Copy.Functions)
	{
		if (SourceFunctionNames.Contains(Function.Name))
		{
			Function.StableId = TEXT("$function:") + Function.Name;
			RemapLocalFunction(Function.FunctionIdentifier);
		}
		RemapGraphId(Function.GraphStableId);
		for (FRigFunctionDependencyAST& Dependency : Function.Dependencies)
		{
			FRigFunctionIdentifierAST Identifier;
			Identifier.HostObject = Dependency.HostObject;
			Identifier.LibraryNodePath = Dependency.LibraryNodePath;
			RemapLocalFunction(Identifier);
			Dependency.HostObject = Identifier.HostObject;
			Dependency.LibraryNodePath = Identifier.LibraryNodePath;
		}
		Function.Graph = FRigGraphAST();
	}
	for (FRigEntryAST& Entry : Copy.Entries)
	{
		if (SourceEntryNames.Contains(Entry.Name)) Entry.StableId = TEXT("$entry:") + Entry.Name;
		RemapGraphId(Entry.GraphStableId);
		Entry.Graph = FRigGraphAST();
	}
	Copy.Graphs.Sort([](const FRigGraphAST& A, const FRigGraphAST& B)
	{
		return A.StableId < B.StableId;
	});
	Copy.Functions.Sort([](const FRigFunctionAST& A, const FRigFunctionAST& B)
	{
		return A.Name < B.Name;
	});
	Copy.Entries.Sort([](const FRigEntryAST& A, const FRigEntryAST& B)
	{
		return A.Name < B.Name;
	});
	return Copy.ToCanonicalHashInput();
}
}

#if WITH_DEV_AUTOMATION_TESTS
bool FRigLangImporter::ResolveTemplateNodeForTest(
	URigVMController* Controller,
	URigVMNode*& Node,
	const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	return ResolveTemplateNodeFromSource(Controller, Node, Source, Result);
}
#endif

FString FRigLangImporter::BuildGraphSemanticSnapshot(
	const FRigModuleAST& Value, const FRigModuleAST& SourceIdentity)
{
	return GraphSemanticSnapshot(Value, SourceIdentity);
}

TMap<FString, FString> FRigLangImporter::BuildGraphSemanticTokens(
	const FRigModuleAST& Value, TSet<FString>* OutCollidingTokens)
{
	const FGraphSemanticTokenIndex Index = BuildGraphSemanticTokenIndex(Value);
	if (OutCollidingTokens) *OutCollidingTokens = Index.CollisionTokens();
	return Index.ByStableId;
}

FString FRigLangImporter::BuildCanonicalPinDefault(const FRigPinAST& Pin)
{
	return CanonicalTypedPinDefault(Pin);
}

FString FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
	const FRigModuleAST& Value, const FRigModuleAST& SourceIdentity)
{
	TSet<FString> GeneratedIdentityVariables;
	for (const FRigVariableAST& Variable : SourceIdentity.Variables)
	{
		FGuid SourceGuid;
		if (!FGuid::Parse(Variable.StableId, SourceGuid))
		{
			GeneratedIdentityVariables.Add(Variable.Name);
		}
	}
	return HierarchyVariableSemanticSnapshot(Value, GeneratedIdentityVariables);
}

FRigLangImportResult FRigLangImporter::Import(
	const FRigModuleAST& Module,
	const FRigLangImportOptions& Options)
{
	FRigLangImportResult Result;
	TArray<int32> Order;
	if (Options.TargetPackage.IsEmpty())
	{
		AddError(Result, TEXT("Rig staging import requires a target package"));
		return Result;
	}
	if (!Options.bTransient && !FPackageName::IsValidLongPackageName(Options.TargetPackage, true))
	{
		AddError(Result, TEXT("Rig staging target is not a valid package name"));
		return Result;
	}
	if (!Options.bTransient && FindPackage(nullptr, *Options.TargetPackage))
	{
		AddError(Result, TEXT("Rig staging target package already exists"));
		return Result;
	}
	if (!Preflight(Module, Result, Order)) return Result;

	UPackage* Package = Options.bTransient ? GetTransientPackage() : CreatePackage(*Options.TargetPackage);
	auto AbortImport = [&]() -> FRigLangImportResult
	{
		UControlRigBlueprint* FailedBlueprint = Result.Blueprint.Get();
		Result.Blueprint = nullptr;
		if (FailedBlueprint && Options.bTransient)
		{
			FailedBlueprint->ClearFlags(RF_Public | RF_Standalone);
			FailedBlueprint->MarkAsGarbage();
		}
		if (!Options.bTransient && Package)
		{
			const FString DiscardedPackageName = FString::Printf(
				TEXT("/Engine/Transient/DiscardedRigLangImport_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Package->Rename(
				*DiscardedPackageName, nullptr,
				REN_DontCreateRedirectors | REN_NonTransactional);
			ForEachObjectWithPackage(Package, [](UObject* Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
				return true;
			});
			Package->SetDirtyFlag(false);
			Package->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
		return Result;
	};
	if (!Package)
	{
		AddError(Result, TEXT("Failed to create Rig staging package"));
		return AbortImport();
	}
	FString AssetName = FPackageName::GetLongPackageAssetName(Options.TargetPackage);
	if (AssetName.IsEmpty()) AssetName = TEXT("CR_RigLangImport");
	const FName BlueprintName = Options.bTransient
		? MakeUniqueObjectName(Package, UControlRigBlueprint::StaticClass(), *AssetName)
		: FName(*AssetName);
#if ENGINE_MAJOR_VERSION >= 5
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	Result.Blueprint = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), Package, BlueprintName,
		Options.bTransient ? RF_Transient : RF_Public | RF_Standalone,
		nullptr, GWarn));
#else
	Result.Blueprint = NewObject<UControlRigBlueprint>(Package, BlueprintName,
		Options.bTransient ? RF_Transient : RF_Public | RF_Standalone);
#endif
	if (!Result.Blueprint)
	{
		AddError(Result, TEXT("Failed to create Rig staging blueprint"));
		return AbortImport();
	}

	URigHierarchyController* Controller = Result.Blueprint->GetHierarchyController();
	URigHierarchy* Hierarchy = Result.Blueprint->GetHierarchy();
	TMap<FString, FRigElementKey> Keys;
	TMap<FString, FRigElementKey> KeysByStableId;
	for (const int32 Index : Order)
	{
		const FRigHierarchyElementAST& Element = Module.Hierarchy[Index];
		const bool bDeferTypedParents = Element.Kind != ERigHierarchyElementKind::Bone
			&& Element.Parents.Num() != 0;
		const FRigElementKey Parent = bDeferTypedParents || Element.ParentName.IsEmpty()
			? FRigElementKey() : Keys.FindChecked(Element.ParentName);
		const FRigHierarchyTransformAST* InitialLocal = FindTransform(Element, ERigHierarchyTransformRole::InitialLocal);
		const FTransform InitialTransform = InitialLocal ? InitialLocal->Value : FTransform::Identity;
		FRigElementKey Added;
		switch (Element.Kind)
		{
		case ERigHierarchyElementKind::Bone:
		{
			ERigBoneType BoneType = ERigBoneType::User;
			if (const FRigHierarchyStateAST* State = FindState(Element, ERigHierarchyStateKind::BoneType))
				BoneType = static_cast<ERigBoneType>(StaticEnum<ERigBoneType>()->GetValueByNameString(State->Type));
			Added = Controller->AddBone(*Element.Name, Parent, InitialTransform, false, BoneType, false);
			break;
		}
		case ERigHierarchyElementKind::Null:
			Added = Controller->AddNull(*Element.Name, Parent, InitialTransform, false, false);
			break;
		case ERigHierarchyElementKind::Control:
		{
			FRigControlSettings Settings;
			const FRigHierarchyStateAST* SettingsState = FindState(Element, ERigHierarchyStateKind::ControlSettings);
			if (!SettingsState || !FRigControlSettings::StaticStruct()->ImportText(
				*SettingsState->SerializedValue, &Settings, nullptr, PPF_None, nullptr, TEXT("FRigControlSettings")))
			{
				AddError(Result, FString::Printf(TEXT("Control '%s' has invalid settings"), *Element.Name), Element.Location);
				return AbortImport();
			}
			FRigControlValue InitialValue;
			if (!ControlValueFromState(
				FindState(Element, ERigHierarchyStateKind::ControlValue, TEXT("initial")), Settings.ControlType, InitialValue))
			{
				AddError(Result, FString::Printf(TEXT("Control '%s' has invalid initial value"), *Element.Name), Element.Location);
				return AbortImport();
			}
			const FRigHierarchyTransformAST* Offset = FindTransform(Element, ERigHierarchyTransformRole::OffsetInitialLocal);
			const FRigHierarchyTransformAST* Shape = FindTransform(Element, ERigHierarchyTransformRole::ShapeInitialLocal);
			Added = Controller->AddControl(*Element.Name, Parent, Settings, InitialValue,
				Offset ? Offset->Value : FTransform::Identity,
				Shape ? Shape->Value : FTransform::Identity, false);
			break;
		}
		case ERigHierarchyElementKind::Curve:
		{
			const FRigHierarchyStateAST* State = FindState(Element, ERigHierarchyStateKind::Curve);
			Added = Controller->AddCurve(*Element.Name, State ? static_cast<float>(State->NumberValue) : 0.0f, false);
			break;
		}
		}
		const FRigElementKey Expected(*Element.Name, ElementType(Element.Kind));
		if (Added != Expected || !Hierarchy->Contains(Added))
		{
			AddError(Result, FString::Printf(TEXT("Hierarchy key creation mismatch for '%s'"), *Element.Name), Element.Location);
			return AbortImport();
		}
		Keys.Add(Element.Name, Added);
		KeysByStableId.Add(EffectiveStableId(Element), Added);
	}

	for (const FRigHierarchyElementAST& Element : Module.Hierarchy)
	{
		if (Element.Kind == ERigHierarchyElementKind::Bone || Element.Parents.Num() == 0) continue;
		const FRigElementKey Child = Keys.FindChecked(Element.Name);
		for (const FRigHierarchyParentAST& Parent : Element.Parents)
		{
			const FRigElementKey ParentKey = KeysByStableId.FindChecked(Parent.StableId);
			Controller->AddParent(
				Child, ParentKey, static_cast<float>(Parent.InitialWeight.Location),
				false, FName(*Parent.Label), false);
			if (!Hierarchy->GetParents(Child, false).Contains(ParentKey))
			{
				AddError(Result, FString::Printf(
					TEXT("Failed to create typed parent '%s' on '%s'"),
					*Parent.StableId, *Element.Name), Parent.Location);
				return AbortImport();
			}
			Hierarchy->SetParentWeight(Child, ParentKey, FRigElementWeight(
				static_cast<float>(Parent.InitialWeight.Location),
				static_cast<float>(Parent.InitialWeight.Rotation),
				static_cast<float>(Parent.InitialWeight.Scale)), true, false);
			Hierarchy->SetParentWeight(Child, ParentKey, FRigElementWeight(
				static_cast<float>(Parent.CurrentWeight.Location),
				static_cast<float>(Parent.CurrentWeight.Rotation),
				static_cast<float>(Parent.CurrentWeight.Scale)), false, false);
		}
	}

	for (const FRigHierarchyElementAST& Element : Module.Hierarchy)
	{
		const FRigElementKey Key = Keys.FindChecked(Element.Name);
		if (const FRigHierarchyTransformAST* Transform = FindTransform(Element, ERigHierarchyTransformRole::InitialLocal))
			Hierarchy->SetLocalTransform(Key, Transform->Value, true);
		if (const FRigHierarchyTransformAST* Transform = FindTransform(Element, ERigHierarchyTransformRole::CurrentLocal))
			Hierarchy->SetLocalTransform(Key, Transform->Value, false);
		if (Element.Kind == ERigHierarchyElementKind::Control)
		{
			FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key);
			for (const FRigHierarchyStateAST& State : Element.States)
			{
				if (State.Kind == ERigHierarchyStateKind::ControlValue)
				{
					ERigControlValueType ValueType = ERigControlValueType::Current;
					if (State.Role == TEXT("initial")) ValueType = ERigControlValueType::Initial;
					else if (State.Role == TEXT("minimum")) ValueType = ERigControlValueType::Minimum;
					else if (State.Role == TEXT("maximum")) ValueType = ERigControlValueType::Maximum;
					FRigControlValue Value;
					if (!ControlValueFromState(&State, Control->Settings.ControlType, Value))
					{
						AddError(Result, FString::Printf(TEXT("Control '%s' has invalid '%s' value"),
							*Element.Name, *State.Role), State.Location);
						return AbortImport();
					}
					Hierarchy->SetControlValue(Control, Value, ValueType, false, true);
				}
				else if (State.Kind == ERigHierarchyStateKind::PreferredEuler && State.Components.Num() == 3)
				{
					Control->PreferredEulerAngles.RotationOrder = static_cast<EEulerRotationOrder>(
						StaticEnum<EEulerRotationOrder>()->GetValueByNameString(State.Type));
					FVector& Value = State.Role == TEXT("initial")
						? Control->PreferredEulerAngles.Initial : Control->PreferredEulerAngles.Current;
					Value = FVector(State.Components[0], State.Components[1], State.Components[2]);
				}
			}
			if (const FRigHierarchyTransformAST* Transform = FindTransform(Element, ERigHierarchyTransformRole::OffsetInitialLocal))
				Hierarchy->SetControlOffsetTransform(Key, Transform->Value, true);
			if (const FRigHierarchyTransformAST* Transform = FindTransform(Element, ERigHierarchyTransformRole::OffsetCurrentLocal))
				Hierarchy->SetControlOffsetTransform(Key, Transform->Value, false);
			if (const FRigHierarchyTransformAST* Transform = FindTransform(Element, ERigHierarchyTransformRole::ShapeInitialLocal))
				Hierarchy->SetControlShapeTransform(Key, Transform->Value, true);
			if (const FRigHierarchyTransformAST* Transform = FindTransform(Element, ERigHierarchyTransformRole::ShapeCurrentLocal))
				Hierarchy->SetControlShapeTransform(Key, Transform->Value, false);
			FRigControlSettings ExactSettings;
			const FRigHierarchyStateAST* ExactSettingsState = FindState(Element, ERigHierarchyStateKind::ControlSettings);
			if (!ExactSettingsState || !FRigControlSettings::StaticStruct()->ImportText(
				*ExactSettingsState->SerializedValue, &ExactSettings, nullptr, PPF_None, nullptr, TEXT("FRigControlSettings"))
				|| !Controller->SetControlSettings(Key, ExactSettings, false))
			{
				AddError(Result, FString::Printf(TEXT("Failed to restore exact settings for control '%s'"),
					*Element.Name), Element.Location);
				return AbortImport();
			}
		}
		if (Element.Kind == ERigHierarchyElementKind::Curve)
		{
			if (const FRigHierarchyStateAST* State = FindState(Element, ERigHierarchyStateKind::Curve))
			{
				FRigCurveElement* Curve = Hierarchy->Find<FRigCurveElement>(Key);
				if (State->bBoolValue)
					Hierarchy->SetCurveValue(Curve, static_cast<float>(State->NumberValue), false, true);
				else
					Hierarchy->UnsetCurveValue(Curve, false, true);
			}
		}
		for (const FRigHierarchyMetadataAST& Metadata : Element.Metadata)
		{
			if (!RestoreMetadata(Hierarchy, Key, Metadata))
			{
				AddError(Result, FString::Printf(
					TEXT("Failed to restore metadata '%s' on '%s'"), *Metadata.Name, *Element.Name), Metadata.Location);
				return AbortImport();
			}
		}
	}

	for (const FRigVariableAST& Variable : Module.Variables)
	{
		FString CPPType = Variable.Type.CPPType;
		if (Variable.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
			CPPType = TEXT("TArray<") + CPPType + TEXT(">");
		UObject* TypeObject = Variable.Type.CPPTypeObject.IsEmpty()
			? nullptr : LoadObject<UObject>(nullptr, *Variable.Type.CPPTypeObject);
		FGuid Guid;
		if (!FGuid::Parse(Variable.StableId, Guid)) Guid = FGuid::NewGuid();
		const FRigVMExternalVariable External = FRigVMExternalVariable::Make(
			Guid, FName(*Variable.Name), CPPType, TypeObject,
			Variable.Access == ERigVariableAccess::PublicInput, false);
		const FString MemberDefault = BlueprintMemberDefault(Variable);
		const FName Added = static_cast<IRigVMEditorAssetInterface*>(Result.Blueprint.Get())
			->AddHostMemberVariableFromExternal(External, MemberDefault);
		if (Added != FName(*Variable.Name))
		{
			AddError(Result, FString::Printf(TEXT("Failed to create exact Rig variable '%s'"), *Variable.Name), Variable.Location);
			return AbortImport();
		}
	}

	const TArray<FRigVMGraphVariableDescription> ReflectedVariables = Result.Blueprint->GetMemberVariables();
	if (ReflectedVariables.Num() != Module.Variables.Num())
	{
		AddError(Result, TEXT("Rig variable reflection count mismatch"));
		return AbortImport();
	}
	for (const FRigVariableAST& Variable : Module.Variables)
	{
		const FRigVMGraphVariableDescription* Reflected = ReflectedVariables.FindByPredicate(
			[&Variable](const FRigVMGraphVariableDescription& Candidate) { return Candidate.Name == FName(*Variable.Name); });
		FGuid ExpectedGuid;
		const bool bHasExpectedGuid = FGuid::Parse(Variable.StableId, ExpectedGuid);
		FString ReflectedTypeObject = Reflected
			? (Reflected->CPPTypeObject ? Reflected->CPPTypeObject->GetPathName() : Reflected->CPPTypeObjectPath.ToString())
			: FString();
		if (ReflectedTypeObject.Equals(TEXT("None"), ESearchCase::IgnoreCase)) ReflectedTypeObject.Reset();
		TArray<FString> Mismatches;
		if (!Reflected) Mismatches.Add(TEXT("missing"));
		else
		{
			if (bHasExpectedGuid && Reflected->Guid != ExpectedGuid) Mismatches.Add(TEXT("Guid"));
			if (Reflected->CPPType != Variable.Type.CPPType) Mismatches.Add(TEXT("CPPType"));
			if (ReflectedTypeObject != Variable.Type.CPPTypeObject) Mismatches.Add(TEXT("CPPTypeObject"));
			if (Reflected->ToExternalVariable().IsArray() != (Variable.Type.ContainerType == TEXT("array"))) Mismatches.Add(TEXT("container"));
			if (Reflected->bPublic != (Variable.Access == ERigVariableAccess::PublicInput)) Mismatches.Add(TEXT("access"));
			if (Reflected->DefaultValue != BlueprintMemberDefault(Variable)) Mismatches.Add(TEXT("default"));
		}
		if (Mismatches.Num() != 0)
		{
			AddError(Result, FString::Printf(TEXT("Rig variable '%s' reflection mismatch: %s"),
				*Variable.Name, *FString::Join(Mismatches, TEXT(", "))), Variable.Location);
			return AbortImport();
		}
	}

	const bool bHasGraphInventory = Module.Graphs.Num() != 0;
	const bool bHasExecutableGraphContent = Module.Functions.Num() != 0 || Module.Entries.Num() != 0
		|| Module.Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.Num() != 0 || Graph.Links.Num() != 0 || Graph.LocalVariables.Num() != 0;
		});
	if (bHasGraphInventory)
	{
		FKismetEditorUtilities::CompileBlueprint(Result.Blueprint.Get());
		if (Result.Blueprint->Status == BS_Error)
		{
			AddError(Result, TEXT("Rig host variable compilation failed before graph reconstruction"));
			return AbortImport();
		}
		FRigVMClient* Client = Result.Blueprint->URigVMBlueprint::GetRigVMClient();
		URigVMFunctionLibrary* FunctionLibrary = Result.Blueprint->GetLocalFunctionLibrary();
		URigVMController* LibraryController = Result.Blueprint->GetOrCreateController(FunctionLibrary);
		if (!Client || !FunctionLibrary || !LibraryController)
		{
			AddError(Result, TEXT("Failed to initialize RigVM graph controllers"));
			return AbortImport();
		}

		TMap<FString, URigVMGraph*> GraphsByStableId;
		TMap<FString, URigVMLibraryNode*> FunctionsByIdentifier;
		TMap<FString, TMap<FGuid, FGuid>> LocalGuidRemapByGraph;
		for (const FRigGraphAST& Graph : Module.Graphs)
		{
			if (Graph.Role == TEXT("function-library"))
			{
				GraphsByStableId.Add(Graph.StableId, FunctionLibrary);
			}
		}

		for (const FRigFunctionAST& Function : Module.Functions)
		{
			FString FunctionName = Function.Name;
			if (const FString* ShortName = Function.Properties.Find(TEXT("short-name")))
				FunctionName = UnquoteRigLangProperty(*ShortName);
			const bool bMutable = Function.Arguments.ContainsByPredicate(
				[](const FRigCallableArgumentAST& Argument) { return Argument.bExecuteContext; });
			URigVMLibraryNode* LibraryNode = LibraryController->AddFunctionToLibrary(
				FName(*FunctionName), bMutable, FVector2D::ZeroVector, false, false);
			if (!LibraryNode || !LibraryNode->GetContainedGraph())
			{
				AddError(Result, FString::Printf(TEXT("Failed to predeclare Rig function '%s'"), *Function.Name), Function.Location);
				return AbortImport();
			}
			Result.Blueprint->MarkFunctionPublic(FName(*FunctionName), Function.Visibility == TEXT("public"));
			GraphsByStableId.Add(Function.GraphStableId, LibraryNode->GetContainedGraph());
			if (!Function.StableId.IsEmpty()) FunctionsByIdentifier.Add(Function.StableId, LibraryNode);
			if (Function.FunctionIdentifier.IsComplete())
				FunctionsByIdentifier.Add(Function.FunctionIdentifier.ToStableId(), LibraryNode);

			URigVMController* FunctionController = Result.Blueprint->GetOrCreateController(LibraryNode->GetContainedGraph());
			for (const FRigCallableArgumentAST& Argument : Function.Arguments)
			{
				if (Argument.bExecuteContext) continue;
				FString CPPType = Argument.Type.CPPType;
				if (Argument.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
					CPPType = TEXT("TArray<") + CPPType + TEXT(">");
				const FName AddedPin = FunctionController->AddExposedPin(
					FName(*Argument.Name), ToRigVMPinDirection(Argument.Direction), CPPType,
					FName(*Argument.Type.CPPTypeObject), Argument.DefaultValue, false, false, Argument.bInputVariable);
				if (AddedPin.IsNone())
				{
					AddError(Result, FString::Printf(TEXT("Failed to add argument '%s' to function '%s'"),
						*Argument.Name, *Function.Name), Argument.Location);
					return AbortImport();
				}
			}
			const FRigGraphAST* FunctionGraphAST = Module.Graphs.FindByPredicate(
				[&Function](const FRigGraphAST& Graph)
				{
					return Graph.StableId == Function.GraphStableId && Graph.Role == TEXT("function");
				});
			if (!FunctionGraphAST)
			{
				AddError(Result, FString::Printf(TEXT("Missing graph for function '%s'"),
					*Function.Name), Function.Location);
				return AbortImport();
			}
			for (const FRigGraphVariableAST& Variable : FunctionGraphAST->LocalVariables)
			{
				FString CPPType = Variable.Type.CPPType;
				if (Variable.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
					CPPType = TEXT("TArray<") + CPPType + TEXT(">");
				UObject* TypeObject = Variable.Type.CPPTypeObject.IsEmpty()
					? nullptr : LoadObject<UObject>(nullptr, *Variable.Type.CPPTypeObject);
				const FRigVMGraphVariableDescription Added = FunctionController->AddLocalVariable(
					FName(*Variable.Name), CPPType, TypeObject, Variable.DefaultValue, false, false);
				FGuid SourceGuid;
				if (Added.Name != FName(*Variable.Name) || !Added.Guid.IsValid()
					|| !FGuid::Parse(Variable.Guid, SourceGuid))
				{
					AddError(Result, FString::Printf(TEXT("Failed exact local variable '%s'"),
						*Variable.Name), Variable.Location);
					return AbortImport();
				}
				LocalGuidRemapByGraph.FindOrAdd(FunctionGraphAST->StableId).Add(SourceGuid, Added.Guid);
			}
			const FRigGraphAST* LibraryGraphAST = Module.Graphs.FindByPredicate(
				[](const FRigGraphAST& Graph) { return Graph.Role == TEXT("function-library"); });
			const FRigNodeAST* DeclarationAST = LibraryGraphAST
				? LibraryGraphAST->Nodes.FindByPredicate([&Function](const FRigNodeAST& Candidate)
				{
					return Candidate.Kind == ERigNodeKind::Collapse
						&& Candidate.ContainedGraphStableId == Function.GraphStableId;
				}) : nullptr;
			if (!DeclarationAST
				|| !RestoreAndVerifyPins(LibraryController, LibraryNode, *DeclarationAST, Result))
			{
				AddError(Result, FString::Printf(TEXT("Failed exact function-library declaration '%s'"),
					*Function.Name), Function.Location);
				return AbortImport();
			}
		}

		URigVMGraph* DefaultGraph = Client->GetDefaultModel();
		for (const FRigGraphAST& Graph : Module.Graphs)
		{
			if (Graph.Role != TEXT("root")) continue;
			const FString GraphName = GraphNameFromAST(Graph);
			URigVMGraph* TargetGraph = nullptr;
			if (DefaultGraph && DefaultGraph->GetGraphName() == GraphName)
				TargetGraph = DefaultGraph;
			else
				TargetGraph = Client->AddModel(FName(*GraphName), false);
			if (!TargetGraph)
			{
				AddError(Result, FString::Printf(TEXT("Failed to create Rig graph '%s'"), *GraphName), Graph.Location);
				return AbortImport();
			}
			if (TargetGraph->GetGraphName() != GraphName)
			{
				AddError(Result, FString::Printf(TEXT("Rig graph name mismatch: expected '%s', got '%s'"),
					*GraphName, *TargetGraph->GetGraphName()), Graph.Location);
				return AbortImport();
			}
			GraphsByStableId.Add(Graph.StableId, TargetGraph);
		}

		TFunction<bool(const FRigGraphAST&, URigVMGraph*, URigVMController*, const FString&, bool, TArray<FName>*)> BuildGraphRecursive;
		TFunction<URigVMNode*(const FRigNodeAST&, const FRigGraphAST&, URigVMGraph*, URigVMController*, const FString&)> CreateTypedNode;
		TFunction<void(const FRigGraphAST&, URigVMGraph*)> RebindContainedGraphs;

		RebindContainedGraphs = [&Module, &GraphsByStableId, &RebindContainedGraphs](
			const FRigGraphAST& GraphAST, URigVMGraph* ActualGraph)
		{
			if (!ActualGraph) return;
			GraphsByStableId.Add(GraphAST.StableId, ActualGraph);
			for (const FRigNodeAST& NodeAST : GraphAST.Nodes)
			{
				if (NodeAST.ContainedGraphStableId.IsEmpty()) continue;
				URigVMCollapseNode* CollapseNode = Cast<URigVMCollapseNode>(
					ActualGraph->FindNodeByName(FName(*NodeAST.StableId)));
				if (!CollapseNode || !CollapseNode->GetContainedGraph()) continue;
				GraphsByStableId.Add(NodeAST.ContainedGraphStableId, CollapseNode->GetContainedGraph());
				if (const FRigGraphAST* ChildAST = Module.Graphs.FindByPredicate(
					[&NodeAST](const FRigGraphAST& Candidate)
					{
						return Candidate.StableId == NodeAST.ContainedGraphStableId;
					}))
				{
					RebindContainedGraphs(*ChildAST, CollapseNode->GetContainedGraph());
				}
			}
		};

		CreateTypedNode = [&Module, &Result, &FunctionsByIdentifier, &GraphsByStableId,
			&LocalGuidRemapByGraph, &BuildGraphRecursive, &RebindContainedGraphs](
				const FRigNodeAST& NodeAST, const FRigGraphAST& OwnerAST,
				URigVMGraph* TargetGraph, URigVMController* Controller,
				const FString& OwningLocalScopeStableId) -> URigVMNode*
		{
			URigVMNode* Node = nullptr;
			switch (NodeAST.Kind)
			{
			case ERigNodeKind::Entry:
			case ERigNodeKind::Return:
				Node = TargetGraph->FindNodeByName(FName(*NodeAST.StableId));
				break;
			case ERigNodeKind::Unit:
			{
				const FString StructPath = UnquoteRigLangProperty(
					NodeAST.Properties.FindRef(TEXT("script-struct")));
				UScriptStruct* ScriptStruct = StructPath.IsEmpty()
					? nullptr : LoadObject<UScriptStruct>(nullptr, *StructPath);
				if (StructPath.Contains(TEXT("RigVMFunction_ControlFlowBranch")))
					Node = Controller->AddBranchNode(FVector2D::ZeroVector, NodeAST.StableId, false, false);
				else if (ScriptStruct)
					Node = Controller->AddUnitNode(ScriptStruct, FName(*NodeAST.MethodName),
						FVector2D::ZeroVector, NodeAST.StableId, false, false);
				else
				{
					const FString Notation = UnquoteRigLangProperty(
						NodeAST.Properties.FindRef(TEXT("template-notation")));
					if (!Notation.IsEmpty()) Node = Controller->AddTemplateNode(
						FName(*Notation), FVector2D::ZeroVector,
						NodeAST.StableId, false, false);
				}
				break;
			}
			case ERigNodeKind::Dispatch:
			{
				const FString DispatchStruct = UnquoteRigLangProperty(
					NodeAST.Properties.FindRef(TEXT("dispatch-script-struct")));
				const FRigPinAST* ResolvedPin = NodeAST.Pins.FindByPredicate(
					[](const FRigPinAST& Pin) { return Pin.Path == TEXT("Result"); });
				if (ResolvedPin && DispatchStruct.Contains(TEXT("RigVMDispatch_If")))
					Node = Controller->AddIfNode(ResolvedPin->Type.CPPType,
						FName(*ResolvedPin->Type.CPPTypeObject), FVector2D::ZeroVector,
						NodeAST.StableId, false, false);
				else if (ResolvedPin && DispatchStruct.Contains(TEXT("RigVMDispatch_Select")))
					Node = Controller->AddSelectNode(ResolvedPin->Type.CPPType,
						FName(*ResolvedPin->Type.CPPTypeObject), FVector2D::ZeroVector,
						NodeAST.StableId, false, false);
				else if (ResolvedPin && DispatchStruct.Contains(TEXT("RigVMDispatch_MakeStruct")))
					Node = Controller->AddMakeStructNode(ResolvedPin->Type.CPPType,
						FName(*ResolvedPin->Type.CPPTypeObject), ResolvedPin->DefaultValue,
						FVector2D::ZeroVector, NodeAST.StableId, false);
				else
				{
					const FString Notation = UnquoteRigLangProperty(
						NodeAST.Properties.FindRef(TEXT("template-notation")));
					if (!Notation.IsEmpty()) Node = Controller->AddTemplateNode(
						FName(*Notation), FVector2D::ZeroVector, NodeAST.StableId, false, false);
				}
				break;
			}
			case ERigNodeKind::Collapse:
			{
				const FRigGraphAST* ContainedAST = Module.Graphs.FindByPredicate(
					[&NodeAST](const FRigGraphAST& Candidate)
					{
						return Candidate.StableId == NodeAST.ContainedGraphStableId
							&& Candidate.Role == TEXT("node-contained");
					});
				if (!ContainedAST)
				{
					AddError(Result, FString::Printf(TEXT("Collapse '%s' has no contained graph '%s'"),
						*NodeAST.StableId, *NodeAST.ContainedGraphStableId), NodeAST.Location);
					return nullptr;
				}
				TArray<FName> InnerNames;
				if (!BuildGraphRecursive(*ContainedAST, TargetGraph, Controller,
					OwningLocalScopeStableId, true, &InnerNames))
					return nullptr;
				URigVMCollapseNode* CollapseNode = InnerNames.Num() == 0 ? nullptr
					: Controller->CollapseNodes(InnerNames, NodeAST.StableId, false, false, false);
				if (!CollapseNode || !CollapseNode->GetContainedGraph()) return nullptr;
				GraphsByStableId.Add(NodeAST.ContainedGraphStableId, CollapseNode->GetContainedGraph());
				URigVMController* ContainedController = Result.Blueprint->GetOrCreateController(
					CollapseNode->GetContainedGraph());
				for (const FRigPinAST& PinAST : NodeAST.Pins)
				{
					if (CollapseNode->FindPin(PinAST.Path)) continue;
					FString CPPType = PinAST.Type.CPPType;
					if (PinAST.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
						CPPType = TEXT("TArray<") + CPPType + TEXT(">");
					if (!ContainedController || ContainedController->AddExposedPin(
						FName(*PinAST.Path), ToRigVMPinDirection(PinAST.Direction), CPPType,
						FName(*PinAST.Type.CPPTypeObject), PinAST.DefaultValue, false, false).IsNone())
						return nullptr;
				}
				for (const FRigLinkAST& Link : ContainedAST->Links)
					if (!RestoreAndVerifyLink(ContainedController,
						CollapseNode->GetContainedGraph(), Link, Result)) return nullptr;
				RebindContainedGraphs(*ContainedAST, CollapseNode->GetContainedGraph());
				Node = CollapseNode;
				break;
			}
			case ERigNodeKind::Aggregate:
			{
				const FRigGraphAST* ContainedAST = Module.Graphs.FindByPredicate(
					[&NodeAST](const FRigGraphAST& Candidate)
					{
						return Candidate.StableId == NodeAST.ContainedGraphStableId;
					});
				FString FirstInnerName = UnquoteRigLangProperty(
					NodeAST.Properties.FindRef(TEXT("first-inner-node")));
				int32 Separator = INDEX_NONE;
				if (FirstInnerName.FindLastChar(TEXT('|'), Separator))
					FirstInnerName = FirstInnerName.Mid(Separator + 1);
				const FRigNodeAST* FirstInner = ContainedAST ? ContainedAST->Nodes.FindByPredicate(
					[&FirstInnerName](const FRigNodeAST& Candidate)
					{
						return Candidate.StableId == FirstInnerName && Candidate.Kind == ERigNodeKind::Unit;
					}) : nullptr;
				UScriptStruct* ScriptStruct = FirstInner ? LoadObject<UScriptStruct>(nullptr,
					*UnquoteRigLangProperty(FirstInner->Properties.FindRef(TEXT("script-struct")))) : nullptr;
				URigVMUnitNode* Seed = ScriptStruct ? Controller->AddUnitNode(
					ScriptStruct, FName(*FirstInner->MethodName), FVector2D::ZeroVector,
					NodeAST.StableId, false, false) : nullptr;
				if (Seed)
				{
					const bool bInputAggregate = NodeAST.Properties.FindRef(TEXT("input-aggregate")) == TEXT("true");
					TArray<const FRigPinAST*> AggregatePins;
					for (const FRigPinAST& Pin : NodeAST.Pins)
						if ((bInputAggregate && Pin.Direction == ERigPinDirection::Input)
							|| (!bInputAggregate && Pin.Direction == ERigPinDirection::Output))
							AggregatePins.Add(&Pin);
					for (int32 PinIndex = 2; PinIndex < AggregatePins.Num(); ++PinIndex)
						if (Controller->AddAggregatePin(NodeAST.StableId, AggregatePins[PinIndex]->Path,
							AggregatePins[PinIndex]->DefaultValue, false, false).IsEmpty()) Seed = nullptr;
					Node = Seed ? TargetGraph->FindNodeByName(FName(*NodeAST.StableId)) : nullptr;
				}
				break;
			}
			case ERigNodeKind::Variable:
			{
				const FString VariableName = UnquoteRigLangProperty(NodeAST.Properties.FindRef(TEXT("variable-name")));
				const FString CPPType = UnquoteRigLangProperty(NodeAST.Properties.FindRef(TEXT("variable-cpp-type")));
				const FString ObjectPath = UnquoteRigLangProperty(NodeAST.Properties.FindRef(TEXT("variable-cpp-type-object")));
				FGuid VariableGuid;
				const bool bHasVariableGuid = FGuid::Parse(
					UnquoteRigLangProperty(NodeAST.Properties.FindRef(TEXT("variable-guid"))),
					VariableGuid) && VariableGuid.IsValid();
				FString CreationVariableName = VariableName;
				if (bHasVariableGuid
					&& NodeAST.Properties.FindRef(TEXT("external")) == TEXT("true"))
				{
					if (const FRigVariableAST* Owner = Module.Variables.FindByPredicate(
						[&VariableGuid](const FRigVariableAST& Candidate)
						{
							FGuid CandidateGuid;
							return FGuid::Parse(Candidate.StableId, CandidateGuid)
								&& CandidateGuid == VariableGuid;
						}))
					{
						CreationVariableName = Owner->Name;
					}
				}
				URigVMVariableNode* VariableNode = Controller->AddVariableNode(
					FName(*CreationVariableName), CPPType,
					ObjectPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *ObjectPath),
					NodeAST.Properties.FindRef(TEXT("getter")) == TEXT("true"),
					UnquoteRigLangProperty(NodeAST.Properties.FindRef(TEXT("variable-default"))),
					FVector2D::ZeroVector, NodeAST.StableId, false, false);
				if (VariableNode && bHasVariableGuid)
				{
					if (const FGuid* GeneratedGuid = LocalGuidRemapByGraph.FindOrAdd(
						OwningLocalScopeStableId).Find(VariableGuid))
						VariableGuid = *GeneratedGuid;
					Controller->RefreshVariableNode(FName(*NodeAST.StableId), VariableGuid,
						FName(*VariableName), CPPType,
						ObjectPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *ObjectPath), false, false);
					if (VariableNode->GetVariableGuid() != VariableGuid) VariableNode = nullptr;
				}
				Node = VariableNode;
				break;
			}
			case ERigNodeKind::Call:
			{
				if (!NodeAST.FunctionIdentifier.IsComplete()) return nullptr;
				URigVMLibraryNode* Referenced = FunctionsByIdentifier.FindRef(
					NodeAST.FunctionIdentifier.ToStableId());
				if (Referenced) Node = Controller->AddFunctionReferenceNode(
					Referenced, FVector2D::ZeroVector, NodeAST.StableId, false, false);
				else
				{
					const FRigVMGraphFunctionIdentifier Identifier(
						FSoftObjectPath(NodeAST.FunctionIdentifier.HostObject),
						NodeAST.FunctionIdentifier.LibraryNodePath);
					const FRigVMGraphFunctionHeader Header =
						FRigVMGraphFunctionHeader::FindGraphFunctionHeader(Identifier);
					if (Header.IsValid() && Header.LibraryPointer == Identifier)
						Node = Controller->AddFunctionReferenceNodeFromDescription(
							Header, FVector2D::ZeroVector, NodeAST.StableId, false, false);
				}
				break;
			}
			case ERigNodeKind::InvokeEntry:
				Node = Controller->AddInvokeEntryNode(FName(*UnquoteRigLangProperty(
					NodeAST.Properties.FindRef(TEXT("entry-name")))), FVector2D::ZeroVector,
					NodeAST.StableId, false, false);
				break;
			case ERigNodeKind::Comment:
				Node = Controller->AddCommentNode(
					UnquoteRigLangProperty(NodeAST.Properties.FindRef(TEXT("comment-text"))),
					FVector2D::ZeroVector, FVector2D(400.0, 300.0), FLinearColor::Black,
					NodeAST.StableId, false, false);
				break;
			case ERigNodeKind::Reroute:
			{
				const FRigPinAST* ValuePin = NodeAST.Pins.FindByPredicate(
					[](const FRigPinAST& Pin) { return Pin.Path == TEXT("Value"); });
				if (ValuePin)
				{
					FString CPPType = ValuePin->Type.CPPType;
					if (ValuePin->Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
						CPPType = TEXT("TArray<") + CPPType + TEXT(">");
					Node = Controller->AddFreeRerouteNode(CPPType,
						FName(*ValuePin->Type.CPPTypeObject),
						NodeAST.Properties.FindRef(TEXT("literal")) == TEXT("true"), NAME_None,
						ValuePin->DefaultValue, FVector2D::ZeroVector, NodeAST.StableId, false);
				}
				break;
			}
			default:
				break;
			}
			return Node;
		};

		BuildGraphRecursive = [&Module, &Result, &GraphsByStableId, &LocalGuidRemapByGraph,
			&CreateTypedNode, &RebindContainedGraphs](
			const FRigGraphAST& GraphAST, URigVMGraph* TargetGraph, URigVMController* Controller,
			const FString& InOwningLocalScopeStableId, const bool bPreparingCollapse,
			TArray<FName>* BuiltNames) -> bool
		{
			if (!TargetGraph || !Controller) return false;
			const FString OwningLocalScopeStableId = GraphAST.Role == TEXT("function")
				? GraphAST.StableId : InOwningLocalScopeStableId;
			TArray<const FRigNodeAST*> OrderedNodes;
			for (const FRigNodeAST& NodeAST : GraphAST.Nodes) OrderedNodes.Add(&NodeAST);
			OrderedNodes.StableSort([](const FRigNodeAST& A, const FRigNodeAST& B)
			{
				if (A.bInjected != B.bInjected) return !A.bInjected;
				if (!A.bInjected) return A.StableId < B.StableId;
				if (A.InjectionOwnerPin != B.InjectionOwnerPin)
					return A.InjectionOwnerPin < B.InjectionOwnerPin;
				return A.InjectionOrder < B.InjectionOrder;
			});
			for (const FRigNodeAST* NodeAST : OrderedNodes)
			{
				if (NodeAST->bInjected) continue;
				if (bPreparingCollapse && (NodeAST->Kind == ERigNodeKind::Entry
					|| NodeAST->Kind == ERigNodeKind::Return)) continue;
				URigVMNode* Node = CreateTypedNode(*NodeAST, GraphAST, TargetGraph, Controller,
					OwningLocalScopeStableId);
				if (!Node)
				{
					AddError(Result, FString::Printf(TEXT("Failed exact reconstruction of Rig node '%s' in graph '%s'"),
						*NodeAST->StableId, *GraphAST.StableId), NodeAST->Location);
					return false;
				}
				if (NodeAST->Kind == ERigNodeKind::Call
					&& !RestoreAndVerifyVariableRemapping(Controller,
						CastChecked<URigVMFunctionReferenceNode>(Node), *NodeAST, Result)) return false;
				if (!ResolveTemplateNodeFromSource(Controller, Node, *NodeAST, Result)) return false;
				if (!RestoreAndVerifyPins(Controller, Node, *NodeAST, Result)) return false;
				if (NodeAST->Properties.FindRef(TEXT("template-resolved")) == TEXT("true"))
				{
					if (!ResolveTemplateNodeFromSource(Controller, Node, *NodeAST, Result)) return false;
					Node = Controller->GetGraph()->FindNodeByName(FName(*NodeAST->StableId));
					if (!Node || !VerifyPinsReadOnly(Node, *NodeAST, Result)) return false;
				}
				if (!NodeAST->ContainedGraphStableId.IsEmpty())
				{
					if (URigVMCollapseNode* ContainedNode = Cast<URigVMCollapseNode>(Node))
						if (ContainedNode->GetContainedGraph())
						{
							GraphsByStableId.Add(NodeAST->ContainedGraphStableId,
								ContainedNode->GetContainedGraph());
							if (const FRigGraphAST* ContainedAST = Module.Graphs.FindByPredicate(
								[NodeAST](const FRigGraphAST& Candidate)
								{
									return Candidate.StableId == NodeAST->ContainedGraphStableId;
								}))
								RebindContainedGraphs(*ContainedAST, ContainedNode->GetContainedGraph());
						}
				}
				if (BuiltNames) BuiltNames->Add(FName(*NodeAST->StableId));
			}
			for (const FRigNodeAST* NodeAST : OrderedNodes)
			{
				if (!NodeAST->bInjected) continue;
				FString OwnerPin = NodeAST->InjectionOwnerPin;
				for (const FRigNodeAST& OwnerCandidate : GraphAST.Nodes)
				{
					if (OwnerCandidate.bInjected) continue;
					const FString Prefix = OwnerCandidate.StableId + TEXT(".");
					const int32 PrefixIndex = OwnerPin.Find(
						Prefix, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (PrefixIndex != INDEX_NONE) { OwnerPin = OwnerPin.Mid(PrefixIndex); break; }
				}
				URigVMInjectionInfo* Injection = nullptr;
				if (URigVMPin* Owner = TargetGraph->FindPin(OwnerPin))
					for (URigVMInjectionInfo* Candidate : Owner->GetInjectedNodes())
						if (Candidate && Candidate->Node
							&& Candidate->Node->GetName() == NodeAST->StableId
							&& Candidate->bInjectedAsInput == NodeAST->bInjectedAsInput)
						{
							Injection = Candidate;
							break;
						}
				if (!Injection)
				{
					UScriptStruct* ScriptStruct = LoadObject<UScriptStruct>(nullptr,
						*UnquoteRigLangProperty(NodeAST->Properties.FindRef(TEXT("script-struct"))));
					if (ScriptStruct) Injection = Controller->AddInjectedNode(
						OwnerPin, NodeAST->bInjectedAsInput, ScriptStruct,
						FName(*NodeAST->MethodName), FName(*NodeAST->InjectionInputPin),
						FName(*NodeAST->InjectionOutputPin), NodeAST->StableId, false, false);
				}
				if (!Injection || !Injection->Node
					|| !RestoreAndVerifyPins(Controller, Injection->Node, *NodeAST, Result)) return false;
			}
			for (const FRigLinkAST& Link : GraphAST.Links)
			{
				if (bPreparingCollapse
					&& (Link.SourceNodeId == TEXT("Entry") || Link.TargetNodeId == TEXT("Return"))) continue;
				if (!RestoreAndVerifyLink(Controller, TargetGraph, Link, Result)) return false;
			}
			return true;
		};

		TArray<const FRigGraphAST*> GraphBuildOrder;
		for (const FRigGraphAST& Graph : Module.Graphs) GraphBuildOrder.Add(&Graph);
		GraphBuildOrder.Sort([](const FRigGraphAST& A, const FRigGraphAST& B)
		{
			auto Priority = [](const FString& Role)
			{
				if (Role == TEXT("function")) return 0;
				if (Role == TEXT("root")) return 1;
				if (Role == TEXT("function-library")) return 2;
				return 3;
			};
			const int32 APriority = Priority(A.Role);
			const int32 BPriority = Priority(B.Role);
			return APriority == BPriority ? A.StableId < B.StableId : APriority < BPriority;
		});
		bool bCompiledFunctionDefinitions = false;
		for (const FRigGraphAST* GraphPtr : GraphBuildOrder)
		{
			const FRigGraphAST& Graph = *GraphPtr;
			if (!bCompiledFunctionDefinitions && Graph.Role == TEXT("root"))
			{
				Result.Blueprint->RecompileVM();
				bCompiledFunctionDefinitions = true;
			}
			URigVMGraph* const* TargetGraphPtr = GraphsByStableId.Find(Graph.StableId);
			if (!TargetGraphPtr || !*TargetGraphPtr)
			{
				if (Graph.Role == TEXT("node-contained")) continue;
				AddError(Result, FString::Printf(TEXT("No owner graph for '%s'"), *Graph.StableId), Graph.Location);
				return AbortImport();
			}
			URigVMGraph* TargetGraph = *TargetGraphPtr;
			URigVMController* GraphController = Result.Blueprint->GetOrCreateController(TargetGraph);
			if (!GraphController)
			{
				AddError(Result, FString::Printf(TEXT("No controller for Rig graph '%s'"), *Graph.StableId), Graph.Location);
				return AbortImport();
			}
			if (Graph.Role == TEXT("function-library")) continue;
			if (Graph.Role == TEXT("node-contained")) continue;
			if (!BuildGraphRecursive(Graph, TargetGraph, GraphController, FString(), false, nullptr))
				return AbortImport();
		}

#if ENGINE_MAJOR_VERSION >= 5
		TMap<const URigVMGraph*, URigVMEdGraph*> EditorGraphsByModel;
		auto FindEditorGraphForModel = [&Result, &EditorGraphsByModel](const URigVMGraph* Model) -> URigVMEdGraph*
		{
			if (URigVMEdGraph** Existing = EditorGraphsByModel.Find(Model)) return *Existing;
			TArray<UEdGraph*> EditorGraphs;
			Result.Blueprint->GetAllGraphs(EditorGraphs);
			for (UEdGraph* Candidate : EditorGraphs)
			{
				URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Candidate);
				if (RigGraph && RigGraph->GetModel() == Model)
				{
					EditorGraphsByModel.Add(Model, RigGraph);
					return RigGraph;
				}
			}
			return nullptr;
		};
		TFunction<URigVMEdGraph*(URigVMGraph*)> EnsureEditorGraphForModel;
		EnsureEditorGraphForModel = [&Result, &EditorGraphsByModel, &FindEditorGraphForModel,
			&EnsureEditorGraphForModel](URigVMGraph* Model) -> URigVMEdGraph*
		{
			if (!Model) return nullptr;
			if (URigVMEdGraph* Existing = FindEditorGraphForModel(Model)) return Existing;
			URigVMGraph* ParentModel = Model->GetParentGraph();
			URigVMEdGraph* ParentEditor = EnsureEditorGraphForModel(ParentModel);
			URigVMCollapseNode* OwnerNode = Cast<URigVMCollapseNode>(Model->GetOuter());
			if (!ParentEditor || !OwnerNode) return nullptr;
			URigVMEdGraph* EditorGraph = NewObject<URigVMEdGraph>(ParentEditor, URigVMEdGraph::StaticClass(),
				*OwnerNode->GetEditorSubGraphName(), RF_Transactional);
			EditorGraph->Schema = URigVMEdGraphSchema::StaticClass();
			EditorGraph->bAllowRenaming = true;
			EditorGraph->bEditable = !OwnerNode->IsA<URigVMAggregateNode>();
			EditorGraph->bAllowDeletion = true;
			EditorGraph->ModelNodePath = Model->GetNodePath();
			EditorGraph->bIsFunctionDefinition = false;
			ParentEditor->SubGraphs.Add(EditorGraph);
			EditorGraph->InitializeFromAsset(Result.Blueprint.Get());
			EditorGraphsByModel.Add(Model, EditorGraph);
			if (URigVMController* SyncController = Result.Blueprint->GetOrCreateController(Model))
				SyncController->ResendAllNotifications();
			return EditorGraph;
		};
		for (const FRigGraphAST& Graph : Module.Graphs)
		{
			URigVMGraph* const* ModelGraphPtr = GraphsByStableId.Find(Graph.StableId);
			URigVMEdGraph* EditorGraph = ModelGraphPtr ? FindEditorGraphForModel(*ModelGraphPtr) : nullptr;
			FGuid SourceGraphGuid;
			if (!EditorGraph || !FGuid::Parse(Graph.EditorGuid, SourceGraphGuid)) continue;
			for (URigVMNode* ModelNode : (*ModelGraphPtr)->GetNodes())
			{
				if (ModelNode && !EditorGraph->FindNodeForModelNodeName(ModelNode->GetFName()))
					EditorGraph->HandleModifiedEvent(ERigVMGraphNotifType::NodeAdded, *ModelGraphPtr, ModelNode);
			}
			EditorGraph->GraphGuid = SourceGraphGuid;
			for (const FRigNodeAST& NodeAST : Graph.Nodes)
			{
				if (NodeAST.bInjected) continue;
				FGuid SourceNodeGuid;
				UEdGraphNode* EditorNode = EditorGraph->FindNodeForModelNodeName(FName(*NodeAST.StableId));
				if (!EditorNode)
				{
					for (UEdGraphNode* Candidate : EditorGraph->Nodes)
					{
						if (Candidate && Candidate->GetFName() == FName(*NodeAST.StableId))
						{
							EditorNode = Candidate;
							break;
						}
					}
				}
				if (EditorNode && FGuid::Parse(NodeAST.Guid, SourceNodeGuid)) EditorNode->NodeGuid = SourceNodeGuid;
			}
		}

		Result.Blueprint->RecompileVM();
		const FCompilerResultsLog& CompileLog =
			static_cast<IRigVMEditorAssetInterface*>(Result.Blueprint.Get())->GetCompileLog();
		if (CompileLog.NumErrors > 0 || (Options.bStrict && CompileLog.NumWarnings > 0))
		{
			AddError(Result, FString::Printf(
				TEXT("RigVM compilation produced %d errors and %d warnings%s"),
				CompileLog.NumErrors, CompileLog.NumWarnings,
				Options.bStrict ? TEXT(" in strict mode") : TEXT("")));
			return AbortImport();
		}
		TScriptInterface<IRigVMGraphFunctionHost> FunctionHost =
			static_cast<IRigVMEditorAssetInterface*>(Result.Blueprint.Get())
				->GetRigVMClientHost()->GetRigVMGraphFunctionHost();
		FRigVMGraphFunctionStore* FunctionStore = FunctionHost
			? FunctionHost->GetRigVMGraphFunctionStore() : nullptr;
		if (!FunctionStore && Module.Functions.ContainsByPredicate(
			[](const FRigFunctionAST& Function) { return Function.ExternalVariables.Num() != 0; }))
		{
			AddError(Result, TEXT("Cannot restore Rig function external-variable identities"));
			return AbortImport();
		}
		for (const FRigFunctionAST& Function : Module.Functions)
		{
			if (Function.ExternalVariables.Num() == 0) continue;
			FString FunctionName = Function.Name;
			if (const FString* ShortName = Function.Properties.Find(TEXT("short-name")))
				FunctionName = UnquoteRigLangProperty(*ShortName);
			FRigVMGraphFunctionData* FunctionData = FunctionStore
				? FunctionStore->FindFunctionByName(FName(*FunctionName)) : nullptr;
			if (!FunctionData)
			{
				AddError(Result, FString::Printf(TEXT("Cannot restore external variables for Rig function '%s'"),
					*Function.Name), Function.Location);
				return AbortImport();
			}
			TArray<FRigVMExternalVariable> ExactExternalVariables;
			for (const FRigExternalVariableAST& Variable : Function.ExternalVariables)
			{
				FGuid Guid;
				if (!FGuid::Parse(Variable.Guid, Guid))
				{
					AddError(Result, FString::Printf(TEXT("Invalid external-variable Guid '%s' for '%s.%s'"),
						*Variable.Guid, *Function.Name, *Variable.Name), Variable.Location);
					return AbortImport();
				}
				FString CPPType = Variable.Type.CPPType;
				if (Variable.Type.ContainerType == TEXT("array") && !IsArrayCPPType(CPPType))
					CPPType = TEXT("TArray<") + CPPType + TEXT(">");
				UObject* TypeObject = Variable.Type.CPPTypeObject.IsEmpty()
					? nullptr : LoadObject<UObject>(nullptr, *Variable.Type.CPPTypeObject);
				ExactExternalVariables.Add(FRigVMExternalVariable::Make(
					Guid, FName(*Variable.Name), CPPType, TypeObject, Variable.bPublic, Variable.bReadOnly));
			}
			FunctionData->Header.ExternalVariables = MoveTemp(ExactExternalVariables);
		}
		for (int32 SyncPass = 0; SyncPass < 3; ++SyncPass)
		{
			for (const FRigGraphAST& Graph : Module.Graphs)
			{
				URigVMGraph* const* ModelGraphPtr = GraphsByStableId.Find(Graph.StableId);
				URigVMEdGraph* EditorGraph = ModelGraphPtr ? FindEditorGraphForModel(*ModelGraphPtr) : nullptr;
				if (!EditorGraph && ModelGraphPtr && Graph.Role == TEXT("node-contained"))
					EditorGraph = EnsureEditorGraphForModel(*ModelGraphPtr);
				if (!EditorGraph) continue;
				for (URigVMNode* ModelNode : (*ModelGraphPtr)->GetNodes())
				{
					if (ModelNode && !EditorGraph->FindNodeForModelNodeName(ModelNode->GetFName()))
						EditorGraph->HandleModifiedEvent(ERigVMGraphNotifType::NodeAdded, *ModelGraphPtr, ModelNode);
				}
			}
		}
		for (const FRigGraphAST& Graph : Module.Graphs)
		{
			URigVMGraph* const* ModelGraphPtr = GraphsByStableId.Find(Graph.StableId);
			URigVMEdGraph* EditorGraph = ModelGraphPtr ? FindEditorGraphForModel(*ModelGraphPtr) : nullptr;
			FGuid SourceGraphGuid;
			const bool bHasSourceEditorNode = Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				FGuid EditorGuid;
				return !Node.bInjected && FGuid::Parse(Node.Guid, EditorGuid);
			});
			if (!EditorGraph && !bHasSourceEditorNode) continue;
			if (!EditorGraph || !FGuid::Parse(Graph.EditorGuid, SourceGraphGuid))
			{
				AddError(Result, FString::Printf(TEXT("Cannot restore editor graph identity for '%s'"),
					*Graph.StableId), Graph.Location);
				return AbortImport();
			}
			EditorGraph->GraphGuid = SourceGraphGuid;
			for (const FRigNodeAST& NodeAST : Graph.Nodes)
			{
				if (NodeAST.bInjected) continue;
				FGuid SourceNodeGuid;
				if (!FGuid::Parse(NodeAST.Guid, SourceNodeGuid)) continue;
				UEdGraphNode* EditorNode = EditorGraph->FindNodeForModelNodeName(FName(*NodeAST.StableId));
				if (!EditorNode)
				{
					for (UEdGraphNode* Candidate : EditorGraph->Nodes)
					{
						if (Candidate && Candidate->GetFName() == FName(*NodeAST.StableId))
						{
							EditorNode = Candidate;
							break;
						}
					}
				}
				if (!EditorNode)
				{
					AddError(Result, FString::Printf(TEXT("Cannot restore editor node identity for '%s.%s'"),
						*Graph.StableId, *NodeAST.StableId), NodeAST.Location);
					return AbortImport();
				}
				EditorNode->NodeGuid = SourceNodeGuid;
			}
		}
		Result.bCompiled = bHasExecutableGraphContent;
#endif
	}

	TSet<FString> GeneratedIdentityVariables;
	for (const FRigVariableAST& Variable : Module.Variables)
	{
		FGuid SourceGuid;
		if (!FGuid::Parse(Variable.StableId, SourceGuid))
		{
			GeneratedIdentityVariables.Add(Variable.Name);
		}
	}
	const FRigLangExportResult ReExport = FRigLangExporter::Export(Result.Blueprint.Get());
	const FString ExpectedSnapshot = HierarchyVariableSemanticSnapshot(Module, GeneratedIdentityVariables);
	const FString ActualSnapshot = ReExport.Module
		? HierarchyVariableSemanticSnapshot(*ReExport.Module, GeneratedIdentityVariables)
		: FString();
	if (!ReExport.bSuccess || !ReExport.Module || ExpectedSnapshot != ActualSnapshot)
	{
		int32 Difference = 0;
		while (Difference < ExpectedSnapshot.Len() && Difference < ActualSnapshot.Len()
			&& ExpectedSnapshot[Difference] == ActualSnapshot[Difference]) ++Difference;
		AddError(Result, FString::Printf(
			TEXT("Rig hierarchy/variable immediate re-export semantic mismatch at %d; expected '%s', actual '%s'"),
			Difference,
			*ExpectedSnapshot.Mid(FMath::Max(0, Difference - 40), 160).Replace(TEXT("\n"), TEXT(" ")),
			*ActualSnapshot.Mid(FMath::Max(0, Difference - 40), 160).Replace(TEXT("\n"), TEXT(" "))));
		return AbortImport();
	}
	if (bHasGraphInventory)
	{
		const FString ExpectedGraph = GraphSemanticSnapshot(Module, Module);
		const FString ActualGraph = GraphSemanticSnapshot(*ReExport.Module, Module);
		if (ExpectedGraph != ActualGraph)
		{
			int32 Difference = 0;
			while (Difference < ExpectedGraph.Len() && Difference < ActualGraph.Len()
				&& ExpectedGraph[Difference] == ActualGraph[Difference]) ++Difference;
			AddError(Result, FString::Printf(
				TEXT("Rig graph immediate re-export semantic mismatch at %d; expected '%s', actual '%s'"),
				Difference,
				*ExpectedGraph.Mid(FMath::Max(0, Difference - 40), 160).Replace(TEXT("\n"), TEXT(" ")),
				*ActualGraph.Mid(FMath::Max(0, Difference - 40), 160).Replace(TEXT("\n"), TEXT(" "))));
			return AbortImport();
		}
	}
	if (!bHasExecutableGraphContent) Result.bCompiled = false;
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangInheritedLocalSemanticSnapshotTest,
	"AnimBP2FP.RigLang.Importer.InheritedLocalSemanticSnapshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigLangInheritedLocalSemanticSnapshotTest::RunTest(const FString& Parameters)
{
	auto MakeModule = [](const FString& Prefix, const FString& LocalGuid)
	{
		FRigModuleAST Module;
		FRigGraphAST FunctionGraph;
		FunctionGraph.StableId = Prefix + TEXT("-function");
		FunctionGraph.Role = TEXT("function");
		FunctionGraph.Properties.Add(TEXT("graph-name"), TEXT("\"PublicScale\""));
		FRigGraphVariableAST Local;
		Local.Name = TEXT("FunctionLocal");
		Local.Guid = LocalGuid;
		Local.Type.CPPType = TEXT("float");
		FunctionGraph.LocalVariables.Add(Local);
		FRigNodeAST Outer;
		Outer.StableId = TEXT("OuterCollapse");
		Outer.Guid = TEXT("model:") + Prefix + TEXT("-outer");
		Outer.Kind = ERigNodeKind::Collapse;
		Outer.ContainedGraphStableId = Prefix + TEXT("-outer-graph");
		FunctionGraph.Nodes.Add(Outer);

		FRigGraphAST OuterGraph;
		OuterGraph.StableId = Prefix + TEXT("-outer-graph");
		OuterGraph.ParentStableId = FunctionGraph.StableId;
		OuterGraph.Role = TEXT("node-contained");
		OuterGraph.Properties.Add(TEXT("graph-name"), TEXT("\"OuterContained\""));
		FRigNodeAST Inner;
		Inner.StableId = TEXT("InnerCollapse");
		Inner.Guid = TEXT("model:") + Prefix + TEXT("-inner");
		Inner.Kind = ERigNodeKind::Collapse;
		Inner.ContainedGraphStableId = Prefix + TEXT("-inner-graph");
		OuterGraph.Nodes.Add(Inner);

		FRigGraphAST InnerGraph;
		InnerGraph.StableId = Prefix + TEXT("-inner-graph");
		InnerGraph.ParentStableId = OuterGraph.StableId;
		InnerGraph.Role = TEXT("node-contained");
		InnerGraph.Properties.Add(TEXT("graph-name"), TEXT("\"InnerContained\""));
		FRigNodeAST Getter;
		Getter.StableId = TEXT("ReadNestedFunctionLocal");
		Getter.Guid = TEXT("model:") + Prefix + TEXT("-getter");
		Getter.Kind = ERigNodeKind::Variable;
		Getter.Properties.Add(TEXT("variable-name"), TEXT("\"FunctionLocal\""));
		Getter.Properties.Add(TEXT("variable-guid"), TEXT("\"") + LocalGuid + TEXT("\""));
		InnerGraph.Nodes.Add(Getter);

		Module.Graphs = {FunctionGraph, OuterGraph, InnerGraph};
		return Module;
	};

	const FRigModuleAST Source = MakeModule(
		TEXT("source"), TEXT("11111111-1111-1111-1111-111111111111"));
	const FRigModuleAST Imported = MakeModule(
		TEXT("imported"), TEXT("22222222-2222-2222-2222-222222222222"));
	const FString Expected = GraphSemanticSnapshot(Source, Source);
	const FString Actual = GraphSemanticSnapshot(Imported, Source);
	TestEqual(TEXT("contained local references use the ancestor function local token"), Actual, Expected);
	TestTrue(TEXT("snapshot contains the ancestor generated-local token"),
		Actual.Contains(TEXT("$generated-local-guid:graph/function:PublicScale|FunctionLocal")));
	return true;
}
#endif

#else

FString FRigLangImporter::BuildCanonicalPinDefault(const FRigPinAST& Pin)
{
	return Pin.DefaultValue;
}

FString FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(
	const FRigModuleAST& Value, const FRigModuleAST& SourceIdentity)
{
	return Value.ToCanonicalHashInput();
}

FString FRigLangImporter::BuildGraphSemanticSnapshot(
	const FRigModuleAST& Value, const FRigModuleAST& SourceIdentity)
{
	return Value.ToCanonicalHashInput();
}

TMap<FString, FString> FRigLangImporter::BuildGraphSemanticTokens(
	const FRigModuleAST& Value, TSet<FString>* OutCollidingTokens)
{
	if (OutCollidingTokens) OutCollidingTokens->Reset();
	TMap<FString, FString> Result;
	for (const FRigGraphAST& Graph : Value.Graphs)
	{
		Result.Add(Graph.StableId, Graph.StableId);
	}
	return Result;
}

FRigLangImportResult FRigLangImporter::Import(
	const FRigModuleAST& Module, const FRigLangImportOptions& Options)
{
	FRigLangImportResult Result;
	Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::RoundTrip,
		TEXT("[UNSUPPORTED:UE4ControlRigAssetAuthoring] RigLang asset import requires Unreal Engine 5"));
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
bool FRigLangImporter::ResolveTemplateNodeForTest(
	URigVMController* Controller, URigVMNode*& Node, const FRigNodeAST& Source,
	FRigLangImportResult& Result)
{
	Result.Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::RoundTrip,
		TEXT("[UNSUPPORTED:LegacyRigVMTemplate] RigVM template resolution requires Unreal Engine 5.4+"));
	return false;
}
#endif

#endif
