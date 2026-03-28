// AnimLangAST.cpp - Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangAST.h"

// ========== FAnimNodeAST ==========

FString FAnimNodeAST::ToString(int32 Indent) const
{
	FString IndentStr = FString::ChrN(Indent, ' ');
	FString ChildIndentStr = FString::ChrN(Indent + 2, ' ');
	FString Result = FString::Printf(TEXT("%s(%s"), *IndentStr, *NodeType);
	
	// Add properties (non-pose parameters)
	for (const auto& Pair : Properties)
	{
		// If the value is a quoted string, re-escape internal quotes for correct DSL output
		FString OutputValue = Pair.Value;
		if (OutputValue.StartsWith(TEXT("\"")) && OutputValue.EndsWith(TEXT("\"")))
		{
			// Extract inner content (strip outer quotes)
			FString Inner = OutputValue.Mid(1, OutputValue.Len() - 2);
			// Re-escape backslashes first, then quotes
			Inner.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Inner.ReplaceInline(TEXT("\""), TEXT("\\\""));
			OutputValue = FString::Printf(TEXT("\"%s\""), *Inner);
		}
		Result += FString::Printf(TEXT(" :%s %s"), *Pair.Key, *OutputValue);
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
		case EPinType::Float:  TypeStr = TEXT("float"); break;
		case EPinType::Int:    TypeStr = TEXT("int"); break;
		case EPinType::Bool:   TypeStr = TEXT("bool"); break;
		case EPinType::Vector: TypeStr = TEXT("vector"); break;
		default:               TypeStr = TEXT("unknown"); break;
	}
	
	return FString::Printf(TEXT("(%s :%s %s)"), *TypeStr, *Name, *DefaultValue);
}

// ========== FCachedPoseDef ==========

FString FCachedPoseDef::GetIdentifier() const
{
	// Convert "Post Layering" -> "Post-Layering", keep as-is if already clean
	FString Id = Name;
	Id.ReplaceInline(TEXT(" "), TEXT("-"));
	return Id;
}

// ========== FAnimGraphAST ==========

FString FAnimGraphAST::ToString() const
{
	FString Result = FString::Printf(TEXT("(anim-blueprint \"%s\"\n"), *Name);
	
	// Skeleton path
	if (!SkeletonPath.IsEmpty())
	{
		Result += FString::Printf(TEXT("  :skeleton \"%s\"\n"), *SkeletonPath);
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
	
	// Defines (SaveCachedPose -> (define name body))
	if (Defines.Num() > 0)
	{
		Result += TEXT("\n");
		for (const auto& Def : Defines)
		{
			Result += FString::Printf(TEXT("  (define %s\n"), *Def.GetIdentifier());
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