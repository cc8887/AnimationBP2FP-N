// RigLangAST.cpp - Canonical RigLang printer and semantic comparison
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "RigLangAST.h"

namespace
{
FString Quote(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
	return TEXT("\"") + Escaped + TEXT("\"");
}

}

FString FRigFunctionIdentifierAST::ToStableId() const
{
	return FString::Printf(TEXT("rigfn:v1:%d:%s:%d:%s"),
		HostObject.Len(), *HostObject, LibraryNodePath.Len(), *LibraryNodePath);
}

bool RigFunctionHostMatchesModule(const FString& HostObject, const FString& ModuleAssetPath)
{
	FString NormalizedHost = HostObject;
	if (NormalizedHost.EndsWith(TEXT("_C"))) NormalizedHost.LeftChopInline(2);
	if (NormalizedHost == ModuleAssetPath) return true;
	auto PackagePath = [](const FString& Value)
	{
		FString Package;
		FString Object;
		return Value.Split(TEXT("."), &Package, &Object) ? Package : Value;
	};
	return PackagePath(NormalizedHost) == PackagePath(ModuleAssetPath);
}

FString RigFunctionStableRuntimeSymbol(const FString& Value)
{
	return AnimLispStableRuntimeSymbol(Value);
}

FString RigFunctionSymbolFromLibraryNodePath(const FString& LibraryNodePath)
{
	FString Symbol = LibraryNodePath;
	int32 Separator = INDEX_NONE;
	if (Symbol.FindLastChar(TEXT('.'), Separator)) Symbol = Symbol.Mid(Separator + 1);
	return RigFunctionStableRuntimeSymbol(Symbol);
}

namespace
{

bool IsEditorOnlyProperty(const FString& Key)
{
	return Key.Equals(TEXT("editor-position"), ESearchCase::IgnoreCase)
		|| Key.Equals(TEXT("editor-position-x"), ESearchCase::IgnoreCase)
		|| Key.Equals(TEXT("editor-position-y"), ESearchCase::IgnoreCase)
		|| Key.Equals(TEXT("node-position"), ESearchCase::IgnoreCase);
}

void AppendProperties(FString& Out, const TMap<FString, FString>& Properties, const FString& Prefix)
{
	TArray<FString> Keys;
	Properties.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		if (!IsEditorOnlyProperty(Key))
		{
			Out += Prefix + TEXT(":") + Key + TEXT(" ") + Properties[Key];
		}
	}
}

bool IsPromotedHierarchyProperty(const FString& Key)
{
	static const TSet<FString> Keys = {
		TEXT("parents"), TEXT("parent-weights-current"), TEXT("parent-weights-initial"), TEXT("parent-labels"),
		TEXT("initial-local-transform"), TEXT("initial-global-transform"), TEXT("current-local-transform"), TEXT("current-global-transform"),
		TEXT("bone-type"), TEXT("curve-value"), TEXT("curve-value-set"), TEXT("control-settings"),
		TEXT("control-type"), TEXT("settings"),
		TEXT("control-value-current"), TEXT("control-value-initial"), TEXT("control-value-minimum"), TEXT("control-value-maximum"),
		TEXT("control-pose-current-local"), TEXT("control-pose-current-global"), TEXT("control-pose-initial-local"), TEXT("control-pose-initial-global"),
		TEXT("control-offset-current-local"), TEXT("control-offset-current-global"), TEXT("control-offset-initial-local"), TEXT("control-offset-initial-global"),
		TEXT("control-shape-current-local"), TEXT("control-shape-current-global"), TEXT("control-shape-initial-local"), TEXT("control-shape-initial-global"),
		TEXT("preferred-euler-order"), TEXT("preferred-euler-current"), TEXT("preferred-euler-initial"), TEXT("metadata")
	};
	return Keys.Contains(Key);
}

void AppendHierarchyProperties(FString& Out, const FRigHierarchyElementAST& Element)
{
	TMap<FString, FString> Legacy = Element.Properties;
	if (!Element.Parents.IsEmpty() || !Element.Transforms.IsEmpty() || !Element.States.IsEmpty() || !Element.Metadata.IsEmpty())
	{
		for (auto It = Legacy.CreateIterator(); It; ++It) if (IsPromotedHierarchyProperty(It.Key())) It.RemoveCurrent();
	}
	AppendProperties(Out, Legacy, TEXT(" "));
}

const TCHAR* HierarchyKindText(const ERigHierarchyElementKind Kind)
{
	switch (Kind)
	{
	case ERigHierarchyElementKind::Bone: return TEXT("bone");
	case ERigHierarchyElementKind::Control: return TEXT("control");
	case ERigHierarchyElementKind::Null: return TEXT("null");
	case ERigHierarchyElementKind::Curve: return TEXT("curve");
	default: return TEXT("bone");
	}
}

