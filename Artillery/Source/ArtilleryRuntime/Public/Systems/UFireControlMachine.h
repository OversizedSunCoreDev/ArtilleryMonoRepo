// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreTypes.h"
#include "CoreMinimal.h"
#include "AttributeSet.h"
#include <unordered_map>

#include "FArtilleryGun.h"
#include "CanonicalInputStreamECS.h"
#include "ArtilleryDispatch.h"
#include <bitset>
#include "ArtilleryCommonTypes.h"
#include "ArtilleryControlComponent.h"
#include "FAttributeMap.h"
#include "UArtilleryGameplayTagContainer.h"
#include "TransformDispatch.h"
#include "Components/ActorComponent.h"
#include "UFireControlMachine.generated.h"

//dynamic constructed statemachine for matching patterns in action records to triggering abilities.
//extends the Ability System Component to remove even more boiler plate and smoothly integrate
//Artillery's threaded input model.

//The fire control machine manages activation patterns.
//it does not interact directly with input or run the patterns itself.
// 
// Patterns are always run by the artillery worker thread. events generated by pattern success flow through Dispatch.
//
//Each Fire Control Machine manages attributes and abilities, with all abilities being ordered into ArtilleryGuns.
//This additional constraint, combined with the pattern system, allows us to better build abilities that are general
//but NOT abstract. Our goal is to reduce boilerplate and improve ease of use, not necessarily increase code reuse.
//We HOPE that will happen, but bear this set of priorities in mind while dealing with Guns and FireControl.

//As a final word of warning, it has been our extensive experience that generic is better than general, and concrete
//is better than abstract. The urge towards pattern-before-problem programming is deadly.
UCLASS()
class ARTILLERYRUNTIME_API UFireControlMachine : public UArtilleryFireControl
{
	GENERATED_BODY()

public:
	static inline int orderInInitialize = 0;
	UCanonicalInputStreamECS* MyInput;

	/**Found this in the AbilitySystemComponent.h:
	 *
	 *	The abilities we can activate. 
	 *		-This will include CDOs for non instanced abilities and per-execution instanced abilities. 
	 *		-Actor-instanced abilities will be the actual instance (not CDO)
	 *		
     *	This array is not vital for things to work. It is a convenience thing for 'giving abilities to the actor'. But abilities could also work on things
     *	without an AbilitySystemComponent. For example an ability could be written to execute on a StaticMeshActor. As long as the ability doesn't require 
     *	instancing or anything else that the AbilitySystemComponent would provide, then it doesn't need the component to function.
     */

	//it's really not clear how vital granting and removing abilities is. getting my arms around this system is still a lot.
	
	//*******************************************************************************************
	//patterns are run in ArtilleryBusyWorker. Search for ARTILLERY_FIRE_CONTROL_MACHINE_HANDLING
	//*******************************************************************************************

	//IF YOU DO NOT CALL THIS FROM THE GAMETHREAD, YOU WILL HAVE A BAD TIME.
	void pushPatternToRunner(IPM::CanonPattern ToBind, PlayerKey InputStreamByPlayer, FActionBitMask ToSeek, FGunKey ToFire)
	{
		FActionPatternParams myParams = FActionPatternParams(ToSeek, MyKey, InputStreamKey(InputStreamByPlayer), ToFire);
		MyInput->registerPattern(ToBind, myParams);
	}

	//IF YOU DO NOT CALL THIS FROM THE GAMETHREAD, YOU WILL HAVE A BAD TIME.
	void popPatternFromRunner(IPM::CanonPattern ToBind, PlayerKey InputStreamByPlayer, FActionBitMask ToSeek, FGunKey ToFire)
	{
		FActionPatternParams myParams = FActionPatternParams(ToSeek, MyKey, InputStreamKey(InputStreamByPlayer), ToFire);
		MyInput->removePattern(ToBind, myParams);
	}
	
	virtual void PushGunToFireMapping(const FGunKey& ToFire) override
	{
		Super::PushGunToFireMapping(ToFire);
	}

	virtual void PopGunFromFireMapping(const FGunKey& ToRemove) override
	{
		Super::PopGunFromFireMapping(ToRemove);
	}

