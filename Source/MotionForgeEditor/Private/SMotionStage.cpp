#include "SMotionStage.h"

#include "MotionForgeEditorStyle.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/SOverlay.h"

#define LOCTEXT_NAMESPACE "MotionForgeStage"

namespace MotionStagePrivate
{
	/** Two bodies stand this far apart, along the axis across their front. */
	static constexpr float SideBySide = 90.f;
}

// -------------------------------------------------------------------------------------------------
// The camera
// -------------------------------------------------------------------------------------------------

FMotionStageViewportClient::FMotionStageViewportClient(
	FPreviewScene* InPreviewScene, const TWeakPtr<SMotionStageViewport>& InViewport)
	: FEditorViewportClient(nullptr, InPreviewScene, InViewport)
	, OwnerViewport(InViewport)
{
	SetRealtime(true);
	SetViewMode(VMI_Lit);
}

void FMotionStageViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// The clock first, so the world tick evaluates the pose this frame asked for.
	if (TSharedPtr<SMotionStageViewport> Owner = OwnerViewport.Pin())
	{
		Owner->AdvanceClock(DeltaSeconds);
	}

	if (PreviewScene && !GIntraFrameDebuggingGameThread)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}
}

void FMotionStageViewportClient::Frame(const FVector& Pivot, float Radius, float SubjectRadius)
{
	OrbitPivot = Pivot;
	OrbitRadius = Radius;

	// Camera on +Y looking back at the body: the Narrative skeleton's measured front is +Y, so a clip
	// is seen from the front the way it is judged.
	OrbitYaw = -90.f;
	OrbitPitch = -8.f;

	MinRadius = FMath::Max(SubjectRadius * 0.4f, 30.f);
	MaxRadius = FMath::Max(SubjectRadius * 20.f, 1000.f);

	if (bOrbitEnabled)
	{
		ApplyOrbit();
	}
	else
	{
		SetViewRotation(FRotator(-8.f, -90.f, 0.f));
		SetViewLocation(Pivot + FVector(0.f, Radius, Radius * 0.14f));
		Invalidate();
	}
}

void FMotionStageViewportClient::SetPivot(const FVector& Pivot)
{
	if (!bOrbitEnabled || OrbitPivot.Equals(Pivot, 0.5f))
	{
		return;
	}
	OrbitPivot = Pivot;
	ApplyOrbit();
}

void FMotionStageViewportClient::SetOrbitEnabled(bool bEnabled)
{
	if (bOrbitEnabled == bEnabled)
	{
		return;
	}
	bOrbitEnabled = bEnabled;

	if (bOrbitEnabled)
	{
		ApplyOrbit();
	}
}

void FMotionStageViewportClient::ApplyOrbit()
{
	// A tenth of a degree short of the poles, where the look-at has no defined roll and snaps.
	OrbitPitch = FMath::Clamp(OrbitPitch, -89.9f, 89.9f);
	OrbitYaw = FMath::Fmod(OrbitYaw, 360.f);
	OrbitRadius = FMath::Clamp(OrbitRadius, MinRadius, MaxRadius);

	const FRotator Rotation(OrbitPitch, OrbitYaw, 0.f);
	SetViewRotation(Rotation);
	SetViewLocation(OrbitPivot - Rotation.Vector() * OrbitRadius);
	Invalidate();
}

bool FMotionStageViewportClient::InputAxis(const FInputKeyEventArgs& Args)
{
	if (!bOrbitEnabled || Args.Viewport == nullptr)
	{
		return FEditorViewportClient::InputAxis(Args);
	}

	const FKey Key = Args.Key;
	const float Delta = Args.AmountDepressed;

	if (Key == EKeys::MouseWheelAxis)
	{
		OrbitRadius *= FMath::Pow(0.9f, Delta);
		ApplyOrbit();
		return true;
	}

	if (Key == EKeys::MouseX || Key == EKeys::MouseY)
	{
		// Left turns around the bodies, right moves in and out. The camera is always computed from the
		// pivot, so nothing a drag does can lose them.
		if (Args.Viewport->KeyState(EKeys::LeftMouseButton))
		{
			const float Speed = 0.35f;
			if (Key == EKeys::MouseX) { OrbitYaw += Delta * Speed; }
			else                      { OrbitPitch -= Delta * Speed; }
			ApplyOrbit();
		}
		else if (Args.Viewport->KeyState(EKeys::RightMouseButton) && Key == EKeys::MouseY)
		{
			OrbitRadius *= FMath::Pow(1.01f, Delta);
			ApplyOrbit();
		}
		return true;
	}

	return FEditorViewportClient::InputAxis(Args);
}

