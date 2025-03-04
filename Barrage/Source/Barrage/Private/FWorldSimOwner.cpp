﻿#include "FWorldSimOwner.h"

#include "BarrageContactListener.h"
#include "CoordinateUtils.h"
#include "PhysicsCharacter.h"
#include "CastShapeCollectors/SphereCastCollector.h"
#include "CastShapeCollectors/SphereSearchCollector.h"
#include "CollisionDetectionFilters/FirstHitRayCastCollector.h"

using namespace JOLT;

	FWorldSimOwner::FWorldSimOwner(float cDeltaTime)
	{
		DeltaTime = cDeltaTime;
		// Register allocation hook. In this example we'll just let Jolt use malloc / free but you can override these if you want (see Memory.h).
		// This needs to be done before any other Jolt function is called.
		BarrageToJoltMapping = MakeShareable(new KeyToBody());
		BoxCache = MakeShareable(new BoundsToShape());
		CharacterToJoltMapping = MakeShareable(new TMap<FBarrageKey, TSharedPtr<FBCharacterBase>>());
		RegisterDefaultAllocator();
		contact_listener = MakeShareable(new BarrageContactListener());
		body_activation_listener = MakeShareable(new MyBodyActivationListener());
		Allocator = MakeShareable(new TempAllocatorImpl(AllocationArenaSize));
		physics_system = MakeShareable(new PhysicsSystem());
		// Install trace and assert callbacks
		Trace = TraceImpl;
		JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)

		// Create a factory, this class is responsible for creating instances of classes based on their name or hash and is mainly used for deserialization of saved data.
		// It is not directly used in this example but still required.
		Factory::sInstance = new Factory();

		// Register all physics types with the factory and install their collision handlers with the CollisionDispatch class.
		// If you have your own custom shape types you probably need to register their handlers with the CollisionDispatch before calling this function.
		// If you implement your own default material (PhysicsMaterial::sDefault) make sure to initialize it before this function or else this function will create one for you.
		RegisterTypes();

		// We need a job system that will execute physics jobs on multiple threads. Typically
		// you would implement the JobSystem interface yourself and let Jolt Physics run on top
		// of your own job scheduler. JobSystemThreadPool is an example implementation.
		job_system = MakeShareable(
			new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, thread::hardware_concurrency() - 1));
		// Now we can create the actual physics system.
		physics_system->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
		                     broad_phase_layer_interface, object_vs_broadphase_layer_filter,
		                     object_vs_object_layer_filter);


		physics_system->SetBodyActivationListener(body_activation_listener.Get());


		physics_system->SetContactListener(contact_listener.Get());

		// The main way to interact with the bodies in the physics system is through the body interface. There is a locking and a non-locking
		// variant of this. We're going to use the locking version (even though we're not planning to access bodies from multiple threads)
		body_interface = &physics_system->GetBodyInterface();


		// Optional step: Before starting the physics simulation you can optimize the broad phase. This improves collision detection performance (it's pointless here because we only have 2 bodies).
		// You should definitely not call this every frame or when e.g. streaming in a new level section as it is an expensive operation.
		// Instead insert all new objects in batches instead of 1 at a time to keep the broad phase efficient.
		physics_system->OptimizeBroadPhase();

		// here's Andrea's transform into jolt.
		//	https://youtu.be/jhCupKFly_M?si=umi0zvJer8NymGzX&t=438
	}
	
	inline void FWorldSimOwner::SphereCast(
		double Radius,
		double Distance,
		FVector3d CastFrom,
		FVector3d Direction,
		TSharedPtr<FHitResult> OutHit,
		const BroadPhaseLayerFilter& BroadPhaseFilter,
		const ObjectLayerFilter& ObjectFilter,
		const BodyFilter& BodiesFilter) const
	{
		check(OutHit.IsValid());
		OutHit->Init();
		// In order to denote whether we actually hit anything, we'll munge Jolt's uint32 BodyID values into
		// the int32 of `FHitResult::MyItem`. This should be fine.
		OutHit->MyItem = JPH::BodyID::cInvalidBodyID;

		ShapeCastSettings settings;
		settings.mUseShrunkenShapeAndConvexRadius = true;
		settings.mReturnDeepestPoint = true;
	
		JPH::SphereShape sphere(Radius);
		sphere.SetEmbedded();

		JPH::Vec3 JoltCastFrom = CoordinateUtils::ToJoltCoordinates(CastFrom);
		JPH::Vec3 JoltDirection = CoordinateUtils::ToJoltCoordinates(Direction) * Distance;
		
		JPH::RShapeCast ShapeCast(
			&sphere,
			JPH::Vec3::sReplicate(1.0f),
			JPH::RMat44::sTranslation(JoltCastFrom),
			JoltDirection);

		// Actually do the shapecast
		SphereCastCollector CastCollector(*(physics_system.Get()), ShapeCast);
		physics_system->GetNarrowPhaseQueryNoLock().CastShape(
			ShapeCast,
			settings,
			ShapeCast.mCenterOfMassStart.GetTranslation(),
			CastCollector,
			BroadPhaseFilter,
			ObjectFilter,
			BodiesFilter);

		if (CastCollector.mBody) {
			// Fill out the hit result
			FHitResult* HitResultPtr = OutHit.Get();

			HitResultPtr->MyItem = CastCollector.mBody->GetID().GetIndexAndSequenceNumber();
			HitResultPtr->bBlockingHit = true;
			
			FVector3f UnrealContactPos = CoordinateUtils::FromJoltCoordinates(CastCollector.mContactPosition);
			HitResultPtr->Location.Set(UnrealContactPos.X, UnrealContactPos.Y, UnrealContactPos.Z);
			HitResultPtr->ImpactPoint.Set(UnrealContactPos.X, UnrealContactPos.Y, UnrealContactPos.Z);
			HitResultPtr->Distance = (UnrealContactPos - FVector3f(CastFrom)).Length();

			JPH::Vec3& HitNormal = CastCollector.mContactNormal;
			FVector3f UnrealImpactNormal = CoordinateUtils::FromJoltUnitVector(HitNormal);
			HitResultPtr->ImpactNormal.Set(UnrealImpactNormal.X, UnrealImpactNormal.Y, UnrealImpactNormal.Z);
		}
	}

	inline void FWorldSimOwner::SphereSearch(
		const JPH::BodyID& CastingBody,
		const FVector3d& Location,
		double Radius,
		const JPH::BroadPhaseLayerFilter& BroadPhaseFilter,
		const JPH::ObjectLayerFilter& ObjectFilter,
		const JPH::BodyFilter& BodiesFilter,
		uint32* OutFoundObjectCount,
		TArray<uint32>& OutFoundObjectIDs) const
	{
		// const DefaultBroadPhaseLayerFilter default_broadphase_layer_filter = physics_system->GetDefaultBroadPhaseLayerFilter(Layers::CAST_QUERY);
		// const BroadPhaseLayerFilter &broadphase_layer_filter = default_broadphase_layer_filter;
		// const DefaultObjectLayerFilter default_object_layer_filter = physics_system->GetDefaultLayerFilter(Layers::CAST_QUERY);
		// const ObjectLayerFilter &object_layer_filter = default_object_layer_filter;
		// const IgnoreSingleBodyFilter default_body_filter(CastingBody);
		// const BodyFilter &body_filter = default_body_filter;
		
		JPH::Vec3 JoltLocation = CoordinateUtils::ToJoltCoordinates(Location);
		
		SphereSearchCollector Collector(physics_system.Get()->GetBodyLockInterfaceNoLock(), BodiesFilter);
		physics_system->GetBroadPhaseQuery().CollideSphere(JoltLocation, Radius, Collector, BroadPhaseFilter, ObjectFilter);

		(*OutFoundObjectCount) = Collector.BodyCount;
		for (uint32 FoundBodyIdx = 0; FoundBodyIdx < Collector.BodyCount; ++FoundBodyIdx)
		{
			OutFoundObjectIDs.Add(Collector.mBodies[FoundBodyIdx]->GetID().GetIndexAndSequenceNumber());
		}
	}

	inline void FWorldSimOwner::CastRay(FVector3d CastFrom, FVector3d Direction, const BroadPhaseLayerFilter& BroadPhaseFilter, const ObjectLayerFilter& ObjectFilter, const BodyFilter& BodiesFilter, TSharedPtr<FHitResult> OutHit) const
	{
		check(OutHit.IsValid());
		OutHit->Init();
		// Use the same ID munging as we do in SphereCast
		OutHit->MyItem = JPH::BodyID::cInvalidBodyID;
		
		JPH::Vec3 JoltCastFromLocation = CoordinateUtils::ToJoltCoordinates(CastFrom);
		JPH::Vec3 JoltDirection = CoordinateUtils::ToJoltCoordinates(Direction);
		
		RRayCast Ray(JoltCastFromLocation, JoltDirection);
		RayCastResult CastResult;
		FirstHitRayCastCollector FirstHitCollector(Ray, CastResult, physics_system->GetBodyLockInterfaceNoLock(), BodiesFilter);
		
		physics_system->GetBroadPhaseQuery().CastRay(RayCast(Ray), FirstHitCollector, BroadPhaseFilter, ObjectFilter);

		if (FirstHitCollector.mHit.mBodyID != BodyID())
		{
			// Fill out the hit result
			FHitResult* HitResultPtr = OutHit.Get();

			HitResultPtr->MyItem = FirstHitCollector.mHit.mBodyID.GetIndexAndSequenceNumber();
			HitResultPtr->bBlockingHit = true;
			
			FVector3f UnrealContactPos = CoordinateUtils::FromJoltCoordinates(FirstHitCollector.mContactPosition);
			HitResultPtr->Location.Set(UnrealContactPos.X, UnrealContactPos.Y, UnrealContactPos.Z);
			HitResultPtr->ImpactPoint.Set(UnrealContactPos.X, UnrealContactPos.Y, UnrealContactPos.Z);
			HitResultPtr->Distance = (UnrealContactPos - FVector3f(CastFrom)).Length();
		}
	}

	inline EMotionType LayerToMotionTypeMapping(uint16 Layer)
	{
		switch (Layer)
		{
		case Layers::NON_MOVING:
			return EMotionType::Static;
		case Layers::MOVING:
			return EMotionType::Dynamic;
		case Layers::HITBOX:
			return EMotionType::Kinematic;
		case Layers::PROJECTILE:
			return EMotionType::Kinematic;
		case Layers::ENEMYPROJECTILE:
			return EMotionType::Kinematic;
		case Layers::ENEMY:
			return EMotionType::Dynamic;
		case Layers::BONKFREEENEMY:
			return EMotionType::Dynamic;
		case Layers::CAST_QUERY:
			return EMotionType::Kinematic;
		case Layers::CAST_QUERY_LEVEL_GEOMETRY_ONLY:
			return EMotionType::Kinematic;
		case Layers::DEBRIS:
			return EMotionType::Dynamic;
		default:
			JPH_ASSERT(false);
			return EMotionType::Static;
		}
	}

	inline EMotionQuality LayerToMotionQualityMapping(uint16 Layer)
	{
		switch (Layer)
		{
		case Layers::NON_MOVING:
			return EMotionQuality::Discrete;
		case Layers::MOVING:
			return EMotionQuality::Discrete;
		case Layers::HITBOX:
			return EMotionQuality::Discrete;
		case Layers::PROJECTILE:
			return EMotionQuality::LinearCast;
		case Layers::ENEMYPROJECTILE:
			return EMotionQuality::LinearCast;
		case Layers::BONKFREEENEMY:
			return EMotionQuality::Discrete;
		case Layers::ENEMY:
			return EMotionQuality::Discrete;
		case Layers::CAST_QUERY:
			return EMotionQuality::Discrete;
		case Layers::CAST_QUERY_LEVEL_GEOMETRY_ONLY:
			return EMotionQuality::Discrete;
		case Layers::DEBRIS:
			return EMotionQuality::Discrete;
		default:
			JPH_ASSERT(false);
			return EMotionQuality::Discrete;
		}
	}

	inline Ref<Shape> FWorldSimOwner::AttemptBoxCache(double JoltX, double JoltY, double JoltZ, float HEReduceMin)
	{
		auto Bounds = Vec3(JoltX, JoltY, JoltZ);
		void* At = &Bounds;
		auto BoundsHash = HashBytes(At,sizeof(Bounds));
		if (!BoxCache->contains(BoundsHash))
		{
			Ref<Shape> NewShape = new BoxShape(Vec3(JoltX, JoltY, JoltZ), FMath::Min(HEReduceMin / 2.f, 0.01));
			BoxCache->insert_or_assign(BoundsHash, NewShape);
			return NewShape;
		}
		else
		{
			Ref<Shape> Result;
			bool Success =  BoxCache->find(BoundsHash, Result);
			if (Success)
			{
				return Result;
			}
			else
			{
				Ref<Shape> NewShape = new BoxShape(Vec3(JoltX, JoltY, JoltZ), FMath::Min(HEReduceMin / 2.f, 0.01));
				BoxCache->insert_or_assign(BoundsHash, NewShape);
				return NewShape;
			}
		}
	}

	//we need the coordinate utils, but we don't really want to include them in the .h
	inline FBarrageKey FWorldSimOwner::CreatePrimitive(FBBoxParams& ToCreate, uint16 Layer, bool IsSensor, bool forceDynamic)
	{

		BodyID BodyIDTemp = BodyID();
		EMotionType MovementType = forceDynamic ? EMotionType::Dynamic : LayerToMotionTypeMapping(Layer);
		EMotionQuality MotionQuality = LayerToMotionQualityMapping(Layer);

		Vec3 HalfExtent = Vec3(ToCreate.JoltX, ToCreate.JoltY, ToCreate.JoltZ);
		float HEReduceMin = HalfExtent.ReduceMin();
		if(MotionQuality == EMotionQuality::LinearCast)
		{
			HEReduceMin = 0.01;
		}
		//not really sure how much our cache helps us, but it could in theory improve GJK perf? idk. hm.
		Ref<Shape> CachedShape = AttemptBoxCache(ToCreate.JoltX, ToCreate.JoltY, ToCreate.JoltZ, FMath::Min(HEReduceMin / 2.f, 0.01));
	
		ShapeSettings::ShapeResult box = RotatedTranslatedShapeSettings(
			 	 CoordinateUtils::ToJoltCoordinates(ToCreate.Offset.X, ToCreate.Offset.Y, ToCreate.Offset.Z), Quat::sIdentity(), CachedShape).Create();
		ShapeRefC box_shape = box.Get();

		// We don't expect an error here, but you can check floor_shape_result for HasError() / GetError()
		// Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
		BodyCreationSettings box_body_settings(box_shape,
		                                       CoordinateUtils::ToJoltCoordinates(ToCreate.Point.GridSnap(1)),
		                                       Quat::sIdentity(),
		                                       MovementType, Layer);
		JPH::MassProperties msp;
		msp.ScaleToMass(ToCreate.MassClass); //actual mass in kg
		box_body_settings.mMassPropertiesOverride = msp;
		box_body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
		box_body_settings.mIsSensor = IsSensor;
		box_body_settings.mMotionQuality = MotionQuality;

		// IMPORTANT! If this isn't set, sensors don't collide with static stuff (like level geometry!)
		if (IsSensor)
		{
			box_body_settings.mCollideKinematicVsNonDynamic = true;
		}
		
		// Create the actual rigid body
		Body* box_body = body_interface->CreateBody(box_body_settings);
		// Note that if we run out of bodies this can return nullptr

		// Add it to the world
		body_interface->AddBody(box_body->GetID(), EActivation::Activate);
		BodyIDTemp = box_body->GetID();
		auto FBK = GenerateBarrageKeyFromBodyId(BodyIDTemp);
		//Barrage key is unique to WORLD and BODY. This is crushingly important.
		BarrageToJoltMapping->insert(FBK, BodyIDTemp);

		return FBK;
	}

	//we need the coordinate utils, but we don't really want to include them in the .h
	inline FBarrageKey FWorldSimOwner::CreatePrimitive(FBCharParams& ToCreate, uint16 Layer)
	{

		BodyID BodyIDTemp = BodyID();
		EMotionType MovementType = LayerToMotionTypeMapping(Layer);
		TSharedPtr<FBCharacter> NewCharacter = MakeShareable<FBCharacter>(new FBCharacter);
		NewCharacter->mHeightStanding = 2 * ToCreate.JoltHalfHeightOfCylinder;
		NewCharacter->mRadiusStanding = ToCreate.JoltRadius;
		NewCharacter->mInitialPosition = CoordinateUtils::ToJoltCoordinates(ToCreate.point);
		NewCharacter->mMaxSpeed = ToCreate.speed;
		if(NewCharacter->mInitialPosition.IsNearZero() || NewCharacter->mInitialPosition.IsNaN())
		{
			NewCharacter->mInitialPosition = Vec3::sZero();
		}
		NewCharacter->World = this->physics_system;
		NewCharacter->mDeltaTime = DeltaTime;
		NewCharacter->mForcesUpdate = Vec3::sZero();
		//floor_shape_settings.SetEmbedded(); // A ref counted object on the stack (base class RefTarget) should be marked as such to prevent it from being freed when its reference count goes to 0.
		// Create the shape
		BodyIDTemp = NewCharacter->Create(&this->CharacterVsCharacterCollisionSimple);
		//Barrage key is unique to WORLD and BODY. This is crushingly important.
		auto FBK = GenerateBarrageKeyFromBodyId(BodyIDTemp);
		BarrageToJoltMapping->insert(FBK, BodyIDTemp);
		CharacterToJoltMapping->Add(FBK, NewCharacter);

		return FBK;
	}

	inline FBarrageKey FWorldSimOwner::CreatePrimitive(FBSphereParams& ToCreate, uint16 Layer, bool IsSensor)
	{

		BodyID BodyIDTemp = BodyID();
		EMotionType MovementType = LayerToMotionTypeMapping(Layer);

		BodyCreationSettings sphere_settings(new SphereShape(ToCreate.JoltRadius),
		                                     CoordinateUtils::ToJoltCoordinates(ToCreate.point.GridSnap(1)),
		                                     Quat::sIdentity(),
		                                     MovementType,
		                                     Layer);
		sphere_settings.mIsSensor = IsSensor;
		BodyIDTemp = body_interface->CreateAndAddBody(sphere_settings, EActivation::Activate);

		auto FBK = GenerateBarrageKeyFromBodyId(BodyIDTemp);
		//Barrage key is unique to WORLD and BODY. This is crushingly important.
		BarrageToJoltMapping->insert(FBK, BodyIDTemp);
		return FBK;
	}

	inline FBarrageKey FWorldSimOwner::CreatePrimitive(FBCapParams& ToCreate, uint16 Layer, bool IsSensor, FMassByCategory::BMassCategories MassClass)
	{
		BodyID BodyIDTemp = BodyID();
		EMotionType MovementType = LayerToMotionTypeMapping(Layer);
		BodyCreationSettings cap_settings(new CapsuleShape(ToCreate.JoltHalfHeightOfCylinder, ToCreate.JoltRadius),
		                                  CoordinateUtils::ToJoltCoordinates(ToCreate.point.GridSnap(1)),
		                                  Quat::sIdentity(),
		                                  MovementType,
		                                  Layer);
		JPH::MassProperties msp;
		msp.ScaleToMass(MassClass); //actual mass in kg
		cap_settings.mMassPropertiesOverride = msp;
		cap_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
		cap_settings.mIsSensor = IsSensor;
		BodyIDTemp = body_interface->CreateAndAddBody(cap_settings, EActivation::Activate);
		auto FBK = GenerateBarrageKeyFromBodyId(BodyIDTemp);
		//Barrage key is unique to WORLD and BODY. This is crushingly important.
		BarrageToJoltMapping->insert(FBK, BodyIDTemp);
		return FBK;
	}

	FBLet FWorldSimOwner::LoadComplexStaticMesh(FBTransform& MeshTransform,
	                                            const UStaticMeshComponent* StaticMeshComponent,
	                                            FSkeletonKey Outkey)
	{
		using ParticlesType = Chaos::TParticles<Chaos::FRealSingle, 3>;
		using ParticleVecType = Chaos::TVec3<Chaos::FRealSingle>;
		using ::CoordinateUtils;
		if (!StaticMeshComponent)
		{
			return nullptr;
		}
		if (!StaticMeshComponent->GetStaticMesh())
		{
			return nullptr;
		}
		if (!StaticMeshComponent->GetStaticMesh()->GetRenderData())
		{
			return nullptr;
		}
		UBodySetup* body = StaticMeshComponent->GetStaticMesh()->GetBodySetup();
		if (!body)
		{
			return nullptr; // we don't accept anything but complex or primitive yet.
			//simple collision tends to use primitives, in which case, don't call this
			//or compound shapes which will get added back in.
		}
		auto CollisionMesh = StaticMeshComponent->GetStaticMesh();
		if (!CollisionMesh)
		{
			UE_LOG(LogTemp, Warning, TEXT("Falling back to ACTUAL MESH."));
			CollisionMesh = StaticMeshComponent->GetStaticMesh();
		}
		if (!CollisionMesh)
		{
			return nullptr;
		}
		if (!CollisionMesh->IsCompiling() || !CollisionMesh->IsPostLoadThreadSafe())
		{
			auto collbody = CollisionMesh->GetBodySetup();
			if (collbody == nullptr)
			{
				return nullptr;
			}

			//Here we go!
			auto& MeshSet = collbody->TriMeshGeometries;
			JPH::VertexList JoltVerts;
			JPH::IndexedTriangleList JoltIndexedTriangles;
			uint32 tris = 0;
			for (auto& Mesh : MeshSet)
			{
				tris += Mesh->Elements().GetNumTriangles();
			}
			JoltVerts.reserve(tris);
			JoltIndexedTriangles.reserve(tris);
			for (auto& Mesh : MeshSet)
			{
				//indexed triangles are made by collecting the vertexes, then generating triples describing the triangles.
				//this allows the heavier vertices to be stored only once, rather than each time they are used. for large models
				//like terrain, this can be extremely significant. though, it's not truly clear to me if it's worth it.
				auto& VertToTriBuffers = Mesh->Elements();
				auto& Verts = Mesh->Particles().X();
				if (VertToTriBuffers.RequiresLargeIndices())
				{
					for (auto& aTri : VertToTriBuffers.GetLargeIndexBuffer())
					{
						JoltIndexedTriangles.push_back(IndexedTriangle(aTri[2], aTri[1], aTri[0]));
					}
				}
				else
				{
					for (auto& aTri : VertToTriBuffers.GetSmallIndexBuffer())
					{
						JoltIndexedTriangles.push_back(IndexedTriangle(aTri[2], aTri[1], aTri[0]));
					}
				}

				for (auto& vtx : Verts)
				{
					//need to figure out how to defactor this without breaking typehiding or having to create a bunch of util.h files.
					//though, tbh, the util.h is the play. TODO: util.h ?
					JoltVerts.push_back(CoordinateUtils::ToJoltCoordinates(vtx));
				}
			}
			JPH::MeshShapeSettings FullMesh(JoltVerts, JoltIndexedTriangles);
			//just the last boiler plate for now.
			JPH::ShapeSettings::ShapeResult err = FullMesh.Create();
			if (err.HasError())
			{
				return nullptr;
			}
			//TODO: should we be holding the shape ref in gamesim owner?
			auto& shape = err.Get();
			BodyCreationSettings creation_settings;
			creation_settings.mMotionType = EMotionType::Static;
			creation_settings.mObjectLayer = Layers::NON_MOVING;
			creation_settings.mFriction = 0.5f;
			creation_settings.mRestitution = 0;
			creation_settings.mUseManifoldReduction = true;
			
			Shape::ShapeResult result = shape->ScaleShape(MeshTransform.GetScaleJoltArg());
			if (result.HasError() || result.IsEmpty())
			{
				throw;
			}

			Ref<Shape> OriginAndRotationApplied = new RotatedTranslatedShape(CoordinateUtils::ToJoltCoordinates(MeshTransform.GetLocation()), CoordinateUtils::ToJoltRotation(MeshTransform.GetRotationQuat()),  result.Get());
			creation_settings.SetShape(OriginAndRotationApplied);
			BodyID bID = body_interface->CreateAndAddBody(creation_settings, EActivation::Activate);
			FBarrageKey FBK = GenerateBarrageKeyFromBodyId(bID);
			BarrageToJoltMapping->insert(FBK, bID);
			FBLet shared = MakeShareable(new FBarragePrimitive(FBK, Outkey));

			return shared;
		}
		return nullptr;
	}

	void FWorldSimOwner::StepSimulation()
	{
		// If you take larger steps than 1 / 60th of a second you need to do multiple collision steps in order to keep the simulation stable. Do 1 collision step per 1 / 60th of a second (round up).
		constexpr int cCollisionSteps = 1;

		// Step the world
		auto AllocHoldOpen = Allocator;
		auto JobHoldOpen = job_system;
		auto PhysicsHoldOpen = physics_system;
		if (AllocHoldOpen && JobHoldOpen)
		{
			PhysicsHoldOpen->Update(DeltaTime, cCollisionSteps, AllocHoldOpen.Get(), JobHoldOpen.Get());
		}
	}

	void FWorldSimOwner::OptimizeBroadPhase()
	{
		// Optional step: Before starting the physics simulation you can optimize the broad phase. This improves collision detection performance (it's pointless here because we only have 2 bodies).
		// You should definitely not call this every frame or when e.g. streaming in a new level section as it is an expensive operation.
		// Instead insert all new objects in batches instead of 1 at a time to keep the broad phase efficient.
		auto HoldOpen = physics_system;
		HoldOpen->OptimizeBroadPhase();
	}

	FBarrageKey FWorldSimOwner::GenerateBarrageKeyFromBodyId(const BodyID& Input) const
	{
		return GenerateBarrageKeyFromBodyId(Input.GetIndexAndSequenceNumber());
	}

	FBarrageKey FWorldSimOwner::GenerateBarrageKeyFromBodyId(const uint32 RawIndexAndSequenceNumberInput) const
	{
		uint64_t KeyCompose = PointerHash(this);
		KeyCompose = KeyCompose << 32;
		KeyCompose |= RawIndexAndSequenceNumberInput;
		return FBarrageKey(KeyCompose);
	}

	FWorldSimOwner::~FWorldSimOwner()
	{
		UnregisterTypes();
		Factory::sInstance = nullptr;


		//this is the canonical order.
		//grab our hold open.		
		auto HoldOpen = physics_system;
		auto& magic = physics_system->GetBodyLockInterface();
		physics_system.Reset(); //cast it into the fire.
		std::this_thread::yield(); //Cycle.
		HoldOpen.Reset();
		job_system.Reset();
		Allocator.Reset();
	}

	bool FWorldSimOwner::UpdateCharacter(FBPhysicsInput& Update)
	{
		auto key = Update.Target.Get()->KeyIntoBarrage;
		auto CharacterOuter = CharacterToJoltMapping->Find(key);
		//As you add handling for Characters with Inner Shapes, you'll need to use something like the line below.
		//Unfortunately, it's going to be a lot of work. Right now, there's a bug preventing us from doing it, something in the lifecycle.
		//auto CharacterInner = BarrageToJoltMapping->find(Update.Target.Get()->KeyIntoBarrage); 
		if (CharacterOuter)
		{
			(*CharacterOuter)->IngestUpdate(Update);
			return true;
		}
		return false;
	}

	//convenience function for bulk updates.
	bool FWorldSimOwner::UpdateCharacters(TSharedPtr<TArray<FBPhysicsInput>> Array)
	{
		for (auto& update : *Array)
		{
			UpdateCharacter(update);
		}
		return true;
	}
