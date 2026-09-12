#include "AnimLispLintCommandlet.h"

#include "RigLangExportCommandlet.h"
#include "AnimLangDiagnostics.h"
#include "AnimLispWorkspace.h"
#include "AnimLangParser.h"
#include "RigLangExporter.h"
#include "RigLangParser.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimLispLintCommandlet, Log, All);

namespace
{
	FString SeverityName(const EAnimLangDiagSeverity Severity)
	{
		switch (Severity)
		{
		case EAnimLangDiagSeverity::Error: return TEXT("error");
		case EAnimLangDiagSeverity::Warning: return TEXT("warning");
		case EAnimLangDiagSeverity::Info: return TEXT("info");
		case EAnimLangDiagSeverity::Hint: return TEXT("hint");
		}
		return TEXT("unknown");
	}

	FString CategoryName(const EAnimLangDiagCategory Category)
	{
		switch (Category)
		{
		case EAnimLangDiagCategory::Lex: return TEXT("lex");
		case EAnimLangDiagCategory::Parse: return TEXT("parse");
		case EAnimLangDiagCategory::Type: return TEXT("type");
		case EAnimLangDiagCategory::Semantic: return TEXT("semantic");
		case EAnimLangDiagCategory::Import: return TEXT("import");
		case EAnimLangDiagCategory::RoundTrip: return TEXT("roundtrip");
		case EAnimLangDiagCategory::Module: return TEXT("module");
		case EAnimLangDiagCategory::Capability: return TEXT("capability");
		}
		return TEXT("unknown");
	}
}

FString AnimBP2FPCommandlets::SerializeLintDiagnostics(
	const FString& WorkspaceRoot,
	const int32 SourceCount,
	const FAnimLangDiagnostics& Diagnostics)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("workspace"), WorkspaceRoot);
	Root->SetNumberField(TEXT("source_count"), SourceCount);
	Root->SetNumberField(TEXT("error_count"), Diagnostics.ErrorCount());
	Root->SetNumberField(TEXT("warning_count"), Diagnostics.WarningCount());
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("severity"), SeverityName(Diagnostic.Severity));
		Item->SetStringField(TEXT("category"), CategoryName(Diagnostic.Category));
		Item->SetStringField(TEXT("message"), Diagnostic.Message);
		Item->SetStringField(TEXT("source"), Diagnostic.Location.SourceFile);
		Item->SetNumberField(TEXT("line"), Diagnostic.Location.Line);
		Item->SetNumberField(TEXT("column"), Diagnostic.Location.Column);
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("diagnostics"), Items);
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

int32 AnimBP2FPCommandlets::LintExitCode(const FAnimLangDiagnostics& Diagnostics)
{
	return Diagnostics.HasErrors() ? 1 : 0;
}

void AnimBP2FPCommandlets::CollectStrictCoverageDiagnostics(
	const FString& SourcePath, const FString& Source, FAnimLangDiagnostics& OutDiagnostics)
{
	if (FPaths::GetExtension(SourcePath).Equals(TEXT("riglang"), ESearchCase::IgnoreCase))
	{
		TArray<FRigLangParseError> Errors;
		const TSharedPtr<FRigModuleAST> AST = FRigLangParser::Parse(Source, SourcePath, Errors);
		for (const FRigLangParseError& Error : Errors)
		{
			if (Error.bWarning)
			{
				FAnimLangSourceLoc Location = Error.Location;
				if (Location.SourceFile.IsEmpty()) Location.SourceFile = SourcePath;
				OutDiagnostics.Add(EAnimLangDiagSeverity::Warning, EAnimLangDiagCategory::Parse,
					Error.Message, Location);
			}
		}
		if (!AST.IsValid() || Errors.ContainsByPredicate(
			[](const FRigLangParseError& Error) { return !Error.bWarning; }))
		{
			FAnimLangSourceLoc Location; Location.SourceFile = SourcePath;
			OutDiagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Parse,
				TEXT("Rig source has fatal parse diagnostics"), Location);
			return;
		}
		const FString SemanticHash = FRigLangExporter::ComputeContentHash(AST->ToCanonicalHashInput());
		if (SemanticHash != AST->Header.ContentHash)
		{
			FAnimLangSourceLoc Location; Location.SourceFile = SourcePath;
			OutDiagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
				TEXT("Rig semantic hash does not match its content-hash header"), Location);
		}
		const FRigCoverageTotals Totals = AST->GetCoverageTotals();
		if (Totals.Lossy > 0 || Totals.Unsupported > 0)
		{
			FAnimLangSourceLoc Location; Location.SourceFile = SourcePath;
			OutDiagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
				TEXT("Strict coverage rejects lossy or unsupported Rig semantics"), Location);
		}
		return;
	}
	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(Source, Errors);
	for (const FAnimLangParseError& Error : Errors)
	{
		if (Error.bWarning)
		{
			FAnimLangSourceLoc Location = Error.Location;
			if (Location.SourceFile.IsEmpty()) Location.SourceFile = SourcePath;
			OutDiagnostics.Add(EAnimLangDiagSeverity::Warning, EAnimLangDiagCategory::Parse,
				Error.Message, Location);
		}
	}
	if (!AST.IsValid() || Errors.ContainsByPredicate(
		[](const FAnimLangParseError& Error) { return !Error.bWarning; }))
	{
		FAnimLangSourceLoc Location; Location.SourceFile = SourcePath;
		OutDiagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Parse,
			TEXT("Anim source has fatal parse diagnostics"), Location);
		return;
	}
	bool bInvalid = false;
	AST->VisitNodes([&](const TSharedPtr<FAnimNodeAST>& Node)
	{
		bInvalid |= Node->Coverage == EAnimNodeCoverage::Lossy || Node->Coverage == EAnimNodeCoverage::Unsupported;
	});
	if (bInvalid)
	{
		FAnimLangSourceLoc Location; Location.SourceFile = SourcePath;
		OutDiagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic,
			TEXT("Strict coverage rejects lossy or unsupported Anim semantics"), Location);
	}
}

