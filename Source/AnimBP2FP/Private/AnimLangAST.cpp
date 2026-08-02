// AnimLangAST.cpp - Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangAST.h"

namespace
{
	static FString EscapeQuotedStringForDSL(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static FString PinTypeToAnimLangString(EPinType Type)
	{
		switch (Type)
		{
		case EPinType::Pose:      return TEXT("pose");
		case EPinType::Float:     return TEXT("float");
		case EPinType::Int:       return TEXT("int");
		case EPinType::Bool:      return TEXT("bool");
		case EPinType::Vector:    return TEXT("vector");
		case EPinType::Rotator:   return TEXT("rotator");
		case EPinType::Transform: return TEXT("transform");
		case EPinType::Name:      return TEXT("name");
		case EPinType::Enum:      return TEXT("enum");
		case EPinType::Object:    return TEXT("object");
		case EPinType::Struct:    return TEXT("struct");
		case EPinType::Unknown:   return TEXT("unknown");
		default:                  return TEXT("unknown");
		}
	}

	static FString FormatHelperDSLValue(const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return TEXT("\"\"");
		}

		if (Trimmed.StartsWith(TEXT("(")) || Trimmed.StartsWith(TEXT("[")))
		{
			return Trimmed;
		}

		return EscapeQuotedStringForDSL(Trimmed);
	}

	static const TCHAR* CoverageToAnimLangString(const EAnimNodeCoverage Coverage)
	{
		switch (Coverage)
		{
		case EAnimNodeCoverage::Exact: return TEXT("exact");
		case EAnimNodeCoverage::Reflected: return TEXT("reflected");
		case EAnimNodeCoverage::Lossy: return TEXT("lossy");
		case EAnimNodeCoverage::Unsupported: return TEXT("unsupported");
		default: return TEXT("unsupported");
		}
	}
}

// ========== FAnimNodeAST ==========

