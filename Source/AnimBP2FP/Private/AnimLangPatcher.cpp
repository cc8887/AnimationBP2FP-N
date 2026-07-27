// AnimLangPatcher.cpp - Incremental Blueprint Patcher Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangPatcher.h"
#include "AnimLangVariableCodec.h"

#if WITH_EDITOR

#include "AnimBPExporter.h"
#include "AnimLangParser.h"
#include "AnimLangDiffer.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_Base.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimLangPatcher, Log, All);

// ========== FAnimLangPatchResult ==========

FString FAnimLangPatchResult::ToString() const
{
	FString Result;
	Result += FString::Printf(TEXT("Patch %s: %d applied, %d failed, %d warnings\n"),
		bSuccess ? TEXT("succeeded") : TEXT("failed"),
		AppliedOps.Num(), FailedOps.Num(), Warnings.Num());
	
	if (AppliedOps.Num() > 0)
	{
		Result += TEXT("Applied:\n");
		for (const FString& Op : AppliedOps)
		{
			Result += TEXT("  ✓ ") + Op + TEXT("\n");
		}
	}
	
	if (FailedOps.Num() > 0)
	{
		Result += TEXT("Failed:\n");
		for (const FString& Op : FailedOps)
		{
			Result += TEXT("  ✗ ") + Op + TEXT("\n");
		}
	}
	
	if (Warnings.Num() > 0)
	{
		Result += TEXT("Warnings:\n");
		for (const FString& W : Warnings)
		{
			Result += TEXT("  ⚠ ") + W + TEXT("\n");
		}
	}
	
	return Result;
}

// ========== Full Pipeline ==========

FAnimLangPatchResult FAnimLangPatcher::IncrementalUpdate(
	UAnimBlueprint* Blueprint,
	const FString& NewDSLCode)
{
	FAnimLangPatchResult Result;
	
	if (!Blueprint)
	{
		Result.bSuccess = false;
		Result.FailedOps.Add(TEXT("Null blueprint"));
		return Result;
	}
	
	// Step 1: Export current state
	TSharedPtr<FAnimGraphAST> OldAST = FAnimBPExporter::ExportToAST(Blueprint);
	if (!OldAST.IsValid())
	{
		Result.bSuccess = false;
		Result.FailedOps.Add(TEXT("Failed to export current blueprint to AST"));
		return Result;
	}
	
	// Step 2: Parse new DSL
	TArray<FAnimLangParseError> ParseErrors;
	TSharedPtr<FAnimGraphAST> NewAST = FAnimLangParser::Parse(NewDSLCode, ParseErrors);
	
	for (const auto& Err : ParseErrors)
	{
		Result.Warnings.Add(FString::Printf(TEXT("Parse: %s"), *Err.ToString()));
	}
	
	if (!NewAST.IsValid())
	{
		Result.bSuccess = false;
		Result.FailedOps.Add(TEXT("Failed to parse new DSL code"));
		return Result;
	}
	
	// Step 3: Compute diff
	FAnimLangDiffResult Diff = FAnimLangDiffer::Diff(OldAST, NewAST);
	
	if (!Diff.HasChanges())
	{
		Result.bSuccess = true;
		Result.Warnings.Add(TEXT("No changes detected between current and new DSL"));
		return Result;
	}
	
	UE_LOG(LogAnimLangPatcher, Log, TEXT("Diff computed: %s"), *Diff.ToSummary());
	
	// Step 4: Apply diff
	Result = Apply(Blueprint, Diff, NewAST);
	
	// Step 5: Compile
	if (Result.bSuccess)
	{
		FString CompileError;
		if (!CompileBlueprint(Blueprint, CompileError))
		{
			Result.Warnings.Add(FString::Printf(TEXT("Compilation warning: %s"), *CompileError));
		}
	}
	
	return Result;
}

// ========== Apply Diff ==========

