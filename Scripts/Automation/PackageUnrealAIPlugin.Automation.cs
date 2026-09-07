// Copyright UnrealOps. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using AutomationTool;
using EpicGames.Core;
using UnrealBuildTool;
using JsonObject = System.Text.Json.Nodes.JsonObject;

/// <summary>Use Unreal's package filter with an explicitly staged dependency closure.</summary>
public sealed class PackageUnrealAIPlugin : BuildCommand
{
	public override void ExecuteBuild()
	{
		FileReference ProjectFile = new FileReference(ParseRequiredStringParam("HostProject"));
		FileReference PluginFile = new FileReference(ParseRequiredStringParam("Plugin"));
		DirectoryReference Output = new DirectoryReference(ParseRequiredStringParam("Package"));
		UnrealTargetPlatform Platform = UnrealTargetPlatform.Parse(ParseRequiredStringParam("TargetPlatform"));
		if (DirectoryReference.Exists(Output))
		{
			throw new AutomationException("Refusing to reuse package output: {0}", Output);
		}
		FileReference Ubt = new FileReference(
			Path.Combine(CmdEnv.LocalRoot, "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll"));
		string Extra = ParseParam("EditorOnly") && !String.IsNullOrEmpty(ParseParamValue("EditorModules"))
						   ? ""
						   : " -NoPCH -NoSharedPCH -DisableUnity";
		string Architecture = ParseParamValue("Architecture");
		if (!String.IsNullOrEmpty(Architecture))
		{
			Extra += " -architecture=" + Architecture;
		}
		// Strict development mode changes staged plugin rules, not global engine
		// flags. Do not use -Module: UBT then omits required loader metadata actions.
		Extra += " -BuildDependantPlugins";
		IReadOnlyList<UnrealTargetPlatform> Hosts = new[] { Platform };
		List<UnrealTargetPlatform> Targets =
			ParseParam("EditorOnly") ? new List<UnrealTargetPlatform>() : new List<UnrealTargetPlatform> { Platform };
		string PluginDirectory = Path.Combine(ProjectFile.Directory.FullName, "Plugins");
		Dictionary<string, FileReference> Plugins = Directory
			.GetFiles(PluginDirectory, "*.uplugin", SearchOption.AllDirectories)
			.Select(Path => new FileReference(Path))
			.ToDictionary(File => File.GetFileNameWithoutExtension(), StringComparer.OrdinalIgnoreCase);
		string OriginalProject = File.ReadAllText(ProjectFile.FullName);
		try
		{
			// Each artifact needs its own manifest and target eligibility. For example,
			// experimental access excludes Shipping while its runtime dependencies do not.
			foreach (FileReference Current in Plugins.Values.OrderBy(File => File == PluginFile ? 0 : 1))
			{
				DirectoryReference CurrentOutput = Current == PluginFile
					? Output
					: DirectoryReference.Combine(Output.ParentDirectory, Current.GetFileNameWithoutExtension());
				if (DirectoryReference.Exists(CurrentOutput))
				{
					throw new AutomationException("Refusing to reuse package output: {0}", CurrentOutput);
				}
				HashSet<string> Active = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
				CollectDependencies(Current.GetFileNameWithoutExtension(), Plugins, Active);
				JsonObject Project = JsonNode.Parse(OriginalProject).AsObject();
				JsonArray References = new JsonArray();
				foreach (string Name in Plugins.Keys)
				{
					References.Add(new JsonObject { ["Name"] = Name, ["Enabled"] = Active.Contains(Name) });
				}
				// UBT gives the selected -Plugin its own rules scope. Reverse dependents
				// must be disabled while packaging a lower-level SDK plugin.
				Project["Plugins"] = References;
				File.WriteAllText(ProjectFile.FullName, Project.ToJsonString());
				PluginDescriptor Original = PluginDescriptor.FromFile(Current);
				// UBT validates every discovered rules module, including disabled plugins.
				// Hide reverse dependents outside the staged host during this build.
				Dictionary<string, string> HiddenDirectories = new Dictionary<string, string>();
				try
				{
					foreach (KeyValuePair<string, FileReference> Plugin in Plugins)
					{
						if (!Active.Contains(Plugin.Key))
						{
							string Source = Plugin.Value.Directory.FullName;
							string Hidden = Path.Combine(ProjectFile.Directory.ParentDirectory.FullName,
								"InactivePlugins", Plugin.Key);
							Directory.CreateDirectory(Path.GetDirectoryName(Hidden));
							Directory.Move(Source, Hidden);
							HiddenDirectories.Add(Source, Hidden);
						}
					}
					FileReference[] Products =
						BuildPlugin.CompilePlugin(Ubt, ProjectFile, Current, Original, Hosts, Targets, Extra);
					BuildPlugin.PackagePlugin(Current, Products, CurrentOutput, false, new[] { Platform });
				}
				finally
				{
					foreach (KeyValuePair<string, string> Hidden in HiddenDirectories)
					{
						Directory.Move(Hidden.Value, Hidden.Key);
					}
				}
				FileReference Result = FileReference.Combine(CurrentOutput, Current.GetFileName());
				PluginDescriptor Saved = PluginDescriptor.FromFile(Result);
				Saved.bEnabledByDefault = Original.bEnabledByDefault;
				Saved.Save(Result.FullName);
			}
		}
		finally
		{
			File.WriteAllText(ProjectFile.FullName, OriginalProject);
		}
	}

	private static void CollectDependencies(string Name, Dictionary<string, FileReference> Plugins, HashSet<string> Active)
	{
		if (!Plugins.ContainsKey(Name) || !Active.Add(Name))
		{
			return;
		}
		foreach (PluginReferenceDescriptor Reference in PluginDescriptor.FromFile(Plugins[Name]).Plugins ?? new List<PluginReferenceDescriptor>())
		{
			if (Reference.bEnabled)
			{
				CollectDependencies(Reference.Name, Plugins, Active);
			}
		}
	}
}