// -------------------------------------------------------------------------------------------------
// The viewport
// -------------------------------------------------------------------------------------------------

void SMotionStageViewport::Construct(const FArguments& InArgs)
{
	Message = InArgs._Message;

	// The animation editors' stage: studio environment, lighting that flatters nothing, and a floor -
	// on by default, because foot contact is most of what gets judged. bDirect so the shared asset
	// viewer profile every other editor reads is left alone.
	PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	PreviewScene->SetFloorVisibility(true, /*bDirect=*/true);

	SEditorViewport::Construct(SEditorViewport::FArguments());
}

SMotionStageViewport::~SMotionStageViewport()
{
	for (UDebugSkelMeshComponent*& Component : Components)
	{
		if (Component && PreviewScene.IsValid())
		{
			PreviewScene->RemoveComponent(Component);
		}
		Component = nullptr;
	}
}

TSharedRef<FEditorViewportClient> SMotionStageViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FMotionStageViewportClient>(PreviewScene.Get(), SharedThis(this));
	return ViewportClient.ToSharedRef();
}

void SMotionStageViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
	SEditorViewport::PopulateViewportOverlays(Overlay);

	Overlay->AddSlot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(24.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(14.f, 10.f))
			.Visibility_Lambda([this]()
			{
				return Message.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
			})
			[
				SNew(STextBlock)
				.Text(Message)
				.AutoWrapText(true)
				.Justification(ETextJustify::Center)
			]
		];
}

FVector SMotionStageViewport::SlotOrigin(int32 Slot) const
{
	const bool bBoth = Components[0] && Components[1];
	if (!bBoth)
	{
		return FVector::ZeroVector;
	}

	// Across the front: the camera looks along -Y, so X is left and right on screen. A on the left.
	return FVector(Slot == 0 ? MotionStagePrivate::SideBySide : -MotionStagePrivate::SideBySide, 0.f, 0.f);
}

void SMotionStageViewport::SetSlot(int32 Slot, USkeletalMesh* Mesh, UAnimSequence* Clip)
{
	if (Slot < 0 || Slot > 1 || !PreviewScene.IsValid())
	{
		return;
	}

	const bool bHadBoth = Components[0] && Components[1];

	// A fresh component per mesh rather than re-pointing one: FaceForge found a component built for one
	// mesh keeps render state shaped for its material slots and draws the next mesh through them.
	if (Components[Slot])
	{
		PreviewScene->RemoveComponent(Components[Slot]);
		Components[Slot] = nullptr;
	}
	Clips[Slot] = nullptr;

	if (Mesh)
	{
		UDebugSkelMeshComponent* Component = NewObject<UDebugSkelMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		PreviewScene->AddComponent(Component, FTransform::Identity);
		Component->SetSkeletalMesh(Mesh);
		Component->SetForcedLOD(1);

		if (Clip && Mesh->GetSkeleton() && Clip->GetSkeleton()
			&& Mesh->GetSkeleton()->IsCompatibleForEditor(Clip->GetSkeleton()))
		{
			Component->PlayAnimation(Clip, /*bLooping=*/true);

			// Posed by the shared clock, never by its own playback: two independent loops drift.
			if (UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance())
			{
				Instance->SetPlaying(false);
			}
			Clips[Slot] = Clip;
		}

		Components[Slot] = Component;
	}

	// Slots moved from centred to side by side, or back: put both where they now belong, and reframe.
	const bool bHasBoth = Components[0] && Components[1];
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (Components[Index])
		{
			Components[Index]->SetRelativeLocation(SlotOrigin(Index));
		}
	}

	if (bHadBoth != bHasBoth || Mesh)
	{
		FrameBodies();
	}

	PoseAt(Time);
}

bool SMotionStageViewport::HasSlot(int32 Slot) const
{
	return Slot >= 0 && Slot < 2 && Components[Slot] != nullptr;
}