FString VectorText(const FVector& Value)
{
	auto ExactDouble = [](const double Number) { return FString::Printf(TEXT("%.17g"), Number); };
	return TEXT("(") + ExactDouble(Value.X) + TEXT(" ") + ExactDouble(Value.Y) + TEXT(" ") + ExactDouble(Value.Z) + TEXT(")");
}

FString QuatText(const FQuat& Value)
{
	auto ExactDouble = [](const double Number) { return FString::Printf(TEXT("%.17g"), Number); };
	return TEXT("(") + ExactDouble(Value.X) + TEXT(" ") + ExactDouble(Value.Y) + TEXT(" ")
		+ ExactDouble(Value.Z) + TEXT(" ") + ExactDouble(Value.W) + TEXT(")");
}

const TCHAR* TransformRoleText(const ERigHierarchyTransformRole Role)
{
	switch (Role)
	{
	case ERigHierarchyTransformRole::InitialLocal: return TEXT("initial-local");
	case ERigHierarchyTransformRole::InitialGlobal: return TEXT("initial-global");
	case ERigHierarchyTransformRole::CurrentLocal: return TEXT("current-local");
	case ERigHierarchyTransformRole::CurrentGlobal: return TEXT("current-global");
	case ERigHierarchyTransformRole::PoseInitialLocal: return TEXT("pose-initial-local");
	case ERigHierarchyTransformRole::PoseInitialGlobal: return TEXT("pose-initial-global");
	case ERigHierarchyTransformRole::PoseCurrentLocal: return TEXT("pose-current-local");
	case ERigHierarchyTransformRole::PoseCurrentGlobal: return TEXT("pose-current-global");
	case ERigHierarchyTransformRole::OffsetInitialLocal: return TEXT("offset-initial-local");
	case ERigHierarchyTransformRole::OffsetInitialGlobal: return TEXT("offset-initial-global");
	case ERigHierarchyTransformRole::OffsetCurrentLocal: return TEXT("offset-current-local");
	case ERigHierarchyTransformRole::OffsetCurrentGlobal: return TEXT("offset-current-global");
	case ERigHierarchyTransformRole::ShapeInitialLocal: return TEXT("shape-initial-local");
	case ERigHierarchyTransformRole::ShapeInitialGlobal: return TEXT("shape-initial-global");
	case ERigHierarchyTransformRole::ShapeCurrentLocal: return TEXT("shape-current-local");
	default: return TEXT("shape-current-global");
	}
}

bool IsCurrentTransformRole(const ERigHierarchyTransformRole Role)
{
	return Role == ERigHierarchyTransformRole::CurrentLocal || Role == ERigHierarchyTransformRole::CurrentGlobal
		|| Role == ERigHierarchyTransformRole::PoseCurrentLocal || Role == ERigHierarchyTransformRole::PoseCurrentGlobal
		|| Role == ERigHierarchyTransformRole::OffsetCurrentLocal || Role == ERigHierarchyTransformRole::OffsetCurrentGlobal
		|| Role == ERigHierarchyTransformRole::ShapeCurrentLocal || Role == ERigHierarchyTransformRole::ShapeCurrentGlobal;
}

bool IsTransientCurrentState(const FRigHierarchyStateAST& State)
{
	return State.Role == TEXT("current")
		&& (State.Kind == ERigHierarchyStateKind::ControlValue
			|| State.Kind == ERigHierarchyStateKind::PreferredEuler);
}

const TCHAR* StateKindText(const ERigHierarchyStateKind Kind)
{
	switch (Kind)
	{
	case ERigHierarchyStateKind::BoneType: return TEXT("bone-type");
	case ERigHierarchyStateKind::Curve: return TEXT("curve");
	case ERigHierarchyStateKind::ControlSettings: return TEXT("control-settings");
	case ERigHierarchyStateKind::ControlValue: return TEXT("control-value");
	default: return TEXT("preferred-euler");
	}
}

const TCHAR* MetadataKindText(const ERigHierarchyMetadataValueKind Kind)
{
	switch (Kind)
	{
	case ERigHierarchyMetadataValueKind::Bool: return TEXT("bool");
	case ERigHierarchyMetadataValueKind::Integer: return TEXT("integer");
	case ERigHierarchyMetadataValueKind::Float: return TEXT("float");
	case ERigHierarchyMetadataValueKind::Name: return TEXT("name");
	case ERigHierarchyMetadataValueKind::Vector: return TEXT("vector");
	case ERigHierarchyMetadataValueKind::Rotator: return TEXT("rotator");
	case ERigHierarchyMetadataValueKind::Quat: return TEXT("quat");
	case ERigHierarchyMetadataValueKind::Transform: return TEXT("transform");
	case ERigHierarchyMetadataValueKind::LinearColor: return TEXT("linear-color");
	case ERigHierarchyMetadataValueKind::ElementKey: return TEXT("element-key");
	case ERigHierarchyMetadataValueKind::BoolArray: return TEXT("bool-array");
	case ERigHierarchyMetadataValueKind::IntegerArray: return TEXT("integer-array");
	case ERigHierarchyMetadataValueKind::FloatArray: return TEXT("float-array");
	case ERigHierarchyMetadataValueKind::NameArray: return TEXT("name-array");
	case ERigHierarchyMetadataValueKind::VectorArray: return TEXT("vector-array");
	case ERigHierarchyMetadataValueKind::RotatorArray: return TEXT("rotator-array");
	case ERigHierarchyMetadataValueKind::QuatArray: return TEXT("quat-array");
	case ERigHierarchyMetadataValueKind::TransformArray: return TEXT("transform-array");
	case ERigHierarchyMetadataValueKind::LinearColorArray: return TEXT("linear-color-array");
	default: return TEXT("element-key-array");
	}
}

