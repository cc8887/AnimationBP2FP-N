#include "RigLangImportCommandlet.h"

#if ENGINE_MAJOR_VERSION < 5
#include "ControlRigBlueprint.h"
#else
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#endif
#include "RigLangExporter.h"
#include "RigLangImporter.h"
#include "RigLangExportCommandlet.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigLangImportCommandlet, Log, All);

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
FString GArtifactWriteFailureForTest;
#endif

FString SafeRunId(FString RunId)
{
	if (RunId.IsEmpty()) RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%S"));
	for (TCHAR& Character : RunId)
		if (!FChar::IsAlnum(Character) && Character != TEXT('-') && Character != TEXT('_')) Character = TEXT('_');
	return RunId;
}

bool SaveArtifact(const FString& Directory, const FString& Name, const FString& Content,
	TArray<FString>& Diagnostics)
{
	const FString Path = Directory / Name;
#if WITH_DEV_AUTOMATION_TESTS
	if (Name == GArtifactWriteFailureForTest)
	{
		Diagnostics.Add(TEXT("Injected artifact write failure: ") + Path);
		return false;
	}
#endif
	const FString TemporaryPath = Path + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(
		Content, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Diagnostics.Add(TEXT("Failed to write artifact: ") + Path);
		return false;
	}
	const FTCHARToUTF8 Utf8(*Content);
	const int64 WrittenSize = IFileManager::Get().FileSize(*TemporaryPath);
	if (WrittenSize != Utf8.Length())
	{
		Diagnostics.Add(FString::Printf(
			TEXT("Artifact write verification failed: %s (expected %d bytes, found %lld)"),
			*Path, Utf8.Length(), WrittenSize));
		IFileManager::Get().Delete(*TemporaryPath, false, true);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TemporaryPath, true, true))
	{
		Diagnostics.Add(TEXT("Failed to finalize staged artifact: ") + Path);
		IFileManager::Get().Delete(*TemporaryPath, false, true);
		return false;
	}
	return true;
}

FString JsonEscape(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Value;
}

FString DiagnosticsJson(const TArray<FString>& Diagnostics)
{
	TArray<FString> Items;
	for (const FString& Diagnostic : Diagnostics)
		Items.Add(TEXT("{\"severity\":\"error\",\"message\":\"")
			+ JsonEscape(Diagnostic) + TEXT("\"}"));
	return TEXT("{\"diagnostics\":[") + FString::Join(Items, TEXT(",")) + TEXT("]}");
}

