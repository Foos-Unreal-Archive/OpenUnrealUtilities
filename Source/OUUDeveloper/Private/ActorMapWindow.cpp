// Copyright (c) 2022 Jonas Reich

#include "CoreMinimal.h"

#include "AbilitySystemComponent.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTagContainer.h"
#include "GameplayTags/GameplayTagQueryParser.h"
#include "LogOpenUnrealUtilities.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/RegexUtils.h"
#include "Tickable.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SWindow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#if WITH_EDITOR
	#include "LevelEditorViewport.h"
#endif

/**
 * Hard coded editor colors, do not update with editor style config,
 * but I did not want to deal with that at this time..
 */
FSlateColorBrush DarkGrey(FColor(6, 6, 6, 255));
FSlateColorBrush MediumGrey(FColor(13, 13, 13, 255));
FSlateColorBrush White(FColor::White);

/**
 * Default colors for actor overlays.
 * The list contains the most extreme saturated colors only to make the stand out as much as possible.
 */
TArray<FColor> DefaultColors =
	{FColorList::Red, FColorList::Green, FColorList::Blue, FColorList::Magenta, FColorList::Cyan, FColorList::Yellow};

/**
 * Utility class that allows querying actors matching certain filter conditions.
 * Conditions are cumulative: All conditions must match for an actor to be included.
 */
class FActorQuery
{
public:
	struct FResult
	{
	public:
		TArray<AActor*> Actors;
	};

	/** Color in which the query results are displayed. */
	FColor QueryColor;

	/** String that must be contained within the actor name. Ignored if empty. */
	FString NameFilter;

	/** Regex pattern that actor names must match. Ignored if empty. */
	FString NameRegexPattern;

	/**
	 * Exact name of the actor class or any of its parent classes.
	 * The name must be an exact match, e.g. StaticMeshActor for AStaticMeshActors
	 */
	FString ActorClassName;

	/**
	 * If this is valid, actors are expected to have a gameplay ability system component
	 * of which the owned gameplay tags are compared with this query.
	 */
	FGameplayTagQuery ActorTagQuery;

	/**
	 * Cached result from executing the query via ExecuteAndCacheQuery()
	 */
	FResult CachedQueryResult;

	bool MatchesActor(const AActor* Actor) const
	{
		if (!IsValid(Actor))
			return false;

		bool bAtLeastOneFilterActive = false;

		const FString ActorName = Actor->GetName();
		if (!NameFilter.IsEmpty())
		{
			bAtLeastOneFilterActive = true;
			if (!ActorName.Contains(NameFilter))
				return false;
		}

		if (!NameRegexPattern.IsEmpty())
		{
			bAtLeastOneFilterActive = true;
			if (!OUU::Runtime::RegexUtils::MatchesRegex(NameRegexPattern, ActorName))
				return false;
		}

		// Perform this check last, because it's the most expensive
		if (!ActorClassName.IsEmpty())
		{
			bAtLeastOneFilterActive = true;
			if (!MatchesActorClassSearchString(Actor))
				return false;
		}

		if (!ActorTagQuery.IsEmpty())
		{
			bAtLeastOneFilterActive = true;
			if (UAbilitySystemComponent* AbilitySystemComponent =
					Actor->FindComponentByClass<UAbilitySystemComponent>())
			{
				FGameplayTagContainer OwnedTags;
				AbilitySystemComponent->GetOwnedGameplayTags(OUT OwnedTags);
				if (!ActorTagQuery.Matches(OwnedTags))
					return false;
			}
			else
			{
				return false;
			}
		}

		return bAtLeastOneFilterActive;
	}

	bool MatchesActorClassSearchString(const AActor* Actor) const
	{
		if (!IsValid(Actor))
			return false;

		auto* Class = Actor->GetClass();
		// Iterate through all parent classes to find a match
		while (Class != UStruct::StaticClass() && Class != UClass::StaticClass() && Class != AActor::StaticClass())
		{
			if (Class->GetName().Equals(ActorClassName, ESearchCase::IgnoreCase))
				return true;
			Class = Class->GetSuperClass();
		}
		return false;
	}

	FResult ExecuteQuery(UWorld* World) const
	{
		FResult ResultList;
		if (!IsValid(World))
			return ResultList;

		for (AActor* Actor : TActorRange<AActor>(World))
		{
			if (!IsValid(Actor))
				continue;

			if (MatchesActor(Actor))
			{
				ResultList.Actors.Add(Actor);
			}
		}
		return ResultList;
	}

