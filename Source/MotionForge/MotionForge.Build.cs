using System.IO;
using UnrealBuildTool;

public class MotionForge : ModuleRules
{
	/**
	 * Whether a sibling plugin is installed beside this one.
	 *
	 * Needed because "optional" has to hold at *build* time, not only at runtime. Naming a module in
	 * PrivateIncludePathModuleNames takes no link and adds no .uplugin dependency - but UBT still has
	 * to resolve the name, and refuses the whole build with "Could not find definition for module"
	 * when it cannot. A plugin packaged and installed on its own then fails to compile for the
	 * customer, which is the exact opposite of what the header-only pattern was for.
	 *
	 * Walking up from this module covers every layout a plugin is ever in: beside us in a project's
	 * Plugins folder, in our own Plugins/Forge, or under Engine/Plugins/AutomationForge. It also
	 * correctly says no inside the throwaway host project BuildPlugin stages, which contains one
	 * plugin and nothing else.
	 */
	private bool IsPluginPresent(string PluginName)
	{
		for (DirectoryInfo Dir = new DirectoryInfo(ModuleDirectory); Dir != null; Dir = Dir.Parent)
		{
			if (File.Exists(Path.Combine(Dir.FullName, PluginName, PluginName + ".uplugin")))
			{
				return true;
			}
		}

		return false;
	}

	public MotionForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",           // public headers derive from UDataAsset / reference UAnimSequence
				"DeveloperSettings",
				"EditorSubsystem",  // UMotionForgeSubsystem derives from UEditorSubsystem
				"IKRig",            // UMotionCharacter holds a UIKRetargeter, so this is public
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"HTTP",             // provider REST/GraphQL calls
				"Json",
				"JsonUtilities",
				"Projects",         // IPluginManager, for locating the shipped Blender script
				"UnrealEd",         // editor subsystem, asset creation
				"AssetTools",
				"AssetRegistry",
				"InterchangeCore",
				"InterchangeEngine",
				"LevelSequence",    // constraint keys harvested from a sequence the artist authored
				"MovieScene",       // its key times, and the tick resolution they are stored at
				"MovieSceneTracks",
				"Slate",
				"SlateCore",
				"IKRigEditor",   // UIKRetargetBatchOperation, for retargeting onto the game's rig
				"MovieSceneTools", // Sequencer's own bake, the only thing that resolves a Control Rig
				"ControlRig",      // the rig a constraint pose is authored on
				"ControlRigEditor",// FindOrCreateControlRigTrack - adding one to a sequence is editor work
			}
			);

		// The credential store talks to the Windows Credential Manager directly. Other platforms
		// fall back to the environment variable until a Keychain/libsecret backend is written.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}

		// Headers only, deliberately not a link and not a .uplugin dependency: with ForgeKeys
		// absent the module lookup returns null and this plugin carries on with its own settings
		// page. See IForgeKeysModule.
		//
		// Conditional because the reference itself has to be optional too - see IsPluginPresent.
		bool bWithForgeKeys = IsPluginPresent("ForgeKeys");

		if (bWithForgeKeys)
		{
			PrivateIncludePathModuleNames.Add("ForgeKeys");
		}

		PublicDefinitions.Add("WITH_FORGE_KEYS=" + (bWithForgeKeys ? "1" : "0"));
	}
}