FAnimLangPatchResult FAnimLangPatcher::Apply(
	UAnimBlueprint* Blueprint,
	const FAnimLangDiffResult& Diff,
	const TSharedPtr<FAnimGraphAST>& NewAST)
{
	FAnimLangPatchResult Result;
	Result.bSuccess = true;
	
	if (!Blueprint)
	{
		Result.bSuccess = false;
		Result.FailedOps.Add(TEXT("Null blueprint"));
		return Result;
	}
	
	// Apply each diff entry
	for (const FAnimLangDiffEntry& Entry : Diff.Entries)
	{
		bool bApplied = false;
		
		switch (Entry.Op)
		{
		case EAnimLangDiffOp::PropertyChanged:
		case EAnimLangDiffOp::PropertyAdded:
		case EAnimLangDiffOp::PropertyRemoved:
			bApplied = ApplyPropertyChange(Blueprint, Entry, Result.Warnings);
			break;
			
		case EAnimLangDiffOp::VariableAdded:
		case EAnimLangDiffOp::VariableRemoved:
		case EAnimLangDiffOp::VariableChanged:
			bApplied = ApplyVariableChange(Blueprint, Entry, NewAST, Result.Warnings);
			break;
			
		case EAnimLangDiffOp::NodeAdded:
		case EAnimLangDiffOp::NodeRemoved:
		case EAnimLangDiffOp::ChildAdded:
		case EAnimLangDiffOp::ChildRemoved:
		case EAnimLangDiffOp::DefineAdded:
		case EAnimLangDiffOp::DefineRemoved:
		case EAnimLangDiffOp::DefineBodyChanged:
		case EAnimLangDiffOp::RootChanged:
			bApplied = ApplyNodeStructuralChange(Blueprint, Entry, NewAST, Result.Warnings);
			break;
			
		default:
			Result.Warnings.Add(FString::Printf(TEXT("Unhandled diff op: %s"), *Entry.ToString()));
			break;
		}
		
		if (bApplied)
		{
			Result.AppliedOps.Add(Entry.ToString());
		}
		else
		{
			Result.FailedOps.Add(Entry.ToString());
		}
	}
	
	if (Result.FailedOps.Num() > 0)
	{
		Result.bSuccess = false;
	}
	else
	{
		FString MapDefaultError;
		if (!ApplyMapVariableDefaults(Blueprint, NewAST->Variables, MapDefaultError))
		{
			Result.bSuccess = false;
			Result.FailedOps.Add(FString::Printf(TEXT("Map variable defaults: %s"), *MapDefaultError));
		}
	}
	
	return Result;
}

// ========== Property Change ==========

bool FAnimLangPatcher::ApplyPropertyChange(
	UAnimBlueprint* Blueprint,
	const FAnimLangDiffEntry& Entry,
	TArray<FString>& OutWarnings)
{
	// Find the AnimGraph
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph->GetFName().ToString().Contains(TEXT("AnimGraph")))
		{
			AnimGraph = Graph;
			break;
		}
	}
	
	if (!AnimGraph)
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph->GetFName().ToString().Contains(TEXT("AnimGraph")))
			{
				AnimGraph = Graph;
				break;
			}
		}
	}
	
	if (!AnimGraph)
	{
		OutWarnings.Add(TEXT("Could not find AnimGraph"));
		return false;
	}
	
	// Find the node
	UAnimGraphNode_Base* Node = FindNodeByPath(AnimGraph, Entry.NodePath);
	if (!Node)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not find node at path: %s"), *Entry.NodePath));
		return false;
	}
	
	// Apply the property change
	if (Entry.Op == EAnimLangDiffOp::PropertyChanged || Entry.Op == EAnimLangDiffOp::PropertyAdded)
	{
		return SetNodeProperty(Node, Entry.PropertyKey, Entry.NewValue);
	}
	else if (Entry.Op == EAnimLangDiffOp::PropertyRemoved)
	{
		// Resetting to default — set to empty string or the pin's default
		return SetNodeProperty(Node, Entry.PropertyKey, TEXT(""));
	}
	
	return false;
}

// ========== Variable Change ==========