FString FAnimNodeAST::ToString(int32 Indent) const
{
	FString IndentStr = FString::ChrN(Indent, ' ');
	FString ChildIndentStr = FString::ChrN(Indent + 2, ' ');
	FString Result = FString::Printf(TEXT("%s(%s"), *IndentStr, *NodeType);
	if (!NodeId.IsEmpty())
	{
		Result += FString::Printf(TEXT(" :node-id %s"), *EscapeQuotedStringForDSL(NodeId));
	}
	if (!NodeClassPath.IsEmpty())
	{
		Result += FString::Printf(TEXT(" :node-class %s"), *EscapeQuotedStringForDSL(NodeClassPath));
	}
	Result += FString::Printf(TEXT(" :coverage %s"), CoverageToAnimLangString(Coverage));
	if (RigBinding.IsSet())
	{
		const FAnimRigNodeBinding& Binding = RigBinding.GetValue();
		Result += FString::Printf(TEXT(" :library (rig-ref %s)"), *Binding.ImportAlias);
		if (!Binding.EntryName.IsEmpty())
		{
			Result += FString::Printf(TEXT(" :entry (rig-entry %s/%s)"),
				*Binding.ImportAlias, *Binding.EntryName);
		}
		Result += TEXT(" :inputs (");
		TArray<FAnimRigInputBinding> SortedInputs = Binding.Inputs;
		SortedInputs.Sort([](const FAnimRigInputBinding& A, const FAnimRigInputBinding& B)
		{
			return A.RigInputName < B.RigInputName;
		});
		for (int32 Index = 0; Index < SortedInputs.Num(); ++Index)
		{
			if (Index > 0) Result += TEXT(" ");
			const FAnimRigInputBinding& Input = SortedInputs[Index];
			Result += FString::Printf(TEXT("(%s"), *Input.RigInputName);
			if (!Input.ResolvedType.CPPType.IsEmpty())
			{
				Result += FString::Printf(TEXT(" :cpp-type %s :cpp-type-object %s :container-type %s"),
					*EscapeQuotedStringForDSL(Input.ResolvedType.CPPType),
					*EscapeQuotedStringForDSL(Input.ResolvedType.CPPTypeObject),
					*EscapeQuotedStringForDSL(Input.ResolvedType.ContainerType));
			}
			Result += TEXT(" ") + Input.ValueExpression + TEXT(")");
		}
		Result += TEXT(")");
	}
	
	// Add properties (non-pose parameters) in a stable order. TMap iteration
	// depends on hash allocation history, which must not affect canonical DSL.
	TArray<FString> PropertyKeys;
	Properties.GetKeys(PropertyKeys);
	PropertyKeys.Sort();
	for (const FString& PropertyKey : PropertyKeys)
	{
		if (RigBinding.IsSet()
			&& (PropertyKey == TEXT("library") || PropertyKey == TEXT("entry") || PropertyKey == TEXT("inputs")
				|| PropertyKey == TEXT("control-rig-asset-reference")
				|| PropertyKey == TEXT("exposed-input-pins")))
		{
			continue;
		}
		const FString& PropertyValue = Properties.FindChecked(PropertyKey);
		// If the value is a quoted string, re-escape internal quotes for correct DSL output
		FString OutputValue = PropertyValue;
		if (OutputValue.StartsWith(TEXT("\"")) && OutputValue.EndsWith(TEXT("\"")))
		{
			// Extract inner content (strip outer quotes)
			FString Inner = OutputValue.Mid(1, OutputValue.Len() - 2);
			// Re-escape backslashes first, then quotes
			Inner.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Inner.ReplaceInline(TEXT("\""), TEXT("\\\""));
			OutputValue = FString::Printf(TEXT("\"%s\""), *Inner);
		}
		Result += FString::Printf(TEXT(" :%s %s"), *PropertyKey, *OutputValue);
	}
	
	// Add named children (pose inputs)
	if (Children.Num() > 0)
	{
		Result += TEXT("\n");
		for (const auto& NamedChild : Children)
		{
			if (NamedChild.Node.IsValid())
			{
				// Output format: :pin-name\n  (child-node ...)
				if (!NamedChild.PinName.IsEmpty())
				{
					Result += FString::Printf(TEXT("%s:%s\n"), *ChildIndentStr, *NamedChild.PinName);
					Result += NamedChild.Node->ToString(Indent + 4) + TEXT("\n");
				}
				else
				{
					Result += NamedChild.Node->ToString(Indent + 2) + TEXT("\n");
				}
			}
		}
		Result += IndentStr;
	}
	
	Result += TEXT(")");
	return Result;
}

void FAnimNodeAST::AddChild(const FString& PinName, TSharedPtr<FAnimNodeAST> ChildNode)
{
	if (ChildNode.IsValid())
	{
		FNamedChild Named;
		Named.PinName = PinName;
		Named.Node = ChildNode;
		Children.Add(Named);
	}
}

void FAnimNodeAST::AddChild(TSharedPtr<FAnimNodeAST> ChildNode)
{
	if (ChildNode.IsValid())
	{
		FNamedChild Named;
		Named.Node = ChildNode;
		Children.Add(Named);
	}
}

float FAnimNodeAST::GetFloatProperty(const FString& Key, float Default) const
{
	const FString* Value = Properties.Find(Key);
	return Value ? FCString::Atof(**Value) : Default;
}

bool FAnimNodeAST::GetBoolProperty(const FString& Key, bool Default) const
{
	const FString* Value = Properties.Find(Key);
	return Value ? Value->ToBool() : Default;
}

FString FAnimNodeAST::GetStringProperty(const FString& Key, const FString& Default) const
{
	const FString* Value = Properties.Find(Key);
	return Value ? *Value : Default;
}

// ========== FLogicalExpr ==========

FString FLogicalExpr::ToString() const
{
	FString Result = FString::Printf(TEXT("(%s"), *Operator);
	for (const auto& Operand : Operands)
	{
		Result += TEXT(" ") + Operand->ToString();
	}
	Result += TEXT(")");
	return Result;
}

// ========== FStateMachineAST ==========