	FResult& ExecuteAndCacheQuery(UWorld* World)
	{
		CachedQueryResult = ExecuteQuery(World);
		return CachedQueryResult;
	}
};

/** Utility structure for a SSplitter column */
struct FColumnSizeData
{
	TAttribute<float> LeftColumnWidth;
	TAttribute<float> RightColumnWidth;
	SSplitter::FOnSlotResized OnWidthChanged;

	void SetColumnWidth(float InWidth) const { OnWidthChanged.ExecuteIfBound(InWidth); }

	TSharedRef<SWidget> DetailsSplitter(const FText& Label, const FText& Tooltip, TSharedRef<SWidget> RightWidget) const
	{
		// clang-format off
		return SNew(SSplitter)
			+ SSplitter::Slot()
				.SizeRule(SSplitter::ESizeRule::FractionOfParent)
				.Value(LeftColumnWidth)
				.OnSlotResized(OnWidthChanged)
			[
				SNew(STextBlock)
				.Text(Label)
				.ToolTipText(Tooltip)
			]
			+ SSplitter::Slot()
				.SizeRule(SSplitter::ESizeRule::FractionOfParent)
				.Value(RightColumnWidth)
				.OnSlotResized(OnWidthChanged)
			[
				RightWidget
			];
		// clang-format on
	}
};

/**
 * The actual overlay widget that paints actor locations, names, etc.
 * on-top of the scene capture in the background.
 */
class SActorLocationOverlay : public SLeafWidget
{
	using Super = SLeafWidget;

	SLATE_BEGIN_ARGS(SActorLocationOverlay)
		{
		}

		SLATE_ATTRIBUTE(const TArray<TSharedPtr<FActorQuery>>*, ActorQueries);
		SLATE_ATTRIBUTE(FVector, ReferencePosition);
		SLATE_ATTRIBUTE(float, MapSize);
	SLATE_END_ARGS()

	TAttribute<const TArray<TSharedPtr<FActorQuery>>*> ActorQueries;
	TAttribute<FVector> ReferencePosition = FVector::ZeroVector;
	TAttribute<float> MapSize = 0.f;

	void Construct(const FArguments& InArgs)
	{
		ActorQueries = InArgs._ActorQueries;
		ReferencePosition = InArgs._ReferencePosition;
		MapSize = InArgs._MapSize;
	}

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		// No desired size. Always use maximum available space
		return FVector2D::ZeroVector;
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		if (ActorQueries.Get() == nullptr)
			return LayerId;

		// Used to track the layer ID we will return.
		int32 RetLayerId = LayerId;

		const bool bEnabled = ShouldBeEnabled(bParentEnabled);
		const ESlateDrawEffect DrawEffects = bEnabled ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;

		const FLinearColor ColorAndOpacitySRGB = InWidgetStyle.GetColorAndOpacityTint();

		const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
		const FVector2D LocalCenter = LocalSize / 2;
		const float MaxComponent = LocalCenter.GetMin();
		const FVector2D CenterOffset = FVector2D(-MaxComponent, -MaxComponent);
		const FVector2D Position = LocalCenter + CenterOffset;
		const FVector2D Size = FVector2D(MaxComponent * 2.f, MaxComponent * 2.f);

