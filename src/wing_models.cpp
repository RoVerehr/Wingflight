#include "wing_models.hpp"
#include "wings.hpp"
#include "effect_owner.hpp"
#include "diagnostics.hpp"
#include <chrono>
#include <mutex>
#include <unordered_set>
namespace wingflight {
namespace {
using Model=RE::NiPointer<RE::ModelReferenceEffect>;
std::mutex cacheMutex;
std::vector<Model> scriptedModels;
std::chrono::steady_clock::time_point scanned{};
}
void resetWingModelCache(){std::lock_guard lock(cacheMutex);scriptedModels.clear();scanned={};}
std::vector<Model> wingModels(const WingMatch& match){
    std::vector<Model> out;
    if(!match.profile)return out;
    if(match.runtimeArt){
        const auto model=match.runtimeArt;
        if(wingSoundActor(match.owner,match.effect,true)&&!model->finished
            &&model->target.native_handle()==match.owner->GetHandle().native_handle()
            &&model->GetAttached()&&model->Get3D())out.emplace_back(model);
        return out;
    }
    if(!wingEventActor(match.owner,match.effect))return out;
    const auto& ids=match.profile->artObjectIDs;
    const auto data=RE::TESDataHandler::GetSingleton();
    std::unordered_set<RE::BGSArtObject*> selected;
    for(auto id:ids)if(auto art=data->LookupForm<RE::BGSArtObject>(id,match.profile->plugin))selected.insert(art);
    auto add=[&](RE::ModelReferenceEffect* model,bool scripted=false){
        if(!model||model->finished||!model->GetAttached()||!model->Get3D())return;
        if(!ids.empty()&&!selected.contains(model->artObject))return;
        if(scripted&&model->target.native_handle()!=match.owner->GetHandle().native_handle())return;
        for(const auto& old:out)if(old.get()==model)return;
        if(out.size()<16)out.emplace_back(model);
    };
    // Preserve existing provider discovery. Never borrow an unrelated aura ART.
    if(match.effect->hitEffects)for(auto effect:*match.effect->hitEffects)
        if(effect&&!effect->finished&&effect->GetType()==RE::TEMP_EFFECT_TYPE::kRefModel)add(static_cast<RE::ModelReferenceEffect*>(effect));
    if(ids.empty()||selected.empty())return out;
    std::vector<Model> snapshot;
    {
        std::lock_guard lock(cacheMutex);
        const auto now=std::chrono::steady_clock::now();
        if(scanned==std::chrono::steady_clock::time_point{}||now-scanned>=std::chrono::milliseconds(200)){
            scriptedModels.clear();unsigned visited{};
            if(auto lists=RE::ProcessLists::GetSingleton())lists->ForEachMagicTempEffect([&](RE::BSTempEffect* effect){
                if(++visited>4096)return RE::BSContainer::ForEachResult::kStop;
                if(auto model=effect->As<RE::ModelReferenceEffect>();model&&!model->finished&&scriptedModels.size()<512)scriptedModels.emplace_back(model);
                return RE::BSContainer::ForEachResult::kContinue;
            });
            scanned=now;
        }
        const auto owner=match.owner->GetHandle().native_handle();
        for(const auto& model:scriptedModels)
            if(!model->finished&&model->target.native_handle()==owner&&selected.contains(model->artObject))snapshot.push_back(model);
    }
    // No process-list or cache lock is held while inspecting live model state.
    for(const auto& model:snapshot)add(model.get(),true);
    return out;
}
}
