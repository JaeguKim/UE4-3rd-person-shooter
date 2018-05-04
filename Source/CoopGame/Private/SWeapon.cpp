// Fill out your copyright notice in the Description page of Project Settings.

#include "SWeapon.h"
#include "DrawDebugHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Components/SkeletalMeshComponent.h"
#include "Particles/ParticleSystemComponent.h"
#include "CoopGame.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "TimerManager.h"
#include "Sound/SoundCue.h"
#include "Net/UnrealNetwork.h"
#include "SCharacter.h"

static int32 DebugWeaponDrawing = 0;
FAutoConsoleVariableRef CVARDebugWeaponDrawing(
	TEXT("COOP.DebugWeapons"), 
	DebugWeaponDrawing, 
	TEXT("Draw Debug Lines for Weapons"), 
	ECVF_Cheat);

// Sets default values
ASWeapon::ASWeapon()
{
	MeshComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("MeshComp"));
	RootComponent = MeshComp;

	MuzzleSocketName = "MuzzleSocket";
	TracerTargetName = "Target";

	BaseDamage = 20.0f;
	BulletSpread = 2.0f;
	RateOfFire = 600;

	SetReplicates(true);

	NetUpdateFrequency = 66.0f;
	MinNetUpdateFrequency = 33.0f;

	CurrentAmmo = 999;
	CurrentAmmoInClip = 30;
	MaxAmmoInClip = 30;
	
	bCanReloading = true;
}

float ASWeapon::PlayWeaponAnimation(UAnimMontage* Animation, float InPlayRate, FName StartSectionName)
{
	float Duration = 0.0f;

	ASCharacter * MyPawn = Cast<ASCharacter>(GetOwner());

	if (MyPawn)
	{
		if (Animation)
		{
			
			Duration = MyPawn->PlayAnimMontage(Animation, InPlayRate, StartSectionName);
		}
		
	}

	return Duration;
}

void ASWeapon::StopWeaponAnimation(UAnimMontage* Animation)
{
	if (MyPawn)
	{
		if (Animation)
		{
			MyPawn->StopAnimMontage(Animation);
		}
	}
}

bool ASWeapon:: CanReload()
{
	int32 AmmoCountToReload = MaxAmmoInClip - CurrentAmmoInClip;
	
	bool IsFullAmmoInClip = (AmmoCountToReload == 0);
	bool IsAmmoToReload = (AmmoCountToReload <= CurrentAmmo);

	return (!IsFullAmmoInClip) && IsAmmoToReload;
}

void ASWeapon:: ReloadWeapon()
{
	if (!bCanReloading || !CanReload())
		return;
	
	bCanReloading = false;
	bReloading = true;
	
	float AnimDuration = PlayWeaponAnimation(ReloadAnim);
	if (AnimDuration <= 0.0f)
	{
		AnimDuration = 1.5f;
	}

	GetWorldTimerManager().SetTimer(TimerHandle_StopReload, this, &ASWeapon::StopReload, AnimDuration, false);
	
	
	PlayWeaponSound(ReloadSound);
	
	
}

void ASWeapon::StopReload()
{
	//Handle Ammo Count 
	int32 AmmoToGive = FMath::Min(CurrentAmmo, MaxAmmoInClip - CurrentAmmoInClip);
	CurrentAmmoInClip += AmmoToGive;
	CurrentAmmo -= AmmoToGive;

	bReloading = false;
	bCanReloading = true;
	StopWeaponAnimation(ReloadAnim);
}


int32 ASWeapon::GetCurrentAmmo() const
{
	return CurrentAmmo;
}


int32 ASWeapon::GetCurrentAmmoInClip() const
{
	return CurrentAmmoInClip;
}

void ASWeapon::BeginPlay()
{
	Super::BeginPlay();

	TimeBetweenShots = 60 / RateOfFire;
}