		const float MapSizeActual = MapSize.Get();
		const FVector HalfMapSizeVector = FVector(MapSizeActual / 2.f, MapSizeActual / 2.f, 0);
		const FVector TopLeftCorner = ReferencePosition.Get() - HalfMapSizeVector;
		FBox BBox(TopLeftCorner, ReferencePosition.Get() + HalfMapSizeVector);
		auto ActualActorQueries = *ActorQueries.Get();
		for (auto Query : ActualActorQueries)
		{
			if (!Query.IsValid())
				continue;

			for (AActor* Actor : Query->CachedQueryResult.Actors)
			{
				if (!IsValid(Actor))
					continue;

				const FVector WorldLocation = Actor->GetActorLocation();
				if (!BBox.IsInsideXY(WorldLocation))
					continue;

				const FVector RelativeLocation3D = WorldLocation - TopLeftCorner;
				const FVector2D RelativeLocation2D{RelativeLocation3D.X, RelativeLocation3D.Y};
				const FVector2D RelativeLocation2D_Normalized = RelativeLocation2D / MapSizeActual;
				// Need to remap coordinates from world space when looking down (x is up, y is right) to UI space (x is
				// right, y is down)
				const FVector2D WidgetSpaceLocationNormalized{
					RelativeLocation2D_Normalized.Y,
					1.f - RelativeLocation2D_Normalized.X};
				const FVector2D WidgetSpaceLocation = Position + WidgetSpaceLocationNormalized * Size;

				const float MarkerSize = 6.f;
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					RetLayerId++,
					AllottedGeometry
						.ToPaintGeometry(WidgetSpaceLocation - (MarkerSize / 2.f), FVector2D(MarkerSize, MarkerSize)),
					&White,
					DrawEffects,
					Query->QueryColor);

				auto& Style = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("SmallText");
				const FSlateFontInfo FontInfo = Style.Font;
				FSlateDrawElement::MakeText(
					OutDrawElements,
					RetLayerId++,
					AllottedGeometry.ToPaintGeometry(
						WidgetSpaceLocation - (MarkerSize / 2.f) + FVector2D(0, MarkerSize),
						FVector2D(MarkerSize, MarkerSize)),
					FText::FromString(Actor->GetName()),
					FontInfo,
					DrawEffects,
					Query->QueryColor);
			}
		}

		return RetLayerId - 1;
	}
};

/** Slate widget for entries of a list of actor queries. */
class SActorQueryRow : public STableRow<TSharedPtr<FActorQuery>>
{
public:
	SLATE_BEGIN_ARGS(SActorQueryRow)
		{
		}

		SLATE_ARGUMENT(FColumnSizeData*, ColumnSizeData)
	SLATE_END_ARGS()

	void Construct(
		const FArguments& InArgs,
		TSharedRef<STableViewBase> InOwnerTableView,
		TSharedPtr<FActorQuery>& InActorQuery)
	{
		ActorQuery = InActorQuery;
		ensure(ActorQuery.IsValid());

		ColumnSizeData = InArgs._ColumnSizeData;

		// clang-format off
		STableRow<TSharedPtr<FActorQuery>>::Construct(
			STableRow<TSharedPtr<FActorQuery>>::FArguments()
			.Content()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				[
					SNew(SColorBlock)
					.Color(ActorQuery->QueryColor)
				]
				+ SVerticalBox::Slot()
				[
					ColumnSizeData->DetailsSplitter(
						INVTEXT("Name Filter"),
						INVTEXT("Name string that must be contained within the actor names, for the actor to be "
							"included in the query."),
						SNew(SEditableTextBox)
						.Text(this, &SActorQueryRow::GetNameFilter_Text)
						.HintText(INVTEXT("<empty>"))
						.OnTextCommitted(this, &SActorQueryRow::SetNameFilter_Text)
					)
				]
				+ SVerticalBox::Slot()
				[
					ColumnSizeData->DetailsSplitter(
						INVTEXT("Name Regex Pattern"),
						INVTEXT("Regular expression pattern that must match to actor names, for the actor to be "
							"included in the query."),
						SNew(SEditableTextBox)
						.Text(this, &SActorQueryRow::GetNameRegexPattern_Text)
						.HintText(INVTEXT("<empty>"))
						.OnTextCommitted(this, &SActorQueryRow::SetNameRegexPattern_Text)
					)
				]
				+ SVerticalBox::Slot()
				[
					ColumnSizeData->DetailsSplitter(
						INVTEXT("Class Filter"),
						INVTEXT("Name string that must be contained within the actors class name or any of its "
							"super classes, for the actor to be included in the query."),
						SNew(SEditableTextBox)
						.Text(this, &SActorQueryRow::GetActorClassSearchString_Text)
						.HintText(INVTEXT("<empty>"))
						.OnTextCommitted(this, &SActorQueryRow::SetActorClassSearchString_Text)
					)
				]
				+ SVerticalBox::Slot()
				[
					ColumnSizeData->DetailsSplitter(
						INVTEXT("Gameplay Tag Query"),
						INVTEXT("Gameplay tag query. Must use the "),
						SNew(SEditableTextBox)
						.Text(this, &SActorQueryRow::GetGameplayTagQueryString_Text)
						.HintText(INVTEXT("<empty>"))
						.OnTextCommitted(this, &SActorQueryRow::SetGameplayTagQueryString_Text)
					)
				]
			], InOwnerTableView);
		// clang-format on
	}

