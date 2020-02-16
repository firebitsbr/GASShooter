// Copyright 2020 Dan Kestranek.


#include "Characters/Heroes/GSHeroCharacter.h"
#include "Animation/AnimInstance.h"
#include "AI/GSHeroAIController.h"
#include "Camera/CameraComponent.h"
#include "Characters/Abilities/GSAbilitySystemComponent.h"
#include "Characters/Abilities/AttributeSets/GSAmmoAttributeSet.h"
#include "Characters/Abilities/AttributeSets/GSAttributeSetBase.h"
#include "Components/WidgetComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GASShooter/GASShooterGameModeBase.h"
#include "GSBlueprintFunctionLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Net/UnrealNetwork.h"
#include "Player/GSPlayerController.h"
#include "Player/GSPlayerState.h"
#include "Sound/SoundCue.h"
#include "TimerManager.h"
#include "UI/GSFloatingStatusBarWidget.h"
#include "Weapons/GSWeapon.h"

AGSHeroCharacter::AGSHeroCharacter(const class FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	BaseTurnRate = 45.0f;
	BaseLookUpRate = 45.0f;
	bStartInFirstPersonPerspective = true;
	bIsFirstPersonPerspective = false;
	bASCInputBound = false;
	bChangedWeaponLocally = false;
	Default1PFOV = 90.0f;
	Default3PFOV = 80.0f;
	WeaponChangingDelayReplicationTag = FGameplayTag::RequestGameplayTag(FName("Ability.Weapon.IsChangingDelayReplication"));
	WeaponAmmoTypeNoneTag = FGameplayTag::RequestGameplayTag(FName("Weapon.Ammo.None"));
	WeaponAbilityTag = FGameplayTag::RequestGameplayTag(FName("Ability.Weapon"));
	Inventory = FGSHeroInventory();
	
	ThirdPersonCameraBoom = CreateDefaultSubobject<USpringArmComponent>(FName("CameraBoom"));
	ThirdPersonCameraBoom->SetupAttachment(RootComponent);
	ThirdPersonCameraBoom->bUsePawnControlRotation = true;
	ThirdPersonCameraBoom->SetRelativeLocation(FVector(0, 50, 68.492264));

	ThirdPersonCamera = CreateDefaultSubobject<UCameraComponent>(FName("FollowCamera"));
	ThirdPersonCamera->SetupAttachment(ThirdPersonCameraBoom);
	ThirdPersonCamera->FieldOfView = Default3PFOV;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(FName("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(RootComponent);
	FirstPersonCamera->bUsePawnControlRotation = true;

	FirstPersonMesh = CreateDefaultSubobject<USkeletalMeshComponent>(FName("FirstPersonMesh"));
	FirstPersonMesh->SetupAttachment(FirstPersonCamera);
	FirstPersonMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FirstPersonMesh->SetCollisionProfileName(FName("NoCollision"));
	FirstPersonMesh->bReceivesDecals = false;
	FirstPersonMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPose;
	FirstPersonMesh->CastShadow = false;
	FirstPersonMesh->SetVisibility(false, true);

	// Makes sure that the animations play on the Server so that we can use bone and socket transforms
	// to do things like spawning projectiles and other FX.
	//GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPose;
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetMesh()->SetCollisionProfileName(FName("NoCollision"));
	GetMesh()->bCastHiddenShadow = true;
	GetMesh()->bReceivesDecals = false;

	UIFloatingStatusBarComponent = CreateDefaultSubobject<UWidgetComponent>(FName("UIFloatingStatusBarComponent"));
	UIFloatingStatusBarComponent->SetupAttachment(RootComponent);
	UIFloatingStatusBarComponent->SetRelativeLocation(FVector(0, 0, 120));
	UIFloatingStatusBarComponent->SetWidgetSpace(EWidgetSpace::Screen);
	UIFloatingStatusBarComponent->SetDrawSize(FVector2D(500, 500));

	UIFloatingStatusBarClass = StaticLoadClass(UObject::StaticClass(), nullptr, TEXT("/Game/GASShooter/UI/UI_FloatingStatusBar_Hero.UI_FloatingStatusBar_Hero_C"));
	if (!UIFloatingStatusBarClass)
	{
		UE_LOG(LogTemp, Error, TEXT("%s() Failed to find UIFloatingStatusBarClass. If it was moved, please update the reference location in C++."), TEXT(__FUNCTION__));
	}

	AutoPossessAI = EAutoPossessAI::PlacedInWorld;
	AIControllerClass = AGSHeroAIController::StaticClass();
}

void AGSHeroCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AGSHeroCharacter, Inventory);
	// Only replicate CurrentWeapon to simulated clients and manually sync CurrentWeeapon with Owner when we're ready.
	// This allows us to predict weapon changing.
	DOREPLIFETIME_CONDITION(AGSHeroCharacter, CurrentWeapon, COND_SimulatedOnly);
}

