# Turn a provider's raw FBX into something Unreal can use as a game animation.
#
# Run headless:
#   blender --background --python normalize_motion.py -- \
#       --in raw.fbx --out clean.fbx --trim 0.4,4.6 --fps 30 --zero-root --ensure-root
#
# Four jobs, in order, because each depends on the last:
#
#   1. trim        Models with a minimum clip length pad the action out to fill it. The motion you
#                  asked for is somewhere in the middle, with dead air either side.
#   2. root bone   Most providers rig without one. Unreal drives root motion from a bone named
#                  "root" at the origin, so without it every clip is stuck in place forever.
#   3. zero root   Translation baked into the hips makes a clip drift. In-place clips are what
#                  motion warping needs - it owns where the character actually is.
#   4. fps         Resample so the engine is not interpolating against an odd source rate.
#
# Exit codes: 0 success, 1 bad arguments, 2 import failed, 3 processing failed, 4 export failed.

import sys
import os
import argparse

import bpy


ROOT_BONE_NAME = "root"


def parse_args():
    argv = sys.argv
    if "--" not in argv:
        print("normalize_motion: expected arguments after '--'")
        sys.exit(1)

    argv = argv[argv.index("--") + 1:]

    parser = argparse.ArgumentParser(prog="normalize_motion")
    parser.add_argument("--in", dest="input_path", required=True)
    parser.add_argument("--out", dest="output_path", required=True)
    parser.add_argument("--trim", dest="trim", default="",
                        help="start,end in seconds. Empty or 0,0 keeps everything.")
    parser.add_argument("--fps", dest="fps", type=int, default=30)
    parser.add_argument("--zero-root", dest="zero_root", action="store_true")
    parser.add_argument("--ensure-root", dest="ensure_root", action="store_true")
    return parser.parse_args(argv)


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path):
    if not os.path.isfile(path):
        print("normalize_motion: no such file: %s" % path)
        sys.exit(2)

    # automatic_bone_orientation rebuilds every bone's orientation from its children rather than
    # keeping what the file says. That is right when importing a rig to work on, and wrong for a
    # round trip that has to come out matching the skeleton it went in as - it is half of why clips
    # arrived rotated ninety degrees and twisted. The other half is the axis conversion on export.
    #
    # Set False here, but the pair has NOT been verified end to end: normalisation is disabled in
    # settings until someone re-enables it and checks a clip against the provider's web viewer.
    try:
        bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=False)
    except Exception as exc:
        print("normalize_motion: import failed: %s" % exc)
        sys.exit(2)

    armatures = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
    if not armatures:
        print("normalize_motion: no armature in %s" % path)
        sys.exit(2)

    armature = armatures[0]

    # Providers commonly name the armature object after its root bone, so the exported FBX carries
    # two scene nodes called 'root'. Unreal resolves that clash by renaming one to 'root1', which
    # then matches nothing in the target skeleton, and it silently drops the root bone's animation -
    # every other bone ends up animating against a root that never moves. Rename the object so the
    # only 'root' in the file is the bone.
    # Rename unconditionally. Providers name the armature object after the root bone they stripped,
    # so the file arrives with an object called 'root' and no bone of that name. On export that
    # object becomes a scene node called 'root', which collides with the *target* skeleton's root
    # bone inside Unreal - not with anything in this file, so testing against the file's own bone
    # names does not catch it. Unreal resolves the collision by renaming the node to 'root1' and
    # dropping its animation.
    armature.name = "MotionForgeArmature"
    armature.data.name = "MotionForgeArmatureData"

    # Providers wrap the armature in an empty named after the uploaded mesh. That empty is exported
    # as another scene node and collides in Unreal exactly the way the armature node does - the log
    # says "node 'SKM_Quinn' was renamed to 'SKM_Quinn1'". Nothing downstream needs it, so unparent
    # the armature and drop everything that is not it.
    armature.parent = None
    for obj in list(bpy.context.scene.objects):
        if obj is not armature:
            bpy.data.objects.remove(obj, do_unlink=True)

    return armature


def get_action(armature):
    if armature.animation_data and armature.animation_data.action:
        return armature.animation_data.action
    return None


def trim_action(armature, action, start_sec, end_sec, fps):
    """Keep [start, end] and slide it back to frame 0."""
    if end_sec <= start_sec:
        return

    start_frame = int(round(start_sec * fps))
    end_frame = int(round(end_sec * fps))

    for fcurve in action.fcurves:
        # Walk backwards - removing shifts the indices of everything after.
        for keyframe in reversed(fcurve.keyframe_points):
            frame = keyframe.co.x
            if frame < start_frame or frame > end_frame:
                fcurve.keyframe_points.remove(keyframe)

        for keyframe in fcurve.keyframe_points:
            keyframe.co.x -= start_frame
            keyframe.handle_left.x -= start_frame
            keyframe.handle_right.x -= start_frame

        fcurve.update()

    bpy.context.scene.frame_start = 0
    bpy.context.scene.frame_end = max(1, end_frame - start_frame)


