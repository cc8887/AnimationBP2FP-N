// AnimBPImporter.cpp - DSL to Animation Blueprint Importer
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBPImporter.h"

#if WITH_EDITOR

#include "AnimLangParser.h"
#include "AnimBPExporter.h"
#include "AnimLangDiffer.h"
#include "AnimLangPatcher.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_BlendListBase.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_ModifyCurve.h"
#include "AnimGraphNode_BlendListBase.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_LinkedAnimLayer.h"

#include "Factories/AnimBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimBPImporter, Log, All);

// Helper: strip surrounding double quotes from a string
static FString StripQuotes(const FString& Input)
{
	FString Result = Input;
	if (Result.StartsWith(TEXT("\"")) && Result.EndsWith(TEXT("\"")))
	{
		Result = Result.Mid(1, Result.Len() - 2);
	}
	return Result;
}

// ========== Name Conversion Helpers ==========

FString FAnimBPImporter::KebabToCamel(const FString& Input)
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
}

FString FAnimBPImporter::KebabToCamelClassName(const FString& KebabName)
{
	// Special mappings for known node types
	static TMap<FString, FString> SpecialMappings;
	if (SpecialMappings.Num() == 0)
	{
		SpecialMappings.Add(TEXT("sequence-player"), TEXT("AnimGraphNode_SequencePlayer"));
		SpecialMappings.Add(TEXT("blendspace-player"), TEXT("AnimGraphNode_BlendSpacePlayer"));
		SpecialMappings.Add(TEXT("blend"), TEXT("AnimGraphNode_TwoWayBlend"));
		SpecialMappings.Add(TEXT("apply-additive"), TEXT("AnimGraphNode_ApplyAdditive"));
		SpecialMappings.Add(TEXT("layered-bone-blend"), TEXT("AnimGraphNode_LayeredBoneBlend"));
		SpecialMappings.Add(TEXT("state-machine"), TEXT("AnimGraphNode_StateMachine"));
		SpecialMappings.Add(TEXT("identity-pose"), TEXT(""));  // No node needed
		SpecialMappings.Add(TEXT("root"), TEXT("AnimGraphNode_Root"));
		// Note: blend-list is NOT here — it uses :class property for the actual type
	}
	
	const FString* Found = SpecialMappings.Find(KebabName);
	if (Found)
	{
		return *Found;
	}
	
	// Generic conversion: kebab-case -> AnimGraphNode_CamelCase
	return TEXT("AnimGraphNode_") + KebabToCamel(KebabName);
}

// ========== Node Class Discovery ==========

UClass* FAnimBPImporter::FindAnimNodeClass(const FString& DSLNodeType)
{
	FString ClassName;
	
	// If the input already looks like a UE class name, use it directly
	if (DSLNodeType.StartsWith(TEXT("AnimGraphNode_")))
	{
		ClassName = DSLNodeType;
	}
	else
	{
		ClassName = KebabToCamelClassName(DSLNodeType);
	}
	
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}
	
	// Try to find the UClass by name — iterate all classes
	// (FindObject with ANY_PACKAGE is deprecated in UE5)
	
	// Fallback: iterate all classes to find by name suffix
	FString ShortName = ClassName;
	ShortName.RemoveFromStart(TEXT("AnimGraphNode_"));
	
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* TestClass = *It;
		if (TestClass->IsChildOf(UAnimGraphNode_Base::StaticClass()) && !TestClass->HasAnyClassFlags(CLASS_Abstract))
		{
			FString TestName = TestClass->GetName();
			if (TestName == ClassName || TestName.EndsWith(ShortName))
			{
				return TestClass;
			}
		}
	}
	
	UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not find UClass for node type '%s' (tried '%s')"), *DSLNodeType, *ClassName);
	return nullptr;
}

// ========== Pin Utilities ==========

UEdGraphPin* FAnimBPImporter::FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
	if (!Node) return nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinName.ToString() == PinName)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FAnimBPImporter::FindOutputPosePin(UAnimGraphNode_Base* Node)
{
	if (!Node) return nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FAnimBPImporter::FindInputPosePin(UAnimGraphNode_Base* Node, const FString& KebabPinName)
{
	if (!Node) return nullptr;
	
	// Convert kebab-case pin name to the UE pin name (CamelCase)
	FString CamelPinName = KebabToCamel(KebabPinName);
	
	// Build a normalized version for fuzzy matching (lowercase, no spaces/hyphens/underscores)
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};
	FString NormalizedTarget = Normalize(KebabPinName);
	
	UEdGraphPin* BestMatch = nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			FString PinNameStr = Pin->PinName.ToString();
			
			// Try exact CamelCase match
			if (PinNameStr == CamelPinName)
			{
				return Pin;
			}
			// Try case-insensitive match
			if (PinNameStr.Equals(CamelPinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
			// Try normalized fuzzy match
			if (Normalize(PinNameStr) == NormalizedTarget)
			{
				BestMatch = Pin;
			}
		}
	}
	
	if (BestMatch) return BestMatch;
	
	// Fallback: if only one input pose pin exists and no specific name requested, use it
	UEdGraphPin* SingleInput = nullptr;
	int32 PoseInputCount = 0;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			SingleInput = Pin;
			PoseInputCount++;
		}
	}
	if (PoseInputCount == 1)
	{
		return SingleInput;
	}
	
	UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not find input pose pin '%s' (CamelCase: '%s') on node '%s'"), 
		*KebabPinName, *CamelPinName, *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	return nullptr;
}

void FAnimBPImporter::ConnectPins(UEdGraphPin* OutputPin, UEdGraphPin* InputPin)
{
	if (!OutputPin || !InputPin)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("ConnectPins: null pin(s)"));
		return;
	}
	
	// Break existing connections
	OutputPin->BreakAllPinLinks();
	InputPin->BreakAllPinLinks();
	
	// Make the connection
	OutputPin->MakeLinkTo(InputPin);
}

// ========== Node Property Setting ==========

bool FAnimBPImporter::SetNodeProperty(UAnimGraphNode_Base* Node, const FString& KebabKey, const FString& Value)
{
	if (!Node) return false;
	
	FString CamelKey = KebabToCamel(KebabKey);
	
	// Normalize helper: lowercase, strip all spaces/hyphens/underscores
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In.ToLower();
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("-"), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		return Out;
	};
	
	FString NormalizedKey = Normalize(KebabKey);
	
	// Strip quotes from value
	FString CleanValue = Value;
	if (CleanValue.StartsWith(TEXT("\"")) && CleanValue.EndsWith(TEXT("\"")))
	{
		CleanValue = CleanValue.Mid(1, CleanValue.Len() - 2);
	}
	
	// Pass 1: Try exact CamelCase match, then case-insensitive, then normalized fuzzy
	UEdGraphPin* BestMatch = nullptr;
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && 
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
		{
			FString PinNameStr = Pin->PinName.ToString();
			
			// Exact CamelCase match
			if (PinNameStr == CamelKey)
			{
				Pin->DefaultValue = CleanValue;
				return true;
			}
			// Case-insensitive match
			if (PinNameStr.Equals(CamelKey, ESearchCase::IgnoreCase))
			{
				Pin->DefaultValue = CleanValue;
				return true;
			}
			// Normalized fuzzy match (handles "CurveValues 0" vs "CurveValues0",
			// "BlendTime 0" vs "BlendTime0", etc.)
			if (Normalize(PinNameStr) == NormalizedKey)
			{
				BestMatch = Pin;
			}
		}
	}
	
	if (BestMatch)
	{
		BestMatch->DefaultValue = CleanValue;
		return true;
	}
	
	// Pass 2: Try FProperty reflection on the internal anim node struct
	// This handles properties that are not exposed as pins but are FProperty members
	// The internal FAnimNode_xxx struct is stored as the first struct property
	// of the UAnimGraphNode_xxx.
	for (TFieldIterator<FStructProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FStructProperty* StructProp = *PropIt;
		if (StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(Node);
			
			// Search for the property by name in the internal struct
			for (TFieldIterator<FProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
			{
				FProperty* InnerProp = *InnerIt;
				FString PropName = InnerProp->GetName();
				
				if (Normalize(PropName) == NormalizedKey || PropName.Equals(CamelKey, ESearchCase::IgnoreCase))
				{
					// Try to set the property from string
					void* ValuePtr = InnerProp->ContainerPtrToValuePtr<void>(StructPtr);
					const TCHAR* ImportResult = InnerProp->ImportText_Direct(*CleanValue, ValuePtr, nullptr, PPF_None);
					if (ImportResult != nullptr)
					{
						return true;
					}
				}
			}
			break; // Only check the first FAnimNode struct
		}
	}
	
	UE_LOG(LogAnimBPImporter, Verbose, TEXT("SetNodeProperty: Could not find pin or property for '%s' on '%s'"), 
		*KebabKey, *Node->GetClass()->GetName());
	return false;
}

