// RigLangImporter.cpp - Staging importer for Rig hierarchy and member variables

#include "RigLangImporter.h"

#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "Misc/PackageName.h"
#include "RigLangExporter.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMEditorAsset.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "UObject/Package.h"
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
					const FRigElementKey ReferencedKey(StableId);
					if (!ReferencedKey.IsValid() || !ElementByStableId.Contains(StableId))
					{
						AddError(Result, FString::Printf(
							TEXT("Metadata '%s' on '%s' references missing hierarchy key '%s'"),
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
		for (const FString& Value : Metadata.StringValues) Values.Emplace(Value);
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
		return Metadata.StringValues.Num() == 1 && Hierarchy->SetRigElementKeyMetadata(Key, Name, FRigElementKey(Metadata.StringValues[0]));
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
	for (FRigVariableAST& Variable : Copy.Variables)
	{
		if (GeneratedIdentityVariables.Contains(Variable.Name))
		{
			Variable.StableId = Variable.Name;
		}
	}
	return Copy.ToCanonicalString();
}
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
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	Result.Blueprint = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), Package, BlueprintName,
		Options.bTransient ? RF_Transient : RF_Public | RF_Standalone,
		nullptr, GWarn));
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
			&& !Element.Parents.IsEmpty();
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
		if (Element.Kind == ERigHierarchyElementKind::Bone || Element.Parents.IsEmpty()) continue;
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
		const FName Added = static_cast<IRigVMEditorAssetInterface*>(Result.Blueprint.Get())
			->AddHostMemberVariableFromExternal(External, Variable.DefaultValue);
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
			if (Reflected->DefaultValue != Variable.DefaultValue) Mismatches.Add(TEXT("default"));
		}
		if (!Mismatches.IsEmpty())
		{
			AddError(Result, FString::Printf(TEXT("Rig variable '%s' reflection mismatch: %s"),
				*Variable.Name, *FString::Join(Mismatches, TEXT(", "))), Variable.Location);
			return AbortImport();
		}
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
	Result.bCompiled = false;
	return Result;
}