bool FAnimLangPatcher::ApplyVariableChange(
	UAnimBlueprint* Blueprint,
	const FAnimLangDiffEntry& Entry,
	const TSharedPtr<FAnimGraphAST>& NewAST,
	TArray<FString>& OutWarnings)
{
	if (!Blueprint || !NewAST.IsValid()) return false;
	
	if (Entry.Op == EAnimLangDiffOp::VariableAdded)
	{
		// Find the variable definition in the new AST
		const FVariableDef* NewVar = nullptr;
		for (const FVariableDef& Var : NewAST->Variables)
		{
			if (Var.Name == Entry.VariableName)
			{
				NewVar = &Var;
				break;
			}
		}
		
		if (!NewVar)
		{
			OutWarnings.Add(FString::Printf(TEXT("Variable '%s' not found in new AST"), *Entry.VariableName));
			return false;
		}
		
		// Check if variable already exists
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			if (ExistingVar.VarName == FName(*Entry.VariableName))
			{
				// Already exists — skip
				return true;
			}
		}
		
		FEdGraphPinType PinType;
		FString TypeError;
		if (!FAnimLangVariableCodec::BuildPinType(*NewVar, PinType, TypeError))
		{
			OutWarnings.Add(FString::Printf(TEXT("Variable '%s' type is unsupported: %s"), *Entry.VariableName, *TypeError));
			return false;
		}
		
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Entry.VariableName), PinType, NewVar->DefaultValue))
		{
			OutWarnings.Add(FString::Printf(TEXT("Failed to add variable '%s'"), *Entry.VariableName));
			return false;
		}
		const int32 AddedIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Entry.VariableName));
		if (AddedIndex == INDEX_NONE)
		{
			OutWarnings.Add(FString::Printf(TEXT("Added variable '%s' cannot be found"), *Entry.VariableName));
			return false;
		}
		Blueprint->NewVariables[AddedIndex].VarType = PinType;
		Blueprint->NewVariables[AddedIndex].DefaultValue = NewVar->DefaultValue;
		return true;
	}
	else if (Entry.Op == EAnimLangDiffOp::VariableRemoved)
	{
		// Remove the variable
		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Entry.VariableName));
		return true;
	}
	else if (Entry.Op == EAnimLangDiffOp::VariableChanged)
	{
		// Variable type/default changed — remove and re-add
		// Find the new definition
		const FVariableDef* NewVar = nullptr;
		for (const FVariableDef& Var : NewAST->Variables)
		{
			if (Var.Name == Entry.VariableName)
			{
				NewVar = &Var;
				break;
			}
		}
		
		if (!NewVar)
		{
			OutWarnings.Add(FString::Printf(TEXT("Variable '%s' not found in new AST for change"), *Entry.VariableName));
			return false;
		}
		
		FEdGraphPinType PinType;
		FString TypeError;
		if (!FAnimLangVariableCodec::BuildPinType(*NewVar, PinType, TypeError))
		{
			OutWarnings.Add(FString::Printf(TEXT("Variable '%s' type is unsupported: %s"), *Entry.VariableName, *TypeError));
			return false;
		}

		const int32 ExistingIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Entry.VariableName));
		if (ExistingIndex == INDEX_NONE)
		{
			OutWarnings.Add(FString::Printf(TEXT("Variable '%s' no longer exists for type change"), *Entry.VariableName));
			return false;
		}
		Blueprint->NewVariables[ExistingIndex].VarType = PinType;
		Blueprint->NewVariables[ExistingIndex].DefaultValue = NewVar->DefaultValue;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return true;
	}
	
	return false;
}

bool FAnimLangPatcher::ApplyMapVariableDefaults(
	UAnimBlueprint* Blueprint,
	const TArray<FVariableDef>& Variables,
	FString& OutError)
{
	const bool bHasMapEntries = Variables.ContainsByPredicate([](const FVariableDef& Variable)
	{
		return Variable.ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase)
			&& !Variable.MapEntries.IsEmpty();
	});
	if (!bHasMapEntries)
	{
		return true;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint,
		EBlueprintCompileOptions::RegenerateSkeletonOnly
		| EBlueprintCompileOptions::SkipGarbageCollection
		| EBlueprintCompileOptions::SkipSave);
	for (const FVariableDef& Variable : Variables)
	{
		if (!Variable.ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase)
			|| Variable.MapEntries.IsEmpty())
		{
			continue;
		}

		FString DefaultText;
		if (!FAnimLangVariableCodec::BuildMapDefaultText(*Blueprint, Variable, DefaultText, OutError))
		{
			return false;
		}
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(
			Blueprint, FName(*Variable.Name));
		if (VariableIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("map variable '%s' could not be found after skeleton compile"), *Variable.Name);
			return false;
		}
		Blueprint->NewVariables[VariableIndex].DefaultValue = MoveTemp(DefaultText);
	}
	return true;
}

// ========== Structural Change ==========

bool FAnimLangPatcher::ApplyNodeStructuralChange(
	UAnimBlueprint* Blueprint,
	const FAnimLangDiffEntry& Entry,
	const TSharedPtr<FAnimGraphAST>& NewAST,
	TArray<FString>& OutWarnings)
{
	// TODO: Implement node addition/removal/restructuring
	// This is the most complex operation, requiring:
	//   - Creating new UAnimGraphNode instances
	//   - Connecting/disconnecting pins
	//   - Rebuilding state machine internals
	OutWarnings.Add(FString::Printf(TEXT("Structural changes not yet implemented: %s"), *Entry.ToString()));
	return false;
}

// ========== Node Finding ==========