private:
	TSharedPtr<FActorQuery> ActorQuery;
	FColumnSizeData* ColumnSizeData = nullptr;
	FString GameplayTagQueryString;

	FText GetNameFilter_Text() const
	{
		return ActorQuery.IsValid() ? FText::FromString(ActorQuery->NameFilter) : INVTEXT("<invalid>");
	}
	void SetNameFilter_Text(const FText& Text, ETextCommit::Type) const
	{
		if (ActorQuery.IsValid())
		{
			ActorQuery->NameFilter = Text.ToString();
		}
	}
	FText GetNameRegexPattern_Text() const
	{
		return ActorQuery.IsValid() ? FText::FromString(ActorQuery->NameRegexPattern) : INVTEXT("<invalid>");
	}
	void SetNameRegexPattern_Text(const FText& Text, ETextCommit::Type) const
	{
		if (ActorQuery.IsValid())
		{
			ActorQuery->NameRegexPattern = Text.ToString();
		}
	}
	FText GetActorClassSearchString_Text() const
	{
		return ActorQuery.IsValid() ? FText::FromString(ActorQuery->ActorClassName) : INVTEXT("<invalid>");
	}
	void SetActorClassSearchString_Text(const FText& Text, ETextCommit::Type) const
	{
		if (ActorQuery.IsValid())
		{
			ActorQuery->ActorClassName = Text.ToString();
		}
	}
	FText GetGameplayTagQueryString_Text() const
	{
		return ActorQuery.IsValid() ? FText::FromString(GameplayTagQueryString) : INVTEXT("<invalid>");
	}
	void SetGameplayTagQueryString_Text(const FText& Text, ETextCommit::Type)
	{
		FString TextAsString = Text.ToString();
		if (TextAsString != GameplayTagQueryString)
		{
			GameplayTagQueryString = TextAsString;
			if (ActorQuery.IsValid())
			{
				ActorQuery->ActorTagQuery = FGameplayTagQueryParser::ParseQuery(GameplayTagQueryString);
			}
		}
	}
};

/**
 * The data and core functionality of the actor map window:
 * FActorMap takes care of creating objects, widgets and performing actor queries in tick.
 */
class FActorMap : public TSharedFromThis<FActorMap>, public FTickableGameObject
{
public:
	~FActorMap()
	{
		if (SceneCaptureActor.IsValid())
		{
			SceneCaptureActor->Destroy();
			SceneCaptureActor.Reset();
		}

		MapBrush.SetResourceObject(nullptr);
	}