FString FStateMachineAST::ToString(int32 Indent) const
{
	FString IndentStr = FString::ChrN(Indent, ' ');
	FString ChildIndent = FString::ChrN(Indent + 2, ' ');
	FString DeepIndent = FString::ChrN(Indent + 4, ' ');
	FString Result = FString::Printf(TEXT("%s(state-machine \"%s\"\n"), *IndentStr, *Name);
	
	if (!InitialState.IsEmpty())
	{
		Result += FString::Printf(TEXT("%s:initial \"%s\"\n"), *ChildIndent, *InitialState);
	}
	
	// States with their animation subtrees
	if (States.Num() > 0)
	{
		Result += FString::Printf(TEXT("%s:states\n"), *ChildIndent);
		for (const auto& State : States)
		{
			Result += FString::Printf(TEXT("%s(state \"%s\"\n"), *DeepIndent, *State.Name);
			if (State.Animation.IsValid())
			{
				Result += State.Animation->ToString(Indent + 6) + TEXT("\n");
			}
			else
			{
				Result += FString::ChrN(Indent + 6, ' ') + TEXT("(identity-pose)\n");
			}
			Result += DeepIndent + TEXT(")\n");
		}
	}
	
	// Transitions
	if (Transitions.Num() > 0)
	{
		Result += FString::Printf(TEXT("%s:transitions [\n"), *ChildIndent);
		for (const auto& Trans : Transitions)
		{
			Result += FString::Printf(TEXT("%s(%s -> %s"), *DeepIndent, *Trans.FromState, *Trans.ToState);
			if (!FMath::IsNearlyEqual(Trans.BlendDuration, 0.2f))
			{
				Result += FString::Printf(TEXT(" :duration %s"), *FString::SanitizeFloat(Trans.BlendDuration));
			}
			if (Trans.Priority != 0)
			{
				Result += FString::Printf(TEXT(" :priority %d"), Trans.Priority);
			}
			if (Trans.bInterruptible)
			{
				Result += TEXT(" :bidirectional true");
			}
			if (Trans.Condition.IsValid())
			{
				Result += FString::Printf(TEXT(" :rule %s"), *Trans.Condition->ToString());
			}
			// BlueprintLisp export of the full transition graph (for import-side restore)
			if (!Trans.RuleGraph.IsEmpty())
			{
				// Escape inner double-quotes and store as a single quoted string
				FString EscapedGraph = Trans.RuleGraph;
				EscapedGraph.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
				EscapedGraph.ReplaceInline(TEXT("\""), TEXT("\\\""));
				EscapedGraph.ReplaceInline(TEXT("\n"), TEXT("\\n"));
				EscapedGraph.ReplaceInline(TEXT("\r"), TEXT("\\r"));
				Result += FString::Printf(TEXT(" :rule-graph \"%s\""), *EscapedGraph);
			}
			Result += TEXT(")\n");
		}
		Result += FString::Printf(TEXT("%s]\n"), *ChildIndent);
	}
	
	Result += IndentStr + TEXT(")");
	return Result;
}

// ========== FVariableDef ==========