// Called to bind functionality to input
void AGSHeroCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	PlayerInputComponent->BindAxis("MoveForward", this, &AGSHeroCharacter::MoveForward);
	PlayerInputComponent->BindAxis("MoveRight", this, &AGSHeroCharacter::MoveRight);

	PlayerInputComponent->BindAxis("LookUp", this, &AGSHeroCharacter::LookUp);
	PlayerInputComponent->BindAxis("LookUpRate", this, &AGSHeroCharacter::LookUpRate);
	PlayerInputComponent->BindAxis("Turn", this, &AGSHeroCharacter::Turn);
	PlayerInputComponent->BindAxis("TurnRate", this, &AGSHeroCharacter::TurnRate);

	PlayerInputComponent->BindAction("TogglePerspective", IE_Pressed, this, &AGSHeroCharacter::TogglePerspective);

	// Bind player input to the AbilitySystemComponent. Also called in OnRep_PlayerState because of a potential race condition.
	BindASCInput();
}

// Server only
void AGSHeroCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	AGSPlayerState* PS = GetPlayerState<AGSPlayerState>();
	if (PS)
	{
		// Set the ASC on the Server. Clients do this in OnRep_PlayerState()
		AbilitySystemComponent = Cast<UGSAbilitySystemComponent>(PS->GetAbilitySystemComponent());

		// AI won't have PlayerControllers so we can init again here just to be sure. No harm in initing twice for heroes that have PlayerControllers.
		PS->GetAbilitySystemComponent()->InitAbilityActorInfo(PS, this);

		WeaponChangingDelayReplicationTagChangedDelegateHandle = AbilitySystemComponent->RegisterGameplayTagEvent(WeaponChangingDelayReplicationTag)
			.AddUObject(this, &AGSHeroCharacter::WeaponChangingDelayReplicationTagChanged);

		// Set the AttributeSetBase for convenience attribute functions
		AttributeSetBase = PS->GetAttributeSetBase();

		AmmoAttributeSet = PS->GetAmmoAttributeSet();

		// If we handle players disconnecting and rejoining in the future, we'll have to change this so that possession from rejoining doesn't reset attributes.
		// For now assume possession = spawn/respawn.
		InitializeAttributes();

		AddStartupEffects();

		AddCharacterAbilities();

		AGSPlayerController* PC = Cast<AGSPlayerController>(GetController());
		if (PC)
		{
			PC->CreateHUD();
		}

		if (AbilitySystemComponent->GetTagCount(DeadTag) > 0)
		{
			// Set Health/Mana/Stamina to their max. This is only necessary for *Respawn*.
			SetHealth(GetMaxHealth());
			SetMana(GetMaxMana());
			SetStamina(GetMaxStamina());
			SetShield(GetMaxShield());
		}

		// Forcibly set the DeadTag count to 0. This is only necessary for *Respawn*.
		AbilitySystemComponent->SetTagMapCount(DeadTag, 0);

		InitializeFloatingStatusBar();

		// If player is host on listen server, the floating status bar would have been created for them from BeginPlay before player possession, hide it
		if (IsLocallyControlled() && IsPlayerControlled() && UIFloatingStatusBarComponent && UIFloatingStatusBar)
		{
			UIFloatingStatusBarComponent->SetVisibility(false, true);
		}
	}

	SetupStartupPerspective();
}

void AGSHeroCharacter::Restart()
{
	Super::Restart();

	//UE_LOG(LogTemp, Log, TEXT("%s %s %s"), TEXT(__FUNCTION__), *GetName(), ACTOR_ROLE_FSTRING);
}

UGSFloatingStatusBarWidget* AGSHeroCharacter::GetFloatingStatusBar()
{
	return UIFloatingStatusBar;
}

void AGSHeroCharacter::Die()
{
	// Go into third person for death animation
	SetPerspective(false);

	RemoveAllWeaponsFromInventory();

	AbilitySystemComponent->RegisterGameplayTagEvent(WeaponChangingDelayReplicationTag).Remove(WeaponChangingDelayReplicationTagChangedDelegateHandle);

	Super::Die();
}

void AGSHeroCharacter::FinishDying()
{
	if (GetLocalRole() == ROLE_Authority)
	{
		AGASShooterGameModeBase* GM = Cast<AGASShooterGameModeBase>(GetWorld()->GetAuthGameMode());

		if (GM)
		{
			GM->HeroDied(GetController());
		}
	}

	Super::FinishDying();
}

bool AGSHeroCharacter::IsInFirstPersonPerspective() const
{
	return bIsFirstPersonPerspective;
}

USkeletalMeshComponent* AGSHeroCharacter::GetFirstPersonMesh() const
{
	return FirstPersonMesh;
}

USkeletalMeshComponent* AGSHeroCharacter::GetThirdPersonMesh() const
{
	return GetMesh();
}

AGSWeapon* AGSHeroCharacter::GetCurrentWeapon() const
{
	return CurrentWeapon;
}

