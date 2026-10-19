#include "registered_wings.hpp"
#include "actor_state.hpp"
#include "wing_registration_file.hpp"
#include "wing_channel_pose.hpp"
#include "wing_flow.hpp"
#include "wing_model_names.hpp"
#include "wings.hpp"
#include "logical_wings.hpp"
#include "lower_body_policy.hpp"
#include "armed_bank.hpp"
#include "vampire_runtime.hpp"
#include "runtime_callable.hpp"
#include "diagnostics.hpp"
#include "wing_runtime_policy.hpp"
#include <SKSE/SKSE.h>
#include <RE/N/NiGeometry.h>
#include <RE/N/NiSkinData.h>
#include <RE/B/BSFlattenedBoneTree.h>
#include <RE/B/BSModelDB.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <chrono>

namespace wingflight {
namespace {
using Object=RE::NiAVObject;
struct RigBone {RE::NiPointer<Object> node;int side{};unsigned depth{};float span{},parentSpan{};std::optional<legs::Quaternion> reference;unsigned pair{};};
legs::Quaternion rotation(const RE::NiMatrix3& m);
struct Rig {
    struct SkinLink {RE::NiPointer<Object> geometry;RE::NiPointer<RE::NiSkinInstance> instance;std::vector<std::pair<unsigned,RE::NiPointer<Object>>> bones;};
    RE::NiPointer<Object> model,body,anchor;
    RE::NiMatrix3 referenceToAnchor;
    std::vector<RigBone> bones;
    std::vector<SkinLink> skins;
    std::vector<std::string> left,right;
    std::string reason;
    explicit operator bool()const{return reason.empty()&&!bones.empty()&&!left.empty()&&left.size()==right.size();}
};
std::mutex registryMutex;
std::vector<std::shared_ptr<WingProfile>> registrations,retiredProfiles;
std::unordered_map<const WingProfile*,std::unordered_set<RE::TESForm*>> resolvedForms;
std::filesystem::path directory(){return std::filesystem::absolute("Data/SKSE/Plugins/Wingflight/Wings");}
bool inside(Object* node,Object* root){for(unsigned i=0;node&&i<64;++i,node=node->parent)if(node==root)return true;return false;}
bool bodyBone(Object* node){
    if(!node||!node->name.c_str())return true;
    const std::string_view n=channel::stableWingNodeName(node->name.c_str());
    // Names are an exclusion for known torso/limb bones, never positive proof of wings.
    return n.starts_with("NPC ")&&!n.starts_with("NPC LUpArm")&&!n.starts_with("NPC RUpArm")
        &&!n.starts_with("NPC LForearmWing")&&!n.starts_with("NPC RForearmWing");
}
// Clothing physics (skirts, loincloths, tassets) branches from the pelvis or
// legs; wings attach to the back. Automatic detection only.
bool lowerBodyAnchor(Object* node){
    if(!node||!node->name.c_str())return false;
    const std::string_view n=channel::stableWingNodeName(node->name.c_str());
    for(const auto prefix:{"NPC Root","NPC COM","NPC Pelvis","NPC L Thigh","NPC R Thigh","NPC L Calf","NPC R Calf","NPC L Foot","NPC R Foot"})
        if(n.starts_with(prefix))return true;
    return false;
}
void walk(Object* root,const auto& visit,unsigned& budget,unsigned depth=0){
    if(!root||!budget||depth>64)return;--budget;visit(root);
    if(const auto node=root->AsNode())for(const auto& child:node->GetChildren())if(child)walk(child.get(),visit,budget,depth+1);
}
RE::NiSkinInstance* skin(Object* object){
    if(auto geometry=object->AsGeometry())return geometry->GetGeometryRuntimeData().skinInstance.get();
    if(auto geometry=netimmerse_cast<RE::NiGeometry*>(object))return geometry->GetRuntimeData().spSkinInstance.get();
    return nullptr;
}
Rig inspect(RE::Actor* actor,Object* model,const WingProfile& profile){
    Rig out;out.model.reset(model);out.body.reset(actor?actor->Get3D(false):nullptr);
    if(!out.body||!model){out.reason="Model is not loaded";return out;}
    std::unordered_set<Object*> weighted;unsigned budget=4096,skinReferences{};bool skinLimit{};
    walk(model,[&](Object* object){
        if(skinLimit)return;
        if(object->GetFlags().any(Object::Flag::kHidden))return;
        const auto instance=skin(object);
        if(!instance||!instance->skinData||!instance->bones)return;
        const auto n=instance->skinData->GetBoneCount();if(n>512)return;
        Rig::SkinLink link;link.geometry.reset(object);link.instance.reset(instance);
        for(unsigned i=0;i<n;++i){auto bone=instance->bones[i];
            if(bone&&!bodyBone(bone)&&(inside(bone,out.body.get())||inside(bone,model))){weighted.insert(bone);link.bones.emplace_back(i,RE::NiPointer<Object>(bone));}}
        if(!link.bones.empty()){
            skinReferences+=static_cast<unsigned>(link.bones.size());
            if(out.skins.size()>=64||skinReferences>2048){skinLimit=true;return;}
            out.skins.push_back(std::move(link));
        }
    },budget);
    if(!budget){out.reason="Model exceeds the bounded scan limit";return out;}
    if(weighted.size()<2){out.reason="Incompatible: no independent skinned wing bones";return out;}
    if(skinLimit){out.reason="Wing model exceeds 64 skin bindings or 2048 skin-bone references";return out;}
    std::unordered_set<Object*> scoped;
    for(auto bone:weighted){unsigned hops{};for(auto node=bone;node&&hops++<64;node=node->parent){
        if(!bodyBone(node))scoped.insert(node);if(node==out.body.get()||node==model)break;
    }}
    if(scoped.size()>4096){out.reason="Wing skin ancestry exceeds the bounded scan limit";return out;}
    const std::vector<Object*> modelBones(scoped.begin(),scoped.end());
    auto resolve=[&](std::string_view name){return channel::uniqueWingNode(name,modelBones,[](Object* node){return std::string_view(node->name.c_str());});};
    Object* anchor=*weighted.begin();
    for(unsigned depth=0;anchor&&depth<64;++depth,anchor=anchor->parent){
        bool all=true;for(auto bone:weighted)if(!inside(bone,anchor)){all=false;break;}if(all)break;
    }
    if(!anchor){out.reason="Wing bones have no common model branch";return out;}
    if(profile.rootNode.empty()&&lowerBodyAnchor(anchor)){out.reason="Branches from the pelvis or legs: clothing physics, not wings";return out;}
    if(!profile.rootNode.empty()){
        Object* requested=resolve(profile.rootNode);
        if(!requested){out.reason="Configured RootNode was not found";return out;}
        for(auto bone:weighted)if(!inside(bone,requested)){out.reason="Configured RootNode does not contain the model's wing skin";return out;}
        anchor=requested;
    }
    out.anchor.reset(anchor);
    std::vector<std::pair<Object*,int>> roots;
    if(!profile.leftRoots.empty()){
        auto add=[&](const auto& names,int side){for(const auto& name:names){
            auto node=resolve(name);if(!node||node==anchor||bodyBone(node)||!inside(node,anchor))return false;
            bool used=false;for(auto bone:weighted)if(inside(bone,node)){used=true;break;}if(!used)return false;
            roots.emplace_back(node,side);}return true;};
        if(!add(profile.leftRoots,1)||!add(profile.rightRoots,-1)){out.reason="Configured wing roots are missing or do not drive this model";return out;}
    }else{
        std::unordered_set<Object*> unique;
        for(auto bone:weighted){if(bone==anchor)continue;auto root=bone;
            for(unsigned depth=0;root->parent&&root->parent!=anchor&&depth<64;++depth)root=root->parent;
            if(root->parent!=anchor||bodyBone(root)||!unique.insert(root).second)continue;
            RE::NiPoint3 delta=out.body->world.rotate.Transpose()*(root->world.translate-anchor->world.translate);
            // Near-centre branches use their weighted descendants to establish side.
            if(std::abs(delta.x)<.01f){float furthest{};for(auto candidate:weighted)if(inside(candidate,root)){
                const auto q=out.body->world.rotate.Transpose()*(candidate->world.translate-anchor->world.translate);
                if(std::abs(q.x)>std::abs(furthest))furthest=q.x;}delta.x=furthest;}
            if(std::abs(delta.x)<.01f)continue;roots.emplace_back(root,delta.x>0?1:-1);
        }
    }
    std::vector<std::pair<Object*,int>> ordered;
    for(int side:{1,-1}){
        std::vector<Object*> group;for(const auto& [root,s]:roots)if(s==side)group.push_back(root);
        // Configured roots keep their INI order: it defines the pairs, and the
        // per-pair settings, that the user sees and edits.
        if(profile.leftRoots.empty())std::sort(group.begin(),group.end(),[&](auto a,auto b){const auto ra=out.body->world.rotate.Transpose()*(a->world.translate-anchor->world.translate),rb=out.body->world.rotate.Transpose()*(b->world.translate-anchor->world.translate);
            return ra.z==rb.z?std::abs(ra.x)<std::abs(rb.x):ra.z>rb.z;});
        for(auto node:group)ordered.emplace_back(node,side);
    }
    const auto left=std::count_if(ordered.begin(),ordered.end(),[](const auto& v){return v.second>0;});
    if(!left||left>8||left*2!=static_cast<int>(ordered.size())){out.reason="Cannot establish balanced left/right wing roots; provide LeftRoots and RightRoots";return out;}
    for(unsigned i=0;i<ordered.size();++i)for(unsigned j=i+1;j<ordered.size();++j)
        if(inside(ordered[i].first,ordered[j].first)||inside(ordered[j].first,ordered[i].first)){out.reason="Wing root branches overlap";return out;}
    std::unordered_set<Object*> added;
    for(const auto& [root,side]:ordered){
        if(!root->name.c_str()||root->name.empty()){out.reason="Wing root has no stable name";return out;}
        auto& sideNames=side>0?out.left:out.right;const auto pair=static_cast<unsigned>(sideNames.size());
        sideNames.emplace_back(channel::stableWingNodeName(root->name.c_str()));
        const auto branchStart=out.bones.size();unsigned branchBudget=256;
        walk(root,[&](Object* node){
            if(bodyBone(node)||!node->AsNode()||!added.insert(node).second)return;
            bool used=weighted.contains(node);if(!used)for(auto bone:weighted)if(inside(bone,node)){used=true;break;}
            if(!used)return;
            unsigned depth{};for(auto p=node;p&&p!=root;p=p->parent)++depth;
            out.bones.push_back({RE::NiPointer<Object>(node),side,depth,0,0,std::nullopt,pair});
        },branchBudget);
        if(!branchBudget||out.bones.size()>128){out.reason="Wing rig exceeds 128 controlled bones";return out;}
        // Normalize by cumulative segment length within each wing, so shorter
        // chains and six-wing meshes do not require a provider-specific map.
        std::unordered_map<Object*,float> positions;float longest{};unsigned deepest{};
        for(auto i=branchStart;i<out.bones.size();++i){auto& b=out.bones[i];float distance{};
            unsigned hops{};for(auto node=b.node.get();node&&node!=root&&hops++<64;node=node->parent){
                if(!node->parent)break;const float segment=(node->world.translate-node->parent->world.translate).Length();
                if(std::isfinite(segment))distance+=segment;
            }
            positions[b.node.get()]=distance;longest=std::max(longest,distance);deepest=std::max(deepest,b.depth);
        }
        for(auto i=branchStart;i<out.bones.size();++i){auto& b=out.bones[i];if(!b.depth)continue;Object* parent=b.node->parent;
            unsigned hops{};while(parent&&parent!=root&&!positions.contains(parent)&&hops++<64)parent=parent->parent;
            if(longest>.0001f){b.span=positions[b.node.get()]/longest;b.parentSpan=positions.contains(parent)?positions[parent]/longest:0;}
            else{b.span=static_cast<float>(b.depth)/std::max(deepest,1u);b.parentSpan=b.depth?static_cast<float>(b.depth-1)/std::max(deepest,1u):0;}
        }
    }
    // Skin inverse-bind transforms are static, unlike the animated scene nodes.
    // Derive a local reference only from a parent/child pair in the SAME skin.
    // This also cancels geometry-space differences between separate mesh pieces.
    for(auto& b:out.bones){
        for(const auto& link:out.skins){
            const auto data=link.instance->skinData.get();
            if(!data->boneData)continue;
            std::optional<unsigned> childIndex,parentIndex;
            for(const auto& [index,node]:link.bones){if(node==b.node)childIndex=index;if(node.get()==b.node->parent)parentIndex=index;}
            if(!childIndex||!parentIndex)continue;
            const auto local=data->GetBoneDataSkinToBone(*parentIndex).rotate*
                data->GetBoneDataSkinToBone(*childIndex).rotate.Transpose();
            bool finite=true;for(const auto& row:local.entry)for(float x:row)finite&=std::isfinite(x);
            if(finite){b.reference=rotation(local);break;}
        }
    }
    // Calibrate character-style axes from immutable skin bind data. Runtime
    // rotations above the attachment (spine, look-at, COM lean) are NOT axes.
    bool calibrated=false;
    for(const auto& link:out.skins){
        const auto data=link.instance->skinData.get();if(!data->boneData)continue;
        for(const auto& [index,node]:link.bones){
            if(node.get()!=anchor)continue;
            out.referenceToAnchor=data->GetBoneDataSkinToBone(index).rotate*data->rootParentToSkin.rotate;
            calibrated=true;break;
        }
        if(calibrated)break;
    }
    // An unweighted attachment may still be reconstructed via a fully bound
    // descendant path. Do not capture a camera-dependent live body orientation.
    if(!calibrated)for(const auto& link:out.skins){
        const auto data=link.instance->skinData.get();if(!data->boneData)continue;
        for(const auto& [index,node]:link.bones){
            RE::NiMatrix3 boneToAnchor;auto current=node.get();unsigned hops{};
            while(current&&current!=anchor&&hops++<64){
                const auto found=std::find_if(out.bones.begin(),out.bones.end(),[&](const auto& b){return b.node.get()==current;});
                if(found==out.bones.end()||!found->reference)break;
                const auto r=legs::matrix(*found->reference);RE::NiMatrix3 local;
                for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)local.entry[i][j]=r[i][j];
                boneToAnchor=local*boneToAnchor;current=current->parent;
            }
            if(current==anchor){out.referenceToAnchor=boneToAnchor*data->GetBoneDataSkinToBone(index).rotate*data->rootParentToSkin.rotate;calibrated=true;break;}
        }
        if(calibrated)break;
    }
    // When a model supplies no bind-space orientation for its attachment, use
    // that attachment's own axes. They are stable and can be tuned in the UI.
    return out;
}
struct Source {RE::TESForm* form{};RE::ActiveEffect* effect{};RE::NiPointer<Object> model;bool armor{};const RE::TESModel* part{};};
std::vector<Source> sources(RE::Actor* actor){
    std::vector<Source> result;if(!actor||!actor->Is3DLoaded())return result;
    if(const auto biped=actor->GetBiped()){
        std::unordered_set<Object*> seen;std::unordered_set<RE::TESForm*> modelLess;
        for(const auto& part:biped->objects)if(part.item&&part.item->As<RE::TESObjectARMO>()
            &&(part.partClone?seen.insert(part.partClone.get()).second:modelLess.insert(part.item).second))
            result.push_back({part.item,nullptr,part.partClone,true,part.part});
    }
    if(const auto effects=actor->AsMagicTarget()->GetActiveEffectList()){
        unsigned count{};
        for(auto effect:*effects){if(++count>256||result.size()>=96)break;
            if(!effect||effect->target!=actor->AsMagicTarget()||effect->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled)||!effect->GetBaseObject())continue;
            bool attached=false;
            if(effect->hitEffects)for(auto visual:*effect->hitEffects){
                if(!visual||visual->finished||visual->GetType()!=RE::TEMP_EFFECT_TYPE::kRefModel)continue;
                auto model=static_cast<RE::ModelReferenceEffect*>(visual);
                if(model->GetAttached()&&model->Get3D()){result.push_back({effect->GetBaseObject(),effect,RE::NiPointer<Object>(model->Get3D()),false});attached=true;}
                if(result.size()>=96)break;
            }
            if(!attached)result.push_back({effect->GetBaseObject(),effect,{},false});
        }
    }
    return result;
}
bool sourceMatches(const WingProfile& p,const Source& s){
    if(!s.form)return false;
    auto it=resolvedForms.find(&p);
    if(it==resolvedForms.end()){
        std::unordered_set<RE::TESForm*> forms;const auto data=RE::TESDataHandler::GetSingleton();
        for(const auto& ids:{p.effectIDs,p.armorIDs})for(auto id:ids)if(auto form=data->LookupForm(id,p.plugin))forms.insert(form);
        it=resolvedForms.emplace(&p,std::move(forms)).first;
    }
    return it->second.contains(s.form);
}
RE::NiControllerManager* controllerManager(Object* root,unsigned depth=0){
    if(!root||depth>32)return nullptr;
    if(auto manager=root->GetController<RE::NiControllerManager>())return manager;
    if(auto node=root->AsNode())for(auto& child:node->GetChildren())if(auto m=controllerManager(child.get(),depth+1))return m;
    return nullptr;
}
// The worn armor's model record, for a clone that no longer carries its
// controller manager.
const RE::TESModel* armorModel(RE::Actor* actor,Object* model){
    if(!actor||!model)return nullptr;
    if(const auto biped=actor->GetBiped())for(const auto& part:biped->objects)if(part.partClone.get()==model)return part.part;
    return nullptr;
}
// The model's own wingbeat sequences, read without starting or changing them.
// A clone without its controller manager falls back to the shared NIF from
// the model database, which is never modified either.
struct AuthoredClips {
    RE::NiPointer<RE::NiNode> file;
    std::array<RE::NiControllerSequence*,3> sequences{};
    std::string reason;
    explicit operator bool()const{return sequences[0]!=nullptr;}
};
AuthoredClips authoredClips(Object* model,const RE::TESModel* part,const WingProfile& p){
    AuthoredClips out;auto manager=controllerManager(model);
    const auto path=part?part->GetModel():nullptr;
    if(!manager&&path&&*path){
        RE::BSModelDB::DBTraits::ArgsType args{};
        if(RE::BSModelDB::Demand(path,out.file,args)==RE::BSResource::ErrorCode::kNone&&out.file)manager=controllerManager(out.file.get());
    }
    if(!manager){out.reason="model has no embedded animation sequences";return out;}
    const std::array<const std::string*,3> names{&p.flapSequence,&p.glideSequence,&p.spreadSequence};
    for(unsigned i=0;i<3;++i){
        auto q=names[i]->empty()?nullptr:manager->GetSequenceByName(*names[i]);
        if(q&&(q->arraySize<1||q->arraySize>256||!q->idTagArray.data()||!q->interpArray.data()
            ||!std::isfinite(q->endKeyTime-q->beginKeyTime)||q->endKeyTime-q->beginKeyTime<=0||q->endKeyTime-q->beginKeyTime>30)){
            diagnostic::report("registration.authored-sequence-invalid","ignore authored sequence",q,names[i]->c_str(),q->arraySize,256);q=nullptr;}
        out.sequences[i]=q?q:i?out.sequences[i-1]:nullptr;
    }
    if(!out.sequences[0])out.reason="PowerSequence '"+p.flapSequence+"' was not found in the model";
    return out;
}
struct CachedRig {RE::ActorHandle actor;const WingProfile* profile{};RE::NiPointer<Object> body,model;Rig rig;std::chrono::steady_clock::time_point inspected;};
std::vector<CachedRig> rigCache;
bool currentRig(const Rig& rig){
    for(const auto& link:rig.skins){
        if(!inside(link.geometry.get(),rig.model.get())||skin(link.geometry.get())!=link.instance.get())return false;
        const auto instance=link.instance.get();if(!instance||!instance->skinData||!instance->bones)return false;
        const auto count=instance->skinData->GetBoneCount();
        for(const auto& [index,bone]:link.bones)if(index>=count||instance->bones[index]!=bone.get())return false;
    }
    for(const auto& b:rig.bones)if(!b.depth&&!inside(b.node.get(),rig.body.get())&&!inside(b.node.get(),rig.model.get()))return false;
    return true;
}
const Rig& cachedRig(RE::Actor* actor,Object* model,const WingProfile& p){
    const auto now=std::chrono::steady_clock::now();
    for(auto it=rigCache.begin();it!=rigCache.end();++it)if(it->profile==&p&&it->body.get()==actor->Get3D(false)&&it->model.get()==model&&it->actor.get().get()==actor){
        if(it->rig?currentRig(it->rig):now-it->inspected<std::chrono::seconds(1))return it->rig;
        rigCache.erase(it);break;
    }
    auto rig=inspect(actor,model,p);if(rigCache.size()>=128)rigCache.erase(rigCache.begin());
    rigCache.push_back({actor->GetHandle(),&p,RE::NiPointer<Object>(actor->Get3D(false)),RE::NiPointer<Object>(model),std::move(rig),now});
    const auto& added=rigCache.back().rig;
    if(!added)diagnostic::report("registration.rig-unavailable","leave actor's other wing providers available",actor,p.id+": "+added.reason);
    return added;
}
legs::Quaternion rotation(const RE::NiMatrix3& m){legs::Matrix a;for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)a[i][j]=m.entry[i][j];return legs::quaternion(a);}
void rotation(RE::NiMatrix3& m,legs::Quaternion q){const auto a=legs::matrix(q);for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)m.entry[i][j]=a[i][j];}
using SceneMap=std::unordered_map<Object*,std::vector<std::weak_ptr<RegisteredWingRuntime::State>>>;
std::mutex sceneMutex;
std::atomic<std::shared_ptr<const SceneMap>> scenes;
struct ControllerOwner {std::weak_ptr<RegisteredWingRuntime::State> state;unsigned bone{};};
using ControllerMap=std::unordered_map<RE::NiTimeController*,ControllerOwner>;
std::atomic<std::shared_ptr<const ControllerMap>> wingControllers;
struct TransformUpdate {
    static void thunk(RE::NiTimeController* controller,float time);
    static inline REL::Relocation<decltype(thunk)> original;
};
bool transformHookInstalled{};
bool installTransformHook(){
    if(transformHookInstalled)return true;
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_NiTransformController[0]};
    const auto previous=*reinterpret_cast<const std::uintptr_t*>(table.address()+0x27*sizeof(void*));
    if(!runtime::executableMemory(previous)||previous==reinterpret_cast<std::uintptr_t>(&TransformUpdate::thunk)){
        diagnostic::report("registration.transform-callback-unavailable","cannot retain procedural pose after model controllers",nullptr,fmt::format("target={:X}",previous));return false;}
    // NiTimeController::Update(float), inherited by NiTransformController.
    // Only controllers belonging to registered wing bones receive a post-write.
    TransformUpdate::original=table.write_vfunc(0x27,TransformUpdate::thunk);transformHookInstalled=true;
    SKSE::log::info("Registered wing transform callback installed: chainedTarget={:X}; explicit owned wing controllers only",previous);return true;
}
}