// ========== Blueprint Creation ==========

UAnimBlueprint* FAnimBPImporter::CreateEmptyBlueprint(const FString& PackagePath, const FString& BlueprintName, const FString& SkeletonPath)
{
	// Find or load the skeleton
	USkeleton* Skeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		FString CleanPath = SkeletonPath;
		if (CleanPath.StartsWith(TEXT("\"")) && CleanPath.EndsWith(TEXT("\"")))
		{
			CleanPath = CleanPath.Mid(1, CleanPath.Len() - 2);
		}
		Skeleton = LoadObject<USkeleton>(nullptr, *CleanPath);
		if (!Skeleton)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not load skeleton: %s"), *CleanPath);
		}
	}
	
	// Create the package
	FString FullPackagePath = PackagePath / BlueprintName;
	UPackage* Package = CreatePackage(*FullPackagePath);
	if (!Package)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Failed to create package: %s"), *FullPackagePath);
		return nullptr;
	}
	
	// Use the factory to create the Animation Blueprint
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->TargetSkeleton = Skeleton;
	
	UObject* CreatedAsset = Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(),
		Package,
		FName(*BlueprintName),
		RF_Public | RF_Standalone,
		nullptr,
		GWarn
	);
	
	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(CreatedAsset);
	if (!AnimBlueprint)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Factory failed to create AnimBlueprint"));
		return nullptr;
	}
	
	// Notify asset registry
	FAssetRegistryModule::AssetCreated(AnimBlueprint);
	Package->MarkPackageDirty();
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Created AnimBlueprint: %s (Skeleton: %s)"), 
		*BlueprintName, Skeleton ? *Skeleton->GetName() : TEXT("none"));
	
	return AnimBlueprint;
}

UEdGraph* FAnimBPImporter::FindAnimGraph(UAnimBlueprint* Blueprint)
{
	if (!Blueprint) return nullptr;
	
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
		{
			return Graph;
		}
	}
	
	UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not find AnimGraph in blueprint"));
	return nullptr;
}

// ========== Variable Building ==========

bool FAnimBPImporter::BuildVariables(UAnimBlueprint* Blueprint, const TArray<FVariableDef>& Variables)
{
	if (!Blueprint || Variables.Num() == 0) return true;
	
	for (const FVariableDef& Var : Variables)
	{
		FEdGraphPinType PinType;
		
		switch (Var.Type)
		{
		case EPinType::Float:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			break;
		case EPinType::Int:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			break;
		case EPinType::Bool:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			break;
		case EPinType::Vector:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			break;
		case EPinType::Rotator:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			break;
		case EPinType::Name:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			break;
		default:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			break;
		}
		
		// Add the variable to the blueprint
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Var.Name), PinType);
		
		UE_LOG(LogAnimBPImporter, Verbose, TEXT("Added variable: %s"), *Var.Name);
	}
	
	return true;
}

// ========== Node Building ==========