bool AGSHeroCharacter::AddWeaponToInventory(AGSWeapon* NewWeapon, bool bEquipWeapon)
{
	UE_LOG(LogTemp, Log, TEXT("%s %s %s"), TEXT(__FUNCTION__), *UGSBlueprintFunctionLibrary::GetPlayerEditorWindowRole(GetWorld()), *GetName());

	if (DoesWeaponExistInInventory(NewWeapon))
	{
		UE_LOG(LogTemp, Log, TEXT("%s %s %s Weapon already exists in Inventory, collecting %d ammo and destroying"),
			TEXT(__FUNCTION__), *GetName(), *UGSBlueprintFunctionLibrary::GetPlayerEditorWindowRole(GetWorld()), NewWeapon->GetPrimaryClipAmmo());

		USoundCue* PickupSound = NewWeapon->GetPickupSound();

		if (PickupSound && IsLocallyControlled())
		{
			UGameplayStatics::SpawnSoundAttached(PickupSound, GetRootComponent());
		}

		if (GetLocalRole() < ROLE_Authority)
		{
			return false;
		}

		// Create a dynamic instant Gameplay Effect to give the primary and secondary ammo
		UGameplayEffect* GEAmmo = NewObject<UGameplayEffect>(GetTransientPackage(), FName(TEXT("Ammo")));
		GEAmmo->DurationPolicy = EGameplayEffectDurationType::Instant;

		if (NewWeapon->PrimaryAmmoType != WeaponAmmoTypeNoneTag)
		{
			int32 Idx = GEAmmo->Modifiers.Num();
			GEAmmo->Modifiers.SetNum(Idx + 1);

			FGameplayModifierInfo& InfoPrimaryAmmo = GEAmmo->Modifiers[Idx];
			InfoPrimaryAmmo.ModifierMagnitude = FScalableFloat(NewWeapon->GetPrimaryClipAmmo());
			InfoPrimaryAmmo.ModifierOp = EGameplayModOp::Additive;
			InfoPrimaryAmmo.Attribute = UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(NewWeapon->PrimaryAmmoType);
		}

		if (NewWeapon->SecondaryAmmoType != WeaponAmmoTypeNoneTag)
		{
			int32 Idx = GEAmmo->Modifiers.Num();
			GEAmmo->Modifiers.SetNum(Idx + 1);

			FGameplayModifierInfo& InfoSecondaryAmmo = GEAmmo->Modifiers[Idx];
			InfoSecondaryAmmo.ModifierMagnitude = FScalableFloat(NewWeapon->GetSecondaryClipAmmo());
			InfoSecondaryAmmo.ModifierOp = EGameplayModOp::Additive;
			InfoSecondaryAmmo.Attribute = UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(NewWeapon->SecondaryAmmoType);
		}

		if (GEAmmo->Modifiers.Num() > 0)
		{
			AbilitySystemComponent->ApplyGameplayEffectToSelf(GEAmmo, 1.0f, AbilitySystemComponent->MakeEffectContext());
		}

		NewWeapon->Destroy();

		return false;
	}

	if (GetLocalRole() < ROLE_Authority)
	{
		return false;
	}

	Inventory.Weapons.Add(NewWeapon);
	NewWeapon->SetOwningCharacter(this);
	NewWeapon->AddAbilities();

	if (bEquipWeapon)
	{
		EquipWeapon(NewWeapon);
		ClientSyncCurrentWeapon(CurrentWeapon);
	}

	return true;
}

bool AGSHeroCharacter::RemoveWeaponFromInventory(AGSWeapon* WeaponToRemove)
{
	if (DoesWeaponExistInInventory(WeaponToRemove))
	{
		if (WeaponToRemove == CurrentWeapon)
		{
			UnEquipCurrentWeapon();
		}

		Inventory.Weapons.Remove(WeaponToRemove);
		WeaponToRemove->RemoveAbilities();
		WeaponToRemove->SetOwningCharacter(nullptr);
		WeaponToRemove->ResetWeapon();

		// Add parameter to drop weapon?

		return true;
	}

	return false;
}

void AGSHeroCharacter::RemoveAllWeaponsFromInventory()
{
	UE_LOG(LogTemp, Log, TEXT("%s %s %s %s Num Weapons in Inventory: %d"), TEXT(__FUNCTION__),
		*UGSBlueprintFunctionLibrary::GetPlayerEditorWindowRole(GetWorld()), ACTOR_ROLE_FSTRING, *GetName(), Inventory.Weapons.Num());

	if (GetLocalRole() < ROLE_Authority)
	{
		return;
	}

	UnEquipCurrentWeapon();

	float radius = 50.0f;
	float NumWeapons = Inventory.Weapons.Num();

	for (int32 i = Inventory.Weapons.Num() - 1; i >= 0; i--)
	{
		AGSWeapon* Weapon = Inventory.Weapons[i];
		RemoveWeaponFromInventory(Weapon);

		// Set the weapon up as a pickup

		// Add parameter to drop weapons and pass to RemoveWeaponFromInventory()?

		//TODO Set location based on num of weapons in a circle
		// x = radius * cos(angle)
		// y = radius * sin(angle)
		// angle = i / 360 if degrees
		float OffsetX = radius * FMath::Cos((i / NumWeapons) * 2.0f * PI);
		float OffsetY = radius * FMath::Sin((i / NumWeapons) * 2.0f * PI);
		Weapon->OnDropped(GetActorLocation() + FVector(OffsetX, OffsetY, 0.0f));
	}
}

