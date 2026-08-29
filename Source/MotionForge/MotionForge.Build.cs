using UnrealBuildTool;

public class MotionForge : ModuleRules
{
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
	}
}