UAnimGraphNode_Base* FAnimBPImporter::BuildAnimNode(const TSharedPtr<FAnimNodeAST>& NodeAST, UEdGraph* Graph)
{
	if (!NodeAST.IsValid() || !Graph)
	{
		return nullptr;
	}
	
	const FString& NodeType = NodeAST->NodeType;
	
	// Skip identity-pose (no actual node)
	if (NodeType == TEXT("identity-pose"))
	{
		return nullptr;
	}
	
	// Handle blend-list: the :class property contains the actual UE class name
	FString EffectiveNodeType = NodeType;
	if (NodeType == TEXT("blend-list"))
	{
		const FString* ClassProp = NodeAST->Properties.Find(TEXT("class"));
		if (ClassProp)
		{
			EffectiveNodeType = StripQuotes(*ClassProp);
			// ClassProp might be "AnimGraphNode_BlendListByEnum" — use directly
		}
	}
	
	// Handle variable references (bare identifiers like "Post-Layering" for UseCachedPose)
	// These won't have parentheses in DSL; they are plain identifiers
	// We detect them by checking: no children, no properties, and name doesn't match a known node class
	UClass* NodeClass = FindAnimNodeClass(EffectiveNodeType);
	if (!NodeClass)
	{
		// This might be a cached pose variable reference
		// Create a UseCachedPose node
		UAnimGraphNode_UseCachedPose* UseNode = NewObject<UAnimGraphNode_UseCachedPose>(Graph);
		if (UseNode)
		{
			UseNode->CreateNewGuid();
			UseNode->PostPlacedNewNode();
			UseNode->AllocateDefaultPins();
			
			// Set the cache name (convert kebab back to space-separated)
			FString CacheName = NodeType;
			CacheName.ReplaceInline(TEXT("-"), TEXT(" "));
			// The internal property for linked cache name
			// UseCachedPose stores the reference name differently per engine version
			// We'll try the common approach
			
			Graph->AddNode(UseNode, false, false);
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:CachedPoseLink] Created UseCachedPose for '%s' but cannot set link to SaveCachedPose — node will show as 'None'"),
				*CacheName);
			return UseNode;
		}
		
		UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not create node for type '%s'"), *NodeType);
		return nullptr;
	}
	
	// Create the node
	UAnimGraphNode_Base* NewNode = NewObject<UAnimGraphNode_Base>(Graph, NodeClass);
	if (!NewNode)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Failed to create node of class '%s'"), *NodeClass->GetName());
		return nullptr;
	}
	
	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	
	// Clean up pins where AllocateDefaultPins copies AutogeneratedDefaultValue into DefaultValue.
	// This prevents re-export from emitting redundant properties like :alpha 1.0, :x 0.0, etc.
	for (UEdGraphPin* Pin : NewNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && !Pin->DefaultValue.IsEmpty() && !Pin->AutogeneratedDefaultValue.IsEmpty())
		{
			if (Pin->DefaultValue == Pin->AutogeneratedDefaultValue)
			{
				Pin->DefaultValue.Empty();
			}
			else if (FCString::IsNumeric(*Pin->DefaultValue) && FCString::IsNumeric(*Pin->AutogeneratedDefaultValue))
			{
				float ValF = FCString::Atof(*Pin->DefaultValue);
				float DefF = FCString::Atof(*Pin->AutogeneratedDefaultValue);
				if (FMath::IsNearlyEqual(ValF, DefF, 1e-6f))
				{
					Pin->DefaultValue.Empty();
				}
			}
		}
	}
	
	Graph->AddNode(NewNode, false, false);
	
	// ---- Handle special node types ----
	
	// LinkedAnimLayer: set layer name and reconstruct to generate pins
	if (UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(NewNode))
	{
		const FString* LayerNameStr = NodeAST->Properties.Find(TEXT("layer"));
		if (LayerNameStr)
		{
			FName LayerFName(*StripQuotes(*LayerNameStr));
			
			// Set layer name directly on the runtime node struct (public member).
			// Note: SetLayerName() is MinimalAPI (not exported), so we set Node.Layer directly.
			LayerNode->Node.Layer = LayerFName;
			
			// If an interface path is specified, set it on the runtime node
			const FString* InterfacePath = NodeAST->Properties.Find(TEXT("interface"));
			if (InterfacePath)
			{
				FString CleanPath = StripQuotes(*InterfacePath);
				UClass* InterfaceClass = LoadObject<UClass>(nullptr, *CleanPath);
				if (InterfaceClass)
				{
					LayerNode->Node.Interface = InterfaceClass;
				}
				else
				{
					UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not load AnimLayerInterface: %s"), *CleanPath);
				}
			}
			
			// Reconstruct node to regenerate pins from the layer definition.
			// Call via base class pointer since MinimalAPI class vtable may not be exported.
			static_cast<UEdGraphNode*>(LayerNode)->ReconstructNode();
			
			// Clean up pins after ReconstructNode
			for (UEdGraphPin* Pin : LayerNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && !Pin->DefaultValue.IsEmpty() && !Pin->AutogeneratedDefaultValue.IsEmpty())
				{
					if (Pin->DefaultValue == Pin->AutogeneratedDefaultValue)
					{
						Pin->DefaultValue.Empty();
					}
				}
			}
			
			UE_LOG(LogAnimBPImporter, Log, TEXT("LinkedAnimLayer: set layer='%s', pins=%d"),
				*LayerFName.ToString(), LayerNode->Pins.Num());
		}
		
		// Ensure pose input pins exist for all DSL children.
		// If ReconstructNode did not produce them (e.g., new blueprint without SkeletonGeneratedClass),
		// manually create the necessary pose pins from the DSL child node names.
		for (const auto& Child : NodeAST->Children)
		{
			const FString& ChildPinName = Child.PinName;
			FString CamelPinName = KebabToCamel(ChildPinName);
			
			// Check if this pose pin already exists
			bool bPinExists = false;
			for (UEdGraphPin* Pin : LayerNode->Pins)
			{
				if (Pin && Pin->PinName.ToString() == CamelPinName && 
					Pin->Direction == EGPD_Input &&
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					bPinExists = true;
					break;
				}
			}
			
			if (!bPinExists)
			{
				// Create a pose input pin manually
				FEdGraphPinType PoseType;
				PoseType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PoseType.PinSubCategoryObject = FPoseLink::StaticStruct();
				
				UEdGraphPin* NewPin = LayerNode->CreatePin(EGPD_Input, PoseType, FName(*CamelPinName));
				if (NewPin)
				{
					UE_LOG(LogAnimBPImporter, Log, TEXT("LinkedAnimLayer: manually created pose pin '%s'"), *CamelPinName);
				}
			}
		}
	}
	
	// SequencePlayer / SequenceEvaluator: set animation sequence
	// Supports both :name "ShortName" (legacy) and :sequence (asset "/Game/Path") formats
	if (UAnimGraphNode_SequencePlayer* SeqPlayer = Cast<UAnimGraphNode_SequencePlayer>(NewNode))
	{
		UAnimSequence* Seq = nullptr;
		FString SearchPath;
		
		// Priority 1: :sequence (asset "...") — full path
		const FString* SeqAsset = NodeAST->Properties.Find(TEXT("sequence"));
		if (SeqAsset && SeqAsset->StartsWith(TEXT("(asset ")))
		{
			SearchPath = *SeqAsset;
			SearchPath.RemoveFromStart(TEXT("(asset "));
			SearchPath.RemoveFromEnd(TEXT(")"));
			SearchPath = StripQuotes(SearchPath);
			Seq = LoadObject<UAnimSequence>(nullptr, *SearchPath);
		}
		
		// Priority 2: :name "ShortName" — search by name
		if (!Seq)
		{
			const FString* SeqName = NodeAST->Properties.Find(TEXT("name"));
			if (SeqName)
			{
				SearchPath = StripQuotes(*SeqName);
				Seq = LoadObject<UAnimSequence>(nullptr, *SearchPath);
				if (!Seq)
				{
					for (TObjectIterator<UAnimSequence> It; It; ++It)
					{
						if (It->GetName() == SearchPath)
						{
							Seq = *It;
							break;
						}
					}
				}
				if (!Seq)
				{
					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
					IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
					
					TArray<FAssetData> AssetList;
					AssetRegistry.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(), AssetList, false);
					
					for (const FAssetData& AssetData : AssetList)
					{
						if (AssetData.AssetName.ToString() == SearchPath)
						{
							Seq = Cast<UAnimSequence>(AssetData.GetAsset());
							if (Seq) break;
						}
					}
				}
			}
		}
		
		if (Seq)
		{
			SeqPlayer->Node.SetSequence(Seq);
		}
		else if (!SearchPath.IsEmpty() && SearchPath != TEXT("None"))
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:AssetLoad] Node '%s': could not find AnimSequence asset '%s'"),
				*NodeType, *SearchPath);
		}
		
		// Loop — explicitly handle both true and false
		const FString* LoopVal = NodeAST->Properties.Find(TEXT("loop"));
		if (LoopVal)
		{
			bool bLoop = LoopVal->ToBool();
			SeqPlayer->Node.SetLoopAnimation(bLoop);
		}
	}
	
	// BlendSpacePlayer: set blend space
	// Supports both :name "ShortName" (legacy) and :blend-space (asset "/Path") formats
	if (UAnimGraphNode_BlendSpacePlayer* BSPlayer = Cast<UAnimGraphNode_BlendSpacePlayer>(NewNode))
	{
		UBlendSpace* BS = nullptr;
		FString SearchPath;
		
		// Priority 1: :blend-space (asset "...") — full path
		const FString* BSAsset = NodeAST->Properties.Find(TEXT("blend-space"));
		if (BSAsset && BSAsset->StartsWith(TEXT("(asset ")))
		{
			SearchPath = *BSAsset;
			SearchPath.RemoveFromStart(TEXT("(asset "));
			SearchPath.RemoveFromEnd(TEXT(")"));
			SearchPath = StripQuotes(SearchPath);
			BS = LoadObject<UBlendSpace>(nullptr, *SearchPath);
		}
		
		// Priority 2: :name "ShortName" — search by name
		if (!BS)
		{
			const FString* BSName = NodeAST->Properties.Find(TEXT("name"));
			if (BSName)
			{
				SearchPath = StripQuotes(*BSName);
				BS = LoadObject<UBlendSpace>(nullptr, *SearchPath);
				if (!BS)
				{
					for (TObjectIterator<UBlendSpace> It; It; ++It)
					{
						if (It->GetName() == SearchPath)
						{
							BS = *It;
							break;
						}
					}
				}
				if (!BS)
				{
					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
					IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
					
					// Force scan Engine/Content paths (Tutorial assets may not be indexed yet)
					TArray<FString> ScanPaths;
					ScanPaths.Add(TEXT("/Engine/"));
					AssetRegistry.ScanPathsSynchronous(ScanPaths, true);
					
					TArray<FAssetData> AssetList;
					// Search for UBlendSpace and all subclasses (including BlendSpace1D)
					AssetRegistry.GetAssetsByClass(UBlendSpace::StaticClass()->GetClassPathName(), AssetList, true);
					
					for (const FAssetData& AssetData : AssetList)
					{
						if (AssetData.AssetName.ToString() == SearchPath)
						{
							BS = Cast<UBlendSpace>(AssetData.GetAsset());
							if (BS) break;
						}
					}
				}
			}
		}
		
		if (BS)
		{
			BSPlayer->Node.SetBlendSpace(BS);
		}
		else if (!SearchPath.IsEmpty())
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:AssetLoad] Node '%s': could not find BlendSpace asset '%s'"),
				*NodeType, *SearchPath);
		}
		
		const FString* LoopVal2 = NodeAST->Properties.Find(TEXT("loop"));
		if (LoopVal2)
		{
			bool bLoop = LoopVal2->ToBool();
			BSPlayer->Node.SetLoop(bLoop);
		}
	}
	
	// ---- Pre-process: Expand dynamic array pins before setting properties ----
	// UE creates dynamic pins based on internal array sizes (e.g. CurveValues, BlendTime, BlendWeights).
	// We need to pre-size these arrays and ReconstructNode() so pins exist when SetNodeProperty is called.
	{
		// Collect array-indexed properties: "curve-values-0" → BaseName="curve-values", Indices={0: value}
		TMap<FString, TMap<int32, FString>> ArrayProperties;
		for (const auto& Pair : NodeAST->Properties)
		{
			// Check for trailing "-N" pattern (e.g. "curve-values-0", "blend-time-1")
			int32 LastDash = INDEX_NONE;
			Pair.Key.FindLastChar('-', LastDash);
			if (LastDash != INDEX_NONE && LastDash < Pair.Key.Len() - 1)
			{
				FString Suffix = Pair.Key.Mid(LastDash + 1);
				if (FCString::IsNumeric(*Suffix))
				{
					int32 Idx = FCString::Atoi(*Suffix);
					FString BaseName = Pair.Key.Left(LastDash);
					ArrayProperties.FindOrAdd(BaseName).Add(Idx, Pair.Value);
				}
			}
		}
		
		bool bNeedReconstruct = false;
		
		// ModifyCurve: populate CurveNames and CurveValues arrays via AddCurve API
		if (UAnimGraphNode_ModifyCurve* MCNode = Cast<UAnimGraphNode_ModifyCurve>(NewNode))
		{
			const TMap<int32, FString>* CurveVals = ArrayProperties.Find(TEXT("curve-values"));
			if (CurveVals && CurveVals->Num() > 0)
			{
				// Find max index to determine array size
				int32 MaxIdx = 0;
				for (const auto& KV : *CurveVals)
				{
					MaxIdx = FMath::Max(MaxIdx, KV.Key);
				}
				int32 ArraySize = MaxIdx + 1;
				
				// Access FAnimNode_ModifyCurve via FProperty reflection and use AddCurve()
				for (TFieldIterator<FStructProperty> PropIt(MCNode->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = *PropIt;
					if (StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
					{
						FAnimNode_ModifyCurve* InternalNode = StructProp->ContainerPtrToValuePtr<FAnimNode_ModifyCurve>(MCNode);
						if (InternalNode)
						{
							for (int32 i = 0; i < ArraySize; i++)
							{
								float Val = 0.f;
								const FString* FoundVal = CurveVals->Find(i);
								if (FoundVal)
								{
									Val = FCString::Atof(*StripQuotes(*FoundVal));
								}
								FName CurveName = *FString::Printf(TEXT("Curve_%d"), i);
								InternalNode->AddCurve(CurveName, Val);
							}
						}
						break;
					}
				}
				bNeedReconstruct = true;
			}
		}
		
		// BlendListBase (includes BlendListByEnum, BlendListByBool, etc.): expand BlendTime array
		if (UAnimGraphNode_BlendListBase* BLNode = Cast<UAnimGraphNode_BlendListBase>(NewNode))
		{
			const TMap<int32, FString>* TimeVals = ArrayProperties.Find(TEXT("blend-time"));
			if (TimeVals && TimeVals->Num() > 0)
			{
				int32 MaxIdx = 0;
				for (const auto& KV : *TimeVals) MaxIdx = FMath::Max(MaxIdx, KV.Key);
				int32 RequiredSize = MaxIdx + 1;
				
				// Access the internal FAnimNode_BlendListBase via the first FAnimNode struct property
				for (TFieldIterator<FStructProperty> PropIt(BLNode->GetClass()); PropIt; ++PropIt)
				{
					FStructProperty* StructProp = *PropIt;
					if (StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
					{
						void* NodePtr = StructProp->ContainerPtrToValuePtr<void>(BLNode);
						
						// Find and resize the BlendTime TArray
						FArrayProperty* BlendTimeArrayProp = nullptr;
						for (TFieldIterator<FArrayProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
						{
							if (InnerIt->GetName() == TEXT("BlendTime"))
							{
								BlendTimeArrayProp = *InnerIt;
								break;
							}
						}
						
						if (BlendTimeArrayProp)
						{
							FScriptArrayHelper ArrayHelper(BlendTimeArrayProp, BlendTimeArrayProp->ContainerPtrToValuePtr<void>(NodePtr));
							// Ensure array has enough elements (add poses as needed)
							while (ArrayHelper.Num() < RequiredSize)
							{
								ArrayHelper.AddValue();
							}
							// Set values
							FFloatProperty* InnerFloat = CastField<FFloatProperty>(BlendTimeArrayProp->Inner);
							if (InnerFloat)
							{
								for (const auto& KV : *TimeVals)
								{
									if (KV.Key < ArrayHelper.Num())
									{
										float Val = FCString::Atof(*StripQuotes(KV.Value));
										InnerFloat->SetPropertyValue(ArrayHelper.GetRawPtr(KV.Key), Val);
									}
								}
							}
						}
						
						// Also resize BlendPose TArray to match
						FArrayProperty* BlendPoseArrayProp = nullptr;
						for (TFieldIterator<FArrayProperty> InnerIt(StructProp->Struct); InnerIt; ++InnerIt)
						{
							if (InnerIt->GetName() == TEXT("BlendPose"))
							{
								BlendPoseArrayProp = *InnerIt;
								break;
							}
						}
						if (BlendPoseArrayProp)
						{
							FScriptArrayHelper PoseHelper(BlendPoseArrayProp, BlendPoseArrayProp->ContainerPtrToValuePtr<void>(NodePtr));
							while (PoseHelper.Num() < RequiredSize)
							{
								PoseHelper.AddValue();
							}
						}
						break;
					}
				}
				bNeedReconstruct = true;
			}
		}
		
		// LayeredBoneBlend: expand BlendWeights array
		if (UAnimGraphNode_LayeredBoneBlend* LBBNode = Cast<UAnimGraphNode_LayeredBoneBlend>(NewNode))
		{
			const TMap<int32, FString>* WeightVals = ArrayProperties.Find(TEXT("blend-weights"));
			if (WeightVals && WeightVals->Num() > 0)
			{
				int32 MaxIdx = 0;
				for (const auto& KV : *WeightVals) MaxIdx = FMath::Max(MaxIdx, KV.Key);
				int32 RequiredSize = MaxIdx + 1;
				
				// Directly resize the internal arrays
				while (LBBNode->Node.BlendWeights.Num() < RequiredSize)
				{
					LBBNode->Node.AddPose();
				}
				
				for (const auto& KV : *WeightVals)
				{
					if (KV.Key < LBBNode->Node.BlendWeights.Num())
					{
						LBBNode->Node.BlendWeights[KV.Key] = FCString::Atof(*StripQuotes(KV.Value));
					}
				}
				bNeedReconstruct = true;
			}
		}
		
		if (bNeedReconstruct)
		{
			NewNode->ReconstructNode();
			
			// Clean up pins where ReconstructNode copies AutogeneratedDefaultValue into DefaultValue
			for (UEdGraphPin* Pin : NewNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && !Pin->DefaultValue.IsEmpty() && !Pin->AutogeneratedDefaultValue.IsEmpty())
				{
					if (Pin->DefaultValue == Pin->AutogeneratedDefaultValue)
					{
						Pin->DefaultValue.Empty();
					}
					else if (FCString::IsNumeric(*Pin->DefaultValue) && FCString::IsNumeric(*Pin->AutogeneratedDefaultValue))
					{
						float ValF = FCString::Atof(*Pin->DefaultValue);
						float DefF = FCString::Atof(*Pin->AutogeneratedDefaultValue);
						if (FMath::IsNearlyEqual(ValF, DefF, 1e-6f))
						{
							Pin->DefaultValue.Empty();
						}
					}
				}
			}
		}
	}
	
	// Set non-pose properties via pins
	for (const auto& Pair : NodeAST->Properties)
	{
		// Skip special properties already handled above
		if (Pair.Key == TEXT("name") || Pair.Key == TEXT("loop") || 
			Pair.Key == TEXT("class") || Pair.Key == TEXT("initial") || 
			Pair.Key == TEXT("transitions") || Pair.Key == TEXT("layer") ||
			Pair.Key == TEXT("interface"))
		{
			continue;
		}
		
		// Skip sequence/blend-space only for nodes that handle them in special branches above
		if (Pair.Key == TEXT("sequence") && Cast<UAnimGraphNode_SequencePlayer>(NewNode))
		{
			continue;
		}
		if (Pair.Key == TEXT("blend-space") && Cast<UAnimGraphNode_BlendSpacePlayer>(NewNode))
		{
			continue;
		}
		
		// Skip (ref "...") values — these are connected pins, not default values
		// These require EventGraph K2Node_VariableGet nodes which we cannot create yet
		if (Pair.Value.StartsWith(TEXT("(ref ")))
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:RefConnection] Node '%s' property ':%s' = %s — EventGraph variable connection cannot be restored (requires K2Node_VariableGet)"),
				*NodeType, *Pair.Key, *Pair.Value);
			continue;
		}
		
		// (asset "...") values need special handling — extract the path and set via SetNodeProperty
		if (Pair.Value.StartsWith(TEXT("(asset ")))
		{
			// Extract path from (asset "path")
			FString AssetPath = Pair.Value;
			AssetPath.RemoveFromStart(TEXT("(asset "));
			AssetPath.RemoveFromEnd(TEXT(")"));
			AssetPath = StripQuotes(AssetPath);
			
			// Try to set as a quoted path so ImportText_Direct can load it
			if (!SetNodeProperty(NewNode, Pair.Key, FString::Printf(TEXT("\"%s\""), *AssetPath)))
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:AssetRef] Node '%s' property ':%s' = %s — asset reference could not be set via property reflection"),
					*NodeType, *Pair.Key, *Pair.Value);
			}
			continue;
		}
		
		if (!SetNodeProperty(NewNode, Pair.Key, Pair.Value))
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("[DEGRADATION:PropertySet] Node '%s' property ':%s' = %s — could not find matching pin or FProperty"),
				*NodeType, *Pair.Key, *Pair.Value);
		}
	}
	
	// ---- Handle state machine ----
	if (NodeType == TEXT("state-machine"))
	{
		UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(NewNode);
		if (SMNode)
		{
			BuildStateMachine(SMNode, NodeAST);
		}
	}
	
	// ---- Recursively build children and connect ----
	for (const FNamedChild& Child : NodeAST->Children)
	{
		if (!Child.Node.IsValid()) continue;
		
		UAnimGraphNode_Base* ChildNode = BuildAnimNode(Child.Node, Graph);
		if (!ChildNode) continue;
		
		// Find the output pin on the child
		UEdGraphPin* ChildOutput = FindOutputPosePin(ChildNode);
		if (!ChildOutput) continue;
		
		// Find the input pin on the parent by the named pin
		UEdGraphPin* ParentInput = nullptr;
		if (!Child.PinName.IsEmpty())
		{
			ParentInput = FindInputPosePin(NewNode, Child.PinName);
		}
		else
		{
			// Try the default/first pose input
			ParentInput = FindInputPosePin(NewNode, TEXT(""));
		}
		
		if (ParentInput)
		{
			ConnectPins(ChildOutput, ParentInput);
		}
		else
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("Could not connect child '%s' to parent '%s' pin '%s'"),
				*Child.Node->NodeType, *NodeType, *Child.PinName);
		}
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Created node: %s (%s)"), *NodeType, *NodeClass->GetName());
	return NewNode;
}