	// - FTickableGameObject
	virtual bool IsTickableInEditor() const override { return true; }
	virtual bool IsTickableWhenPaused() const override { return true; }

	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FActorMap, STATGROUP_Tickables); }

	virtual void Tick(float DeltaTime) override
	{
		AccumulatedDeltaTime += DeltaTime;

		if (!ensure(TickRate > 0))
			return;

		if (AccumulatedDeltaTime >= TickRate)
		{
			while (AccumulatedDeltaTime >= TickRate)
			{
				AccumulatedDeltaTime -= TickRate;
			}

			if (SceneCaptureActor.IsValid())
			{
				LocalCameraLocation = FVector::ZeroVector;
				if (bShouldFollowCamera)
				{
					bool bSetLocalCameraLocation = false;
					if (auto* LocalPlayerController = TargetWorld->GetFirstPlayerController())
					{
#if UE_VERSION_OLDER_THAN(5, 0, 0)
						if (auto* Camera = LocalPlayerController->PlayerCameraManager)
#else
						if (APlayerCameraManager* Camera = LocalPlayerController->PlayerCameraManager.Get())
#endif
						{
							bSetLocalCameraLocation = true;
							LocalCameraLocation = Camera->GetCameraLocation();
						}
					}
					if (!bSetLocalCameraLocation)
					{
#if WITH_EDITOR
						for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
						{
							if (LevelVC && LevelVC->IsPerspective())
							{
								LocalCameraLocation = LevelVC->GetViewLocation();
							}
						}
#endif
					}
				}

				SceneCaptureActor->SetActorLocation(LocalCameraLocation + ReferencePosition);
				SceneCaptureActor->GetCaptureComponent2D()->CaptureScene();
			}

			// Update the actor queries
			UWorld* World = TargetWorld.Get();
			if (!IsValid(World))
				return;

			for (auto Query : ActorQueries)
			{
				if (!Query.IsValid())
					continue;

				Query->ExecuteAndCacheQuery(World);
			}
		}
	}

	// --

	/** Separate initializer outside of constructor, so shared pointer from this works as expected */
	void Initialize(UWorld* InTargetWorld)
	{
		check(IsValid(InTargetWorld));

		TargetWorld = InTargetWorld;

		// Look down
		const FRotator Direction(-90, 0, 0);
		SceneCaptureActor = TargetWorld->SpawnActor<ASceneCapture2D>(ReferencePosition, Direction);
		auto* CaptureComponent = SceneCaptureActor->GetCaptureComponent2D();

		CaptureComponent->bCaptureEveryFrame = false;
		CaptureComponent->bCaptureOnMovement = false;
		CaptureComponent->ProjectionType = ECameraProjectionMode::Orthographic;
		CaptureComponent->OrthoWidth = OrthoWidth;
		CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
		CaptureComponent->bEnableClipPlane = false;

		const FName TargetName = MakeUniqueObjectName(
			SceneCaptureActor.Get(),
			UTextureRenderTarget2D::StaticClass(),
			TEXT("SceneCaptureTextureTarget"));
		CaptureComponent->TextureTarget = NewObject<UTextureRenderTarget2D>(SceneCaptureActor.Get(), TargetName);
		CaptureComponent->TextureTarget->InitCustomFormat(CaptureSize, CaptureSize, PF_FloatRGB, false);
		CaptureComponent->TextureTarget->ClearColor = FLinearColor::Black;
		CaptureComponent->TextureTarget->TargetGamma = 2.2f;

		CaptureComponent->CaptureScene();

		MapBrush = FSlateBrush();
		MapBrush.SetResourceObject(CaptureComponent->TextureTarget);
		MapBrush.ImageSize.X = CaptureComponent->TextureTarget->GetResource()->GetSizeX();
		MapBrush.ImageSize.Y = CaptureComponent->TextureTarget->GetResource()->GetSizeY();

		MainColumns.LeftColumnWidth = TAttribute<float>(this, &FActorMap::OnGetDetailsWidth);
		MainColumns.RightColumnWidth = TAttribute<float>(this, &FActorMap::OnGetMapWidth);
		MainColumns.OnWidthChanged = SSplitter::FOnSlotResized::CreateSP(this, &FActorMap::OnSetMapWidth);

		DetailsColumns.LeftColumnWidth = TAttribute<float>(this, &FActorMap::OnGetLeftDetailsColumnWidth);
		DetailsColumns.RightColumnWidth = TAttribute<float>(this, &FActorMap::OnGetRightDetailsColumnWidth);
		DetailsColumns.OnWidthChanged = SSplitter::FOnSlotResized::CreateSP(this, &FActorMap::OnSetDetailsColumnWidth);
	}

	UWorld* GetTargetWorld() const { return TargetWorld.Get(); }

	TSharedRef<SWidget> TakeWidget()
	{
		// clang-format off
		return SNew(SBorder).BorderImage(&DarkGrey).Content()[SNew(SSplitter)
			+ SSplitter::Slot()
			  .SizeRule(SSplitter::ESizeRule::FractionOfParent)
			  .Value(MainColumns.LeftColumnWidth)
			  .OnSlotResized(MainColumns.OnWidthChanged)
			[
				SNew(SBorder)
				.Padding(2.f)
				.BorderImage(&MediumGrey)
				.Content()
				[
					SNew(SBox)
					.Padding(2.f)
					.Content()
					[
						DetailsWidget()
					]
				]
			]
			+ SSplitter::Slot()
			  .SizeRule(SSplitter::ESizeRule::FractionOfParent)
			  .Value(MainColumns.RightColumnWidth)
			  .OnSlotResized(MainColumns.OnWidthChanged)
			[
				SNew(SBorder)
				.Padding(2.f)
				.BorderImage(&MediumGrey)
				.Content()
				[
					SNew(SBox)
					.Padding(2.f)
					.Content()
					[
						MapWidget()
					]
				]
			]
		];
		// clang-format on
	}

	FText GetTitleText() const
	{
		return FText::FromString(FString::Printf(
			TEXT("OUU Actor Map (%s) [%s]"),
			*TargetWorld->GetName(),
			GetWorldTypeString(TargetWorld->WorldType)));
	}

