// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#if ANIMBP2FP_HAS_MODERN_RIGVM_AUTHORING
#include "Misc/AutomationTest.h"
#include "RigLangDiffer.h"
#include "RigLangImportCommandlet.h"
#include "RigLangExporter.h"
#include "RigLangImporter.h"
#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#if ENGINE_MINOR_VERSION >= 7
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RigLangRoundTripTests
{
const ANIMBP2FP_AUTOMATION_TEST_FLAGS_TYPE Flags =
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter;

bool HasPath(const FRigLangDiffResult& Result, const FString& Path)
{
	return Result.Differences.ContainsByPredicate(
		[&Path](const FRigLangDifference& Difference) { return Difference.Path == Path; });
}

bool HasPathPrefix(const FRigLangDiffResult& Result, const FString& Prefix)
{
	return Result.Differences.ContainsByPredicate(
		[&Prefix](const FRigLangDifference& Difference) { return Difference.Path.StartsWith(Prefix); });
}

FRigLangDiffResult DiffWithValidHashes(const FRigModuleAST& OldModule, const FRigModuleAST& NewModule)
{
	FRigModuleAST OldCopy = OldModule;
	FRigModuleAST NewCopy = NewModule;
	OldCopy.Header.ContentHash = FRigLangExporter::ComputeContentHash(OldCopy.ToCanonicalHashInput());
	NewCopy.Header.ContentHash = FRigLangExporter::ComputeContentHash(NewCopy.ToCanonicalHashInput());
	return FRigLangDiffer::Diff(OldCopy, NewCopy);
}

FRigModuleAST MakePathFixture()
{
	FRigModuleAST Module;
	FRigHierarchyElementAST Control;
	Control.Kind = ERigHierarchyElementKind::Control;
	Control.StableId = TEXT("Control:foot_l_ctrl");
	Control.Name = TEXT("foot_l_ctrl");
	Control.ParentName = TEXT("Null:clamped_foot_l_null");
	Module.Hierarchy.Add(Control);

	FRigFunctionAST Function;
	Function.Name = TEXT("WantsToLock");
	FRigNodeAST Node;
	Node.StableId = TEXT("threshold-node");
	Node.Guid = TEXT("11111111-1111-1111-1111-111111111111");
	FRigPinAST Pin;
	Pin.Path = TEXT("FootContactLockThreshold");
	Pin.DefaultValue = TEXT("0.5");
	Node.Pins.Add(Pin);
	Function.Graph.Nodes.Add(Node);
	Module.Functions.Add(Function);

	FRigEntryAST Entry;
	Entry.Name = TEXT("ForwardsSolve");
	FRigLinkAST Link;
	Link.SourceNodeId = TEXT("DoSceneQuery");
	Link.SourcePinPath = TEXT("ExecuteContext");
	Link.TargetNodeId = TEXT("UpdateFloor");
	Link.TargetPinPath = TEXT("ExecuteContext");
	Entry.Graph.Links.Add(Link);
	Module.Entries.Add(Entry);
	return Module;
}

UControlRigBlueprint* MakeSyntheticRig()
{
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UControlRigBlueprint::StaticClass(), TEXT("CR_RoundTripSynthetic")),
		RF_Transient, nullptr, GWarn));
	if (!Blueprint) return nullptr;
	URigVMGraph* Graph = Blueprint->URigVMBlueprint::GetRigVMClient()->GetDefaultModel();
	URigVMController* Controller = Blueprint->GetOrCreateController(Graph);
	if (!Controller || !Controller->AddUnitNode(
		FRigUnit_BeginExecution::StaticStruct(), FRigUnit::GetMethodName(),
		FVector2D::ZeroVector, TEXT("ForwardsSolve"), false)) return nullptr;
	Blueprint->RecompileVM();
	return Blueprint;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangPathAddressedDiffTest,
	"AnimBP2FP.RigLang.RoundTrip.PathAddressedDiff",
	RigLangRoundTripTests::Flags)