UAnimGraphNode_Base* FAnimLangPatcher::FindNodeByPath(UEdGraph* AnimGraph, const FString& NodePath)
{
	if (!AnimGraph || NodePath.IsEmpty())
	{
		return nullptr;
	}
	
	// Parse path segments (e.g. "root.blend-pose-0.sequence-player")
	TArray<FString> Segments;
	NodePath.ParseIntoArray(Segments, TEXT("."), true);
	
	if (Segments.Num() == 0) return nullptr;
	
	// Helper: normalize a name for matching (lowercase, no spaces/hyphens/underscores)
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};
	
	// Helper: convert kebab-case to CamelCase
	auto KebabToCamel = [](const FString& Input) -> FString
	{
		FString Result;
		bool bCapNext = true;
		for (int32 i = 0; i < Input.Len(); i++)
		{
			TCHAR Ch = Input[i];
			if (Ch == '-' || Ch == '_')
			{
				bCapNext = true;
				continue;
			}
			if (bCapNext)
			{
				Result += FChar::ToUpper(Ch);
				bCapNext = false;
			}
			else
			{
				Result += Ch;
			}
		}
		return Result;
	};
	
	// Find root starting point
	UAnimGraphNode_Base* CurrentNode = nullptr;
	
	for (UEdGraphNode* GraphNode : AnimGraph->Nodes)
	{
		UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(GraphNode);
		if (!AnimNode) continue;
		
		FString ClassName = AnimNode->GetClass()->GetName();
		if (ClassName.Contains(TEXT("Root")))
		{
			CurrentNode = AnimNode;
			break;
		}
	}
	
	if (!CurrentNode) return nullptr;
	
	// Traverse path segments (skip "root")
	for (int32 i = 1; i < Segments.Num(); i++)
	{
		const FString& Segment = Segments[i];
		FString NormalizedSegment = Normalize(Segment);
		FString CamelSegment = KebabToCamel(Segment);
		
		bool bFound = false;
		
		// Try to match by pin name first (most reliable)
		for (UEdGraphPin* Pin : CurrentNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
			if (Pin->LinkedTo.Num() == 0) continue;
			
			FString PinNameStr = Pin->PinName.ToString();
			
			// Check if this pin name matches the segment
			bool bMatch = PinNameStr.Equals(CamelSegment, ESearchCase::IgnoreCase)
				|| Normalize(PinNameStr) == NormalizedSegment;
			
			if (bMatch)
			{
				UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
				if (Child)
				{
					CurrentNode = Child;
					bFound = true;
					break;
				}
			}
		}
		
		if (bFound) continue;
		
		// Fall back: match by node type (from kebab-case class name)
		for (UEdGraphPin* Pin : CurrentNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
			if (Pin->LinkedTo.Num() == 0) continue;
			
			UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(Pin->LinkedTo[0]->GetOwningNode());
			if (Child)
			{
				// Convert the child's class name to a normalized form for comparison
				FString ChildClassName = Child->GetClass()->GetName();
				ChildClassName.RemoveFromStart(TEXT("AnimGraphNode_"));
				
				if (Normalize(ChildClassName) == NormalizedSegment)
				{
					CurrentNode = Child;
					bFound = true;
					break;
				}
			}
		}
		
		if (!bFound)
		{
			UE_LOG(LogAnimLangPatcher, Warning, TEXT("FindNodeByPath: could not resolve segment '%s' at depth %d"), *Segment, i);
			return nullptr;
		}
	}
	
	return CurrentNode;
}

// ========== Property Setting ==========

bool FAnimLangPatcher::SetNodeProperty(UAnimGraphNode_Base* Node, const FString& PropertyKey, const FString& Value)
{
	if (!Node) return false;
	
	// Helper: normalize for comparison
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};
	
	FString NormalizedKey = Normalize(PropertyKey);
	
	// Strip quotes from value if present
	FString CleanValue = Value;
	if (CleanValue.StartsWith(TEXT("\"")) && CleanValue.EndsWith(TEXT("\"")))
	{
		CleanValue = CleanValue.Mid(1, CleanValue.Len() - 2);
	}
	
	// Find the pin by normalized name matching (ignoring hyphens/underscores/case)
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) continue;  // Skip pose pins
		
		FString PinNormalized = Normalize(Pin->PinName.ToString());
		
		if (PinNormalized == NormalizedKey)
		{
			Pin->DefaultValue = CleanValue;
			Node->PinDefaultValueChanged(Pin);
			return true;
		}
	}
	
	UE_LOG(LogAnimLangPatcher, Warning, TEXT("Could not find pin '%s' on node '%s'"), 
		*PropertyKey, *Node->GetClass()->GetName());
	return false;
}

// ========== Compilation ==========

bool FAnimLangPatcher::CompileBlueprint(UAnimBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Null blueprint");
		return false;
	}
	
	// Mark dirty
	Blueprint->Modify();
	
	// Compile
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	
	// Check for errors
	if (Blueprint->Status == BS_Error)
	{
		OutError = TEXT("Blueprint has compilation errors after patching");
		return false;
	}
	
	return true;
}

#endif // WITH_EDITOR