void AGSHeroCharacter::EquipWeapon(AGSWeapon* NewWeapon)
{
	if (GetLocalRole() < ROLE_Authority)
	{
		ServerEquipWeapon(NewWeapon);
		SetCurrentWeapon(NewWeapon, CurrentWeapon);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("%s Equipping %s"), TEXT(__FUNCTION__), *NewWeapon->GetName());

		SetCurrentWeapon(NewWeapon, CurrentWeapon);
		bChangedWeaponLocally = true;
	}
}

void AGSHeroCharacter::ServerEquipWeapon_Implementation(AGSWeapon* NewWeapon)
{
	EquipWeapon(NewWeapon);
}

bool AGSHeroCharacter::ServerEquipWeapon_Validate(AGSWeapon* NewWeapon)
{
	return true;
}

void AGSHeroCharacter::NextWeapon()
{
	if (Inventory.Weapons.Num() < 2)
	{
		return;
	}

	int32 CurrentWeaponIndex = Inventory.Weapons.Find(CurrentWeapon);
	UnEquipCurrentWeapon();

	if (CurrentWeaponIndex == INDEX_NONE)
	{
		EquipWeapon(Inventory.Weapons[0]);
	}
	else
	{
		EquipWeapon(Inventory.Weapons[(CurrentWeaponIndex + 1) % Inventory.Weapons.Num()]);
	}
}

void AGSHeroCharacter::PreviousWeapon()
{
	if (Inventory.Weapons.Num() < 2)
	{
		return;
	}

	int32 CurrentWeaponIndex = Inventory.Weapons.Find(CurrentWeapon);

	UnEquipCurrentWeapon();

	if (CurrentWeaponIndex == INDEX_NONE)
	{
		EquipWeapon(Inventory.Weapons[0]);
	}
	else
	{
		int32 IndexOfPrevWeapon = FMath::Abs(CurrentWeaponIndex - 1 + Inventory.Weapons.Num()) % Inventory.Weapons.Num();
		EquipWeapon(Inventory.Weapons[IndexOfPrevWeapon]);
	}
}

FName AGSHeroCharacter::GetWeaponAttachPoint()
{
	return WeaponAttachPoint;
}

int32 AGSHeroCharacter::GetPrimaryClipAmmo() const
{
	if (CurrentWeapon)
	{
		return CurrentWeapon->GetPrimaryClipAmmo();
	}

	return 0;
}

int32 AGSHeroCharacter::GetMaxPrimaryClipAmmo() const
{
	if (CurrentWeapon)
	{
		return CurrentWeapon->GetMaxPrimaryClipAmmo();
	}

	return 0;
}

int32 AGSHeroCharacter::GetPrimaryReserveAmmo() const
{
	if (CurrentWeapon && AmmoAttributeSet)
	{
		FGameplayAttribute Attribute = AmmoAttributeSet->GetReserveAmmoAttributeFromTag(CurrentWeapon->PrimaryAmmoType);
		if (Attribute.IsValid())
		{
			return AbilitySystemComponent->GetNumericAttribute(Attribute);
		}
	}

	return 0;
}

int32 AGSHeroCharacter::GetSecondaryClipAmmo() const
{
	if (CurrentWeapon)
	{
		return CurrentWeapon->GetSecondaryClipAmmo();
	}

	return 0;
}

int32 AGSHeroCharacter::GetMaxSecondaryClipAmmo() const
{
	if (CurrentWeapon)
	{
		return CurrentWeapon->GetMaxSecondaryClipAmmo();
	}

	return 0;
}

int32 AGSHeroCharacter::GetSecondaryReserveAmmo() const
{
	if (CurrentWeapon)
	{
		FGameplayAttribute Attribute = AmmoAttributeSet->GetReserveAmmoAttributeFromTag(CurrentWeapon->SecondaryAmmoType);
		if (Attribute.IsValid())
		{
			return AbilitySystemComponent->GetNumericAttribute(Attribute);
		}
	}

	return 0;
}

int32 AGSHeroCharacter::GetNumWeapons() const
{
	return Inventory.Weapons.Num();
}

/**
* On the Server, Possession happens before BeginPlay.
* On the Client, BeginPlay happens before Possession.
* So we can't use BeginPlay to do anything with the AbilitySystemComponent because we don't have it until the PlayerState replicates from possession.
*/
void AGSHeroCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Only needed for Heroes placed in world and when the player is the Server.
	// On respawn, they are set up in PossessedBy.
	// When the player a client, the floating status bars are all set up in OnRep_PlayerState.
	InitializeFloatingStatusBar();

	// CurrentWeapon is replicated only to Simulated clients so sync the current weapon manually
	if (GetLocalRole() == ROLE_AutonomousProxy)
	{
		ServerSyncCurrentWeapon();
	}
}

void AGSHeroCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	StartingThirdPersonCameraBoomArmLength = ThirdPersonCameraBoom->TargetArmLength;
	StartingThirdPersonCameraBoomLocation = ThirdPersonCameraBoom->GetRelativeLocation();
	StartingThirdPersonMeshLocation = GetMesh()->GetRelativeLocation();

	GetWorldTimerManager().SetTimerForNextTick(this, &AGSHeroCharacter::SpawnDefaultInventory);
}

void AGSHeroCharacter::LookUp(float Value)
{
	if (IsAlive())
	{
		AddControllerPitchInput(Value);
	}
}

void AGSHeroCharacter::LookUpRate(float Value)
{
	if (IsAlive())
	{
		AddControllerPitchInput(Value * BaseLookUpRate * GetWorld()->DeltaTimeSeconds);
	}
}

void AGSHeroCharacter::Turn(float Value)
{
	if (IsAlive())
	{
		AddControllerYawInput(Value);
	}
}

void AGSHeroCharacter::TurnRate(float Value)
{
	if (IsAlive())
	{
		AddControllerYawInput(Value * BaseTurnRate * GetWorld()->DeltaTimeSeconds);
	}
}

void AGSHeroCharacter::MoveForward(float Value)
{
	if (IsAlive())
	{
		AddMovementInput(UKismetMathLibrary::GetForwardVector(FRotator(0, GetControlRotation().Yaw, 0)), Value);
	}
}

void AGSHeroCharacter::MoveRight(float Value)
{
	if (IsAlive())
	{
		AddMovementInput(UKismetMathLibrary::GetRightVector(FRotator(0, GetControlRotation().Yaw, 0)), Value);
	}
}

void AGSHeroCharacter::TogglePerspective()
{
	bIsFirstPersonPerspective = !bIsFirstPersonPerspective;
	SetPerspective(bIsFirstPersonPerspective);
}

void AGSHeroCharacter::SetPerspective(bool InIsFirstPersonPerspective)
{
	// Only change perspective for the locally controlled player. Simulated proxies should stay in third person.
	// To swap cameras, deactivate current camera (defaults to ThirdPersonCamera), activate desired camera, and call PlayerController->SetViewTarget() on self
	AGSPlayerController* PC = GetController<AGSPlayerController>();
	if (PC && PC->IsLocalPlayerController())
	{
		if (InIsFirstPersonPerspective)
		{
			ThirdPersonCamera->Deactivate();
			FirstPersonCamera->Activate();
			PC->SetViewTarget(this);

			GetMesh()->SetVisibility(false, true);
			FirstPersonMesh->SetVisibility(true, true);

			// Move third person mesh back so that the shadow doesn't look disconnected
			GetMesh()->SetRelativeLocation(StartingThirdPersonMeshLocation + FVector(-120.0f, 0.0f, 0.0f));
		}
		else
		{
			FirstPersonCamera->Deactivate();
			ThirdPersonCamera->Activate();
			PC->SetViewTarget(this);

			FirstPersonMesh->SetVisibility(false, true);
			GetMesh()->SetVisibility(true, true);

			// Reset the third person mesh
			GetMesh()->SetRelativeLocation(StartingThirdPersonMeshLocation);
		}
	}
}

void AGSHeroCharacter::InitializeFloatingStatusBar()
{
	// Only create once
	if (UIFloatingStatusBar || !IsValid(AbilitySystemComponent))
	{
		return;
	}

	// Don't create for locally controlled player. We could add a game setting to toggle this later.
	if (IsPlayerControlled() && IsLocallyControlled())
	{
		return;
	}

	// Need a valid PlayerState
	if (!GetPlayerState())
	{
		return;
	}

	// Setup UI for Locally Owned Players only, not AI or the server's copy of the PlayerControllers
	AGSPlayerController* PC = Cast<AGSPlayerController>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (PC && PC->IsLocalPlayerController())
	{
		if (UIFloatingStatusBarClass)
		{
			UIFloatingStatusBar = CreateWidget<UGSFloatingStatusBarWidget>(PC, UIFloatingStatusBarClass);
			if (UIFloatingStatusBar && UIFloatingStatusBarComponent)
			{
				UIFloatingStatusBarComponent->SetWidget(UIFloatingStatusBar);

				// Setup the floating status bar
				UIFloatingStatusBar->SetHealthPercentage(GetHealth() / GetMaxHealth());
				UIFloatingStatusBar->SetManaPercentage(GetMana() / GetMaxMana());
				UIFloatingStatusBar->SetShieldPercentage(GetShield() / GetMaxShield());
				UIFloatingStatusBar->OwningCharacter = this;
				UIFloatingStatusBar->SetCharacterName(CharacterName);
			}
		}
	}
}