protected:
	TWeakObjectPtr<UWorld> TargetWorld = nullptr;
	TWeakObjectPtr<ASceneCapture2D> SceneCaptureActor = nullptr;
	FSlateBrush MapBrush;
	float AccumulatedDeltaTime = 0.f;

	static const TCHAR* GetWorldTypeString(EWorldType::Type Type)
	{
		switch (Type)
		{
		case EWorldType::None: return TEXT("None");
		case EWorldType::Game: return TEXT("Game");
		case EWorldType::Editor: return TEXT("Editor");
		case EWorldType::PIE: return TEXT("PIE");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		case EWorldType::GamePreview: return TEXT("GamePreview");
		case EWorldType::GameRPC: return TEXT("GameRPC");
		case EWorldType::Inactive: return TEXT("Inactive");
		default: return TEXT("Unknown");
		}
	}

	//------------------------
	// Property accessors
	//------------------------

	float OrthoWidth = 10000.f;
	float CaptureSize = 2048.f;
	TOptional<float> OnGetOptionalOrthoWidth() const { return OrthoWidth; }
	float GetOrthoWidth() const { return OrthoWidth; }

	void OnSetOrthoWidth(float InOrthoSize)
	{
		OrthoWidth = InOrthoSize;
		if (SceneCaptureActor.IsValid())
		{
			SceneCaptureActor->GetCaptureComponent2D()->OrthoWidth = OrthoWidth;
		}
	}

	FColumnSizeData MainColumns;
	float MapColumnWidthFactor = 0.75f;
	float OnGetDetailsWidth() const { return 1.0f - MapColumnWidthFactor; }
	float OnGetMapWidth() const { return MapColumnWidthFactor; }
	void OnSetMapWidth(float InWidth) { MapColumnWidthFactor = InWidth; }

	FColumnSizeData DetailsColumns;
	float DetailsColumnWidthFactor = 0.6f;
	float OnGetLeftDetailsColumnWidth() const { return 1.0f - DetailsColumnWidthFactor; }
	float OnGetRightDetailsColumnWidth() const { return DetailsColumnWidthFactor; }
	void OnSetDetailsColumnWidth(float InWidth) { DetailsColumnWidthFactor = InWidth; }

	FVector ReferencePosition = FVector(0, 0, 10000);
	TOptional<float> GetPositionX() const { return ReferencePosition.X; }
	TOptional<float> GetPositionY() const { return ReferencePosition.Y; }
	TOptional<float> GetPositionZ() const { return ReferencePosition.Z; }

	void OnSetPosition(float NewValue, ETextCommit::Type CommitInfo, int32 Axis)
	{
		ReferencePosition.Component(Axis) = NewValue;
	}

	FVector LocalCameraLocation = FVector::ZeroVector;
	FVector GetReferencePosition() const { return ReferencePosition + LocalCameraLocation; }

	bool bShouldFollowCamera = false;
	ECheckBoxState GetFollowCameraCheckBoxState() const
	{
		return bShouldFollowCamera ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}
	void OnFollowCameraCheckBoxStateChanged(ECheckBoxState CheckBoxState)
	{
		bShouldFollowCamera = CheckBoxState == ECheckBoxState::Checked;
	}

	float TickRate = 0.1f;
	TOptional<float> OnGetOptionalTickRate() const { return TickRate; }
	float GetTickRate() const { return TickRate; }
	void OnSetTickRate(float InTickRate) { TickRate = InTickRate; }

	TArray<TSharedPtr<FActorQuery>> ActorQueries;

	void AddActorQuery()
	{
		const int32 NewIndex = ActorQueries.Add(MakeShared<FActorQuery>());
		ActorQueries[NewIndex]->QueryColor = DefaultColors[NewIndex % DefaultColors.Num()];
		if (ActorQueryListWidget.IsValid())
		{
			ActorQueryListWidget->RebuildList();
		}
	}

	void RemoveLastActorQuery()
	{
		if (ActorQueries.Num() > 0)
		{
			ActorQueries.Pop();
		}
		if (ActorQueryListWidget.IsValid())
		{
			ActorQueryListWidget->RebuildList();
		}
	}

	//------------------------
	// Cached Widgets
	//------------------------
	TSharedPtr<SListView<TSharedPtr<FActorQuery>>> ActorQueryListWidget;

	//------------------------
	// Widget builder functions
	//------------------------
	TSharedRef<SWidget> DetailsWidget()
	{
		// clang-format off
		return SNew(SBox)
		.MinDesiredWidth(200.f)
		.Content()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				DetailsColumns.DetailsSplitter(
					INVTEXT("Ortho Width"),
					INVTEXT("Orthographic height and width of the actor map. Always assumes a square map / render target background"),
					SNew(SNumericEntryBox<float>)
					.Value(this, &FActorMap::OnGetOptionalOrthoWidth)
					.OnValueChanged(this, &FActorMap::OnSetOrthoWidth)
				)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				DetailsColumns.DetailsSplitter(
					INVTEXT("Origin"),
					INVTEXT("The position from which the render capture of the world is made"),
					SNew(SVectorInputBox)
					.X(this, &FActorMap::GetPositionX)
					.Y(this, &FActorMap::GetPositionY)
					.Z(this, &FActorMap::GetPositionZ)
					.AllowSpin(true)
					.OnXCommitted(this, &FActorMap::OnSetPosition, 0)
					.OnYCommitted(this, &FActorMap::OnSetPosition, 1)
					.OnZCommitted(this, &FActorMap::OnSetPosition, 2)
				)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				DetailsColumns.DetailsSplitter(
					INVTEXT("Follow Camera"),
					INVTEXT("If to apply the Origin relative to the location of the currently possessed camera"),
					SNew(SCheckBox)
					.IsChecked(this, &FActorMap::GetFollowCameraCheckBoxState)
					.OnCheckStateChanged(this, &FActorMap::OnFollowCameraCheckBoxStateChanged)
					)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				DetailsColumns.DetailsSplitter(
					INVTEXT("Tick Rate"),
					INVTEXT("Time between two map updates in seconds"),
					SNew(SNumericEntryBox<float>)
					.Value(this, &FActorMap::OnGetOptionalTickRate)
					.OnValueChanged(this, &FActorMap::OnSetTickRate)
				)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSpacer)
				.Size(FVector2D{0.f, 20.f})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				[
					SNew(SButton)
					.Text(INVTEXT("Add Actor Query"))
					.OnPressed(this, &FActorMap::AddActorQuery)
				]
				+ SHorizontalBox::Slot()
				[
					SNew(SButton)
					.Text(INVTEXT("Remove Last Actor Query"))
					.OnPressed(this, &FActorMap::RemoveLastActorQuery)
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SAssignNew(ActorQueryListWidget, SListView<TSharedPtr<FActorQuery>>)
				.ListItemsSource(&ActorQueries)
				.OnGenerateRow(this, &FActorMap::OnGenerateActorQueryRow)
			]
		];
		// clang-format on
	}

	TSharedRef<ITableRow> OnGenerateActorQueryRow(
		TSharedPtr<FActorQuery> InItem,
		const TSharedRef<STableViewBase>& OwnerTable)
	{
		return SNew(SActorQueryRow, OwnerTable, InItem).ColumnSizeData(&DetailsColumns);
	}

	TSharedRef<SWidget> MapWidget()
	{
		// clang-format off
		return
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SScaleBox)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				.Stretch(EStretch::ScaleToFit)
				.Content()
				[
					SNew(SImage)
					.Image(&MapBrush)
				]
			]
			+ SOverlay::Slot()
			[
				SNew(SActorLocationOverlay)
				.ActorQueries(&ActorQueries)
				.MapSize(this, &FActorMap::GetOrthoWidth)
				.ReferencePosition(this, &FActorMap::GetReferencePosition)
			];
		// clang-format on
	}
};

