// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "RigLangExporter.h"
#include "RigLangImporter.h"
#include "RigVMFunctions/Math/RigVMFunction_MathFloat.h"
#include "RigVMFunctions/Math/RigVMFunction_MathVector.h"
#include "RigVMFunctions/RigVMFunction_String.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RigLangGraphImportTests
{
const EAutomationTestFlags Flags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

UControlRigBlueprint* MakeSourceRig()
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_GraphImportSource")),
		RF_Transient, nullptr, GWarn));
	if (!Blueprint) return nullptr;

	Blueprint->AddMemberVariable(TEXT("Speed"), TEXT("float"), true, false, TEXT("3.5"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	URigVMController* LibraryController = Blueprint->GetOrCreateController(Blueprint->GetLocalFunctionLibrary());
	URigVMLibraryNode* WantsToLock = LibraryController->AddFunctionToLibrary(
		TEXT("WantsToLock"), false, FVector2D::ZeroVector, false);
	URigVMLibraryNode* PublicScale = LibraryController->AddFunctionToLibrary(
		TEXT("PublicScale"), false, FVector2D(300.0, 0.0), false);
	URigVMLibraryNode* LocalState = LibraryController->AddFunctionToLibrary(
		TEXT("LocalState"), true, FVector2D(600.0, 0.0), false);
	if (!WantsToLock || !PublicScale || !LocalState) return nullptr;
	Blueprint->MarkFunctionPublic(TEXT("PublicScale"), true);

	URigVMController* FunctionController = Blueprint->GetOrCreateController(PublicScale->GetContainedGraph());
	FunctionController->AddExposedPin(TEXT("Value"), ERigVMPinDirection::Input, TEXT("float"), NAME_None, TEXT("0.0"), false);
	FunctionController->AddExposedPin(TEXT("Result"), ERigVMPinDirection::Output, TEXT("float"), NAME_None, TEXT("0.0"), false);
	URigVMUnitNode* Add = FunctionController->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"), FVector2D::ZeroVector, TEXT("AddOne"), false);
	URigVMVariableNode* FunctionSpeed = FunctionController->AddVariableNode(
		TEXT("Speed"), TEXT("float"), nullptr, true, TEXT("3.5"),
		FVector2D(0.0, 200.0), TEXT("ReadFunctionSpeed"), false);
	URigVMFunctionReferenceNode* FunctionDependency = FunctionController->AddFunctionReferenceNode(
		WantsToLock, FVector2D(200.0, 200.0), TEXT("CallWantsToLock"), false);
	if (!Add
		|| !FunctionController->AddLink(TEXT("Entry.Value"), TEXT("AddOne.A"), false)
		|| !FunctionController->SetPinDefaultValue(TEXT("AddOne.B"), TEXT("1.0"), true, false)
		|| !FunctionController->AddLink(TEXT("AddOne.Result"), TEXT("Return.Result"), false))
	{
		return nullptr;
	}

	URigVMController* LocalStateController = Blueprint->GetOrCreateController(LocalState->GetContainedGraph());
	const FRigVMGraphVariableDescription FunctionLocal = LocalStateController->AddLocalVariable(
		TEXT("FunctionLocal"), TEXT("float"), nullptr, TEXT("7.0"), false, false);
	URigVMVariableNode* FunctionLocalGetter = LocalStateController->AddVariableNode(
		TEXT("FunctionLocal"), TEXT("float"), nullptr, true, TEXT("7.0"),
		FVector2D(0.0, 300.0), TEXT("ReadFunctionLocal"), false);
	URigVMVariableNode* FunctionLocalSetter = LocalStateController->AddVariableNode(
		TEXT("FunctionLocal"), TEXT("float"), nullptr, false, TEXT("7.0"),
		FVector2D(200.0, 300.0), TEXT("WriteFunctionLocal"), false);
	if (FunctionLocal.Name.IsNone() || !FunctionLocalGetter || !FunctionLocalSetter
		|| FunctionLocalGetter->GetVariableGuid() != FunctionLocal.Guid
		|| FunctionLocalSetter->GetVariableGuid() != FunctionLocal.Guid)
	{
		return nullptr;
	}
	Blueprint->RecompileVM();

	FRigVMClient* Client = Blueprint->URigVMBlueprint::GetRigVMClient();
	URigVMGraph* ForwardsGraph = Client->GetDefaultModel();
	URigVMController* ForwardsController = Blueprint->GetOrCreateController(ForwardsGraph);
	URigVMUnitNode* Forwards = ForwardsController->AddUnitNode(
		FRigUnit_BeginExecution::StaticStruct(), FRigUnit::GetMethodName(),
		FVector2D::ZeroVector, TEXT("ForwardsSolve"), false);
	URigVMVariableNode* Speed = ForwardsController->AddVariableNode(
		TEXT("Speed"), TEXT("float"), nullptr, true, TEXT("3.5"), FVector2D(0.0, 200.0), TEXT("ReadSpeed"), false);
	URigVMUnitNode* VectorAdd = ForwardsController->AddUnitNode(
		FRigVMFunction_MathVectorAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(0.0, 400.0), TEXT("VectorWithSubPins"), false);
	URigVMFunctionReferenceNode* Call = ForwardsController->AddFunctionReferenceNode(
		PublicScale, FVector2D(350.0, 200.0), TEXT("CallPublicScale"), false);
	URigVMPin* RemappedSpeedPin = Call ? Call->FindRootPinByName(TEXT("Speed")) : nullptr;
	const bool bRestoredSourceRemapping = RemappedSpeedPin
		&& ForwardsController->BindPinToVariable(
			RemappedSpeedPin->GetPinPath(), TEXT("Speed"), false, false);
	URigVMRerouteNode* Reroute = ForwardsController->AddFreeRerouteNode(
		TEXT("float"), NAME_None, false, NAME_None, TEXT("0.0"),
		FVector2D(200.0, 200.0), TEXT("SpeedReroute"), false);
	URigVMCommentNode* Comment = ForwardsController->AddCommentNode(
		TEXT("Imported graph fixture"), FVector2D(-100.0, -100.0), FVector2D(600.0, 550.0),
		FLinearColor::Black, TEXT("FixtureComment"), false, false);
	URigVMUnitNode* InjectionOwner = ForwardsController->AddUnitNode(
		FRigVMFunction_MathVectorAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(0.0, 600.0), TEXT("AddForInjection"), false);
	URigVMInjectionInfo* Injection = InjectionOwner ? ForwardsController->AddInjectedNode(
		TEXT("AddForInjection.A"), true, FRigVMFunction_MathVectorNegate::StaticStruct(), TEXT("Execute"),
		TEXT("Value"), TEXT("Result"), TEXT("InjectedNegate"), false) : nullptr;
	URigVMNode* Branch = ForwardsController->AddBranchNode(FVector2D(650.0, 0.0), TEXT("TypedBranch"), false, false);
	URigVMNode* IfNode = ForwardsController->AddIfNode(
		TEXT("float"), NAME_None, FVector2D(650.0, 200.0), TEXT("TypedIf"), false, false);
	URigVMNode* SelectNode = ForwardsController->AddSelectNode(
		TEXT("float"), NAME_None, FVector2D(650.0, 400.0), TEXT("TypedSelect"), false, false);
	URigVMNode* MakeTransform = ForwardsController->AddMakeStructNode(
		TEXT("FTransform"), FName(*TBaseStructure<FTransform>::Get()->GetPathName()), TEXT("()"),
		FVector2D(850.0, 400.0), TEXT("TypedMakeTransform"), false);
	URigVMUnitNode* AggregateSeed = ForwardsController->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(650.0, 600.0), TEXT("AggregatedAdd"), false);
	const FString AggregatePin = AggregateSeed
		? ForwardsController->AddAggregatePin(TEXT("AggregatedAdd"), TEXT("C"), TEXT("0.0"), false, false)
		: FString();
	if (!Forwards || !Speed || !VectorAdd || !Call || !Reroute || !Comment || !Injection
		|| !Branch || !IfNode || !SelectNode || !MakeTransform || AggregatePin.IsEmpty()
		|| !ForwardsController->AddLink(TEXT("ReadSpeed.Value"), TEXT("SpeedReroute.Value"), false)
		|| !ForwardsController->AddLink(TEXT("SpeedReroute.Value"), TEXT("CallPublicScale.Value"), false))
	{
		return nullptr;
	}

	URigVMGraph* ConstructionGraph = Client->AddModel(TEXT("ConstructionGraph"), false);
	URigVMController* ConstructionController = Blueprint->GetOrCreateController(ConstructionGraph);
	if (!ConstructionController->AddUnitNode(
		FRigUnit_PrepareForExecution::StaticStruct(), FRigUnit::GetMethodName(),
		FVector2D::ZeroVector, TEXT("Construction"), false))
	{
		return nullptr;
	}
	return Blueprint;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangGraphImportRoundTripTest,
	"AnimBP2FP.RigLang.Importer.GraphRoundTrip",
	RigLangGraphImportTests::Flags)

bool FRigLangGraphImportRoundTripTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Source = RigLangGraphImportTests::MakeSourceRig();
	if (!TestNotNull(TEXT("graph fixture is created"), Source)) return false;
	const FRigLangExportResult SourceExport = FRigLangExporter::Export(Source);
	if (!TestTrue(TEXT("graph fixture exports strictly"), SourceExport.bSuccess)
		|| !TestNotNull(TEXT("source module exists"), SourceExport.Module.Get()))
	{
		for (const FString& Error : SourceExport.Errors) AddError(Error);
		return false;
	}

	const FRigFunctionAST* Internal = SourceExport.Module->Functions.FindByPredicate(
		[](const FRigFunctionAST& Function) { return Function.Name == TEXT("WantsToLock"); });
	const FRigFunctionAST* Public = SourceExport.Module->Functions.FindByPredicate(
		[](const FRigFunctionAST& Function) { return Function.Name == TEXT("PublicScale"); });
	TestNotNull(TEXT("internal WantsToLock function exports"), Internal);
	if (Internal) TestEqual(TEXT("WantsToLock stays internal"), Internal->Visibility, FString(TEXT("internal")));
	TestNotNull(TEXT("public function exports"), Public);
	if (Public)
	{
		TestEqual(TEXT("public visibility exports"), Public->Visibility, FString(TEXT("public")));
		TestTrue(TEXT("function external variable exports"), Public->ExternalVariables.ContainsByPredicate(
			[](const FRigExternalVariableAST& Variable) { return Variable.Name == TEXT("Speed"); }));
		TestTrue(TEXT("function dependency exports with exact identity"), !Public->Dependencies.IsEmpty()
			&& Public->Dependencies[0].HostObject.Contains(TEXT("CR_GraphImportSource"))
			&& Public->Dependencies[0].LibraryNodePath.Contains(TEXT("WantsToLock")));
	}
	TestTrue(TEXT("function-contained local exports"), SourceExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph)
		{
			return Graph.Role == TEXT("function") && Graph.LocalVariables.ContainsByPredicate(
				[](const FRigGraphVariableAST& Variable) { return Variable.Name == TEXT("FunctionLocal"); });
		}));
	TestTrue(TEXT("function-local getter and setter export the declaration guid"),
		SourceExport.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
		{
			const FRigGraphVariableAST* Local = Graph.LocalVariables.FindByPredicate(
				[](const FRigGraphVariableAST& Variable) { return Variable.Name == TEXT("FunctionLocal"); });
			if (!Local) return false;
			int32 MatchingNodes = 0;
			for (const FRigNodeAST& Node : Graph.Nodes)
				if ((Node.StableId == TEXT("ReadFunctionLocal") || Node.StableId == TEXT("WriteFunctionLocal"))
					&& Node.Properties.FindRef(TEXT("variable-guid")).Contains(Local->Guid))
					++MatchingNodes;
			return MatchingNodes == 2;
		}));
	TestTrue(TEXT("Construction and Forwards Solve entries export"),
		SourceExport.Module->Entries.ContainsByPredicate([](const FRigEntryAST& Entry)
		{
			return Entry.EventName == TEXT("Construction");
		}) && SourceExport.Module->Entries.ContainsByPredicate([](const FRigEntryAST& Entry)
		{
			return Entry.EventName == TEXT("Forwards Solve");
		}));
	TestTrue(TEXT("source call variable remapping fixture is active"),
		SourceExport.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				return Node.StableId == TEXT("CallPublicScale")
					&& Node.Properties.FindRef(TEXT("variable-remapping")).Contains(TEXT("Speed"));
			});
		}));

	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_GraphImportStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(*SourceExport.Module, Options);
	if (!TestNotNull(TEXT("graph import creates a Control Rig blueprint"), Imported.Blueprint.Get()))
	{
		AddError(Imported.Diagnostics.ToReport());
		return false;
	}
	TestTrue(TEXT("graph import compiles"), Imported.bCompiled);
	TestFalse(TEXT("graph import has no diagnostics"), Imported.Diagnostics.HasErrors());
	if (URigVMLibraryNode* ImportedLocalState = Imported.Blueprint->GetLocalFunctionLibrary()
		->FindFunction(TEXT("LocalState")))
	{
		const TArray<FRigVMGraphVariableDescription> ImportedLocals =
			ImportedLocalState->GetContainedGraph()->GetLocalVariables();
		const FRigVMGraphVariableDescription* ImportedLocal = ImportedLocals.FindByPredicate(
			[](const FRigVMGraphVariableDescription& Variable)
			{
				return Variable.Name == TEXT("FunctionLocal");
			});
		URigVMVariableNode* ImportedGetter = Cast<URigVMVariableNode>(
			ImportedLocalState->GetContainedGraph()->FindNodeByName(TEXT("ReadFunctionLocal")));
		URigVMVariableNode* ImportedSetter = Cast<URigVMVariableNode>(
			ImportedLocalState->GetContainedGraph()->FindNodeByName(TEXT("WriteFunctionLocal")));
		if (TestNotNull(TEXT("imported function local exists"), ImportedLocal)
			&& TestNotNull(TEXT("imported local getter exists"), ImportedGetter)
			&& TestNotNull(TEXT("imported local setter exists"), ImportedSetter))
		{
			TestEqual(TEXT("local getter binds generated local guid"), ImportedGetter->GetVariableGuid(), ImportedLocal->Guid);
			TestEqual(TEXT("local setter binds generated local guid"), ImportedSetter->GetVariableGuid(), ImportedLocal->Guid);
		}
	}

	const FRigLangExportResult ReExport = FRigLangExporter::Export(Imported.Blueprint);
	if (!TestTrue(TEXT("imported graph re-exports strictly"), ReExport.bSuccess)
		|| !TestNotNull(TEXT("re-exported module exists"), ReExport.Module.Get())) return false;
	TestEqual(TEXT("function inventory is exact"), ReExport.Module->Functions.Num(), SourceExport.Module->Functions.Num());
	TestEqual(TEXT("entry inventory is exact"), ReExport.Module->Entries.Num(), SourceExport.Module->Entries.Num());
	TestEqual(TEXT("graph inventory is exact"), ReExport.Module->Graphs.Num(), SourceExport.Module->Graphs.Num());
	const FRigGraphAST* SourceForwardsGraph = SourceExport.Module->Graphs.FindByPredicate([](const FRigGraphAST& Graph)
	{
		return Graph.Nodes.ContainsByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("ForwardsSolve"); });
	});
	const FRigGraphAST* ReExportedForwardsGraph = ReExport.Module->Graphs.FindByPredicate([](const FRigGraphAST& Graph)
	{
		return Graph.Nodes.ContainsByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("ForwardsSolve"); });
	});
	TestNotNull(TEXT("source forwards graph exists for editor identity"), SourceForwardsGraph);
	TestNotNull(TEXT("re-exported forwards graph exists for editor identity"), ReExportedForwardsGraph);
	if (SourceForwardsGraph && ReExportedForwardsGraph)
	{
		TestEqual(TEXT("editor graph guid is exact"),
			ReExportedForwardsGraph->EditorGuid, SourceForwardsGraph->EditorGuid);
		const FRigNodeAST* SourceForwardsNode = SourceForwardsGraph->Nodes.FindByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("ForwardsSolve"); });
		const FRigNodeAST* ReExportedForwardsNode = ReExportedForwardsGraph->Nodes.FindByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("ForwardsSolve"); });
		if (TestNotNull(TEXT("source node exists for editor identity"), SourceForwardsNode)
			&& TestNotNull(TEXT("re-exported node exists for editor identity"), ReExportedForwardsNode))
		{
			FGuid SourceEditorNodeGuid;
			if (FGuid::Parse(SourceForwardsNode->Guid, SourceEditorNodeGuid))
				TestEqual(TEXT("editor node guid is exact"), ReExportedForwardsNode->Guid, SourceForwardsNode->Guid);
			else
			{
				FGuid ImportedEditorNodeGuid;
				TestTrue(TEXT("source node identity uses the model-path fallback only when no editor node exists"),
					SourceForwardsNode->Guid.StartsWith(TEXT("model:")));
				TestTrue(TEXT("import creates a real editor node identity"),
					FGuid::Parse(ReExportedForwardsNode->Guid, ImportedEditorNodeGuid));
			}
		}
	}
	const FRigFunctionAST* ReExportedPublic = ReExport.Module->Functions.FindByPredicate(
		[](const FRigFunctionAST& Function) { return Function.Name == TEXT("PublicScale"); });
	TestNotNull(TEXT("re-exported public function exists"), ReExportedPublic);
	if (Public && ReExportedPublic)
	{
		TestEqual(TEXT("function external variable inventory is exact"),
			ReExportedPublic->ExternalVariables.Num(), Public->ExternalVariables.Num());
		TestEqual(TEXT("function dependency inventory is exact"),
			ReExportedPublic->Dependencies.Num(), Public->Dependencies.Num());
		const FRigExternalVariableAST* SourceSpeed = Public->ExternalVariables.FindByPredicate(
			[](const FRigExternalVariableAST& Variable) { return Variable.Name == TEXT("Speed"); });
		const FRigExternalVariableAST* ReExportedSpeed = ReExportedPublic->ExternalVariables.FindByPredicate(
			[](const FRigExternalVariableAST& Variable) { return Variable.Name == TEXT("Speed"); });
		if (TestNotNull(TEXT("source external variable exists for identity"), SourceSpeed)
			&& TestNotNull(TEXT("re-exported external variable exists for identity"), ReExportedSpeed))
		{
			TestEqual(TEXT("external variable guid is exact"), ReExportedSpeed->Guid, SourceSpeed->Guid);
		}
	}
	TestTrue(TEXT("call variable remapping is restored"), ReExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				return Node.StableId == TEXT("CallPublicScale")
					&& Node.Properties.FindRef(TEXT("variable-remapping")).Contains(TEXT("Speed"));
			});
		}));
	TestTrue(TEXT("function-contained local is restored"), ReExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph)
		{
			return Graph.Role == TEXT("function") && Graph.LocalVariables.ContainsByPredicate(
				[](const FRigGraphVariableAST& Variable) { return Variable.Name == TEXT("FunctionLocal"); });
		}));
	TestTrue(TEXT("function reference is reconstructed"), ReExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Call; });
		}));
	TestTrue(TEXT("variable node is reconstructed"), ReExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.ContainsByPredicate(
				[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Variable; });
		}));
	TestTrue(TEXT("struct subpins are reconstructed"), ReExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				return Node.Pins.ContainsByPredicate([](const FRigPinAST& Pin) { return !Pin.SubPins.IsEmpty(); });
			});
		}));
	TestTrue(TEXT("links are reconstructed"), ReExport.Module->Graphs.ContainsByPredicate(
		[](const FRigGraphAST& Graph) { return !Graph.Links.IsEmpty(); }));
	TestTrue(TEXT("reroute, comment, and injected nodes are reconstructed"),
		ReExport.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
		{
			const bool bReroute = Graph.Nodes.ContainsByPredicate(
				[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Reroute; });
			const bool bComment = Graph.Nodes.ContainsByPredicate(
				[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Comment; });
			const bool bInjected = Graph.Nodes.ContainsByPredicate(
				[](const FRigNodeAST& Node) { return Node.bInjected; });
			return bReroute && bComment && bInjected;
		}));
	TestTrue(TEXT("branch, if, and select nodes are reconstructed through typed APIs"),
		ReExport.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
		{
			const bool bBranch = Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				return Node.StableId == TEXT("TypedBranch") && Node.Kind == ERigNodeKind::Unit;
			});
			int32 DispatchCount = 0;
			for (const FRigNodeAST& Node : Graph.Nodes)
				if (Node.Kind == ERigNodeKind::Dispatch) ++DispatchCount;
			return bBranch && DispatchCount >= 3;
		}));
	TestTrue(TEXT("aggregate node and contained graph are reconstructed"),
		ReExport.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
		{
			return Graph.Nodes.ContainsByPredicate(
				[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Aggregate; });
		}));
	TestTrue(TEXT("importer graph semantic gate accepted the immediate re-export"),
		Imported.bCompiled && !Imported.Diagnostics.HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangGraphImportArrayShapeTest,
	"AnimBP2FP.RigLang.Importer.ArrayShapeRoundTrip",
	RigLangGraphImportTests::Flags)

bool FRigLangGraphImportArrayShapeTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Source = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_ArrayShapeSource")),
		RF_Transient, nullptr, GWarn));
	if (!Source) return false;
	URigVMController* Controller = Source->GetOrCreateController(
		Source->URigVMBlueprint::GetRigVMClient()->GetDefaultModel());
	URigVMNode* ArrayNode = Controller->AddArrayNode(
		ERigVMOpCode::ArrayClone, TEXT("FTransform"), TBaseStructure<FTransform>::Get(),
		FVector2D::ZeroVector, TEXT("TransformArray"), false, false);
	if (!ArrayNode || !Controller->SetArrayPinSize(
		TEXT("TransformArray.Array"), 2, TEXT("()"), false, false)) return false;
	const FRigLangExportResult Exported = FRigLangExporter::Export(Source);
	if (!Exported.bSuccess || !Exported.Module) return false;
	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_ArrayShapeStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(*Exported.Module, Options);
	if (!TestNotNull(TEXT("array shape import creates a blueprint"), Imported.Blueprint.Get()))
	{
		AddError(Imported.Diagnostics.ToReport());
		return false;
	}
	const FRigLangExportResult ReExported = FRigLangExporter::Export(Imported.Blueprint);
	return TestTrue(TEXT("array shape re-exports"), ReExported.bSuccess && ReExported.Module)
		&& TestTrue(TEXT("array-of-struct elements and nested subpins are exact"),
			ReExported.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
			{
				return Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
				{
					return Node.StableId == TEXT("TransformArray")
						&& Node.Pins.ContainsByPredicate([](const FRigPinAST& Pin)
						{
							return Pin.Path == TEXT("Array") && Pin.SubPins.Num() == 2
								&& Pin.SubPins[0].SubPins.Num() > 0;
						});
				});
			}));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangGraphImportOrdinaryCollapseTest,
	"AnimBP2FP.RigLang.Importer.OrdinaryCollapseRoundTrip",
	RigLangGraphImportTests::Flags)