// Client only
void AGSHeroCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	AGSPlayerState* PS = GetPlayerState<AGSPlayerState>();
	if (PS)
	{
		// Set the ASC for clients. Server does this in PossessedBy.
		AbilitySystemComponent = Cast<UGSAbilitySystemComponent>(PS->GetAbilitySystemComponent());

		// Init ASC Actor Info for clients. Server will init its ASC when it possesses a new Actor.
		AbilitySystemComponent->InitAbilityActorInfo(PS, this);

		// Bind player input to the AbilitySystemComponent. Also called in SetupPlayerInputComponent because of a potential race condition.
		BindASCInput();

		AbilitySystemComponent->AbilityFailedCallbacks.AddUObject(this, &AGSHeroCharacter::OnAbilityActivationFailed);

		// Set the AttributeSetBase for convenience attribute functions
		AttributeSetBase = PS->GetAttributeSetBase();
		
		AmmoAttributeSet = PS->GetAmmoAttributeSet();

		// If we handle players disconnecting and rejoining in the future, we'll have to change this so that posession from rejoining doesn't reset attributes.
		// For now assume possession = spawn/respawn.
		InitializeAttributes();

		AGSPlayerController* PC = Cast<AGSPlayerController>(GetController());
		if (PC)
		{
			PC->CreateHUD();
		}
		
		if (CurrentWeapon)
		{
			// Update owning character and ASC just in case it repped before PlayerState
			CurrentWeapon->SetOwningCharacter(this);

			if (!PrimaryReserveAmmoChangedDelegateHandle.IsValid())
			{
				PrimaryReserveAmmoChangedDelegateHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(CurrentWeapon->PrimaryAmmoType)).AddUObject(this, &AGSHeroCharacter::CurrentWeaponPrimaryReserveAmmoChanged);
			}
			if (!SecondaryReserveAmmoChangedDelegateHandle.IsValid())
			{
				SecondaryReserveAmmoChangedDelegateHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(CurrentWeapon->SecondaryAmmoType)).AddUObject(this, &AGSHeroCharacter::CurrentWeaponSecondaryReserveAmmoChanged);
			}
		}

		if (AbilitySystemComponent->GetTagCount(DeadTag) > 0)
		{
			// Set Health/Mana/Stamina/Shield to their max. This is only necessary for *Respawn*.
			SetHealth(GetMaxHealth());
			SetMana(GetMaxMana());
			SetStamina(GetMaxStamina());
			SetShield(GetMaxShield());
		}

		// Forcibly set the DeadTag count to 0. This is only necessary for *Respawn*.
		AbilitySystemComponent->SetTagMapCount(DeadTag, 0);

		// Simulated on proxies don't have their PlayerStates yet when BeginPlay is called so we call it again here
		InitializeFloatingStatusBar();
	}
}

void AGSHeroCharacter::OnRep_Controller()
{
	Super::OnRep_Controller();

	SetupStartupPerspective();
}

void AGSHeroCharacter::BindASCInput()
{
	if (!bASCInputBound && IsValid(AbilitySystemComponent) && IsValid(InputComponent))
	{
		AbilitySystemComponent->BindAbilityActivationToInputComponent(InputComponent, FGameplayAbilityInputBinds(FString("ConfirmTarget"),
			FString("CancelTarget"), FString("EGSAbilityInputID"), static_cast<int32>(EGSAbilityInputID::Confirm), static_cast<int32>(EGSAbilityInputID::Cancel)));

		bASCInputBound = true;
	}
}

