#include <pch.h>

IInterface* GetInterfaceAddress(UObject* Object, UClass* Class)
{
    static auto GetInterfaceAddress_ = (IInterface * (*)(UObject*, UClass*))(ImageBase + 0x246F870);
    return GetInterfaceAddress_(Object, Class);
}

__forceinline UObject* StaticFindObject(const wchar_t* ObjectPath, UClass* Class)
{
    auto StaticFindObjectInternal = (UObject * (*)(const UClass*, UObject*, const wchar_t*, bool))(ImageBase + 0x24860A0);
    return StaticFindObjectInternal(Class, nullptr, ObjectPath, false);
}

__forceinline UObject* StaticLoadObject(const wchar_t* ObjectPath, UClass* InClass, UObject* Outer = nullptr)
{
    auto StaticLoadObjectInternal = (UObject * (*)(const UClass*, UObject*, const wchar_t*, const wchar_t*, uint32_t, UObject*, bool, void*))(ImageBase + 0x2486B30);
    return StaticLoadObjectInternal(InClass, Outer, ObjectPath, nullptr, 0, nullptr, true, nullptr);
}

UObject* FindObject(const wchar_t* ObjectPath, UClass* Class)
{
    auto Object = StaticFindObject(ObjectPath, Class);

    if (Object)
        return Object;

    return StaticLoadObject(ObjectPath, Class);
}

FQuat RotatorToQuat(FRotator Rot)
{
    float halfOfARadian = 0.008726646259971648f;
    float sinPitch = sin(Rot.Pitch * halfOfARadian), sinYaw = sin(Rot.Yaw * halfOfARadian), sinRoll = sin(Rot.Roll * halfOfARadian);
    float cosPitch = cos(Rot.Pitch * halfOfARadian), cosYaw = cos(Rot.Yaw * halfOfARadian), cosRoll = cos(Rot.Roll * halfOfARadian);

    FQuat out{};
    out.X = cosRoll * sinPitch * sinYaw - sinRoll * cosPitch * cosYaw;
    out.Y = -cosRoll * sinPitch * cosYaw - sinRoll * cosPitch * sinYaw;
    out.Z = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;
    out.W = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;
    return out;
}

float ClampAxis(float Angle)
{
    Angle = fmod(Angle, 360.f); // rat

    if (Angle < 0.)
        Angle += 360.;
    return Angle;
}

float NormalizeAxis(float Angle)
{
    Angle = ClampAxis(Angle);

    if (Angle > 180.)
        Angle -= 360.;
    return Angle;
}

FRotator QuatToRotator(FQuat Quat)
{
    const float SingularityTest = Quat.Z * Quat.X - Quat.W * Quat.Y;
    const float YawY = 2.f * (Quat.W * Quat.Z + Quat.X * Quat.Y);
    const float YawX = (1.f - 2.f * ((Quat.Y * Quat.Y) + (Quat.Z * Quat.Z)));

    const float SINGULARITY_THRESHOLD = 0.4999995f;
    const float RAD_TO_DEG = 57.29577951308232f;
    FRotator RotatorFromQuat{};

    if (SingularityTest < -SINGULARITY_THRESHOLD)
    {
        RotatorFromQuat.Pitch = -90.;
        RotatorFromQuat.Yaw = atan2(YawY, YawX) * RAD_TO_DEG;
        RotatorFromQuat.Roll = NormalizeAxis(-RotatorFromQuat.Yaw - (2.f * atan2(Quat.X, Quat.W) * RAD_TO_DEG));
    }
    else if (SingularityTest > SINGULARITY_THRESHOLD)
    {
        RotatorFromQuat.Pitch = 90.;
        RotatorFromQuat.Yaw = atan2(YawY, YawX) * RAD_TO_DEG;
        RotatorFromQuat.Roll = NormalizeAxis(RotatorFromQuat.Yaw - (2.f * atan2(Quat.X, Quat.W) * RAD_TO_DEG));
    }
    else
    {
        RotatorFromQuat.Pitch = asin(2.f * SingularityTest) * RAD_TO_DEG;
        RotatorFromQuat.Yaw = atan2(YawY, YawX) * RAD_TO_DEG;
        RotatorFromQuat.Roll = atan2(-2.f * (Quat.W * Quat.X + Quat.Y * Quat.Z), (1.f - 2.f * ((Quat.X * Quat.X) + (Quat.Y * Quat.Y)))) * RAD_TO_DEG;
    }

    return RotatorFromQuat;
}

FTransform ConstructTrans(FVector Vec, FRotator Rot, FVector scale)
{
    FTransform Trans;

    Trans.Rotation = RotatorToQuat(Rot);
    Trans.Translation = Vec;
    Trans.Scale3D = scale;

    return Trans;
}

FTransform ConstructTrans(FVector Vec, FQuat Rot, FVector scale)
{
    FTransform Trans;

    Trans.Rotation = Rot;
    Trans.Translation = Vec;
    Trans.Scale3D = scale;

    return Trans;
}

AActor* SpawnActor(UClass* Class, FTransform Transform, AActor* Owner, ESpawnActorCollisionHandlingMethod CollisionOverride)
{
    static auto SpawnActorInternal = (AActor * (*)(UWorld*, const UClass*, FTransform*, void*))(ImageBase + 0x3651AC0);

    FActorSpawnParameters SpawnParameters{};

    SpawnParameters.Owner = Owner;
    SpawnParameters.bDeferConstruction = false;
    SpawnParameters.SpawnCollisionHandlingOverride = CollisionOverride;

    return SpawnActorInternal(UWorld::GetWorld(), Class, &Transform, &SpawnParameters);
}

AActor* SpawnActor(UClass* Class, FVector Loc, FRotator Rot, AActor* Owner, ESpawnActorCollisionHandlingMethod CollisionOverride)
{
    static auto SpawnActorInternal = (AActor * (*)(UWorld*, const UClass*, FVector*, FRotator*, void*))(ImageBase + 0x3652FC0);

    FActorSpawnParameters SpawnParameters{};

    SpawnParameters.Owner = Owner;
    SpawnParameters.bDeferConstruction = false;
    SpawnParameters.SpawnCollisionHandlingOverride = CollisionOverride;

    return SpawnActorInternal(UWorld::GetWorld(), Class, &Loc, &Rot, &SpawnParameters);
}

UAresAssetManager* GetAssetManager()
{
    static UAresAssetManager* AssetManager = nullptr;

    if (!AssetManager)
        AssetManager = (UAresAssetManager*)UEngine::GetEngine()->AssetManager;

    return AssetManager;
}

bool GuidEquals(FGuid& Guid, FGuid& OtherGuid)
{
    return Guid.A == OtherGuid.A && Guid.B == OtherGuid.B && Guid.C == OtherGuid.C && Guid.D == OtherGuid.D;
}