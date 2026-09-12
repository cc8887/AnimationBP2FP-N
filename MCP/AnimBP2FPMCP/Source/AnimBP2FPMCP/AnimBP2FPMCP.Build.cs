// AnimBP2FPMCP.Build.cs - UE 5.8 Toolset Registry adapter

using UnrealBuildTool;

public class AnimBP2FPMCP : ModuleRules
{
	public AnimBP2FPMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		string CppStandardName = Target.Version.MajorVersion > 5
			|| (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 5)
			? "Cpp20" : "Cpp17";
		CppStandard = (CppStandardVersion)System.Enum.Parse(typeof(CppStandardVersion), CppStandardName);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimBP2FP",
			"AnimBP2FPEditor",
			"UnrealEd",
			"AssetRegistry",
			"BlueprintGraph",
			"BlueprintLisp",
			"Json"
		});

		if (Target.Version.MajorVersion < 5
			|| (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 8))
		{
			throw new BuildException("AnimBP2FPMCP requires Unreal Engine 5.8 or newer.");
		}

		// UE5.8 ships the registry API and editor subsystem in one module; later
		// engine branches split the editor implementation into ToolsetRegistryEditor.
		PublicDependencyModuleNames.Add("ToolsetRegistry");
		if (Target.Version.MajorVersion > 5)
		{
			PublicDependencyModuleNames.Add("ToolsetRegistryEditor");
		}
	}
}