FString FVariableDef::ToString() const
{
	FString TypeStr;
	switch (Type)
	{
		case EPinType::Float:     TypeStr = TEXT("float"); break;
		case EPinType::Int:       TypeStr = TEXT("int"); break;
		case EPinType::Bool:      TypeStr = TEXT("bool"); break;
		case EPinType::Vector:    TypeStr = TEXT("vector"); break;
		case EPinType::Rotator:   TypeStr = TEXT("rotator"); break;
		case EPinType::Transform: TypeStr = TEXT("transform"); break;
		case EPinType::Name:      TypeStr = TEXT("name"); break;
		case EPinType::Enum:      TypeStr = TEXT("enum"); break;
		case EPinType::Object:    TypeStr = TEXT("object"); break;
		case EPinType::Struct:    TypeStr = TEXT("struct"); break;
		case EPinType::Unknown:   TypeStr = TEXT("unknown"); break;
		default:                  TypeStr = TEXT("unknown"); break;
	}
	
	FString Result = FString::Printf(TEXT("(%s :name %s"), *TypeStr, *EscapeQuotedStringForDSL(Name));
	FString ImpliedPinCategory;
	switch (Type)
	{
	case EPinType::Int: ImpliedPinCategory = TEXT("int"); break;
	case EPinType::Bool: ImpliedPinCategory = TEXT("bool"); break;
	case EPinType::Name: ImpliedPinCategory = TEXT("name"); break;
	case EPinType::Object: ImpliedPinCategory = TEXT("object"); break;
	default: break;
	}
	if (!PinCategory.IsEmpty()
		&& (ImpliedPinCategory.IsEmpty()
			|| !PinCategory.Equals(ImpliedPinCategory, ESearchCase::IgnoreCase)))
	{
		Result += FString::Printf(TEXT(" :pin-category %s"), *EscapeQuotedStringForDSL(PinCategory));
	}
	if (!PinSubCategory.IsEmpty() && !PinSubCategory.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		Result += FString::Printf(TEXT(" :pin-subcategory %s"), *EscapeQuotedStringForDSL(PinSubCategory));
	}
	if (!TypeObjectPath.IsEmpty())
	{
		Result += FString::Printf(TEXT(" :type-object (asset %s)"), *EscapeQuotedStringForDSL(TypeObjectPath));
	}
	if (!ContainerType.IsEmpty() && !ContainerType.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		Result += FString::Printf(TEXT(" :container %s"), *ContainerType.ToLower());
	}
	const bool bIsMap = ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase);
	if (bIsMap)
	{
		if (!ValuePinCategory.IsEmpty())
		{
			Result += FString::Printf(TEXT(" :value-pin-category %s"),
				*EscapeQuotedStringForDSL(ValuePinCategory));
		}
		if (!ValuePinSubCategory.IsEmpty()
			&& !ValuePinSubCategory.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			Result += FString::Printf(TEXT(" :value-pin-subcategory %s"),
				*EscapeQuotedStringForDSL(ValuePinSubCategory));
		}
		if (!ValueTypeObjectPath.IsEmpty())
		{
			Result += FString::Printf(TEXT(" :value-type-object (asset %s)"),
				*EscapeQuotedStringForDSL(ValueTypeObjectPath));
		}
	}
	if (bIsReference) Result += TEXT(" :reference true");
	if (bIsConst) Result += TEXT(" :const true");
	if (bIsWeakPointer) Result += TEXT(" :weak true");
	if (bIsUObjectWrapper) Result += TEXT(" :object-wrapper true");
	if (bIsMap && MapEntries.Num() != 0)
	{
		TArray<FMapEntryDef> SortedEntries = MapEntries;
		SortedEntries.Sort([](const FMapEntryDef& A, const FMapEntryDef& B)
		{
			const int32 KeyOrder = A.KeyExpression.Compare(B.KeyExpression, ESearchCase::CaseSensitive);
			return KeyOrder == 0
				? A.ValueExpression.Compare(B.ValueExpression, ESearchCase::CaseSensitive) < 0
				: KeyOrder < 0;
		});
		Result += TEXT(" :default [");
		for (const FMapEntryDef& Entry : SortedEntries)
		{
			Result += FString::Printf(TEXT(" (entry :key %s :value %s)"),
				*Entry.KeyExpression, *Entry.ValueExpression);
		}
		Result += TEXT(" ]");
	}
	else if (!bIsMap && !DefaultValue.IsEmpty())
	{
		Result += FString::Printf(TEXT(" %s"), *DefaultValue);
	}
	Result += TEXT(")");
	return Result;
}

// ========== FCachedPoseDef ==========

FString FCachedPoseDef::GetIdentifier() const
{
	// Convert "Post Layering" -> "Post-Layering", keep as-is if already clean
	FString Id = Name;
	Id.ReplaceInline(TEXT(" "), TEXT("-"));
	return Id;
}

// ========== FHelperGraphDef ==========