const TCHAR* AccessText(const ERigVariableAccess Access)
{
	switch (Access)
	{
	case ERigVariableAccess::PublicInput: return TEXT("public-input");
	case ERigVariableAccess::PublicOutput: return TEXT("public-output");
	default: return TEXT("internal");
	}
}

const TCHAR* DirectionText(const ERigPinDirection Direction)
{
	switch (Direction)
	{
	case ERigPinDirection::Output: return TEXT("output");
	case ERigPinDirection::IO: return TEXT("io");
	case ERigPinDirection::Visible: return TEXT("visible");
	case ERigPinDirection::Hidden: return TEXT("hidden");
	case ERigPinDirection::Invalid: return TEXT("invalid");
	default: return TEXT("input");
	}
}

const TCHAR* CoverageText(const ERigNodeCoverage Coverage)
{
	switch (Coverage)
	{
	case ERigNodeCoverage::Reflected: return TEXT("reflected");
	case ERigNodeCoverage::Lossy: return TEXT("lossy");
	case ERigNodeCoverage::Unsupported: return TEXT("unsupported");
	default: return TEXT("exact");
	}
}

void AppendPin(FString& Out, const FRigPinAST& Pin, const int32 Indent)
{
	const FString Pad = FString::ChrN(Indent, TEXT(' '));
	Out += Pad + TEXT("(pin")
		+ TEXT(" :path ") + Quote(Pin.Path)
		+ TEXT(" :direction ") + DirectionText(Pin.Direction)
		+ TEXT(" :cpp-type ") + Quote(Pin.Type.CPPType)
		+ TEXT(" :cpp-type-object ") + Quote(Pin.Type.CPPTypeObject)
		+ TEXT(" :container-type ") + Quote(Pin.Type.ContainerType)
		+ TEXT(" :default ") + Quote(Pin.DefaultValue)
		+ TEXT(" :execute-context ") + (Pin.bExecuteContext ? TEXT("true") : TEXT("false"));
	AppendProperties(Out, Pin.Properties, TEXT(" "));
	for (const FRigPinAST& SubPin : Pin.SubPins)
	{
		Out += TEXT("\n");
		AppendPin(Out, SubPin, Indent + 2);
	}
	Out += TEXT(")");
}

void AppendNode(FString& Out, const FRigNodeAST& Node, const int32 Indent)
{
	const FString Pad = FString::ChrN(Indent, TEXT(' '));
	const TCHAR* Head = TEXT("(rig-unit");
	switch (Node.Kind)
	{
	case ERigNodeKind::Call: Head = TEXT("(rig-call"); break;
	case ERigNodeKind::Variable: Head = TEXT("(rig-variable-node"); break;
	case ERigNodeKind::Comment: Head = TEXT("(rig-comment"); break;
	case ERigNodeKind::Reroute: Head = TEXT("(rig-reroute"); break;
	case ERigNodeKind::Entry: Head = TEXT("(rig-entry-node"); break;
	case ERigNodeKind::Return: Head = TEXT("(rig-return-node"); break;
	case ERigNodeKind::Collapse: Head = TEXT("(rig-collapse"); break;
	case ERigNodeKind::Dispatch: Head = TEXT("(rig-dispatch"); break;
	case ERigNodeKind::Aggregate: Head = TEXT("(rig-aggregate"); break;
	case ERigNodeKind::InvokeEntry: Head = TEXT("(rig-invoke-entry"); break;
	default: break;
	}
	Out += Pad + FString(Head)
		+ TEXT(" :id ") + Quote(Node.StableId)
		+ TEXT(" :guid ") + Quote(Node.Guid);
	if (Node.Kind == ERigNodeKind::Call)
	{
		Out += TEXT(" :function ") + Quote(Node.FunctionName);
		if (Node.FunctionIdentifier.IsComplete())
		{
			Out += TEXT(" :function-identifier-host ") + Quote(Node.FunctionIdentifier.HostObject)
				+ TEXT(" :function-library-node-path ") + Quote(Node.FunctionIdentifier.LibraryNodePath);
		}
	}
	Out += TEXT(" :class ") + Quote(Node.ClassPath)
		+ TEXT(" :method ") + Quote(Node.MethodName)
		+ TEXT(" :event ") + Quote(Node.EventName)
		+ TEXT(" :injected ") + (Node.bInjected ? TEXT("true") : TEXT("false"));
	if (!Node.ContainedGraphStableId.IsEmpty())
	{
		Out += TEXT(" :contained-graph-id ") + Quote(Node.ContainedGraphStableId);
	}
	if (Node.bInjected)
	{
		Out += TEXT(" :injection-owner-pin ") + Quote(Node.InjectionOwnerPin)
			+ TEXT(" :injection-order ") + FString::FromInt(Node.InjectionOrder)
			+ TEXT(" :injected-as-input ") + (Node.bInjectedAsInput ? TEXT("true") : TEXT("false"))
			+ TEXT(" :injection-input-pin ") + Quote(Node.InjectionInputPin)
			+ TEXT(" :injection-output-pin ") + Quote(Node.InjectionOutputPin);
	}
	Out += TEXT(" :coverage ") + FString(CoverageText(Node.Coverage));
	TMap<FString, FString> Properties = Node.Properties;
	if (!Node.ContainedGraphStableId.IsEmpty()) Properties.Remove(TEXT("contained-graph"));
	if (Node.FunctionIdentifier.IsSet())
	{
		Properties.Remove(TEXT("function-identifier-host"));
		Properties.Remove(TEXT("function-library-node-path"));
	}
	AppendProperties(Out, Properties, TEXT(" "));
	for (const FRigPinAST& Pin : Node.Pins)
	{
		Out += TEXT("\n");
		AppendPin(Out, Pin, Indent + 2);
	}
	Out += TEXT(")");
}