UAnimSequence* SMotionStageViewport::GetClip(int32 Slot) const
{
	return Slot >= 0 && Slot < 2 ? Clips[Slot].Get() : nullptr;
}

float SMotionStageViewport::GetLength() const
{
	float Length = 0.f;
	for (const TWeakObjectPtr<UAnimSequence>& Clip : Clips)
	{
		if (Clip.IsValid())
		{
			Length = FMath::Max(Length, Clip->GetPlayLength());
		}
	}
	return Length;
}

void SMotionStageViewport::Play()
{
	if (GetLength() > 0.f && Time >= GetLength() - KINDA_SMALL_NUMBER)
	{
		Time = 0.f;
	}
	bPlaying = true;
}

void SMotionStageViewport::Pause()
{
	bPlaying = false;
}

void SMotionStageViewport::Stop()
{
	bPlaying = false;
	Time = 0.f;
	PoseAt(Time);
}

void SMotionStageViewport::ScrubTo(float Seconds)
{
	Time = FMath::Clamp(Seconds, 0.f, GetLength());
	PoseAt(Time);
}

void SMotionStageViewport::AdvanceClock(float DeltaSeconds)
{
	const float Length = GetLength();
	if (Length <= 0.f)
	{
		return;
	}

	if (bPlaying)
	{
		Time += DeltaSeconds;
		if (Time > Length)
		{
			// Both wrap together, whatever their own lengths - that is the point of one clock.
			Time = FMath::Fmod(Time, Length);
		}
	}

	PoseAt(Time);
}

void SMotionStageViewport::PoseAt(float Seconds)
{
	FVector FollowPivot = FVector::ZeroVector;
	int32 Followed = 0;

	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		UDebugSkelMeshComponent* Component = Components[Slot];
		UAnimSequence* Clip = Clips[Slot].Get();
		if (!Component || !Clip)
		{
			continue;
		}

		const float Local = FMath::Min(Seconds, Clip->GetPlayLength());

		if (UAnimSingleNodeInstance* Instance = Component->GetSingleNodeInstance())
		{
			Instance->SetPosition(Local, /*bFireNotifies=*/false);
		}

		// Where the clip's root has travelled to at this moment, read from the clip itself rather than
		// from the component - so the offset is the same frame as the pose, not one behind.
		FTransform Root = FTransform::Identity;
		Clip->GetBoneTransform(Root, FSkeletonPoseBoneIndex(0), FAnimExtractContext(static_cast<double>(Local)), false);
		const FVector Travel(Root.GetLocation().X, Root.GetLocation().Y, 0.f);

		if (bInPlace)
		{
			Component->SetRelativeLocation(SlotOrigin(Slot) - Travel);
		}
		else
		{
			Component->SetRelativeLocation(SlotOrigin(Slot));
			FollowPivot += SlotOrigin(Slot) + Travel;
			++Followed;
		}
	}

	if (!bInPlace && Followed > 0 && ViewportClient.IsValid())
	{
		FollowPivot /= Followed;
		FollowPivot.Z = 95.f;
		ViewportClient->SetPivot(FollowPivot);
	}
}

void SMotionStageViewport::SetInPlace(bool bEnabled)
{
	bInPlace = bEnabled;
	PoseAt(Time);

	if (bInPlace)
	{
		FrameBodies();
	}
}

void SMotionStageViewport::SetFloorVisible(bool bVisible)
{
	bFloorVisible = bVisible;
	if (PreviewScene.IsValid())
	{
		PreviewScene->SetFloorVisibility(bVisible, /*bDirect=*/true);
	}
}

void SMotionStageViewport::SetOrbitCameraEnabled(bool bEnabled)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetOrbitEnabled(bEnabled);
	}
}

bool SMotionStageViewport::IsOrbitCameraEnabled() const
{
	return ViewportClient.IsValid() && ViewportClient->IsOrbitEnabled();
}

