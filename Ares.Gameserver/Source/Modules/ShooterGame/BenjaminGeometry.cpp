#include <pch.h>
#include <Headers/Modules/ShooterGame/BenjaminGeometry.h>
#include <fstream>
#include <cmath>

namespace
{
    std::vector<BenjaminGeometry::FPart> Geometry;
    template<class T> bool Read(std::ifstream& F,T& Value)
    { return !!F.read((char*)&Value,sizeof(Value)); }
}
bool BenjaminGeometry::Load(const std::filesystem::path& Folder,std::string& Error)
{
    if(!Geometry.empty()) return true;
    std::ifstream File(Folder/"benjamin.bjm",std::ios::binary);
    char Magic[4]{}; uint32 Count=0;
    if(!File.read(Magic,4) || memcmp(Magic,"BJM1",4) || !Read(File,Count) || Count==0 || Count>128)
    { Error="invalid or missing Benjamin mesh"; return false; }
    std::vector<FPart> Loaded;
    for(uint32 PartIndex=0;PartIndex<Count;++PartIndex)
    {
        char Name[64]{}; uint32 Material=0,Motion=0,Vertices=0,Indices=0; FVector Pivot{};
        if(!File.read(Name,64) || !Read(File,Material) || !Read(File,Motion) || !Read(File,Pivot) ||
           !Read(File,Vertices) || !Read(File,Indices) || Material>=9 || Motion>9 ||
           Vertices==0 || Vertices>200000 || Indices==0 || Indices>600000 || Indices%3)
        { Error="invalid Benjamin mesh section"; return false; }
        FPart Part{}; Part.Name=std::string(Name,strnlen(Name,64)); Part.Material=Material; Part.Motion=Motion; Part.Pivot=Pivot;
        Part.Vertices.Reserve(Vertices); Part.Normals.Reserve(Vertices); Part.UV.Reserve(Vertices); Part.Triangles.Reserve(Indices);
        for(uint32 i=0;i<Vertices;++i)
        {
            float V[8]; if(!File.read((char*)V,sizeof(V))) { Error="truncated Benjamin vertices"; return false; }
            for(float Value:V) if(!std::isfinite(Value)) { Error="nonfinite Benjamin vertex"; return false; }
            Part.Vertices.Add({V[0],V[1],V[2]}); Part.Normals.Add({V[3],V[4],V[5]}); Part.UV.Add({V[6],V[7]});
        }
        for(uint32 i=0;i<Indices;++i)
        {
            uint32 Index=0;
            if(!Read(File,Index) || Index>=Vertices) { Error="invalid Benjamin triangle"; return false; }
            Part.Triangles.Add((int32)Index);
        }
        Loaded.push_back(std::move(Part));
    }
    Geometry=std::move(Loaded);
    return true;
}
const std::vector<BenjaminGeometry::FPart>& BenjaminGeometry::Parts() { return Geometry; }
UProceduralMeshComponent* BenjaminGeometry::Create(AAresEquippable* Weapon,const FPart& Part)
{
    auto Class=Weapon->Class->Cast<UBlueprintGeneratedClass>();
    if(!Class) return nullptr;
    static UProceduralMeshComponent* Template=nullptr;
    if(!Template)
    {
        Template=(UProceduralMeshComponent*)UGameplayStatics::SpawnObject(UProceduralMeshComponent::StaticClass(),Class);
        if(!Template) return nullptr;
        Template->AddToRoot();
        Template->SetMobility(EComponentMobility::Movable);
        Template->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Template->bUseAsyncCooking=false;
    }
    // temp template just for AddComponent, removed right after
    const int Index=Class->ComponentTemplates.Num();
    Class->ComponentTemplates.Add(Template);
    auto Component=Weapon->AddComponent(Template->Name,true,ConstructTrans(FVector{},FRotator{}),Weapon);
    Class->ComponentTemplates.Remove(Index);
    auto Mesh=Component?Component->Cast<UProceduralMeshComponent>():nullptr;
    if(!Mesh) return nullptr;
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetCastShadow(false);
    TArray<FColor> Colors; TArray<FProcMeshTangent> Tangents;
    Colors.Reserve(Part.Vertices.Num()); Tangents.Reserve(Part.Vertices.Num());
    for(auto& N:Part.Normals)
    {
        Colors.Add({255,255,255,255});
        FVector T{N.Z,0,-N.X};
        float Length=sqrtf(T.X*T.X+T.Z*T.Z);
        if(Length>.001f) { T.X/=Length; T.Z/=Length; } else T={1,0,0};
        FProcMeshTangent Tangent{}; Tangent.TangentX=T; Tangents.Add(Tangent);
    }
    // flip winding or the faces render black
    TArray<int32> Clockwise(Part.Triangles);
    for(int i=0;i<Clockwise.Num();i+=3) std::swap(Clockwise[i+1],Clockwise[i+2]);
    Mesh->CreateMeshSection(0,Part.Vertices,Clockwise,Part.Normals,Part.UV,Colors,Tangents,false);
    if(Mesh->GetNumSections()!=1) { Mesh->K2_DestroyComponent(Weapon); return nullptr; }
    if(Part.Name=="body")
    {
        auto& V=Mesh->ProcMeshSections[0].ProcVertexBuffer[0];
        Ares::Log("Benjamin mesh: vertices=%d uv=%.3f,%.3f color=%d,%d,%d bounds=%.1f,%.1f,%.1f",
            Part.Vertices.Num(),V.UV0.X,V.UV0.Y,V.Color.R,V.Color.G,V.Color.B,
            Mesh->LocalBounds.BoxExtent.X,Mesh->LocalBounds.BoxExtent.Y,Mesh->LocalBounds.BoxExtent.Z);
    }
    return Mesh;
}
