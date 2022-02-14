// Copyright (c) 2022 Jonas Reich

#pragma once

#include "Widgets/UserFocusResetableWidget.h"

#include "InteractableUserWidgetTests.generated.h"

UCLASS(meta = (Hidden, HideDropDown))
class UMockFocusResetable_True : public UUserWidget, public IUserFocusResetableWidget
{
	GENERATED_BODY()
protected:
	bool ResetUserFocus_Implementation() override { return true; }
};

UCLASS(meta = (Hidden, HideDropDown))
class UMockFocusResetable_False : public UUserWidget, public IUserFocusResetableWidget
{
	GENERATED_BODY()
protected:
	bool ResetUserFocus_Implementation() override { return false; }
};