void AppendLink(FString& Out, const FRigLinkAST& Link, const int32 Indent)
{
	Out += FString::ChrN(Indent, TEXT(' ')) + TEXT("(rig-link :from ")
		+ Quote(Link.SourceNodeId + TEXT(".") + Link.SourcePinPath)
		+ TEXT(" :to ") + Quote(Link.TargetNodeId + TEXT(".") + Link.TargetPinPath);
	AppendProperties(Out, Link.Properties, TEXT(" "));
	Out += TEXT(")");
}

void AppendGraph(FString& Out, const FRigGraphAST& Graph, const int32 Indent)
{
	for (const FRigGraphVariableAST& Variable : Graph.LocalVariables)
	{
		Out += TEXT("\n") + FString::ChrN(Indent, TEXT(' ')) + TEXT("(rig-local-variable")
			+ TEXT(" :guid ") + Quote(Variable.Guid)
			+ TEXT(" :name ") + Quote(Variable.Name)
			+ TEXT(" :cpp-type ") + Quote(Variable.Type.CPPType)
			+ TEXT(" :cpp-type-object ") + Quote(Variable.Type.CPPTypeObject)
			+ TEXT(" :cpp-type-object-path ") + Quote(Variable.CPPTypeObjectPath)
			+ TEXT(" :container-type ") + Quote(Variable.Type.ContainerType)
			+ TEXT(" :default ") + Quote(Variable.DefaultValue)
			+ TEXT(" :category ") + Quote(Variable.Category)
			+ TEXT(" :tooltip ") + Quote(Variable.Tooltip)
			+ TEXT(" :exposed-on-spawn ") + (Variable.bExposedOnSpawn ? TEXT("true") : TEXT("false"))
			+ TEXT(" :expose-to-cinematics ") + (Variable.bExposeToCinematics ? TEXT("true") : TEXT("false"))
			+ TEXT(" :public ") + (Variable.bPublic ? TEXT("true") : TEXT("false"))
			+ TEXT(" :private ") + (Variable.bPrivate ? TEXT("true") : TEXT("false"))
			+ TEXT(")");
	}
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		Out += TEXT("\n");
		AppendNode(Out, Node, Indent);
	}
	for (const FRigLinkAST& Link : Graph.Links)
	{
		Out += TEXT("\n");
		AppendLink(Out, Link, Indent);
	}
}

void AppendArgument(FString& Out, const FRigCallableArgumentAST& Argument, const int32 Indent)
{
	Out += TEXT("\n") + FString::ChrN(Indent, TEXT(' ')) + TEXT("(rig-argument")
		+ TEXT(" :name ") + Quote(Argument.Name)
		+ TEXT(" :direction ") + DirectionText(Argument.Direction)
		+ TEXT(" :cpp-type ") + Quote(Argument.Type.CPPType)
		+ TEXT(" :cpp-type-object ") + Quote(Argument.Type.CPPTypeObject)
		+ TEXT(" :container-type ") + Quote(Argument.Type.ContainerType)
		+ TEXT(" :default ") + Quote(Argument.DefaultValue)
		+ TEXT(" :execute-context ") + (Argument.bExecuteContext ? TEXT("true") : TEXT("false"))
		+ TEXT(" :constant ") + (Argument.bConstant ? TEXT("true") : TEXT("false"))
		+ TEXT(" :input-variable ") + (Argument.bInputVariable ? TEXT("true") : TEXT("false"))
		+ TEXT(")");
}

