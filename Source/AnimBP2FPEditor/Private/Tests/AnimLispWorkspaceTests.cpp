// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimLangDiagnostics.h"
#include "AnimLispWorkspace.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AnimLispWorkspaceTests
{
constexpr EAutomationTestFlags TestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

FString RigHeader(const FString& Asset, const FString& Hash)
{
	return FString::Printf(
		TEXT("(rig-module :asset \"%s\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"%s\")\n"),
		*Asset,
		*Hash);
}

FString AnimHeader(const FString& Asset, const FString& Hash)
{
	return FString::Printf(
		TEXT("(anim-module :asset \"%s\" :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash \"%s\")\n"),
		*Asset,
		*Hash);
}

FString FootRigSource(const FString& Visibility = TEXT("internal"))
{
	return RigHeader(TEXT("/Game/Rigs/FootRig"), TEXT("foot-hash"))
		+ TEXT("(rig-hierarchy\n")
		+ TEXT("  (bone :id \"bone-root\" :name \"root\" :parent \"\"))\n")
		+ TEXT("(rig-variables\n")
		+ TEXT("  (variable :id \"var-ground-normal\" :name \"GroundNormal\" :access public-input :cpp-type \"FVector\" :cpp-type-object \"/Script/CoreUObject.Vector\" :container-type \"None\"))\n")
		+ FString::Printf(
			TEXT("(define-rig-function \"UpdateFoot\" :id \"fn-update-foot\" :visibility %s :return-cpp-type \"void\")\n"),
			*Visibility)
		+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry-forwards\" :event \"Forwards Solve\")\n");
}

FString MoverSource(
	const FString& Operations,
	const FString& ImportHash = TEXT("foot-hash"),
	const FString& VariableType = TEXT("FVector"))
{
	return AnimHeader(TEXT("/Game/Animations/Mover"), TEXT("mover-hash"))
		+ FString::Printf(
			TEXT("(import-rig :asset \"/Game/Rigs/FootRig\" :alias FootPlacement :content-hash \"%s\")\n"),
			*ImportHash)
		+ FString::Printf(
			TEXT("(anim-variables (variable :id \"var-ground-normal\" :name \"GroundNormal\" :cpp-type \"%s\" :cpp-type-object \"\" :container-type \"None\"))\n"),
			*VariableType)
		+ Operations;
}

const FAnimLangDiagnostic* FindDiagnostic(
	const FAnimLangDiagnostics& Diagnostics,
	const EAnimLangDiagCategory Category,
	const FString& MessageFragment)
{
	for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items)
	{
		if (Diagnostic.Category == Category && Diagnostic.Message.Contains(MessageFragment))
		{
			return &Diagnostic;
		}
	}
	return nullptr;
}