// ========== State Machine Building ==========

bool FAnimBPImporter::BuildStateMachine(UAnimGraphNode_StateMachine* SMNode, const TSharedPtr<FAnimNodeAST>& NodeAST)
{
	if (!SMNode || !NodeAST.IsValid()) return false;
	
	// Get the state machine graph
	UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("StateMachine node has no EditorStateMachineGraph"));
		return false;
	}
	
	// Parse state machine properties
	const FString* NameProp = NodeAST->Properties.Find(TEXT("name"));
	if (NameProp)
	{
		FString SMName = StripQuotes(*NameProp);
		// Rename the state machine node (this renames the internal graph too)
		SMNode->OnRenameNode(SMName);
	}
	
	const FString* InitialProp = NodeAST->Properties.Find(TEXT("initial"));
	FString InitialStateName;
	if (InitialProp)
	{
		InitialStateName = StripQuotes(*InitialProp);
	}
	
	// Collect state nodes from children
	TMap<FString, UAnimStateNode*> StateNodes;
	
	for (const FNamedChild& Child : NodeAST->Children)
	{
		if (!Child.Node.IsValid()) continue;
		
		// Convert kebab pin name back to state name (e.g. "in-ragdoll" -> "In Ragdoll")
		FString StateName = Child.PinName;
		StateName.ReplaceInline(TEXT("-"), TEXT(" "));
		// Capitalize first letter of each word
		FString TitleCaseName;
		bool bCapNext = true;
		for (TCHAR Ch : StateName)
		{
			if (Ch == ' ')
			{
				bCapNext = true;
				TitleCaseName += Ch;
			}
			else if (bCapNext)
			{
				TitleCaseName += FChar::ToUpper(Ch);
				bCapNext = false;
			}
			else
			{
				TitleCaseName += Ch;
			}
		}
		
		// Create state node
		UAnimStateNode* StateNode = NewObject<UAnimStateNode>(SMGraph);
		if (StateNode)
		{
			StateNode->CreateNewGuid();
			StateNode->PostPlacedNewNode();
			StateNode->AllocateDefaultPins();
			SMGraph->AddNode(StateNode, false, false);
			
			// Set the state name
			StateNode->OnRenameNode(TitleCaseName);
			
			// Set state name via the bound graph's name
			// The state node should have a BoundGraph where we place the state result + animation tree
			if (StateNode->BoundGraph)
			{
				// Find the state result node in the bound graph
				UAnimGraphNode_StateResult* ResultNode = nullptr;
				for (UEdGraphNode* ExistingNode : StateNode->BoundGraph->Nodes)
				{
					ResultNode = Cast<UAnimGraphNode_StateResult>(ExistingNode);
					if (ResultNode) break;
				}
				
				if (!ResultNode)
				{
					ResultNode = NewObject<UAnimGraphNode_StateResult>(StateNode->BoundGraph);
					ResultNode->CreateNewGuid();
					ResultNode->PostPlacedNewNode();
					ResultNode->AllocateDefaultPins();
					StateNode->BoundGraph->AddNode(ResultNode, false, false);
				}
				
				// Build the animation subtree for this state
				UAnimGraphNode_Base* AnimTree = BuildAnimNode(Child.Node, StateNode->BoundGraph);
				if (AnimTree && ResultNode)
				{
					UEdGraphPin* AnimOutput = FindOutputPosePin(AnimTree);
					UEdGraphPin* ResultInput = nullptr;
					for (UEdGraphPin* Pin : ResultNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input)
						{
							ResultInput = Pin;
							break;
						}
					}
					if (AnimOutput && ResultInput)
					{
						ConnectPins(AnimOutput, ResultInput);
					}
				}
			}
			
			StateNodes.Add(TitleCaseName, StateNode);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created state: %s"), *TitleCaseName);
		}
	}
	
	// Connect entry node to initial state
	if (!InitialStateName.IsEmpty())
	{
		UAnimStateNode** InitState = StateNodes.Find(InitialStateName);
		if (InitState && *InitState)
		{
			// Find the entry node
			UAnimStateEntryNode* EntryNode = SMGraph->EntryNode;
			if (EntryNode)
			{
				UEdGraphPin* EntryOutput = nullptr;
				for (UEdGraphPin* Pin : EntryNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output)
					{
						EntryOutput = Pin;
						break;
					}
				}
				UEdGraphPin* StateInput = nullptr;
				for (UEdGraphPin* Pin : (*InitState)->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input)
					{
						StateInput = Pin;
						break;
					}
				}
				if (EntryOutput && StateInput)
				{
					ConnectPins(EntryOutput, StateInput);
				}
			}
		}
	}
	
	// ---- Build transitions from :transitions property ----
	const FString* TransProp = NodeAST->Properties.Find(TEXT("transitions"));
	if (TransProp && !TransProp->IsEmpty())
	{
		// Parse the transitions string: [(FromState -> ToState :key val ...) ...]
		// State names can contain spaces, so we parse carefully using "->" as the delimiter
		struct FParsedTransition
		{
			FString FromState;
			FString ToState;
			float Duration = 0.2f;
			int32 Priority = 0;
			bool bBidirectional = false;
			bool bAutoRule = false;
			float AutoRuleTriggerTime = -1.0f;
			FString RuleRef; // The (ref "...") string, for logging only
		};
		TArray<FParsedTransition> ParsedTransitions;
		
		FString TransStr = *TransProp;
		TransStr.TrimStartAndEndInline();
		
		// Strip outer brackets [ ... ]
		if (TransStr.StartsWith(TEXT("[")) && TransStr.EndsWith(TEXT("]")))
		{
			TransStr = TransStr.Mid(1, TransStr.Len() - 2);
			TransStr.TrimStartAndEndInline();
		}
		
		// Split into individual transition entries by matching balanced parentheses
		// Each entry is (FromState -> ToState :key val ...)
		int32 Pos = 0;
		while (Pos < TransStr.Len())
		{
			// Skip whitespace
			while (Pos < TransStr.Len() && FChar::IsWhitespace(TransStr[Pos])) Pos++;
			if (Pos >= TransStr.Len()) break;
			
			if (TransStr[Pos] != '(')
			{
				Pos++;
				continue;
			}
			
			// Find matching closing paren, respecting nested parens and quoted strings
			int32 Start = Pos + 1; // skip opening '('
			int32 Depth = 1;
			Pos++;
			bool bInString = false;
			while (Pos < TransStr.Len() && Depth > 0)
			{
				TCHAR Ch = TransStr[Pos];
				if (bInString)
				{
					if (Ch == '"' && (Pos == 0 || TransStr[Pos-1] != '\\'))
					{
						bInString = false;
					}
				}
				else
				{
					if (Ch == '"') bInString = true;
					else if (Ch == '(') Depth++;
					else if (Ch == ')') Depth--;
				}
				if (Depth > 0) Pos++;
			}
			
			if (Depth != 0) break; // malformed
			
			FString EntryStr = TransStr.Mid(Start, Pos - Start);
			EntryStr.TrimStartAndEndInline();
			Pos++; // skip closing ')'
			
			// Parse the entry: "FromState -> ToState :duration X :priority Y :bidirectional true :rule (...)"
			// Find " -> " to split From and the rest
			int32 ArrowIdx = INDEX_NONE;
			EntryStr.FindChar('-', ArrowIdx);
			// Search for " -> " pattern (space-arrow-space or arrow-space)
			int32 SearchPos = 0;
			bool bFoundArrow = false;
			while (SearchPos < EntryStr.Len() - 2)
			{
				if (EntryStr[SearchPos] == '-' && EntryStr[SearchPos+1] == '>')
				{
					ArrowIdx = SearchPos;
					bFoundArrow = true;
					break;
				}
				SearchPos++;
			}
			
			if (!bFoundArrow)
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition entry missing '->': %s"), *EntryStr);
				continue;
			}
			
			FParsedTransition Trans;
			Trans.FromState = EntryStr.Left(ArrowIdx).TrimStartAndEnd();
			
			// After "->", the rest is "ToState :key val ..."
			// ToState ends at the first " :" (space + colon = keyword start)
			FString Rest = EntryStr.Mid(ArrowIdx + 2).TrimStartAndEnd();
			
			// Find the first keyword marker " :" to separate ToState from properties
			int32 FirstKeyword = INDEX_NONE;
			for (int32 i = 0; i < Rest.Len() - 1; i++)
			{
				if (Rest[i] == ' ' && Rest[i+1] == ':')
				{
					FirstKeyword = i;
					break;
				}
			}
			
			if (FirstKeyword != INDEX_NONE)
			{
				Trans.ToState = Rest.Left(FirstKeyword).TrimStartAndEnd();
				FString Props = Rest.Mid(FirstKeyword).TrimStartAndEnd();
				
				// Parse keyword properties from the Props string
				// Keywords: :duration, :priority, :bidirectional, :rule
				// Tokenize by splitting on " :" boundaries
				TArray<TPair<FString, FString>> KeyVals;
				int32 KPos = 0;
				while (KPos < Props.Len())
				{
					// Skip whitespace
					while (KPos < Props.Len() && FChar::IsWhitespace(Props[KPos])) KPos++;
					if (KPos >= Props.Len()) break;
					
					if (Props[KPos] != ':')
					{
						KPos++;
						continue;
					}
					KPos++; // skip ':'
					
					// Read keyword name (until space)
					int32 KeyStart = KPos;
					while (KPos < Props.Len() && !FChar::IsWhitespace(Props[KPos])) KPos++;
					FString Key = Props.Mid(KeyStart, KPos - KeyStart);
					
					// Skip space
					while (KPos < Props.Len() && FChar::IsWhitespace(Props[KPos])) KPos++;
					
					// Read value — could be a simple token, a quoted string, or a nested (...)
					FString Value;
					if (KPos < Props.Len() && Props[KPos] == '(')
					{
						// Nested expression — find matching paren
						int32 VStart = KPos;
						int32 VDepth = 0;
						bool bVInStr = false;
						while (KPos < Props.Len())
						{
							TCHAR VCh = Props[KPos];
							if (bVInStr)
							{
								if (VCh == '"' && (KPos == 0 || Props[KPos-1] != '\\'))
									bVInStr = false;
							}
							else
							{
								if (VCh == '"') bVInStr = true;
								else if (VCh == '(') VDepth++;
								else if (VCh == ')') { VDepth--; if (VDepth == 0) { KPos++; break; } }
							}
							KPos++;
						}
						Value = Props.Mid(VStart, KPos - VStart);
					}
					else
					{
						// Simple token (until next space+colon or end)
						int32 VStart = KPos;
						while (KPos < Props.Len())
						{
							// Check if this is the start of next keyword
							if (Props[KPos] == ' ' && KPos + 1 < Props.Len() && Props[KPos+1] == ':')
								break;
							KPos++;
						}
						Value = Props.Mid(VStart, KPos - VStart).TrimStartAndEnd();
					}
					
					KeyVals.Add(TPair<FString, FString>(Key, Value));
				}
				
				// Apply parsed key-value pairs
				for (const auto& KV : KeyVals)
				{
					if (KV.Key == TEXT("duration"))
					{
						Trans.Duration = FCString::Atof(*KV.Value);
					}
					else if (KV.Key == TEXT("priority"))
					{
						Trans.Priority = FCString::Atoi(*KV.Value);
					}
					else if (KV.Key == TEXT("bidirectional"))
					{
						Trans.bBidirectional = (KV.Value == TEXT("true"));
					}
					else if (KV.Key == TEXT("rule"))
					{
						// Check if it's an auto-rule or a ref
						if (KV.Value.StartsWith(TEXT("(auto-rule")))
						{
							Trans.bAutoRule = true;
							// Parse :time-remaining value
							if (KV.Value.Contains(TEXT("crossfade-duration")))
							{
								Trans.AutoRuleTriggerTime = -1.0f;
							}
							else
							{
								// Extract the float value after :time-remaining
								int32 TRIdx = KV.Value.Find(TEXT(":time-remaining"));
								if (TRIdx != INDEX_NONE)
								{
									FString TimeStr = KV.Value.Mid(TRIdx + 15).TrimStartAndEnd();
									TimeStr.RemoveFromEnd(TEXT(")"));
									TimeStr.TrimStartAndEndInline();
									Trans.AutoRuleTriggerTime = FCString::Atof(*TimeStr);
								}
							}
						}
						else if (KV.Value.StartsWith(TEXT("(ref")))
						{
							Trans.RuleRef = KV.Value;
							// (ref "...") conditions require EventGraph nodes — skip with warning
						}
					}
				}
			}
			else
			{
				// No keywords — just "ToState"
				Trans.ToState = Rest.TrimStartAndEnd();
			}
			
			if (!Trans.FromState.IsEmpty() && !Trans.ToState.IsEmpty())
			{
				ParsedTransitions.Add(Trans);
			}
		}
		
		// Create UAnimStateTransitionNode for each parsed transition
		// Track bidirectional pairs to avoid creating duplicates
		// (Exporter emits A->B and B->A separately when Bidirectional=true)
		TSet<FString> CreatedBidirectionalPairs;
		int32 CreatedCount = 0;
		
		for (const FParsedTransition& Trans : ParsedTransitions)
		{
			// Skip reverse of an already-created bidirectional transition
			if (Trans.bBidirectional)
			{
				FString ReverseKey = FString::Printf(TEXT("%s|%s"), *Trans.ToState, *Trans.FromState);
				if (CreatedBidirectionalPairs.Contains(ReverseKey))
				{
					UE_LOG(LogAnimBPImporter, Log, TEXT("Skipping reverse bidirectional transition: %s -> %s (already created as %s -> %s)"),
						*Trans.FromState, *Trans.ToState, *Trans.ToState, *Trans.FromState);
					continue;
				}
			}
			
			UAnimStateNode** FromStatePtr = StateNodes.Find(Trans.FromState);
			UAnimStateNode** ToStatePtr = StateNodes.Find(Trans.ToState);
			
			if (!FromStatePtr || !*FromStatePtr)
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition: FromState '%s' not found in state machine"), *Trans.FromState);
				continue;
			}
			if (!ToStatePtr || !*ToStatePtr)
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition: ToState '%s' not found in state machine"), *Trans.ToState);
				continue;
			}
			
			UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			if (!TransNode) continue;
			
			TransNode->CreateNewGuid();
			TransNode->PostPlacedNewNode();
			TransNode->AllocateDefaultPins();
			SMGraph->AddNode(TransNode, false, false);
			
			// Connect the transition between From and To state nodes
			TransNode->CreateConnections(*FromStatePtr, *ToStatePtr);
			
			// Set transition properties
			TransNode->CrossfadeDuration = Trans.Duration;
			TransNode->PriorityOrder = Trans.Priority;
			TransNode->Bidirectional = Trans.bBidirectional;
			
			// Handle auto-rule
			if (Trans.bAutoRule)
			{
				TransNode->bAutomaticRuleBasedOnSequencePlayerInState = true;
				TransNode->AutomaticRuleTriggerTime = Trans.AutoRuleTriggerTime;
			}
			
			// Log ref-based rules that we can't restore
			if (!Trans.RuleRef.IsEmpty())
			{
				UE_LOG(LogAnimBPImporter, Warning, TEXT("Transition %s -> %s: rule %s requires EventGraph nodes (skipped)"),
					*Trans.FromState, *Trans.ToState, *Trans.RuleRef);
			}
			
			// Track bidirectional pairs
			if (Trans.bBidirectional)
			{
				FString ForwardKey = FString::Printf(TEXT("%s|%s"), *Trans.FromState, *Trans.ToState);
				CreatedBidirectionalPairs.Add(ForwardKey);
			}
			
			CreatedCount++;
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created transition: %s -> %s (duration=%.2f, priority=%d, bidirectional=%s)"),
				*Trans.FromState, *Trans.ToState, Trans.Duration, Trans.Priority, Trans.bBidirectional ? TEXT("true") : TEXT("false"));
		}
		
		UE_LOG(LogAnimBPImporter, Log, TEXT("State machine: %d/%d transitions created"), CreatedCount, ParsedTransitions.Num());
	}
	
	return true;
}