void AppendExternalVariable(FString& Out, const FRigExternalVariableAST& Variable, const int32 Indent)
{
	Out += TEXT("\n") + FString::ChrN(Indent, TEXT(' ')) + TEXT("(rig-external-variable")
		+ TEXT(" :guid ") + Quote(Variable.Guid)
		+ TEXT(" :name ") + Quote(Variable.Name)
		+ TEXT(" :cpp-type ") + Quote(Variable.Type.CPPType)
		+ TEXT(" :cpp-type-object ") + Quote(Variable.Type.CPPTypeObject)
		+ TEXT(" :container-type ") + Quote(Variable.Type.ContainerType)
		+ TEXT(" :public ") + (Variable.bPublic ? TEXT("true") : TEXT("false"))
		+ TEXT(" :read-only ") + (Variable.bReadOnly ? TEXT("true") : TEXT("false"))
		+ TEXT(")");
}

void AppendDependency(FString& Out, const FRigFunctionDependencyAST& Dependency, const int32 Indent)
{
	Out += TEXT("\n") + FString::ChrN(Indent, TEXT(' ')) + TEXT("(rig-dependency")
		+ TEXT(" :host ") + Quote(Dependency.HostObject)
		+ TEXT(" :library-node-path ") + Quote(Dependency.LibraryNodePath)
		+ TEXT(" :hash ") + FString::Printf(TEXT("%u"), Dependency.Hash)
		+ TEXT(")");
}

