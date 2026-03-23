// AnimLangAST.cpp - Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include \"AnimLangAST.h\"

// ========== FAnimNodeAST ==========

FString FAnimNodeAST::ToString(int32 Indent) const
{
	FString IndentStr = FString::ChrN(Indent, ' ');
	FString Result = FString::Printf(TEXT(\"%s(%s\"), *IndentStr, *NodeType);
	
	// Add properties
	for (const auto& Pair : Properties)
	{
		Result += FString::Printf(TEXT(\" :%s %s\"), *Pair.Key, *Pair.Value);
	}
	
	// Add children
	if (Children.Num() > 0)
	{
		Result += TEXT(\"\n\");
		for (const auto& Child : Children)
		{
			Result += Child->ToString(Indent + 2) + TEXT(\"\n\");
		}
		Result += IndentStr;
	}
	
	Result += TEXT(\")\");
	return Result;
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
	FString Result = FString::Printf(TEXT(\"(%s\"), *Operator);
	for (const auto& Operand : Operands)
	{
		Result += TEXT(\" \") + Operand->ToString();
	}
	Result += TEXT(\")\");
	return Result;
}

// ========== FStateMachineAST ==========

FString FStateMachineAST::ToString(int32 Indent) const
{
	FString IndentStr = FString::ChrN(Indent, ' ');
	FString Result = FString::Printf(TEXT(\"%s(state-machine :%s\n\"), *IndentStr, *Name);
	
	Result += FString::Printf(TEXT(\"%s  :initial :%s\n\"), *IndentStr, *InitialState);
	
	// States
	Result += FString::Printf(TEXT(\"%s  :states [\n\"), *IndentStr);
	for (const auto& State : States)
	{
		Result += FString::Printf(TEXT(\"%s    (state :%s ...)\n\"), *IndentStr, *State.Name);
	}
	Result += FString::Printf(TEXT(\"%s  ]\n\"), *IndentStr);
	
	// Transitions
	Result += FString::Printf(TEXT(\"%s  :transitions [\n\"), *IndentStr);
	for (const auto& Trans : Transitions)
	{
		Result += FString::Printf(TEXT(\"%s    (:%s -> :%s ...)\n\"), 
			*IndentStr, *Trans.FromState, *Trans.ToState);
	}
	Result += FString::Printf(TEXT(\"%s  ])\n\"), *IndentStr);
	
	return Result;
}

// ========== FVariableDef ==========

FString FVariableDef::ToString() const
{
	FString TypeStr;
	switch (Type)
	{
		case EPinType::Float:  TypeStr = TEXT(\"float\"); break;
		case EPinType::Int:    TypeStr = TEXT(\"int\"); break;
		case EPinType::Bool:   TypeStr = TEXT(\"bool\"); break;
		case EPinType::Vector: TypeStr = TEXT(\"vector\"); break;
		default:               TypeStr = TEXT(\"unknown\"); break;
	}
	
	return FString::Printf(TEXT(\"(%s :%s %s)\"), *TypeStr, *Name, *DefaultValue);
}

// ========== FAnimGraphAST ==========

FString FAnimGraphAST::ToString() const
{
	FString Result = FString::Printf(TEXT(\"(anim-blueprint \\"%s\\"\n\"), *Name);
	
	// Variables
	if (Variables.Num() > 0)
	{
		Result += TEXT(\"  :variables [\n\");
		for (const auto& Var : Variables)
		{
			Result += TEXT(\"    \") + Var.ToString() + TEXT(\"\n\");
		}
		Result += TEXT(\"  ]\n\");
	}
	
	// Root node
	if (RootNode.IsValid())
	{
		Result += TEXT(\"  :anim-graph\n\");
		Result += RootNode->ToString(4) + TEXT(\"\n\");
	}
	
	Result += TEXT(\")\");
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