FString FHelperGraphDef::ToString(int32 Indent) const
{
	FString IndentStr = FString::ChrN(Indent, ' ');
	FString ChildIndent = FString::ChrN(Indent + 2, ' ');
	FString Result = FString::Printf(TEXT("%s(helper-graph"), *IndentStr);

	if (!Id.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:id %s"), *ChildIndent, *EscapeQuotedStringForDSL(Id));
	}
	if (!GraphName.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:graph-name %s"), *ChildIndent, *EscapeQuotedStringForDSL(GraphName));
	}
	if (!GeneratedVar.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:generated-var %s"), *ChildIndent, *EscapeQuotedStringForDSL(GeneratedVar));
	}

	Result += FString::Printf(TEXT("\n%s:generated-type %s"), *ChildIndent, *PinTypeToAnimLangString(GeneratedType));

	if (!UpdateGroup.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:update-group %s"), *ChildIndent, *EscapeQuotedStringForDSL(UpdateGroup));
	}
	if (!DSL.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:dsl %s"), *ChildIndent, *FormatHelperDSLValue(DSL));
	}

	Result += FString::Printf(TEXT("\n%s)"), *IndentStr);
	return Result;
}

// ========== FLogicGraphDef ==========

FString FLogicGraphDef::ToString(int32 Indent) const
{
	const FString IndentStr = FString::ChrN(Indent, ' ');
	const FString ChildIndent = FString::ChrN(Indent + 2, ' ');
	FString Result = FString::Printf(TEXT("%s(logic-graph"), *IndentStr);
	Result += FString::Printf(TEXT("\n%s:role %s"), *ChildIndent, *EscapeQuotedStringForDSL(Role));
	Result += FString::Printf(TEXT("\n%s:kind %s"), *ChildIndent, *EscapeQuotedStringForDSL(Kind));
	Result += FString::Printf(TEXT("\n%s:graph-name %s"), *ChildIndent, *EscapeQuotedStringForDSL(GraphName));
	if (!SchemaClassPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:schema %s"), *ChildIndent, *EscapeQuotedStringForDSL(SchemaClassPath));
	}
	Result += FString::Printf(TEXT("\n%s:dsl %s"), *ChildIndent, *EscapeQuotedStringForDSL(DSL));
	Result += FString::Printf(TEXT("\n%s)"), *IndentStr);
	return Result;
}

FString FAnimationLayerDef::ToString(int32 Indent) const
{
	const FString I = FString::ChrN(Indent, ' ');
	const FString C = FString::ChrN(Indent + 2, ' ');
	const FString D = FString::ChrN(Indent + 4, ' ');
	FString Result = FString::Printf(TEXT("%s(animation-layer"), *I);
	Result += FString::Printf(TEXT("\n%s:interface %s"), *C, *EscapeQuotedStringForDSL(InterfaceClassPath));
	Result += FString::Printf(TEXT("\n%s:graph-name %s"), *C, *EscapeQuotedStringForDSL(GraphName));
	if (!SchemaClassPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:schema %s"), *C, *EscapeQuotedStringForDSL(SchemaClassPath));
	}
	if (!GraphGuid.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n%s:graph-guid %s"), *C, *EscapeQuotedStringForDSL(GraphGuid));
	}
	for (const FCachedPoseDef& Def : Defines)
	{
		Result += FString::Printf(TEXT("\n%s(define %s"), *C, *Def.GetIdentifier());
		if (Def.Name != Def.GetIdentifier())
		{
			Result += FString::Printf(TEXT(" :cache-name %s"), *EscapeQuotedStringForDSL(Def.Name));
		}
		Result += TEXT("\n");
		Result += Def.Body.IsValid() ? Def.Body->ToString(Indent + 4) : D + TEXT("(identity-pose)");
		Result += FString::Printf(TEXT("\n%s)"), *C);
	}
	if (RootNode.IsValid())
	{
		Result += FString::Printf(TEXT("\n%s:root\n"), *C);
		Result += RootNode->ToString(Indent + 4);
	}
	Result += FString::Printf(TEXT("\n%s)"), *I);
	return Result;
}

