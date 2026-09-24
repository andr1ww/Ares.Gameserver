#include <pch.h>

// SpawnGameObject is stripped, always returns null

namespace LocalGameObjects { void Init(); }
INIT_MODULE(LocalGameObjects);

namespace
{
    AGameObject* SpawnGameObject(UObject* WorldContext, UClass* GameObjectClass, const FTransform* Where,
                                 bool bSpawnEvenIfColliding, APawn* InstigatedBy, AActor* Owner)
    {
        if (!GameObjectClass || !Where) return nullptr;
        static auto SpawnActorInternal =
            (AActor * (*)(UWorld*, const UClass*, FTransform*, void*))(ImageBase + 0x3651AC0);

        FActorSpawnParameters Parameters{}; // instigator has to be set before construction
        Parameters.Owner = Owner;
        Parameters.Instigator = InstigatedBy;
        Parameters.bDeferConstruction = false;
        Parameters.SpawnCollisionHandlingOverride =
            bSpawnEvenIfColliding ? ESpawnActorCollisionHandlingMethod::AlwaysSpawn
                                  : ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

        FTransform Transform = *Where;
        auto Spawned = SpawnActorInternal(UWorld::GetWorld(), GameObjectClass, &Transform, &Parameters);
        Ares::Log("gameobject: %s -> %s at %.0f %.0f %.0f", GameObjectClass->GetName().c_str(),
                  Spawned ? Spawned->GetName().c_str() : "<null>", Transform.Translation.X,
                  Transform.Translation.Y, Transform.Translation.Z);
        return (AGameObject*)Spawned;
    }
}

void LocalGameObjects::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Rel32Hook(ImageBase + 0x209ED7B, SpawnGameObject); // call inside the exec thunk, operand addr
}
