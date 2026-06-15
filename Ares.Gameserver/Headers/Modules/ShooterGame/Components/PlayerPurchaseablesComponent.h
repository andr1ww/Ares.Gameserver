#pragma once
#include "pch.h"

class PlayerPurchaseablesComponent
{
public:
    static void Init();

private:
    static bool AuthForceGivePurchasable(UPlayerPurchaseablesComponent* _this, FFrame& Stack);
};