FString BuildCanonical(const FRigModuleAST& Module, const bool bIncludeContentHash)
{
	FString Out = TEXT("(rig-module :asset ") + Quote(Module.Header.ModuleId.AssetPath)
		+ TEXT(" :class ") + Quote(Module.Header.AssetClassPath)
		+ TEXT(" :version ") + FString::FromInt(Module.Header.Version);
	if (bIncludeContentHash)
	{
		Out += TEXT(" :content-hash ") + Quote(Module.Header.ContentHash);
	}
	AppendProperties(Out, Module.Header.Properties, TEXT(" "));
	Out += TEXT(")");

	TArray<FRigImportAST> Imports = Module.Imports;
	Imports.Sort([](const FRigImportAST& A, const FRigImportAST& B)
	{
		if (A.Import.Target.AssetPath != B.Import.Target.AssetPath)
		{
			return A.Import.Target.AssetPath < B.Import.Target.AssetPath;
		}
		return A.Import.Alias < B.Import.Alias;
	});
	for (const FRigImportAST& Import : Imports)
	{
		Out += TEXT("\n(import-rig :asset ") + Quote(Import.Import.Target.AssetPath)
			+ TEXT(" :alias ") + Import.Import.Alias
			+ TEXT(" :content-hash ") + Quote(Import.Import.ExpectedHash);
		AppendProperties(Out, Import.Properties, TEXT(" "));
		Out += TEXT(")");
	}

	if (!Module.Hierarchy.IsEmpty())
	{
		Out += TEXT("\n(rig-hierarchy");
		for (const FRigHierarchyElementAST& Element : Module.Hierarchy)
		{
			Out += TEXT("\n  (") + FString(HierarchyKindText(Element.Kind))
				+ TEXT(" :id ") + Quote(Element.StableId)
				+ TEXT(" :name ") + Quote(Element.Name)
				+ TEXT(" :parent ") + Quote(Element.ParentName);
			AppendHierarchyProperties(Out, Element);
			for (const FRigHierarchyParentAST& Parent : Element.Parents)
			{
				const FRigHierarchyWeightAST& Current = bIncludeContentHash ? Parent.CurrentWeight : FRigHierarchyWeightAST();
				Out += TEXT("\n    (rig-parent :id ") + Quote(Parent.StableId) + TEXT(" :label ") + Quote(Parent.Label)
					+ TEXT(" :current-location ") + LexToString(Current.Location) + TEXT(" :current-rotation ") + LexToString(Current.Rotation)
					+ TEXT(" :current-scale ") + LexToString(Current.Scale) + TEXT(" :initial-location ") + LexToString(Parent.InitialWeight.Location)
					+ TEXT(" :initial-rotation ") + LexToString(Parent.InitialWeight.Rotation) + TEXT(" :initial-scale ") + LexToString(Parent.InitialWeight.Scale) + TEXT(")");
			}
			for (const FRigHierarchyTransformAST& Transform : Element.Transforms)
			{
				if (!bIncludeContentHash && IsCurrentTransformRole(Transform.Role)) continue;
				Out += TEXT("\n    (rig-transform :role ") + FString(TransformRoleText(Transform.Role))
					+ TEXT(" :translation ") + VectorText(Transform.Value.GetTranslation())
					+ TEXT(" :rotation ") + QuatText(Transform.Value.GetRotation())
					+ TEXT(" :scale ") + VectorText(Transform.Value.GetScale3D()) + TEXT(")");
			}
			for (const FRigHierarchyStateAST& State : Element.States)
			{
				if (!bIncludeContentHash && IsTransientCurrentState(State)) continue;
				Out += TEXT("\n    (rig-state :kind ") + FString(StateKindText(State.Kind)) + TEXT(" :role ") + State.Role
					+ TEXT(" :type ") + State.Type;
				if (State.Kind == ERigHierarchyStateKind::BoneType)
				{
				}
				else if (State.Kind == ERigHierarchyStateKind::ControlSettings)
				{
					Out += TEXT(" :serialized ") + Quote(State.SerializedValue);
				}
				else if (State.Kind == ERigHierarchyStateKind::Curve)
				{
					Out += TEXT(" :number ") + LexToString(State.NumberValue)
						+ TEXT(" :bool ") + FString(State.bBoolValue ? TEXT("true") : TEXT("false"));
				}
				else if (State.Kind == ERigHierarchyStateKind::PreferredEuler)
				{
					TArray<FString> V; for(double X:State.Components)V.Add(LexToString(X)); Out += TEXT(" :components (") + FString::Join(V,TEXT(" ")) + TEXT(")");
				}
				else if (State.Kind == ERigHierarchyStateKind::ControlValue)
				{
					if (State.Type == TEXT("Bool")) Out += TEXT(" :bool ") + FString(State.bBoolValue ? TEXT("true") : TEXT("false"));
					else if (State.Type == TEXT("Integer")) Out += TEXT(" :integer ") + LexToString(State.IntegerValue);
					else if (State.Type == TEXT("Float") || State.Type == TEXT("ScaleFloat")) Out += TEXT(" :number ") + LexToString(State.NumberValue);
					else { TArray<FString> V; for(double X:State.Components)V.Add(LexToString(X)); Out += TEXT(" :components (") + FString::Join(V,TEXT(" ")) + TEXT(")"); }
				}
				Out += TEXT(")");
			}
			for (const FRigHierarchyMetadataAST& Metadata : Element.Metadata)
			{
				TArray<FString> Values;
				FString ValueKey;
				switch (Metadata.Kind)
				{
				case ERigHierarchyMetadataValueKind::Bool:
				case ERigHierarchyMetadataValueKind::BoolArray:
					ValueKey = TEXT("bools"); for (bool X : Metadata.BoolValues) Values.Add(X ? TEXT("true") : TEXT("false")); break;
				case ERigHierarchyMetadataValueKind::Integer:
				case ERigHierarchyMetadataValueKind::IntegerArray:
					ValueKey = TEXT("integers"); for (int64 X : Metadata.IntegerValues) Values.Add(LexToString(X)); break;
				case ERigHierarchyMetadataValueKind::Float:
				case ERigHierarchyMetadataValueKind::FloatArray:
					ValueKey = TEXT("numbers"); for (double X : Metadata.NumberValues) Values.Add(LexToString(X)); break;
				case ERigHierarchyMetadataValueKind::Name:
				case ERigHierarchyMetadataValueKind::ElementKey:
				case ERigHierarchyMetadataValueKind::NameArray:
				case ERigHierarchyMetadataValueKind::ElementKeyArray:
					ValueKey = TEXT("strings"); for (const FString& X : Metadata.StringValues) Values.Add(Quote(X)); break;
				case ERigHierarchyMetadataValueKind::Vector:
				case ERigHierarchyMetadataValueKind::VectorArray:
					ValueKey = TEXT("vectors"); for (const FVector& X : Metadata.VectorValues) Values.Add(VectorText(X)); break;
				case ERigHierarchyMetadataValueKind::Rotator:
				case ERigHierarchyMetadataValueKind::RotatorArray:
					ValueKey = TEXT("rotators"); for (const FRotator& X : Metadata.RotatorValues) Values.Add(TEXT("(")+LexToString(X.Pitch)+TEXT(" ")+LexToString(X.Yaw)+TEXT(" ")+LexToString(X.Roll)+TEXT(")")); break;
				case ERigHierarchyMetadataValueKind::Quat:
				case ERigHierarchyMetadataValueKind::QuatArray:
					ValueKey = TEXT("quats"); for (const FQuat& X : Metadata.QuatValues) Values.Add(QuatText(X)); break;
				case ERigHierarchyMetadataValueKind::Transform:
				case ERigHierarchyMetadataValueKind::TransformArray:
					ValueKey = TEXT("transforms"); for(const FTransform& X:Metadata.TransformValues){const FVector T=X.GetTranslation(),S=X.GetScale3D();const FQuat Q=X.GetRotation();Values.Add(TEXT("(")+LexToString(T.X)+TEXT(" ")+LexToString(T.Y)+TEXT(" ")+LexToString(T.Z)+TEXT(" ")+LexToString(Q.X)+TEXT(" ")+LexToString(Q.Y)+TEXT(" ")+LexToString(Q.Z)+TEXT(" ")+LexToString(Q.W)+TEXT(" ")+LexToString(S.X)+TEXT(" ")+LexToString(S.Y)+TEXT(" ")+LexToString(S.Z)+TEXT(")"));} break;
				case ERigHierarchyMetadataValueKind::LinearColor:
				case ERigHierarchyMetadataValueKind::LinearColorArray:
					ValueKey = TEXT("colors"); for(const FLinearColor& X:Metadata.ColorValues)Values.Add(TEXT("(")+LexToString(X.R)+TEXT(" ")+LexToString(X.G)+TEXT(" ")+LexToString(X.B)+TEXT(" ")+LexToString(X.A)+TEXT(")")); break;
				}
				Out += TEXT("\n    (rig-metadata :name ") + Quote(Metadata.Name) + TEXT(" :kind ")
					+ MetadataKindText(Metadata.Kind) + TEXT(" :") + ValueKey + TEXT(" (") + FString::Join(Values,TEXT(" ")) + TEXT("))");
			}
			Out += TEXT(")");
		}
		Out += TEXT(")");
	}

	if (!Module.Variables.IsEmpty())
	{
		TArray<FRigVariableAST> Variables = Module.Variables;
		Variables.Sort([](const FRigVariableAST& A, const FRigVariableAST& B)
		{
			if (A.Name != B.Name) return A.Name < B.Name;
			return A.StableId < B.StableId;
		});
		Out += TEXT("\n(rig-variables");
		for (const FRigVariableAST& Variable : Variables)
		{
			Out += TEXT("\n  (variable :id ") + Quote(Variable.StableId)
				+ TEXT(" :name ") + Quote(Variable.Name)
				+ TEXT(" :access ") + AccessText(Variable.Access)
				+ TEXT(" :cpp-type ") + Quote(Variable.Type.CPPType)
				+ TEXT(" :cpp-type-object ") + Quote(Variable.Type.CPPTypeObject)
				+ TEXT(" :container-type ") + Quote(Variable.Type.ContainerType)
				+ TEXT(" :default ") + Quote(Variable.DefaultValue)
				+ TEXT(" :execute-context ") + (Variable.bExecuteContext ? TEXT("true") : TEXT("false"));
			AppendProperties(Out, Variable.Properties, TEXT(" "));
			Out += TEXT(")");
		}
		Out += TEXT(")");
	}

	TArray<FRigGraphAST> Graphs = Module.Graphs;
	Graphs.Sort([](const FRigGraphAST& A, const FRigGraphAST& B)
	{
		return A.StableId < B.StableId;
	});
	for (const FRigGraphAST& Graph : Graphs)
	{
		Out += TEXT("\n(define-rig-graph :id ") + Quote(Graph.StableId)
			+ TEXT(" :editor-guid ") + Quote(Graph.EditorGuid)
			+ TEXT(" :role ") + Quote(Graph.Role)
			+ TEXT(" :parent-id ") + Quote(Graph.ParentStableId);
		AppendProperties(Out, Graph.Properties, TEXT(" "));
		AppendGraph(Out, Graph, 2);
		Out += TEXT(")");
	}

	TArray<FRigFunctionAST> Functions = Module.Functions;
	Functions.Sort([](const FRigFunctionAST& A, const FRigFunctionAST& B)
	{
		if (A.Name != B.Name) return A.Name < B.Name;
		return A.StableId < B.StableId;
	});
	for (const FRigFunctionAST& Function : Functions)
	{
		Out += TEXT("\n(define-rig-function ") + Quote(Function.Name)
			+ TEXT(" :id ") + Quote(Function.StableId)
			+ TEXT(" :visibility ") + Function.Visibility
			+ TEXT(" :return-cpp-type ") + Quote(Function.ReturnCPPType);
		if (Function.FunctionIdentifier.IsComplete())
		{
			Out += TEXT(" :identifier-host ") + Quote(Function.FunctionIdentifier.HostObject)
				+ TEXT(" :library-node-path ") + Quote(Function.FunctionIdentifier.LibraryNodePath);
		}
		if (!Function.GraphStableId.IsEmpty())
		{
			Out += TEXT(" :graph-id ") + Quote(Function.GraphStableId);
		}
		TMap<FString, FString> FunctionProperties = Function.Properties;
		if (Function.FunctionIdentifier.IsSet())
		{
			FunctionProperties.Remove(TEXT("identifier-host"));
			FunctionProperties.Remove(TEXT("library-node-path"));
		}
		AppendProperties(Out, FunctionProperties, TEXT(" "));
		if (!Function.Arguments.IsEmpty())
		{
			for (const FRigCallableArgumentAST& Argument : Function.Arguments) AppendArgument(Out, Argument, 2);
		}
		else
		{
			for (const FRigCallableArgumentAST& Argument : Function.Inputs) AppendArgument(Out, Argument, 2);
			for (const FRigCallableArgumentAST& Argument : Function.Outputs) AppendArgument(Out, Argument, 2);
		}
		for (const FRigExternalVariableAST& Variable : Function.ExternalVariables) AppendExternalVariable(Out, Variable, 2);
		for (const FRigFunctionDependencyAST& Dependency : Function.Dependencies) AppendDependency(Out, Dependency, 2);
		AppendGraph(Out, Function.Graph, 2);
		Out += TEXT(")");
	}

	TArray<FRigEntryAST> Entries = Module.Entries;
	Entries.Sort([](const FRigEntryAST& A, const FRigEntryAST& B)
	{
		if (A.Name != B.Name) return A.Name < B.Name;
		return A.StableId < B.StableId;
	});
	for (const FRigEntryAST& Entry : Entries)
	{
		Out += TEXT("\n(define-rig-entry ") + Quote(Entry.Name)
			+ TEXT(" :id ") + Quote(Entry.StableId)
			+ TEXT(" :event ") + Quote(Entry.EventName);
		if (!Entry.GraphStableId.IsEmpty())
		{
			Out += TEXT(" :graph-id ") + Quote(Entry.GraphStableId);
		}
		AppendProperties(Out, Entry.Properties, TEXT(" "));
		if (!Entry.Arguments.IsEmpty())
		{
			for (const FRigCallableArgumentAST& Argument : Entry.Arguments) AppendArgument(Out, Argument, 2);
		}
		else
		{
			for (const FRigCallableArgumentAST& Argument : Entry.Inputs) AppendArgument(Out, Argument, 2);
			for (const FRigCallableArgumentAST& Argument : Entry.Outputs) AppendArgument(Out, Argument, 2);
		}
		AppendGraph(Out, Entry.Graph, 2);
		Out += TEXT(")");
	}

	Out += TEXT("\n");
	return Out;
}

