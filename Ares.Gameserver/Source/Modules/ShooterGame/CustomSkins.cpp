#include <pch.h>
#include <Headers/Modules/ShooterGame/CustomSkins.h>
#include <Headers/Modules/ShooterGame/LocalSkins.h>
#include <Headers/Modules/ShooterGame/BenjaminGeometry.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <mmsystem.h>

namespace
{
    constexpr float Pi = 3.14159265359f;
    struct FHidden { TWeakObjectPtr<UMeshComponent> Mesh; bool Visible; };
    struct FPiece { TWeakObjectPtr<UProceduralMeshComponent> Mesh; TWeakObjectPtr<USkeletalMeshComponent> Rig; int Part; };
    struct FEffectOverride
    {
        TSoftClassPtr<UClass> Key;
        UClass* Previous = nullptr;
        bool Existed = false;
    };
    struct FAttachmentOverride
    {
        TWeakObjectPtr<UActorComponent> Component;
        int Offset;
        TWeakObjectPtr<UClass> Original;
    };
    struct FWorn
    {
        TWeakObjectPtr<AAresEquippable> Weapon;
        std::vector<FPiece> Pieces;
        std::array<TWeakObjectPtr<UMaterialInstanceDynamic>,9> Materials;
        std::vector<FHidden> Hidden;
        TWeakObjectPtr<UAmmoComponent> Ammo;
        TWeakObjectPtr<UClass> OriginalBanner;
        std::vector<FEffectOverride> Effects;
        std::vector<FAttachmentOverride> Attachments;
        int LastAmmo = -1;
        int Shots = 0, Tracers = 0, Optics = 0, MutedSounds = 0, BannerReplacements = 0;
        bool Equipped = false;
        std::string State;
        FRotator Alignment{};
        FVector Offset{-64,0,-5};
        FName Socket;
        float Zoom = 0;
        double Started = 0, Reload = -100, Equip = -100, Inspect = -100, Fire = -100, Kill = -100;
    };
    std::unordered_map<AAresEquippable*, FWorn> Dressed;
    std::string Lower(std::string S) { std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C){return (char)tolower(C);}); return S; }
    bool Busy = false;
    enum class EEvent { Other, Shot, Inspect, Sound, Banner, Tracer, Effect, Reset, Kill };
    EEvent Event(UFunction* Function)
    {
        static std::unordered_map<UFunction*, EEvent> Cache;
        auto It=Cache.find(Function); if(It!=Cache.end()) return It->second;
        auto Name=Function->GetName(); EEvent Kind=EEvent::Other;
        if(Name=="Custom_OnShot") Kind=EEvent::Shot;
        else if(Name=="GunInspect" || Name=="LocalPlayInspect") Kind=EEvent::Inspect;
        else if(Name=="TriggerWorldEquippableSound" || Name=="TriggerCharacterEquippableSound") Kind=EEvent::Sound;
        else if(Name=="OnAnyPlayerKilled") Kind=EEvent::Banner;
        else if(Name=="TracerStartEffect") Kind=EEvent::Tracer;
        else if(Name=="StartEffect" || Name=="ReceiveBeginPlay") Kind=EEvent::Effect;
        else if(Name=="ResetEffect" || Name=="OnResetEffect") Kind=EEvent::Reset;
        else if(Name=="MulticastNotifyKilledEnemy") Kind=EEvent::Kill;
        Cache.emplace(Function,Kind); return Kind;
    }
    // by name, bp param packing varies
    UObject* ObjectParam(UFunction* Function,void* Parameters,const char* Name)
    {
        if(!Parameters) return nullptr;
        for(auto Field=Function->Children;Field;Field=Field->Next)
            if(auto P=Field->Cast<UObjectProperty>(); P && (P->PropertyFlags&0x80) && P->GetName()==Name)
                return *(UObject**)((uint8*)Parameters+P->Offset);
        return nullptr;
    }
    FWorn* ForWeapon(AAresEquippable* Weapon)
    {
        auto It=Dressed.find(Weapon);
        return It!=Dressed.end() && It->second.Weapon.Get()==Weapon ? &It->second : nullptr;
    }
    FWorn* ForPawn(AShooterCharacter* Pawn)
    {
        return Pawn && Pawn->Inventory ? ForWeapon(Pawn->Inventory->CurrentEquippable) : nullptr;
    }
    FWorn* ForContext(UObject* Object)
    {
        for(auto O=Object;O;O=O->Outer)
            if(auto W=ForWeapon((AAresEquippable*)O)) return W;
        if(auto C=Object?Object->Cast<UActorComponent>():nullptr) Object=C->GetOwner();
        auto Actor=Object?Object->Cast<AActor>():nullptr;
        for(int i=0;Actor && i<8;++i,Actor=Actor->GetOwner())
        {
            if(auto W=ForWeapon((AAresEquippable*)Actor)) return W;
            if(auto Pawn=Actor->Cast<AShooterCharacter>()) return ForPawn(Pawn);
        }
        return nullptr;
    }
    struct FPresentation
    {
        UClass* Tracer = nullptr;
        UClass* Optic = nullptr;
        UClass* Banner = nullptr;
        UStructProperty* BannerData = nullptr;
        std::array<UTexture2D*,7> Textures{};
    } Presentation;
    enum ETexture { Frame, Badge, Pip, Reticle, TracerMask, TracerLUT, Dissolve };
    bool LoadPresentation(UObject* Context,const std::filesystem::path& Folder)
    {
        if(Presentation.BannerData) return true;
        auto& P=Presentation;
        P.Tracer=FindObject<UClass>(L"/Game/Equippables/Guns/_Core/Tracers/Tracer_Hype_1P.Tracer_Hype_1P_C");
        P.Optic=FindObject<UClass>(L"/Game/Equippables/Guns/Attachments/RedDot/HypeBeast/FXC_RedDot_Hype_VFX.FXC_RedDot_Hype_VFX_C");
        for(auto C:GetObjectsOfClass<UClass>())
            if(C->GetName()=="KillBannerData_Base_C") { P.Banner=C; break; }
        if(!P.Tracer || !P.Optic || !P.Banner || !P.Banner->DefaultObject) return false;
        const wchar_t* Names[]={L"banner-frame.png",L"banner-badge.png",L"banner-pip.png",L"reticle.png",L"tracer-mask.png",L"tracer-lut.png",L"dissolve.png"};
        for(int i=0;i<7;++i)
        {
            if(!P.Textures[i])
            {
                auto Path=(Folder/Names[i]).wstring();
                P.Textures[i]=UKismetRenderingLibrary::ImportFileAsTexture2D(Context,FString(Path.c_str()));
                if(!P.Textures[i]) return false;
                P.Textures[i]->AddToRoot();
            }
        }
        for(auto S=(UStruct*)P.Banner;S;S=S->Super)
            for(auto F=S->Children;F;F=F->Next)
                if(auto Property=F->Cast<UStructProperty>(); Property && Property->GetName()=="KillBannerData")
                    P.BannerData=Property;
        if(!P.BannerData) return false;
        P.Tracer->AddToRoot(); P.Optic->AddToRoot(); P.Banner->AddToRoot();
        return true;
    }
    void Override(FWorn& W,const wchar_t* Path,UClass* Replacement)
    {
        FEffectOverride Saved{}; Saved.Key.ObjectID.AssetPathName=FName(Path);
        auto Weapon=W.Weapon.Get();
        for(auto& Entry:Weapon->EffectOverrides)
            if(Entry.Key().ObjectID.AssetPathName==Saved.Key.ObjectID.AssetPathName)
            { Saved.Existed=true; Saved.Previous=Entry.Value(); break; }
        W.Effects.push_back(Saved);
        Weapon->OverrideEffect(Saved.Key,Replacement);
    }
    bool ConfigureOptic(FWorn& W)
    {
        auto Components=W.Weapon.Get()->GetComponentsByClass(UAresWeaponAttachmentComponent::StaticClass());
        for(auto C:Components)
            for(auto S=(UStruct*)C->Class;S;S=S->Super)
                for(auto F=S->Children;F;F=F->Next)
                    if(auto P=F->Cast<UClassProperty>(); P && P->GetName()=="Red Dot VFX")
                    {
                        auto Slot=(UClass**)((uint8*)C+P->Offset);
                        W.Attachments.push_back({TWeakObjectPtr<UActorComponent>(C),P->Offset,TWeakObjectPtr<UClass>(*Slot)});
                        *Slot=Presentation.Optic;
                    }
        Components.Free();
        Ares::Log("benjamin native optic: configured %d red-dot attachment(s)",(int)W.Attachments.size());
        return !W.Attachments.empty();
    }
    // patch the shared banner CDO for one dispatch only, restored in EndBanner
    struct FBannerScope
    {
        UObject* Object;
        UFunction* Function;
        void* Parameters;
        uint8* Data;
        std::vector<uint8> Original;
    };
    std::vector<FBannerScope> BannerScopes;
    void BeginBanner(UObject* Object,UFunction* Function,void* Parameters)
    {
        auto Property=Presentation.BannerData;
        auto Data=(uint8*)Presentation.Banner->DefaultObject+Property->Offset;
        BannerScopes.push_back({Object,Function,Parameters,Data,{Data,Data+Property->ElementSize}});
        int Textures=0;
        for(auto F=Property->Struct->Children;F;F=F->Next)
        {
            auto P=F->Cast<UProperty>(); if(!P) continue;
            auto Name=P->GetName(); auto At=Data+P->Offset;
            if(P->IsA<UObjectProperty>())
            {
                UTexture2D* Texture=nullptr;
                if(Name.find("BackgroundFrame_TXT")==0 || Name.find("KillWheel-TXT")==0) Texture=Presentation.Textures[Frame];
                else if(Name.find("Badge_Default_TXT")==0) Texture=Presentation.Textures[Badge];
                else if(Name.find("KillWheel_Slice_")==0) Texture=Presentation.Textures[Pip];
                else if(Name.find("BackgroundFrame_DSLV")==0 || Name.find("Badge_DSLV")==0) Texture=Presentation.Textures[Dissolve];
                if(Texture) { *(UObject**)At=Texture; ++Textures; }
                else if(Name.find("Sound_")==0 || Name.find("OptionalEffects")==0) *(UObject**)At=nullptr;
            }
            else if(Name.find("PrimaryColor")==0 && P->ElementSize==sizeof(FLinearColor))
                *(FLinearColor*)At={.58f,1.f,.045f,1};
            else if(Name.find("FlipBookFX")==0 && P->IsA<UArrayProperty>())
                memset(At,0,P->ElementSize); // restored after, never freed
        }
        Ares::Log("benjamin native banner: %d original textures supplied to KillBannerAnnouncement",Textures);
    }
    void EndBanner(UObject* Object,UFunction* Function,void* Parameters)
    {
        if(BannerScopes.empty()) return;
        auto& S=BannerScopes.back();
        if(S.Object!=Object || S.Function!=Function || S.Parameters!=Parameters) return;
        memcpy(S.Data,S.Original.data(),S.Original.size());
        BannerScopes.pop_back();
    }
    struct FBoundMaterial
    {
        TWeakObjectPtr<UParticleSystemComponent> Component;
        TWeakObjectPtr<AAresEquippable> Weapon;
        int Slot;
        TWeakObjectPtr<UMaterialInterface> Original;
        TWeakObjectPtr<UMaterialInstanceDynamic> Material;
    };
    std::vector<FBoundMaterial> BoundMaterials;
    void RestoreEffect(AEffectContainer* Effect)
    {
        for(auto It=BoundMaterials.begin();It!=BoundMaterials.end();)
        {
            auto C=It->Component.Get();
            if(!C || C->GetOwner()==Effect)
            {
                if(C && C->GetMaterial(It->Slot)==It->Material.Get()) C->SetMaterial(It->Slot,It->Original.Get());
                It=BoundMaterials.erase(It);
            }
            else ++It;
        }
    }
    void RestoreMaterials(AAresEquippable* Weapon)
    {
        for(auto It=BoundMaterials.begin();It!=BoundMaterials.end();)
        {
            auto C=It->Component.Get();
            if(!C || It->Weapon.Get()==Weapon)
            {
                if(C && C->GetMaterial(It->Slot)==It->Material.Get()) C->SetMaterial(It->Slot,It->Original.Get());
                It=BoundMaterials.erase(It);
            }
            else ++It;
        }
    }
    FWorn* ForEffect(AEffectContainer* Effect)
    {
        if(auto W=ForContext(Effect)) return W;
        if(auto W=ForContext(Effect->OriginalTarget)) return W;
        for(auto& Value:Effect->EffectData.ObjectValues)
            if(auto W=ForContext(Value.Object.Get())) return W;
        return nullptr;
    }
    void BindParticles(FWorn& W,AActor* Owner,bool Tracer)
    {
        auto Components=Owner->GetComponentsByClass(UParticleSystemComponent::StaticClass());
        int Changed=0;
        for(auto Component:Components)
        {
            auto C=Component->Cast<UParticleSystemComponent>();
            if(!C || !C->Template) continue;
            auto Name=C->Template->GetName();
            if(!Tracer && Name.find("ADS_Hype")==std::string::npos) continue;
            for(int Slot=0;Slot<C->Template->Emitters.Num();++Slot)
            {
                auto E=C->Template->Emitters[Slot];
                if(!E || E->LODLevels.Num()==0 || !E->LODLevels[0] || !E->LODLevels[0]->RequiredModule) continue;
                bool Already=false;
                for(auto& B:BoundMaterials)
                    if(B.Component.Get()==C && B.Slot==Slot && C->GetMaterial(Slot)==B.Material.Get()) { Already=true; break; }
                if(Already) continue;
                auto Source=C->GetMaterial(Slot);
                if(!Source) Source=E->LODLevels[0]->RequiredModule->Material;
                if(!Source) continue;
                auto MID=C->CreateDynamicMaterialInstance(Slot,Source,FName());
                if(!MID) continue;
                MID->SetTextureParameterValue(FName(L"Emissive_LUT_Base"),Presentation.Textures[TracerLUT]);
                if(!Tracer)
                {
                    MID->SetTextureParameterValue(FName(L"Emissive_Base"),Presentation.Textures[TracerLUT]);
                    MID->SetTextureParameterValue(FName(L"Alpha_Base"),Presentation.Textures[Reticle]);
                    MID->SetScalarParameterValue(FName(L"Emissive_Base_Power"),2.f);
                    MID->SetVectorParameterValue(FName(L"Color"),{.58f,1.f,.045f,1});
                }
                // tracer only gets the LUT, custom alpha makes the beam invisible
                BoundMaterials.push_back({TWeakObjectPtr<UParticleSystemComponent>(C),W.Weapon,Slot,
                    TWeakObjectPtr<UMaterialInterface>(Source),TWeakObjectPtr<UMaterialInstanceDynamic>(MID)});
                ++Changed;
            }
        }
        Components.Free();
        if(Changed)
        {
            if(Tracer) ++W.Tracers; else ++W.Optics;
            if((Tracer?W.Tracers:W.Optics)<=4)
                Ares::Log("benjamin native %s: %s, %d material slots",Tracer?"tracer":"optic",Owner->GetName().c_str(),Changed);
        }
    }
    void BindEffect(FWorn& W,AEffectContainer* Effect)
    {
        if(Effect->IsA(Presentation.Tracer)) BindParticles(W,Effect,true);
        else if(Effect->IsA(Presentation.Optic))
        { BindParticles(W,Effect,false); BindParticles(W,W.Weapon.Get(),false); }
    }
    FVector Rotate(FVector V, FRotator R)
    {
        auto Q=RotatorToQuat(R);
        FVector T{2*(Q.Y*V.Z-Q.Z*V.Y),2*(Q.Z*V.X-Q.X*V.Z),2*(Q.X*V.Y-Q.Y*V.X)};
        return {V.X+Q.W*T.X+Q.Y*T.Z-Q.Z*T.Y,V.Y+Q.W*T.Y+Q.Z*T.X-Q.X*T.Z,V.Z+Q.W*T.Z+Q.X*T.Y-Q.Y*T.X};
    }

    // multiple voices so shots can overlap
    struct FVoice { HWAVEOUT Device = nullptr; WAVEHDR Header{}; std::vector<short> Samples; };
    std::array<FVoice, 12> Voices;
    using OpenFn = decltype(&waveOutOpen); using PrepareFn = decltype(&waveOutPrepareHeader);
    using WriteFn = decltype(&waveOutWrite); using UnprepareFn = decltype(&waveOutUnprepareHeader);
    OpenFn OpenWave = nullptr; PrepareFn PrepareWave = nullptr; WriteFn WriteWave = nullptr; UnprepareFn UnprepareWave = nullptr;
    void Sound(int Kind)
    {
        static bool Tried = false;
        if (!Tried)
        {
            Tried = true;
            auto Library = LoadLibraryW(L"winmm.dll");
            if (Library) { OpenWave = (OpenFn)GetProcAddress(Library,"waveOutOpen"); PrepareWave = (PrepareFn)GetProcAddress(Library,"waveOutPrepareHeader"); WriteWave = (WriteFn)GetProcAddress(Library,"waveOutWrite"); UnprepareWave = (UnprepareFn)GetProcAddress(Library,"waveOutUnprepareHeader"); }
        }
        if (!OpenWave || !PrepareWave || !WriteWave || !UnprepareWave) return;
        float Volume=1;
        static TWeakObjectPtr<UAresSettingsManager> Settings;
        if(!Settings.Get()) for(auto M:GetObjectsOfClass<UAresSettingsManager>())
            if(!M->IsDefaultObject()) { Settings=TWeakObjectPtr<UAresSettingsManager>(M); break; }
        if(auto M=Settings.Get()) Volume=std::clamp(M->GetFloatSetting(EAresFloatSettingName::MasterVolume),0.f,1.f)*
            std::clamp(M->GetFloatSetting(EAresFloatSettingName::SoundEffectsVolume),0.f,1.f);
        for (auto& Voice : Voices)
        {
            if (Voice.Header.dwFlags & WHDR_INQUEUE) continue;
            if (!Voice.Device)
            {
                WAVEFORMATEX Format{WAVE_FORMAT_PCM,1,22050,44100,2,16,0};
                if (OpenWave(&Voice.Device,WAVE_MAPPER,&Format,0,0,CALLBACK_NULL) != MMSYSERR_NOERROR) return;
            }
            if (Voice.Header.dwFlags & WHDR_PREPARED) UnprepareWave(Voice.Device,&Voice.Header,sizeof(WAVEHDR));
            const float Duration = Kind==0 ? .20f : Kind==2 ? .72f : Kind==3 ? .85f : .42f;
            Voice.Samples.resize((int)(22050*Duration));
            unsigned Seed = 0x7639;
            for (size_t i=0;i<Voice.Samples.size();++i)
            {
                float T = (float)i/22050, P=T/Duration;
                Seed=1664525*Seed+1013904223;
                float Noise = (float)((Seed>>16)&65535)/32768-1;
                float Tone=0,Env=0;
                if(Kind==0)
                {
                    Tone=.55f*sinf(2*Pi*(155*T-240*T*T))+.22f*sinf(2*Pi*420*T)*expf(-18*T)+Noise*.35f;
                    Env=expf(-25*T)*std::min(1.f,T*1200);
                }
                else
                {
                    float Base=Kind==4?720.f:Kind==3?105.f:Kind==2?180.f:140.f;
                    float Warble=Kind==3?2.4f:Kind==2?7.f:4.f;
                    float Phase=2*Pi*Base*T+2.5f*sinf(2*Pi*Warble*T)+(Kind==4?900*T*T:-70*T*T);
                    Tone=.48f*sinf(Phase)+.19f*sinf(Phase*2)+.12f*sinf(Phase*3)+Noise*.06f;
                    Env=sinf(Pi*P)*expf(-1.6f*P)*(.75f+.25f*sinf(2*Pi*Warble*T));
                }
                Voice.Samples[i]=(short)(Tone*Env*11000*Volume);
            }
            Voice.Header={}; Voice.Header.lpData=(LPSTR)Voice.Samples.data(); Voice.Header.dwBufferLength=(DWORD)(Voice.Samples.size()*2);
            if (PrepareWave(Voice.Device,&Voice.Header,sizeof(WAVEHDR)) == MMSYSERR_NOERROR) WriteWave(Voice.Device,&Voice.Header,sizeof(WAVEHDR));
            return;
        }
    }
    bool Held(AAresEquippable* W) { return W && W->MyPawn && W->MyPawn->Inventory && W->MyPawn->Inventory->CurrentEquippable==W; }
    std::string State(AAresEquippable* W)
    {
        std::string S;
        for(auto M:W->EquippableStateMachines) if(M && M->AuthCurrentState.CurrentState) S+=Lower(M->AuthCurrentState.CurrentState->GetName())+" ";
        return S;
    }
    void Remove(FWorn& W)
    {
        RestoreMaterials(W.Weapon.Get());
        for(auto& Saved:W.Attachments)
            if(auto C=Saved.Component.Get()) *(UClass**)((uint8*)C+Saved.Offset)=Saved.Original.Get();
        if(auto Weapon=W.Weapon.Get()) for(auto& Saved:W.Effects)
        {
            if(Saved.Existed) Weapon->OverrideEffect(Saved.Key,Saved.Previous);
            else for(auto It=Weapon->EffectOverrides.begin();It!=Weapon->EffectOverrides.end();++It)
                if(It->Key().ObjectID.AssetPathName==Saved.Key.ObjectID.AssetPathName)
                { Weapon->EffectOverrides.Elements.Remove(It.GetIndex()); break; }
        }
        for(auto& P:W.Pieces) if(auto M=P.Mesh.Get()) M->K2_DestroyComponent(W.Weapon.Get());
        for(auto& H:W.Hidden) if(auto M=H.Mesh.Get()) M->SetVisibility(H.Visible,false);
        if(auto Weapon=W.Weapon.Get()) Weapon->SkinKillBannerData=W.OriginalBanner.Get();
    }
    float Arc(double Now,double Start,float Duration) { float P=(float)(Now-Start)/Duration; return P>=0 && P<1 ? sinf(P*Pi):0; }

    // anim notifies call this from the VM, skips PE, still eat the args
    void (*EquippableSoundOG)(UObject*,FFrame&,UAresAudioComponent**) = nullptr;
    void EquippableSound(UObject* Object,FFrame& Stack,UAresAudioComponent** Result)
    {
        auto W=ForWeapon((AAresEquippable*)Object);
        if(!W) { EquippableSoundOG(Object,Stack,Result); return; }
        UAkAudioEvent* Event=nullptr; FName Socket; uint32 Follow=0; UAresAudioComponent* Reuse=nullptr;
        Stack.StepCompiledIn(&Event); Stack.StepCompiledIn(&Socket);
        Stack.StepCompiledIn(&Follow); Stack.StepCompiledIn(&Reuse); Stack.IncrementCode();
        if(Result) *Result=nullptr;
        ++W->MutedSounds;
    }
    void InstallSoundHook()
    {
        if(EquippableSoundOG) return;
        Hooking::ExecHook(AAresEquippable::StaticClass()->FindFunction("PlayEquippableSoundEvent"),EquippableSound,EquippableSoundOG);
    }

}