bool FRigLangGraphImportOrdinaryCollapseTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Source = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_CollapseSource")),
		RF_Transient, nullptr, GWarn));
	if (!Source) return false;
	Source->AddMemberVariable(TEXT("CollapseSpeed"), TEXT("float"), true, false, TEXT("2.0"));
	FKismetEditorUtilities::CompileBlueprint(Source);
	URigVMController* Controller = Source->GetOrCreateController(
		Source->URigVMBlueprint::GetRigVMClient()->GetDefaultModel());
	URigVMUnitNode* OutsideSource = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(-300.0, 0.0), TEXT("OutsideSource"), false);
	URigVMUnitNode* CollapseAddA = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D::ZeroVector, TEXT("CollapseAddA"), false)
		;
	URigVMUnitNode* CollapseAddB = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(200.0, 0.0), TEXT("CollapseAddB"), false);
	URigVMUnitNode* OutsideSink = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(1100.0, 0.0), TEXT("OutsideSink"), false);
	URigVMUnitNode* NestedAddA = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(200.0, 250.0), TEXT("NestedAddA"), false);
	URigVMUnitNode* NestedAddB = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(400.0, 250.0), TEXT("NestedAddB"), false);
	URigVMVariableNode* DeepVariable = Controller->AddVariableNode(
		TEXT("CollapseSpeed"), TEXT("float"), nullptr, true, TEXT("2.0"),
		FVector2D(450.0, 350.0), TEXT("DeepVariable"), false);
	URigVMNode* DeepDispatch = Controller->AddArrayNode(
		ERigVMOpCode::ArrayClone, TEXT("FTransform"), TBaseStructure<FTransform>::Get(),
		FVector2D(600.0, 350.0), TEXT("DeepDispatch"), false, false);
	URigVMRerouteNode* DeepReroute = Controller->AddFreeRerouteNode(
		TEXT("float"), NAME_None, false, NAME_None, TEXT("0.0"),
		FVector2D(750.0, 350.0), TEXT("DeepReroute"), false);
	URigVMInjectionInfo* FirstInjection = CollapseAddA ? Controller->AddInjectedNode(
		TEXT("CollapseAddA.B"), true, FRigVMFunction_MathFloatNegate::StaticStruct(), TEXT("Execute"),
		TEXT("Value"), TEXT("Result"), TEXT("InjectedFirst"), false) : nullptr;
	URigVMInjectionInfo* SecondInjection = CollapseAddA ? Controller->AddInjectedNode(
		TEXT("CollapseAddA.B"), true, FRigVMFunction_MathFloatNegate::StaticStruct(), TEXT("Execute"),
		TEXT("Value"), TEXT("Result"), TEXT("InjectedSecond"), false) : nullptr;
	URigVMCollapseNode* DeepestCollapse = Controller->CollapseNodes(
		{TEXT("NestedAddA"), TEXT("NestedAddB"), TEXT("DeepVariable"),
		 TEXT("DeepDispatch"), TEXT("DeepReroute")}, TEXT("DeepestCollapse"), false, false, false);
	URigVMController* DeepestController = DeepestCollapse
		? Source->GetOrCreateController(DeepestCollapse->GetContainedGraph()) : nullptr;
	URigVMInjectionInfo* DeepInjection = DeepestController ? DeepestController->AddInjectedNode(
		TEXT("NestedAddA.B"), true, FRigVMFunction_MathFloatNegate::StaticStruct(), TEXT("Execute"),
		TEXT("Value"), TEXT("Result"), TEXT("DeepInjected"), false) : nullptr;
	URigVMUnitNode* NestedMidAdd = Controller->AddUnitNode(
		FRigVMFunction_MathFloatAdd::StaticStruct(), TEXT("Execute"),
		FVector2D(400.0, 250.0), TEXT("NestedMidAdd"), false);
	URigVMCollapseNode* NestedCollapse = DeepestCollapse && NestedMidAdd ? Controller->CollapseNodes(
		{TEXT("DeepestCollapse"), TEXT("NestedMidAdd")}, TEXT("NestedCollapse"), false, false, false) : nullptr;
	if (!TestNotNull(TEXT("outside source"), OutsideSource)
		|| !TestNotNull(TEXT("collapse add A"), CollapseAddA)
		|| !TestNotNull(TEXT("collapse add B"), CollapseAddB)
		|| !TestNotNull(TEXT("outside sink"), OutsideSink)
		|| !TestNotNull(TEXT("nested add A"), NestedAddA)
		|| !TestNotNull(TEXT("nested add B"), NestedAddB)
		|| !TestNotNull(TEXT("deep variable"), DeepVariable)
		|| !TestNotNull(TEXT("deep dispatch"), DeepDispatch)
		|| !TestNotNull(TEXT("deep reroute"), DeepReroute)
		|| !TestNotNull(TEXT("deep injection"), DeepInjection)
		|| !TestNotNull(TEXT("first injection"), FirstInjection)
		|| !TestNotNull(TEXT("second injection"), SecondInjection)
		|| !TestNotNull(TEXT("deepest collapse"), DeepestCollapse)
		|| !TestNotNull(TEXT("nested mid add"), NestedMidAdd)
		|| !TestNotNull(TEXT("nested collapse"), NestedCollapse)) return false;
	if (!TestTrue(TEXT("outer input link"), Controller->AddLink(
		TEXT("OutsideSource.Result"), TEXT("CollapseAddA.A"), false))
		|| !TestTrue(TEXT("inner link"), Controller->AddLink(
			TEXT("CollapseAddA.Result"), TEXT("CollapseAddB.A"), false))
		|| !TestTrue(TEXT("outer output link"), Controller->AddLink(
			TEXT("CollapseAddB.Result"), TEXT("OutsideSink.A"), false))
		|| !TestNotNull(TEXT("contained member getter"), Controller->AddVariableNode(
			TEXT("CollapseSpeed"), TEXT("float"), nullptr, true,
			TEXT("2.0"), FVector2D(400.0, 0.0), TEXT("CollapseVariable"), false))
		|| !TestNotNull(TEXT("contained dispatch"), Controller->AddArrayNode(
			ERigVMOpCode::ArrayClone, TEXT("FTransform"), TBaseStructure<FTransform>::Get(),
			FVector2D(600.0, 0.0), TEXT("CollapseDispatch"), false, false))
		|| !TestNotNull(TEXT("contained reroute"), Controller->AddFreeRerouteNode(
			TEXT("float"), NAME_None, false, NAME_None, TEXT("0.0"),
			FVector2D(800.0, 0.0), TEXT("CollapseReroute"), false))) return false;
	NestedCollapse = Controller->CollapseNodes(
			{TEXT("CollapseAddA"), TEXT("CollapseAddB"), TEXT("CollapseVariable"),
			 TEXT("CollapseDispatch"), TEXT("CollapseReroute"), TEXT("NestedCollapse")},
			TEXT("OrdinaryCollapse"), false, false, false);
	if (!TestNotNull(TEXT("ordinary collapse"), NestedCollapse)) return false;
	const FRigLangExportResult Exported = FRigLangExporter::Export(Source);
	if (!Exported.bSuccess || !Exported.Module) return false;
	const FRigGraphAST* SourceContained = Exported.Module->Graphs.FindByPredicate([](const FRigGraphAST& Graph)
	{
		return Graph.Role == TEXT("node-contained")
			&& Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
			{
				return Node.StableId == TEXT("CollapseAddA");
			});
	});
	if (!TestNotNull(TEXT("collapse fixture exports contained graph"), SourceContained)) return false;
	TestTrue(TEXT("collapse fixture exports input boundary link"), SourceContained->Links.ContainsByPredicate(
		[](const FRigLinkAST& Link) { return Link.SourceNodeId == TEXT("Entry"); }));
	TestTrue(TEXT("collapse fixture exports output boundary link"), SourceContained->Links.ContainsByPredicate(
		[](const FRigLinkAST& Link) { return Link.TargetNodeId == TEXT("Return"); }));
	TestTrue(TEXT("collapse fixture exports nested collapse"), SourceContained->Nodes.ContainsByPredicate(
		[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Collapse; }));
	const FRigNodeAST* SourceNestedNode = SourceContained->Nodes.FindByPredicate(
		[](const FRigNodeAST& Node) { return Node.StableId == TEXT("NestedCollapse"); });
	const FRigGraphAST* SourceNestedGraph = SourceNestedNode ? Exported.Module->Graphs.FindByPredicate(
		[SourceNestedNode](const FRigGraphAST& Graph)
		{
			return Graph.StableId == SourceNestedNode->ContainedGraphStableId;
		}) : nullptr;
	const FRigNodeAST* SourceDeepestNode = SourceNestedGraph ? SourceNestedGraph->Nodes.FindByPredicate(
		[](const FRigNodeAST& Node) { return Node.StableId == TEXT("DeepestCollapse"); }) : nullptr;
	const FRigGraphAST* SourceDeepestGraph = SourceDeepestNode ? Exported.Module->Graphs.FindByPredicate(
		[SourceDeepestNode](const FRigGraphAST& Graph)
		{
			return Graph.StableId == SourceDeepestNode->ContainedGraphStableId;
		}) : nullptr;
	TestNotNull(TEXT("collapse fixture exports second nested graph"), SourceNestedGraph);
	TestNotNull(TEXT("collapse fixture exports third-level collapse"), SourceDeepestNode);
	if (TestNotNull(TEXT("collapse fixture exports deepest graph"), SourceDeepestGraph))
	{
		TestTrue(TEXT("deepest graph exports variable"), SourceDeepestGraph->Nodes.ContainsByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("DeepVariable") && Node.Kind == ERigNodeKind::Variable; }));
		TestTrue(TEXT("deepest graph exports dispatch"), SourceDeepestGraph->Nodes.ContainsByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("DeepDispatch") && Node.Kind == ERigNodeKind::Dispatch; }));
		TestTrue(TEXT("deepest graph exports reroute"), SourceDeepestGraph->Nodes.ContainsByPredicate(
			[](const FRigNodeAST& Node) { return Node.StableId == TEXT("DeepReroute") && Node.Kind == ERigNodeKind::Reroute; }));
		TestTrue(TEXT("deepest graph exports injection"), SourceDeepestGraph->Nodes.ContainsByPredicate(
			[](const FRigNodeAST& Node)
			{
				return Node.bInjected && Node.InjectionOwnerPin.Contains(TEXT("NestedAddA.B"));
			}));
	}
	TArray<const FRigNodeAST*> SourceInjections;
	for (const FRigNodeAST& Node : SourceContained->Nodes)
		if (Node.bInjected && Node.InjectionOwnerPin.Contains(TEXT("CollapseAddA.B"))) SourceInjections.Add(&Node);
	SourceInjections.Sort([](const FRigNodeAST& A, const FRigNodeAST& B) { return A.InjectionOrder < B.InjectionOrder; });
	TestEqual(TEXT("collapse fixture exports both injected nodes"), SourceInjections.Num(), 2);
	if (SourceInjections.Num() == 2)
	{
		TestEqual(TEXT("first injection order"), SourceInjections[0]->InjectionOrder, 0);
		TestEqual(TEXT("second injection order"), SourceInjections[1]->InjectionOrder, 1);
	}
	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_CollapseStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(*Exported.Module, Options);
	if (!TestNotNull(TEXT("ordinary collapse import creates a blueprint"), Imported.Blueprint.Get()))
	{
		AddError(Imported.Diagnostics.ToReport());
		return false;
	}
	const FRigLangExportResult ReExported = FRigLangExporter::Export(Imported.Blueprint);
	return TestTrue(TEXT("ordinary collapse re-exports"), ReExported.bSuccess && ReExported.Module)
		&& TestTrue(TEXT("ordinary collapse retains typed contained nodes"),
			ReExported.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
			{
				if (Graph.Role != TEXT("node-contained")) return false;
				const bool bVariable = Graph.Nodes.ContainsByPredicate(
					[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Variable; });
				const bool bDispatch = Graph.Nodes.ContainsByPredicate(
					[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Dispatch; });
				const bool bReroute = Graph.Nodes.ContainsByPredicate(
					[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Reroute; });
				const bool bNested = Graph.Nodes.ContainsByPredicate(
					[](const FRigNodeAST& Node) { return Node.Kind == ERigNodeKind::Collapse; });
				const bool bInputBoundary = Graph.Links.ContainsByPredicate(
					[](const FRigLinkAST& Link) { return Link.SourceNodeId == TEXT("Entry"); });
				const bool bOutputBoundary = Graph.Links.ContainsByPredicate(
					[](const FRigLinkAST& Link) { return Link.TargetNodeId == TEXT("Return"); });
				TArray<int32> Orders;
				for (const FRigNodeAST& Node : Graph.Nodes)
					if (Node.bInjected && Node.InjectionOwnerPin.Contains(TEXT("CollapseAddA.B"))) Orders.Add(Node.InjectionOrder);
				Orders.Sort();
				return bVariable && bDispatch && bReroute && bNested
					&& bInputBoundary && bOutputBoundary && Orders == TArray<int32>({0, 1});
			}))
		&& TestTrue(TEXT("ordinary collapse retains third-level typed nodes and injection"),
			ReExported.Module->Graphs.ContainsByPredicate([](const FRigGraphAST& Graph)
			{
				return Graph.Role == TEXT("node-contained")
					&& Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
					{
						return Node.StableId == TEXT("DeepVariable") && Node.Kind == ERigNodeKind::Variable;
					})
					&& Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
					{
						return Node.StableId == TEXT("DeepDispatch") && Node.Kind == ERigNodeKind::Dispatch;
					})
					&& Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
					{
						return Node.StableId == TEXT("DeepReroute") && Node.Kind == ERigNodeKind::Reroute;
					})
					&& Graph.Nodes.ContainsByPredicate([](const FRigNodeAST& Node)
					{
						return Node.bInjected && Node.InjectionOwnerPin.Contains(TEXT("NestedAddA.B"));
					});
			}));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangGraphImportEmptyDefaultTest,
	"AnimBP2FP.RigLang.Importer.GraphEmptyDefaultRoundTrip",
	RigLangGraphImportTests::Flags)

bool FRigLangGraphImportEmptyDefaultTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Source = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_EmptyDefaultSource")),
		RF_Transient, nullptr, GWarn));
	if (!Source) return false;
	URigVMController* Controller = Source->GetOrCreateController(
		Source->URigVMBlueprint::GetRigVMClient()->GetDefaultModel());
	if (!Controller->AddUnitNode(FRigVMFunction_StringConcat::StaticStruct(), TEXT("Execute"),
		FVector2D::ZeroVector, TEXT("EmptyDefaultNode"), false)
		|| !Controller->SetPinDefaultValue(TEXT("EmptyDefaultNode.B"), TEXT("non-empty"), true, false))
	{
		return false;
	}
	const FRigLangExportResult Exported = FRigLangExporter::Export(Source);
	if (!Exported.bSuccess || !Exported.Module) return false;
	FRigModuleAST Module = *Exported.Module;
	for (FRigGraphAST& Graph : Module.Graphs)
	{
		for (FRigNodeAST& Node : Graph.Nodes)
		{
			if (Node.StableId != TEXT("EmptyDefaultNode")) continue;
			if (FRigPinAST* Pin = Node.Pins.FindByPredicate(
				[](const FRigPinAST& Candidate) { return Candidate.Path == TEXT("B"); }))
			{
				Pin->DefaultValue.Reset();
			}
		}
	}
	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_GraphEmptyDefaultStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(Module, Options);
	if (!TestNotNull(TEXT("empty-default import creates a blueprint"), Imported.Blueprint.Get()))
	{
		AddError(Imported.Diagnostics.ToReport());
		return false;
	}
	return TestFalse(TEXT("empty defaults are restored without semantic drift"), Imported.Diagnostics.HasErrors());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangGraphImportRejectsAmbiguousFunctionIdentityTest,
	"AnimBP2FP.RigLang.Importer.RejectsAmbiguousFunctionIdentity",
	RigLangGraphImportTests::Flags)