	void AddTestGun(Intents::Intent BindIntent, FArtilleryGun* Gun, IPM::CanonPattern Pattern)
	{
		FActionBitMask IntentBitPattern;
		IntentBitPattern.buttons = BindIntent;
		Gun->UpdateProbableOwner(ParentKey);
		Gun->Initialize(Gun->MyGunKey, false);
		TSharedPtr<FArtilleryGun> GunSharedPtr = MakeShareable(Gun);
		FGunKey key = MyDispatch->RegisterExistingGun(GunSharedPtr, ParentKey);
		pushPatternToRunner(Pattern, APlayer::CABLE, IntentBitPattern, key);
		PushGunToFireMapping(key);
	}

	void RegisterGunPatternPair(Intents::Intent BindIntent, TSharedPtr<FArtilleryGun> Gun, IPM::CanonPattern Pattern)
	{
		if (Gun.IsValid())
		{
			FActionBitMask IntentBitPattern;
			IntentBitPattern.buttons = BindIntent;
			Gun->UpdateProbableOwner(ParentKey);
			Gun->Initialize(Gun->MyGunKey, false);
			FGunKey key = MyDispatch->RegisterExistingGun(Gun, ParentKey);
			pushPatternToRunner(Pattern, APlayer::CABLE, IntentBitPattern, key);
			PushGunToFireMapping(key);
		}
	}

	//IF YOU DO NOT CALL THIS FROM THE GAMETHREAD, YOU WILL HAVE A BAD TIME.
	ActorKey CompleteRegistrationByActorParent(bool IsLocalPlayerCharacter,
		const ActorKey Parent,
		TMap<AttribKey, double> Attributes)
	{
		//these are initialized earlier under all intended orderings, but we cannot ensure that this function will be called correctly
		//so we should do what we can to foolproof things. As long as the world subsystems are up, force-updating
		//here will either:
		//work correctly
		//fail fast
		MyInput = GetWorld()->GetSubsystem<UCanonicalInputStreamECS>();
		MyDispatch = GetWorld()->GetSubsystem<UArtilleryDispatch>();
		TransformDispatch =  GetWorld()->GetSubsystem<UTransformDispatch>();
		MyInput->RegisterKeysToParentActorMapping(MyKey, true, Parent);
		ParentKey = Parent;
		Usable = true;
		MyAttributes = MakeShareable(new FAttributeMap(ParentKey, MyDispatch, Attributes));
		MyTags = NewObject<UArtilleryGameplayTagContainer>();
		MyTags->Initialize(ParentKey, MyDispatch);

		UE_LOG(LogTemp, Warning, TEXT("FCM Mana: %f"), MyDispatch->GetAttrib(ParentKey, Attr::Mana)->GetCurrentValue());
		
		return ParentKey;
		
		//right now, we can push all our patterns here as well, and we can use a static set of patterns for
		//each of our fire control machines. you can basically think of a fire control machine as a full set
		//of related abilities, their attributes, and similar required to, you know, actually fire a gun.
		//it is also the gas component, if you're using gas.
		//There's a bit more blueprint exposure work to do here as a result.
#ifdef CONTROL_TEST_MODE

#endif
	}
	
	virtual void InitializeComponent() override
	{
		Super::InitializeComponent();
		MyInput = GetWorld()->GetSubsystem<UCanonicalInputStreamECS>();
		MyDispatch = GetWorld()->GetSubsystem<UArtilleryDispatch>();
	}
	
	//this happens post init but pre begin play, and the world subsystems should exist by this point.
	//we use this to help ensure that if the actor's begin play triggers first, things will be set correctly
	//I've left the same code in begin play as a fallback.
	void ReadyForReplication() override
	{
		Super::ReadyForReplication();
	}

	//on components, begin play can fire twice, because we aren't allowed to have nice things.
	//This can cause it to fire BEFORE the actor's begin play fires, which leaves you with
	//very few good options. the bool Usable helps control this.
	//This is, ironically, not a problem in actual usage, only testing, for us.
	void BeginPlay() override
	{
		Super::BeginPlay(); 
		MyInput = GetWorld()->GetSubsystem<UCanonicalInputStreamECS>();
	}
};