void AGSHeroCharacter::SpawnDefaultInventory()
{
	UE_LOG(LogTemp, Log, TEXT("%s %s"), TEXT(__FUNCTION__), *GetName());

	if (GetLocalRole() < ROLE_Authority)
	{
		return;
	}

	int32 NumWeaponClasses = DefaultInventoryWeaponClasses.Num();
	for (int32 i = 0; i < NumWeaponClasses; i++)
	{
		if (!DefaultInventoryWeaponClasses[i])
		{
			// An empty item was added to the Array in blueprint
			continue;
		}

		FActorSpawnParameters SpawnInfo;
		SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AGSWeapon* NewWeapon = GetWorld()->SpawnActorDeferred<AGSWeapon>(DefaultInventoryWeaponClasses[i],
			FTransform::Identity, this, this, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		NewWeapon->bSpawnWithCollision = false;
		NewWeapon->FinishSpawning(FTransform::Identity);

		bool bEquipFirstWeapon = i == 0;
		AddWeaponToInventory(NewWeapon, bEquipFirstWeapon);
	}
}

void AGSHeroCharacter::SetupStartupPerspective()
{
	APlayerController* PC = Cast<APlayerController>(GetController());

	if (PC && PC->IsLocalController())
	{
		bIsFirstPersonPerspective = bStartInFirstPersonPerspective;
		SetPerspective(bIsFirstPersonPerspective);
	}
}

bool AGSHeroCharacter::DoesWeaponExistInInventory(AGSWeapon* InWeapon)
{
	UE_LOG(LogTemp, Log, TEXT("%s InWeapon class %s"), TEXT(__FUNCTION__), *InWeapon->GetClass()->GetName());

	for (AGSWeapon* Weapon : Inventory.Weapons)
	{
		if (Weapon && InWeapon && Weapon->GetClass() == InWeapon->GetClass())
		{
			return true;
		}
	}

	return false;
}

void AGSHeroCharacter::SetCurrentWeapon(AGSWeapon* NewWeapon, AGSWeapon* LastWeapon)
{
	if (NewWeapon == LastWeapon)
	{
		return;
	}

	// Cancel active weapon abilities
	if (AbilitySystemComponent)
	{
		FGameplayTagContainer AbilityTagsToCancel = FGameplayTagContainer(WeaponAbilityTag);
		AbilitySystemComponent->CancelAbilities(&AbilityTagsToCancel);
	}

	UnEquipWeapon(LastWeapon);

	if (NewWeapon)
	{
		// Weapons coming from OnRep_CurrentWeapon won't have the owner set
		CurrentWeapon = NewWeapon;
		CurrentWeapon->SetOwningCharacter(this);
		CurrentWeapon->Equip();

		AGSPlayerController* PC = GetController<AGSPlayerController>();
		if (PC && PC->IsLocalController())
		{
			PC->SetEquippedWeaponPrimaryIconFromSprite(CurrentWeapon->PrimaryIcon);
			PC->SetEquippedWeaponStatusText(CurrentWeapon->StatusText);
			PC->SetPrimaryClipAmmo(CurrentWeapon->GetPrimaryClipAmmo());
			PC->SetPrimaryReserveAmmo(GetPrimaryReserveAmmo());
			PC->SetHUDReticle(CurrentWeapon->GetPrimaryHUDReticleClass());
		}

		NewWeapon->OnPrimaryClipAmmoChanged.AddDynamic(this, &AGSHeroCharacter::CurrentWeaponPrimaryClipAmmoChanged);
		NewWeapon->OnSecondaryClipAmmoChanged.AddDynamic(this, &AGSHeroCharacter::CurrentWeaponSecondaryClipAmmoChanged);
		
		if (AbilitySystemComponent)
		{
			PrimaryReserveAmmoChangedDelegateHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(CurrentWeapon->PrimaryAmmoType)).AddUObject(this, &AGSHeroCharacter::CurrentWeaponPrimaryReserveAmmoChanged);
			SecondaryReserveAmmoChangedDelegateHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(CurrentWeapon->SecondaryAmmoType)).AddUObject(this, &AGSHeroCharacter::CurrentWeaponSecondaryReserveAmmoChanged);
		}

		UAnimMontage* Equip1PMontage = CurrentWeapon->GetEquip1PMontage();
		if (Equip1PMontage && GetFirstPersonMesh())
		{
			GetFirstPersonMesh()->GetAnimInstance()->Montage_Play(Equip1PMontage);
		}

		UAnimMontage* Equip3PMontage = CurrentWeapon->GetEquip3PMontage();
		if (Equip3PMontage && GetThirdPersonMesh())
		{
			GetThirdPersonMesh()->GetAnimInstance()->Montage_Play(Equip3PMontage);
		}
	}
	else
	{
		// This will clear HUD, tags etc
		UnEquipCurrentWeapon();
	}
}

void AGSHeroCharacter::UnEquipWeapon(AGSWeapon* WeaponToUnEquip)
{
	//TODO this will run into issues when calling UnEquipWeapon explicitly and the WeaponToUnEquip == CurrentWeapon

	if (WeaponToUnEquip)
	{
		WeaponToUnEquip->OnPrimaryClipAmmoChanged.RemoveDynamic(this, &AGSHeroCharacter::CurrentWeaponPrimaryClipAmmoChanged);
		WeaponToUnEquip->OnSecondaryClipAmmoChanged.RemoveDynamic(this, &AGSHeroCharacter::CurrentWeaponSecondaryClipAmmoChanged);

		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(WeaponToUnEquip->PrimaryAmmoType)).Remove(PrimaryReserveAmmoChangedDelegateHandle);
			AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UGSAmmoAttributeSet::GetReserveAmmoAttributeFromTag(WeaponToUnEquip->SecondaryAmmoType)).Remove(SecondaryReserveAmmoChangedDelegateHandle);
		}
		
		WeaponToUnEquip->UnEquip();
	}
}

void AGSHeroCharacter::UnEquipCurrentWeapon()
{
	UnEquipWeapon(CurrentWeapon);
	CurrentWeapon = nullptr;

	AGSPlayerController* PC = GetController<AGSPlayerController>();
	if (PC && PC->IsLocalController())
	{
		PC->SetEquippedWeaponPrimaryIconFromSprite(nullptr);
		PC->SetEquippedWeaponStatusText(FText());
		PC->SetPrimaryClipAmmo(0);
		PC->SetPrimaryReserveAmmo(0);
		PC->SetHUDReticle(nullptr);
	}
}