void ASWeapon::Fire()
{
	AActor* MyOwner = GetOwner();
	ASCharacter * MyCharacter = Cast<ASCharacter>(MyOwner);

	if (bReloading || MyCharacter->IsRunning() || CurrentAmmoInClip == 0)
		return;

	if (Role < ROLE_Authority)
	{
		ServerFire();
	}

	if (MyOwner)
	{
		FVector EyeLocation;
		FRotator EyeRotation;
		MyOwner->GetActorEyesViewPoint(EyeLocation, EyeRotation);

		FVector ShotDirection = EyeRotation.Vector();

		//Bullet Spread
		float HalfRad = FMath::DegreesToRadians(BulletSpread);
		ShotDirection = FMath::VRandCone(ShotDirection, HalfRad, HalfRad);

		FVector TraceEnd = EyeLocation + (ShotDirection * 10000);

		EPhysicalSurface SurfaceType = SurfaceType_Default;

		FHitResult Hit;

		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(MyOwner);
		QueryParams.AddIgnoredActor(this);
		QueryParams.bTraceComplex = true; //Set collision effect to specific mesh
		QueryParams.bReturnPhysicalMaterial = true;

		//Particle "Target" parameter
		FVector TracerEndPoint = TraceEnd;

		PlayWeaponSound(FireSound);

		if (GetWorld()->LineTraceSingleByChannel(Hit, EyeLocation, TraceEnd, COLLISON_WEAPON, QueryParams))
		{
			AActor* HitActor = Hit.GetActor();
			
			SurfaceType = UPhysicalMaterial::DetermineSurfaceType(Hit.PhysMaterial.Get());
			
			float ActualDamage = BaseDamage;
			if (SurfaceType == SURFACE_FLESHVULNERABLE)
			{
				ActualDamage *= 4.0f;
			}

			UGameplayStatics::ApplyPointDamage(HitActor, ActualDamage, ShotDirection,Hit, MyOwner->GetInstigatorController(), MyOwner, DamageType);

			PlayImpactEffects(SurfaceType, Hit.ImpactPoint);

			TracerEndPoint = Hit.ImpactPoint;
		}

		if (DebugWeaponDrawing > 0)
		{
			DrawDebugLine(GetWorld(), EyeLocation, TraceEnd, FColor::White, false, 1.0f, 0, 1.0f);
		}

		PlayFireEffects(TracerEndPoint);
		
		CurrentAmmoInClip--;
		if (CurrentAmmoInClip == 0)
		{
			ReloadWeapon();
		}

		if (Role == ROLE_Authority)
		{
			HitScanTrace.TraceTo = TracerEndPoint;
			HitScanTrace.SurfaceType = SurfaceType;
		}


		LastFireTime = GetWorld()->TimeSeconds;
	}
}


void ASWeapon::OnRep_HitScanTrace()
{
	PlayFireEffects(HitScanTrace.TraceTo);
	PlayImpactEffects(HitScanTrace.SurfaceType, HitScanTrace.TraceTo);
}


void ASWeapon::ServerFire_Implementation()
{
	Fire();
}

bool ASWeapon::ServerFire_Validate()
{
	return true;
}

void ASWeapon::StartFire()
{
	float FirstDelay = FMath::Max(LastFireTime + TimeBetweenShots - GetWorld()->TimeSeconds, 0.0f);

	GetWorldTimerManager().SetTimer(TimerHandle_TimeBetweenShots, this, &ASWeapon::Fire, TimeBetweenShots, true, FirstDelay);
}

void ASWeapon::StopFire()
{
	GetWorldTimerManager().ClearTimer(TimerHandle_TimeBetweenShots);
}



void ASWeapon::PlayFireEffects(FVector TraceEnd)
{
	if (MuzzleEffect)
	{
		UGameplayStatics::SpawnEmitterAttached(MuzzleEffect, MeshComp, MuzzleSocketName);
	}

	if (TracerEffect)
	{
		FVector MuzzleLocation = MeshComp->GetSocketLocation(MuzzleSocketName);
		UParticleSystemComponent* TracerComp = UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), TracerEffect, MuzzleLocation);
		if (TracerComp)
		{
			TracerComp->SetVectorParameter("Target", TraceEnd);
		}
	}

	APawn* MyOwner = Cast<APawn>(GetOwner());
	if (MyOwner)
	{
		APlayerController * PC = Cast<APlayerController>(MyOwner->GetController());
		if (PC)
		{
			PC->ClientPlayCameraShake(FireCamShake);
		}
	}
}

void ASWeapon::PlayImpactEffects(EPhysicalSurface SurfaceType, FVector ImpactPoint)
{
	UParticleSystem* SelectedEffect = nullptr;

	switch (SurfaceType)
	{
	case SURFACE_FLESHDEFAULT:
	case SURFACE_FLESHVULNERABLE:
		SelectedEffect = FleshImpactEffect;
		break;
	default:
		SelectedEffect = DefaultImpactEffect;
		break;
	}


	if (SelectedEffect)
	{
		FVector MuzzleLocation = MeshComp->GetSocketLocation(MuzzleSocketName);

		FVector ShotDirection = ImpactPoint - MuzzleLocation;
		ShotDirection.Normalize();

		UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), SelectedEffect, ImpactPoint, ShotDirection.Rotation());
	}
}

void ASWeapon::PlayWeaponSound(USoundCue* SoundToPlay)
{
	UAudioComponent* AC = nullptr;
	APawn* MyPawn = Cast<APawn>(GetOwner());
	
	if (SoundToPlay && MyPawn)
	{
		UGameplayStatics::SpawnSoundAttached(SoundToPlay, MyPawn->GetRootComponent());
	}


}

void ASWeapon::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(ASWeapon, HitScanTrace, COND_SkipOwner); //ÃÑ¾ËÀ» ½ð »ç¶÷Àº º¹Á¦°¡ ¾ÈµÊ
}