FString FAnimDependency::ToString(int32 Indent) const
{
	const FString I = FString::ChrN(Indent, ' ');
	const FString C = FString::ChrN(Indent + 2, ' ');
	FString Result = FString::Printf(TEXT("%s(dependency\n%s:mode %s\n%s:object-path %s\n%s:class-path %s\n%s:role %s"),
		*I, *C, *Mode, *C, *EscapeQuotedStringForDSL(ObjectPath), *C, *EscapeQuotedStringForDSL(ClassPath), *C, *EscapeQuotedStringForDSL(Role));
	if (AssetMetadata.bHasSnapshot)
	{
		Result += FString::Printf(TEXT("\n%s:snapshot (animation-asset-metadata"), *C);
		Result += FString::Printf(TEXT(" :has-root-motion %s :enable-root-motion %s :force-root-lock %s"),
			AssetMetadata.bHasRootMotion ? TEXT("true") : TEXT("false"),
			AssetMetadata.bEnableRootMotion ? TEXT("true") : TEXT("false"),
			AssetMetadata.bForceRootLock ? TEXT("true") : TEXT("false"));
		if (!AssetMetadata.RootMotionRootLock.IsEmpty())
		{
			Result += FString::Printf(TEXT(" :root-motion-root-lock %s"), *EscapeQuotedStringForDSL(AssetMetadata.RootMotionRootLock));
		}
		if (AssetMetadata.Notifies.Num() != 0)
		{
			Result += TEXT(" :notifies [");
			for (const FAnimNotifySnapshot& Notify : AssetMetadata.Notifies)
			{
				Result += FString::Printf(TEXT(" (notify :class-path %s :name %s :time %s :duration %s :state %s)"),
					*EscapeQuotedStringForDSL(Notify.ClassPath), *EscapeQuotedStringForDSL(Notify.Name),
					*FString::SanitizeFloat(Notify.Time), *FString::SanitizeFloat(Notify.Duration), Notify.bIsState ? TEXT("true") : TEXT("false"));
			}
			Result += TEXT(" ]");
		}
		if (AssetMetadata.SyncMarkers.Num() != 0)
		{
			Result += TEXT(" :sync-markers [");
			for (const FAnimSyncMarkerSnapshot& Marker : AssetMetadata.SyncMarkers)
			{
				Result += FString::Printf(TEXT(" (sync-marker :name %s :time %s)"), *EscapeQuotedStringForDSL(Marker.Name), *FString::SanitizeFloat(Marker.Time));
			}
			Result += TEXT(" ]");
		}
		if (AssetMetadata.MontageSections.Num() != 0)
		{
			Result += TEXT(" :montage-sections [");
			for (const FMontageSectionSnapshot& Section : AssetMetadata.MontageSections)
			{
				Result += FString::Printf(TEXT(" (montage-section :name %s :start-time %s :next-section %s)"),
					*EscapeQuotedStringForDSL(Section.Name), *FString::SanitizeFloat(Section.StartTime), *EscapeQuotedStringForDSL(Section.NextSectionName));
			}
			Result += TEXT(" ]");
		}
		if (AssetMetadata.SlotTrackNames.Num() != 0)
		{
			Result += TEXT(" :slot-tracks [");
			for (const FString& Slot : AssetMetadata.SlotTrackNames) Result += TEXT(" ") + EscapeQuotedStringForDSL(Slot);
			Result += TEXT(" ]");
		}
		if (AssetMetadata.UnsupportedFields.Num() != 0)
		{
			Result += TEXT(" :unsupported [");
			for (const FString& Field : AssetMetadata.UnsupportedFields) Result += TEXT(" ") + EscapeQuotedStringForDSL(Field);
			Result += TEXT(" ]");
		}
		Result += TEXT(")");
	}
	if (TypedSnapshot.bHasSnapshot)
	{
		Result += FString::Printf(TEXT("\n%s:typed-snapshot (asset-structure :kind %s :stable-hash %s"),
			*C, *EscapeQuotedStringForDSL(TypedSnapshot.Kind), *EscapeQuotedStringForDSL(TypedSnapshot.StableHash));
		if (TypedSnapshot.Fields.Num() != 0)
		{
			Result += TEXT(" :fields [");
			for (const FExternalAssetSnapshotField& Field : TypedSnapshot.Fields)
			{
				Result += FString::Printf(TEXT(" (field :path %s :type %s :value %s)"),
					*EscapeQuotedStringForDSL(Field.Path), *EscapeQuotedStringForDSL(Field.Type), *EscapeQuotedStringForDSL(Field.Value));
			}
			Result += TEXT(" ]");
		}
		if (TypedSnapshot.ObjectReferences.Num() != 0)
		{
			Result += TEXT(" :object-references [");
			for (const FString& Reference : TypedSnapshot.ObjectReferences)
			{
				Result += TEXT(" ") + EscapeQuotedStringForDSL(Reference);
			}
			Result += TEXT(" ]");
		}
		Result += TEXT(")");
	}
	Result += FString::Printf(TEXT("\n%s)"), *I);
	return Result;
}

