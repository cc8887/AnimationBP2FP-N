// RigLangExporter.cpp - Control Rig to RigLang export
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "RigLangExporter.h"

namespace
{
constexpr uint32 SHA256Constants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

uint32 RotateRight(const uint32 Value, const uint32 Count)
{
	return (Value >> Count) | (Value << (32 - Count));
}

FString SHA256UTF8(const FString& Text)
{
	FTCHARToUTF8 UTF8(*Text);
	TArray<uint8> Message;
	Message.Append(reinterpret_cast<const uint8*>(UTF8.Get()), UTF8.Length());
	const uint64 BitLength = static_cast<uint64>(Message.Num()) * 8;
	Message.Add(0x80);
	while ((Message.Num() % 64) != 56)
	{
		Message.Add(0);
	}
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Message.Add(static_cast<uint8>(BitLength >> Shift));
	}

	uint32 State[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	for (int32 Offset = 0; Offset < Message.Num(); Offset += 64)
	{
		uint32 Words[64];
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const int32 Byte = Offset + Index * 4;
			Words[Index] = (static_cast<uint32>(Message[Byte]) << 24)
				| (static_cast<uint32>(Message[Byte + 1]) << 16)
				| (static_cast<uint32>(Message[Byte + 2]) << 8)
				| static_cast<uint32>(Message[Byte + 3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 S0 = RotateRight(Words[Index - 15], 7)
				^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
			const uint32 S1 = RotateRight(Words[Index - 2], 17)
				^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
			Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
		}

		uint32 A = State[0], B = State[1], C = State[2], D = State[3];
		uint32 E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
			const uint32 Choice = (E & F) ^ (~E & G);
			const uint32 Temp1 = H + S1 + Choice + SHA256Constants[Index] + Words[Index];
			const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
			const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
			const uint32 Temp2 = S0 + Majority;
			H = G; G = F; F = E; E = D + Temp1;
			D = C; C = B; B = A; A = Temp1 + Temp2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D;
		State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}

	uint8 Digest[32];
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Digest[Index * 4] = static_cast<uint8>(State[Index] >> 24);
		Digest[Index * 4 + 1] = static_cast<uint8>(State[Index] >> 16);
		Digest[Index * 4 + 2] = static_cast<uint8>(State[Index] >> 8);
		Digest[Index * 4 + 3] = static_cast<uint8>(State[Index]);
	}
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
}
}

FString FRigLangExporter::ComputeContentHash(const FString& CanonicalHashInput)
{
	return TEXT("sha256:") + SHA256UTF8(CanonicalHashInput);
}

FString FRigLangExporter::ComputeDeterministicEditorGuid(
	const FString& ModuleIdentity,
	const FString& GraphIdentity)
{
	const FString Input = FString::Printf(
		TEXT("riglang-editor-guid-v1\n%d:%s\n%d:%s"),
		ModuleIdentity.Len(), *ModuleIdentity,
		GraphIdentity.Len(), *GraphIdentity);
	FString Digits = SHA256UTF8(Input).Left(32);
	if (Digits == TEXT("00000000000000000000000000000000")) Digits[31] = TEXT('1');
	FGuid Guid;
	check(FGuid::ParseExact(Digits, EGuidFormats::Digits, Guid) && Guid.IsValid());
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}

bool FRigLangExporter::ValidateStrictCoverage(
	const FRigLangExportCoverage& Coverage,
	TArray<FString>& OutErrors)
{
	bool bValid = true;
	auto CheckCount = [&OutErrors, &bValid](
		const TCHAR* Kind, const int32 Total, const int32 Visited)
	{
		if (Total != Visited)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Strict export visited %d of %d %s"), Visited, Total, Kind));
			bValid = false;
		}
	};
	CheckCount(TEXT("models"), Coverage.ModelTotal, Coverage.VisitedModels.Num());
	CheckCount(TEXT("nodes"), Coverage.NodeTotal, Coverage.VisitedNodes.Num());
	CheckCount(TEXT("pins"), Coverage.PinTotal, Coverage.VisitedPins.Num());
	CheckCount(TEXT("links"), Coverage.LinkTotal, Coverage.VisitedLinks.Num());
	for (const FString& ConnectedPin : Coverage.ConnectedPins)
	{
		if (!Coverage.VisitedPins.Contains(ConnectedPin))
		{
			OutErrors.Add(TEXT("Strict export did not visit connected pin: ") + ConnectedPin);
			bValid = false;
		}
	}
	for (const FString& Node : Coverage.LossyOrUnsupportedNodes)
	{
		OutErrors.Add(TEXT("Strict export cannot reconstruct lossy or unsupported node: ") + Node);
		bValid = false;
	}
	return bValid;
}

#if WITH_EDITOR

#include "ControlRigBlueprintLegacy.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyMetadata.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMDispatchNode.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMCore/RigVMRegistry.h"

namespace
{
FString QuoteRigLangValue(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return TEXT("\"") + Escaped + TEXT("\"");
}

FString ExportStructText(const UScriptStruct* Struct, const void* Value)
{
	FString Text;
	if (Struct && Value)
	{
		Struct->ExportText(Text, Value, Value, nullptr, PPF_None, nullptr);
	}
	return Text;
}

FString ExportTypedControlValue(const FRigControlValue& Value, ERigControlType Type)
{
	auto F = [](float Number) { return LexToString(Number); };
	const FString TypeName = StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Type));
	TArray<FString> Fields;
	switch (Type)
	{
	case ERigControlType::Bool: Fields.Add(Value.Get<bool>() ? TEXT("true") : TEXT("false")); break;
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat: Fields.Add(F(Value.Get<float>())); break;
	case ERigControlType::Integer: Fields.Add(FString::FromInt(Value.Get<int32>())); break;
	case ERigControlType::Position:
	case ERigControlType::Scale:
	case ERigControlType::Rotator:
	case ERigControlType::Vector2D:
	{
		const FVector3f& V = Value.GetRef<FVector3f>();
		Fields = {F(V.X), F(V.Y), F(V.Z)};
		break;
	}
	case ERigControlType::Transform:
	{
		const FRigControlValue::FTransform_Float& T = Value.GetRef<FRigControlValue::FTransform_Float>();
		Fields = {F(T.TranslationX), F(T.TranslationY), F(T.TranslationZ), F(T.RotationX), F(T.RotationY),
			F(T.RotationZ), F(T.RotationW), F(T.ScaleX), F(T.ScaleY), F(T.ScaleZ)};
		break;
	}
	case ERigControlType::TransformNoScale:
	{
		const FRigControlValue::FTransformNoScale_Float& T = Value.GetRef<FRigControlValue::FTransformNoScale_Float>();
		Fields = {F(T.TranslationX), F(T.TranslationY), F(T.TranslationZ), F(T.RotationX), F(T.RotationY),
			F(T.RotationZ), F(T.RotationW)};
		break;
	}
	case ERigControlType::EulerTransform:
	{
		const FRigControlValue::FEulerTransform_Float& T = Value.GetRef<FRigControlValue::FEulerTransform_Float>();
		Fields = {F(T.TranslationX), F(T.TranslationY), F(T.TranslationZ), F(T.RotationPitch), F(T.RotationYaw),
			F(T.RotationRoll), F(T.ScaleX), F(T.ScaleY), F(T.ScaleZ)};
		break;
	}
	default: break;
	}
	return TEXT("(") + TypeName + (Fields.IsEmpty() ? FString() : TEXT(" ") + FString::Join(Fields, TEXT(" "))) + TEXT(")");
}

FRigHierarchyStateAST ExportControlState(const FRigControlValue& Value, const ERigControlType Type, const FString& Role)
{
	FRigHierarchyStateAST State;
	State.Kind = ERigHierarchyStateKind::ControlValue;
	State.Role = Role;
	State.Type = StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Type));
	auto Add = [&State](const float Number) { State.Components.Add(Number); };
	switch (Type)
	{
	case ERigControlType::Bool: State.bBoolValue = Value.Get<bool>(); break;
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat: State.NumberValue = Value.Get<float>(); break;
	case ERigControlType::Integer: State.IntegerValue = Value.Get<int32>(); break;
	case ERigControlType::Vector2D:
	{
		const FVector3f V = Value.Get<FVector3f>(); Add(V.X); Add(V.Y); break;
	}
	case ERigControlType::Position:
	case ERigControlType::Scale:
	case ERigControlType::Rotator:
	{
		const FVector3f& V = Value.GetRef<FVector3f>(); Add(V.X); Add(V.Y); Add(V.Z); break;
	}
	case ERigControlType::Transform:
	{
		const auto& T = Value.GetRef<FRigControlValue::FTransform_Float>();
		Add(T.TranslationX); Add(T.TranslationY); Add(T.TranslationZ); Add(T.RotationX); Add(T.RotationY);
		Add(T.RotationZ); Add(T.RotationW); Add(T.ScaleX); Add(T.ScaleY); Add(T.ScaleZ); break;
	}
	case ERigControlType::TransformNoScale:
	{
		const auto& T = Value.GetRef<FRigControlValue::FTransformNoScale_Float>();
		Add(T.TranslationX); Add(T.TranslationY); Add(T.TranslationZ); Add(T.RotationX); Add(T.RotationY); Add(T.RotationZ); Add(T.RotationW); break;
	}
	case ERigControlType::EulerTransform:
	{
		const auto& T = Value.GetRef<FRigControlValue::FEulerTransform_Float>();
		Add(T.TranslationX); Add(T.TranslationY); Add(T.TranslationZ); Add(T.RotationPitch); Add(T.RotationYaw); Add(T.RotationRoll);
		Add(T.ScaleX); Add(T.ScaleY); Add(T.ScaleZ); break;
	}
	default: break;
	}
	return State;
}

