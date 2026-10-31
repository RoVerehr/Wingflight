#pragma once
#include <RE/Skyrim.h>
#include <algorithm>
#include <vector>

namespace wingflight {
// Foot IK feeds ground corrections back into Actor position. Full flight must
// follow its collision controller even while touching the floor. Use the native
// graph switch, never a global INI setting or a forced position update.
class FlightFootIK {
    struct Entry {
        RE::BSTSmartPointer<RE::BShkbAnimationGraph> graph;
        const RE::hkbBehaviorGraph* behavior{};
        bool changed{};
    };
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager_;
    std::vector<Entry> entries_;
    static const RE::BSFixedString& variable(){
        static const RE::BSFixedString name("bHumanoidFootIKDisable");return name;
    }
    static void restore(Entry& entry){
        // A graph rebuilt in place has its own defaults; do not apply the old
        // graph's snapshot to it. Graph references keep detached objects alive.
        bool current{};
        if(entry.changed&&entry.graph->characterInstance.behaviorGraph.get()==entry.behavior
            &&entry.graph->GetGraphVariableBool(variable(),current)&&current)
            entry.graph->SetGraphVariableBool(variable(),false);
        entry.changed=false;
    }
public:
    void release(){
        if(manager_){
            RE::BSSpinLockGuard guard(manager_->GetRuntimeData().updateLock);
            for(auto& entry:entries_)restore(entry);
            entries_.clear();
        }
        manager_.reset();
    }
    void update(RE::PlayerCharacter* actor,bool fullFlight){
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> current;
        if(!fullFlight||!actor||!actor->GetAnimationGraphManager(current)||!current){release();return;}
        if(current.get()!=manager_.get()){release();manager_=current;}
        RE::BSSpinLockGuard guard(manager_->GetRuntimeData().updateLock);
        std::erase_if(entries_,[&](Entry& entry){
            const bool present=std::any_of(manager_->graphs.begin(),manager_->graphs.end(),
                [&](const auto& graph){return graph.get()==entry.graph.get()&&graph->holder==actor
                    &&graph->characterInstance.behaviorGraph.get()==entry.behavior;});
            if(!present)restore(entry);
            return !present;
        });
        for(auto& graph:manager_->graphs){
            if(!graph||graph->holder!=actor||!graph->characterInstance.behaviorGraph)continue;
            bool disabled{};
            if(!graph->GetGraphVariableBool(variable(),disabled))continue; // e.g. non-humanoid graphs
            auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.graph.get()==graph.get();});
            if(it==entries_.end()){
                entries_.push_back({graph,graph->characterInstance.behaviorGraph.get(),false});it=entries_.end()-1;
            }
            // Already-disabled graphs remain disabled on release. Record only
            // false -> true writes that we actually made successfully.
            if(!disabled&&graph->SetGraphVariableBool(variable(),true))it->changed=true;
        }
    }
};
}