void loadWingRegistrations(){
    SKSE::log::info("Wing registration channel revision=template-channel-10: attachment-local wing axes; static bind calibration; camera-independent wing articulation");
    const auto folder=directory();if(!std::filesystem::is_directory(folder))return;
    {std::lock_guard lock(registryMutex);retiredProfiles.insert(retiredProfiles.end(),registrations.begin(),registrations.end());registrations.clear();rigCache.clear();resolvedForms.clear();}
    std::vector<std::filesystem::path> paths;for(const auto& entry:std::filesystem::directory_iterator(folder))if(entry.path().extension()==".ini")paths.push_back(entry.path());
    std::sort(paths.begin(),paths.end());
    for(const auto& path:paths){
        char kind[64]{};GetPrivateProfileStringA("Wings","Template","",kind,sizeof(kind),path.string().c_str());if(!kind[0])continue;
        try{
            auto p=std::make_shared<WingProfile>(readWingRegistration(path));
            std::lock_guard lock(registryMutex);auto found=std::find_if(registrations.begin(),registrations.end(),[&](const auto& old){return old->registrationFile==p->registrationFile;});
            if(found==registrations.end())registrations.push_back(p);else{retiredProfiles.push_back(*found);*found=p;}
            SKSE::log::info("Wing registration loaded: file={} plugin={} effects={} armors={} template={}; model bindings are transient, INI rules persist",path.filename().string(),p->plugin,p->effectIDs.size(),p->armorIDs.size(),kind);
        }catch(const std::exception& e){SKSE::log::error("Wing registration rejected: file={} reason={}; other registrations retained",path.string(),e.what());}
    }
}
WingMatch findRegisteredWings(RE::Actor* actor){
    if(!actor||!actor->Is3DLoaded()||actor->IsDead()||knockedDown(actor))return {};
    std::lock_guard lock(registryMutex);if(registrations.empty())return {};
    auto current=sources(actor);
    // An intentionally model-less armor may have no biped clone at all.
    // Only explicit Invisible rules use this inventory fallback.
    for(const auto& p:registrations)if(p->wingTemplate==channel::Template::invisible)for(auto id:p->armorIDs){
        const auto form=RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESObjectARMO>(id,p->plugin);
        if(form&&std::none_of(current.begin(),current.end(),[&](const auto& s){return s.form==form;})&&actor->GetWornArmor(form->GetFormID(),true))current.push_back({form,nullptr,{},true});
    }
    for(const auto& p:registrations)for(const auto& source:current)if(sourceMatches(*p,source)){
        if(p->wingTemplate==channel::Template::invisible)return {p.get(),source.effect,actor,nullptr};
        if(source.model&&cachedRig(actor,source.model.get(),*p))return {p.get(),source.effect,actor,source.model.get()};
    }
    return {};
}
std::vector<WingCandidate> scanWingCandidates(RE::Actor* actor){
    std::vector<WingCandidate> result;
    const auto data=RE::TESDataHandler::GetSingleton();
    const bool awuSound=data&&data->LookupForm<RE::BGSSoundDescriptorForm>(0xC6B,"Animated Wings Ultimate.esp");
    for(const auto& source:sources(actor)){
        // No model can ever produce an automatically created Invisible rule.
        if(!source.model)continue;
        const auto file=source.form->GetFile(0);if(!file)continue;
        WingCandidate c;c.profile.plugin=file->fileName;c.profile.id=source.form->GetName();c.name=c.profile.id;
        if(c.name.empty())c.name=source.form->GetFormEditorID();if(c.name.empty())c.name=fmt::format("{:08X}",source.form->GetFormID());
        auto local=source.form->GetLocalFormID();(source.armor?c.profile.armorIDs:c.profile.effectIDs).push_back(local);
        c.profile.wingTemplate=channel::Template::awu;c.profile.wingPose=channel::defaults(c.profile.wingTemplate);
        defaultWingRegistrationSounds(c.profile,awuSound);
        const auto rig=inspect(actor,source.model.get(),c.profile);c.compatible=static_cast<bool>(rig);c.reason=rig.reason;
        // Offer only rigs that a registration can actually store and read back.
        if(c.compatible){bool storable=channel::storableWingName(channel::stableWingNodeName(rig.anchor->name.c_str()));
            for(const auto& names:{rig.left,rig.right})for(const auto& name:names)storable&=channel::storableWingName(name);
            if(!storable){c.compatible=false;c.reason="Wing bone names cannot be stored in a registration INI";}}
        c.bones=static_cast<unsigned>(rig.bones.size());c.pairs=static_cast<unsigned>(rig.left.size());
        if(c.compatible){c.profile.rootNode=channel::stableWingNodeName(rig.anchor->name.c_str());c.profile.leftRoots=rig.left;c.profile.rightRoots=rig.right;c.reason="Independent skinned wing branches found; choose a template and tune in flight";
            // Wings that carry their own wingbeat sequences play them by default.
            if(authoredClips(source.model.get(),source.part,c.profile)){c.authored=true;c.profile.wingTemplate=channel::Template::authored;c.profile.wingPose=channel::defaults(c.profile.wingTemplate);
                c.reason="Embedded wingbeat animations found; Authored plays them on these wing bones";}}
        result.push_back(std::move(c));
    }
    return result;
}
std::vector<WingProfile> wingRegistrationProfiles(){std::lock_guard lock(registryMutex);std::vector<WingProfile> out;for(const auto& p:registrations)out.push_back(*p);return out;}
std::string saveWingRegistration(WingProfile p,bool fromScanner){
    try{
        if(fromScanner&&p.wingTemplate==channel::Template::invisible)return "Invisible wings can only be registered by editing an INI.";
        if(p.wingTemplate==channel::Template::legacy)return "Choose a wing animation template.";
        if(p.effectIDs.empty()&&p.armorIDs.empty())return "Registration requires an effect or armor FormID.";
        const auto folder=directory();std::filesystem::create_directories(folder);
        std::filesystem::path path;
        if(!p.registrationFile.empty())path=std::filesystem::absolute(p.registrationFile);
        else{
            auto stem=std::filesystem::path(p.plugin).stem().string();for(auto& c:stem)if(!std::isalnum(static_cast<unsigned char>(c))&&c!='-'&&c!='_')c='_';
            const auto id=!p.armorIDs.empty()?p.armorIDs.front():p.effectIDs.front();
            path=folder/fmt::format("Custom-{}-{}-{:06X}.ini",stem,p.armorIDs.empty()?"Effect":"Armor",id);
        }
        if(path.parent_path()!=folder)return "Registration must be inside SKSE/Plugins/Wingflight/Wings.";
        const auto text=wingRegistrationText(p);auto temporary=path;temporary+=".tmp";
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file.exceptions(std::ios::failbit|std::ios::badbit);file<<text;}
        try{const auto verified=readWingRegistration(temporary);(void)verified;}
        catch(...){std::error_code ignored;std::filesystem::remove(temporary,ignored);throw;}
        if(std::filesystem::exists(path)){auto backup=path;backup+=".bak";std::filesystem::copy_file(path,backup,std::filesystem::copy_options::overwrite_existing);}
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not replace wing registration INI");
        WritePrivateProfileStringA(nullptr,nullptr,nullptr,path.string().c_str());
        auto next=std::make_shared<WingProfile>(readWingRegistration(path));
        {std::lock_guard lock(registryMutex);auto it=std::find_if(registrations.begin(),registrations.end(),[&](const auto& old){return old->registrationFile==next->registrationFile;});
            if(it==registrations.end())registrations.push_back(next);else{retiredProfiles.push_back(*it);*it=next;}rigCache.clear();resolvedForms.clear();}
        SKSE::log::info("Wing registration saved: {}; player and NPC rules shared; unequipping retains the file",path.string());return {};
    }catch(const std::exception& e){SKSE::log::error("Wing registration save failed: {}",e.what());return e.what();}
}

