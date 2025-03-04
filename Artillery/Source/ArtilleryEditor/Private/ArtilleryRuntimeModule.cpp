// Copyright Epic Games, Inc. All Rights Reserved.

#include "ArtilleryEditorModule.h"
#include "UArtilleryAbilityMinimum.h"

#define LOCTEXT_NAMESPACE "FArtilleryEditorModule"

void FArtilleryEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory;
	// the exact timing is specified in the .uplugin file per-module
	PrepareAutoGeneratedDefaultEvents();
}

void FArtilleryEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.
	// For modules that support dynamic reloading, we call this function before unloading the module.
	FKismetEditorUtilities::UnregisterAutoBlueprintNodeCreation(this);

}

void FArtilleryEditorModule::PrepareAutoGeneratedDefaultEvents()
{
	
	RegisterDefaultEvent(UArtilleryPerActorAbilityMinimum, K2_ActivateViaArtillery);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FArtilleryEditorModule, ArtilleryEditor)
