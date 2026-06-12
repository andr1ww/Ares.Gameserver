#pragma once
#include "pch.h"

/**
 * Struct to store an actor pointer and any internal metadata for that actor used
 * internally by a UNetDriver.
 */
struct FNetworkObjectInfo
{
    /** Pointer to the replicated actor. */
    AActor* Actor;

    /** WeakPtr to actor. This is cached here to prevent constantly constructing one when needed for (things like) keys in TMaps/TSets */
    TWeakObjectPtr<AActor> WeakActor;

    /** Next time to consider replicating the actor. Based on FPlatformTime::Seconds(). */
    double NextUpdateTime;

    /** Last absolute time in seconds since actor actually sent something during replication */
    double LastNetReplicateTime;

    /** Optimal delta between replication updates based on how frequently actor properties are actually changing */
    float OptimalNetUpdateDelta;

    /** Last time this actor was updated for replication via NextUpdateTime
     * @warning: internal net driver time, not related to WorldSettings.TimeSeconds */
    float LastNetUpdateTime;

    /** List of connections that this actor is dormant on */
    TSet<TWeakObjectPtr<UNetConnection>> DormantConnections;

    /** A list of connections that this actor has recently been dormant on, but the actor doesn't have a channel open yet.
     *  These need to be differentiated from actors that the client doesn't know about, but there's no explicit list for just those actors.
     *  (this list will be very transient, with connections being moved off the DormantConnections list, onto this list, and then off once the actor has a channel again)
     */
    TSet<TWeakObjectPtr<UNetConnection>> RecentlyDormantConnections;

    /** Is this object still pending a full net update due to clients that weren't able to replicate the actor at the time of LastNetUpdateTime */
    uint8 bPendingNetUpdate : 1;

    /** Force this object to be considered relevant for at least one update */
    uint8 bForceRelevantNextUpdate : 1;

    /** Should this object be considered for replay checkpoint writes */
    uint8 bDirtyForReplay : 1;

    /** Force this object to be considered relevant for at least one update */
    uint32 ForceRelevantFrame = 0;
};
template <class ObjectType>
class TSharedPtr
{
public:
    ObjectType* Object;
    void* ReferenceController;

    bool IsValid() const
    {
        return Object != nullptr;
    }
    ObjectType* Get()
    {
        return Object;
    }
    ObjectType* Get() const
    {
        return Object;
    }
    ObjectType& operator*()
    {
        return *Object;
    }
    const ObjectType& operator*() const
    {
        return *Object;
    }
    ObjectType* operator->()
    {
        return Object;
    }
    ObjectType* operator->() const
    {
        return Object;
    }
};
/**
 * Stores the list of replicated actors for a given UNetDriver.
 */
class FNetworkObjectList
{
public:
    typedef TSet<TSharedPtr<FNetworkObjectInfo>> FNetworkObjectSet;

    FNetworkObjectSet AllNetworkObjects;
    FNetworkObjectSet ActiveNetworkObjects;
    FNetworkObjectSet ObjectsDormantOnAllConnections;

    TMap<TWeakObjectPtr<UNetConnection>, int32> NumDormantObjectsPerConnection;
};

class NetDriver
{
public:
    static void Init();

private:
    static void TickFlush(UNetDriver* _this, float DeltaTime);
    static int32 ServerReplicateActors(UNetDriver* _this, float DeltaSeconds);
    static int32 ServerReplicateActors_PrepConnections(UNetDriver*, float);
    static void ServerReplicateActors_BuildConsiderList(UNetDriver*, std::vector<AActor*>&, const float);
    static int32 ServerReplicateActors_ProcessActors(UNetDriver*, UNetConnection*, std::vector<AActor*>&);
};