struct RegisteredWingRuntime::State : std::enable_shared_from_this<State> {
    struct Bone {RigBone rig;legs::Quaternion reference,before,output;bool wrote{};legs::Quaternion editFrom;
        std::array<RE::NiPointer<RE::NiInterpolator>,3> clips;}; // Private copies of authored power/fast/spread tracks.
    std::mutex mutex;RE::ActorHandle actor;RE::NiPointer<Object> body,model,anchor;
    RE::NiMatrix3 referenceToAnchor;
    const WingProfile* profile{};std::vector<Bone> bones;LogicalWings clocks;double flowTime{};
    // Per pair: its own root-angle history for root-to-tip flow. Shared stroke
    // progress histories let a pair follow the driven stroke after its delay.
    std::array<channel::WingFlowHistory,channel::PoseSettings::maxPairs> flowHistory;
    channel::WingFlowHistory powerHistory,fastHistory;channel::StrokeProgress powerProgress,fastProgress;
    void clearFlow(){for(auto& h:flowHistory)h.clear();powerHistory.clear();fastHistory.clear();powerProgress.clear();fastProgress.clear();}
    float dt{},clock{},ownership{},targetOwnership{},pitch{},fastRate{1},editBlend{1};bool released{};
    // Authored: sequence timing and the model file that owns the source keys.
    bool authored{};std::array<float,3> clipBegin{},clipDuration{};RE::NiPointer<RE::NiNode> clipFile;
    unsigned bindAuthored(AuthoredClips clips){
        const auto table=REL::Relocation<std::uintptr_t>(RE::VTABLE_NiTransformInterpolator[0]).address();
        std::unordered_map<RE::NiInterpolator*,RE::NiPointer<RE::NiInterpolator>> copies;unsigned bound{},foreign{};
        for(unsigned i=0;i<3;++i){const auto q=clips.sequences[i];
            clipBegin[i]=q->beginKeyTime;clipDuration[i]=q->endKeyTime-q->beginKeyTime;
            for(unsigned j=0;j<q->arraySize;++j){
                const auto name=channel::stableWingNodeName(q->idTagArray[j].avObjectName.c_str());
                const auto bone=std::find_if(bones.begin(),bones.end(),[&](const Bone& b){return channel::stableWingNodeName(b.rig.node->name.c_str())==name;});
                if(bone==bones.end())continue;
                const auto interp=q->interpArray[j].interpolator.get();
                // Keyframed transforms only; the sampler reads them on our private copy.
                if(!interp||*reinterpret_cast<const std::uintptr_t*>(interp)!=table){++foreign;continue;}
                auto& copy=copies[interp];
                if(!copy){RE::NiPointer<RE::NiObject> object;interp->CreateDeepCopy(object);
                    const auto result=object?static_cast<RE::NiInterpolator*>(object.get()):nullptr;
                    if(!result||result==interp||*reinterpret_cast<const std::uintptr_t*>(result)!=table){
                        diagnostic::report("registration.authored-copy-failed","leave bone to the model's own motion",interp,std::string(name));continue;}
                    copy.reset(result);}
                bone->clips[i]=copy;
            }
        }
        for(const auto& b:bones)if(b.clips[0]||b.clips[1]||b.clips[2])++bound;
        clipFile=std::move(clips.file);authored=bound>0;
        SKSE::log::info("Registered wing authored sequences: bones={}/{} privateTracks={} unsupportedTracks={} durations={:.4f}/{:.4f}/{:.4f} fromFile={}; model keys unchanged",
            bound,bones.size(),copies.size(),foreign,clipDuration[0],clipDuration[1],clipDuration[2],clipFile!=nullptr);
        return bound;
    }
    legs::Quaternion authoredPose(const Bone& b,const std::array<float,3>& phases,const WaveWeights& w,legs::Quaternion native){
        const visual::Sample current{0,0,0,native.w,native.x,native.y,native.z,1};
        std::array<visual::Sample,4> poses{current,current,current,current};
        const std::array<float,4> weights{0,w.flap,w.fast,w.spread};
        if(!(weights[1]+weights[2]+weights[3]>0))return native;
        for(unsigned i=0;i<3;++i){
            if(weights[i+1]<=0||!b.clips[i])continue;
            visual::Sample value{};auto& output=*reinterpret_cast<RE::NiQuatTransform*>(&value);
            const float time=clipBegin[i]+phases[i]*clipDuration[i];
            if(b.clips[i]->Update1(time,b.rig.node.get(),output))poses[i+1]=visual::validSample(value,current,b.rig.node.get());
            else diagnostic::report("registration.authored-sample-failed","use current bone pose",b.clips[i].get(),b.rig.node->name.c_str(),time);
        }
        const auto result=visual::combine(poses,weights);
        return {result.qx,result.qy,result.qz,result.w};
    }
    std::uint64_t tick{},applied{};
    bool current(){const auto a=actor.get();return a&&a->Is3DLoaded()&&a->Get3D(false)==body.get()&&!a->IsDead()&&!knockedDown(a);}
    void restore(){for(auto& b:bones)if(b.wrote&&std::abs(legs::dot(rotation(b.rig.node->local.rotate),b.output))>.99999f){rotation(b.rig.node->local.rotate,b.before);b.wrote=false;}}
    void apply(Object* object,RE::NiUpdateData* data,bool post){
        std::lock_guard lock(mutex);if(!profile||body.get()!=object||!current())return;
        const bool active=ownership>.0001f;
        if(!active){restore();return;}
        const bool repeated=applied==tick;
        bool changed=false;
        const auto& p=profile->wingPose;const auto kind=profile->wingTemplate;const float downEnd=profile->timing.downstrokeEnd;
        const auto& w=clocks.weights;
        if(!repeated){
            powerHistory.push(flowTime,static_cast<float>(powerProgress.update(clocks.phase)));
            fastHistory.push(flowTime,static_cast<float>(fastProgress.update(clocks.fastPhase)));
        }
        // Each pair samples the same driven stroke, optionally a little later.
        struct PairMotion {float power{},fast{},spread{},clock{},scale{1},rootAngle{},bend{};};
        std::array<PairMotion,channel::PoseSettings::maxPairs> pairs;
        unsigned pairCount{};for(const auto& b:bones)pairCount=std::max(pairCount,std::min(b.rig.pair+1,channel::PoseSettings::maxPairs));
        for(unsigned i=0;i<pairCount;++i){auto& m=pairs[i];
            const float delay=p.pairDelaySeconds[i];m.scale=p.pairScale[i];
            m.power=clocks.phase;m.fast=clocks.fastPhase;m.spread=clocks.spreadPhase;m.clock=clock;
            if(delay>0){
                m.spread=channel::wrap(clocks.spreadPhase-delay*clocks.spreadRate/LogicalWings::spreadDuration);
                m.power=channel::wrap(powerHistory.sample(flowTime-delay,m.power));
                m.fast=channel::wrap(fastHistory.sample(flowTime-delay,m.fast));
                m.clock=clock-delay;
            }
            m.rootAngle=m.scale*channel::angle(p,kind,m.power,m.fast,m.clock,w.flap,w.fast,w.spread,downEnd,0,clocks.rate,clocks.fastRate);
            if(!repeated)flowHistory[i].push(flowTime,m.rootAngle);
            m.bend=channel::wingBendWeight(kind,m.power,m.fast,downEnd,w.flap,w.fast,w.spread,p.bottomHold);
        }
        // Explicit fold/feather articulation owns the raised pose, so flow no
        // longer has to be suppressed there to keep the wing extended.
        const bool articulated=channel::articulated(p,kind);
        for(auto& b:bones){
            // Authored leaves bones without a track to the model's own motion.
            if(authored&&!b.clips[0]&&!b.clips[1]&&!b.clips[2])continue;
            auto native=rotation(b.rig.node->local.rotate);
            if(repeated){
                if(b.wrote&&std::abs(legs::dot(native,b.output))<.99999f){b.before=native;rotation(b.rig.node->local.rotate,b.output);changed=true;}
                continue;
            }
            if(b.wrote&&std::abs(legs::dot(native,b.output))>.99999f)native=b.before;
            b.before=native;
            // Preserve a chosen fraction of authored sway, without accumulating our previous overlay.
            auto base=legs::blend(b.reference,native,p.nativeSwayWeight);
            RE::NiMatrix3 basis;auto parent=b.rig.node->parent;unsigned depth{};
            while(parent&&parent!=anchor.get()&&depth++<64){basis=parent->local.rotate*basis;parent=parent->parent;}
            if(!anchor||parent!=anchor.get())continue;
            const auto tilted=channel::wingParentAxis(basis.entry,referenceToAnchor.entry,{1,0,0});
            const RE::NiPoint3 tiltAxis(tilted[0],tilted[1],tilted[2]);
            if(authored){
                const auto& m=pairs[std::min(b.rig.pair,channel::PoseSettings::maxPairs-1)];
                auto target=authoredPose(b,{m.power,m.fast,m.spread},w,native);
                target=channel::tiltWingRoot(target,p.forwardTiltDegrees,b.rig.depth,tiltAxis.x,tiltAxis.y,tiltAxis.z);
                if(!b.rig.depth&&pitch!=0&&w.flap>0)target=legs::bankAroundAxis(target,-pitch*w.flap*.25f,tiltAxis.x,tiltAxis.y,tiltAxis.z);
                b.output=channel::poseOutput(native,target,b.editFrom,ownership,editBlend);
                b.wrote=true;rotation(b.rig.node->local.rotate,b.output);changed=true;continue;
            }
            base=channel::tiltWingRoot(base,p.forwardTiltDegrees,b.rig.depth,tiltAxis.x,tiltAxis.y,tiltAxis.z);
            const auto mirroredAxis=channel::mirroredFlapAxis(p,b.rig.side);
            const auto mapped=channel::wingParentAxis(basis.entry,referenceToAnchor.entry,mirroredAxis);
            RE::NiPoint3 axis(mapped[0],mapped[1],mapped[2]);
            const float length=axis.Length();if(!std::isfinite(length)||length<.0001f)continue;axis=axis/length;
            const auto& m=pairs[std::min(b.rig.pair,channel::PoseSettings::maxPairs-1)];
            auto angle=p.flowStrength>0?(b.rig.depth==0?m.rootAngle:flowHistory[std::min(b.rig.pair,channel::PoseSettings::maxPairs-1)].jointAngle(flowTime,m.rootAngle,
                b.rig.span,b.rig.parentSpan,p.flowStrength,p.tipDelaySeconds)):
                m.scale*channel::angle(p,kind,m.power,m.fast,m.clock,w.flap,w.fast,w.spread,downEnd,b.rig.depth,clocks.rate,clocks.fastRate);
            if(b.rig.depth){
                if(!articulated)angle*=m.bend;
                angle+=m.scale*channel::articulation(p,kind,m.power,m.fast,w.flap,w.fast,downEnd,b.rig.span,b.rig.parentSpan);
            }
            auto target=legs::bankAroundAxis(base,-angle,axis.x,axis.y,axis.z);
            if(!b.rig.depth&&pitch!=0&&clocks.weights.flap>0)target=legs::bankAroundAxis(target,-pitch*clocks.weights.flap*.25f,tiltAxis.x,tiltAxis.y,tiltAxis.z);
            b.output=channel::poseOutput(native,target,b.editFrom,ownership,editBlend);
            b.wrote=true;rotation(b.rig.node->local.rotate,b.output);
            changed=true;
        }
        applied=tick;
        if(!post||!data||!changed)return;
        unsigned budget=1024;
        for(const auto& b:bones)if(b.rig.depth==0)walk(b.rig.node.get(),[&](Object* n){n->UpdateWorldData(data);},budget);
        if(const auto tree=netimmerse_cast<RE::BSFlattenedBoneTree*>(object)){
            auto& cache=tree->GetRuntimeData();if(!cache.boneEntries||cache.numBones>2048)return;
            for(unsigned i=0;i<cache.numBones;++i){auto& bone=cache.boneEntries[i];if(bone.node){bone.local=bone.node->local;bone.world=bone.node->world;}
                else if(bone.parentIndex>=0&&static_cast<unsigned>(bone.parentIndex)<i)bone.world=cache.boneEntries[bone.parentIndex].world*bone.local;}
        }
    }
};
void TransformUpdate::thunk(RE::NiTimeController* controller,float time){
    original(controller,time);
    const auto map=wingControllers.load();if(!map)return;
    const auto found=map->find(controller);if(found==map->end())return;
    const auto state=found->second.state.lock();if(!state)return;
    bool compute{};
    {std::lock_guard lock(state->mutex);
        if(!state->profile||!state->current()||state->ownership<=.0001f||found->second.bone>=state->bones.size())return;
        auto& b=state->bones[found->second.bone];if(controller->target!=b.rig.node.get())return;
        compute=state->applied!=state->tick;
        if(!compute&&b.wrote){const auto native=rotation(b.rig.node->local.rotate);b.before=native;rotation(b.rig.node->local.rotate,b.output);}
    }
    if(compute)state->apply(state->body.get(),nullptr,false);
}
RegisteredWingRuntime::RegisteredWingRuntime()=default;
RegisteredWingRuntime::~RegisteredWingRuntime(){release(false);}
WingPlayback RegisteredWingRuntime::prepare(WingMatch match){
    if(!registeredWing(match.profile)||!match.owner)return {};
    if(match.profile->wingTemplate!=channel::Template::invisible&&!vampireSceneReady()){
        diagnostic::report("registration.scene-callback-unavailable","cannot animate registered wing bones",match.owner,match.profile->id);return {};}
    bool sameBones=match.profile->wingTemplate==channel::Template::invisible;Rig replacement;
    if(!sameBones){
        std::lock_guard lock(registryMutex);const auto& rig=cachedRig(match.owner,match.model,*match.profile);
        if(!rig){release(false);return {};}
        sameBones=state_&&state_->anchor==rig.anchor&&state_->bones.size()==rig.bones.size();
        if(sameBones)for(unsigned i=0;i<rig.bones.size();++i){const auto& old=state_->bones[i].rig;const auto& next=rig.bones[i];
            if(old.node!=next.node||old.side!=next.side||old.depth!=next.depth||old.pair!=next.pair
                ||std::abs(old.span-next.span)>.0001f||std::abs(old.parentSpan-next.parentSpan)>.0001f){sameBones=false;break;}}
        if(!sameBones||state_->profile!=match.profile||state_->body.get()!=match.owner->Get3D(false)||state_->model.get()!=match.model)replacement=rig;
    }
    if(state_){std::lock_guard lock(state_->mutex);const auto old=state_->profile;
        if(old&&old!=match.profile&&old->registrationFile==match.profile->registrationFile&&old->wingTemplate==match.profile->wingTemplate
            &&old->leftRoots==match.profile->leftRoots&&old->rightRoots==match.profile->rightRoots&&old->rootNode==match.profile->rootNode
            &&old->flapSequence==match.profile->flapSequence&&old->glideSequence==match.profile->glideSequence&&old->spreadSequence==match.profile->spreadSequence
            &&sameBones&&state_->body.get()==match.owner->Get3D(false)&&state_->model.get()==match.model){
            for(auto& b:state_->bones)b.editFrom=b.wrote?b.output:rotation(b.rig.node->local.rotate);
            state_->editBlend=0;state_->clearFlow();state_->profile=match.profile;
        }
    }
    if(!state_||!sameBones||state_->profile!=match.profile||state_->body.get()!=match.owner->Get3D(false)||state_->model.get()!=match.model){
        release(false);auto s=std::make_shared<State>();s->actor=match.owner->GetHandle();s->body.reset(match.owner->Get3D(false));s->model.reset(match.model);s->profile=match.profile;
        if(!s->body)return {};
        if(match.profile->wingTemplate!=channel::Template::invisible){
            if(!replacement)return {};
            s->anchor=replacement.anchor;s->referenceToAnchor=replacement.referenceToAnchor;
            for(const auto& b:replacement.bones){
                const auto q=rotation(b.node->local.rotate);
                auto reference=match.profile->wingTemplate==channel::Template::fairy?q:b.reference.value_or(q);
                if(match.profile->wingTemplate!=channel::Template::fairy&&b.depth){
                    RE::NiPoint3 outgoing{};unsigned children{};
                    if(const auto node=b.node->AsNode())for(const auto& child:node->GetChildren()){
                        if(!child||!child->AsNode()||bodyBone(child.get()))continue;
                        const float length=child->local.translate.Length();
                        if(std::isfinite(length)&&length>.00001f){++children;outgoing=child->local.translate;}
                    }
                    const auto incoming=b.node->local.translate;
                    // Branching joints retain the authored fan instead of
                    // choosing one finger/feather as the direction of all others.
                    if(children>1)outgoing={};
                    if(!children&&b.node->parent){
                        // A leaf can inherit the segment axis only when its two
                        // preceding local axes agree (e.g. a consistent X-axis rig).
                        const auto previous=b.node->parent->local.translate;
                        const float lengths=incoming.Length()*previous.Length();
                        if(lengths>.00001f&&incoming.Dot(previous)/lengths>.98f)outgoing=incoming;
                    }
                    reference=channel::extendedWingReference(reference,{outgoing.x,outgoing.y,outgoing.z},{incoming.x,incoming.y,incoming.z});
                }
                s->bones.push_back({b,reference,q,q,false,q});
            }
            if(match.profile->wingTemplate==channel::Template::authored){
                auto clips=authoredClips(match.model,armorModel(match.owner,match.model),*match.profile);
                const auto reason=clips.reason;
                if(!clips||!s->bindAuthored(std::move(clips)))
                    SKSE::log::warn("Registered wing {}: Authored sequences unavailable ({}); using the procedural wingbeat",match.profile->id,
                        reason.empty()?"no sequence track matches a registered wing bone":reason);
            }
            if(!installTransformHook())return {};
        }
        s->clocks.reset(match.profile->timing.glideEntryPhase);state_=s;
        {std::lock_guard lock(sceneMutex);auto next=std::make_shared<SceneMap>();if(const auto old=scenes.load())for(const auto& [object,entries]:*old)for(const auto& weak:entries)if(!weak.expired())(*next)[object].push_back(weak);
            (*next)[s->body.get()].push_back(s);scenes.store(next);
            auto controllers=std::make_shared<ControllerMap>();if(const auto old=wingControllers.load())for(const auto& [controller,owner]:*old)if(!owner.state.expired())controllers->emplace(controller,owner);
            const auto transformTable=REL::Relocation<std::uintptr_t>(RE::VTABLE_NiTransformController[0]).address();
            for(unsigned i=0;i<s->bones.size();++i){unsigned count{};for(auto c=s->bones[i].rig.node->GetControllers();c&&count++<16;c=c->GetNext())
                if(*reinterpret_cast<const std::uintptr_t*>(c)==transformTable)(*controllers)[c]={s,i};}
            wingControllers.store(controllers);
        }
        SKSE::log::debug("Registered wing bound: actor={:08X} template={} bones={}; wing branches only",match.owner->GetFormID(),channel::templateName(match.profile->wingTemplate),s->bones.size());
    }
    std::lock_guard lock(state_->mutex);auto& s=*state_;
    if(s.released){
        // A smooth release keeps this state while it fades. The next flight must
        // not resume the previous stroke: lift is earned only by real downstroke
        // progress, so a mid-downstroke phase would delay takeoff by a cycle.
        s.clearFlow();s.clocks.restart(s.profile->timing.glideEntryPhase);
        if(s.ownership>.0001f){
            for(auto& b:s.bones)b.editFrom=b.wrote?b.output:rotation(b.rig.node->local.rotate);
            s.editBlend=0; // Still visible: ease from the rendered pose to the raised start.
        }
    }
    s.released=false;
    return {.found=true,.active=true,.gliding=true,.spreadActive=true,.spreadWeight=s.clocks.weights.spread,.graphSpreadWeight=s.clocks.weights.spread,
        .spreadPhase=s.clocks.spreadPhase,.spreadFrequency=s.clocks.spreadRate,.phase=s.clocks.phase,.frequency=s.clocks.rate,.duration=LogicalWings::duration,
        .flapWeight=s.clocks.weights.flap,.glideWeight=s.clocks.weights.fast,.graphWeight=s.clocks.weights.flap,.otherWeight=s.clocks.weights.original,
        .graphGlideWeight=s.clocks.weights.fast,.glidePhase=s.clocks.fastPhase,.glideFrequency=s.clocks.fastRate,.activeSequence="Registered wing template"};
}
void RegisteredWingRuntime::advance(float dt){if(!state_||!std::isfinite(dt))return;std::lock_guard lock(state_->mutex);auto& s=*state_;s.dt=std::clamp(dt,0.f,.1f);s.clock+=s.dt*(s.profile->wingTemplate==channel::Template::fairy?s.fastRate:1.f);++s.tick;s.clocks.advance(s.dt);
    s.flowTime+=s.dt;
    s.editBlend=std::min(1.f,s.editBlend+s.dt/s.profile->wingPose.blendSeconds);
    s.ownership=legs::approach(s.ownership,s.released?0.f:s.targetOwnership,s.dt,s.profile->wingPose.blendSeconds);if(s.released&&s.ownership<=.0001f)s.restore();}