/**
 * This is a bootstrapper class that opens the actor map inside a standalone editor window.
 * This is separate from FActorMap to make it easier to use FActorMap in different contexts later on.
 * The bootstrapper should also take care of automatically closing the actor map if the world is destroyed, etc.
 */
class FActorMapWindowBootstrapper
{
public:
	TSharedPtr<FActorMap> ActorMap;

	DECLARE_EVENT(FActorMapWindowBootstrapper, FOnSlateWindowClosed);

	FOnSlateWindowClosed OnWindowClosed;

	TSharedPtr<SWindow> SlateWindow;

	static UWorld* GetCurrentTargetWorld()
	{
		// Always prefer the play world (both in cooked game and in PIE)
		if (UWorld* PossibleResult = GEngine->GetCurrentPlayWorld())
			return PossibleResult;

#if WITH_EDITOR
		if (GIsEditor)
		{
			// Fallback to the editor world in the editor
			return GEditor->GetEditorWorldContext().World();
		}
#endif
		return nullptr;
	}

	void HandleSlateWindowClosed(const TSharedRef<SWindow>& ClosedWindow)
	{
		ensure(SlateWindow.ToSharedRef() == ClosedWindow);
		HandleSlateWindowClosed_Inner();
	}

	void HandleSlateWindowClosed_Inner()
	{
		ActorMap.Reset();
		if (SlateWindow.IsValid())
		{
			SlateWindow.Reset();
			OnWindowClosed.Broadcast();
		}
	}

