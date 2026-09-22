// The stage a take is judged on: the character, playing it, on a floor - and a second take beside it.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"

class UAnimSequence;
class UDebugSkelMeshComponent;
class USkeletalMesh;
class SMotionStageViewport;

/**
 * The stage's camera and clock. An orbit around the bodies, driven by hand the way FaceForge's and
 * MeshForge's stages drive theirs - the base class's orbit fights a hand-set pivot - with Orbit off
 * falling back to the stock editor camera. It also ticks the preview world, which nothing else does.
 */
class FMotionStageViewportClient : public FEditorViewportClient
{
public:

	FMotionStageViewportClient(FPreviewScene* InPreviewScene, const TWeakPtr<SMotionStageViewport>& InViewport);

	virtual void Tick(float DeltaSeconds) override;
	virtual bool InputAxis(const FInputKeyEventArgs& Args) override;

	/** Aim at the bodies: pivot, distance, and a zoom range from their size. The front is +Y. */
	void Frame(const FVector& Pivot, float Radius, float SubjectRadius);

	/** Move the pivot without changing distance or angle - following a travelling clip. */
	void SetPivot(const FVector& Pivot);

	void SetOrbitEnabled(bool bEnabled);
	bool IsOrbitEnabled() const { return bOrbitEnabled; }

private:

	void ApplyOrbit();

	TWeakPtr<SMotionStageViewport> OwnerViewport;

	bool bOrbitEnabled = true;
	FVector OrbitPivot = FVector::ZeroVector;
	float OrbitRadius = 400.f;
	float OrbitYaw = -90.f;
	float OrbitPitch = -8.f;
	float MinRadius = 50.f;
	float MaxRadius = 5000.f;
};

/**
 * The viewport half: a preview scene with up to two bodies, driven by one clock.
 *
 * One clock rather than two looping animations, because two loops drift apart within seconds and a
 * comparison of two different moments is not a comparison. Each body is posed at the same time every
 * frame; the shorter clip holds its last frame until the longer one wraps.
 */
class SMotionStageViewport : public SEditorViewport
{
public:

	SLATE_BEGIN_ARGS(SMotionStageViewport) {}
		/** Shown over the viewport: what is loading, or why there is nothing to show. */
		SLATE_ATTRIBUTE(FText, Message)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMotionStageViewport() override;

	/** Put a mesh and a clip in slot 0 (A) or 1 (B). A null clip rests the mesh; a null mesh empties the slot. */
	void SetSlot(int32 Slot, USkeletalMesh* Mesh, UAnimSequence* Clip);

	bool HasSlot(int32 Slot) const;
	UAnimSequence* GetClip(int32 Slot) const;

	// The clock.
	void Play();
	void Pause();
	void Stop();
	void ScrubTo(float Seconds);
	bool IsPlaying() const { return bPlaying; }
	float GetTime() const { return Time; }
	float GetLength() const;

	/** Hold the bodies where they stand, cancelling their travel, or let them walk and follow. */
	void SetInPlace(bool bEnabled);
	bool IsInPlace() const { return bInPlace; }

	void SetFloorVisible(bool bVisible);
	bool IsFloorVisible() const { return bFloorVisible; }

	void SetOrbitCameraEnabled(bool bEnabled);
	bool IsOrbitCameraEnabled() const;

	/** Called by the client every frame: advance the clock and pose both bodies. */
	void AdvanceClock(float DeltaSeconds);

	/** Aim the camera at whatever is on stage now. */
	void FrameBodies();

protected:

	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
	virtual void PopulateViewportOverlays(TSharedRef<SOverlay> Overlay) override;

private:

	void PoseAt(float Seconds);

	/** Where a slot's body stands: side by side when both are filled, centred when one is. */
	FVector SlotOrigin(int32 Slot) const;

	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FMotionStageViewportClient> ViewportClient;

	UDebugSkelMeshComponent* Components[2] = { nullptr, nullptr };
	TWeakObjectPtr<UAnimSequence> Clips[2];

	TAttribute<FText> Message;

	bool bPlaying = true;
	float Time = 0.f;
	bool bInPlace = true;
	bool bFloorVisible = true;
};

/**
 * The stage with its transport under it: play, pause, stop, a scrubber, the time, and the switches
 * for floor, in place and orbit. What the definition window puts over its take list.
 */
class SMotionStage : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SMotionStage) {}
		SLATE_ATTRIBUTE(FText, Message)
		/** The line under the transport: each slot's take, frames, length and travel. */
		SLATE_ATTRIBUTE(FText, Stats)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	TSharedPtr<SMotionStageViewport> GetViewport() const { return Viewport; }

private:

	TSharedPtr<SMotionStageViewport> Viewport;
};