bool RegisteredWingRuntime::blend(float flap,float ownership,float fast){if(!state_)return false;std::lock_guard lock(state_->mutex);state_->targetOwnership=std::clamp(ownership,0.f,1.f);state_->clocks.blend(flap,1,fast);return true;}
bool RegisteredWingRuntime::preview(float amount,float period,float amplitude){if(!state_)return false;std::lock_guard lock(state_->mutex);state_->targetOwnership=std::clamp(amount,0.f,1.f);state_->clocks.blend(0,1,0);(void)period;(void)amplitude;return true;}
bool RegisteredWingRuntime::parkPower(){if(!state_)return false;std::lock_guard lock(state_->mutex);return state_->clocks.parkPower();}
bool RegisteredWingRuntime::parkFast(){if(!state_)return false;std::lock_guard lock(state_->mutex);return state_->clocks.parkFast();}
void RegisteredWingRuntime::powerRate(float rate){if(state_){std::lock_guard lock(state_->mutex);state_->clocks.powerRate(rate);}}
void RegisteredWingRuntime::loopRate(float fast,float spread){if(state_){std::lock_guard lock(state_->mutex);state_->clocks.loopRate(fast,spread);state_->fastRate=fast;}}
void RegisteredWingRuntime::pitch(float degrees){if(state_){std::lock_guard lock(state_->mutex);state_->pitch=degrees;}}
void RegisteredWingRuntime::release(bool smooth){if(!state_)return;{std::lock_guard lock(state_->mutex);state_->released=true;state_->targetOwnership=0;if(!smooth){state_->ownership=0;state_->restore();}}if(!smooth)state_.reset();}
void applyRegisteredWingPoses(Object* object,RE::NiUpdateData* data,bool post){
    const auto map=scenes.load();if(!map||!object)return;
    auto ancestor=object;for(unsigned depth=0;ancestor&&depth<64;++depth,ancestor=ancestor->parent){
        const auto found=map->find(ancestor);if(found==map->end())continue;
        for(const auto& weak:found->second)if(const auto s=weak.lock())s->apply(ancestor,data,post);return;
    }
}
}
