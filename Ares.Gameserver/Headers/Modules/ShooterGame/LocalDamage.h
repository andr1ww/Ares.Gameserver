#pragma once
namespace LocalDamage
{
    void ApplyHit(class AActor* Causer, class APawn* Instigator, class UClass* Type,
                  const struct FHitResult& Hit, float Amount);
}