// ========== Graph Building ==========

bool FAnimBPImporter::BuildAnimGraph(UAnimBlueprint* Blueprint, const TSharedPtr<FAnimGraphAST>& AST)
{
	if (!Blueprint || !AST.IsValid()) return false;
	
	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	if (!AnimGraph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("Could not find AnimGraph"));
		return false;
	}
	
	// Build variables
	BuildVariables(Blueprint, AST->Variables);
	
	// Build defines (SaveCachedPose nodes)
	TMap<FString, UAnimGraphNode_SaveCachedPose*> DefineNodes;
	for (const FCachedPoseDef& Def : AST->Defines)
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(AnimGraph);
		if (SaveNode)
		{
			SaveNode->CreateNewGuid();
			SaveNode->PostPlacedNewNode();
			SaveNode->AllocateDefaultPins();
			AnimGraph->AddNode(SaveNode, false, false);
			
			// Set the cache name
			SaveNode->CacheName = Def.Name;
			
			// Build the body subtree
			if (Def.Body.IsValid())
			{
				UAnimGraphNode_Base* BodyNode = BuildAnimNode(Def.Body, AnimGraph);
				if (BodyNode)
				{
					UEdGraphPin* BodyOutput = FindOutputPosePin(BodyNode);
					// Find SaveCachedPose's input pose pin
					UEdGraphPin* SaveInput = nullptr;
					for (UEdGraphPin* Pin : SaveNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input && 
							Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
						{
							SaveInput = Pin;
							break;
						}
					}
					if (BodyOutput && SaveInput)
					{
						ConnectPins(BodyOutput, SaveInput);
					}
				}
			}
			
			DefineNodes.Add(Def.GetIdentifier(), SaveNode);
			UE_LOG(LogAnimBPImporter, Log, TEXT("Created define: %s"), *Def.Name);
		}
	}
	
	// Build the root animation tree
	if (AST->RootNode.IsValid())
	{
		UAnimGraphNode_Base* RootTree = BuildAnimNode(AST->RootNode, AnimGraph);
		
		if (RootTree)
		{
			// Find the AnimGraph's root node (already created by the factory)
			UAnimGraphNode_Root* RootNode = nullptr;
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				RootNode = Cast<UAnimGraphNode_Root>(Node);
				if (RootNode) break;
			}
			
			if (RootNode)
			{
				UEdGraphPin* TreeOutput = FindOutputPosePin(RootTree);
				UEdGraphPin* RootInput = nullptr;
				for (UEdGraphPin* Pin : RootNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input && 
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						RootInput = Pin;
						break;
					}
				}
				if (TreeOutput && RootInput)
				{
					ConnectPins(TreeOutput, RootInput);
				}
			}
		}
	}
	
	return true;
}