void SMotionStageViewport::FrameBodies()
{
	if (!ViewportClient.IsValid())
	{
		return;
	}

	// From the meshes' own bounds at rest, so a clip that crouches does not reframe the shot.
	FBox Bounds(ForceInit);
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		if (Components[Slot] && Components[Slot]->GetSkeletalMeshAsset())
		{
			const FBoxSphereBounds MeshBounds = Components[Slot]->GetSkeletalMeshAsset()->GetBounds();
			Bounds += MeshBounds.GetBox().ShiftBy(SlotOrigin(Slot));
		}
	}

	if (!Bounds.IsValid)
	{
		Bounds = FBox(FVector(-50.f, -50.f, 0.f), FVector(50.f, 50.f, 180.f));
	}

	const float Radius = Bounds.GetExtent().Size();
	ViewportClient->Frame(Bounds.GetCenter(), FMath::Max(Radius * 2.4f, 200.f), Radius);

	// A direct floor visibility does not survive every profile refresh; re-assert on framing.
	PreviewScene->SetFloorVisibility(bFloorVisible, /*bDirect=*/true);
}

// -------------------------------------------------------------------------------------------------
// The stage and its transport
// -------------------------------------------------------------------------------------------------

void SMotionStage::Construct(const FArguments& InArgs)
{
	TAttribute<FText> Stats = InArgs._Stats;

	auto Toggle = [](const FText& Label, const FText& Tip, TFunction<bool()> Get, TFunction<void(bool)> Set)
	{
		return SNew(SCheckBox)
			.ToolTipText(Tip)
			.IsChecked_Lambda([Get]() { return Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Set](ECheckBoxState State) { Set(State == ECheckBoxState::Checked); })
			[
				SNew(STextBlock).Text(Label)
			];
	};

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(Viewport, SMotionStageViewport).Message(InArgs._Message)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("PlayPauseTip", "Play or pause. Both takes run on one clock, so they are always compared at the same moment."))
				.OnClicked_Lambda([this]()
				{
					if (Viewport->IsPlaying()) { Viewport->Pause(); } else { Viewport->Play(); }
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image_Lambda([this]()
					{
						return FAppStyle::GetBrush(Viewport.IsValid() && Viewport->IsPlaying() ? "Animation.Pause" : "Animation.Forward");
					})
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("StopTip", "Stop at the first frame."))
				.OnClicked_Lambda([this]() { Viewport->Stop(); return FReply::Handled(); })
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Animation.Stop")).ColorAndOpacity(FSlateColor::UseForeground())
				]
			]

			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8.f, 0.f)
			[
				SNew(SSlider)
				.Value_Lambda([this]()
				{
					const float Length = Viewport.IsValid() ? Viewport->GetLength() : 0.f;
					return Length > 0.f ? Viewport->GetTime() / Length : 0.f;
				})
				.OnValueChanged_Lambda([this](float Value)
				{
					Viewport->Pause();
					Viewport->ScrubTo(Value * Viewport->GetLength());
				})
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
			[
				SNew(SBox).MinDesiredWidth(96.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return Viewport.IsValid() && Viewport->GetLength() > 0.f
							? FText::FromString(FString::Printf(TEXT("%.2f / %.2f s"), Viewport->GetTime(), Viewport->GetLength()))
							: FText::GetEmpty();
					})
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				Toggle(LOCTEXT("InPlace", "In place"),
					LOCTEXT("InPlaceTip", "Hold the bodies where they stand, cancelling their travel. Off, they walk and the camera follows."),
					[this]() { return Viewport.IsValid() && Viewport->IsInPlace(); },
					[this](bool b) { Viewport->SetInPlace(b); })
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				Toggle(LOCTEXT("Floor", "Floor"),
					LOCTEXT("FloorTip", "Show the floor. Foot contact is most of what is judged, so it is on by default."),
					[this]() { return Viewport.IsValid() && Viewport->IsFloorVisible(); },
					[this](bool b) { Viewport->SetFloorVisible(b); })
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				Toggle(LOCTEXT("Orbit", "Orbit"),
					LOCTEXT("OrbitTip", "Left drag turns around the bodies, the wheel zooms, right drag moves in and out. Off gives the ordinary editor camera."),
					[this]() { return Viewport.IsValid() && Viewport->IsOrbitCameraEnabled(); },
					[this](bool b) { Viewport->SetOrbitCameraEnabled(b); })
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(Stats)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]
	];
}

#undef LOCTEXT_NAMESPACE