UAnimLispLintCommandlet::UAnimLispLintCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UAnimLispLintCommandlet::Main(const FString& Params)
{
	FString WorkspaceRoot;
	FString Error;
	if (!AnimBP2FPCommandlets::ParseWorkspace(Params, WorkspaceRoot, Error))
	{
		UE_LOG(LogAnimLispLintCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	const bool bWorkspaceExists = IFileManager::Get().DirectoryExists(*WorkspaceRoot);

	TArray<FString> Sources;
	TArray<FString> RigSources;
	IFileManager::Get().FindFilesRecursive(Sources, *WorkspaceRoot, TEXT("*.animlang"), true, false);
	IFileManager::Get().FindFilesRecursive(RigSources, *WorkspaceRoot, TEXT("*.riglang"), true, false);
	Sources.Append(RigSources);
	Sources.RemoveAll([](const FString& SourcePath)
	{
		const FString Filename = FPaths::GetCleanFilename(SourcePath);
		return Filename.StartsWith(TEXT("_"))
			|| Filename.EndsWith(TEXT(".tmp"))
			|| SourcePath.Contains(TEXT("/Archive/"), ESearchCase::IgnoreCase)
			|| SourcePath.Contains(TEXT("/Diagnostics/"), ESearchCase::IgnoreCase);
	});
	Sources.Sort();
	FAnimLispWorkspace Workspace;
	FAnimLangDiagnostics Diagnostics;
	if (!bWorkspaceExists || Sources.Num() == 0)
	{
		FAnimLangSourceLoc Location;
		Location.SourceFile = WorkspaceRoot;
		Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Module,
			bWorkspaceExists ? TEXT("Workspace contains no AnimLisp module sources")
				: TEXT("Workspace directory does not exist"), Location);
	}
	for (const FString& SourcePath : Sources)
	{
		FString Source;
		if (!FFileHelper::LoadFileToString(Source, *SourcePath))
		{
			FAnimLangSourceLoc Location;
			Location.SourceFile = SourcePath;
			Diagnostics.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Parse,
				TEXT("Failed to read workspace source"), Location);
			continue;
		}
		Workspace.AddSource(SourcePath, Source);
		AnimBP2FPCommandlets::CollectStrictCoverageDiagnostics(SourcePath, Source, Diagnostics);
	}
	FAnimLangDiagnostics BuildDiagnostics;
	Workspace.Build(BuildDiagnostics);
	Diagnostics.Items.Append(BuildDiagnostics.Items);

	for (const FAnimLangDiagnostic& Diagnostic : Diagnostics.Items)
	{
		if (Diagnostic.Severity == EAnimLangDiagSeverity::Error)
		{
			UE_LOG(LogAnimLispLintCommandlet, Error, TEXT("%s"), *Diagnostic.ToCompactString());
		}
		else
		{
			UE_LOG(LogAnimLispLintCommandlet, Warning, TEXT("%s"), *Diagnostic.ToCompactString());
		}
	}

	const FString Json = AnimBP2FPCommandlets::SerializeLintDiagnostics(
		WorkspaceRoot, Sources.Num(), Diagnostics);
	const FString DiagnosticsPath = WorkspaceRoot / TEXT("Diagnostics/animlisp-lint.json");
	if (!AnimBP2FPCommandlets::PrepareOutputForExport(DiagnosticsPath, Error)
		|| !AnimBP2FPCommandlets::WriteFileAtomically(DiagnosticsPath, Json, Error))
	{
		UE_LOG(LogAnimLispLintCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}
	UE_LOG(LogAnimLispLintCommandlet, Display,
		TEXT("AnimLisp lint: %d errors, %d warnings -> %s"),
		Diagnostics.ErrorCount(), Diagnostics.WarningCount(), *DiagnosticsPath);
	return AnimBP2FPCommandlets::LintExitCode(Diagnostics);
}