// ========== Compilation ==========

bool FAnimBPImporter::CompileBlueprint(UAnimBlueprint* Blueprint, FString* OutError)
{
	if (!Blueprint)
	{
		if (OutError) *OutError = TEXT("Null blueprint");
		return false;
	}
	
	Blueprint->Modify();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	
	if (Blueprint->Status == BS_Error)
	{
		if (OutError) *OutError = TEXT("Blueprint has compilation errors after import");
		return false;
	}
	
	return true;
}

// ========== Top-Level API ==========

UAnimBlueprint* FAnimBPImporter::Import(const FString& DSLCode, const FString& PackagePath, FString* OutError)
{
	// Parse the DSL
	TArray<FAnimLangParseError> ParseErrors;
	TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(DSLCode, ParseErrors);
	
	if (!AST.IsValid())
	{
		if (OutError)
		{
			*OutError = TEXT("Failed to parse DSL code");
			for (const FAnimLangParseError& Err : ParseErrors)
			{
				*OutError += TEXT("\n  ") + Err.ToString();
			}
		}
		return nullptr;
	}
	
	if (ParseErrors.Num() > 0)
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("Parse warnings:"));
		for (const FAnimLangParseError& Err : ParseErrors)
		{
			UE_LOG(LogAnimBPImporter, Warning, TEXT("  %s"), *Err.ToString());
		}
	}
	
	return ImportFromAST(AST, PackagePath, OutError);
}

