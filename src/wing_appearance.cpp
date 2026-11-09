#include "wing_appearance.hpp"
#include "wing_appearance_file.hpp"
#include "wings.hpp"
#include "wing_models.hpp"
#include "actor_state.hpp"
#include "wing_model_names.hpp"
#include <RE/B/BSFlattenedBoneTree.h>
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace wingflight {
namespace {
using Object=RE::NiAVObject;
struct Root {
    RE::NiPointer<Object> node;
    appearance::Channel x,y,z,scale;
};
struct Binding {
    RE::NiPointer<Object> model;
    RE::NiPointer<RE::ModelReferenceEffect> effect;
    std::vector<Root> roots;
    bool smp{};
    std::string key;
    appearance::ProfileSettings values;
    appearance::FlightBlend flightBlend;
};
std::mutex mutex;
std::atomic_bool active{};
RE::ActorHandle owner;
RE::NiPointer<Object> body,basis;
std::vector<Binding> bindings;
std::mutex profileMutex;
appearance::Profiles profiles;
bool profilesLoaded{};
std::string profileError;
std::uint64_t profileRevision{};
WingAppearanceSnapshot currentInfo;
std::filesystem::path profilePath(){return std::filesystem::absolute("Data/SKSE/Plugins/Wingflight/Appearance/Presets.ini");}
WingAppearanceSnapshot identify(RE::Actor* actor,const WingMatch& match){
    WingAppearanceSnapshot info;
    if(!actor||match.owner!=actor||!match.profile||match.profile->animationless)return info;
    RE::TESForm* form=match.source;
    if(!form&&match.effect)form=match.effect->GetBaseObject();
    if(!form&&match.profile->nativeVampire)form=actor->GetRace();
    const char* kind=match.profile->nativeVampire?"RACE":form&&form->As<RE::TESObjectARMO>()?"ARMO":"MGEF";
    if(form){if(const auto file=form->GetFile(0))info.key=appearance::formKey(file->GetFilename(),kind,form->GetLocalFormID());
        if(const auto name=form->GetName();name&&*name)info.name=name;}
    if(info.name.empty()&&match.effect&&match.effect->spell)if(const auto name=match.effect->spell->GetName();name&&*name)info.name=name;
    if(info.name.empty())info.name=match.profile->id;
    // Providers without a persistent Form still have a stable registration;
    // never key by a transient pointer or the current plugin load-order index.
    if(info.key.empty())info.key=appearance::formKey(match.profile->plugin+"|"+match.profile->id,"PROVIDER",0);
    return info;
}
appearance::ProfileSettings publishSelection(WingAppearanceSnapshot info,appearance::Settings defaults){
    std::lock_guard lock(profileMutex);
    if(!profilesLoaded){
        profilesLoaded=true;
        try{profiles=appearance::loadProfiles(profilePath());}
        catch(const std::exception& e){profileError=e.what();SKSE::log::error("Wing appearance presets: {}",profileError);}
    }
    auto values=appearance::legacy(defaults);
    if(const auto found=profiles.find(info.key);found!=profiles.end())values=found->second;
    info.values=values;info.error=profileError;info.revision=profileRevision;currentInfo=std::move(info);return values;
}
bool inside(Object* node,Object* root){
    for(unsigned i=0;node&&i<64;++i,node=node->parent)if(node==root)return true;
    return false;
}
bool valid(const Binding& b){
    // A stopped magic effect can still display its native exit animation.
    if(b.effect&&(!b.effect->GetAttached()||b.effect->Get3D()!=b.model.get()
        ||b.effect->target.native_handle()!=owner.native_handle()))return false;
    for(const auto& r:b.roots)if(!inside(r.node.get(),body.get())&&!inside(r.node.get(),b.model.get()))return false;
    return true;
}
bool current(){
    const auto actor=owner.get();
    return actor&&actor->Is3DLoaded()&&actor->Get3D(false)==body.get();
}
bool touches(const Binding& b,Object* scope){
    if(!scope)return false;
    // A player body pass also updates detached magic-effect models.
    if(scope==body.get())return true;
    for(const auto& r:b.roots)if(inside(r.node.get(),scope)||inside(scope,r.node.get()))return true;
    return false;
}
void restore(Binding& b){
    for(auto& r:b.roots){auto& t=r.node->local;
        t.translate.x=r.x.restore(t.translate.x);t.translate.y=r.y.restore(t.translate.y);
        t.translate.z=r.z.restore(t.translate.z);t.scale=r.scale.restore(t.scale);
    }
}
// Resolve current local ancestry instead of last frame's world orientation.
RE::NiTransform world(Object* node){
    if(!node){return RE::NiTransform{};}
    std::array<Object*,64> chain{};unsigned count{};
    auto p=node;
    while(p&&p!=body.get()&&count<chain.size()){chain[count++]=p;p=p->parent;}
    if(p!=body.get())return node->world;
    auto value=body->world;
    while(count)value=value*chain[--count]->local;
    return value;
}
void refresh(Binding& b,RE::NiUpdateData* input){
    RE::NiUpdateData fallback{};auto data=input?input:&fallback;
    // Outside flight the solver still owns dynamic descendant world poses.
    // Refresh only anchors here, otherwise cosmetic settings would erase SMP
    // motion on every actor update. The existing flight final pass refreshes
    // the complete subtree only while its procedural driver actually owns it.
    if(b.smp){for(auto& r:b.roots)r.node->UpdateWorldData(data);return;}
    unsigned budget=4096;
    auto walk=[&](auto&& self,Object* node,unsigned depth)->void{
        if(!node||!budget||depth>64)return;--budget;node->UpdateWorldData(data);
        if(auto branch=node->AsNode())for(const auto& child:branch->GetChildren())if(child)self(self,child.get(),depth+1);
    };
    for(auto& r:b.roots)walk(walk,r.node.get(),0);
    // Only copy entries owned by the selected wings, never the character body.
    std::unordered_set<RE::BSFlattenedBoneTree*> trees;
    for(auto& r:b.roots){unsigned depth{};for(auto node=r.node.get();node&&depth++<64;node=node->parent){
        auto tree=netimmerse_cast<RE::BSFlattenedBoneTree*>(node);
        if(!tree||!trees.insert(tree).second)continue;
        auto& cache=tree->GetRuntimeData();if(!cache.boneEntries||cache.numBones>2048)continue;
        std::vector<bool> selected(cache.numBones);
        for(unsigned i=0;i<cache.numBones;++i){auto& e=cache.boneEntries[i];
            if(e.node){
                for(const auto& root:b.roots)if(inside(e.node,root.node.get())){selected[i]=true;break;}
                if(selected[i]){e.local=e.node->local;e.world=e.node->world;}
            }else if(e.parentIndex>=0&&static_cast<unsigned>(e.parentIndex)<i&&selected[e.parentIndex]){
                selected[i]=true;e.world=cache.boneEntries[e.parentIndex].world*e.local;
            }
        }
    }
    }
}
void apply(Binding& b,RE::NiUpdateData* data){
    const auto scale=b.flightBlend.scale(b.values);
    if(scale==100&&b.values.verticalOffset==0){
        const bool owned=std::any_of(b.roots.begin(),b.roots.end(),[](const auto& r){return r.x.wrote||r.y.wrote||r.z.wrote||r.scale.wrote;});
        if(owned){restore(b);refresh(b,data);}return;
    }
    const auto torso=world(basis.get());
    // Offsets are character-local Skyrim units, independent of wing size.
    for(auto& r:b.roots){
        const auto parent=world(r.node->parent);
        const auto offset=appearance::localOffset(b.values.verticalOffset,body->world.scale,parent.scale,torso.rotate.entry,parent.rotate.entry);
        auto& t=r.node->local;
        if(!offset||!std::isfinite(t.scale)||!std::isfinite(t.translate.x)||!std::isfinite(t.translate.y)||!std::isfinite(t.translate.z))continue;
        t.translate.x=r.x.apply(t.translate.x,1,(*offset)[0]);
        t.translate.y=r.y.apply(t.translate.y,1,(*offset)[1]);
        t.translate.z=r.z.apply(t.translate.z,1,(*offset)[2]);
        t.scale=r.scale.apply(t.scale,scale*.01f,0);
    }
    refresh(b,data);
}
void clear(){
    active=false;
    for(auto& b:bindings){restore(b);if(current()&&valid(b))refresh(b,nullptr);}
    bindings.clear();owner={};body.reset();basis.reset();
}
}
WingAppearanceSnapshot currentWingAppearance(){std::lock_guard lock(profileMutex);return currentInfo;}
std::string saveCurrentWingAppearance(RE::Actor* actor,const WingMatch& match,const std::string& expectedKey,appearance::ProfileSettings values){
    const auto info=identify(actor,match);
    if(expectedKey.empty()||info.key!=expectedKey)return "Your equipped wings changed. The current wings have been refreshed; no settings were saved.";
    if(!appearance::valid(values))return "Wing sizes must be 10-200%, and vertical offset must be -60 to 60.";
    try{
        auto saved=appearance::saveProfile(profilePath(),expectedKey,values);
        std::lock_guard lock(profileMutex);profiles=std::move(saved);profilesLoaded=true;profileError.clear();++profileRevision;
    }catch(const std::exception& e){return e.what();}
    return {};
}
void resetWingAppearance(){
    {std::lock_guard lock(profileMutex);currentInfo={};}
    if(!active.load())return;std::lock_guard lock(mutex);clear();
}
void updateWingAppearance(RE::Actor* actor,const WingMatch& match,const appearance::Settings& defaults,bool flying,float delta){
    if(!actor||!actor->Is3DLoaded()){
        resetWingAppearance();return;
    }
    const auto identity=identify(actor,match);
    const auto requested=publishSelection(identity,defaults);
    if(!active.load()&&requested.normalScale==100&&requested.flightScale==100&&requested.verticalOffset==0)return;
    std::vector<Binding> next;
    const bool selected=match.owner==actor&&match.profile&&!match.profile->animationless;
    if(selected&&registeredWing(match.profile)){
        auto view=registeredWingAppearanceRoots(match);
        if(!view.roots.empty()){
            Binding b;b.model=std::move(view.model);b.smp=view.smp;
            for(auto& r:view.roots)b.roots.push_back({std::move(r)});
            next.push_back(std::move(b));
        }
    }else if(selected&&match.profile->nativeVampire){
        auto model=match.owner->Get3D(false);Binding b;b.model.reset(model);
        for(const auto name:{"LeftWing1","RightWing1"})if(auto root=model->GetObjectByName(name))b.roots.push_back({RE::NiPointer<Object>(root)});
        if(b.roots.size()==2)next.push_back(std::move(b));
    }else if(selected)for(const auto& effect:wingModels(match)){
        auto model=effect->Get3D();if(!model)continue;
        // Keep attachment positioning on the ART root native if its dedicated
        // skeleton is available. Never touch the player's own Skeleton node.
        auto root=model->GetObjectByName("Skeleton");if(!root)root=model;
        if(inside(match.owner->Get3D(false),root))continue;
        Binding b;b.model.reset(model);b.effect=effect;b.roots.push_back({RE::NiPointer<Object>(root)});next.push_back(std::move(b));
    }
    // Snapshot known outgoing ARTs before taking the process-list lock. Do not
    // discover unrelated effects or retain a model merely through our NiPointer.
    std::vector<RE::NiPointer<RE::ModelReferenceEffect>> outgoing;
    {
        std::lock_guard lock(mutex);
        if(owner.get().get()!=actor||body.get()!=actor->Get3D(false))clear();
        for(const auto& b:bindings)if(b.effect&&std::none_of(next.begin(),next.end(),[&](const auto& n){return n.model==b.model;}))
            outgoing.push_back(b.effect);
    }
    std::unordered_set<const RE::ModelReferenceEffect*> listed;
    if(!outgoing.empty())if(auto lists=RE::ProcessLists::GetSingleton()){
        unsigned visited{};
        lists->ForEachMagicTempEffect([&](RE::BSTempEffect* effect){
            for(const auto& known:outgoing)if(known.get()==effect){listed.insert(known.get());break;}
            return ++visited>=4096||listed.size()==outgoing.size()?RE::BSContainer::ForEachResult::kStop:RE::BSContainer::ForEachResult::kContinue;
        });
    }
    std::lock_guard lock(mutex);
    owner=actor->GetHandle();body.reset(actor->Get3D(false));
    auto torso=body->GetObjectByName("NPC Spine2 [Spn2]");basis.reset(torso?torso:body.get());
    for(auto& b:next)for(const auto& r:b.roots)if(r.node->name.c_str()
        &&channel::stableWingNodeName(r.node->name.c_str())!=r.node->name.c_str())b.smp=true;
    for(auto& b:next){
        b.key=identity.key;b.values=requested;
        for(const auto& old:bindings)if(old.model==b.model&&old.key==b.key){b.flightBlend=old.flightBlend;break;}
    }
    // Transfer channel ownership only for exactly the same physical nodes.
    for(auto& b:next)for(auto& r:b.roots)for(auto& old:bindings)for(auto& prior:old.roots)
        if(prior.node==r.node){r=prior;prior.x.wrote=prior.y.wrote=prior.z.wrote=prior.scale.wrote=false;}
    for(auto& old:bindings)if(old.effect&&next.size()<32&&valid(old)
        &&appearance::retainRemovedArt(listed.contains(old.effect.get()),old.effect->GetAttached(),old.effect->Get3D()==old.model.get())){
        bool overlap=false;
        for(const auto& n:next)for(const auto& r:n.roots)for(const auto& prior:old.roots)
            overlap|=inside(r.node.get(),prior.node.get())||inside(prior.node.get(),r.node.get());
        if(!overlap)next.push_back(std::move(old));
    }
    for(auto& old:bindings){
        const bool owned=std::any_of(old.roots.begin(),old.roots.end(),[](const auto& r){return r.x.wrote||r.y.wrote||r.z.wrote||r.scale.wrote;});
        if(owned){restore(old);if(valid(old))refresh(old,nullptr);}
    }
    bindings=std::move(next);active=!bindings.empty();
    if(bindings.empty()){clear();return;}
    for(auto& b:bindings)if(valid(b)){b.flightBlend.update(flying,delta);apply(b,nullptr);}
}
void beginWingAppearance(Object* scope){
    if(!active.load()||!scope)return;std::lock_guard lock(mutex);
    if(!current())return;
    for(auto& b:bindings)if(valid(b)&&touches(b,scope))restore(b);
}
void endWingAppearance(Object* scope,RE::NiUpdateData* data){
    if(!active.load()||!scope)return;std::lock_guard lock(mutex);
    if(!current())return;
    for(auto& b:bindings)if(valid(b)&&touches(b,scope))apply(b,data);
}
}