def find_deepest_root(armature):
    """The bone everything else hangs off - usually the hips."""
    for bone in armature.data.bones:
        if bone.parent is None:
            return bone
    return None


def ensure_root_bone(armature):
    """Add a 'root' bone at the origin and reparent every top-level bone under it."""
    existing = armature.data.bones.get(ROOT_BONE_NAME)
    if existing is not None:
        return False

    # ALL of them, not just the first. A provider that strips the root bone leaves several
    # parentless bones behind - here pelvis plus ik_foot_root, ik_hand_root, interaction,
    # center_of_mass and attach - and reparenting only one leaves the rest as separate roots. The
    # clip then imports with a hierarchy the target skeleton does not share, which shows up as
    # bones rotated wrongly rather than as any error.
    top_level = [b.name for b in armature.data.bones if b.parent is None]
    if not top_level:
        return False

    print("normalize_motion: adding root above %d top-level bone(s): %s"
          % (len(top_level), ", ".join(top_level)))

    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="EDIT")

    edit_bones = armature.data.edit_bones
    root = edit_bones.new(ROOT_BONE_NAME)
    root.head = (0.0, 0.0, 0.0)
    root.tail = (0.0, 0.2, 0.0)
    root.roll = 0.0

    for name in top_level:
        child = edit_bones.get(name)
        if child is not None:
            child.parent = root
            child.use_connect = False

    bpy.ops.object.mode_set(mode="OBJECT")
    return True


def zero_root_translation(armature, action):
    """
    Strip horizontal travel from the pelvis so the clip plays in place.

    X and Y only - Z is crouching, jumping and the general rise and fall of a body, and flattening it
    makes everything look like it is gliding.
    """
    pelvis = find_deepest_root(armature)
    if pelvis is None or action is None:
        return

    # After ensure_root_bone the deepest bone is 'root' itself, so the pelvis is its first child.
    pelvis_name = pelvis.name
    if pelvis_name == ROOT_BONE_NAME and pelvis.children:
        pelvis_name = pelvis.children[0].name

    path = 'pose.bones["%s"].location' % pelvis_name

    for fcurve in action.fcurves:
        if fcurve.data_path != path:
            continue
        if fcurve.array_index not in (0, 1):
            continue

        if not fcurve.keyframe_points:
            continue

        # Offset to the first frame rather than to zero, so the character keeps whatever stance
        # offset the animation started with instead of snapping to the origin.
        base = fcurve.keyframe_points[0].co.y
        for keyframe in fcurve.keyframe_points:
            delta = keyframe.co.y - base
            keyframe.co.y -= delta
            keyframe.handle_left.y -= delta
            keyframe.handle_right.y -= delta

        fcurve.update()


def export_fbx(path, armature):
    directory = os.path.dirname(path)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)

    bpy.ops.object.select_all(action="DESELECT")
    armature.select_set(True)
    for child in armature.children:
        child.select_set(True)
    bpy.context.view_layer.objects.active = armature

    try:
        bpy.ops.export_scene.fbx(
            filepath=path,
            use_selection=True,
            object_types={"ARMATURE", "MESH"},
            add_leaf_bones=False,
            bake_anim=True,
            bake_anim_use_all_bones=True,
            bake_anim_use_nla_strips=False,
            bake_anim_use_all_actions=False,
            bake_anim_force_startend_keying=True,
            armature_nodetype="NULL",
            axis_forward="-Z",
            axis_up="Y",
        )
    except Exception as exc:
        print("normalize_motion: export failed: %s" % exc)
        sys.exit(4)


def main():
    args = parse_args()

    clear_scene()
    armature = import_fbx(args.input_path)

    try:
        bpy.context.scene.render.fps = args.fps
        action = get_action(armature)

        if action is None:
            print("normalize_motion: armature has no action - nothing to normalise")
        else:
            if args.trim:
                parts = args.trim.split(",")
                if len(parts) == 2:
                    trim_action(armature, action, float(parts[0]), float(parts[1]), args.fps)

            if args.ensure_root:
                if ensure_root_bone(armature):
                    print("normalize_motion: added '%s' bone" % ROOT_BONE_NAME)

            if args.zero_root:
                zero_root_translation(armature, action)

    except Exception as exc:
        print("normalize_motion: processing failed: %s" % exc)
        sys.exit(3)

    export_fbx(args.output_path, armature)
    print("normalize_motion: wrote %s" % args.output_path)
    sys.exit(0)


if __name__ == "__main__":
    main()