UAnimBlueprint* FAnimBPImporter::ImportFromAST(const TSharedPtr<FAnimGraphAST>& AST, const FString& PackagePath, FString* OutError)
{
	if (!AST.IsValid())
	{
		if (OutError) *OutError = TEXT("Null AST");
		return nullptr;
	}
	
	// Create the blueprint
	UAnimBlueprint* Blueprint = CreateEmptyBlueprint(PackagePath, AST->Name, AST->SkeletonPath);
	if (!Blueprint)
	{
		if (OutError) *OutError = TEXT("Failed to create empty blueprint");
		return nullptr;
	}
	
	// Build the animation graph
	if (!BuildAnimGraph(Blueprint, AST))
	{
		if (OutError) *OutError = TEXT("Failed to build animation graph");
		return nullptr;
	}
	
	// Compile
	FString CompileError;
	if (!CompileBlueprint(Blueprint, &CompileError))
	{
		UE_LOG(LogAnimBPImporter, Warning, TEXT("Compilation warning: %s"), *CompileError);
		// Don't fail — the blueprint is still usable
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Successfully imported '%s' from DSL (%d variables, %d defines)"),
		*AST->Name, AST->Variables.Num(), AST->Defines.Num());
	
	return Blueprint;
}

// ========== AnimGraph Clearing ==========

void FAnimBPImporter::ClearAnimGraph(UEdGraph* AnimGraph)
{
	if (!AnimGraph) return;
	
	// Collect all nodes except the Root node (which is persistent)
	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (!Cast<UAnimGraphNode_Root>(Node))
		{
			NodesToRemove.Add(Node);
		}
	}
	
	// Break all connections on root node's input pins first
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
		{
			for (UEdGraphPin* Pin : Root->Pins)
			{
				if (Pin) Pin->BreakAllPinLinks();
			}
			break;
		}
	}
	
	// Remove non-root nodes
	for (UEdGraphNode* Node : NodesToRemove)
	{
		// Break all pin links
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) Pin->BreakAllPinLinks();
		}
		AnimGraph->RemoveNode(Node);
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("Cleared AnimGraph: removed %d nodes"), NodesToRemove.Num());
}

void FAnimBPImporter::ClearDefines(UAnimBlueprint* Blueprint)
{
	if (!Blueprint) return;
	
	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	if (!AnimGraph) return;
	
	// SaveCachedPose nodes are in the AnimGraph — they were cleared by ClearAnimGraph
	// This method is here for future use if defines get stored elsewhere
}