void AddCoverage(const FRigGraphAST& Graph, FRigCoverageTotals& Totals)
{
	for (const FRigNodeAST& Node : Graph.Nodes)
	{
		switch (Node.Coverage)
		{
		case ERigNodeCoverage::Exact: ++Totals.Exact; break;
		case ERigNodeCoverage::Reflected: ++Totals.Reflected; break;
		case ERigNodeCoverage::Lossy: ++Totals.Lossy; break;
		case ERigNodeCoverage::Unsupported: ++Totals.Unsupported; break;
		}
	}
}
}

FString FRigModuleAST::ToCanonicalString() const
{
	return BuildCanonical(*this, true);
}

FString FRigModuleAST::ToCanonicalHashInput() const
{
	FRigModuleAST Semantic = *this;
	auto NormalizeNodeProperties = [](FRigGraphAST& Graph)
	{
		for (FRigNodeAST& Node : Graph.Nodes)
		{
			if (Node.Kind == ERigNodeKind::Comment)
			{
				Node.Properties.Remove(TEXT("font-size"));
				Node.Properties.Remove(TEXT("bubble-visible"));
				Node.Properties.Remove(TEXT("color-bubble"));
			}
			if (Node.Properties.FindRef(TEXT("template-resolved")) == TEXT("false"))
				Node.Properties.Remove(TEXT("resolved-function"));
		}
	};
	for (FRigGraphAST& Graph : Semantic.Graphs) NormalizeNodeProperties(Graph);
	for (FRigFunctionAST& Function : Semantic.Functions)
	{
		NormalizeNodeProperties(Function.Graph);
		// RigVM recomputes this stale-detection cache from the referenced function's
		// compilation data. Dependency identity is host + library node path.
		for (FRigFunctionDependencyAST& Dependency : Function.Dependencies)
			Dependency.Hash = 0;
	}
	for (FRigEntryAST& Entry : Semantic.Entries) NormalizeNodeProperties(Entry.Graph);
	return BuildCanonical(Semantic, false);
}

bool FRigModuleAST::SemanticEquals(const FRigModuleAST& Other) const
{
	return ToCanonicalHashInput() == Other.ToCanonicalHashInput();
}

FRigCoverageTotals FRigModuleAST::GetCoverageTotals() const
{
	FRigCoverageTotals Totals;
	if (!Graphs.IsEmpty())
	{
		for (const FRigGraphAST& Graph : Graphs) AddCoverage(Graph, Totals);
		return Totals;
	}
	for (const FRigFunctionAST& Function : Functions) AddCoverage(Function.Graph, Totals);
	for (const FRigEntryAST& Entry : Entries) AddCoverage(Entry.Graph, Totals);
	return Totals;
}