bool FRigLangGraphImportRejectsAmbiguousFunctionIdentityTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Source = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_FunctionIdentitySource")),
		RF_Transient, nullptr, GWarn));
	if (!Source) return false;
	URigVMController* LibraryController = Source->GetOrCreateController(Source->GetLocalFunctionLibrary());
	URigVMLibraryNode* LocalFunction = LibraryController->AddFunctionToLibrary(
		TEXT("SameName"), false, FVector2D::ZeroVector, false);
	URigVMController* RootController = Source->GetOrCreateController(
		Source->URigVMBlueprint::GetRigVMClient()->GetDefaultModel());
	if (!LocalFunction || !RootController->AddFunctionReferenceNode(
		LocalFunction, FVector2D::ZeroVector, TEXT("CallSameName"), false)) return false;
	const FRigLangExportResult Exported = FRigLangExporter::Export(Source);
	if (!Exported.bSuccess || !Exported.Module) return false;
	FRigModuleAST Module = *Exported.Module;
	for (FRigGraphAST& Graph : Module.Graphs)
	{
		for (FRigNodeAST& Node : Graph.Nodes)
		{
			if (Node.Kind == ERigNodeKind::Call && Node.FunctionName.Contains(TEXT("SameName")))
			{
				Node.FunctionIdentifier.HostObject = TEXT("/Game/DefinitelyMissing/CR_Other.CR_Other_C");
			}
		}
	}
	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_GraphFunctionIdentityStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(Module, Options);
	TestNull(TEXT("mismatched host identity never falls back to a same-name local function"), Imported.Blueprint.Get());
	return TestTrue(TEXT("mismatched host identity reports a strict diagnostic"), Imported.Diagnostics.HasErrors());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangGraphImportEmptyNamedGraphTest,
	"AnimBP2FP.RigLang.Importer.EmptyNamedGraphRoundTrip",
	RigLangGraphImportTests::Flags)

bool FRigLangGraphImportEmptyNamedGraphTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Source = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_EmptyGraphSource")),
		RF_Transient, nullptr, GWarn));
	if (!Source) return false;
	Source->URigVMBlueprint::GetRigVMClient()->AddModel(TEXT("EmptyNamedGraph"), false);
	const FRigLangExportResult Exported = FRigLangExporter::Export(Source);
	if (!Exported.bSuccess || !Exported.Module) return false;
	const int32 ExpectedGraphs = Exported.Module->Graphs.Num();
	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/CR_EmptyNamedGraphStaging");
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(*Exported.Module, Options);
	if (!TestNotNull(TEXT("empty named graph import creates a blueprint"), Imported.Blueprint.Get()))
	{
		AddError(Imported.Diagnostics.ToReport());
		return false;
	}
	TestFalse(TEXT("empty graph inventory does not claim executable compilation"), Imported.bCompiled);
	const FRigLangExportResult ReExported = FRigLangExporter::Export(Imported.Blueprint);
	return TestTrue(TEXT("empty named graph re-exports"), ReExported.bSuccess && ReExported.Module)
		&& TestEqual(TEXT("empty graph inventory is exact"), ReExported.Module->Graphs.Num(), ExpectedGraphs);
}

#endif