// ========== FAnimGraphAST ==========

void FAnimGraphAST::VisitNodes(TFunctionRef<void(const TSharedPtr<FAnimNodeAST>&)> Visitor) const
{
	TFunction<void(const TSharedPtr<FAnimNodeAST>&)> VisitTree;
	VisitTree = [&Visitor, &VisitTree](const TSharedPtr<FAnimNodeAST>& Node)
	{
		if (!Node.IsValid()) return;
		Visitor(Node);
		for (const FNamedChild& Child : Node->Children) VisitTree(Child.Node);
	};

	VisitTree(RootNode);
	for (const FCachedPoseDef& Definition : Defines) VisitTree(Definition.Body);
	for (const FAnimationLayerDef& Layer : AnimationLayers)
	{
		VisitTree(Layer.RootNode);
		for (const FCachedPoseDef& Definition : Layer.Defines) VisitTree(Definition.Body);
	}
}

FString FAnimGraphAST::ToString() const
{
	FString Result = FString::Printf(TEXT("(anim-blueprint \"%s\"\n"), *Name);
	
	// Skeleton path
	if (!SkeletonPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("  :skeleton \"%s\"\n"), *SkeletonPath);
	}

	if (!Metadata.RootMotionMode.IsEmpty())
	{
		Result += FString::Printf(TEXT("  (metadata :root-motion-mode %s)\n"), *EscapeQuotedStringForDSL(Metadata.RootMotionMode));
	}

	if (RigImports.Num() != 0)
	{
		TArray<FAnimLispImport> SortedImports = RigImports;
		SortedImports.Sort([](const FAnimLispImport& A, const FAnimLispImport& B)
		{
			return A.Alias < B.Alias;
		});
		for (const FAnimLispImport& Import : SortedImports)
		{
			Result += FString::Printf(TEXT("  (import-rig :asset %s :as %s"),
				*EscapeQuotedStringForDSL(Import.Target.AssetPath), *Import.Alias);
			if (!Import.ExpectedHash.IsEmpty())
			{
				Result += FString::Printf(TEXT(" :expected-hash %s"), *EscapeQuotedStringForDSL(Import.ExpectedHash));
			}
			Result += TEXT(")\n");
		}
	}

	if (Dependencies.Num() != 0)
	{
		TArray<FAnimDependency> SortedDependencies = Dependencies;
		SortedDependencies.Sort([](const FAnimDependency& A, const FAnimDependency& B)
		{
			if (A.ObjectPath != B.ObjectPath) return A.ObjectPath < B.ObjectPath;
			if (A.ClassPath != B.ClassPath) return A.ClassPath < B.ClassPath;
			return A.Role < B.Role;
		});
		Result += TEXT("  (dependencies\n");
		for (const FAnimDependency& Dependency : SortedDependencies)
		{
			Result += Dependency.ToString(4) + TEXT("\n");
		}
		Result += TEXT("  )\n");
	}
	
	// Implemented interfaces (AnimLayerInterfaces)
	if (ImplementedInterfaces.Num() > 0)
	{
		Result += TEXT("  :implements [\n");
		for (const FString& InterfacePath : ImplementedInterfaces)
		{
			Result += FString::Printf(TEXT("    (interface \"%s\")\n"), *InterfacePath);
		}
		Result += TEXT("  ]\n");
	}
	
	// Variables
	if (Variables.Num() > 0)
	{
		Result += TEXT("  :variables [\n");
		for (const auto& Var : Variables)
		{
			Result += TEXT("    ") + Var.ToString() + TEXT("\n");
		}
		Result += TEXT("  ]\n");
	}

	// Helper graphs for complex value bindings
	if (HelperGraphs.Num() > 0)
	{
		TArray<FHelperGraphDef> SortedHelpers = HelperGraphs;
		SortedHelpers.Sort([](const FHelperGraphDef& A, const FHelperGraphDef& B)
		{
			if (A.Id == B.Id)
			{
				return A.GraphName < B.GraphName;
			}
			return A.Id < B.Id;
		});

		Result += TEXT("  (helpers\n");
		for (const FHelperGraphDef& Helper : SortedHelpers)
		{
			Result += Helper.ToString(4) + TEXT("\n");
		}
		Result += TEXT("  )\n");
	}

	if (bHasLogicGraphsBlock || LogicGraphs.Num() > 0)
	{
		TArray<FLogicGraphDef> SortedGraphs = LogicGraphs;
		SortedGraphs.StableSort([](const FLogicGraphDef& A, const FLogicGraphDef& B)
		{
			if (A.Role != B.Role)
			{
				return A.Role == TEXT("event");
			}
			return A.GraphName < B.GraphName;
		});

		Result += TEXT("  (logic-graphs\n");
		for (const FLogicGraphDef& Graph : SortedGraphs)
		{
			Result += Graph.ToString(4) + TEXT("\n");
		}
		Result += TEXT("  )\n");
	}

	if (AnimationLayers.Num() != 0)
	{
		TArray<FAnimationLayerDef> SortedLayers = AnimationLayers;
		SortedLayers.Sort([](const FAnimationLayerDef& A, const FAnimationLayerDef& B)
		{
			if (A.InterfaceClassPath != B.InterfaceClassPath) return A.InterfaceClassPath < B.InterfaceClassPath;
			return A.GraphName < B.GraphName;
		});
		Result += TEXT("  (animation-layers\n");
		for (const FAnimationLayerDef& Layer : SortedLayers)
		{
			Result += Layer.ToString(4) + TEXT("\n");
		}
		Result += TEXT("  )\n");
	}
	
	// Defines (SaveCachedPose -> (define name body))
	if (Defines.Num() > 0)
	{
		Result += TEXT("\n");
		for (const auto& Def : Defines)
		{
			Result += FString::Printf(TEXT("  (define %s"), *Def.GetIdentifier());
			if (Def.Name != Def.GetIdentifier())
			{
				Result += FString::Printf(TEXT(" :cache-name %s"), *EscapeQuotedStringForDSL(Def.Name));
			}
			Result += TEXT("\n");
			if (Def.Body.IsValid())
			{
				Result += Def.Body->ToString(4) + TEXT(")\n\n");
			}
			else
			{
				Result += TEXT("    (identity-pose))\n\n");
			}
		}
	}
	
	// Root node (anim-graph)
	if (RootNode.IsValid())
	{
		Result += TEXT("  :anim-graph\n");
		Result += RootNode->ToString(4) + TEXT("\n");
	}
	
	Result += TEXT(")");
	return Result;
}

FString FAnimGraphAST::ToSExpression(bool bPrettyPrint, int32 IndentSize) const
{
	// For now, just use ToString
	// TODO: Implement proper S-expression formatting
	return ToString();
}

// ========== FTypeChecker ==========

bool FTypeChecker::Check(const TSharedPtr<FAnimGraphAST>& AST, TArray<FTypeError>& OutErrors)
{
	// TODO: Implement type checking
	return true;
}

EPinType FTypeChecker::GetNodeOutputType(const FString& NodeType)
{
	// Most animation nodes output Pose
	return EPinType::Pose;
}

TArray<EPinType> FTypeChecker::GetNodeInputTypes(const FString& NodeType)
{
	// TODO: Implement based on node type
	return {EPinType::Pose};
}

bool FTypeChecker::IsCompatible(EPinType From, EPinType To)
{
	if (From == To) return true;
	
	// Int can be converted to Float
	if (From == EPinType::Int && To == EPinType::Float) return true;
	
	return false;
}