bool PublishArtifactDirectory(const FString& Root, const FString& Staging,
	const FString& Final, TArray<FString>& Diagnostics)
{
	IFileManager& Files = IFileManager::Get();
	const FString Backup = Root / (TEXT(".backup-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	bool bMovedOld = false;
	if (Files.DirectoryExists(*Final))
	{
		bMovedOld = Files.Move(*Backup, *Final, true, true);
		if (!bMovedOld)
		{
			Diagnostics.Add(TEXT("Failed to move previous artifact directory to backup: ") + Final);
			return false;
		}
	}
	if (!Files.Move(*Final, *Staging, true, true))
	{
		Diagnostics.Add(TEXT("Failed to publish artifact staging directory: ") + Staging);
		if (bMovedOld && !Files.Move(*Final, *Backup, true, true))
		{
			Diagnostics.Add(TEXT("Failed to restore previous artifact directory from backup '")
				+ Backup + TEXT("' to '") + Final + TEXT("'; backup was preserved"));
		}
		return false;
	}
	if (bMovedOld) Files.DeleteDirectory(*Backup, false, true);
	return true;
}
}

#if WITH_DEV_AUTOMATION_TESTS
void RigLangRoundTrip::SetArtifactWriteFailureForTest(const FString& ArtifactName)
{
	GArtifactWriteFailureForTest = ArtifactName;
}
#endif

FRigModuleAST RigLangRoundTrip::BuildComparableModule(
	const FRigModuleAST& SourceModule,
	const FRigModuleAST& ReExportedModule)
{
	FRigModuleAST Comparable = ReExportedModule;
	FString SourceObjectRoot;
	FString GeneratedObjectRoot;
	for (const FRigFunctionAST& GeneratedFunction : Comparable.Functions)
	{
		const FRigFunctionAST* SourceFunction = SourceModule.Functions.FindByPredicate(
			[&GeneratedFunction](const FRigFunctionAST& Candidate)
			{
				return Candidate.Name == GeneratedFunction.Name;
			});
		if (!SourceFunction || SourceFunction->FunctionIdentifier.HostObject.IsEmpty()
			|| GeneratedFunction.FunctionIdentifier.HostObject.IsEmpty()) continue;
		SourceObjectRoot = SourceFunction->FunctionIdentifier.HostObject;
		GeneratedObjectRoot = GeneratedFunction.FunctionIdentifier.HostObject;
		SourceObjectRoot.RemoveFromEnd(TEXT("_C"));
		GeneratedObjectRoot.RemoveFromEnd(TEXT("_C"));
		break;
	}
	auto RemapGeneratedIdentity = [&SourceObjectRoot, &GeneratedObjectRoot](FString& Value)
	{
		if (!GeneratedObjectRoot.IsEmpty()
			&& Value.StartsWith(GeneratedObjectRoot, ESearchCase::CaseSensitive))
		{
			Value = SourceObjectRoot + Value.Mid(GeneratedObjectRoot.Len());
		}
	};
	auto RemapGraph = [&RemapGeneratedIdentity](FRigGraphAST& Graph)
	{
		RemapGeneratedIdentity(Graph.StableId);
		RemapGeneratedIdentity(Graph.ParentStableId);
		for (FRigNodeAST& Node : Graph.Nodes)
		{
			RemapGeneratedIdentity(Node.ContainedGraphStableId);
			RemapGeneratedIdentity(Node.FunctionIdentifier.HostObject);
			RemapGeneratedIdentity(Node.FunctionIdentifier.LibraryNodePath);
		}
	};
	for (FRigGraphAST& Graph : Comparable.Graphs) RemapGraph(Graph);
	for (FRigFunctionAST& Function : Comparable.Functions)
	{
		const FRigFunctionAST* SourceFunction = SourceModule.Functions.FindByPredicate(
			[&Function](const FRigFunctionAST& Candidate)
			{
				return Candidate.Name == Function.Name;
			});
		if (SourceFunction && !GeneratedObjectRoot.IsEmpty()
			&& Function.FunctionIdentifier.HostObject.StartsWith(
				GeneratedObjectRoot, ESearchCase::CaseSensitive))
		{
			Function.StableId = SourceFunction->StableId;
		}
		RemapGeneratedIdentity(Function.GraphStableId);
		RemapGeneratedIdentity(Function.FunctionIdentifier.HostObject);
		RemapGeneratedIdentity(Function.FunctionIdentifier.LibraryNodePath);
		for (FRigFunctionDependencyAST& Dependency : Function.Dependencies)
		{
			RemapGeneratedIdentity(Dependency.HostObject);
			RemapGeneratedIdentity(Dependency.LibraryNodePath);
		}
		RemapGraph(Function.Graph);
	}
	for (FRigEntryAST& Entry : Comparable.Entries)
	{
		const FRigEntryAST* SourceEntry = SourceModule.Entries.FindByPredicate(
			[&Entry](const FRigEntryAST& Candidate)
			{
				return Candidate.Name == Entry.Name;
			});
		if (SourceEntry
			&& Entry.StableId.StartsWith(TEXT("/Engine/Transient."), ESearchCase::CaseSensitive))
		{
			Entry.StableId = SourceEntry->StableId;
		}
		RemapGeneratedIdentity(Entry.StableId);
		RemapGeneratedIdentity(Entry.GraphStableId);
		RemapGraph(Entry.Graph);
	}
	TMap<FString, FString> GeneratedToSourceGraphIds;
	TFunction<void(const FRigGraphAST&, const FRigGraphAST&)> MapGraphPair;
	MapGraphPair = [&SourceModule, &Comparable, &GeneratedToSourceGraphIds, &MapGraphPair](
		const FRigGraphAST& SourceGraph, const FRigGraphAST& GeneratedGraph)
	{
		if (GeneratedGraph.StableId.IsEmpty() || SourceGraph.StableId.IsEmpty()) return;
		if (const FString* Existing = GeneratedToSourceGraphIds.Find(GeneratedGraph.StableId))
		{
			if (*Existing != SourceGraph.StableId) GeneratedToSourceGraphIds.Remove(GeneratedGraph.StableId);
			return;
		}
		GeneratedToSourceGraphIds.Add(GeneratedGraph.StableId, SourceGraph.StableId);
		for (const FRigNodeAST& GeneratedNode : GeneratedGraph.Nodes)
		{
			if (GeneratedNode.ContainedGraphStableId.IsEmpty()) continue;
			const FRigNodeAST* SourceNode = SourceGraph.Nodes.FindByPredicate(
				[&GeneratedNode](const FRigNodeAST& Candidate)
				{
					return !GeneratedNode.Guid.IsEmpty()
						? Candidate.Guid == GeneratedNode.Guid
						: Candidate.Guid.IsEmpty() && Candidate.StableId == GeneratedNode.StableId;
				});
			if (!SourceNode || SourceNode->ContainedGraphStableId.IsEmpty()) continue;
			const FRigGraphAST* SourceChild = SourceModule.Graphs.FindByPredicate(
				[SourceNode](const FRigGraphAST& Graph)
				{
					return Graph.StableId == SourceNode->ContainedGraphStableId;
				});
			const FRigGraphAST* GeneratedChild = Comparable.Graphs.FindByPredicate(
				[&GeneratedNode](const FRigGraphAST& Graph)
				{
					return Graph.StableId == GeneratedNode.ContainedGraphStableId;
				});
			if (SourceChild && GeneratedChild) MapGraphPair(*SourceChild, *GeneratedChild);
		}
	};
	for (const FRigFunctionAST& GeneratedFunction : Comparable.Functions)
	{
		if (const FRigFunctionAST* SourceFunction = SourceModule.Functions.FindByPredicate(
			[&GeneratedFunction](const FRigFunctionAST& Candidate)
			{
				return Candidate.Name == GeneratedFunction.Name;
			}))
		{
			MapGraphPair(SourceFunction->Graph, GeneratedFunction.Graph);
		}
	}
	for (const FRigEntryAST& GeneratedEntry : Comparable.Entries)
	{
		if (const FRigEntryAST* SourceEntry = SourceModule.Entries.FindByPredicate(
			[&GeneratedEntry](const FRigEntryAST& Candidate)
			{
				return Candidate.Name == GeneratedEntry.Name;
			}))
		{
			MapGraphPair(SourceEntry->Graph, GeneratedEntry.Graph);
		}
	}
	for (const FString& UniqueRole : {TEXT("root"), TEXT("function-library")})
	{
		const FRigGraphAST* SourceGraph = SourceModule.Graphs.FindByPredicate(
			[UniqueRole](const FRigGraphAST& Graph) { return Graph.Role == UniqueRole; });
		const FRigGraphAST* GeneratedGraph = Comparable.Graphs.FindByPredicate(
			[UniqueRole](const FRigGraphAST& Graph) { return Graph.Role == UniqueRole; });
		if (SourceGraph && GeneratedGraph) MapGraphPair(*SourceGraph, *GeneratedGraph);
	}
	auto ApplyGraphIdMap = [&GeneratedToSourceGraphIds](FRigGraphAST& Graph)
	{
		if (const FString* StableId = GeneratedToSourceGraphIds.Find(Graph.StableId))
			Graph.StableId = *StableId;
		if (const FString* ParentId = GeneratedToSourceGraphIds.Find(Graph.ParentStableId))
			Graph.ParentStableId = *ParentId;
		for (FRigNodeAST& Node : Graph.Nodes)
			if (const FString* ContainedId = GeneratedToSourceGraphIds.Find(Node.ContainedGraphStableId))
				Node.ContainedGraphStableId = *ContainedId;
	};
	for (FRigGraphAST& Graph : Comparable.Graphs) ApplyGraphIdMap(Graph);
	for (FRigFunctionAST& Function : Comparable.Functions)
	{
		if (const FString* GraphId = GeneratedToSourceGraphIds.Find(Function.GraphStableId))
			Function.GraphStableId = *GraphId;
		ApplyGraphIdMap(Function.Graph);
	}
	for (FRigEntryAST& Entry : Comparable.Entries)
	{
		if (const FString* GraphId = GeneratedToSourceGraphIds.Find(Entry.GraphStableId))
			Entry.GraphStableId = *GraphId;
		ApplyGraphIdMap(Entry.Graph);
	}
	Comparable.Header.ModuleId.AssetPath = SourceModule.Header.ModuleId.AssetPath;
	Comparable.Header.ContentHash = FRigLangExporter::ComputeContentHash(
		Comparable.ToCanonicalHashInput());
	return Comparable;
}

FRigLangTransientRoundTripResult RigLangRoundTrip::RunTransient(
	UControlRigBlueprint* SourceBlueprint, const FString& RunId)
{
	FRigLangTransientRoundTripResult Result;
	const FString ArtifactRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Tests/RigLangRoundTrip"));
	Result.ArtifactDirectory = ArtifactRoot / SafeRunId(RunId);
	const FString StagingDirectory = ArtifactRoot / (TEXT(".staging-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*StagingDirectory, true);
	FString SourceText;
	FString ImportedText;
	FString ExpectedNormalized;
	FString ActualNormalized;
	auto Finalize = [&]()
	{
		const bool bSemanticSuccess = Result.bSuccess && Result.Diagnostics.Num() == 0;
		bool bArtifactsComplete = true;
		bArtifactsComplete &= SaveArtifact(
			StagingDirectory, TEXT("source.riglang"), SourceText, Result.Diagnostics);
		bArtifactsComplete &= SaveArtifact(
			StagingDirectory, TEXT("imported.riglang"), ImportedText, Result.Diagnostics);
		bArtifactsComplete &= SaveArtifact(StagingDirectory, TEXT("expected.normalized.riglang"),
			ExpectedNormalized, Result.Diagnostics);
		bArtifactsComplete &= SaveArtifact(StagingDirectory, TEXT("actual.normalized.riglang"),
			ActualNormalized, Result.Diagnostics);
		bArtifactsComplete &= SaveArtifact(
			StagingDirectory, TEXT("diff.json"), Result.Diff.ToJson(), Result.Diagnostics);
		const FString Summary = FString::Printf(
			TEXT("{\"success\":%s,\"models\":%d,\"nodes\":%d,\"links\":%d,\"hierarchy\":%d,\"compile_errors\":%d,\"semantic_differences\":%d}"),
			bSemanticSuccess ? TEXT("true") : TEXT("false"), Result.ModelCount, Result.NodeCount,
			Result.LinkCount, Result.HierarchyCount, Result.CompileErrorCount,
			Result.Diff.Differences.Num());
		bArtifactsComplete &= SaveArtifact(
			StagingDirectory, TEXT("summary.json"), Summary, Result.Diagnostics);
		bArtifactsComplete &= SaveArtifact(StagingDirectory, TEXT("diagnostics.txt"),
			FString::Join(Result.Diagnostics, TEXT("\n")), Result.Diagnostics);
		bArtifactsComplete &= SaveArtifact(StagingDirectory, TEXT("diagnostics.json"),
			DiagnosticsJson(Result.Diagnostics), Result.Diagnostics);
		if (!bArtifactsComplete)
		{
			Result.bSuccess = false;
			IFileManager::Get().DeleteDirectory(*StagingDirectory, false, true);
			return;
		}
		const bool bPublished = PublishArtifactDirectory(ArtifactRoot, StagingDirectory,
			Result.ArtifactDirectory, Result.Diagnostics);
		Result.bSuccess = bSemanticSuccess && bPublished;
		if (!bPublished)
			IFileManager::Get().DeleteDirectory(*StagingDirectory, false, true);
	};
	if (!SourceBlueprint)
	{
		Result.Diagnostics.Add(TEXT("Source Control Rig is null"));
		Finalize();
		return Result;
	}

	const FRigLangExportResult Source = FRigLangExporter::Export(SourceBlueprint);
	Result.ModelCount = Source.Coverage.ModelTotal;
	Result.NodeCount = Source.Coverage.NodeTotal;
	Result.LinkCount = Source.Coverage.LinkTotal;
	Result.HierarchyCount = Source.Module ? Source.Module->Hierarchy.Num() : 0;
	if (!Source.bSuccess || !Source.Module)
	{
		Result.Diagnostics.Append(Source.Errors);
		if (Result.Diagnostics.Num() == 0) Result.Diagnostics.Add(TEXT("Source Control Rig export failed"));
		Finalize();
		return Result;
	}
	SourceText = Source.Module->ToCanonicalString();
	ExpectedNormalized = Source.Module->ToCanonicalHashInput();

	FRigLangImportOptions Options;
	Options.TargetPackage = TEXT("/Engine/Transient/RT_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Options.bTransient = true;
	Options.bStrict = true;
	const FRigLangImportResult Imported = FRigLangImporter::Import(*Source.Module, Options);
	Result.ImportedBlueprint = Imported.Blueprint;
	if (Imported.Diagnostics.HasErrors())
	{
		Result.CompileErrorCount = Imported.Diagnostics.ErrorCount();
		Result.Diagnostics.Add(Imported.Diagnostics.ToReport());
	}
	if (!Imported.Blueprint || !Imported.bCompiled)
	{
		if (!Imported.Blueprint && Result.Diagnostics.Num() == 0) Result.Diagnostics.Add(TEXT("Transient import failed"));
	}
	else
	{
		const FRigLangExportResult ReExport = FRigLangExporter::Export(Imported.Blueprint);
		if (!ReExport.bSuccess || !ReExport.Module)
			Result.Diagnostics.Append(ReExport.Errors);
		else
		{
			ImportedText = ReExport.Module->ToCanonicalString();
			FRigModuleAST Comparable = BuildComparableModule(*Source.Module, *ReExport.Module);
			ActualNormalized = Comparable.ToCanonicalHashInput();
			Result.Diff = FRigLangDiffer::Diff(*Source.Module, Comparable);
		}
	}
	Result.bSuccess = Imported.Blueprint && Imported.bCompiled
		&& Result.CompileErrorCount == 0 && Result.Diff.IsEmpty() && Result.Diagnostics.Num() == 0;
	Finalize();
	return Result;
}

URigLangImportCommandlet::URigLangImportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 URigLangImportCommandlet::Main(const FString& Params)
{
	FString AssetPath, Error;
	if (!AnimBP2FPCommandlets::ParseAssetPath(Params, AssetPath, Error))
	{
		UE_LOG(LogRigLangImportCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	FString RunId;
	FParse::Value(*Params, TEXT("RunId="), RunId);
	const FString ObjectPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
	UControlRigBlueprint* Blueprint = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
	const FRigLangTransientRoundTripResult Result = RigLangRoundTrip::RunTransient(Blueprint, RunId);
	UE_LOG(LogRigLangImportCommandlet, Display, TEXT("RigLang round-trip artifacts: %s"),
		*Result.ArtifactDirectory);
	for (const FString& Diagnostic : Result.Diagnostics)
		UE_LOG(LogRigLangImportCommandlet, Error, TEXT("%s"), *Diagnostic);
	return Result.bSuccess ? 0 : 1;
}