bool FAnimBPImporter::RebuildAnimGraph(UAnimBlueprint* Blueprint, const TSharedPtr<FAnimGraphAST>& NewAST)
{
	if (!Blueprint || !NewAST.IsValid()) return false;
	
	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	if (!AnimGraph)
	{
		UE_LOG(LogAnimBPImporter, Error, TEXT("RebuildAnimGraph: Could not find AnimGraph"));
		return false;
	}
	
	// Step 1: Clear all existing nodes (except Root)
	ClearAnimGraph(AnimGraph);
	ClearDefines(Blueprint);
	
	// Step 2: Rebuild using the standard BuildAnimGraph logic
	// Note: Variables already exist in the blueprint — we may need to add new ones
	// but we don't remove existing ones to avoid breaking EventGraph references
	
	// Add any new variables that don't already exist
	for (const FVariableDef& Var : NewAST->Variables)
	{
		bool bExists = false;
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			if (ExistingVar.VarName == FName(*Var.Name))
			{
				bExists = true;
				break;
			}
		}
		if (!bExists)
		{
			TArray<FVariableDef> SingleVar;
			SingleVar.Add(Var);
			BuildVariables(Blueprint, SingleVar);
		}
	}
	
	// Step 3: Build defines (SaveCachedPose nodes)
	for (const FCachedPoseDef& Def : NewAST->Defines)
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(AnimGraph);
		if (SaveNode)
		{
			SaveNode->CreateNewGuid();
			SaveNode->PostPlacedNewNode();
			SaveNode->AllocateDefaultPins();
			AnimGraph->AddNode(SaveNode, false, false);
			
			SaveNode->CacheName = Def.Name;
			
			if (Def.Body.IsValid())
			{
				UAnimGraphNode_Base* BodyNode = BuildAnimNode(Def.Body, AnimGraph);
				if (BodyNode)
				{
					UEdGraphPin* BodyOutput = FindOutputPosePin(BodyNode);
					UEdGraphPin* SaveInput = nullptr;
					for (UEdGraphPin* Pin : SaveNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input && 
							Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
						{
							SaveInput = Pin;
							break;
						}
					}
					if (BodyOutput && SaveInput)
					{
						ConnectPins(BodyOutput, SaveInput);
					}
				}
			}
		}
	}
	
	// Step 4: Build the root animation tree
	if (NewAST->RootNode.IsValid())
	{
		UAnimGraphNode_Base* RootTree = BuildAnimNode(NewAST->RootNode, AnimGraph);
		
		if (RootTree)
		{
			UAnimGraphNode_Root* RootNode = nullptr;
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				RootNode = Cast<UAnimGraphNode_Root>(Node);
				if (RootNode) break;
			}
			
			if (RootNode)
			{
				UEdGraphPin* TreeOutput = FindOutputPosePin(RootTree);
				UEdGraphPin* RootInput = nullptr;
				for (UEdGraphPin* Pin : RootNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input && 
						Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						RootInput = Pin;
						break;
					}
				}
				if (TreeOutput && RootInput)
				{
					ConnectPins(TreeOutput, RootInput);
				}
			}
		}
	}
	
	return true;
}

// ========== UpdateBlueprint Implementation ==========

FAnimBPImporter::FUpdateResult FAnimBPImporter::UpdateBlueprintDetailed(UAnimBlueprint* ExistingBlueprint, const FString& NewDSLCode)
{
	FUpdateResult Result;
	
	if (!ExistingBlueprint)
	{
		Result.Warnings.Add(TEXT("Null blueprint"));
		return Result;
	}
	
	// Step 1: Export current blueprint to AST (old state)
	TSharedPtr<FAnimGraphAST> OldAST = FAnimBPExporter::ExportToAST(ExistingBlueprint);
	if (!OldAST.IsValid())
	{
		Result.Warnings.Add(TEXT("Failed to export current blueprint to AST — falling back to full rebuild"));
		
		// Parse new DSL
		TArray<FAnimLangParseError> ParseErrors;
		TSharedPtr<FAnimGraphAST> NewAST = FAnimLangParser::Parse(NewDSLCode, ParseErrors);
		if (!NewAST.IsValid())
		{
			Result.Warnings.Add(TEXT("Failed to parse new DSL code"));
			return Result;
		}
		
		// Full rebuild
		Result.bUsedIncrementalPatch = false;
		Result.bSuccess = RebuildAnimGraph(ExistingBlueprint, NewAST);
		
		if (Result.bSuccess)
		{
			FString CompileError;
			CompileBlueprint(ExistingBlueprint, &CompileError);
			if (!CompileError.IsEmpty())
			{
				Result.Warnings.Add(TEXT("Compile: ") + CompileError);
			}
		}
		
		return Result;
	}
	
	// Step 2: Parse new DSL to AST
	TArray<FAnimLangParseError> ParseErrors;
	TSharedPtr<FAnimGraphAST> NewAST = FAnimLangParser::Parse(NewDSLCode, ParseErrors);
	
	for (const FAnimLangParseError& Err : ParseErrors)
	{
		Result.Warnings.Add(FString::Printf(TEXT("Parse: %s"), *Err.ToString()));
	}
	
	if (!NewAST.IsValid())
	{
		Result.Warnings.Add(TEXT("Failed to parse new DSL code"));
		return Result;
	}
	
	// Step 3: Compute diff
	FAnimLangDiffResult Diff = FAnimLangDiffer::Diff(OldAST, NewAST);
	
	Result.NumChanges = Diff.Entries.Num();
	Result.NumPropertyChanges = Diff.NumPropertyChanges();
	Result.NumStructuralChanges = Diff.NumStructuralChanges();
	Result.DiffSummary = Diff.ToSummary();
	
	if (!Diff.HasChanges())
	{
		Result.bSuccess = true;
		Result.Warnings.Add(TEXT("No changes detected between current blueprint and new DSL"));
		return Result;
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("UpdateBlueprint diff: %s"), *Result.DiffSummary);
	
	// Step 4: Decide strategy
	//
	// Use incremental patch when:
	//   - Only property changes (no structural modifications)
	//   - No node additions/removals
	//   - No define changes
	//   - No variable changes
	//
	// Use full rebuild when:
	//   - Any structural change exists
	//   - Root node type changed
	
	bool bCanUseIncremental = (Result.NumStructuralChanges == 0);
	
	if (bCanUseIncremental)
	{
		// ===== INCREMENTAL PATH =====
		UE_LOG(LogAnimBPImporter, Log, TEXT("Using incremental patch (%d property changes)"), Result.NumPropertyChanges);
		Result.bUsedIncrementalPatch = true;
		
		// Use the Patcher module
		FAnimLangPatchResult PatchResult = FAnimLangPatcher::Apply(ExistingBlueprint, Diff, NewAST);
		
		Result.AppliedOps = PatchResult.AppliedOps;
		Result.Warnings.Append(PatchResult.Warnings);
		
		if (PatchResult.FailedOps.Num() > 0)
		{
			// Some incremental operations failed — fall back to full rebuild
			UE_LOG(LogAnimBPImporter, Warning, TEXT("Incremental patch had %d failures — falling back to full rebuild"), PatchResult.FailedOps.Num());
			
			for (const FString& FailedOp : PatchResult.FailedOps)
			{
				Result.Warnings.Add(TEXT("Incremental failed: ") + FailedOp);
			}
			
			// Fall back to full rebuild
			Result.bUsedIncrementalPatch = false;
			Result.bSuccess = RebuildAnimGraph(ExistingBlueprint, NewAST);
			Result.AppliedOps.Add(TEXT("Full AnimGraph rebuild (incremental fallback)"));
		}
		else
		{
			Result.bSuccess = true;
		}
	}
	else
	{
		// ===== FULL REBUILD PATH =====
		UE_LOG(LogAnimBPImporter, Log, TEXT("Using full rebuild (%d structural changes)"), Result.NumStructuralChanges);
		Result.bUsedIncrementalPatch = false;
		
		Result.bSuccess = RebuildAnimGraph(ExistingBlueprint, NewAST);
		
		if (Result.bSuccess)
		{
			Result.AppliedOps.Add(FString::Printf(TEXT("Full AnimGraph rebuild (%d changes applied)"), Result.NumChanges));
		}
	}
	
	// Step 5: Compile
	if (Result.bSuccess)
	{
		FString CompileError;
		if (!CompileBlueprint(ExistingBlueprint, &CompileError))
		{
			Result.Warnings.Add(TEXT("Compile: ") + CompileError);
			// Don't fail — the blueprint is still potentially usable
		}
		else
		{
			Result.AppliedOps.Add(TEXT("Blueprint compiled successfully"));
		}
	}
	
	UE_LOG(LogAnimBPImporter, Log, TEXT("UpdateBlueprint %s: %s"),
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"),
		*Result.DiffSummary);
	
	return Result;
}

bool FAnimBPImporter::UpdateBlueprint(UAnimBlueprint* ExistingBlueprint, const FString& NewDSLCode, FString* OutError)
{
	FUpdateResult Result = UpdateBlueprintDetailed(ExistingBlueprint, NewDSLCode);
	
	if (!Result.bSuccess && OutError)
	{
		*OutError = TEXT("UpdateBlueprint failed");
		for (const FString& W : Result.Warnings)
		{
			*OutError += TEXT("\n  ") + W;
		}
	}
	
	return Result.bSuccess;
}

#endif // WITH_EDITOR