	void OpenWindowForCurrentWorld()
	{
		checkf(!ActorMap.IsValid(), TEXT("OpenWindowForCurrentWorld() must not be called twice on the same object"));

		ActorMap = MakeShared<FActorMap>();
		ActorMap->Initialize(GetCurrentTargetWorld());
		GEngine->OnWorldDestroyed().AddRaw(this, &FActorMapWindowBootstrapper::HandleWorldDestroyed);

		SlateWindow = SNew(SWindow)
						  .AutoCenter(EAutoCenter::None)
						  .IsInitiallyMaximized(true)
						  .ScreenPosition({20.f, 20.f})
						  .CreateTitleBar(true)
						  .SizingRule(ESizingRule::UserSized)
						  .SupportsMaximize(true)
						  .SupportsMinimize(true)
						  .HasCloseButton(true)
						  .Style(&FCoreStyle::Get().GetWidgetStyle<FWindowStyle>("Window"))
						  .ClientSize({500.f, 300.f})
						  .UseOSWindowBorder(false)
						  .Title(ActorMap->GetTitleText());

		SlateWindow->SetContent(ActorMap->TakeWidget());

		SlateWindow->SetOnWindowClosed(
			FOnWindowClosed::CreateRaw(this, &FActorMapWindowBootstrapper::HandleSlateWindowClosed));

		FSlateApplication::Get().AddWindow(SlateWindow.ToSharedRef());
		FSlateApplication::Get().GetRenderer()->CreateViewport(SlateWindow.ToSharedRef());
	}

	void HandleWorldDestroyed(UWorld* WorldDestroyed)
	{
		if (!ensure(IsValid(WorldDestroyed)))
			return;

		// Automatically close the window once the world is destroyed.
		if (WorldDestroyed == ActorMap->GetTargetWorld())
			CloseWindow();
	}

	void CloseWindow()
	{
		if (SlateWindow.IsValid())
		{
			SlateWindow->DestroyWindowImmediately();
			HandleSlateWindowClosed_Inner();
		}
	}
};

// Use unique pointer for now, so we only have to support a single window using the cheat.
TUniquePtr<FActorMapWindowBootstrapper> ActorMapWindowBootstrapper;

/** Function to open the actor map window via singleton bootstrapper */
void OpenActorMapWindowForCurrentWorld()
{
	if (ActorMapWindowBootstrapper.IsValid())
	{
		UE_LOG(
			LogOpenUnrealUtilities,
			Warning,
			TEXT("OpenActorMapWindowForCurrentWorld() was called, but a different window is already opened. Closing "
				 "the previous instance..."))
		ActorMapWindowBootstrapper->CloseWindow();
	}
	ActorMapWindowBootstrapper = MakeUnique<FActorMapWindowBootstrapper>();
	// Automatically clean up unique pointer once the window is closed.
	ActorMapWindowBootstrapper->OnWindowClosed.AddLambda([]() { ActorMapWindowBootstrapper.Reset(); });
	ActorMapWindowBootstrapper->OpenWindowForCurrentWorld();
}

static FAutoConsoleCommand OpenActorMapCommand(
	TEXT("ouu.Debug.OpenActorMap"),
	TEXT("Open an actor map for the current world (game or editor)"),
	FConsoleCommandDelegate::CreateStatic(OpenActorMapWindowForCurrentWorld));