void AGSHeroCharacter::CurrentWeaponPrimaryClipAmmoChanged(int32 OldPrimaryClipAmmo, int32 NewPrimaryClipAmmo)
{
	AGSPlayerController* PC = GetController<AGSPlayerController>();
	if (PC && PC->IsLocalController())
	{
		PC->SetPrimaryClipAmmo(NewPrimaryClipAmmo);
	}
}

void AGSHeroCharacter::CurrentWeaponSecondaryClipAmmoChanged(int32 OldSecondaryClipAmmo, int32 NewSecondaryClipAmmo)
{
	AGSPlayerController* PC = GetController<AGSPlayerController>();
	if (PC && PC->IsLocalController())
	{
		PC->SetSecondaryClipAmmo(NewSecondaryClipAmmo);
	}
}

void AGSHeroCharacter::CurrentWeaponPrimaryReserveAmmoChanged(const FOnAttributeChangeData& Data)
{
	AGSPlayerController* PC = GetController<AGSPlayerController>();
	if (PC && PC->IsLocalController())
	{
		PC->SetPrimaryReserveAmmo(Data.NewValue);
	}
}

void AGSHeroCharacter::CurrentWeaponSecondaryReserveAmmoChanged(const FOnAttributeChangeData& Data)
{
	AGSPlayerController* PC = GetController<AGSPlayerController>();
	if (PC && PC->IsLocalController())
	{
		PC->SetSecondaryReserveAmmo(Data.NewValue);
	}
}

void AGSHeroCharacter::WeaponChangingDelayReplicationTagChanged(const FGameplayTag CallbackTag, int32 NewCount)
{
	UE_LOG(LogTemp, Log, TEXT("%s %s %s TagCount: %d"), TEXT(__FUNCTION__), *UGSBlueprintFunctionLibrary::GetPlayerEditorWindowRole(GetWorld()), *CallbackTag.ToString(), NewCount);

	if (CallbackTag == WeaponChangingDelayReplicationTag)
	{
		if (NewCount < 1)
		{
			// We only replicate the current weapon to simulated proxies so manually sync it when the weapon changing delay replication
			// tag is removed. We keep the weapon changing tag on for ~1s after the equip montage to allow for activating changing weapon
			// again without the server trying to clobber the next locally predicted weapon.
			ClientSyncCurrentWeapon(CurrentWeapon);
		}
	}
}

void AGSHeroCharacter::OnRep_CurrentWeapon(AGSWeapon* LastWeapon)
{
	bChangedWeaponLocally = false;
	SetCurrentWeapon(CurrentWeapon, LastWeapon);
}

void AGSHeroCharacter::OnRep_Inventory()
{
	if (GetLocalRole() == ROLE_AutonomousProxy && Inventory.Weapons.Num() > 0 && !CurrentWeapon)
	{
		// Since we don't replicate the CurrentWeapon to the owning client, this is a way to ask the Server to sync
		// the CurrentWeapon after it's been spawned via replication from the Server.
		// The weapon spawning is replicated but the variable CurrentWeapon is not on the owning client.
		ServerSyncCurrentWeapon();
	}
}

void AGSHeroCharacter::OnAbilityActivationFailed(const UGameplayAbility* FailedAbility, const FGameplayTagContainer& FailTags)
{
	if (FailedAbility && FailedAbility->AbilityTags.HasTagExact(FGameplayTag::RequestGameplayTag(FName("Ability.Weapon.IsChanging"))))
	{
		if (bChangedWeaponLocally)
		{
			// Ask the Server to resync the CurrentWeapon that we predictively changed
			UE_LOG(LogTemp, Warning, TEXT("%s Weapon Changing ability activation failed. Syncing CurrentWeapon. %s. %s"), TEXT(__FUNCTION__),
				*UGSBlueprintFunctionLibrary::GetPlayerEditorWindowRole(GetWorld()), *FailTags.ToString());

			ServerSyncCurrentWeapon();
		}
	}
}

void AGSHeroCharacter::ServerSyncCurrentWeapon_Implementation()
{
	UE_LOG(LogTemp, Log, TEXT("%s %s"), TEXT(__FUNCTION__), *UGSBlueprintFunctionLibrary::GetPlayerEditorWindowRole(GetWorld()));

	ClientSyncCurrentWeapon(CurrentWeapon);
}

bool AGSHeroCharacter::ServerSyncCurrentWeapon_Validate()
{
	return true;
}

void AGSHeroCharacter::ClientSyncCurrentWeapon_Implementation(AGSWeapon* InWeapon)
{
	AGSWeapon* LastWeapon = CurrentWeapon;
	CurrentWeapon = InWeapon;
	OnRep_CurrentWeapon(LastWeapon);
}

bool AGSHeroCharacter::ClientSyncCurrentWeapon_Validate(AGSWeapon* InWeapon)
{
	return true;
}