bool FRigLangPathAddressedDiffTest::RunTest(const FString& Parameters)
{
	const FRigModuleAST Source = RigLangRoundTripTests::MakePathFixture();
	FRigModuleAST Changed = Source;
	Changed.Header.AssetClassPath = TEXT("/Script/ControlRig.OtherRigBlueprint");
	TestTrue(TEXT("header class diff is explicit"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("module/header/asset-class")));
	Changed = Source;
	Changed.Header.Version = Source.Header.Version + 1;
	TestTrue(TEXT("header version diff is explicit"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("module/header/version")));
	Changed = Source;
	Changed.Header.Properties.Add(TEXT("review-marker"), TEXT("changed"));
	TestTrue(TEXT("header property diff is explicit"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("module/header/property:review-marker")));
	Changed = Source;
	Changed.Header.ContentHash = TEXT("sha256:shared-stale-value");
	FRigModuleAST SameStaleHash = Changed;
	TestTrue(TEXT("matching stale declared hashes are diagnosed"), RigLangRoundTripTests::HasPath(
		FRigLangDiffer::Diff(Changed, SameStaleHash), TEXT("module/content-hash")));
	FRigModuleAST MissingHash = Source;
	MissingHash.Header.ContentHash.Reset();
	TestTrue(TEXT("matching missing declared hashes are diagnosed"), RigLangRoundTripTests::HasPath(
		FRigLangDiffer::Diff(MissingHash, MissingHash), TEXT("module/content-hash")));

	FRigModuleAST ReExported = Source;
	ReExported.Header.ModuleId.AssetPath = TEXT("/Engine/Transient/RT_HeaderReview");
	ReExported.Header.AssetClassPath = TEXT("/Script/ControlRig.UnexpectedClass");
	ReExported.Header.Version = Source.Header.Version + 1;
	ReExported.Header.Properties.Add(TEXT("unexpected"), TEXT("true"));
	ReExported.Header.ContentHash = TEXT("sha256:stale");
	const FRigModuleAST Comparable = RigLangRoundTrip::BuildComparableModule(Source, ReExported);
	TestEqual(TEXT("round-trip remaps only the transient asset path"),
		Comparable.Header.ModuleId.AssetPath, Source.Header.ModuleId.AssetPath);
	TestEqual(TEXT("round-trip retains the re-exported class for diffing"),
		Comparable.Header.AssetClassPath, ReExported.Header.AssetClassPath);
	TestEqual(TEXT("round-trip retains the re-exported version for diffing"),
		Comparable.Header.Version, ReExported.Header.Version);
	TestTrue(TEXT("round-trip retains re-exported header properties for diffing"),
		Comparable.Header.Properties.Contains(TEXT("unexpected")));
	TestEqual(TEXT("round-trip recomputes content hash after the asset-path remap"),
		Comparable.Header.ContentHash,
		FRigLangExporter::ComputeContentHash(Comparable.ToCanonicalHashInput()));

	Changed = Source;
	Changed.Hierarchy[0].ParentName = TEXT("Null:other_parent");
	TestTrue(TEXT("hierarchy parent diff is path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("hierarchy/control:foot_l_ctrl/parent")));

	Changed = Source;
	Changed.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("0.75");
	TestTrue(TEXT("function pin diff is path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed),
		TEXT("function:WantsToLock/node:11111111-1111-1111-1111-111111111111/pin:FootContactLockThreshold/default")));

	Changed = Source;
	Changed.Entries[0].Graph.Links[0].TargetNodeId = TEXT("UpdateFloorChanged");
	TestTrue(TEXT("entry link diff is path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed),
		TEXT("entry:ForwardsSolve/link:DoSceneQuery.ExecuteContext->UpdateFloor.ExecuteContext")));

	auto MakeOwnedContainedGraphs = [](const FString& Prefix, const bool bSwapPayloads)
	{
		FRigModuleAST Module;
		for (int32 OwnerIndex = 0; OwnerIndex < 2; ++OwnerIndex)
		{
			const FString OwnerName = OwnerIndex == 0 ? TEXT("OwnerA") : TEXT("OwnerB");
			FRigGraphAST& Owner = Module.Graphs.AddDefaulted_GetRef();
			Owner.StableId = Prefix + TEXT("/") + OwnerName;
			Owner.Role = TEXT("function");
			Owner.Properties.Add(TEXT("graph-name"), TEXT("\"") + OwnerName + TEXT("\""));
			FRigNodeAST& OwnerNode = Owner.Nodes.AddDefaulted_GetRef();
			OwnerNode.Kind = ERigNodeKind::Collapse;
			OwnerNode.StableId = TEXT("SharedOwner");
			OwnerNode.Guid = OwnerIndex == 0
				? TEXT("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")
				: TEXT("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
			OwnerNode.ContainedGraphStableId = Prefix + TEXT("/") + OwnerName + TEXT("/Shared");

			FRigGraphAST& Child = Module.Graphs.AddDefaulted_GetRef();
			Child.StableId = OwnerNode.ContainedGraphStableId;
			Child.ParentStableId = Owner.StableId;
			Child.Role = TEXT("node-contained");
			Child.Properties.Add(TEXT("graph-name"), TEXT("\"Shared\""));
			FRigNodeAST& Payload = Child.Nodes.AddDefaulted_GetRef();
			const int32 PayloadIndex = bSwapPayloads ? 1 - OwnerIndex : OwnerIndex;
			Payload.StableId = PayloadIndex == 0 ? TEXT("PayloadA") : TEXT("PayloadB");
			Payload.Guid = PayloadIndex == 0
				? TEXT("11111111-aaaa-aaaa-aaaa-aaaaaaaaaaaa")
				: TEXT("22222222-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
		}
		return Module;
	};
	const FRigModuleAST OwnedSource = MakeOwnedContainedGraphs(TEXT("source"), false);
	const FRigModuleAST OwnedSwapped = MakeOwnedContainedGraphs(TEXT("imported"), true);
	TestFalse(TEXT("same-name contained graphs under different owners cannot exchange payloads"),
		FRigLangImporter::BuildGraphSemanticSnapshot(OwnedSource, OwnedSource).Equals(
			FRigLangImporter::BuildGraphSemanticSnapshot(OwnedSwapped, OwnedSource),
			ESearchCase::CaseSensitive));
	TestFalse(TEXT("owned same-name contained graph exchange is a structured difference"),
		RigLangRoundTripTests::DiffWithValidHashes(OwnedSource, OwnedSwapped).IsEmpty());
	FRigModuleAST RealGuidChanged = OwnedSource;
	RealGuidChanged.Graphs[0].EditorGuid = TEXT("77777777-7777-7777-7777-777777777777");
	TestTrue(TEXT("real graph editor GUID mutation is retained"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(OwnedSource, RealGuidChanged),
		TEXT("graph:") + OwnedSource.Graphs[0].StableId + TEXT("/graph/editor-guid")));
	RealGuidChanged = OwnedSource;
	RealGuidChanged.Graphs[0].Nodes[0].Guid = TEXT("88888888-8888-8888-8888-888888888888");
	TestFalse(TEXT("real node GUID mutation is retained"),
		RigLangRoundTripTests::DiffWithValidHashes(OwnedSource, RealGuidChanged).IsEmpty());

	FRigModuleAST GraphRemoved = OwnedSource;
	const FString RemovedGraphId = GraphRemoved.Graphs.Last().StableId;
	GraphRemoved.Graphs.Pop();
	FRigLangDiffResult GraphSetDiff = RigLangRoundTripTests::DiffWithValidHashes(OwnedSource, GraphRemoved);
	TestTrue(TEXT("removed graph has an explicit path"), RigLangRoundTripTests::HasPath(
		GraphSetDiff, TEXT("graph:") + RemovedGraphId));
	FRigGraphAST AddedGraph;
	AddedGraph.StableId = TEXT("source/AddedGraph");
	AddedGraph.Role = TEXT("root");
	GraphRemoved.Graphs.Add(AddedGraph);
	GraphSetDiff = RigLangRoundTripTests::DiffWithValidHashes(OwnedSource, GraphRemoved);
	TestTrue(TEXT("added graph has an explicit path"), RigLangRoundTripTests::HasPath(
		GraphSetDiff, TEXT("graph:") + AddedGraph.StableId));

	Changed = Source;
	FRigImportAST Import;
	Import.Import.Target.AssetPath = TEXT("/Game/Rigs/SharedRig");
	Import.Import.Alias = TEXT("shared");
	Changed.Imports.Add(Import);
	TestTrue(TEXT("import inventory is path-addressed"), RigLangRoundTripTests::HasPathPrefix(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("import:/Game/Rigs/SharedRig")));
	Changed = Source;
	FRigVariableAST AddedVariable;
	AddedVariable.StableId = TEXT("66666666-6666-6666-6666-666666666666");
	AddedVariable.Name = TEXT("StrideScale");
	Changed.Variables.Add(AddedVariable);
	TestTrue(TEXT("module variable inventory is path-addressed"), RigLangRoundTripTests::HasPathPrefix(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("variable:StrideScale")));
	Changed = Source;
	Changed.Functions[0].Visibility = TEXT("public");
	TestTrue(TEXT("function signature fields are path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("function:WantsToLock/visibility")));
	Changed = Source;
	Changed.Entries[0].EventName = TEXT("OtherEvent");
	TestTrue(TEXT("entry signature fields are path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("entry:ForwardsSolve/event-name")));
	Changed = Source;
	Changed.Functions[0].Graph.Role = TEXT("function");
	TestTrue(TEXT("graph fields are path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed), TEXT("function:WantsToLock/graph/role")));
	Changed = Source;
	Changed.Functions[0].Graph.Nodes[0].Coverage = ERigNodeCoverage::Lossy;
	TestTrue(TEXT("node coverage is path-addressed"), RigLangRoundTripTests::HasPath(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed),
		TEXT("function:WantsToLock/node:11111111-1111-1111-1111-111111111111/coverage")));
	Changed = Source;
	Changed.Functions[0].Graph.Nodes[0].Pins[0].Properties.Add(TEXT("constant"), TEXT("true"));
	TestTrue(TEXT("pin properties are path-addressed"), RigLangRoundTripTests::HasPathPrefix(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed),
		TEXT("function:WantsToLock/node:11111111-1111-1111-1111-111111111111/pin:FootContactLockThreshold/property:")));
	Changed = Source;
	Changed.Entries[0].Graph.Links[0].Properties.Add(TEXT("weight"), TEXT("1"));
	TestTrue(TEXT("link properties are path-addressed"), RigLangRoundTripTests::HasPathPrefix(
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed),
		TEXT("entry:ForwardsSolve/link:DoSceneQuery.ExecuteContext->UpdateFloor.ExecuteContext/property:")));
	Changed = Source;
	Changed.Hierarchy[0].ParentName = TEXT("Null:other_parent");
	Changed.Hierarchy[0].Properties.Add(TEXT("uncovered-review-field"), TEXT("true"));
	const FRigLangDiffResult ExplicitAndFallback = RigLangRoundTripTests::DiffWithValidHashes(Source, Changed);
	TestTrue(TEXT("explicit difference remains"), RigLangRoundTripTests::HasPath(
		ExplicitAndFallback, TEXT("hierarchy/control:foot_l_ctrl/parent")));
	TestTrue(TEXT("canonical entity fallback is added even after an explicit difference"),
		RigLangRoundTripTests::HasPathPrefix(ExplicitAndFallback,
			TEXT("hierarchy/control:foot_l_ctrl/canonical")));
	FString ImporterSource;
	const FString ImporterSourcePath = FPaths::ProjectPluginsDir()
		/ TEXT("AnimBP2FP/Source/AnimBP2FP/Private/RigLangImporter.cpp");
	TestTrue(TEXT("RigLang importer source is readable for private API audit"),
		FFileHelper::LoadFileToString(ImporterSource, *ImporterSourcePath));
	TestFalse(TEXT("RigLang importer has no private member accessor shim"),
		ImporterSource.Contains(TEXT("TPrivateMemberAccessor"))
		|| ImporterSource.Contains(TEXT("FChangeRigPinTypeTag"))
		|| ImporterSource.Contains(TEXT("GetPrivateMember")));
	TestTrue(TEXT("RigLang importer resolves template permutations through public APIs"),
		ImporterSource.Contains(TEXT("FindPermutation"))
		&& ImporterSource.Contains(TEXT("GetTypesForPermutation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangDocumentedNormalizationTest,
	"AnimBP2FP.RigLang.RoundTrip.DocumentedNormalization",
	RigLangRoundTripTests::Flags)

bool FRigLangDocumentedNormalizationTest::RunTest(const FString& Parameters)
{
	FRigModuleAST Source = RigLangRoundTripTests::MakePathFixture();
	FRigModuleAST Changed = Source;
	FRigPinAST CachePin;
	CachePin.Path = TEXT("CachedIndex");
	CachePin.Direction = ERigPinDirection::Hidden;
	CachePin.Type.CPPType = TEXT("FCachedRigElement");
	CachePin.DefaultValue = TEXT("(Key=(Type=None,Name=\"None\"),Index=65535,ContainerVersion=-1)");
	Source.Functions[0].Graph.Nodes[0].Pins.Add(CachePin);
	Changed = Source;
	Changed.Functions[0].Graph.Nodes[0].Pins.Pop();
	TestTrue(TEXT("documented transient Rig element cache normalization is ignored"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());

	Changed = Source;
	Changed.Functions[0].Graph.Nodes[0].Properties.Add(TEXT("editor-position"), TEXT("(10 20)"));
	TestTrue(TEXT("documented editor position normalization is ignored"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());
	FRigNodeAST Comment;
	Comment.Kind = ERigNodeKind::Comment;
	Comment.StableId = TEXT("EditorNote");
	Comment.Guid = TEXT("44444444-4444-4444-4444-444444444444");
	Comment.Properties.Add(TEXT("comment-text"), TEXT("\"Keep this note\""));
	Changed.Functions[0].Graph.Nodes.Add(Comment);
	TestFalse(TEXT("comment presence is semantic"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());
	FRigModuleAST CommentChanged = Changed;
	CommentChanged.Functions[0].Graph.Nodes.Last().Guid =
		TEXT("55555555-5555-5555-5555-555555555555");
	TestFalse(TEXT("comment GUID is semantic"),
		RigLangRoundTripTests::DiffWithValidHashes(Changed, CommentChanged).IsEmpty());
	CommentChanged = Changed;
	CommentChanged.Functions[0].Graph.Nodes.Last().Properties[TEXT("comment-text")] =
		TEXT("\"Changed note\"");
	TestFalse(TEXT("comment text is semantic"),
		RigLangRoundTripTests::DiffWithValidHashes(Changed, CommentChanged).IsEmpty());
	CommentChanged = Changed;
	FRigNodeAST& LayoutComment = CommentChanged.Functions[0].Graph.Nodes.Last();
	LayoutComment.Properties.Add(TEXT("font-size"), TEXT("36"));
	LayoutComment.Properties.Add(TEXT("bubble-visible"), TEXT("true"));
	LayoutComment.Properties.Add(TEXT("color-bubble"), TEXT("true"));
	LayoutComment.Properties.Add(TEXT("editor-position"), TEXT("(200 300)"));
	TestTrue(TEXT("comment visual layout properties are ignored"),
		RigLangRoundTripTests::DiffWithValidHashes(Changed, CommentChanged).IsEmpty());
	FRigHierarchyTransformAST WorkTransform;
	WorkTransform.Role = ERigHierarchyTransformRole::CurrentLocal;
	WorkTransform.Value.SetTranslation(FVector(10.0, 20.0, 30.0));
	Changed.Hierarchy[0].Transforms.Add(WorkTransform);
	Changed.Functions[0].Graph.Nodes.Pop();
	TestTrue(TEXT("documented editor/current-work normalization is ignored"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());

	Source = RigLangRoundTripTests::MakePathFixture();
	FRigHierarchyTransformAST SignedZeroTransform;
	SignedZeroTransform.Role = ERigHierarchyTransformRole::InitialLocal;
	SignedZeroTransform.Value = FTransform(FQuat(0.0, -0.0, 0.0, 1.0));
	Source.Hierarchy[0].Transforms.Add(SignedZeroTransform);
	FRigVariableAST TransformVariable;
	TransformVariable.Name = TEXT("FootTransform");
	TransformVariable.StableId = TEXT("33333333-3333-3333-3333-333333333333");
	TransformVariable.Type.CPPType = TEXT("FTransform");
	TransformVariable.Type.CPPTypeObject = TEXT("/Script/CoreUObject.Transform");
	TransformVariable.DefaultValue = TEXT("(Rotation=(X=0.000000,Y=-0.000000,Z=0.000000,W=1.000000),Translation=(X=0.000000,Y=0.000000,Z=0.000000),Scale3D=(X=1.000000,Y=1.000000,Z=1.000000))");
	Source.Variables.Add(TransformVariable);
	Changed = Source;
	Changed.Hierarchy[0].Transforms[0].Value = FTransform::Identity;
	Changed.Variables[0].DefaultValue = FTransform::Identity.ToString();
	TestTrue(TEXT("documented transform signed-zero and Blueprint text normalization is ignored"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());

	Source = RigLangRoundTripTests::MakePathFixture();
	FRigFunctionDependencyAST Dependency;
	Dependency.HostObject = TEXT("$local");
	Dependency.LibraryNodePath = TEXT("AlphaLinearInterp");
	Dependency.Hash = 1234;
	Source.Functions[0].Dependencies.Add(Dependency);
	Changed = Source;
	Changed.Functions[0].Dependencies[0].Hash = 5678;
	TestFalse(TEXT("dependency cache hash remains available in canonical text"),
		Source.ToCanonicalString() == Changed.ToCanonicalString());
	TestEqual(TEXT("dependency cache hash is excluded from semantic hash input"),
		Source.ToCanonicalHashInput(), Changed.ToCanonicalHashInput());
	const FRigLangDiffResult DependencyHashDiff = RigLangRoundTripTests::DiffWithValidHashes(Source, Changed);
	TestTrue(TEXT("dependency cache hash is ignored by structured semantic diff"),
		DependencyHashDiff.IsEmpty());
	Changed.Functions[0].Dependencies[0].LibraryNodePath = TEXT("DifferentFunction");
	TestFalse(TEXT("dependency library node identity remains semantic"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());
	Changed = Source;
	Changed.Functions[0].Dependencies[0].HostObject = TEXT("/Game/OtherRig.OtherRig");
	TestFalse(TEXT("dependency host identity remains semantic"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());
	Source = RigLangRoundTripTests::MakePathFixture();
	Source.Functions[0].Visibility = TEXT("public");
	Changed = Source;
	Changed.Functions[0].Visibility = TEXT("Public");
	TestFalse(TEXT("canonical-only function field comparison is case-sensitive"),
		RigLangRoundTripTests::DiffWithValidHashes(Source, Changed).IsEmpty());

	FRigModuleAST ResolvedUnitSource = RigLangRoundTripTests::MakePathFixture();
	FRigNodeAST& ResolvedUnit = ResolvedUnitSource.Functions[0].Graph.Nodes[0];
	ResolvedUnit.Kind = ERigNodeKind::Unit;
	ResolvedUnit.ClassPath = TEXT("/Script/RigVMDeveloper.RigVMUnitNode");
	ResolvedUnit.MethodName = TEXT("Execute");
	ResolvedUnit.Properties.Add(TEXT("script-struct"), TEXT("\"/Script/RigVM.RigVMFunction_MathBoolOr\""));
	ResolvedUnit.Properties.Add(TEXT("resolved-function"), TEXT("\"FRigVMFunction_MathBoolOr::Execute\""));
	ResolvedUnit.Properties.Add(TEXT("template-resolved"), TEXT("true"));
	ResolvedUnit.Properties.Add(TEXT("template-types"), TEXT("((\"A\" \"bool\") (\"B\" \"bool\") (\"Result\" \"bool\"))"));
	ResolvedUnit.Properties.Add(TEXT("template-notation"), TEXT("\"Or::Execute(in A,in B,out Result)\""));
	FRigModuleAST ResolvedUnitChanged = ResolvedUnitSource;
	ResolvedUnitChanged.Functions[0].Graph.Nodes[0].Properties[TEXT("template-notation")] = TEXT("\"None\"");
	TestTrue(TEXT("resolved UnitNode legacy notation is ignored"),
		RigLangRoundTripTests::DiffWithValidHashes(ResolvedUnitSource, ResolvedUnitChanged).IsEmpty());
	for (const FString& Field : {TEXT("class"), TEXT("method"), TEXT("resolved-function"), TEXT("template-types")})
	{
		ResolvedUnitChanged = ResolvedUnitSource;
		FRigNodeAST& ChangedUnit = ResolvedUnitChanged.Functions[0].Graph.Nodes[0];
		if (Field == TEXT("class")) ChangedUnit.ClassPath = TEXT("/Script/RigVMDeveloper.OtherNode");
		else if (Field == TEXT("method")) ChangedUnit.MethodName = TEXT("OtherMethod");
		else if (Field == TEXT("resolved-function")) ChangedUnit.Properties[Field] = TEXT("\"Other::Execute\"");
		else ChangedUnit.Properties[Field] = TEXT("((\"A\" \"float\"))");
		TestFalse(TEXT("resolved UnitNode semantic change is retained: ") + Field,
			RigLangRoundTripTests::DiffWithValidHashes(ResolvedUnitSource, ResolvedUnitChanged).IsEmpty());
	}

	FRigModuleAST UnresolvedTemplateSource = RigLangRoundTripTests::MakePathFixture();
	FRigNodeAST& UnresolvedTemplate = UnresolvedTemplateSource.Functions[0].Graph.Nodes[0];
	UnresolvedTemplate.Properties.Add(TEXT("template-resolved"), TEXT("false"));
	UnresolvedTemplate.Properties.Add(TEXT("resolved-function"), TEXT("\"Stale::PermutationA\""));
	FRigModuleAST UnresolvedTemplateChanged = UnresolvedTemplateSource;
	UnresolvedTemplateChanged.Functions[0].Graph.Nodes[0].Properties[TEXT("resolved-function")] =
		TEXT("\"Stale::PermutationB\"");
	TestEqual(TEXT("unresolved stale function is excluded from semantic hash input"),
		UnresolvedTemplateSource.ToCanonicalHashInput(),
		UnresolvedTemplateChanged.ToCanonicalHashInput());
	TestTrue(TEXT("unresolved stale function is ignored by structured semantic diff"),
		RigLangRoundTripTests::DiffWithValidHashes(
			UnresolvedTemplateSource, UnresolvedTemplateChanged).IsEmpty());
	UnresolvedTemplateSource.Functions[0].Graph.Nodes[0].Properties[TEXT("template-resolved")] = TEXT("true");
	UnresolvedTemplateChanged = UnresolvedTemplateSource;
	UnresolvedTemplateChanged.Functions[0].Graph.Nodes[0].Properties[TEXT("resolved-function")] =
		TEXT("\"Other::Permutation\"");
	TestFalse(TEXT("resolved template function remains semantic"),
		RigLangRoundTripTests::DiffWithValidHashes(
			UnresolvedTemplateSource, UnresolvedTemplateChanged).IsEmpty());

	FRigModuleAST TypedDefaultSource = RigLangRoundTripTests::MakePathFixture();
	FRigPinAST& TypedPin = TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0];
	TypedPin.Type.CPPType = TEXT("bool");
	TypedPin.DefaultValue = TEXT("true");
	FRigModuleAST TypedDefaultChanged = TypedDefaultSource;
	TypedDefaultChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("True");
	TestTrue(TEXT("typed bool default case is normalized"),
		RigLangRoundTripTests::DiffWithValidHashes(TypedDefaultSource, TypedDefaultChanged).IsEmpty());
	TypedDefaultChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("false");
	TestFalse(TEXT("typed bool value change is retained"),
		RigLangRoundTripTests::DiffWithValidHashes(TypedDefaultSource, TypedDefaultChanged).IsEmpty());
	for (const FString& StringType : {TEXT("FString"), TEXT("FName")})
	{
		TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].Type.CPPType = StringType;
		TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("true");
		TypedDefaultChanged = TypedDefaultSource;
		TypedDefaultChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("True");
		const FRigLangDiffResult StringDiff =
			RigLangRoundTripTests::DiffWithValidHashes(TypedDefaultSource, TypedDefaultChanged);
		TestFalse(TEXT("string/name default case remains semantic: ") + StringType,
			StringDiff.IsEmpty());
	}
	TypedDefaultSource = RigLangRoundTripTests::MakePathFixture();
	TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].Type.CPPType = TEXT("FQuat");
	TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].Type.CPPTypeObject =
		TEXT("/Script/CoreUObject.Quat");
	TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("()");
	TypedDefaultChanged = TypedDefaultSource;
	TypedDefaultChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue =
		TEXT("(X=0.000000,Y=0.000000,Z=0.000000,W=1.000000)");
	TestTrue(TEXT("typed empty quaternion and identity default are equivalent"),
		RigLangRoundTripTests::DiffWithValidHashes(TypedDefaultSource, TypedDefaultChanged).IsEmpty());
	TypedDefaultChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue =
		TEXT("(X=0.000000,Y=0.000000,Z=1.000000,W=0.000000)");
	TestFalse(TEXT("typed nonidentity quaternion default is retained"),
		RigLangRoundTripTests::DiffWithValidHashes(TypedDefaultSource, TypedDefaultChanged).IsEmpty());
	TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].Type.CPPType = TEXT("FString");
	TypedDefaultSource.Functions[0].Graph.Nodes[0].Pins[0].Type.CPPTypeObject.Reset();
	TypedDefaultChanged = TypedDefaultSource;
	TypedDefaultChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue =
		TEXT("(X=0.000000,Y=0.000000,Z=0.000000,W=1.000000)");
	TestFalse(TEXT("string empty tuple is not quaternion-normalized"),
		RigLangRoundTripTests::DiffWithValidHashes(TypedDefaultSource, TypedDefaultChanged).IsEmpty());

	FRigModuleAST VariableIdentitySource = RigLangRoundTripTests::MakePathFixture();
	FRigVariableAST DeclaredVariable;
	DeclaredVariable.StableId = TEXT("44444444-4444-4444-4444-444444444444");
	DeclaredVariable.Name = TEXT("RightFootPinned");
	DeclaredVariable.Type.CPPType = TEXT("bool");
	VariableIdentitySource.Variables.Add(DeclaredVariable);
	FRigNodeAST& VariableNode = VariableIdentitySource.Functions[0].Graph.Nodes[0];
	VariableNode.Kind = ERigNodeKind::Variable;
	VariableNode.Properties.Add(TEXT("external"), TEXT("true"));
	VariableNode.Properties.Add(TEXT("variable-guid"), TEXT("\"44444444-4444-4444-4444-444444444444\""));
	VariableNode.Properties.Add(TEXT("variable-name"), TEXT("\"RIghtFootPinned\""));
	VariableNode.Pins[0].Path = TEXT("Variable");
	VariableNode.Pins[0].Type.CPPType = TEXT("FName");
	VariableNode.Pins[0].DefaultValue = TEXT("RIghtFootPinned");
	FRigModuleAST VariableIdentityChanged = VariableIdentitySource;
	VariableIdentityChanged.Functions[0].Graph.Nodes[0].Properties[TEXT("variable-name")] =
		TEXT("\"RightFootPinned\"");
	VariableIdentityChanged.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("RightFootPinned");
	FRigModuleAST ExportCanonicalized = VariableIdentitySource;
	FRigLangExporter::CanonicalizeExternalVariableNames(ExportCanonicalized);
	TestEqual(TEXT("export boundary canonicalizes external variable-name"),
		ExportCanonicalized.Functions[0].Graph.Nodes[0].Properties.FindRef(TEXT("variable-name")),
		TEXT("\"RightFootPinned\""));
	TestEqual(TEXT("export boundary canonicalizes external Variable pin"),
		ExportCanonicalized.Functions[0].Graph.Nodes[0].Pins[0].DefaultValue,
		TEXT("RightFootPinned"));
	TestFalse(TEXT("differ does not forgive stale external variable display names"),
		RigLangRoundTripTests::DiffWithValidHashes(VariableIdentitySource, VariableIdentityChanged).IsEmpty());
	VariableIdentityChanged.Functions[0].Graph.Nodes[0].Properties[TEXT("variable-guid")] =
		TEXT("\"55555555-5555-5555-5555-555555555555\"");
	TestFalse(TEXT("external variable different Guid is retained"),
		RigLangRoundTripTests::DiffWithValidHashes(VariableIdentitySource, VariableIdentityChanged).IsEmpty());
	VariableIdentityChanged = VariableIdentitySource;
	VariableIdentityChanged.Variables[0].Name = TEXT("DifferentVariable");
	TestFalse(TEXT("external variable different declaration name is retained"),
		RigLangRoundTripTests::DiffWithValidHashes(VariableIdentitySource, VariableIdentityChanged).IsEmpty());

	Changed = Source;
	Changed.Hierarchy[0].Name = TEXT("clamped_toe_l_null");
	Changed.Hierarchy[0].Kind = ERigHierarchyElementKind::Null;
	Changed.Hierarchy[0].ParentName = TEXT("Null:wrong_parent");
	const FRigLangDiffResult ParentDiff = RigLangRoundTripTests::DiffWithValidHashes(Source, Changed);
	TestFalse(TEXT("clamped toe parent is not normalized"), ParentDiff.IsEmpty());

	Source.Entries[0].Graph.Nodes.Reset();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FRigNodeAST InitLeg;
		InitLeg.StableId = FString::Printf(TEXT("Init Leg %d"), Index);
		InitLeg.Guid = FString::Printf(TEXT("22222222-2222-2222-2222-%012d"), Index + 1);
		FRigPinAST FloorNull;
		FloorNull.Path = TEXT("FloorNull");
		FloorNull.DefaultValue = Index == 0 ? TEXT("clamped_toe_l_null") : TEXT("clamped_foot_l_null");
		InitLeg.Pins.Add(FloorNull);
		Source.Entries[0].Graph.Nodes.Add(InitLeg);
	}
	Changed = Source;
	Changed.Entries[0].Graph.Nodes[0].Pins[0].DefaultValue = TEXT("None");
	Changed.Entries[0].Graph.Nodes[1].Pins[0].DefaultValue = TEXT("None");
	const FRigLangDiffResult FloorDiff = RigLangRoundTripTests::DiffWithValidHashes(Source, Changed);
	TestEqual(TEXT("both Init Leg floor-null arguments participate in diff"),
		FloorDiff.Differences.FilterByPredicate([](const FRigLangDifference& Difference)
		{
			return Difference.Path.Contains(TEXT("/pin:FloorNull/default"));
		}).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangSyntheticTransientRoundTripTest,
	"AnimBP2FP.RigLang.RoundTrip.SyntheticTransient",
	RigLangRoundTripTests::Flags)

bool FRigLangSyntheticTransientRoundTripTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Source = RigLangRoundTripTests::MakeSyntheticRig();
	if (!TestNotNull(TEXT("synthetic Control Rig is created"), Source)) return false;
	const FRigLangTransientRoundTripResult Result = RigLangRoundTrip::RunTransient(
		Source, TEXT("Automation-Synthetic"));
	if (!Result.bSuccess)
	{
		for (const FString& Diagnostic : Result.Diagnostics) AddError(Diagnostic);
		if (!Result.Diff.IsEmpty()) AddError(TEXT("Structured diff: ") + Result.Diff.ToJson());
	}
	TestTrue(TEXT("synthetic transient round-trip succeeds"), Result.bSuccess);
	TestEqual(TEXT("synthetic compile errors are zero"), Result.CompileErrorCount, 0);
	TestTrue(TEXT("synthetic semantic diff is empty"), Result.Diff.IsEmpty());
	for (const FString& Name : {TEXT("source.riglang"), TEXT("imported.riglang"),
		TEXT("expected.normalized.riglang"), TEXT("actual.normalized.riglang"),
		TEXT("diff.json"), TEXT("diagnostics.txt"), TEXT("diagnostics.json"), TEXT("summary.json")})
	{
		TestTrue(TEXT("round-trip artifact is preserved: ") + Name,
			FPaths::FileExists(Result.ArtifactDirectory / Name));
	}

	const FString ReusedRunId = TEXT("Automation-Reused-Artifact-RunId");
	const FRigLangTransientRoundTripResult ReusedSuccess =
		RigLangRoundTrip::RunTransient(Source, ReusedRunId);
	TestTrue(TEXT("reused artifact RunId first succeeds"), ReusedSuccess.bSuccess);
	const FRigLangTransientRoundTripResult ReusedFailure =
		RigLangRoundTrip::RunTransient(nullptr, ReusedRunId);
	TestFalse(TEXT("reused artifact RunId second run fails"), ReusedFailure.bSuccess);
	TestEqual(TEXT("reused artifact RunId publishes the same final directory"),
		ReusedFailure.ArtifactDirectory, ReusedSuccess.ArtifactDirectory);
	FString ReusedImported;
	TestTrue(TEXT("failed reused RunId still publishes imported artifact"), FFileHelper::LoadFileToString(
		ReusedImported, *(ReusedFailure.ArtifactDirectory / TEXT("imported.riglang"))));
	TestTrue(TEXT("failed reused RunId does not retain old imported text"), ReusedImported.IsEmpty());
	FString ReusedSummary;
	TestTrue(TEXT("failed reused RunId publishes summary"), FFileHelper::LoadFileToString(
		ReusedSummary, *(ReusedFailure.ArtifactDirectory / TEXT("summary.json"))));
	TestTrue(TEXT("failed reused RunId summary replaces prior success"),
		ReusedSummary.Contains(TEXT("\"success\":false")));
	FString ReusedDiagnosticsJson;
	TestTrue(TEXT("failed reused RunId publishes structured diagnostics"), FFileHelper::LoadFileToString(
		ReusedDiagnosticsJson, *(ReusedFailure.ArtifactDirectory / TEXT("diagnostics.json"))));
	TestTrue(TEXT("structured diagnostics describe the current failure"),
		ReusedDiagnosticsJson.Contains(TEXT("Source Control Rig is null")));

	const FString ArtifactFailureRunId = TEXT("Automation-Artifact-Write-Failure");
	const FRigLangTransientRoundTripResult ArtifactBaseline =
		RigLangRoundTrip::RunTransient(Source, ArtifactFailureRunId);
	TestTrue(TEXT("artifact failure baseline succeeds"), ArtifactBaseline.bSuccess);
	TMap<FString, FString> BaselineArtifacts;
	for (const FString& Name : {TEXT("source.riglang"), TEXT("imported.riglang"),
		TEXT("expected.normalized.riglang"), TEXT("actual.normalized.riglang"),
		TEXT("diff.json"), TEXT("diagnostics.txt"), TEXT("diagnostics.json"), TEXT("summary.json")})
	{
		FString Content;
		TestTrue(TEXT("artifact failure baseline file loads: ") + Name,
			FFileHelper::LoadFileToString(Content, *(ArtifactBaseline.ArtifactDirectory / Name)));
		BaselineArtifacts.Add(Name, Content);
	}
	const TArray<FString> RequiredArtifactNames = {
		TEXT("source.riglang"), TEXT("imported.riglang"),
		TEXT("expected.normalized.riglang"), TEXT("actual.normalized.riglang"),
		TEXT("diff.json"), TEXT("diagnostics.txt"), TEXT("diagnostics.json"),
		TEXT("summary.json")};
	for (const FString& FailedName : RequiredArtifactNames)
	{
		RigLangRoundTrip::SetArtifactWriteFailureForTest(FailedName);
		const FRigLangTransientRoundTripResult FailedWrite =
			RigLangRoundTrip::RunTransient(Source, ArtifactFailureRunId);
		RigLangRoundTrip::SetArtifactWriteFailureForTest(FString());
		TestFalse(TEXT("injected artifact write failure returns failure: ") + FailedName,
			FailedWrite.bSuccess);
		for (const TPair<FString, FString>& Baseline : BaselineArtifacts)
		{
			FString Current;
			TestTrue(TEXT("existing final artifact still loads after failed write: ") + Baseline.Key,
				FFileHelper::LoadFileToString(Current, *(FailedWrite.ArtifactDirectory / Baseline.Key)));
			TestEqual(TEXT("existing final artifact is unchanged after failed write: ")
				+ FailedName + TEXT("/") + Baseline.Key, Current, Baseline.Value);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangRealAssetTransientRoundTripTest,
	"AnimBP2FP.RigLang.RoundTrip.RealAsset",
	RigLangRoundTripTests::Flags)

bool FRigLangRealAssetTransientRoundTripTest::RunTest(const FString& Parameters)
{
	UControlRigBlueprint* Source = LoadObject<UControlRigBlueprint>(nullptr,
		TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement.CR_Biped_FootPlacement"));
	if (!Source)
	{
		AddInfo(TEXT("SKIPPED: real foot Control Rig fixture is not installed"));
		return true;
	}
	const FRigLangExportResult Export = FRigLangExporter::Export(Source);
	if (!TestTrue(TEXT("real source exports strictly"), Export.bSuccess)
		|| !TestNotNull(TEXT("real source module exists"), Export.Module.Get())) return false;
	const FRigGraphAST* RootGraph = Export.Module->Graphs.FindByPredicate(
		[](const FRigGraphAST& Graph) { return Graph.Role == TEXT("root"); });
	if (TestNotNull(TEXT("real root graph exports"), RootGraph))
	{
		TestEqual(TEXT("root event names export deterministically"),
			RootGraph->Properties.FindRef(TEXT("event-names")),
			FString(TEXT("(\"Construction\" \"Forwards Solve\")")));
	}
	for (const FRigFunctionAST& Function : Export.Module->Functions)
	{
		for (int32 Index = 1; Index < Function.ExternalVariables.Num(); ++Index)
		{
			const FRigExternalVariableAST& Previous = Function.ExternalVariables[Index - 1];
			const FRigExternalVariableAST& Current = Function.ExternalVariables[Index];
			const FString PreviousKey = Previous.Name + TEXT("\x1f") + Previous.Type.CPPType
				+ TEXT("\x1f") + Previous.Type.CPPTypeObject + TEXT("\x1f")
				+ Previous.Type.ContainerType + TEXT("\x1f") + Previous.Guid;
			const FString CurrentKey = Current.Name + TEXT("\x1f") + Current.Type.CPPType
				+ TEXT("\x1f") + Current.Type.CPPTypeObject + TEXT("\x1f")
				+ Current.Type.ContainerType + TEXT("\x1f") + Current.Guid;
			TestTrue(TEXT("function external variables export in canonical order: ") + Function.Name,
				PreviousKey <= CurrentKey);
		}
	}

	for (const FString& Name : {TEXT("clamped_toe_l_null"), TEXT("clamped_foot_l_null")})
	{
		const FRigHierarchyElementAST* Element = Export.Module->Hierarchy.FindByPredicate(
			[&Name](const FRigHierarchyElementAST& Candidate) { return Candidate.Name == Name; });
		if (TestNotNull(TEXT("suspicious hierarchy null is exported: ") + Name, Element))
		{
			TestEqual(TEXT("suspicious hierarchy parent is preserved: ") + Name,
				Element->ParentName, FString(TEXT("toe_r_null")));
			FRigModuleAST Changed = *Export.Module;
			FRigHierarchyElementAST* ChangedElement = Changed.Hierarchy.FindByPredicate(
				[&Name](const FRigHierarchyElementAST& Candidate) { return Candidate.Name == Name; });
			ChangedElement->ParentName = TEXT("normalized_parent_is_forbidden");
			const FString ExpectedPath = TEXT("hierarchy/null:") + Name + TEXT("/parent");
			TestTrue(TEXT("suspicious hierarchy parent participates in diff: ") + Name,
				RigLangRoundTripTests::HasPath(RigLangRoundTripTests::DiffWithValidHashes(*Export.Module, Changed), ExpectedPath));
		}
	}

	TArray<TPair<FString, FString>> FloorBindings;
	for (const FRigGraphAST& Graph : Export.Module->Graphs)
		for (const FRigNodeAST& Node : Graph.Nodes)
			if (Node.Kind == ERigNodeKind::Call
				&& (Node.StableId == TEXT("Init Leg") || Node.StableId == TEXT("Init Leg_1")))
				if (const FRigPinAST* Pin = Node.Pins.FindByPredicate(
					[](const FRigPinAST& Candidate) { return Candidate.Path == TEXT("Smoothed Floor Null"); }))
					FloorBindings.Add({Node.Guid, Pin->DefaultValue});
	TestEqual(TEXT("both Init Leg floor-null bindings export"), FloorBindings.Num(), 2);
	for (const TPair<FString, FString>& Binding : FloorBindings)
		TestTrue(TEXT("suspicious floor-null value is preserved"),
			Binding.Value.Contains(TEXT("smoothed_floor_r_null")));
	FRigModuleAST ChangedFloor = *Export.Module;
	for (FRigGraphAST& Graph : ChangedFloor.Graphs)
		for (FRigNodeAST& Node : Graph.Nodes)
			if (Node.Kind == ERigNodeKind::Call
				&& (Node.StableId == TEXT("Init Leg") || Node.StableId == TEXT("Init Leg_1")))
				if (FRigPinAST* Pin = Node.Pins.FindByPredicate(
					[](const FRigPinAST& Candidate) { return Candidate.Path == TEXT("Smoothed Floor Null"); }))
					Pin->DefaultValue = TEXT("None");
	const FRigLangDiffResult FloorDiff = RigLangRoundTripTests::DiffWithValidHashes(*Export.Module, ChangedFloor);
	TestEqual(TEXT("both real Init Leg floor-null arguments participate in diff"),
		FloorDiff.Differences.FilterByPredicate([](const FRigLangDifference& Difference)
		{
			return Difference.Path.Contains(TEXT("/pin:Smoothed Floor Null/default"));
		}).Num(), 2);

	const FRigLangTransientRoundTripResult Result = RigLangRoundTrip::RunTransient(
		Source, TEXT("CR-Biped-FootPlacement"));
	if (!Result.bSuccess)
	{
		for (const FString& Diagnostic : Result.Diagnostics) AddError(Diagnostic);
		if (!Result.Diff.IsEmpty()) AddError(TEXT("Structured diff: ") + Result.Diff.ToJson());
	}
	TestEqual(TEXT("real model gate"), Result.ModelCount, 39);
	TestEqual(TEXT("real node gate"), Result.NodeCount, 1165);
	TestEqual(TEXT("real link gate"), Result.LinkCount, 1190);
	TestEqual(TEXT("real hierarchy gate"), Result.HierarchyCount, 124);
	TestEqual(TEXT("real compile errors are zero"), Result.CompileErrorCount, 0);
	TestTrue(TEXT("real semantic differences are zero"), Result.Diff.IsEmpty());
	TestTrue(TEXT("real transient round-trip succeeds"), Result.bSuccess);
	return true;
}
#endif
#endif // UE 5.4+