const std::vector<CustomSkins::FDefinition>& CustomSkins::All()
{
    static std::vector<FDefinition> Definitions{{"benjamin","vandal","Benjamin","living Vandal, sleepy face, purple carapace and lime bioluminescence"}};
    return Definitions;
}
const CustomSkins::FDefinition* CustomSkins::Find(const std::string& Name,const std::string& Weapon,std::string& Why)
{
    Why.clear();
    if(Lower(Name)!="benjamin" && Lower(Name)!="placeholder" && Lower(Name)!="placeholder vandal") return nullptr;
    if(Lower(Weapon)!="vandal") { Why="Benjamin is for the vandal, not the "+Weapon; return nullptr; }
    return &All()[0];
}
bool CustomSkins::Apply(AAresEquippable*& Weapon,const FDefinition&,std::string& Report)
{
    if(!Weapon || Weapon->Class->GetName()!="AssaultRifle_AK_C") { Report="Benjamin requires a Vandal"; return false; }
    FReentrancyGuard Guard(Busy);
    if(!Guard.Entered()) { Report="skin update already in progress"; return false; }
    auto Material=FindObject<UMaterialInterface>(L"/Engine/EngineMaterials/Widget3DPassThrough_Opaque.Widget3DPassThrough_Opaque");
    if(!Material) { Report="benjamin mesh/material could not load"; return false; }
    std::array<UTexture2D*,9> Textures{};
    auto Folder=std::filesystem::path(Ares::Config().LogPath).parent_path().parent_path()/"assets"/"benjamin";
    if(!BenjaminGeometry::Load(Folder,Report)) return false;
    if(!LoadPresentation(Weapon,Folder)) { Report="benjamin native presentation resources could not load"; return false; }
    const wchar_t* Names[]={L"shell.png",L"skin.png",L"eye.png",L"pupil.png",L"glow.png",L"socket.png",L"tooth.png",L"lip.png",L"brass.png"};
    for(int i=0;i<9;++i)
    {
        auto Path=(Folder/Names[i]).wstring();
        Textures[i]=UKismetRenderingLibrary::ImportFileAsTexture2D(Weapon,FString(Path.c_str()));
        if(!Textures[i]) { Report="benjamin texture could not load: "+(Folder/Names[i]).string(); return false; }
    }
    if(Weapon->SkinDataAsset)
    {
        auto Fresh=LocalSkins::Replace(Weapon);
        if(!Fresh) { Report="finish the current action before changing skins"; return false; }
        Clear(Weapon); Weapon=Fresh;
    }
    else Clear(Weapon);
    FWorn W{}; W.Weapon=TWeakObjectPtr<AAresEquippable>(Weapon); W.Started=Ares::Seconds();
    W.OriginalBanner=TWeakObjectPtr<UClass>(Weapon->SkinKillBannerData);
    std::string Socket="Muzzle";
    std::ifstream Pose(Folder/"rig.txt");
    if(Pose && !(Pose>>Socket>>W.Alignment.Pitch>>W.Alignment.Yaw>>W.Alignment.Roll>>W.Offset.X>>W.Offset.Y>>W.Offset.Z))
    { Report="invalid benjamin rig.txt"; return false; }
    W.Socket=FName(std::wstring(Socket.begin(),Socket.end()).c_str());
    std::vector<USkeletalMeshComponent*> Rigs;
    auto Components=Weapon->GetComponentsByClass(UMeshComponent::StaticClass());
    for(auto C:Components)
    {
        auto M=C->Cast<UMeshComponent>(); if(!M) continue;
        auto R=M->Cast<USkeletalMeshComponent>();
        if(R && R->SkeletalMesh && R->SkeletalMesh->GetName()=="GN_AK_S0_Skelmesh") Rigs.push_back(R);
        else W.Hidden.push_back({TWeakObjectPtr<UMeshComponent>(M),!!M->bVisible});
    }
    Components.Free();
    if(Rigs.empty()) { Report="Vandal animation rig not found"; return false; }
    if(!ConfigureOptic(W)) { Report="Vandal red-dot attachment not found"; return false; }
    const auto& Parts=BenjaminGeometry::Parts();
    for(auto Rig:Rigs)
    {
        for(int i=0;i<(int)Parts.size();++i)
        {
            const auto& P=Parts[i];
            auto Mesh=BenjaminGeometry::Create(Weapon,P);
            if(!Mesh) { Remove(W); Report="could not register Benjamin's sculpted mesh"; return false; }
            W.Pieces.push_back({TWeakObjectPtr<UProceduralMeshComponent>(Mesh),TWeakObjectPtr<USkeletalMeshComponent>(Rig),i});
            Mesh->SetIsFirstPerson(Rig->bIsFirstPerson);
            Mesh->K2_AttachToComponent(Rig,P.Motion==9?FName(L"Magazine_Main"):W.Socket,
                EAttachmentRule::SnapToTarget,EAttachmentRule::SnapToTarget,EAttachmentRule::KeepWorld,false);
            auto MID=W.Materials[P.Material].Get();
            if(!MID)
            {
                MID=Mesh->CreateDynamicMaterialInstance(0,Material,FName());
                if(!MID) { Remove(W); Report="could not create Benjamin material"; return false; }
                MID->SetTextureParameterValue(FName(L"SlateUI"),Textures[P.Material]);
                MID->SetVectorParameterValue(FName(L"TintColorAndOpacity"),{1,1,1,1});
                MID->SetScalarParameterValue(FName(L"OpacityFromTexture"),1);
                W.Materials[P.Material]=TWeakObjectPtr<UMaterialInstanceDynamic>(MID);
            }
            else Mesh->SetMaterial(0,MID);
        }
    }
    for(auto& H:W.Hidden) if(auto M=H.Mesh.Get()) M->SetVisibility(false,false);
    for(auto A:GetObjectsOfClass<UAmmoComponent>())
        if(A->ReserveComponent) for(auto O=A->Outer;O;O=O->Outer) if(O==Weapon) { W.Ammo=TWeakObjectPtr<UAmmoComponent>(A); W.LastAmmo=A->GetAmmo(); break; }
    Dressed.emplace(Weapon,std::move(W));
    auto& Applied=Dressed.at(Weapon);
    Override(Applied,L"/Game/Equippables/Guns/_Core/Tracers/Tracer_1P.Tracer_1P_C",Presentation.Tracer);
    Weapon->SkinKillBannerData=Presentation.Banner;
    InstallSoundHook();
    Report="Benjamin: sculpted carapace, animated sleepy face, lime tracers, organic audio and native HUD";
    Ares::Log("custom skins: %s (%d rigs)",Report.c_str(),(int)Rigs.size());
    return true;
}
void CustomSkins::Clear(AAresEquippable* Weapon)
{
    auto It=Dressed.find(Weapon); if(It==Dressed.end()) return;
    Remove(It->second); Dressed.erase(It);
}
bool CustomSkins::IsActive(AAresEquippable* Weapon) { return ForWeapon(Weapon)!=nullptr; }
void CustomSkins::Tick()
{
    if(Dressed.empty() || Ares::Config().Mode!=Ares::EMode::Standalone) return;
    FReentrancyGuard Guard(Busy); if(!Guard.Entered()) return;
    const double Now=Ares::Seconds();
    static double Last=0; if(Now-Last<1.0/120) return;
    const float Delta=(float)std::clamp(Now-Last,0.0,.1); Last=Now;
    // FXCs are pooled
    static double LastCleanup=0;
    if(Now-LastCleanup>.25)
    {
        LastCleanup=Now;
        BoundMaterials.erase(std::remove_if(BoundMaterials.begin(),BoundMaterials.end(),
            [](const FBoundMaterial& B){ return !B.Component.Get(); }),BoundMaterials.end());
    }
    for(auto It=Dressed.begin();It!=Dressed.end();)
    {
        auto& W=It->second; auto Weapon=W.Weapon.Get();
        if(!Weapon || Weapon->bActorIsBeingDestroyed) { Remove(W); It=Dressed.erase(It); continue; } ++It;
        const bool Equipped=Held(Weapon);
        if(Equipped && !W.Equipped) { W.Equip=Now; Sound(1); }
        if(!Equipped) W.Reload=W.Inspect=-100;
        W.Equipped=Equipped;
        const auto Current=State(Weapon);
        const bool Reloading=Current.find("reload")!=std::string::npos;
        if(Equipped && Reloading && W.State.find("reload")==std::string::npos)
        { W.Reload=Now; W.Inspect=-100; Sound(2); }
        if(!Reloading) W.Reload=-100;
        W.State=Current;
        if(auto Ammo=W.Ammo.Get()) W.LastAmmo=Ammo->GetAmmo();
        auto Zoom=Weapon->ZoomManager?Weapon->ZoomManager->ZoomComponent:nullptr;
        const bool Aiming=Equipped && Zoom && (Zoom->IsZoomed() || Zoom->IsZoomingIn()) && !Zoom->IsZoomingOut();
        if(Aiming) BindParticles(W,Weapon,false);
        W.Zoom+=std::clamp((Aiming?1.f:0.f)-W.Zoom,-Delta*7,Delta*7);
        const float Reload=Arc(Now,W.Reload,2.2f), Inspect=Arc(Now,W.Inspect,2.f);
        const float Equip=1-std::clamp((float)(Now-W.Equip)/.75f,0.f,1.f);
        const float Recoil=std::max(0.f,1.f-(float)(Now-W.Fire)/.12f);
        for(auto& H:W.Hidden) if(auto M=H.Mesh.Get()) if(M->bVisible) M->SetVisibility(false,false);
        const float Breath=sinf((float)(Now-W.Started)*1.45f);
        const float Phase=fmodf((float)(Now-W.Started),4.7f);
        const float Blink=std::max(Arc(Phase,.1,.23f),Arc(Phase,.43,.16f));
        const float Squint=std::clamp(Blink+Recoil*.42f,0.f,1.f);
        if(auto Glow=W.Materials[4].Get())
        {
            const float Bright=1.05f+.12f*Breath+Recoil*.8f+Arc(Now,W.Kill,.8f)*.65f;
            Glow->SetVectorParameterValue(FName(L"TintColorAndOpacity"),{Bright,Bright,Bright,1});
        }
        for(auto& Piece:W.Pieces)
        {
            auto Mesh=Piece.Mesh.Get(); auto Rig=Piece.Rig.Get(); if(!Mesh || !Rig) continue;
            const auto& P=BenjaminGeometry::Parts()[Piece.Part];
            const bool Sight=P.Name=="sights" || P.Name=="sight-glow";
            const bool Visible=!Rig->bHiddenInGame && (!Rig->bIsFirstPerson || Equipped) && !(Sight && Rig->bIsFirstPerson && W.Zoom>.2f);
            Mesh->SetVisibility(Visible,false);
            if(!Visible) continue;
            FVector At=P.Pivot,Scale{1,1,1}; FRotator Local{};
            if(P.Motion>=1 && P.Motion<=6)
            {
                // shared head pivot
                FVector Head{-12,0,7.5f};
                Local.Pitch=Inspect*7-Equip*9+Breath*.35f;
                Local.Yaw=-Inspect*13;
                Local.Roll=Inspect*5;
                auto D=Rotate({At.X-Head.X,At.Y-Head.Y,At.Z-Head.Z},Local);
                At={Head.X+D.X-Recoil*.45f,Head.Y+D.Y,Head.Z+D.Z+Breath*.10f};
                if(P.Motion==2 || P.Motion==3) Scale.Z=std::max(.035f,1-Squint*.98f);
                if(P.Motion==4 || P.Motion==5) At.Z-=Squint*1.45f;
                if(P.Motion==6) { At.Z-=Recoil*.5f+Inspect*1.25f; Scale.Z=1+Inspect*1.1f+Recoil*.7f; }
            }
            else if(P.Motion==8)
            {
                FVector Anchor{-7,-7.7f,-1};
                Local.Roll=5*sinf((float)(Now-W.Started)*2.5f)+Recoil*16+Inspect*12;
                auto D=Rotate({At.X-Anchor.X,At.Y-Anchor.Y,At.Z-Anchor.Z},Local);
                At={Anchor.X+D.X,Anchor.Y+D.Y,Anchor.Z+D.Z};
            }
            if(P.Motion==9)
            {
                // follows the mag bone
                At={0,0,-2}; Local={};
            }
            else
            {
                At=Rotate(At,W.Alignment);
                At.X+=W.Offset.X; At.Y+=W.Offset.Y; At.Z+=W.Offset.Z;
                Local.Pitch+=W.Alignment.Pitch; Local.Yaw+=W.Alignment.Yaw; Local.Roll+=W.Alignment.Roll;
            }
            FHitResult Hit{};
            Mesh->K2_SetRelativeLocationAndRotation(At,Local,false,&Hit,true);
            Mesh->SetRelativeScale3D(Scale);
        }

    }
}
void CustomSkins::OnKill(AAresEquippable* Weapon)
{
    auto W=ForWeapon(Weapon); if(!W || !Weapon->MyPawn) return;
    const double Now=Ares::Seconds();
    if(Now-W->Kill<.05) return; // same kill reported twice
    W->Kill=Now;
    Sound(4);
    Ares::Log("benjamin: native kill banner requested");
}
bool CustomSkins::BeforeDispatch(UObject* Object,UFunction* Function,void* Parameters)
{
    if(Dressed.empty() || !Object || !Function) return false;
    const auto Kind=Event(Function);
    if(Kind==EEvent::Reset || Kind==EEvent::Tracer || Kind==EEvent::Effect)
        if(auto Effect=Object->Cast<AEffectContainer>()) RestoreEffect(Effect);
    if(Kind==EEvent::Sound && Object->IsA<AAresSoundHandler>())
    {
        auto O=ObjectParam(Function,Parameters,"ShooterCharacter");
        auto W=ForPawn(O?O->Cast<AShooterCharacter>():nullptr);
        if(!W) return false;
        // only touch the outputs
        for(auto Field=Function->Children;Field;Field=Field->Next)
        {
            auto P=Field->Cast<UProperty>(); if(!P) continue;
            if(auto B=P->Cast<UBoolProperty>(); B && P->GetName()=="Result")
                *((uint8*)Parameters+P->Offset+B->ByteOffset)&=~B->FieldMask;
            else if(P->IsA<UObjectProperty>() && P->GetName()=="AudioComponent")
                *(UObject**)((uint8*)Parameters+P->Offset)=nullptr;
        }
        ++W->MutedSounds;
        return true;
    }
    if(Kind==EEvent::Banner && Object->Class->GetName()=="KillBannerAnnouncement_C")
    {
        auto O=ObjectParam(Function,Parameters,"EquippableUsed");
        auto W=ForWeapon(O?O->Cast<AAresEquippable>():nullptr);
        if(W)
        {
            BeginBanner(Object,Function,Parameters);
            ++W->BannerReplacements;
            OnKill(W->Weapon.Get());
            return false;
        }
    }
    return false;
}
void CustomSkins::AfterDispatch(UObject* Object,UFunction* Function,void* Parameters)
{
    EndBanner(Object,Function,Parameters);
    if(Dressed.empty() || !Object || !Function || Busy) return;
    const auto Kind=Event(Function);
    if(Kind==EEvent::Other || Kind==EEvent::Sound || Kind==EEvent::Banner) return;
    FReentrancyGuard Guard(Busy); if(!Guard.Entered()) return;
    if(Kind==EEvent::Shot)
    {
        if(auto W=ForWeapon(Object->Cast<AAresEquippable>()); W && Held(W->Weapon.Get()))
        { W->Fire=Ares::Seconds(); W->Inspect=-100; ++W->Shots; Sound(0); }
    }
    else if(Kind==EEvent::Inspect)
    {
        if(auto W=ForContext(Object); W && Held(W->Weapon.Get()))
        { W->Inspect=Ares::Seconds(); Sound(3); }
    }
    else if(Kind==EEvent::Tracer || Kind==EEvent::Effect)
    {
        auto Effect=Object->Cast<AEffectContainer>(); if(!Effect) return;
        auto W=ForContext(ObjectParam(Function,Parameters,"Context"));
        if(!W) W=ForContext(ObjectParam(Function,Parameters,"Target"));
        if(!W) W=ForEffect(Effect);
        if(W) BindEffect(*W,Effect);
    }
    else if(Kind==EEvent::Kill)
    {
        auto O=ObjectParam(Function,Parameters,"KillerCharacter");
        if(auto W=ForPawn(O?O->Cast<AShooterCharacter>():nullptr)) OnKill(W->Weapon.Get());
    }
}
std::vector<std::string> CustomSkins::Describe()
{
    std::vector<std::string> Result;
    for(auto& [Key,W]:Dressed)
    {
        auto Weapon=W.Weapon.Get(); if(!Weapon) continue;
        Result.push_back(Weapon->GetName()+": Benjamin, "+std::to_string(W.Pieces.size())+" pieces, ammo "+std::to_string(W.LastAmmo)+", state "+W.State);
        Result.push_back("shots="+std::to_string(W.Shots)+" custom tracers="+std::to_string(W.Tracers)+
            " stock sounds suppressed="+std::to_string(W.MutedSounds)+" native banner configurations="+std::to_string(W.BannerReplacements)+
            " native optics="+std::to_string(W.Optics)+" ADS="+std::to_string(W.Zoom)+" body socket="+W.Socket.ToString());
        for(auto& P:W.Pieces) if(P.Part==0) if(auto M=P.Mesh.Get())
        {
            auto At=M->K2_GetComponentLocation();
            char B[192]; snprintf(B,sizeof(B),"sculpt first-person=%d visible=%d xyz %.1f %.1f %.1f sections=%d",
                (int)M->bIsFirstPerson,(int)M->bVisible,At.X,At.Y,At.Z,M->GetNumSections()); Result.push_back(B);
        }

    }
    if(Result.empty()) Result.push_back("no Benjamin active");
    return Result;
}
void CustomSkins::PreviewMaterial(const std::string& Mode)
{
    FReentrancyGuard Guard(Busy); if(!Guard.Entered()) return;
    for(auto& [Key,W]:Dressed)
    {
        auto Weapon=W.Weapon.Get(); if(!Weapon) continue;
        for(auto& Piece:W.Pieces) if(auto Mesh=Piece.Mesh.Get())
        {
            const auto& Part=BenjaminGeometry::Parts()[Piece.Part];
            if(Mode=="flip" || Mode=="normal")
            {
                TArray<int32> Indices(Part.Triangles);
                if(Mode=="flip") for(int i=0;i<Indices.Num();i+=3) std::swap(Indices[i+1],Indices[i+2]);
                TArray<FColor> Colors; TArray<FProcMeshTangent> Tangents;
                Colors.Reserve(Part.Vertices.Num());
                for(int i=0;i<Part.Vertices.Num();++i) Colors.Add({255,255,255,255});
                Mesh->CreateMeshSection(0,Part.Vertices,Indices,Part.Normals,Part.UV,Colors,Tangents,false);
            }
            else if(Mode=="stock") Mesh->SetMaterial(0,Weapon->Mesh1P->GetMaterial(0));
            else if(Mode=="grid") Mesh->SetMaterial(0,FindObject<UMaterialInterface>(L"/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
            else if(Mode=="restore") Mesh->SetMaterial(0,W.Materials[Part.Material].Get());
            else if(Mode=="world") Mesh->SetIsFirstPerson(false);
            else if(Mode=="viewmodel") Mesh->SetIsFirstPerson(Piece.Rig.Get()->bIsFirstPerson);
        }
    }
    Ares::Log("Benjamin material preview: %s",Mode.c_str());
}