void AddHierarchyTransform(FRigHierarchyElementAST& Element, const ERigHierarchyTransformRole Role, const FTransform& Value)
{
	FRigHierarchyTransformAST& Transform = Element.Transforms.AddDefaulted_GetRef();
	Transform.Role = Role;
	Transform.Value = Value;
}

FRigHierarchyMetadataAST ExportHierarchyMetadata(const FRigBaseMetadata* Source)
{
	FRigHierarchyMetadataAST Out;
	if (!Source) return Out;
	Out.Name = Source->GetName().ToString();
	auto Names = [&Out](const auto& Values) { for (const auto& Value : Values) Out.StringValues.Add(Value.ToString()); };
	switch (Source->GetType())
	{
	case ERigMetadataType::Bool: Out.Kind=ERigHierarchyMetadataValueKind::Bool; Out.BoolValues.Add(CastChecked<FRigBoolMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::BoolArray: Out.Kind=ERigHierarchyMetadataValueKind::BoolArray; Out.BoolValues=CastChecked<FRigBoolArrayMetadata>(Source)->GetValue(); break;
	case ERigMetadataType::Float: Out.Kind=ERigHierarchyMetadataValueKind::Float; Out.NumberValues.Add(CastChecked<FRigFloatMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::FloatArray: Out.Kind=ERigHierarchyMetadataValueKind::FloatArray; for(float V:CastChecked<FRigFloatArrayMetadata>(Source)->GetValue())Out.NumberValues.Add(V); break;
	case ERigMetadataType::Int32: Out.Kind=ERigHierarchyMetadataValueKind::Integer; Out.IntegerValues.Add(CastChecked<FRigInt32Metadata>(Source)->GetValue()); break;
	case ERigMetadataType::Int32Array: Out.Kind=ERigHierarchyMetadataValueKind::IntegerArray; for(int32 V:CastChecked<FRigInt32ArrayMetadata>(Source)->GetValue())Out.IntegerValues.Add(V); break;
	case ERigMetadataType::Name: Out.Kind=ERigHierarchyMetadataValueKind::Name; Out.StringValues.Add(CastChecked<FRigNameMetadata>(Source)->GetValue().ToString()); break;
	case ERigMetadataType::NameArray: Out.Kind=ERigHierarchyMetadataValueKind::NameArray; Names(CastChecked<FRigNameArrayMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::Vector: Out.Kind=ERigHierarchyMetadataValueKind::Vector; Out.VectorValues.Add(CastChecked<FRigVectorMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::VectorArray: Out.Kind=ERigHierarchyMetadataValueKind::VectorArray; Out.VectorValues=CastChecked<FRigVectorArrayMetadata>(Source)->GetValue(); break;
	case ERigMetadataType::Rotator: Out.Kind=ERigHierarchyMetadataValueKind::Rotator; Out.RotatorValues.Add(CastChecked<FRigRotatorMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::RotatorArray: Out.Kind=ERigHierarchyMetadataValueKind::RotatorArray; Out.RotatorValues=CastChecked<FRigRotatorArrayMetadata>(Source)->GetValue(); break;
	case ERigMetadataType::Quat: Out.Kind=ERigHierarchyMetadataValueKind::Quat; Out.QuatValues.Add(CastChecked<FRigQuatMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::QuatArray: Out.Kind=ERigHierarchyMetadataValueKind::QuatArray; Out.QuatValues=CastChecked<FRigQuatArrayMetadata>(Source)->GetValue(); break;
	case ERigMetadataType::Transform: Out.Kind=ERigHierarchyMetadataValueKind::Transform; Out.TransformValues.Add(CastChecked<FRigTransformMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::TransformArray: Out.Kind=ERigHierarchyMetadataValueKind::TransformArray; Out.TransformValues=CastChecked<FRigTransformArrayMetadata>(Source)->GetValue(); break;
	case ERigMetadataType::LinearColor: Out.Kind=ERigHierarchyMetadataValueKind::LinearColor; Out.ColorValues.Add(CastChecked<FRigLinearColorMetadata>(Source)->GetValue()); break;
	case ERigMetadataType::LinearColorArray: Out.Kind=ERigHierarchyMetadataValueKind::LinearColorArray; Out.ColorValues=CastChecked<FRigLinearColorArrayMetadata>(Source)->GetValue(); break;
	case ERigMetadataType::RigElementKey: Out.Kind=ERigHierarchyMetadataValueKind::ElementKey; Out.StringValues.Add(CastChecked<FRigElementKeyMetadata>(Source)->GetValue().ToString()); break;
	case ERigMetadataType::RigElementKeyArray: Out.Kind=ERigHierarchyMetadataValueKind::ElementKeyArray; Names(CastChecked<FRigElementKeyArrayMetadata>(Source)->GetValue()); break;
	default: break;
	}
	return Out;
}

FString ExportTransformProperty(const FTransform& Transform)
{
	return QuoteRigLangValue(ExportStructText(TBaseStructure<FTransform>::Get(), &Transform));
}

FString ExportParentWeights(const TArray<FRigElementWeight>& Weights)
{
	TArray<FString> Values;
	for (const FRigElementWeight& Weight : Weights)
	{
		Values.Add(QuoteRigLangValue(ExportStructText(FRigElementWeight::StaticStruct(), &Weight)));
	}
	return TEXT("(") + FString::Join(Values, TEXT(" ")) + TEXT(")");
}

TArray<FRigElementKey> TopologicallySortHierarchy(const URigHierarchy* Hierarchy, TArray<FString>& OutErrors)
{
	TArray<FRigElementKey> Remaining = Hierarchy->GetAllKeys(true);
	TSet<FRigElementKey> AllKeys(Remaining);
	TSet<FRigElementKey> Emitted;
	TArray<FRigElementKey> Result;
	auto Less = [](const FRigElementKey& A, const FRigElementKey& B)
	{
		const FString AType = StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(A.Type));
		const FString BType = StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(B.Type));
		if (AType != BType) return AType < BType;
		return A.Name.LexicalLess(B.Name);
	};
	while (!Remaining.IsEmpty())
	{
		TArray<FRigElementKey> Ready;
		for (const FRigElementKey& Key : Remaining)
		{
			bool bReady = true;
			for (const FRigElementKey& Parent : Hierarchy->GetParents(Key, false))
			{
				if (!AllKeys.Contains(Parent))
				{
					OutErrors.Add(FString::Printf(TEXT("Hierarchy element '%s' has missing parent '%s'"),
						*Key.ToString(), *Parent.ToString()));
					bReady = false;
					break;
				}
				if (!Emitted.Contains(Parent)) { bReady = false; break; }
			}
			if (bReady) Ready.Add(Key);
		}
		Ready.Sort(Less);
		if (Ready.IsEmpty())
		{
			Remaining.Sort(Less);
			OutErrors.Add(TEXT("Hierarchy parent cycle prevents topological export at: ") + Remaining[0].ToString());
			Result.Append(Remaining);
			break;
		}
		const FRigElementKey Next = Ready[0];
		Result.Add(Next);
		Emitted.Add(Next);
		Remaining.RemoveSingle(Next);
	}
	return Result;
}

FString StableRuntimeSymbol(const FString& Value)
{
	return RigFunctionStableRuntimeSymbol(Value);
}

FRigFunctionIdentifierAST ExportFunctionIdentifier(const FRigVMGraphFunctionIdentifier& Identifier)
{
	FRigFunctionIdentifierAST Result;
	Result.HostObject = Identifier.HostObject.ToString();
	Result.LibraryNodePath = Identifier.GetLibraryNodePath();
	return Result;
}

FString FunctionIdentifierStableId(const FRigVMGraphFunctionIdentifier& Identifier)
{
	return ExportFunctionIdentifier(Identifier).ToStableId();
}

FString FunctionIdentifierModuleAssetPath(const FRigFunctionIdentifierAST& Identifier)
{
	FString Host = Identifier.HostObject;
	if (Host.EndsWith(TEXT("_C"))) Host.LeftChopInline(2);
	FString PackagePath;
	FString ObjectName;
	const FString AssetPath = Host.Split(TEXT("."), &PackagePath, &ObjectName) ? PackagePath : Host;
	return FAnimLispModuleId::FromAssetPath(AssetPath, EAnimLispModuleKind::Rig).AssetPath;
}

FString StableImportAliasBase(const FString& AssetPath)
{
	FString Base = AssetPath;
	int32 Slash = INDEX_NONE;
	if (Base.FindLastChar(TEXT('/'), Slash)) Base = Base.Mid(Slash + 1);
	FString Alias;
	for (const TCHAR Character : Base)
	{
		if ((Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('_'))
		{
			Alias.AppendChar(Character);
		}
	}
	if (Alias.IsEmpty()) Alias = TEXT("Rig");
	if (FChar::IsDigit(Alias[0])) Alias = TEXT("Rig_") + Alias;
	return Alias;
}

FString ExportTemplateTypeMap(const FRigVMTemplateTypeMap& TypeMap)
{
	TArray<FString> Values;
	for (const TPair<FName, TRigVMTypeIndex>& Pair : TypeMap)
	{
		const FRigVMTemplateArgumentType& Type = FRigVMRegistry::Get().GetType(Pair.Value);
		Values.Add(TEXT("(") + QuoteRigLangValue(Pair.Key.ToString()) + TEXT(" ")
			+ QuoteRigLangValue(Type.CPPType.ToString()) + TEXT(")"));
	}
	Values.Sort();
	return TEXT("(") + FString::Join(Values, TEXT(" ")) + TEXT(")");
}

ERigPinDirection ExportDirection(const ERigVMPinDirection Direction)
{
	switch (Direction)
	{
	case ERigVMPinDirection::Output: return ERigPinDirection::Output;
	case ERigVMPinDirection::IO: return ERigPinDirection::IO;
	case ERigVMPinDirection::Visible: return ERigPinDirection::Visible;
	case ERigVMPinDirection::Hidden: return ERigPinDirection::Hidden;
	case ERigVMPinDirection::Invalid: return ERigPinDirection::Invalid;
	default: return ERigPinDirection::Input;
	}
}

FString RelativePinPath(const URigVMPin* Pin);

FRigCallableArgumentAST ExportArgument(const FRigVMGraphFunctionArgument& Source)
{
	FRigCallableArgumentAST Result;
	Result.Name = Source.Name.ToString();
	Result.Direction = ExportDirection(Source.Direction);
	Result.Type.CPPType = Source.CPPType.ToString();
	Result.Type.CPPTypeObject = Source.CPPTypeObject.ToSoftObjectPath().ToString();
	Result.Type.ContainerType = Source.bIsArray ? TEXT("array") : FString();
	Result.Type.Canonicalize();
	Result.DefaultValue = Source.DefaultValue;
	Result.bExecuteContext = Source.IsExecuteContext();
	Result.bConstant = Source.bIsConst;
	Result.bInputVariable = Source.bIsInputVariable;
	return Result;
}

FRigCallableArgumentAST ExportArgument(const URigVMPin* Source)
{
	FRigCallableArgumentAST Result;
	Result.Name = RelativePinPath(Source);
	Result.Direction = ExportDirection(Source->GetDirection());
	Result.Type.CPPType = Source->GetCPPType();
	if (const UObject* TypeObject = Source->GetCPPTypeObject())
	{
		Result.Type.CPPTypeObject = TypeObject->GetPathName();
	}
	Result.Type.ContainerType = Source->IsArray() ? TEXT("array") : FString();
	Result.Type.Canonicalize();
	Result.DefaultValue = Source->GetDefaultValue();
	Result.bExecuteContext = Source->IsExecuteContext();
	return Result;
}

FRigGraphVariableAST ExportLocalVariable(const FRigVMGraphVariableDescription& Source)
{
	FRigGraphVariableAST Result;
	Result.Guid = Source.Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
	Result.Name = Source.Name.ToString();
	Result.Type.CPPType = Source.CPPType;
	Result.Type.CPPTypeObject = Source.CPPTypeObject
		? Source.CPPTypeObject->GetPathName()
		: Source.CPPTypeObjectPath.ToString();
	Result.CPPTypeObjectPath = Source.CPPTypeObjectPath.ToString();
	Result.Type.ContainerType = Source.ToExternalVariable().IsArray() ? TEXT("array") : FString();
	Result.Type.Canonicalize();
	Result.DefaultValue = Source.DefaultValue;
	FTextStringHelper::WriteToBuffer(Result.Category, Source.Category);
	FTextStringHelper::WriteToBuffer(Result.Tooltip, Source.Tooltip);
	Result.bExposedOnSpawn = Source.bExposedOnSpawn;
	Result.bExposeToCinematics = Source.bExposeToCinematics;
	Result.bPublic = Source.bPublic;
	Result.bPrivate = Source.bPrivate;
	return Result;
}

FRigExternalVariableAST ExportExternalVariable(const FRigVMExternalVariable& Source)
{
	FRigExternalVariableAST Result;
	Result.Guid = Source.GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Result.Name = Source.GetName().ToString();
	Result.Type.CPPType = Source.GetBaseCPPType().ToString();
	Result.Type.CPPTypeObject = Source.GetCPPTypeObject()
		? Source.GetCPPTypeObject()->GetPathName()
		: FString();
	Result.Type.ContainerType = Source.IsArray() ? TEXT("array") : FString();
	Result.Type.Canonicalize();
	Result.bPublic = Source.IsPublic();
	Result.bReadOnly = Source.IsReadOnly();
	return Result;
}

FString RelativePinPath(const URigVMPin* Pin)
{
	FString Path = Pin->GetPinPath(false);
	if (const URigVMNode* Node = Pin->GetNode())
	{
		const FString Prefix = Node->GetNodePath(false) + TEXT(".");
		Path.RemoveFromStart(Prefix);
	}
	return Path;
}

FString PinCoverageId(const URigVMPin* Pin, const FString& ModelId)
{
	return ModelId + TEXT("|") + Pin->GetPinPath(true);
}

FRigPinAST ExportPin(const URigVMPin* Pin, const FString& ModelId, FRigLangExportCoverage& Coverage)
{
	FRigPinAST Result;
	Result.Path = RelativePinPath(Pin);
	Result.Direction = ExportDirection(Pin->GetDirection());
	Result.Type.CPPType = Pin->GetCPPType();
	if (const UObject* TypeObject = Pin->GetCPPTypeObject())
	{
		Result.Type.CPPTypeObject = TypeObject->GetPathName();
	}
	Result.Type.ContainerType = Pin->IsArray() ? TEXT("array") : FString();
	Result.Type.Canonicalize();
	Result.DefaultValue = Pin->GetDefaultValue();
	Result.bExecuteContext = Pin->IsExecuteContext();
	const FString PinId = PinCoverageId(Pin, ModelId);
	Coverage.VisitedPins.Add(PinId);
	Coverage.Reasons.Add(PinId, TEXT("exact: RigVM pin API"));
	for (const URigVMPin* SubPin : Pin->GetSubPins())
	{
		if (SubPin)
		{
			Result.SubPins.Add(ExportPin(SubPin, ModelId, Coverage));
		}
	}
	return Result;
}

FRigNodeAST ExportNode(
	const URigVMNode* Node,
	const FString& ModelId,
	const TMap<const URigVMNode*, FGuid>& EditorNodeGuids,
	FRigLangExportCoverage& Coverage)
{
	FRigNodeAST Result;
	Result.StableId = Node->GetName();
	if (const FGuid* EditorGuid = EditorNodeGuids.Find(Node); EditorGuid && EditorGuid->IsValid())
	{
		Result.Guid = EditorGuid->ToString(EGuidFormats::DigitsWithHyphensLower);
	}
	else
	{
		Result.Guid = TEXT("model:") + Node->GetNodePath(true);
	}
	Result.ClassPath = Node->GetClass()->GetPathName();
	Result.EventName = Node->GetEventName().ToString();
	Result.bInjected = Node->IsInjected();
	Result.Coverage = ERigNodeCoverage::Exact;
	if (const URigVMAggregateNode* AggregateNode = Cast<URigVMAggregateNode>(Node))
	{
		Result.Kind = ERigNodeKind::Aggregate;
		Result.MethodName = AggregateNode->GetMethodName().ToString();
		Result.Properties.Add(TEXT("input-aggregate"), AggregateNode->IsInputAggregate() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("first-inner-node"), QuoteRigLangValue(
			AggregateNode->GetFirstInnerNode() ? AggregateNode->GetFirstInnerNode()->GetNodePath(true) : FString()));
		Result.Properties.Add(TEXT("last-inner-node"), QuoteRigLangValue(
			AggregateNode->GetLastInnerNode() ? AggregateNode->GetLastInnerNode()->GetNodePath(true) : FString()));
	}
	else if (const URigVMDispatchNode* DispatchNode = Cast<URigVMDispatchNode>(Node))
	{
		Result.Kind = ERigNodeKind::Dispatch;
		const FRigVMDispatchFactory* Factory = DispatchNode->GetFactory();
		Result.Properties.Add(TEXT("dispatch-factory"), QuoteRigLangValue(
			Factory ? Factory->GetFactoryName().ToString() : FString()));
		Result.Properties.Add(TEXT("dispatch-script-struct"), QuoteRigLangValue(
			Factory && Factory->GetScriptStruct() ? Factory->GetScriptStruct()->GetPathName() : FString()));
	}
	else if (const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Node))
	{
		Result.Kind = ERigNodeKind::Variable;
		Result.Properties.Add(TEXT("variable-name"), QuoteRigLangValue(VariableNode->GetVariableName().ToString()));
		Result.Properties.Add(TEXT("variable-guid"), QuoteRigLangValue(
			VariableNode->GetVariableGuid().ToString(EGuidFormats::DigitsWithHyphensLower)));
		Result.Properties.Add(TEXT("getter"), VariableNode->IsGetter() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("external"), VariableNode->IsExternalVariable() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("local"), VariableNode->IsLocalVariable() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("variable-cpp-type"), QuoteRigLangValue(VariableNode->GetCPPType()));
		Result.Properties.Add(TEXT("variable-cpp-type-object"), QuoteRigLangValue(
			VariableNode->GetCPPTypeObject() ? VariableNode->GetCPPTypeObject()->GetPathName() : FString()));
		Result.Properties.Add(TEXT("variable-default"), QuoteRigLangValue(VariableNode->GetDefaultValue()));
	}
	else if (const URigVMCommentNode* CommentNode = Cast<URigVMCommentNode>(Node))
	{
		Result.Kind = ERigNodeKind::Comment;
		Result.Coverage = ERigNodeCoverage::Reflected;
		Result.Properties.Add(TEXT("comment-text"), QuoteRigLangValue(CommentNode->GetCommentText()));
		Result.Properties.Add(TEXT("font-size"), FString::FromInt(CommentNode->GetCommentFontSize()));
		Result.Properties.Add(TEXT("bubble-visible"), CommentNode->GetCommentBubbleVisible() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("color-bubble"), CommentNode->GetCommentColorBubble() ? TEXT("true") : TEXT("false"));
	}
	else if (const URigVMRerouteNode* RerouteNode = Cast<URigVMRerouteNode>(Node))
	{
		Result.Kind = ERigNodeKind::Reroute;
		Result.Properties.Add(TEXT("literal"), RerouteNode->IsLiteral() ? TEXT("true") : TEXT("false"));
	}
	else if (Cast<URigVMFunctionEntryNode>(Node)) Result.Kind = ERigNodeKind::Entry;
	else if (Cast<URigVMFunctionReturnNode>(Node)) Result.Kind = ERigNodeKind::Return;
	else if (const URigVMInvokeEntryNode* InvokeNode = Cast<URigVMInvokeEntryNode>(Node))
	{
		Result.Kind = ERigNodeKind::InvokeEntry;
		Result.Properties.Add(TEXT("entry-name"), QuoteRigLangValue(InvokeNode->GetEntryName().ToString()));
	}
	else if (const URigVMCollapseNode* CollapseNode = Cast<URigVMCollapseNode>(Node))
	{
		Result.Kind = ERigNodeKind::Collapse;
		TArray<FString> PinGuids;
		for (const URigVMPin* Pin : CollapseNode->GetPins())
		{
			if (Pin) PinGuids.Add(TEXT("(") + QuoteRigLangValue(Pin->GetName()) + TEXT(" ")
				+ QuoteRigLangValue(CollapseNode->FindPinGuid(Pin).ToString(EGuidFormats::DigitsWithHyphensLower)) + TEXT(")"));
		}
		Result.Properties.Add(TEXT("interface-pin-guids"), TEXT("(") + FString::Join(PinGuids, TEXT(" ")) + TEXT(")"));
		TArray<FString> ExternalVariables;
		for (const FRigVMExternalVariable& Variable : CollapseNode->GetExternalVariables())
		{
			ExternalVariables.Add(TEXT("(") + QuoteRigLangValue(Variable.GetName().ToString()) + TEXT(" ")
				+ QuoteRigLangValue(Variable.GetExtendedCPPType().ToString()) + TEXT(")"));
		}
		Result.Properties.Add(TEXT("external-variables"), TEXT("(") + FString::Join(ExternalVariables, TEXT(" ")) + TEXT(")"));
	}
	else if (const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
	{
		Result.Coverage = ERigNodeCoverage::Exact;
		Result.MethodName = UnitNode->GetMethodName().ToString();
		if (const UScriptStruct* ScriptStruct = UnitNode->GetScriptStruct())
		{
			Result.Properties.Add(TEXT("script-struct"), QuoteRigLangValue(ScriptStruct->GetPathName()));
		}
	}
	else if (const URigVMFunctionReferenceNode* FunctionNode = Cast<URigVMFunctionReferenceNode>(Node))
	{
		Result.Kind = ERigNodeKind::Call;
		Result.Coverage = ERigNodeCoverage::Exact;
		const FRigVMGraphFunctionHeader Header = FunctionNode->GetReferencedFunctionHeader();
		Result.FunctionName = StableRuntimeSymbol(Header.Name.ToString());
		Result.FunctionIdentifier = ExportFunctionIdentifier(Header.LibraryPointer);
		Result.Properties.Add(TEXT("requires-variable-remapping"),
			FunctionNode->RequiresVariableRemapping() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("fully-remapped"),
			FunctionNode->IsFullyRemapped() ? TEXT("true") : TEXT("false"));
		TArray<FString> VariableMappings;
		for (const TPair<FName, FName>& Mapping : FunctionNode->GetVariableMap())
		{
			VariableMappings.Add(TEXT("(") + QuoteRigLangValue(Mapping.Key.ToString()) + TEXT(" ")
				+ QuoteRigLangValue(Mapping.Value.ToString()) + TEXT(")"));
		}
		VariableMappings.Sort();
		Result.Properties.Add(TEXT("variable-remapping"),
			TEXT("(") + FString::Join(VariableMappings, TEXT(" ")) + TEXT(")"));
	}
	else
	{
		Result.Coverage = ERigNodeCoverage::Unsupported;
		Result.Properties.Add(TEXT("unsupported-reason"), QuoteRigLangValue(TEXT("unknown RigVM node subclass")));
	}
	if (const URigVMLibraryNode* LibraryNode = Cast<URigVMLibraryNode>(Node))
	{
		if (const URigVMGraph* ContainedGraph = LibraryNode->GetContainedGraph())
		{
			Result.ContainedGraphStableId = ContainedGraph->GetPathName();
		}
	}
	if (const URigVMTemplateNode* TemplateNode = Cast<URigVMTemplateNode>(Node))
	{
		Result.Properties.Add(TEXT("template-notation"), QuoteRigLangValue(TemplateNode->GetNotation().ToString()));
		Result.Properties.Add(TEXT("template-resolved"), TemplateNode->IsResolved() ? TEXT("true") : TEXT("false"));
		Result.Properties.Add(TEXT("resolved-function"), QuoteRigLangValue(
			TemplateNode->GetResolvedFunction() ? TemplateNode->GetResolvedFunction()->Name : FString()));
		Result.Properties.Add(TEXT("template-types"), ExportTemplateTypeMap(TemplateNode->GetTemplatePinTypeMap(true, true)));
	}
	for (const URigVMPin* Pin : Node->GetPins())
	{
		if (Pin)
		{
			Result.Pins.Add(ExportPin(Pin, ModelId, Coverage));
		}
	}
	return Result;
}

struct FInjectedNodeRecord
{
	const URigVMNode* Node = nullptr;
	const URigVMPin* OwnerPin = nullptr;
	const URigVMInjectionInfo* Info = nullptr;
	int32 Order = INDEX_NONE;
};

TArray<FInjectedNodeRecord> GatherInjectedNodes(const URigVMGraph* Graph)
{
	TArray<FInjectedNodeRecord> Records;
	TSet<const URigVMNode*> Seen;
	TFunction<void(const URigVMNode*)> VisitNode = [&](const URigVMNode* Node)
	{
		if (!Node) return;
		for (const URigVMPin* Pin : Node->GetAllPinsRecursively())
		{
			if (!Pin) continue;
			const TArray<URigVMInjectionInfo*> Infos = Pin->GetInjectedNodes();
			for (int32 Index = 0; Index < Infos.Num(); ++Index)
			{
				const URigVMInjectionInfo* Info = Infos[Index];
				if (!Info || !Info->Node || Seen.Contains(Info->Node)) continue;
				Seen.Add(Info->Node);
				Records.Add({Info->Node, Pin, Info, Index});
				VisitNode(Info->Node);
			}
		}
	};
	for (const URigVMNode* Node : Graph->GetNodes()) VisitNode(Node);
	return Records;
}

FRigGraphAST ExportGraph(
	const URigVMGraph* Graph,
	const TMap<const URigVMNode*, FGuid>& EditorNodeGuids,
	FRigLangExportCoverage& Coverage)
{
	FRigGraphAST Result;
	const FString ModelId = Graph->GetPathName();
	const TArray<FInjectedNodeRecord> InjectedRecords = GatherInjectedNodes(Graph);
	TSet<const URigVMNode*> InjectedNodes;
	for (const FInjectedNodeRecord& Record : InjectedRecords)
	{
		if (Record.Node) InjectedNodes.Add(Record.Node);
	}
	for (const URigVMNode* Node : Graph->GetNodes())
	{
		if (!Node || InjectedNodes.Contains(Node))
		{
			continue;
		}
		Result.Nodes.Add(ExportNode(Node, ModelId, EditorNodeGuids, Coverage));
		const FString NodeId = Node->GetPathName();
		Coverage.VisitedNodes.Add(NodeId);
		const FRigNodeAST& Exported = Result.Nodes.Last();
		if (Exported.Coverage == ERigNodeCoverage::Lossy || Exported.Coverage == ERigNodeCoverage::Unsupported)
			Coverage.LossyOrUnsupportedNodes.Add(NodeId);
		FString Reason;
		if (Exported.Kind == ERigNodeKind::Comment)
			Reason = TEXT("normalized: editor-only comment excluded from semantic hash");
		else if (Exported.Coverage == ERigNodeCoverage::Unsupported)
			Reason = TEXT("unsupported: no typed RigVM reconstruction contract for node subclass");
		else if (Exported.Coverage == ERigNodeCoverage::Lossy)
			Reason = TEXT("lossy: typed RigVM reconstruction contract is incomplete");
		else if (Exported.Coverage == ERigNodeCoverage::Reflected)
			Reason = TEXT("reflected: complete reflected RigVM reconstruction properties");
		else
			Reason = TEXT("exact: typed RigVM node reconstruction API");
		Coverage.Reasons.Add(NodeId, MoveTemp(Reason));
	}
	for (const FInjectedNodeRecord& Record : InjectedRecords)
	{
		FRigNodeAST Injected = ExportNode(Record.Node, ModelId, EditorNodeGuids, Coverage);
		Injected.bInjected = true;
		Injected.InjectionOwnerPin = Record.OwnerPin ? Record.OwnerPin->GetPinPath(true) : FString();
		Injected.InjectionOrder = Record.Order;
		Injected.bInjectedAsInput = Record.Info->bInjectedAsInput;
		Injected.InjectionInputPin = Record.Info->InputPin ? RelativePinPath(Record.Info->InputPin) : FString();
		Injected.InjectionOutputPin = Record.Info->OutputPin ? RelativePinPath(Record.Info->OutputPin) : FString();
		const FString NodeId = Record.Node->GetPathName();
		if (Injected.Coverage == ERigNodeCoverage::Lossy || Injected.Coverage == ERigNodeCoverage::Unsupported)
			Coverage.LossyOrUnsupportedNodes.Add(NodeId);
		Coverage.VisitedNodes.Add(NodeId);
		Coverage.Reasons.Add(NodeId, TEXT("exact: injected node with owner, order, direction, and exposed pins"));
		Result.Nodes.Add(MoveTemp(Injected));
	}
	Result.Nodes.Sort([](const FRigNodeAST& A, const FRigNodeAST& B)
	{
		if (A.Guid != B.Guid) return A.Guid < B.Guid;
		return A.StableId < B.StableId;
	});

	TArray<URigVMLink*> Links = Graph->GetLinks();
	Links.Sort([](const URigVMLink& A, const URigVMLink& B)
	{
		return A.GetPinPathRepresentation() < B.GetPinPathRepresentation();
	});
	for (const URigVMLink* Link : Links)
	{
		if (!Link || !Link->GetSourceNode() || !Link->GetTargetNode()
			|| !Link->GetSourcePin() || !Link->GetTargetPin())
		{
			continue;
		}
		FRigLinkAST ExportedLink;
		ExportedLink.SourceNodeId = Link->GetSourceNode()->GetName();
		ExportedLink.SourcePinPath = RelativePinPath(Link->GetSourcePin());
		ExportedLink.TargetNodeId = Link->GetTargetNode()->GetName();
		ExportedLink.TargetPinPath = RelativePinPath(Link->GetTargetPin());
		Result.Links.Add(MoveTemp(ExportedLink));
		Coverage.ConnectedPins.Add(PinCoverageId(Link->GetSourcePin(), ModelId));
		Coverage.ConnectedPins.Add(PinCoverageId(Link->GetTargetPin(), ModelId));
		const FString LinkId = ModelId + TEXT("|") + Link->GetPinPathRepresentation();
		Coverage.VisitedLinks.Add(LinkId);
		Coverage.Reasons.Add(LinkId, TEXT("exact: RigVM link API"));
	}
	return Result;
}
}

void FRigLangExporter::NormalizeFunctionCallsAndImports(FRigModuleAST& Module)
{
	TMap<FString, const FRigFunctionAST*> LocalFunctionsByIdentifier;
	for (const FRigFunctionAST& Function : Module.Functions)
	{
		if (Function.FunctionIdentifier.IsComplete())
		{
			LocalFunctionsByIdentifier.FindOrAdd(Function.FunctionIdentifier.ToStableId(), &Function);
		}
	}

	TArray<FRigNodeAST*> Calls;
	auto CollectCalls = [&Calls](FRigGraphAST& Graph)
	{
		for (FRigNodeAST& Node : Graph.Nodes)
		{
			if (Node.Kind == ERigNodeKind::Call && Node.FunctionIdentifier.IsComplete()) Calls.Add(&Node);
		}
	};
	for (FRigGraphAST& Graph : Module.Graphs) CollectCalls(Graph);
	for (FRigFunctionAST& Function : Module.Functions) CollectCalls(Function.Graph);
	for (FRigEntryAST& Entry : Module.Entries) CollectCalls(Entry.Graph);

	TSet<FString> ExternalAssets;
	for (FRigNodeAST* Node : Calls)
	{
		if (const FRigFunctionAST* const* Local =
			LocalFunctionsByIdentifier.Find(Node->FunctionIdentifier.ToStableId()))
		{
			Node->FunctionName = (*Local)->Name;
		}
		else
		{
			ExternalAssets.Add(FunctionIdentifierModuleAssetPath(Node->FunctionIdentifier));
		}
	}

	TMap<FString, FString> UsedAliasTargets;
	TMap<FString, TArray<FString>> ExistingAliasesByAsset;
	for (const FRigImportAST& Import : Module.Imports)
	{
		UsedAliasTargets.FindOrAdd(Import.Import.Alias, Import.Import.Target.AssetPath);
		ExistingAliasesByAsset.FindOrAdd(Import.Import.Target.AssetPath).AddUnique(Import.Import.Alias);
	}
	for (TPair<FString, TArray<FString>>& Pair : ExistingAliasesByAsset) Pair.Value.Sort();

	TArray<FString> SortedExternalAssets = ExternalAssets.Array();
	SortedExternalAssets.Sort();
	TMap<FString, int32> AliasBaseCounts;
	for (const FString& AssetPath : SortedExternalAssets)
	{
		++AliasBaseCounts.FindOrAdd(StableImportAliasBase(AssetPath));
	}

	TMap<FString, FString> AliasByAsset;
	for (const FString& AssetPath : SortedExternalAssets)
	{
		if (const TArray<FString>* ExistingAliases = ExistingAliasesByAsset.Find(AssetPath);
			ExistingAliases && !ExistingAliases->IsEmpty())
		{
			AliasByAsset.Add(AssetPath, (*ExistingAliases)[0]);
			continue;
		}

		const FString Base = StableImportAliasBase(AssetPath);
		FString Alias = Base;
		const FString* ExistingTarget = UsedAliasTargets.Find(Alias);
		if (AliasBaseCounts.FindRef(Base) > 1 || (ExistingTarget && *ExistingTarget != AssetPath))
		{
			const FString Digest = SHA256UTF8(AssetPath);
			int32 SuffixLength = 8;
			do
			{
				Alias = Base + TEXT("_") + Digest.Left(SuffixLength);
				SuffixLength = FMath::Min(SuffixLength + 4, Digest.Len());
				ExistingTarget = UsedAliasTargets.Find(Alias);
			}
			while (ExistingTarget && *ExistingTarget != AssetPath && SuffixLength < Digest.Len());
			for (int32 Collision = 2; ExistingTarget && *ExistingTarget != AssetPath; ++Collision)
			{
				Alias = Base + TEXT("_") + Digest + TEXT("_") + FString::FromInt(Collision);
				ExistingTarget = UsedAliasTargets.Find(Alias);
			}
		}
		UsedAliasTargets.Add(Alias, AssetPath);
		AliasByAsset.Add(AssetPath, Alias);
		FRigImportAST& Import = Module.Imports.AddDefaulted_GetRef();
		Import.Import.Target = FAnimLispModuleId::FromAssetPath(AssetPath, EAnimLispModuleKind::Rig);
		Import.Import.Alias = Alias;
	}

	for (FRigNodeAST* Node : Calls)
	{
		if (LocalFunctionsByIdentifier.Contains(Node->FunctionIdentifier.ToStableId())) continue;
		const FString AssetPath = FunctionIdentifierModuleAssetPath(Node->FunctionIdentifier);
		if (const FString* Alias = AliasByAsset.Find(AssetPath))
		{
			Node->FunctionName = *Alias + TEXT("/")
				+ RigFunctionSymbolFromLibraryNodePath(Node->FunctionIdentifier.LibraryNodePath);
		}
	}
	for (FRigGraphAST& Graph : Module.Graphs)
	{
		Graph.Nodes.Sort([](const FRigNodeAST& A, const FRigNodeAST& B)
		{
			if (A.Guid != B.Guid) return A.Guid < B.Guid;
			return A.StableId < B.StableId;
		});
		Graph.Links.Sort([](const FRigLinkAST& A, const FRigLinkAST& B)
		{
			if (A.SourceNodeId != B.SourceNodeId) return A.SourceNodeId < B.SourceNodeId;
			if (A.SourcePinPath != B.SourcePinPath) return A.SourcePinPath < B.SourcePinPath;
			if (A.TargetNodeId != B.TargetNodeId) return A.TargetNodeId < B.TargetNodeId;
			return A.TargetPinPath < B.TargetPinPath;
		});
	}
	Module.Graphs.Sort([](const FRigGraphAST& A, const FRigGraphAST& B)
	{
		return A.StableId < B.StableId;
	});
}

FRigLangExportResult FRigLangExporter::Export(
	UControlRigBlueprint* Blueprint,
	const FRigLangExportOptions& Options)
{
	FRigLangExportResult Result;
	if (!Blueprint)
	{
		Result.Errors.Add(TEXT("Control Rig blueprint is null"));
		return Result;
	}

	Result.Module = MakeShared<FRigModuleAST>();
	Result.Module->Header.ModuleId = FAnimLispModuleId::FromAssetPath(
		Blueprint->GetOutermost()->GetName(), EAnimLispModuleKind::Rig);
	Result.Module->Header.AssetClassPath = Blueprint->GetClass()->GetPathName();
	Result.Module->Header.Version = 1;

	TMap<const URigVMNode*, FGuid> EditorNodeGuids;
	TMap<const URigVMGraph*, FGuid> EditorGraphGuids;
	TArray<UEdGraph*> EditorGraphs;
	Blueprint->GetAllGraphs(EditorGraphs);
	for (const UEdGraph* EditorGraph : EditorGraphs)
	{
		if (!EditorGraph)
		{
			continue;
		}
		if (const URigVMEdGraph* RigVMEditorGraph = Cast<URigVMEdGraph>(EditorGraph))
		{
			if (const URigVMGraph* Model = RigVMEditorGraph->GetModel())
			{
				EditorGraphGuids.Add(Model, RigVMEditorGraph->GraphGuid);
			}
		}
		for (const UEdGraphNode* EditorNode : EditorGraph->Nodes)
		{
			if (const URigVMEdGraphNode* RigVMEditorNode = Cast<URigVMEdGraphNode>(EditorNode))
			{
				if (const URigVMNode* ModelNode = RigVMEditorNode->GetModelNode())
				{
					EditorNodeGuids.Add(ModelNode, RigVMEditorNode->NodeGuid);
				}
			}
		}
	}

	if (const URigHierarchy* Hierarchy = Blueprint->GetHierarchy())
	{
		for (const FRigElementKey& Key : TopologicallySortHierarchy(Hierarchy, Result.Errors))
		{
			FRigHierarchyElementAST Element;
			Element.StableId = Key.ToString();
			Element.Name = Key.Name.ToString();
			switch (Key.Type)
			{
			case ERigElementType::Bone: Element.Kind = ERigHierarchyElementKind::Bone; break;
			case ERigElementType::Control: Element.Kind = ERigHierarchyElementKind::Control; break;
			case ERigElementType::Null: Element.Kind = ERigHierarchyElementKind::Null; break;
			case ERigElementType::Curve: Element.Kind = ERigHierarchyElementKind::Curve; break;
			default:
				Result.Errors.Add(FString::Printf(TEXT("Unsupported hierarchy element type for '%s'"), *Key.ToString()));
				continue;
			}
			const FRigElementKey FirstParent = Hierarchy->GetFirstParent(Key);
			Element.ParentName = FirstParent.IsValid() ? FirstParent.Name.ToString() : FString();

			TArray<FString> ParentValues;
			const TArray<FRigElementKey> SourceParents = Hierarchy->GetParents(Key, false);
			const FRigMultiParentElement* MultiParent = Hierarchy->Find<FRigMultiParentElement>(Key);
			for (int32 ParentIndex = 0; ParentIndex < SourceParents.Num(); ++ParentIndex)
			{
				const FRigElementKey& ParentKey = SourceParents[ParentIndex];
				if (!ParentKey.IsValid()) continue;
				ParentValues.Add(QuoteRigLangValue(ParentKey.ToString()));
				FRigHierarchyParentAST& Parent = Element.Parents.AddDefaulted_GetRef();
				Parent.StableId = ParentKey.ToString();
				const FRigElementWeight CurrentWeight = Hierarchy->GetParentWeight(Key, ParentKey, false);
				const FRigElementWeight InitialWeight = Hierarchy->GetParentWeight(Key, ParentKey, true);
				Parent.CurrentWeight.Location = CurrentWeight.Location;
				Parent.CurrentWeight.Rotation = CurrentWeight.Rotation;
				Parent.CurrentWeight.Scale = CurrentWeight.Scale;
				Parent.InitialWeight.Location = InitialWeight.Location;
				Parent.InitialWeight.Rotation = InitialWeight.Rotation;
				Parent.InitialWeight.Scale = InitialWeight.Scale;
				if (MultiParent && MultiParent->ParentConstraints.IsValidIndex(ParentIndex))
					Parent.Label = MultiParent->ParentConstraints[ParentIndex].DisplayLabel.ToString();
			}
			Element.Properties.Add(TEXT("parents"), TEXT("(") + FString::Join(ParentValues, TEXT(" ")) + TEXT(")"));
			Element.Properties.Add(TEXT("parent-weights-current"),
				ExportParentWeights(Hierarchy->GetParentWeightArray(Key, false)));
			Element.Properties.Add(TEXT("parent-weights-initial"),
				ExportParentWeights(Hierarchy->GetParentWeightArray(Key, true)));
			TArray<FString> ParentLabels;
			if (MultiParent)
			{
				for (const FRigElementParentConstraint& Constraint : MultiParent->ParentConstraints)
					ParentLabels.Add(QuoteRigLangValue(Constraint.DisplayLabel.ToString()));
			}
			Element.Properties.Add(TEXT("parent-labels"),
				TEXT("(") + FString::Join(ParentLabels, TEXT(" ")) + TEXT(")"));
			const FTransform InitialLocalTransform = Hierarchy->GetInitialLocalTransform(Key);
			const FTransform InitialGlobalTransform = Hierarchy->GetInitialGlobalTransform(Key);
			Element.Properties.Add(TEXT("initial-local-transform"), ExportTransformProperty(InitialLocalTransform));
			Element.Properties.Add(TEXT("initial-global-transform"), ExportTransformProperty(InitialGlobalTransform));
			Element.Properties.Add(TEXT("current-local-transform"),
				ExportTransformProperty(Hierarchy->GetLocalTransform(Key, false)));
			Element.Properties.Add(TEXT("current-global-transform"),
				ExportTransformProperty(Hierarchy->GetGlobalTransform(Key, false)));
			AddHierarchyTransform(Element, ERigHierarchyTransformRole::InitialLocal, InitialLocalTransform);
			AddHierarchyTransform(Element, ERigHierarchyTransformRole::InitialGlobal, InitialGlobalTransform);
			AddHierarchyTransform(Element, ERigHierarchyTransformRole::CurrentLocal, Hierarchy->GetLocalTransform(Key, false));
			AddHierarchyTransform(Element, ERigHierarchyTransformRole::CurrentGlobal, Hierarchy->GetGlobalTransform(Key, false));

			if (const FRigBoneElement* Bone = Hierarchy->Find<FRigBoneElement>(Key))
			{
				Element.Properties.Add(TEXT("bone-type"), StaticEnum<ERigBoneType>()->GetNameStringByValue(
					static_cast<int64>(Bone->BoneType)));
				FRigHierarchyStateAST& State = Element.States.AddDefaulted_GetRef(); State.Kind = ERigHierarchyStateKind::BoneType;
				State.Role = TEXT("initial"); State.Type = StaticEnum<ERigBoneType>()->GetNameStringByValue(static_cast<int64>(Bone->BoneType));
			}
			if (Key.Type == ERigElementType::Curve)
			{
				Element.Properties.Add(TEXT("curve-value"), FString::SanitizeFloat(Hierarchy->GetCurveValue(Key)));
				Element.Properties.Add(TEXT("curve-value-set"),
					Hierarchy->IsCurveValueSet(Key) ? TEXT("true") : TEXT("false"));
				FRigHierarchyStateAST& State = Element.States.AddDefaulted_GetRef(); State.Kind = ERigHierarchyStateKind::Curve;
				State.Role = TEXT("initial"); State.Type = TEXT("Float"); State.NumberValue = Hierarchy->GetCurveValue(Key);
				State.bBoolValue = Hierarchy->IsCurveValueSet(Key);
			}

			if (const FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key))
			{
				FRigHierarchyStateAST& SettingsState = Element.States.AddDefaulted_GetRef(); SettingsState.Kind = ERigHierarchyStateKind::ControlSettings;
				SettingsState.Role = TEXT("initial"); SettingsState.Type = StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Control->Settings.ControlType));
				SettingsState.SerializedValue = ExportStructText(FRigControlSettings::StaticStruct(), &Control->Settings);
				Element.States.Add(ExportControlState(Hierarchy->GetControlValue(Key, ERigControlValueType::Current), Control->Settings.ControlType, TEXT("current")));
				Element.States.Add(ExportControlState(Hierarchy->GetControlValue(Key, ERigControlValueType::Initial), Control->Settings.ControlType, TEXT("initial")));
				Element.States.Add(ExportControlState(Hierarchy->GetControlValue(Key, ERigControlValueType::Minimum), Control->Settings.ControlType, TEXT("minimum")));
				Element.States.Add(ExportControlState(Hierarchy->GetControlValue(Key, ERigControlValueType::Maximum), Control->Settings.ControlType, TEXT("maximum")));
				Element.Properties.Add(TEXT("control-settings"), QuoteRigLangValue(
					ExportStructText(FRigControlSettings::StaticStruct(), &Control->Settings)));
				Element.Properties.Add(TEXT("control-value-current"), ExportTypedControlValue(
					Hierarchy->GetControlValue(Key, ERigControlValueType::Current), Control->Settings.ControlType));
				Element.Properties.Add(TEXT("control-value-initial"), ExportTypedControlValue(
					Hierarchy->GetControlValue(Key, ERigControlValueType::Initial), Control->Settings.ControlType));
				Element.Properties.Add(TEXT("control-value-minimum"), ExportTypedControlValue(
					Hierarchy->GetControlValue(Key, ERigControlValueType::Minimum), Control->Settings.ControlType));
				Element.Properties.Add(TEXT("control-value-maximum"), ExportTypedControlValue(
					Hierarchy->GetControlValue(Key, ERigControlValueType::Maximum), Control->Settings.ControlType));
				Element.Properties.Add(TEXT("control-pose-current-local"),
					ExportTransformProperty(Hierarchy->GetLocalTransform(Key, false)));
				Element.Properties.Add(TEXT("control-pose-current-global"),
					ExportTransformProperty(Hierarchy->GetGlobalTransform(Key, false)));
				Element.Properties.Add(TEXT("control-pose-initial-local"),
					ExportTransformProperty(Hierarchy->GetLocalTransform(Key, true)));
				Element.Properties.Add(TEXT("control-pose-initial-global"),
					ExportTransformProperty(Hierarchy->GetGlobalTransform(Key, true)));
				Element.Properties.Add(TEXT("control-offset-current-local"), ExportTransformProperty(
					Control->GetOffsetTransform()[ERigTransformType::CurrentLocal].Get()));
				Element.Properties.Add(TEXT("control-offset-current-global"), ExportTransformProperty(
					Hierarchy->GetGlobalControlOffsetTransform(Key, false)));
				Element.Properties.Add(TEXT("control-offset-initial-local"), ExportTransformProperty(
					Control->GetOffsetTransform()[ERigTransformType::InitialLocal].Get()));
				Element.Properties.Add(TEXT("control-offset-initial-global"), ExportTransformProperty(
					Hierarchy->GetGlobalControlOffsetTransform(Key, true)));
				Element.Properties.Add(TEXT("control-shape-current-local"), ExportTransformProperty(
					Hierarchy->GetLocalControlShapeTransform(Key, false)));
				Element.Properties.Add(TEXT("control-shape-current-global"), ExportTransformProperty(
					Hierarchy->GetGlobalControlShapeTransform(Key, false)));
				Element.Properties.Add(TEXT("control-shape-initial-local"), ExportTransformProperty(
					Hierarchy->GetLocalControlShapeTransform(Key, true)));
				Element.Properties.Add(TEXT("control-shape-initial-global"), ExportTransformProperty(
					Hierarchy->GetGlobalControlShapeTransform(Key, true)));
				Element.Properties.Add(TEXT("preferred-euler-order"), StaticEnum<EEulerRotationOrder>()->GetNameStringByValue(
					static_cast<int64>(Control->PreferredEulerAngles.RotationOrder)));
				Element.Properties.Add(TEXT("preferred-euler-current"), QuoteRigLangValue(ExportStructText(
					TBaseStructure<FVector>::Get(), &Control->PreferredEulerAngles.Current)));
				Element.Properties.Add(TEXT("preferred-euler-initial"), QuoteRigLangValue(ExportStructText(
					TBaseStructure<FVector>::Get(), &Control->PreferredEulerAngles.Initial)));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::PoseCurrentLocal, Hierarchy->GetLocalTransform(Key, false));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::PoseCurrentGlobal, Hierarchy->GetGlobalTransform(Key, false));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::PoseInitialLocal, Hierarchy->GetLocalTransform(Key, true));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::PoseInitialGlobal, Hierarchy->GetGlobalTransform(Key, true));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::OffsetCurrentLocal, Control->GetOffsetTransform()[ERigTransformType::CurrentLocal].Get());
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::OffsetCurrentGlobal, Hierarchy->GetGlobalControlOffsetTransform(Key, false));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::OffsetInitialLocal, Control->GetOffsetTransform()[ERigTransformType::InitialLocal].Get());
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::OffsetInitialGlobal, Hierarchy->GetGlobalControlOffsetTransform(Key, true));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::ShapeCurrentLocal, Hierarchy->GetLocalControlShapeTransform(Key, false));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::ShapeCurrentGlobal, Hierarchy->GetGlobalControlShapeTransform(Key, false));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::ShapeInitialLocal, Hierarchy->GetLocalControlShapeTransform(Key, true));
				AddHierarchyTransform(Element, ERigHierarchyTransformRole::ShapeInitialGlobal, Hierarchy->GetGlobalControlShapeTransform(Key, true));
				FRigHierarchyStateAST& EulerCurrent = Element.States.AddDefaulted_GetRef(); EulerCurrent.Kind = ERigHierarchyStateKind::PreferredEuler;
				EulerCurrent.Role = TEXT("current"); EulerCurrent.Type = StaticEnum<EEulerRotationOrder>()->GetNameStringByValue(static_cast<int64>(Control->PreferredEulerAngles.RotationOrder));
				EulerCurrent.Components = { Control->PreferredEulerAngles.Current.X, Control->PreferredEulerAngles.Current.Y, Control->PreferredEulerAngles.Current.Z };
				FRigHierarchyStateAST EulerInitial = EulerCurrent; EulerInitial.Role = TEXT("initial");
				EulerInitial.Components = { Control->PreferredEulerAngles.Initial.X, Control->PreferredEulerAngles.Initial.Y, Control->PreferredEulerAngles.Initial.Z };
				Element.States.Add(MoveTemp(EulerInitial));
			}

			TArray<FString> MetadataValues;
			if (const FRigBaseElement* BaseElement = Hierarchy->Find(Key))
			{
				TArray<FName> MetadataNames = Hierarchy->GetMetadataNames(Key);
				MetadataNames.Sort(FNameLexicalLess());
				for (const FName MetadataName : MetadataNames)
				{
					const ERigMetadataType MetadataType = Hierarchy->GetMetadataType(Key, MetadataName);
					if (const FRigBaseMetadata* Metadata = Hierarchy->FindMetadataForElement(
						BaseElement, MetadataName, MetadataType))
					{
						Element.Metadata.Add(ExportHierarchyMetadata(Metadata));
						MetadataValues.Add(TEXT("(") + QuoteRigLangValue(MetadataName.ToString())
							+ TEXT(" ") + QuoteRigLangValue(StaticEnum<ERigMetadataType>()->GetNameStringByValue(
								static_cast<int64>(MetadataType)))
							+ TEXT(" ") + QuoteRigLangValue(ExportStructText(
								Metadata->GetMetadataStruct(), Metadata)) + TEXT(")"));
					}
				}
			}
			Element.Properties.Add(TEXT("metadata"), TEXT("(")
				+ FString::Join(MetadataValues, TEXT(" ")) + TEXT(")"));
			for (const TCHAR* PromotedKey : {
				TEXT("parents"), TEXT("parent-weights-current"), TEXT("parent-weights-initial"), TEXT("parent-labels"),
				TEXT("initial-local-transform"), TEXT("initial-global-transform"), TEXT("current-local-transform"), TEXT("current-global-transform"),
				TEXT("bone-type"), TEXT("curve-value"), TEXT("curve-value-set"), TEXT("control-settings"),
				TEXT("control-value-current"), TEXT("control-value-initial"), TEXT("control-value-minimum"), TEXT("control-value-maximum"),
				TEXT("control-pose-current-local"), TEXT("control-pose-current-global"), TEXT("control-pose-initial-local"), TEXT("control-pose-initial-global"),
				TEXT("control-offset-current-local"), TEXT("control-offset-current-global"), TEXT("control-offset-initial-local"), TEXT("control-offset-initial-global"),
				TEXT("control-shape-current-local"), TEXT("control-shape-current-global"), TEXT("control-shape-initial-local"), TEXT("control-shape-initial-global"),
				TEXT("preferred-euler-order"), TEXT("preferred-euler-current"), TEXT("preferred-euler-initial"), TEXT("metadata") })
			{
				Element.Properties.Remove(PromotedKey);
			}
			Result.Module->Hierarchy.Add(MoveTemp(Element));
		}
	}

	for (const FRigVMGraphVariableDescription& SourceVariable : Blueprint->GetMemberVariables())
	{
		FRigVariableAST Variable;
		Variable.StableId = SourceVariable.Guid.IsValid()
			? SourceVariable.Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
			: SourceVariable.Name.ToString();
		Variable.Name = SourceVariable.Name.ToString();
		Variable.Access = SourceVariable.bPublic ? ERigVariableAccess::PublicInput : ERigVariableAccess::Internal;
		Variable.Type.CPPType = SourceVariable.CPPType;
		Variable.Type.CPPTypeObject = SourceVariable.CPPTypeObject
			? SourceVariable.CPPTypeObject->GetPathName()
			: SourceVariable.CPPTypeObjectPath.ToString();
		Variable.Type.ContainerType = SourceVariable.ToExternalVariable().IsArray()
			? TEXT("array")
			: FString();
		Variable.Type.Canonicalize();
		Variable.DefaultValue = SourceVariable.DefaultValue;
		Result.Module->Variables.Add(MoveTemp(Variable));
	}

	TArray<URigVMGraph*> Models = Blueprint->GetAllModels();
	Models.Sort([](const URigVMGraph& A, const URigVMGraph& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	Result.Coverage.ModelTotal = Models.Num();
	TSet<FString> RigSymbols;
	for (const URigVMGraph* Model : Models)
	{
		if (!Model)
		{
			continue;
		}
		Result.Coverage.VisitedModels.Add(Model->GetPathName());
		const TArray<FInjectedNodeRecord> InjectedRecords = GatherInjectedNodes(Model);
		TSet<const URigVMNode*> UniqueNodes;
		for (const URigVMNode* Node : Model->GetNodes()) if (Node) UniqueNodes.Add(Node);
		for (const FInjectedNodeRecord& Record : InjectedRecords) if (Record.Node) UniqueNodes.Add(Record.Node);
		Result.Coverage.NodeTotal += UniqueNodes.Num();
		Result.Coverage.LinkTotal += Model->GetLinks().Num();
		for (const URigVMNode* Node : UniqueNodes)
		{
			if (Node)
			{
				Result.Coverage.PinTotal += Node->GetAllPinsRecursively().Num();
			}
		}
		Result.Coverage.Reasons.Add(Model->GetPathName(), TEXT("exact: RigVM model API"));
		FRigGraphAST Graph = ExportGraph(Model, EditorNodeGuids, Result.Coverage);
		Graph.StableId = Model->GetPathName();
		Graph.EditorGuid = EditorGraphGuids.Contains(Model) && EditorGraphGuids[Model].IsValid()
			? EditorGraphGuids[Model].ToString(EGuidFormats::DigitsWithHyphensLower)
			: ComputeDeterministicEditorGuid(Blueprint->GetPathName(), Model->GetPathName());
		if (Model == Blueprint->GetLocalFunctionLibrary())
		{
			Graph.Role = TEXT("function-library");
		}
		else if (const URigVMGraph* ParentGraph = Model->GetParentGraph())
		{
			Graph.Role = ParentGraph == Blueprint->GetLocalFunctionLibrary()
				? TEXT("function") : TEXT("node-contained");
		}
		else
		{
			Graph.Role = TEXT("root");
		}
		Graph.ParentStableId = Model->GetParentGraph()
			? Model->GetParentGraph()->GetPathName()
			: FString();
		for (const FRigVMGraphVariableDescription& LocalVariable : Model->GetLocalVariables(false))
		{
			Graph.LocalVariables.Add(ExportLocalVariable(LocalVariable));
		}
		Graph.Properties.Add(TEXT("graph-name"), QuoteRigLangValue(Model->GetGraphName()));
		const TArray<FName> EventNames = Model->GetEventNames();
		TArray<FString> ExportedEventNames;
		for (const FName EventName : EventNames)
		{
			ExportedEventNames.Add(QuoteRigLangValue(EventName.ToString()));
		}
		Graph.Properties.Add(TEXT("event-names"), TEXT("(")
			+ FString::Join(ExportedEventNames, TEXT(" ")) + TEXT(")"));
		Result.Module->Graphs.Add(MoveTemp(Graph));

		for (const FName EventName : EventNames)
		{
			FRigEntryAST Entry;
			Entry.StableId = Model->GetPathName() + TEXT("|event:") + EventName.ToString();
			Entry.Name = StableRuntimeSymbol(EventName.ToString());
			Entry.EventName = EventName.ToString();
			Entry.GraphStableId = Model->GetPathName();
			Entry.Properties.Add(TEXT("graph-name"), QuoteRigLangValue(Model->GetGraphName()));
			for (const URigVMNode* EventNode : Model->GetNodes())
			{
				if (!EventNode || EventNode->GetEventName() != EventName) continue;
				for (const URigVMPin* Pin : EventNode->GetPins())
				{
					if (!Pin) continue;
					FRigCallableArgumentAST Argument = ExportArgument(Pin);
					Entry.Arguments.Add(Argument);
					if (Argument.Direction == ERigPinDirection::Output
						|| Argument.Direction == ERigPinDirection::IO)
					{
						Entry.Outputs.Add(MoveTemp(Argument));
					}
					else Entry.Inputs.Add(MoveTemp(Argument));
				}
			}
			if (Entry.Name.IsEmpty())
			{
				Result.Errors.Add(TEXT("Rig event name cannot form a runtime symbol: ") + Entry.EventName);
				continue;
			}
			if (RigSymbols.Contains(Entry.Name))
			{
				Result.Errors.Add(TEXT("Duplicate exported rig entry symbol: ") + Entry.Name);
				continue;
			}
			RigSymbols.Add(Entry.Name);
			Result.Module->Entries.Add(MoveTemp(Entry));
		}
	}

	if (const URigVMFunctionLibrary* FunctionLibrary = Blueprint->GetLocalFunctionLibrary())
	{
		for (const URigVMLibraryNode* LibraryNode : FunctionLibrary->GetFunctions())
		{
			if (!LibraryNode || !LibraryNode->GetContainedGraph()) continue;
			const FRigVMGraphFunctionHeader Header = LibraryNode->GetFunctionHeader();
			FRigFunctionAST Function;
			Function.FunctionIdentifier = ExportFunctionIdentifier(Header.LibraryPointer);
			Function.StableId = FunctionIdentifierStableId(Header.LibraryPointer);
			Function.Name = StableRuntimeSymbol(Header.Name.ToString());
			Function.GraphStableId = LibraryNode->GetContainedGraph()->GetPathName();
			Function.Visibility = FunctionLibrary->IsFunctionPublic(Header.Name)
				? TEXT("public")
				: TEXT("internal");
			Function.ReturnCPPType = TEXT("void");
			if (Function.Name.IsEmpty())
			{
				Result.Errors.Add(TEXT("Rig function name cannot form a runtime symbol: ") + Header.Name.ToString());
				continue;
			}
			if (RigSymbols.Contains(Function.Name))
			{
				Result.Errors.Add(TEXT("Duplicate exported rig symbol: ") + Function.Name);
				continue;
			}
			RigSymbols.Add(Function.Name);
			Function.Properties.Add(TEXT("short-name"), QuoteRigLangValue(Header.Name.ToString()));
			for (const FRigVMGraphFunctionArgument& SourceArgument : Header.Arguments)
			{
				FRigCallableArgumentAST Argument = ExportArgument(SourceArgument);
				Function.Arguments.Add(Argument);
				if (Argument.Direction == ERigPinDirection::Output
					|| Argument.Direction == ERigPinDirection::IO)
				{
					Function.Outputs.Add(MoveTemp(Argument));
				}
				else Function.Inputs.Add(MoveTemp(Argument));
			}
			for (const FRigVMExternalVariable& ExternalVariable : LibraryNode->GetExternalVariables())
			{
				Function.ExternalVariables.Add(ExportExternalVariable(ExternalVariable));
			}
			for (const TPair<FRigVMGraphFunctionIdentifier, uint32>& Pair : LibraryNode->GetDependencies())
			{
				FRigFunctionDependencyAST& Dependency = Function.Dependencies.AddDefaulted_GetRef();
				Dependency.HostObject = Pair.Key.HostObject.ToString();
				Dependency.LibraryNodePath = Pair.Key.GetLibraryNodePath();
				Dependency.Hash = Pair.Value;
			}
			Function.Dependencies.Sort([](const FRigFunctionDependencyAST& A, const FRigFunctionDependencyAST& B)
			{
				return A.HostObject == B.HostObject
					? A.LibraryNodePath < B.LibraryNodePath
					: A.HostObject < B.HostObject;
			});
			Result.Module->Functions.Add(MoveTemp(Function));
		}
	}

	NormalizeFunctionCallsAndImports(*Result.Module);

	Result.Module->Header.ContentHash = ComputeContentHash(Result.Module->ToCanonicalHashInput());

	if (Options.bStrict)
	{
		Result.bSuccess = Result.Errors.IsEmpty()
			&& ValidateStrictCoverage(Result.Coverage, Result.Errors);
	}
	else
	{
		Result.bSuccess = Result.Errors.IsEmpty();
	}
	return Result;
}

#else

FRigLangExportResult FRigLangExporter::Export(
	UControlRigBlueprint* Blueprint,
	const FRigLangExportOptions& Options)
{
	FRigLangExportResult Result;
	Result.Errors.Add(TEXT("RigLang export requires an editor build"));
	return Result;
}

#endif
