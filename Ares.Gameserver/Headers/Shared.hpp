#pragma once
#include "Hooking/Hooking.hpp"

inline std::vector<void (*)()> Initters;
inline std::vector<void (*)()> Tickers;

#define INIT_MODULE(Name)                                                                                                                                                                                             \
    auto __InitterAdder_##Name = ([]()                                                                                                                                                                                \
    {                                                                                                                                                                                                                 \
        Initters.push_back(Name## ::Init);                                                                                                                                                                            \
        return 1;                                                                                                                                                                                                     \
    })();

#define INIT_TICKER(Name)                                                                                                                                                                                             \
    auto __TickerAdder_##Name = ([]()                                                                                                                                                                                 \
    {                                                                                                                                                                                                                 \
        Tickers.push_back(Name## ::Tick);                                                                                                                                                                             \
        return 1;                                                                                                                                                                                                     \
    })();

#define callOG(_Tr, _Fn, _Th, ...)                                                                                                                                                                                    \
    ([&]()                                                                                                                                                                                                            \
    {                                                                                                                                                                                                                 \
        _Fn->ExecFunction = (void (*)(void*, void*, void*))_Th##OG;                                                                                                                                                   \
        _Tr->_Th(##__VA_ARGS__);                                                                                                                                                                                      \
        _Fn->ExecFunction = (void (*)(void*, void*, void*))_Th;                                                                                                                                                       \
    })()
#define callOGWithRet(_Tr, _Fn, _Th, ...)                                                                                                                                                                             \
    ([&]()                                                                                                                                                                                                            \
    {                                                                                                                                                                                                                 \
        _Fn->ExecFunction = (void (*)(void*, void*, void*))_Th##OG;                                                                                                                                                   \
        auto _Rt = _Tr->_Th(##__VA_ARGS__);                                                                                                                                                                           \
        _Fn->ExecFunction = (void (*)(void*, void*, void*))_Th;                                                                                                                                                       \
        return _Rt;                                                                                                                                                                                                   \
    })()

#define DefineHookWithOG(RetType, _Name, ...)                                                                                                                                                                         \
    static inline RetType (*_Name##OG)(##__VA_ARGS__);                                                                                                                                                                \
    static RetType _Name(##__VA_ARGS__);
#define DefineUHookWithOG(_Name)                                                                                                                                                                                      \
    static inline void (*_Name##OG)(UObject*, FFrame&);                                                                                                                                                               \
    static void _Name(UObject*, FFrame&);
#define DefineUHookWithOGAndRet(RetType, _Name)                                                                                                                                                                       \
    static inline void (*_Name##OG)(UObject*, FFrame&, RetType*);                                                                                                                                                     \
    static void _Name(UObject*, FFrame&, RetType*);

IInterface* GetInterfaceAddress(UObject* Object, UClass* Class);
template <typename T>
inline IInterface* GetInterfaceAddress(UObject* Object)
{
    return GetInterfaceAddress(Object, T::StaticClass());
}

UObject* FindObject(const wchar_t* ObjectPath, UClass* Class);

template <typename ObjectType>
inline ObjectType* FindObject(const wchar_t* ObjectPath, UClass* Class = ObjectType::StaticClass())
{
    return (ObjectType*)FindObject(ObjectPath, Class);
}

template <typename ObjectType>
inline ObjectType* FindObject(std::wstring ObjectPath, UClass* Class = ObjectType::StaticClass())
{
    return (ObjectType*)FindObject(ObjectPath.c_str(), Class);
}

template <typename ObjectType>
inline ObjectType* FindObject(std::string ObjectPath, UClass* Class = ObjectType::StaticClass())
{
    return (ObjectType*)FindObject(std::wstring(ObjectPath.begin(), ObjectPath.end()).c_str(), Class);
}

template <typename ObjectType>
inline ObjectType* FindObject(const char* ObjectPath, UClass* Class = ObjectType::StaticClass())
{
    return FindObject<ObjectType>(std::string(ObjectPath), Class);
}

FQuat RotatorToQuat(FRotator Rot);
FRotator QuatToRotator(FQuat Rot);
FTransform ConstructTrans(FVector Vec, FRotator Rot, FVector scale = {1, 1, 1});
FTransform ConstructTrans(FVector Vec, FQuat Rot, FVector scale = {1, 1, 1});

struct FActorSpawnParameters
{
    FName Name = FName(0);
    UObject* Template;
    UObject* Owner;
    AActor* Instigator;
    UObject* OverrideLevel;
    ESpawnActorCollisionHandlingMethod SpawnCollisionHandlingOverride;
    uint16 bRemoteOwned : 1;
    uint16 bNoFail : 1;
    uint16 bDeferConstruction : 1;
    uint16 bAllowDuringConstructionScript : 1;
    uint32_t ObjectFlags;
};

AActor* SpawnActor(UClass* Class, FTransform Transform, AActor* Owner = nullptr, ESpawnActorCollisionHandlingMethod CollisionOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
AActor* SpawnActor(UClass* Class, FVector Loc, FRotator Rot = {}, AActor* Owner = nullptr, ESpawnActorCollisionHandlingMethod CollisionOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

template <typename T>
inline T* SpawnActor(UClass* Class, FVector Loc, FRotator Rot = {}, AActor* Owner = nullptr)
{
    return (T*)SpawnActor(Class, Loc, Rot, Owner);
}

template <typename T>
inline T* SpawnActor(UClass* Class, FTransform& Transform, AActor* Owner = nullptr)
{
    return (T*)SpawnActor(Class, Transform, Owner);
}

template <typename T>
inline T* SpawnActor(FVector Loc, FRotator Rot = {}, AActor* Owner = nullptr)
{
    return (T*)SpawnActor(T::StaticClass(), Loc, Rot, Owner);
}

template <typename T>
inline T* SpawnActor(FTransform& Transform, AActor* Owner = nullptr)
{
    return (T*)SpawnActor(T::StaticClass(), Transform, Owner);
}

template <typename T = AActor>
static T* SpawnActorUnfinished(UClass* Class, FVector Loc, FRotator Rot = {}, AActor* Owner = nullptr)
{
    static auto SpawnActorInternal = (AActor * (*)(UWorld*, const UClass*, FVector*, FRotator*, void*))(ImageBase + 0x3652FC0);

    FActorSpawnParameters SpawnParameters{};

    SpawnParameters.Owner = Owner;
    SpawnParameters.bDeferConstruction = false;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    SpawnParameters.bDeferConstruction = true;

    return (T*)SpawnActorInternal(UWorld::GetWorld(), Class, &Loc, &Rot, &SpawnParameters);
}

template <typename T = AActor>
static T* FinishSpawnActor(T* Actor, FVector Loc, FRotator Rot)
{
    static auto FinishSpawning = (void (*)(AActor*, FTransform*, char, int))(ImageBase + 0x323AF10);

    auto Transform = ConstructTrans(Loc, Rot);

    FinishSpawning(Actor, &Transform, 0, 0);

    return Actor;
};

UAresAssetManager* GetAssetManager();

bool GuidEquals(FGuid& Guid, FGuid& OtherGuid);

template <typename T>
class FPointerWrapper
{
private:
    T* Ptr;

public:
    FPointerWrapper(T* InPtr)
        : Ptr(InPtr)
    {
    }

    T* GetPtr()
    {
        return Ptr;
    }
};

template <>
struct std::hash<FName>
{
    inline std::size_t operator()(const FName& k) const
    {
        return hash<int32>()(k.ComparisonIndex) ^ hash<int32>()(k.Number);
    }
};

template <typename IteratableType>
inline auto PickWeighted(IteratableType& Map, float (*RandFunc)(float), bool bCheckZero = true) -> typename IteratableType::iterator::value_type
{
    using T = typename IteratableType::iterator::value_type;

    float TotalWeight = std::accumulate(Map.begin(), Map.end(), 0.0f, [&](float acc, T p) { return acc + p->Weight; });
    float RandomNumber = RandFunc(TotalWeight);

    for (auto& Element : Map)
    {
        float Weight = Element->Weight;
        if (bCheckZero && Weight == 0)
            continue;

        if (RandomNumber <= Weight)
            return Element;

        RandomNumber -= Weight;
    }

    return nullptr;
}

template <typename T>
static inline void Set(uintptr_t Address, T Value)
{
    *reinterpret_cast<T*>(__int64(Address)) = Value;
}

class FOutputDevice
{
public:
    bool bSuppressEventTag;
    bool bAutoEmitLineTerminator;
    uint8_t _Padding1[0x6];
};

class FFrame : public FOutputDevice
{
public:
    void** VTable;
    UFunction* Node;
    UObject* Object;
    uint8* Code;
    uint8* Locals;
    void* MostRecentProperty;
    uint8_t* MostRecentPropertyAddress;
    uint8_t _Padding1[0x40];
    UField* PropertyChainForCompiledIn;

public:
    void IncrementCode()
    {
        Code = (uint8_t*)(__int64(Code) + (bool)Code);
    }
};

template <typename T>
__forceinline std::vector<T*> GetObjectsOfClass()
{
    std::vector<T*> Objects;
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = BasicFilesImpleUtils::GetObjectByIndex(i);

        if (!Object)
            continue;

        if (Object->IsA(T::StaticClass()))
        {
            Objects.push_back((T*)Object);
        }
    }
    return Objects;
}