int32 CountDiagnosticsContaining(const FAnimLangDiagnostics& Diagnostics, const FString& MessageFragment)
{
	int32 Count = 0;
	for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items)
	{
		if (Diagnostic.Message.Contains(MessageFragment))
		{
			++Count;
		}
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceCrossFileResolutionTest,
	"AnimBP2FP.AnimLisp.Workspace.CrossFileResolution",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceCrossFileResolutionTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("FootRig.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
	Workspace.AddSource(
		TEXT("Mover.animlang"),
		AnimLispWorkspaceTests::MoverSource(
			TEXT("(rig-entry :target \"FootPlacement/ForwardsSolve\")\n")
			TEXT("(rig-entry :target \"FootPlacement/ForwardsSolve\")\n")));

	FAnimLangDiagnostics Diagnostics;
	TestTrue(TEXT("Valid workspace builds"), Workspace.Build(Diagnostics));
	TestFalse(TEXT("Valid workspace has no errors"), Diagnostics.HasErrors());

	const FAnimLispDefinition* Definition = Workspace.FindDefinition(
		TEXT("Mover.animlang"),
		TEXT("FootPlacement/ForwardsSolve"));
	if (!TestNotNull(TEXT("Imported rig entry resolves"), Definition))
	{
		return false;
	}
	TestEqual(TEXT("Definition points to Rig source"), Definition->Location.SourceFile, FString(TEXT("FootRig.riglang")));
	TestEqual(TEXT("Definition is a rig entry"), Definition->Id.Kind, EAnimLispSymbolKind::RigEntry);

	const TArray<FAnimLispReference> References = Workspace.FindReferences(Definition->Id);
	TestEqual(TEXT("Both entry uses are indexed"), References.Num(), 2);
	if (References.Num() == 2)
	{
		TestEqual(TEXT("References are deterministically source ordered"), References[0].Location.SourceFile, FString(TEXT("Mover.animlang")));
		TestTrue(TEXT("Reference offsets are ascending"), References[0].Location.Offset < References[1].Location.Offset);
	}

	const TArray<FAnimLispCompletion> First = Workspace.Complete(
		TEXT("Mover.animlang"),
		EAnimLispCapability::AnimRuntimeReference);
	const TArray<FAnimLispCompletion> Second = Workspace.Complete(
		TEXT("Mover.animlang"),
		EAnimLispCapability::AnimRuntimeReference);
	TestEqual(TEXT("Completion count is stable"), First.Num(), Second.Num());
	for (int32 Index = 0; Index < FMath::Min(First.Num(), Second.Num()); ++Index)
	{
		TestTrue(TEXT("Completion order is deterministic"), First[Index].Id == Second[Index].Id);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceCapabilityTest,
	"AnimBP2FP.AnimLisp.Workspace.Capability",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceCapabilityTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("FootRig.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("internal")));
	Workspace.AddSource(
		TEXT("Mover.animlang"),
		AnimLispWorkspaceTests::MoverSource(TEXT("(rig-call :target \"FootPlacement/UpdateFoot\")\n")));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Internal rig call makes build fail"), Workspace.Build(Diagnostics));
	const FAnimLangDiagnostic* Diagnostic = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Capability,
		TEXT("UpdateFoot"));
	if (!TestNotNull(TEXT("Capability diagnostic is emitted"), Diagnostic))
	{
		return false;
	}
	TestEqual(TEXT("Capability error points at Anim use"), Diagnostic->Location.SourceFile, FString(TEXT("Mover.animlang")));
	TestEqual(TEXT("Capability error has one definition relation"), Diagnostic->RelatedLocations.Num(), 1);
	if (!Diagnostic->RelatedLocations.IsEmpty())
	{
		TestEqual(TEXT("Related location points at Rig definition"), Diagnostic->RelatedLocations[0].SourceFile, FString(TEXT("FootRig.riglang")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceTypeMismatchTest,
	"AnimBP2FP.AnimLisp.Workspace.TypeMismatch",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceTypeMismatchTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("FootRig.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
	Workspace.AddSource(
		TEXT("Mover.animlang"),
		AnimLispWorkspaceTests::MoverSource(
			TEXT("(bind-rig-variable :anim-variable \"GroundNormal\" :rig-variable \"FootPlacement/GroundNormal\")\n"),
			TEXT("foot-hash"),
			TEXT("bool")));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Exact Unreal type mismatch makes build fail"), Workspace.Build(Diagnostics));
	const FAnimLangDiagnostic* Diagnostic = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Type,
		TEXT("GroundNormal"));
	if (!TestNotNull(TEXT("Type diagnostic is emitted"), Diagnostic))
	{
		return false;
	}
	TestEqual(TEXT("Binding is primary location"), Diagnostic->Location.SourceFile, FString(TEXT("Mover.animlang")));
	TestEqual(TEXT("Type diagnostic identifies both declarations"), Diagnostic->RelatedLocations.Num(), 2);
	if (Diagnostic->RelatedLocations.Num() == 2)
	{
		TestEqual(TEXT("First relation is Anim declaration"), Diagnostic->RelatedLocations[0].SourceFile, FString(TEXT("Mover.animlang")));
		TestEqual(TEXT("Second relation is Rig declaration"), Diagnostic->RelatedLocations[1].SourceFile, FString(TEXT("FootRig.riglang")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceModuleDirectionAndCycleTest,
	"AnimBP2FP.AnimLisp.Workspace.ModuleDirectionAndCycle",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceModuleDirectionAndCycleTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("IllegalDirection.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/IllegalDirection"), TEXT("direction-hash"))
		+ TEXT("(import-anim :asset \"/Game/Animations/Mover\" :alias Mover :content-hash \"mover-hash\")\n"));
	Workspace.AddSource(
		TEXT("A.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/A"), TEXT("a-hash"))
		+ TEXT("(import-rig :asset \"/Game/Rigs/B\" :alias B :content-hash \"b-hash\")\n"));
	Workspace.AddSource(
		TEXT("B.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/B"), TEXT("b-hash"))
		+ TEXT("(import-rig :asset \"/Game/Rigs/C\" :alias C :content-hash \"c-hash\")\n"));
	Workspace.AddSource(
		TEXT("C.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/C"), TEXT("c-hash"))
		+ TEXT("(import-rig :asset \"/Game/Rigs/A\" :alias A :content-hash \"a-hash\")\n"));
	Workspace.AddSource(TEXT("Mover.animlang"), AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/Mover"), TEXT("mover-hash")));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Invalid module graph makes build fail"), Workspace.Build(Diagnostics));
	TestNotNull(
		TEXT("Rig to Anim direction is rejected"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Module, TEXT("Rig module cannot import Anim")));
	const FAnimLangDiagnostic* Cycle = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("cycle"));
	if (!TestNotNull(TEXT("Rig import cycle is diagnosed"), Cycle))
	{
		return false;
	}
	TestTrue(TEXT("Cycle lists A"), Cycle->Message.Contains(TEXT("/Game/Rigs/A")));
	TestTrue(TEXT("Cycle lists B"), Cycle->Message.Contains(TEXT("/Game/Rigs/B")));
	TestTrue(TEXT("Cycle lists C"), Cycle->Message.Contains(TEXT("/Game/Rigs/C")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceModuleIdentityAndHashTest,
	"AnimBP2FP.AnimLisp.Workspace.ModuleIdentityAndHash",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceModuleIdentityAndHashTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("FootRigA.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
	Workspace.AddSource(TEXT("FootRigB.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
	Workspace.AddSource(
		TEXT("Mover.animlang"),
		AnimLispWorkspaceTests::MoverSource(
			TEXT("(rig-entry :target \"FootPlacement/MissingEntry\")\n"),
			TEXT("stale-foot-hash")));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Module identity and reference failures make build fail"), Workspace.Build(Diagnostics));
	const FAnimLangDiagnostic* Duplicate = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("Duplicate module identity"));
	if (TestNotNull(TEXT("Duplicate module identity is diagnosed"), Duplicate))
	{
		TestFalse(TEXT("Duplicate module points at both files"), Duplicate->RelatedLocations.IsEmpty());
	}
	TestNotNull(
		TEXT("Importer is blocked by duplicate dependency"),
		AnimLispWorkspaceTests::FindDiagnostic(
			Diagnostics,
			EAnimLangDiagCategory::Module,
			TEXT("blocked by duplicate module identity")));
	TestNull(
		TEXT("Duplicate dependency suppresses stale expected hash"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Module, TEXT("stale-foot-hash")));
	TestNull(
		TEXT("Duplicate dependency suppresses unresolved use"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("MissingEntry")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceBlockedCascadeTest,
	"AnimBP2FP.AnimLisp.Workspace.BlockedCascade",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceBlockedCascadeTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("BrokenRig.riglang"),
		TEXT("(rig-module :asset \"/Game/Rigs/BrokenRig\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"broken-hash\""));
	Workspace.AddSource(
		TEXT("Mover.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/Mover"), TEXT("mover-hash"))
		+ TEXT("(import-rig :asset \"/Game/Rigs/BrokenRig\" :alias Broken :content-hash \"broken-hash\")\n")
		+ TEXT("(rig-entry :target \"Broken/FirstMissing\")\n")
		+ TEXT("(rig-entry :target \"Broken/SecondMissing\")\n"));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Broken dependency makes build fail"), Workspace.Build(Diagnostics));
	TestEqual(TEXT("One blocked diagnostic per dependent module"), AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("blocked by module error")), 1);
	const FAnimLangDiagnostic* Blocked = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("blocked by module error"));
	if (TestNotNull(TEXT("Dependent module is blocked"), Blocked))
	{
		TestEqual(TEXT("Blocked diagnostic points at dependency parse error"), Blocked->RelatedLocations.Num(), 1);
		if (!Blocked->RelatedLocations.IsEmpty())
		{
			TestEqual(TEXT("Related parse location is in broken module"), Blocked->RelatedLocations[0].SourceFile, FString(TEXT("BrokenRig.riglang")));
		}
	}
	TestEqual(TEXT("Blocked uses do not cascade undefined diagnostics"), AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("undefined")), 0);
	TestEqual(TEXT("Blocked uses do not cascade unresolved diagnostics"), AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("unresolved")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceHierarchyLintTest,
	"AnimBP2FP.AnimLisp.Workspace.HierarchyLint",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceHierarchyLintTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("Hierarchy.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/Hierarchy"), TEXT("hierarchy-hash"))
		+ TEXT("(rig-hierarchy\n")
		+ TEXT("  (bone :id \"missing-child\" :name \"MissingChild\" :parent \"NoSuchParent\")\n")
		+ TEXT("  (bone :id \"cycle-a\" :name \"CycleA\" :parent \"CycleB\")\n")
		+ TEXT("  (bone :id \"cycle-b\" :name \"CycleB\" :parent \"CycleA\"))\n"));
	Workspace.AddSource(
		TEXT("DuplicateHierarchy.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/DuplicateHierarchy"), TEXT("duplicate-hierarchy-hash"))
		+ TEXT("(rig-hierarchy\n")
		+ TEXT("  (bone :id \"duplicate-id\" :name \"First\" :parent \"\")\n")
		+ TEXT("  (control :id \"duplicate-id\" :name \"Second\" :parent \"\"))\n"));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Invalid hierarchy makes build fail"), Workspace.Build(Diagnostics));
	TestNotNull(
		TEXT("Missing parent is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("NoSuchParent")));
	const FAnimLangDiagnostic* Cycle = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Semantic,
		TEXT("hierarchy cycle"));
	if (TestNotNull(TEXT("Hierarchy cycle is diagnosed"), Cycle))
	{
		TestTrue(TEXT("Hierarchy cycle lists CycleA"), Cycle->Message.Contains(TEXT("CycleA")));
		TestTrue(TEXT("Hierarchy cycle lists CycleB"), Cycle->Message.Contains(TEXT("CycleB")));
	}
	TestNotNull(
		TEXT("Duplicate hierarchy element key is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("duplicate-id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceGraphLintTest,
	"AnimBP2FP.AnimLisp.Workspace.GraphLint",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceGraphLintTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("Graph.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/Graph"), TEXT("graph-hash"))
		+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry-forward\" :event \"Forwards Solve\"\n")
		+ TEXT("  (rig-unit :id \"source\" :guid \"11111111-1111-1111-1111-111111111111\" :class \"/Script/Test.Source\" :coverage exact\n")
		+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"FVector\" :cpp-type-object \"/Script/CoreUObject.Vector\" :container-type \"None\"))\n")
		+ TEXT("  (rig-unit :id \"lossy\" :guid \"22222222-2222-2222-2222-222222222222\" :class \"/Script/Test.Lossy\" :coverage lossy\n")
		+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\" :cpp-type-object \"\" :container-type \"None\")\n")
		+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"bool\" :cpp-type-object \"\" :container-type \"None\"))\n")
		+ TEXT("  (rig-unit :id \"unsupported\" :guid \"33333333-3333-3333-3333-333333333333\" :class \"/Script/Test.Unsupported\" :coverage unsupported\n")
		+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\" :cpp-type-object \"\" :container-type \"None\"))\n")
		+ TEXT("  (rig-link :from \"source.Out\" :to \"lossy.In\")\n")
		+ TEXT("  (rig-link :from \"lossy.Out\" :to \"unsupported.In\")\n")
		+ TEXT("  (rig-link :from \"missing.Out\" :to \"unsupported.In\"))\n"));
	Workspace.AddSource(
		TEXT("DuplicateGraph.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/DuplicateGraph"), TEXT("duplicate-graph-hash"))
		+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry-forward\" :event \"Forwards Solve\"\n")
		+ TEXT("  (rig-unit :id \"duplicate-node\" :guid \"44444444-4444-4444-4444-444444444444\" :class \"/Script/Test.First\"\n")
		+ TEXT("    (pin :path \"Value\" :direction input :cpp-type \"float\")\n")
		+ TEXT("    (pin :path \"Value\" :direction output :cpp-type \"float\"))\n")
		+ TEXT("  (rig-unit :id \"duplicate-node\" :guid \"44444444-4444-4444-4444-444444444444\" :class \"/Script/Test.Second\"))\n"));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Invalid graphs make build fail"), Workspace.Build(Diagnostics));
	TestNotNull(
		TEXT("Missing link endpoint is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("missing.Out")));
	const FAnimLangDiagnostic* TypeMismatch = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Type,
		TEXT("source.Out"));
	if (TestNotNull(TEXT("Connected incompatible pin types are diagnosed"), TypeMismatch))
	{
		TestFalse(TEXT("Pin mismatch includes the other endpoint"), TypeMismatch->RelatedLocations.IsEmpty());
	}
	TestNotNull(
		TEXT("Reachable lossy node is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("lossy")));
	TestNotNull(
		TEXT("Reachable unsupported node is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("unsupported")));
	TestNotNull(
		TEXT("Duplicate node ID is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("duplicate-node")));
	TestNotNull(
		TEXT("Duplicate pin ID is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(Diagnostics, EAnimLangDiagCategory::Semantic, TEXT("Value")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceParserIntegrityTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.ParserIntegrity",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceParserIntegrityTest::RunTest(const FString& Parameters)
{
	auto ExpectRejected = [this](
		const FString& Label,
		const FString& SourceFile,
		const FString& Source,
		const FString& PollutedSymbol = FString())
	{
		FAnimLispWorkspace Workspace;
		Workspace.AddSource(SourceFile, Source);
		FAnimLangDiagnostics Diagnostics;
		const bool bBuilt = Workspace.Build(Diagnostics);
		TestFalse(*(Label + TEXT(" build fails")), bBuilt);
		TestTrue(
			*(Label + TEXT(" emits parse or module diagnostic")),
			!Diagnostics.GetByCategory(EAnimLangDiagCategory::Parse).IsEmpty()
				|| !Diagnostics.GetByCategory(EAnimLangDiagCategory::Module).IsEmpty());
		if (!PollutedSymbol.IsEmpty())
		{
			TestNull(
				*(Label + TEXT(" does not publish definitions")),
				Workspace.FindDefinition(SourceFile, PollutedSymbol));
			const TArray<FAnimLispCompletion> Completions = Workspace.Complete(
				SourceFile,
				EAnimLispCapability::AnimRuntimeReference);
			TestEqual(*(Label + TEXT(" does not publish completions")), Completions.Num(), 0);
			TestEqual(
				*(Label + TEXT(" does not publish imports")),
				AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("Unresolved imported module")),
				0);
		}
	};

	const FString BrokenAnimPrefix = AnimLispWorkspaceTests::AnimHeader(
		TEXT("/Game/Animations/Broken"),
		TEXT("broken-hash"))
		+ TEXT("(import-rig :asset \"/Game/Rigs/Missing\" :alias Missing :content-hash \"missing-hash\")\n")
		+ TEXT("(anim-variables (variable :id \"local\" :name \"Local\" :cpp-type \"bool\"))\n");
	ExpectRejected(
		TEXT("Tokenizer error"),
		TEXT("TokenError.animlang"),
		BrokenAnimPrefix + TEXT("@\n"),
		TEXT("Local"));
	ExpectRejected(
		TEXT("Unbalanced Anim form"),
		TEXT("Unbalanced.animlang"),
		BrokenAnimPrefix + TEXT("(rig-entry :target \"Missing/Entry\"\n"),
		TEXT("Local"));
	ExpectRejected(
		TEXT("Missing Anim header"),
		TEXT("MissingHeader.animlang"),
		TEXT("(anim-variables (variable :id \"local\" :name \"Local\" :cpp-type \"bool\"))\n"));
	FAnimLispWorkspace MissingHeaderLexerWorkspace;
	MissingHeaderLexerWorkspace.AddSource(
		TEXT("MissingHeaderLexer.animlang"),
		TEXT("(anim-variables (variable :id \"local\" :name \"Local\" :cpp-type \"bool\"))\n@\n"));
	FAnimLangDiagnostics MissingHeaderLexerDiagnostics;
	TestFalse(
		TEXT("Missing header with lexer error fails build"),
		MissingHeaderLexerWorkspace.Build(MissingHeaderLexerDiagnostics));
	TestFalse(
		TEXT("Missing header retains module diagnostic"),
		MissingHeaderLexerDiagnostics.GetByCategory(EAnimLangDiagCategory::Module).IsEmpty());
	TestFalse(
		TEXT("Missing header retains lexer parse diagnostic"),
		MissingHeaderLexerDiagnostics.GetByCategory(EAnimLangDiagCategory::Parse).IsEmpty());
	ExpectRejected(
		TEXT("Duplicate Anim header"),
		TEXT("DuplicateHeader.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/First"), TEXT("first"))
			+ AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/Second"), TEXT("second")));
	ExpectRejected(
		TEXT("Rig header pre-scan failure"),
		TEXT("MissingRigAsset.riglang"),
		TEXT("(rig-module :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"bad\")\n"));
	ExpectRejected(
		TEXT("Malformed Rig form"),
		TEXT("MalformedRig.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/Malformed"), TEXT("malformed"))
			+ TEXT("(rig-hierarchy (bone :id \"root\" :name \"root\" :parent \"\")\n"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceMissingImportTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.MissingImport",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceMissingImportTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace MissingWorkspace;
	MissingWorkspace.AddSource(
		TEXT("MissingImport.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/MissingImport"), TEXT("anim-hash"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/Absent\" :alias Absent :content-hash \"absent-hash\")\n"));
	FAnimLangDiagnostics MissingDiagnostics;
	TestFalse(TEXT("Unused missing import fails build"), MissingWorkspace.Build(MissingDiagnostics));
	const FAnimLangDiagnostic* Missing = AnimLispWorkspaceTests::FindDiagnostic(
		MissingDiagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("/Game/Rigs/Absent"));
	if (TestNotNull(TEXT("Missing module is diagnosed"), Missing))
	{
		TestEqual(TEXT("Missing module points at import"), Missing->Location.SourceFile, FString(TEXT("MissingImport.animlang")));
	}

	FAnimLispWorkspace BlockedWorkspace;
	BlockedWorkspace.AddSource(
		TEXT("Broken.riglang"),
		TEXT("(rig-module :asset \"/Game/Rigs/Broken\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"broken\""));
	BlockedWorkspace.AddSource(
		TEXT("Dependent.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/Dependent"), TEXT("dependent"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :alias Broken :content-hash \"broken\")\n")
			+ TEXT("(rig-entry :target \"Broken/Missing\")\n"));
	FAnimLangDiagnostics BlockedDiagnostics;
	TestFalse(TEXT("Parse-failed import blocks build"), BlockedWorkspace.Build(BlockedDiagnostics));
	TestEqual(
		TEXT("Parse-failed import emits one blocked diagnostic"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(BlockedDiagnostics, TEXT("blocked by module error")),
		1);
	TestEqual(
		TEXT("Parse-failed import is not also missing"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(BlockedDiagnostics, TEXT("Unresolved imported module")),
		0);
	TestEqual(
		TEXT("Parse-failed import suppresses use cascade"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(BlockedDiagnostics, TEXT("Unresolved symbol")),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceUnresolvedBindingTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.UnresolvedBinding",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceUnresolvedBindingTest::RunTest(const FString& Parameters)
{
	auto ExpectOneBindingError = [this](
		const FString& Label,
		const FString& AnimSource,
		const FString& MissingName)
	{
		FAnimLispWorkspace Workspace;
		Workspace.AddSource(TEXT("FootRig.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
		Workspace.AddSource(TEXT("Mover.animlang"), AnimSource);
		FAnimLangDiagnostics Diagnostics;
		TestFalse(*(Label + TEXT(" fails build")), Workspace.Build(Diagnostics));
		const FAnimLangDiagnostic* Diagnostic = AnimLispWorkspaceTests::FindDiagnostic(
			Diagnostics,
			EAnimLangDiagCategory::Semantic,
			MissingName);
		if (TestNotNull(*(Label + TEXT(" emits unresolved binding")), Diagnostic))
		{
			TestEqual(*(Label + TEXT(" points at binding")), Diagnostic->Location.SourceFile, FString(TEXT("Mover.animlang")));
		}
		TestEqual(
			*(Label + TEXT(" emits no duplicate binding noise")),
			AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, MissingName),
			1);
	};

	ExpectOneBindingError(
		TEXT("Wrong alias"),
		AnimLispWorkspaceTests::MoverSource(
			TEXT("(bind-rig-variable :anim-variable \"GroundNormal\" :rig-variable \"Wrong/GroundNormal\")\n")),
		TEXT("Wrong/GroundNormal"));
	ExpectOneBindingError(
		TEXT("Missing Anim variable"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/Mover"), TEXT("mover-hash"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/FootRig\" :alias FootPlacement :content-hash \"foot-hash\")\n")
			+ TEXT("(bind-rig-variable :anim-variable \"MissingAnim\" :rig-variable \"FootPlacement/GroundNormal\")\n"),
		TEXT("MissingAnim"));
	ExpectOneBindingError(
		TEXT("Missing Rig variable"),
		AnimLispWorkspaceTests::MoverSource(
			TEXT("(bind-rig-variable :anim-variable \"GroundNormal\" :rig-variable \"FootPlacement/MissingRig\")\n")),
		TEXT("FootPlacement/MissingRig"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceDuplicateSymbolTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.DuplicateSymbol",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceDuplicateSymbolTest::RunTest(const FString& Parameters)
{
	auto ExpectQuarantinedDuplicate = [this](
		const FString& Label,
		const FString& SourceFile,
		const FString& Source,
		const FString& Name)
	{
		FAnimLispWorkspace Workspace;
		Workspace.AddSource(SourceFile, Source);
		FAnimLangDiagnostics Diagnostics;
		TestFalse(*(Label + TEXT(" fails build")), Workspace.Build(Diagnostics));
		const FAnimLangDiagnostic* Duplicate = AnimLispWorkspaceTests::FindDiagnostic(
			Diagnostics,
			EAnimLangDiagCategory::Semantic,
			TEXT("Duplicate symbol"));
		if (TestNotNull(*(Label + TEXT(" is diagnosed")), Duplicate))
		{
			TestEqual(*(Label + TEXT(" relates first declaration")), Duplicate->RelatedLocations.Num(), 1);
		}
		TestNull(*(Label + TEXT(" is removed from lookup")), Workspace.FindDefinition(SourceFile, Name));
		int32 CompletionCount = 0;
		for (const FAnimLispCompletion& Completion : Workspace.Complete(
			SourceFile,
			EAnimLispCapability::AnimRuntimeReference))
		{
			if (Completion.Id.QualifiedName == Name) ++CompletionCount;
		}
		TestEqual(*(Label + TEXT(" is removed from completion")), CompletionCount, 0);
	};

	ExpectQuarantinedDuplicate(
		TEXT("Duplicate Anim variable"),
		TEXT("DuplicateVariable.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/DuplicateVariable"), TEXT("duplicate"))
			+ TEXT("(anim-variables\n")
			+ TEXT("  (variable :id \"first\" :name \"Shared\" :cpp-type \"bool\")\n")
			+ TEXT("  (variable :id \"second\" :name \"Shared\" :cpp-type \"bool\"))\n"),
		TEXT("Shared"));
	ExpectQuarantinedDuplicate(
		TEXT("Cross-kind duplicate"),
		TEXT("CrossKind.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/CrossKind"), TEXT("cross-kind"))
			+ TEXT("(anim-variables (variable :id \"variable\" :name \"Shared\" :cpp-type \"bool\"))\n")
			+ TEXT("(define \"Shared\" :id \"define\")\n"),
		TEXT("Shared"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceAnimSchemaValidationTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.AnimSchemaValidation",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceAnimSchemaValidationTest::RunTest(const FString& Parameters)
{
	auto ExpectSchemaRejected = [this](
		const FString& Label,
		const FString& Source,
		const FString& Key,
		const int32 FormLine)
	{
		const FString SourceFile = Label.Replace(TEXT(" "), TEXT("")) + TEXT(".animlang");
		FAnimLispWorkspace Workspace;
		Workspace.AddSource(SourceFile, Source);
		FAnimLangDiagnostics Diagnostics;
		TestFalse(*(Label + TEXT(" fails build")), Workspace.Build(Diagnostics));
		const FAnimLangDiagnostic* SchemaDiagnostic = nullptr;
		for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items)
		{
			if ((Diagnostic.Category == EAnimLangDiagCategory::Parse
					|| Diagnostic.Category == EAnimLangDiagCategory::Module)
				&& Diagnostic.Message.Contains(Key)
				&& Diagnostic.Location.SourceFile == SourceFile
				&& Diagnostic.Location.Line == FormLine)
			{
				SchemaDiagnostic = &Diagnostic;
				break;
			}
		}
		TestNotNull(*(Label + TEXT(" diagnoses required key at form")), SchemaDiagnostic);
		TestNull(
			*(Label + TEXT(" does not publish sentinel definition")),
			Workspace.FindDefinition(SourceFile, TEXT("Sentinel")));
		TestEqual(
			*(Label + TEXT(" does not publish completions")),
			Workspace.Complete(SourceFile, EAnimLispCapability::AnimRuntimeReference).Num(),
			0);
		TestEqual(
			*(Label + TEXT(" does not publish imports")),
			AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("Unresolved imported module")),
			0);
	};

	const FString ValidHeader = AnimLispWorkspaceTests::AnimHeader(
		TEXT("/Game/Animations/Schema"),
		TEXT("schema-hash"));
	const FString LeakTail =
		TEXT("(import-rig :asset \"/Game/Rigs/Leak\" :alias Leak :content-hash \"leak-hash\")\n")
		TEXT("(anim-variables (variable :id \"sentinel\" :name \"Sentinel\" :cpp-type \"bool\"))\n");
	ExpectSchemaRejected(
		TEXT("Header missing class"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :version 1 :content-hash \"schema-hash\")\n") + LeakTail,
		TEXT("class"),
		1);
	ExpectSchemaRejected(
		TEXT("Header missing version"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :class \"/Script/Engine.AnimBlueprint\" :content-hash \"schema-hash\")\n") + LeakTail,
		TEXT("version"),
		1);
	ExpectSchemaRejected(
		TEXT("Header missing content hash"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :class \"/Script/Engine.AnimBlueprint\" :version 1)\n") + LeakTail,
		TEXT("content-hash"),
		1);
	ExpectSchemaRejected(
		TEXT("Import missing asset"),
		ValidHeader + TEXT("(import-rig :alias Broken :content-hash \"broken\")\n") + LeakTail,
		TEXT("asset"),
		2);
	ExpectSchemaRejected(
		TEXT("Import missing alias"),
		ValidHeader + TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :content-hash \"broken\")\n") + LeakTail,
		TEXT("alias"),
		2);
	ExpectSchemaRejected(
		TEXT("Import missing content hash"),
		ValidHeader + TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :alias Broken)\n") + LeakTail,
		TEXT("content-hash"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable missing id"),
		ValidHeader + TEXT("(anim-variables (variable :name \"Broken\" :cpp-type \"bool\"))\n") + LeakTail,
		TEXT("id"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable missing name"),
		ValidHeader + TEXT("(anim-variables (variable :id \"broken\" :cpp-type \"bool\"))\n") + LeakTail,
		TEXT("name"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable missing cpp type"),
		ValidHeader + TEXT("(anim-variables (variable :id \"broken\" :name \"Broken\"))\n") + LeakTail,
		TEXT("cpp-type"),
		2);
	ExpectSchemaRejected(
		TEXT("Rig entry missing target"),
		ValidHeader + TEXT("(rig-entry :id \"broken\")\n") + LeakTail,
		TEXT("target"),
		2);
	ExpectSchemaRejected(
		TEXT("Rig call missing target"),
		ValidHeader + TEXT("(rig-call :id \"broken\")\n") + LeakTail,
		TEXT("target"),
		2);
	ExpectSchemaRejected(
		TEXT("Binding missing anim variable"),
		ValidHeader + TEXT("(bind-rig-variable :rig-variable \"Leak/Value\")\n") + LeakTail,
		TEXT("anim-variable"),
		2);
	ExpectSchemaRejected(
		TEXT("Binding missing rig variable"),
		ValidHeader + TEXT("(bind-rig-variable :anim-variable \"Sentinel\")\n") + LeakTail,
		TEXT("rig-variable"),
		2);
	ExpectSchemaRejected(
		TEXT("Header duplicate asset"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :asset \"/Game/Animations/Other\" :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash \"schema-hash\")\n") + LeakTail,
		TEXT("asset"),
		1);
	ExpectSchemaRejected(
		TEXT("Import duplicate alias"),
		ValidHeader + TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :alias Broken :alias Other :content-hash \"broken\")\n") + LeakTail,
		TEXT("alias"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable duplicate name"),
		ValidHeader + TEXT("(anim-variables (variable :id \"broken\" :name \"Broken\" :name \"Other\" :cpp-type \"bool\"))\n") + LeakTail,
		TEXT("name"),
		2);
	ExpectSchemaRejected(
		TEXT("Rig entry duplicate target"),
		ValidHeader + TEXT("(rig-entry :target \"Leak/First\" :target \"Leak/Second\")\n") + LeakTail,
		TEXT("target"),
		2);
	ExpectSchemaRejected(
		TEXT("Binding duplicate rig variable"),
		ValidHeader + TEXT("(bind-rig-variable :anim-variable \"Sentinel\" :rig-variable \"Leak/First\" :rig-variable \"Leak/Second\")\n") + LeakTail,
		TEXT("rig-variable"),
		2);
	ExpectSchemaRejected(
		TEXT("Header asset without value"),
		TEXT("(anim-module :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash \"schema-hash\" :asset)\n") + LeakTail,
		TEXT("asset"),
		1);
	ExpectSchemaRejected(
		TEXT("Header asset wrong type"),
		TEXT("(anim-module :asset 123 :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash \"schema-hash\")\n") + LeakTail,
		TEXT("asset"),
		1);
	ExpectSchemaRejected(
		TEXT("Header class wrong type"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :class 123 :version 1 :content-hash \"schema-hash\")\n") + LeakTail,
		TEXT("class"),
		1);
	ExpectSchemaRejected(
		TEXT("Header version wrong type"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :class \"/Script/Engine.AnimBlueprint\" :version \"one\" :content-hash \"schema-hash\")\n") + LeakTail,
		TEXT("version"),
		1);
	ExpectSchemaRejected(
		TEXT("Header content hash wrong type"),
		TEXT("(anim-module :asset \"/Game/Animations/Schema\" :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash 1)\n") + LeakTail,
		TEXT("content-hash"),
		1);
	ExpectSchemaRejected(
		TEXT("Import asset without value"),
		ValidHeader + TEXT("(import-rig :alias Broken :content-hash \"broken\" :asset)\n") + LeakTail,
		TEXT("asset"),
		2);
	ExpectSchemaRejected(
		TEXT("Import asset wrong type"),
		ValidHeader + TEXT("(import-rig :asset 123 :alias Broken :content-hash \"broken\")\n") + LeakTail,
		TEXT("asset"),
		2);
	ExpectSchemaRejected(
		TEXT("Import alias without value"),
		ValidHeader + TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :content-hash \"broken\" :alias)\n") + LeakTail,
		TEXT("alias"),
		2);
	ExpectSchemaRejected(
		TEXT("Import alias wrong type"),
		ValidHeader + TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :alias 123 :content-hash \"broken\")\n") + LeakTail,
		TEXT("alias"),
		2);
	ExpectSchemaRejected(
		TEXT("Import content hash wrong type"),
		ValidHeader + TEXT("(import-rig :asset \"/Game/Rigs/Broken\" :alias Broken :content-hash 123)\n") + LeakTail,
		TEXT("content-hash"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable id wrong type"),
		ValidHeader + TEXT("(anim-variables (variable :id 123 :name \"Broken\" :cpp-type \"bool\"))\n") + LeakTail,
		TEXT("id"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable name wrong type"),
		ValidHeader + TEXT("(anim-variables (variable :id \"broken\" :name 123 :cpp-type \"bool\"))\n") + LeakTail,
		TEXT("name"),
		2);
	ExpectSchemaRejected(
		TEXT("Variable cpp type wrong type"),
		ValidHeader + TEXT("(anim-variables (variable :id \"broken\" :name \"Broken\" :cpp-type 123))\n") + LeakTail,
		TEXT("cpp-type"),
		2);
	ExpectSchemaRejected(
		TEXT("Rig entry target wrong type"),
		ValidHeader + TEXT("(rig-entry :target 123)\n") + LeakTail,
		TEXT("target"),
		2);
	ExpectSchemaRejected(
		TEXT("Rig call target wrong type"),
		ValidHeader + TEXT("(rig-call :target 123)\n") + LeakTail,
		TEXT("target"),
		2);
	ExpectSchemaRejected(
		TEXT("Binding anim variable wrong type"),
		ValidHeader + TEXT("(bind-rig-variable :anim-variable 123 :rig-variable \"Leak/Value\")\n") + LeakTail,
		TEXT("anim-variable"),
		2);
	ExpectSchemaRejected(
		TEXT("Binding rig variable wrong type"),
		ValidHeader + TEXT("(bind-rig-variable :anim-variable \"Sentinel\" :rig-variable 123)\n") + LeakTail,
		TEXT("rig-variable"),
		2);
	ExpectSchemaRejected(
		TEXT("Unknown variable child"),
		ValidHeader + TEXT("(anim-variables (not-a-variable :id \"broken\"))\n") + LeakTail,
		TEXT("child"),
		2);
	ExpectSchemaRejected(
		TEXT("Empty variable child"),
		ValidHeader + TEXT("(anim-variables ())\n") + LeakTail,
		TEXT("child"),
		2);

	FAnimLispWorkspace StringAliasWorkspace;
	StringAliasWorkspace.AddSource(TEXT("StringAliasRig.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
	StringAliasWorkspace.AddSource(
		TEXT("StringAlias.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/StringAlias"), TEXT("string-alias"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/FootRig\" :alias \"FootPlacement\" :content-hash \"foot-hash\")\n")
			+ TEXT("(rig-entry :target \"FootPlacement/ForwardsSolve\")\n"));
	FAnimLangDiagnostics StringAliasDiagnostics;
	TestTrue(TEXT("String import alias remains valid"), StringAliasWorkspace.Build(StringAliasDiagnostics));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceFailedDuplicateModuleTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.FailedDuplicateModule",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceFailedDuplicateModuleTest::RunTest(const FString& Parameters)
{
	auto FindDiagnosticInFile = [](const FAnimLangDiagnostics& Diagnostics,
		const EAnimLangDiagCategory Category,
		const FString& Message,
		const FString& SourceFile) -> const FAnimLangDiagnostic*
	{
		for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items)
		{
			if (Diagnostic.Category == Category
				&& Diagnostic.Message.Contains(Message)
				&& Diagnostic.Location.SourceFile == SourceFile)
			{
				return &Diagnostic;
			}
		}
		return nullptr;
	};
	auto InvalidSource = [](const FString& Asset, const FString& Variable)
	{
		return AnimLispWorkspaceTests::AnimHeader(Asset, TEXT("failed"))
			+ FString::Printf(
				TEXT("(anim-variables (variable :id \"failed\" :name \"%s\" :cpp-type \"bool\"))\n"),
				*Variable)
			+ TEXT("(unsupported-form)\n");
	};

	FAnimLispWorkspace FailedPair;
	FailedPair.AddSource(TEXT("AFirstFailed.animlang"), InvalidSource(TEXT("/Game/Animations/DuplicateFailed"), TEXT("FirstLeak")));
	FailedPair.AddSource(TEXT("BSecondFailed.animlang"), InvalidSource(TEXT("/Game/Animations/DuplicateFailed"), TEXT("SecondLeak")));
	FAnimLangDiagnostics FailedPairDiagnostics;
	TestFalse(TEXT("Two failed duplicate modules fail build"), FailedPair.Build(FailedPairDiagnostics));
	const FAnimLangDiagnostic* FailedPairDuplicate = AnimLispWorkspaceTests::FindDiagnostic(
		FailedPairDiagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("Duplicate module identity"));
	if (TestNotNull(TEXT("Two failed modules diagnose duplicate identity"), FailedPairDuplicate))
	{
		TestEqual(TEXT("Failed duplicate relates the other header"), FailedPairDuplicate->RelatedLocations.Num(), 1);
	}
	TestNotNull(
		TEXT("First failed module keeps parse diagnostic"),
		FindDiagnosticInFile(FailedPairDiagnostics, EAnimLangDiagCategory::Parse, TEXT("Unsupported"), TEXT("AFirstFailed.animlang")));
	TestNotNull(
		TEXT("Second failed module keeps parse diagnostic"),
		FindDiagnosticInFile(FailedPairDiagnostics, EAnimLangDiagCategory::Parse, TEXT("Unsupported"), TEXT("BSecondFailed.animlang")));
	TestNull(TEXT("First failed module does not publish definitions"), FailedPair.FindDefinition(TEXT("AFirstFailed.animlang"), TEXT("FirstLeak")));
	TestNull(TEXT("Second failed module does not publish definitions"), FailedPair.FindDefinition(TEXT("BSecondFailed.animlang"), TEXT("SecondLeak")));

	FAnimLispWorkspace MixedPair;
	MixedPair.AddSource(
		TEXT("AValid.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/DuplicateMixed"), TEXT("valid"))
			+ TEXT("(anim-variables (variable :id \"valid\" :name \"ValidOnly\" :cpp-type \"bool\"))\n"));
	MixedPair.AddSource(TEXT("BFailed.animlang"), InvalidSource(TEXT("/Game/Animations/DuplicateMixed"), TEXT("FailedLeak")));
	FAnimLangDiagnostics MixedDiagnostics;
	TestFalse(TEXT("Valid and failed duplicate modules fail build"), MixedPair.Build(MixedDiagnostics));
	const FAnimLangDiagnostic* MixedDuplicate = AnimLispWorkspaceTests::FindDiagnostic(
		MixedDiagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("Duplicate module identity"));
	if (TestNotNull(TEXT("Valid and failed modules diagnose duplicate identity"), MixedDuplicate))
	{
		TestEqual(TEXT("Mixed duplicate relates the other header"), MixedDuplicate->RelatedLocations.Num(), 1);
	}
	TestNotNull(
		TEXT("Failed half keeps parse diagnostic"),
		FindDiagnosticInFile(MixedDiagnostics, EAnimLangDiagCategory::Parse, TEXT("Unsupported"), TEXT("BFailed.animlang")));
	TestNotNull(
		TEXT("Valid half remains indexed"),
		MixedPair.FindDefinition(TEXT("AValid.animlang"), TEXT("ValidOnly")));
	TestNull(
		TEXT("Failed half does not publish definitions"),
		MixedPair.FindDefinition(TEXT("BFailed.animlang"), TEXT("FailedLeak")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceDuplicateValidModuleQuarantineTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.DuplicateValidModuleQuarantine",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceDuplicateValidModuleQuarantineTest::RunTest(const FString& Parameters)
{
	const FString Asset = TEXT("/Game/Rigs/DuplicateValid");
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("ADuplicate.riglang"),
		AnimLispWorkspaceTests::RigHeader(Asset, TEXT("hash-a"))
			+ TEXT("(define-rig-entry \"EntryA\" :id \"entry-a\" :event \"Entry A\")\n"));
	Workspace.AddSource(
		TEXT("BDuplicate.riglang"),
		AnimLispWorkspaceTests::RigHeader(Asset, TEXT("hash-b"))
			+ TEXT("(define-rig-entry \"EntryB\" :id \"entry-b\" :event \"Entry B\")\n"));
	Workspace.AddSource(
		TEXT("Consumer.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/DuplicateConsumer"), TEXT("consumer"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/DuplicateValid\" :alias Duplicate :content-hash \"consumer-expected-hash\")\n")
			+ TEXT("(rig-entry :target \"Duplicate/EntryA\")\n")
			+ TEXT("(rig-entry :target \"Duplicate/EntryB\")\n"));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Duplicate valid module identities fail build"), Workspace.Build(Diagnostics));
	const FAnimLangDiagnostic* Duplicate = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("Duplicate module identity"));
	if (TestNotNull(TEXT("Duplicate valid identity is diagnosed"), Duplicate))
	{
		TestEqual(TEXT("Duplicate valid identity relates first declaration"), Duplicate->RelatedLocations.Num(), 1);
	}
	const FAnimLangDiagnostic* Blocked = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Module,
		TEXT("blocked by duplicate module identity"));
	if (TestNotNull(TEXT("Consumer is blocked by duplicate module dependency"), Blocked))
	{
		TestEqual(TEXT("Blocked consumer relates both duplicate providers"), Blocked->RelatedLocations.Num(), 2);
	}
	TestEqual(
		TEXT("Duplicate dependency blocks consumer exactly once"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("blocked by duplicate module identity")),
		1);
	TestEqual(
		TEXT("Duplicate dependency suppresses arbitrary hash mismatch"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("Import expected hash")),
		0);
	TestEqual(
		TEXT("Duplicate dependency suppresses per-use unresolved cascade"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("Unresolved symbol")),
		0);
	for (const FString SourceFile : {FString(TEXT("ADuplicate.riglang")), FString(TEXT("BDuplicate.riglang"))})
	{
		TestNull(*(SourceFile + TEXT(" cannot resolve EntryA")), Workspace.FindDefinition(SourceFile, TEXT("EntryA")));
		TestNull(*(SourceFile + TEXT(" cannot resolve EntryB")), Workspace.FindDefinition(SourceFile, TEXT("EntryB")));
		TestEqual(
			*(SourceFile + TEXT(" has no completions")),
			Workspace.Complete(SourceFile, EAnimLispCapability::AnimRuntimeReference).Num(),
			0);
	}
	TestNull(
		TEXT("Consumer cannot resolve first duplicate definition"),
		Workspace.FindDefinition(TEXT("Consumer.animlang"), TEXT("Duplicate/EntryA")));
	TestNull(
		TEXT("Consumer cannot resolve second duplicate definition"),
		Workspace.FindDefinition(TEXT("Consumer.animlang"), TEXT("Duplicate/EntryB")));
	TestEqual(
		TEXT("Consumer completions exclude duplicate module"),
		Workspace.Complete(TEXT("Consumer.animlang"), EAnimLispCapability::AnimRuntimeReference).Num(),
		0);
	for (const FString Entry : {FString(TEXT("EntryA")), FString(TEXT("EntryB"))})
	{
		FAnimLispSymbolId Symbol;
		Symbol.Module = FAnimLispModuleId::FromAssetPath(Asset, EAnimLispModuleKind::Rig);
		Symbol.QualifiedName = Entry;
		Symbol.Kind = EAnimLispSymbolKind::RigEntry;
		TestEqual(
			*(Entry + TEXT(" has no references")),
			Workspace.FindReferences(Symbol).Num(),
			0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceSymbolKindAndBindingDirectionTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.SymbolKindAndBindingDirection",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceSymbolKindAndBindingDirectionTest::RunTest(const FString& Parameters)
{
	const FString RigSource =
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/KindRig"), TEXT("kind-hash"))
		+ TEXT("(rig-variables\n")
		+ TEXT("  (variable :id \"input\" :name \"InputVar\" :access public-input :cpp-type \"bool\")\n")
		+ TEXT("  (variable :id \"output\" :name \"OutputVar\" :access public-output :cpp-type \"bool\")\n")
		+ TEXT("  (variable :id \"internal\" :name \"InternalVar\" :access internal :cpp-type \"bool\"))\n")
		+ TEXT("(define-rig-function \"PublicFunction\" :id \"function\" :visibility public :return-cpp-type \"void\")\n")
		+ TEXT("(define-rig-entry \"PublicEntry\" :id \"entry\" :event \"Public Entry\")\n");
	auto AnimSource = [](const FString& Operation)
	{
		return AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/KindAnim"), TEXT("anim-kind"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/KindRig\" :alias Rig :content-hash \"kind-hash\")\n")
			+ TEXT("(anim-variables (variable :id \"local\" :name \"Local\" :cpp-type \"bool\"))\n")
			+ Operation;
	};
	auto ExpectRejected = [this, &RigSource, &AnimSource](
		const FString& Label,
		const FString& Operation,
		const EAnimLangDiagCategory Category,
		const FString& Fragment)
	{
		FAnimLispWorkspace Workspace;
		Workspace.AddSource(TEXT("KindRig.riglang"), RigSource);
		Workspace.AddSource(TEXT("KindAnim.animlang"), AnimSource(Operation));
		FAnimLangDiagnostics Diagnostics;
		TestFalse(*(Label + TEXT(" fails build")), Workspace.Build(Diagnostics));
		const FAnimLangDiagnostic* Diagnostic = AnimLispWorkspaceTests::FindDiagnostic(
			Diagnostics,
			Category,
			Fragment);
		if (TestNotNull(*(Label + TEXT(" emits kind or direction diagnostic")), Diagnostic))
		{
			TestFalse(*(Label + TEXT(" relates declaration")), Diagnostic->RelatedLocations.IsEmpty());
		}
	};

	ExpectRejected(
		TEXT("Rig entry rejects variable"),
		TEXT("(rig-entry :target \"Rig/InputVar\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("InputVar"));
	ExpectRejected(
		TEXT("Rig entry rejects function"),
		TEXT("(rig-entry :target \"Rig/PublicFunction\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("PublicFunction"));
	ExpectRejected(
		TEXT("Rig call rejects entry"),
		TEXT("(rig-call :target \"Rig/PublicEntry\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("PublicEntry"));
	ExpectRejected(
		TEXT("Binding rejects local define"),
		TEXT("(define \"LocalDefine\" :id \"define\")\n")
			TEXT("(bind-rig-variable :anim-variable \"LocalDefine\" :rig-variable \"Rig/InputVar\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("LocalDefine"));
	ExpectRejected(
		TEXT("Binding rejects Rig entry"),
		TEXT("(bind-rig-variable :anim-variable \"Local\" :rig-variable \"Rig/PublicEntry\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("PublicEntry"));
	ExpectRejected(
		TEXT("Binding rejects public output"),
		TEXT("(bind-rig-variable :anim-variable \"Local\" :rig-variable \"Rig/OutputVar\")\n"),
		EAnimLangDiagCategory::Capability,
		TEXT("OutputVar"));
	ExpectRejected(
		TEXT("Binding rejects internal variable"),
		TEXT("(bind-rig-variable :anim-variable \"Local\" :rig-variable \"Rig/InternalVar\")\n"),
		EAnimLangDiagCategory::Capability,
		TEXT("InternalVar"));
	ExpectRejected(
		TEXT("Binding rejects reversed modules"),
		TEXT("(bind-rig-variable :anim-variable \"Rig/InputVar\" :rig-variable \"Local\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("InputVar"));
	ExpectRejected(
		TEXT("Binding rejects local Rig endpoint"),
		TEXT("(bind-rig-variable :anim-variable \"Local\" :rig-variable \"Local\")\n"),
		EAnimLangDiagCategory::Semantic,
		TEXT("Local"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceBindingReferencesTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.BindingReferences",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceBindingReferencesTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace ValidWorkspace;
	ValidWorkspace.AddSource(TEXT("FootRig.riglang"), AnimLispWorkspaceTests::FootRigSource(TEXT("public")));
	ValidWorkspace.AddSource(
		TEXT("Binding.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/Binding"), TEXT("binding"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/FootRig\" :alias FootPlacement :content-hash \"foot-hash\")\n")
			+ TEXT("(anim-variables (variable :id \"var-ground-normal\" :name \"GroundNormal\" :cpp-type \"FVector\" :cpp-type-object \"/Script/CoreUObject.Vector\" :container-type \"None\"))\n")
			+ TEXT("(bind-rig-variable :anim-variable \"GroundNormal\" :rig-variable \"FootPlacement/GroundNormal\")\n")
			+ TEXT("(bind-rig-variable :anim-variable \"GroundNormal\" :rig-variable \"FootPlacement/GroundNormal\")\n"));
	FAnimLangDiagnostics ValidDiagnostics;
	TestTrue(TEXT("Valid repeated bindings build"), ValidWorkspace.Build(ValidDiagnostics));
	const FAnimLispDefinition* AnimDefinition = ValidWorkspace.FindDefinition(TEXT("Binding.animlang"), TEXT("GroundNormal"));
	const FAnimLispDefinition* RigDefinition = ValidWorkspace.FindDefinition(
		TEXT("Binding.animlang"),
		TEXT("FootPlacement/GroundNormal"));
	if (TestNotNull(TEXT("Bound Anim variable resolves"), AnimDefinition)
		&& TestNotNull(TEXT("Bound Rig variable resolves"), RigDefinition))
	{
		const TArray<FAnimLispReference> AnimReferences = ValidWorkspace.FindReferences(AnimDefinition->Id);
		const TArray<FAnimLispReference> RigReferences = ValidWorkspace.FindReferences(RigDefinition->Id);
		TestEqual(TEXT("Each binding references Anim endpoint"), AnimReferences.Num(), 2);
		TestEqual(TEXT("Each binding references Rig endpoint"), RigReferences.Num(), 2);
		if (AnimReferences.Num() == 2 && RigReferences.Num() == 2)
		{
			TestTrue(TEXT("Anim binding references are source ordered"), AnimReferences[0].Location.Offset < AnimReferences[1].Location.Offset);
			TestTrue(TEXT("Rig binding references are source ordered"), RigReferences[0].Location.Offset < RigReferences[1].Location.Offset);
			TestEqual(TEXT("Binding endpoints share first form location"), AnimReferences[0].Location.Offset, RigReferences[0].Location.Offset);
			TestEqual(TEXT("Binding endpoints share second form location"), AnimReferences[1].Location.Offset, RigReferences[1].Location.Offset);
		}
	}

	FAnimLispWorkspace InvalidWorkspace;
	InvalidWorkspace.AddSource(
		TEXT("OutputRig.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/OutputRig"), TEXT("output"))
			+ TEXT("(rig-variables (variable :id \"output\" :name \"OutputVar\" :access public-output :cpp-type \"bool\"))\n"));
	InvalidWorkspace.AddSource(
		TEXT("InvalidBinding.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/InvalidBinding"), TEXT("invalid"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/OutputRig\" :alias Output :content-hash \"output\")\n")
			+ TEXT("(anim-variables (variable :id \"local\" :name \"Local\" :cpp-type \"bool\"))\n")
			+ TEXT("(bind-rig-variable :anim-variable \"Local\" :rig-variable \"Output/OutputVar\")\n"));
	FAnimLangDiagnostics InvalidDiagnostics;
	TestFalse(TEXT("Invalid binding fails build"), InvalidWorkspace.Build(InvalidDiagnostics));
	const FAnimLispDefinition* InvalidAnim = InvalidWorkspace.FindDefinition(TEXT("InvalidBinding.animlang"), TEXT("Local"));
	const FAnimLispDefinition* InvalidRig = InvalidWorkspace.FindDefinition(TEXT("InvalidBinding.animlang"), TEXT("Output/OutputVar"));
	if (TestNotNull(TEXT("Invalid binding Anim endpoint still resolves"), InvalidAnim)
		&& TestNotNull(TEXT("Invalid binding Rig endpoint still resolves"), InvalidRig))
	{
		TestEqual(TEXT("Invalid binding writes no Anim reference"), InvalidWorkspace.FindReferences(InvalidAnim->Id).Num(), 0);
		TestEqual(TEXT("Invalid binding writes no Rig reference"), InvalidWorkspace.FindReferences(InvalidRig->Id).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceDuplicateImportAliasTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.DuplicateImportAlias",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceDuplicateImportAliasTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("DuplicateAlias.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/DuplicateAlias"), TEXT("duplicate-alias"))
			+ TEXT("(import-rig :asset \"/Game/Rigs/First\" :alias Shared :content-hash \"first\")\n")
			+ TEXT("(import-rig :asset \"/Game/Rigs/Second\" :alias Shared :content-hash \"second\")\n")
			+ TEXT("(anim-variables (variable :id \"sentinel\" :name \"Sentinel\" :cpp-type \"bool\"))\n"));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Duplicate import alias fails build"), Workspace.Build(Diagnostics));
	const FAnimLangDiagnostic* Diagnostic = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Parse,
		TEXT("Shared"));
	if (TestNotNull(TEXT("Duplicate alias emits schema diagnostic"), Diagnostic))
	{
		TestEqual(TEXT("Duplicate alias points at second import"), Diagnostic->Location.Line, 3);
		TestEqual(TEXT("Duplicate alias relates first import"), Diagnostic->RelatedLocations.Num(), 1);
		if (!Diagnostic->RelatedLocations.IsEmpty())
		{
			TestEqual(TEXT("Related location points at first import"), Diagnostic->RelatedLocations[0].Line, 2);
		}
	}
	TestNull(
		TEXT("Duplicate-alias source publishes no sentinel definition"),
		Workspace.FindDefinition(TEXT("DuplicateAlias.animlang"), TEXT("Sentinel")));
	TestEqual(
		TEXT("Duplicate-alias source publishes no completions"),
		Workspace.Complete(TEXT("DuplicateAlias.animlang"), EAnimLispCapability::AnimRuntimeReference).Num(),
		0);
	TestEqual(
		TEXT("Duplicate alias emits no missing-module cascade"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("Missing imported module")),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceGraphPinSemanticsTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.GraphPinSemantics",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceGraphPinSemanticsTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(
		TEXT("PinSemantics.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/PinSemantics"), TEXT("pin-semantics"))
			+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry\" :event \"Forwards Solve\"\n")
			+ TEXT("  (rig-unit :id \"input-source\" :guid \"10000000-0000-0000-0000-000000000001\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction input :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-unit :id \"valid-target\" :guid \"10000000-0000-0000-0000-000000000002\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction input :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-unit :id \"valid-source\" :guid \"10000000-0000-0000-0000-000000000003\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction output :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-unit :id \"output-target\" :guid \"10000000-0000-0000-0000-000000000004\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction output :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-unit :id \"hidden-source\" :guid \"10000000-0000-0000-0000-000000000005\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction hidden :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-unit :id \"hidden-target\" :guid \"10000000-0000-0000-0000-000000000006\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction hidden :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-unit :id \"exec-source\" :guid \"10000000-0000-0000-0000-000000000007\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Execute\" :direction output :cpp-type \"bool\" :execute-context true))\n")
			+ TEXT("  (rig-unit :id \"value-target\" :guid \"10000000-0000-0000-0000-000000000008\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"Value\" :direction input :cpp-type \"bool\" :execute-context false))\n")
			+ TEXT("  (rig-link :from \"input-source.Value\" :to \"valid-target.Value\")\n")
			+ TEXT("  (rig-link :from \"valid-source.Value\" :to \"output-target.Value\")\n")
			+ TEXT("  (rig-link :from \"hidden-source.Value\" :to \"valid-target.Value\")\n")
			+ TEXT("  (rig-link :from \"valid-source.Value\" :to \"hidden-target.Value\")\n")
			+ TEXT("  (rig-link :from \"exec-source.Execute\" :to \"value-target.Value\"))\n"));

	FAnimLangDiagnostics Diagnostics;
	TestFalse(TEXT("Invalid graph pin semantics fail build"), Workspace.Build(Diagnostics));
	TestEqual(
		TEXT("Input and hidden pins are rejected as sources"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("cannot be used as a link source")),
		2);
	TestEqual(
		TEXT("Output and hidden pins are rejected as targets"),
		AnimLispWorkspaceTests::CountDiagnosticsContaining(Diagnostics, TEXT("cannot be used as a link target")),
		2);
	const FAnimLangDiagnostic* ExecuteDiagnostic = AnimLispWorkspaceTests::FindDiagnostic(
		Diagnostics,
		EAnimLangDiagCategory::Semantic,
		TEXT("execute-context mismatch"));
	if (TestNotNull(TEXT("Execute/value link mismatch is rejected"), ExecuteDiagnostic))
	{
		TestFalse(TEXT("Execute mismatch relates target pin"), ExecuteDiagnostic->RelatedLocations.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceGraphCycleCoverageTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.GraphCycleCoverage",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceGraphCycleCoverageTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace CycleWorkspace;
	CycleWorkspace.AddSource(
		TEXT("Cycle.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/Cycle"), TEXT("cycle"))
			+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry\" :event \"Forwards Solve\"\n")
			+ TEXT("  (rig-unit :id \"a\" :guid \"20000000-0000-0000-0000-000000000001\" :class \"/Script/Test.Unit\" :coverage exact\n")
			+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\")\n")
			+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"bool\"))\n")
			+ TEXT("  (rig-unit :id \"b\" :guid \"20000000-0000-0000-0000-000000000002\" :class \"/Script/Test.Unit\" :coverage exact\n")
			+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\")\n")
			+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"bool\"))\n")
			+ TEXT("  (rig-unit :id \"downstream-lossy\" :guid \"20000000-0000-0000-0000-000000000003\" :class \"/Script/Test.Unit\" :coverage lossy\n")
			+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\")\n")
			+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"bool\"))\n")
			+ TEXT("  (rig-unit :id \"downstream-unsupported\" :guid \"20000000-0000-0000-0000-000000000004\" :class \"/Script/Test.Unit\" :coverage unsupported\n")
			+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\"))\n")
			+ TEXT("  (rig-link :from \"a.Out\" :to \"b.In\")\n")
			+ TEXT("  (rig-link :from \"b.Out\" :to \"a.In\")\n")
			+ TEXT("  (rig-link :from \"b.Out\" :to \"downstream-lossy.In\")\n")
			+ TEXT("  (rig-link :from \"downstream-lossy.Out\" :to \"downstream-unsupported.In\"))\n"));
	FAnimLangDiagnostics CycleDiagnostics;
	TestFalse(TEXT("Directed graph cycle fails build"), CycleWorkspace.Build(CycleDiagnostics));
	TestNotNull(
		TEXT("Directed graph cycle is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(CycleDiagnostics, EAnimLangDiagCategory::Semantic, TEXT("Graph cycle")));
	TestNotNull(
		TEXT("Lossy successor of cycle is covered"),
		AnimLispWorkspaceTests::FindDiagnostic(CycleDiagnostics, EAnimLangDiagCategory::Semantic, TEXT("lossy node 'downstream-lossy'")));
	TestNotNull(
		TEXT("Unsupported successor of cycle is covered"),
		AnimLispWorkspaceTests::FindDiagnostic(CycleDiagnostics, EAnimLangDiagCategory::Semantic, TEXT("unsupported node 'downstream-unsupported'")));

	FAnimLispWorkspace MissingPinWorkspace;
	MissingPinWorkspace.AddSource(
		TEXT("MissingPinCycle.riglang"),
		AnimLispWorkspaceTests::RigHeader(TEXT("/Game/Rigs/MissingPinCycle"), TEXT("missing-pin-cycle"))
			+ TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry\" :event \"Forwards Solve\"\n")
			+ TEXT("  (rig-unit :id \"a\" :guid \"30000000-0000-0000-0000-000000000001\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\")\n")
			+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"bool\"))\n")
			+ TEXT("  (rig-unit :id \"b\" :guid \"30000000-0000-0000-0000-000000000002\" :class \"/Script/Test.Unit\"\n")
			+ TEXT("    (pin :path \"In\" :direction input :cpp-type \"bool\")\n")
			+ TEXT("    (pin :path \"Out\" :direction output :cpp-type \"bool\"))\n")
			+ TEXT("  (rig-link :from \"a.Missing\" :to \"b.In\")\n")
			+ TEXT("  (rig-link :from \"b.Out\" :to \"a.In\"))\n"));
	FAnimLangDiagnostics MissingPinDiagnostics;
	TestFalse(TEXT("Missing pin graph fails build"), MissingPinWorkspace.Build(MissingPinDiagnostics));
	TestNotNull(
		TEXT("Missing pin endpoint is diagnosed"),
		AnimLispWorkspaceTests::FindDiagnostic(MissingPinDiagnostics, EAnimLangDiagCategory::Semantic, TEXT("a.Missing")));
	TestNull(
		TEXT("Missing pin does not create graph cycle"),
		AnimLispWorkspaceTests::FindDiagnostic(MissingPinDiagnostics, EAnimLangDiagCategory::Semantic, TEXT("Graph cycle")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispWorkspaceOptionalExactTypeSchemaTest,
	"AnimBP2FP.AnimLisp.Workspace.P1.OptionalExactTypeSchema",
	AnimLispWorkspaceTests::TestFlags)

bool FAnimLispWorkspaceOptionalExactTypeSchemaTest::RunTest(const FString& Parameters)
{
	struct FInvalidCase
	{
		const TCHAR* Label;
		const TCHAR* OptionalProperties;
		const TCHAR* DiagnosticKey;
	};
	const FInvalidCase InvalidCases[] = {
		{TEXT("Duplicate cpp-type-object"), TEXT(":cpp-type-object \"A\" :cpp-type-object \"B\""), TEXT(":cpp-type-object")},
		{TEXT("Non-string cpp-type-object"), TEXT(":cpp-type-object 7"), TEXT(":cpp-type-object")},
		{TEXT("Missing cpp-type-object value"), TEXT(":cpp-type-object"), TEXT(":cpp-type-object")},
		{TEXT("Duplicate container-type"), TEXT(":container-type \"None\" :container-type \"Array\""), TEXT(":container-type")},
		{TEXT("Non-string container-type"), TEXT(":container-type 7"), TEXT(":container-type")},
		{TEXT("Missing container-type value"), TEXT(":container-type"), TEXT(":container-type")},
		{TEXT("Empty container-type"), TEXT(":container-type \"\""), TEXT(":container-type")},
	};
	for (const FInvalidCase& InvalidCase : InvalidCases)
	{
		FAnimLispWorkspace Workspace;
		Workspace.AddSource(
			TEXT("InvalidOptional.animlang"),
			AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/InvalidOptional"), TEXT("invalid-optional"))
				+ FString::Printf(
					TEXT("(anim-variables (variable :id \"sentinel\" :name \"Sentinel\" :cpp-type \"bool\" %s))\n"),
					InvalidCase.OptionalProperties));
		FAnimLangDiagnostics Diagnostics;
		TestFalse(InvalidCase.Label, Workspace.Build(Diagnostics));
		TestNotNull(
			InvalidCase.DiagnosticKey,
			AnimLispWorkspaceTests::FindDiagnostic(
				Diagnostics,
				EAnimLangDiagCategory::Parse,
				InvalidCase.DiagnosticKey));
		TestNull(
			TEXT("Invalid optional exact type quarantines definition"),
			Workspace.FindDefinition(TEXT("InvalidOptional.animlang"), TEXT("Sentinel")));
		TestEqual(
			TEXT("Invalid optional exact type quarantines completions"),
			Workspace.Complete(TEXT("InvalidOptional.animlang"), EAnimLispCapability::AnimRuntimeReference).Num(),
			0);
	}

	FAnimLispWorkspace ValidWorkspace;
	ValidWorkspace.AddSource(
		TEXT("ValidOptional.animlang"),
		AnimLispWorkspaceTests::AnimHeader(TEXT("/Game/Animations/ValidOptional"), TEXT("valid-optional"))
			+ TEXT("(anim-variables (variable :id \"valid\" :name \"Valid\" :cpp-type \"bool\" :cpp-type-object \"\" :container-type \"None\"))\n"));
	FAnimLangDiagnostics ValidDiagnostics;
	TestTrue(TEXT("Empty cpp-type-object string remains legal"), ValidWorkspace.Build(ValidDiagnostics));
	const FAnimLispDefinition* ValidDefinition = ValidWorkspace.FindDefinition(TEXT("ValidOptional.animlang"), TEXT("Valid"));
	if (TestNotNull(TEXT("Valid optional exact type is indexed"), ValidDefinition))
	{
		TestEqual(TEXT("Empty cpp-type-object is preserved"), ValidDefinition->TypeSignature.CPPTypeObject, FString());
		TestEqual(TEXT("Container type is preserved"), ValidDefinition->TypeSignature.ContainerType, FString(TEXT("None")));
	}
	return true;
}

#endif
