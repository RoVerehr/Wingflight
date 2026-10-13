#include "wing_runtime.hpp"
#include "wing_runtime_policy.hpp"
#include "wings.hpp"
#include "runtime_support.hpp"
#include "diagnostics.hpp"
#include "jump_wing_preview.hpp"
#include <RE/H/hkbContext.h>
#include <RE/H/hkbBehaviorGraphData.h>
#include <RE/N/NiKeyBasedInterpolator.h>
#include <SKSE/SKSE.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace wingflight {
namespace {
using visual::Sample;
struct Track {
    RE::NiPointer<RE::NiAVObject> node;
    std::array<RE::NiPointer<RE::NiInterpolator>,3> samplers;
    legs::Quaternion flutterCenter;
    Sample reference;
    RE::NiTransform before,output;
    bool wrote{};
};
struct State {
    const void* owner{};
    std::mutex mutex;
    RE::NiPointer<RE::NiAVObject> root;
    RE::NiPointer<RE::NiControllerManager> manager;
    RE::NiPointer<RE::NiMultiTargetTransformController> controller;
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;
    std::vector<const RE::hkbCharacter*> characters;
    std::vector<Track> tracks;
    std::array<visual::Clock,3> clocks;
    std::array<float,3> begin{},originalRates{};
    WaveWeights desired{},displayed{};
    WaveWeights previewSeed{};
    bool previewing{},previewHandoff{};
    float previewFlutter{1},previewFlutterSeed{1};
    std::array<float,3> displayedPhase{},displayedRate{};
    float delta{},fastEntry{},pitch{},amplitude{1},unobserved{};
    std::uint64_t requested{},applied{};
    bool releasing{},cancelled{};
    float releaseSeconds{},releaseWeight{};
    unsigned samples{},audioSamples{};
};
std::mutex registryMutex;
std::unordered_map<RE::NiMultiTargetTransformController*,std::shared_ptr<State>> controllers;
std::unordered_map<const RE::hkbCharacter*,std::weak_ptr<State>> characters;
std::atomic_bool hooksReady{};

Sample readTransform(const RE::NiTransform& t){
    legs::Matrix matrix;for(int r=0;r<3;++r)for(int c=0;c<3;++c)matrix[r][c]=t.rotate.entry[r][c];
    const auto q=legs::quaternion(matrix);return {t.translate.x,t.translate.y,t.translate.z,q.w,q.x,q.y,q.z,t.scale};
}
void writeTransform(RE::NiTransform& t,const Sample& s){
    const auto m=legs::matrix({s.qx,s.qy,s.qz,s.w});
    for(int r=0;r<3;++r)for(int c=0;c<3;++c)t.rotate.entry[r][c]=m[r][c];
    t.translate={s.x,s.y,s.z};t.scale=s.scale;
}
bool inside(RE::NiAVObject* node,RE::NiAVObject* root){
    for(unsigned depth=0;node&&depth<64;++depth,node=node->parent)if(node==root)return true;
    return false;
}
RE::NiControllerManager* managerOf(RE::NiAVObject* root,unsigned depth=0){
    if(!root||depth>32)return nullptr;
    if(auto manager=root->GetController<RE::NiControllerManager>())return manager;
    if(auto node=root->AsNode())for(auto& child:node->children)if(auto m=managerOf(child.get(),depth+1))return m;
    return nullptr;
}
bool centerOf(RE::NiInterpolator* interp,legs::Quaternion& center){
    auto key=static_cast<RE::NiKeyBasedInterpolator*>(interp);
    // The actual virtual order is type at3B and content at3C, as in the existing
    // sampler audit; ignore the swapped comments in this dependency snapshot.
    if(key->GetKeyContent(1)!=RE::NiAnimationKey::KeyContent::kRot)return false;
    const auto type=key->GetKeyType(1);const auto count=key->GetKeyCount(1);const auto stride=key->GetKeyStride(1);
    if((type!=RE::NiAnimationKey::KeyType::kLink&&type!=RE::NiAnimationKey::KeyType::kBez)
        ||count<2||count>4096||stride<20||stride>128){
        // Zero/one key is an ordinary static bone, not an animation error.
        if(count>=2)diagnostic::report("flutter.key-layout","use start-pose center",interp,
            fmt::format("type={} count={} stride={} expectedCount=2..4096 expectedStride=20..128",static_cast<unsigned>(type),count,stride));return false;}
    const auto bytes=static_cast<const std::byte*>(key->GetKeyArray(1));if(!bytes){
        diagnostic::report("flutter.key-array-missing","use start-pose center",interp);return false;}
    std::vector<legs::Quaternion> keys;keys.reserve(count);
    for(unsigned i=0;i<count;++i){RE::NiQuaternion q{};std::memcpy(&q,bytes+i*stride+4,sizeof(q));
        const legs::Quaternion value{q.x,q.y,q.z,q.w};if(!flutter::valid(value)){
            diagnostic::report("flutter.key-rotation-invalid","use start-pose center",interp,{},i,count);return false;}keys.push_back(value);}
    center=flutter::center(keys);return true;
}
void unregisterState(const std::shared_ptr<State>& s){
    std::lock_guard guard(registryMutex);
    if(auto it=controllers.find(s->controller.get());it!=controllers.end()&&it->second==s)controllers.erase(it);
    for(auto c:s->characters){auto it=characters.find(c);if(it!=characters.end()&&it->second.lock()==s)characters.erase(it);}
}
// Build an up-to-date parent basis from local rotations. The old pivots used
// previous world matrices; this avoids a frame of axis lag after body turning.
RE::NiMatrix3 parentRotation(RE::NiAVObject* node,RE::NiAVObject* root){
    std::array<RE::NiAVObject*,64> chain{};unsigned count=0;
    for(auto p=node->parent;p&&p!=root&&count<chain.size();p=p->parent)chain[count++]=p;
    auto out=root->world.rotate;
    while(count)out=out*chain[--count]->local.rotate;
    return out;
}
void angleOverlay(State& s){
    const float total=std::clamp(s.pitch,0.0f,90.0f)*0.01745329252f*s.displayed.flap;
    if(total==0)return;
    const auto lateral=s.root->world.rotate*RE::NiPoint3(1,0,0);
    for(const auto name:{"NPC LUpArm1","NPC LUpArm2","NPC RUpArm1","NPC RUpArm2"}){
        auto found=std::find_if(s.tracks.begin(),s.tracks.end(),[&](const Track& t){return t.node->name==name;});
        if(found==s.tracks.end())continue;auto node=found->node.get();
        auto axis=parentRotation(node,s.root.get()).Transpose()*lateral;
        const float len=std::sqrt(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z);if(!std::isfinite(len)||len<0.5f){
            diagnostic::report("wing-pitch.axis-invalid","skip bone angle overlay",node,name,len,0.5);continue;}
        axis=axis*(1/len);const float a=total*(std::string_view(name).ends_with("1")?0.25f:0.75f);
        auto value=readTransform(node->local);const auto q=legs::multiply({axis.x*std::sin(a/2),axis.y*std::sin(a/2),axis.z*std::sin(a/2),std::cos(a/2)},
            {value.qx,value.qy,value.qz,value.w});value.w=q.w;value.qx=q.x;value.qy=q.y;value.qz=q.z;
        writeTransform(node->local,value);found->output=node->local;
    }
}
struct ControllerUpdate {
    // Verified1.6.1170 D98C60 consumes RDX->time, NOT XMM1 float.
    static void thunk(RE::NiMultiTargetTransformController* self,RE::NiUpdateData* data){
        original(self,data);
        std::shared_ptr<State> s;{
            std::lock_guard guard(registryMutex);auto it=controllers.find(self);if(it==controllers.end())return;s=it->second;
        }
        bool done=false;{
            std::lock_guard lock(s->mutex);if(s->cancelled||!data)return;
            s->unobserved=0;
            if(s->releasing){
                if(s->requested!=s->applied){s->releaseSeconds+=s->delta;s->applied=s->requested;}
                const float f=std::clamp(s->releaseSeconds/0.20f,0.0f,1.0f);
                const float remaining=1-f*f*(3-2*f);s->displayed=s->desired;
                s->displayed.flap*=remaining;s->displayed.fast*=remaining;s->displayed.spread*=remaining;
                s->displayed.original=1-s->displayed.flap-s->displayed.fast-s->displayed.spread;
                done=f>=1;
            }else if(s->requested!=s->applied){
                for(auto& c:s->clocks)c.advance(s->delta);
                s->displayed=s->desired;s->applied=s->requested;
                for(unsigned i=0;i<3;++i){s->displayedPhase[i]=s->clocks[i].phase;s->displayedRate[i]=s->clocks[i].rate;}
            }
            if(!done&&s->displayed.original<0.999999f){
                const std::array<float,4> weights{s->displayed.original,s->displayed.flap,s->displayed.fast,s->displayed.spread};
                for(auto& t:s->tracks){
                    // Selective native updates can skip a channel. Never blend
                    // entry/exit against our own previous output in that case.
                    if(t.wrote&&t.node->local==t.output)t.node->local=t.before;
                    t.before=t.node->local;const auto base=readTransform(t.before);
                    std::array<Sample,4> poses{base,t.reference,t.reference,t.reference};
                    for(unsigned i=0;i<3;++i){
                        if(weights[i+1]==0)continue;Sample value{};
                        const float time=s->begin[i]+s->clocks[i].phase*s->clocks[i].duration;
                        auto& output=*reinterpret_cast<RE::NiQuatTransform*>(&value);
                        if(t.samplers[i]->Update1(time,t.node.get(),output))poses[i+1]=visual::validSample(value,t.reference,t.node.get());
                        else diagnostic::report("sample.update-failed","use reference pose",t.samplers[i].get(),t.node->name.c_str(),time);
                    }
                    const auto q=flutter::scale(t.flutterCenter,{poses[3].qx,poses[3].qy,poses[3].qz,poses[3].w},previewFlutterAmplitude(s->previewing||s->previewHandoff,s->previewFlutter,s->amplitude));
                    poses[3].w=q.w;poses[3].qx=q.x;poses[3].qy=q.y;poses[3].qz=q.z;
                    const auto result=visual::combine(poses,weights);writeTransform(t.node->local,result);
                    t.output=t.node->local;t.wrote=true;
                }
                angleOverlay(*s);
                if(s->samples++<4)SKSE::log::info("Runtime wing pose applied: tracks={} phase={:.4f} weights={:.3f}/{:.3f}/{:.3f} spreadPhase={:.4f} spreadRate={:.3f} spreadAmplitude={:.3f} jumpPreview={} (original assets unchanged)",
                    s->tracks.size(),s->clocks[0].phase,s->displayed.flap,s->displayed.fast,s->displayed.spread,s->clocks[2].phase,s->clocks[2].rate,previewFlutterAmplitude(s->previewing||s->previewHandoff,s->previewFlutter,s->amplitude),s->previewing);
            }else{
                for(auto& t:s->tracks){if(t.wrote&&t.node->local==t.output)t.node->local=t.before;t.wrote=false;}
            }
        }
        if(done)unregisterState(s);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct EventKey {float time;std::uint32_t pad;std::int32_t event;std::uint32_t pad2;void* payload;};
static_assert(sizeof(EventKey)==0x18);
static_assert(offsetof(RE::hkbNode,userData)==0x30);
struct GeneratorUpdate {
    static void thunk(void* generator,const RE::hkbContext& context,float delta){
        std::shared_ptr<State> s;{
            std::lock_guard guard(registryMutex);auto it=characters.find(context.character);
            if(it!=characters.end())s=it->second.lock();
        }
        bool owned=false;if(s){std::lock_guard lock(s->mutex);owned=!s->cancelled&&(s->displayed.original<0.999999f||s->desired.original<0.999999f);}
        if(!owned||!context.behavior||!context.behavior->data||!context.behavior->data->stringData){
            if(owned)diagnostic::report("wing-audio.behavior-data-missing","use native generator update",generator);
            original(generator,context,delta);return;}
        if(!runtimeReadable(generator,0x70)){
            diagnostic::report("wing-audio.generator-unreadable","use native generator update",generator,{},0x70);
            original(generator,context,delta);return;}
        auto raw=static_cast<std::byte*>(generator);
        // Art generators carry their sequence in the public hkbNode userData.
        // Native Update does not access that field. Validate its typed ABI and
        // actual sequence ownership rather than demanding a nonexistent load.
        auto sequence=reinterpret_cast<RE::NiControllerSequence*>(static_cast<RE::hkbNode*>(generator)->userData);
        if(!runtimeReadable(sequence,sizeof(RE::NiControllerSequence))||sequence->owner!=s->manager.get()||raw[0x6C]!=std::byte{}){
            diagnostic::report(!runtimeReadable(sequence,sizeof(RE::NiControllerSequence))?"wing-audio.sequence-unreadable"
                :sequence->owner!=s->manager.get()?"wing-audio.sequence-owner-mismatch":"wing-audio.generator-not-initialized",
                "use native generator update",generator);original(generator,context,delta);return;}
        EventKey* keys{};std::int32_t count{};std::memcpy(&keys,raw+0x58,sizeof(keys));std::memcpy(&count,raw+0x60,sizeof(count));
        if(count<1||count>128||!runtimeReadable(keys,sizeof(EventKey)*static_cast<std::size_t>(count))){
            // Zero events is a normal sequence, not a failed sound filter.
            if(count!=0)diagnostic::report(count<1||count>128?"wing-audio.event-count-invalid":"wing-audio.events-unreadable",
                "use native generator update",generator,{},count,128);
            original(generator,context,delta);return;}
        const auto& names=context.behavior->data->stringData->eventNames;
        std::array<EventKey,128> retained{};std::int32_t kept=0;
        for(std::int32_t i=0;i<count;++i){
            const auto id=keys[i].event;const char* name=id>=0&&id<names.size()?names[id].c_str():nullptr;
            if(!name||std::string_view(name)!="SoundPlay")retained[kept++]=keys[i];
        }
        if(kept==count){original(generator,context,delta);return;}
        // Borrow the payload references only for this synchronous Update. Never
        // free or mutate native/NIF text keys, and retain end/lifecycle events.
        struct BorrowedKeys {
            std::byte* generator;EventKey* original;std::int32_t count;
            ~BorrowedKeys(){std::memcpy(generator+0x58,&original,sizeof(original));std::memcpy(generator+0x60,&count,sizeof(count));}
        } restore{raw,keys,count};
        auto temporary=retained.data();std::memcpy(raw+0x58,&temporary,sizeof(temporary));std::memcpy(raw+0x60,&kept,sizeof(kept));
        original(generator,context,delta);
        {std::lock_guard lock(s->mutex);if(s->audioSamples++<3)SKSE::log::info("Runtime wing audio isolated: removed={} retained={} (player art only)",count-kept,kept);}
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
struct WingRuntime::Impl {
    std::vector<std::shared_ptr<State>> states;
    bool installed{};
    float amplitude{1};
};
WingRuntime::WingRuntime():impl_(std::make_unique<Impl>()){}
WingRuntime::~WingRuntime(){release(false);}
bool WingRuntime::install(float amplitude){
    impl_->amplitude=amplitude;
    REL::Relocation<std::uintptr_t> multi{RE::VTABLE_NiMultiTargetTransformController[0]};
    REL::Relocation<std::uintptr_t> gen{RE::VTABLE_BGSGamebryoSequenceGenerator[0]};
    const auto m=*reinterpret_cast<std::uintptr_t*>(multi.address()+0x27*8);
    const auto g=*reinterpret_cast<std::uintptr_t*>(gen.address()+5*8);
    const auto clone=REL::RelocationID(68839,70191).address();
    const auto sampler=*reinterpret_cast<std::uintptr_t*>(REL::Relocation<std::uintptr_t>(RE::VTABLE_NiTransformInterpolator[0]).address()+0x25*8);
    if(!verifyWingEntrypoints(m,g,clone,sampler)){
        SKSE::log::error("Runtime {} wing driver disabled: required engine functions are not callable",runtimeName());return false;
    }
    ControllerUpdate::original=multi.write_vfunc(0x27,ControllerUpdate::thunk);
    GeneratorUpdate::original=gen.write_vfunc(5,GeneratorUpdate::thunk);
    impl_->installed=true;hooksReady=true;
    SKSE::log::info("Runtime wing driver installed: private sampling; no patched wing NIF/HKX required");return true;
}
void WingRuntime::advance(float dt){
    std::vector<std::shared_ptr<State>> states;{
        std::lock_guard guard(registryMutex);for(const auto& [key,s]:controllers)if(s->owner==impl_.get())states.push_back(s);
    }
    for(auto& s:states){bool expired=false;{
        std::lock_guard lock(s->mutex);s->delta=std::isfinite(dt)?std::clamp(dt,0.0f,0.1f):0;++s->requested;
        s->unobserved+=s->delta;expired=s->releasing&&s->unobserved>2;
        if(expired)diagnostic::report("wing.release-update-timeout","unregister released visual state",s->root.get(),{},s->unobserved,2);
        if(expired)s->cancelled=true;
    }if(expired)unregisterState(s);}
}
WingPlayback WingRuntime::prepare(WingMatch match){
    WingPlayback out;if(!impl_->installed||!match.effect||!match.effect->hitEffects){
        if(match.effect&&match.profile)diagnostic::report(!impl_->installed?"binding.driver-not-installed":"binding.hit-effects-unavailable",
            "return unavailable visual binding",match.effect,match.profile->id);
        return out;}
    unsigned modelCount{},attachedCount{},rootCount{};
    for(auto effect:*match.effect->hitEffects){
        if(!effect||effect->finished||effect->GetType()!=RE::TEMP_EFFECT_TYPE::kRefModel)continue;
        ++modelCount;
        auto model=static_cast<RE::ModelReferenceEffect*>(effect);if(!model->GetAttached())continue;
        ++attachedCount;
        auto root=model->Get3D();if(!root){
            diagnostic::report("binding.model-root-unavailable","skip wing model",model,match.profile->id);continue;}
        ++rootCount;
        auto found=std::find_if(impl_->states.begin(),impl_->states.end(),[&](const auto& s){return s->root.get()==root;});
        std::shared_ptr<State> s;
        if(found!=impl_->states.end())s=*found;
        else{
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;if(!model->GetAnimationGraphManager(graph)||!graph){
                diagnostic::report("binding.animation-graph-missing","skip wing model",root,match.profile->id);continue;}
            RE::BSSpinLockGuard graphLock(graph->GetRuntimeData().updateLock);
            auto manager=managerOf(root);if(!manager){
                diagnostic::report("binding.controller-manager-missing","skip wing model",root,match.profile->id);continue;}
            // The experimental build must be tested against provider originals,
            // not an accidentally winning older Wingflight resource override.
            if(manager->GetSequenceByName("Wingflight_PowerLoop")||root->GetObjectByName("Wingflight_Pitch_L_Inner")){
                SKSE::log::error("Runtime wing driver rejected old Wingflight model patch; disable the older Wingflight mod for0.3.0 testing");continue;
            }
            auto controller=manager->target?manager->target->GetController<RE::NiMultiTargetTransformController>():nullptr;
            if(!controller||controller->numInterps==0||controller->numInterps>128){
                diagnostic::report(!controller?"binding.controller-missing":"binding.target-count-invalid",
                    "skip wing model",root,match.profile->id,controller?controller->numInterps:0,128);continue;}
            std::array<RE::NiControllerSequence*,3> seq{manager->GetSequenceByName(match.profile->flapSequence),
                manager->GetSequenceByName(match.profile->glideSequence),manager->GetSequenceByName(match.profile->spreadSequence)};
            if(!seq[0]||!seq[1]||!seq[2]){
                diagnostic::report("binding.sequence-missing","skip wing model",root,
                    fmt::format("adapter={} power='{}' present={} fast='{}' present={} spread='{}' present={}",match.profile->id,
                        match.profile->flapSequence,bool(seq[0]),match.profile->glideSequence,bool(seq[1]),match.profile->spreadSequence,bool(seq[2])));continue;}
            s=std::make_shared<State>();s->owner=impl_.get();s->root=RE::NiPointer<RE::NiAVObject>(root);s->graph=graph;
            s->manager=RE::NiPointer<RE::NiControllerManager>(manager);s->controller=RE::NiPointer<RE::NiMultiTargetTransformController>(controller);
            s->amplitude=impl_->amplitude;s->fastEntry=match.profile->timing.glideEntryPhase;
            s->desired={0,0,0,1};s->displayed=s->desired;
            bool good=true;const auto table=REL::Relocation<std::uintptr_t>(RE::VTABLE_NiTransformInterpolator[0]).address();
            for(unsigned i=0;i<3;++i){
                const auto q=seq[i];if(q->arraySize!=seq[0]->arraySize||q->arraySize<1||q->arraySize>128
                    ||!q->idTagArray.data()||!q->interpArray.data()){
                    diagnostic::report("binding.track-layout-invalid","reject wing binding",root,
                        fmt::format("adapter={} sequence={} tagsPresent={} samplersPresent={}",match.profile->id,i,bool(q->idTagArray.data()),bool(q->interpArray.data())),
                        q->arraySize,seq[0]->arraySize);good=false;break;}
                s->begin[i]=q->beginKeyTime;s->originalRates[i]=q->frequency;
                s->clocks[i].duration=q->endKeyTime-q->beginKeyTime;s->clocks[i].rate=i==0?0.01f:q->frequency;
                if(!std::isfinite(s->clocks[i].duration)||s->clocks[i].duration<=0||s->clocks[i].duration>30){
                    diagnostic::report("binding.duration-invalid","reject wing binding",root,
                        fmt::format("adapter={} sequence={} begin={} end={}",match.profile->id,i,q->beginKeyTime,q->endKeyTime),s->clocks[i].duration,30);good=false;break;}
            }
            if(!good)continue;s->clocks[1].phase=s->fastEntry;
            std::unordered_set<RE::NiInterpolator*> originals,clones;
            for(auto q:seq)for(unsigned i=0;i<q->arraySize;++i)originals.insert(q->interpArray[i].interpolator.get());
            for(unsigned j=0;j<seq[0]->arraySize&&good;++j){
                Track t;const auto& name=seq[0]->idTagArray[j].avObjectName;auto node=root->GetObjectByName(name);
                if(!node||!inside(node,root)){
                    diagnostic::report(!node?"binding.bone-missing":"binding.bone-outside-root","reject wing binding",root,name.c_str(),j);good=false;break;}
                bool controlled=false;for(unsigned k=0;k<controller->numInterps;++k)if(controller->targets[k]==node)controlled=true;
                if(!controlled){diagnostic::report("binding.bone-not-controlled","reject wing binding",root,name.c_str(),j);good=false;break;}
                t.node=RE::NiPointer<RE::NiAVObject>(node);t.reference=readTransform(node->local);
                for(unsigned i=0;i<3;++i){
                    auto q=seq[i];if(q->idTagArray[j].avObjectName!=name){
                        diagnostic::report("binding.track-name-mismatch","reject wing binding",root,
                            fmt::format("sequence={} track={} expected='{}' actual='{}'",i,j,name.c_str(),q->idTagArray[j].avObjectName.c_str()));good=false;break;}
                    auto interp=q->interpArray[j].interpolator.get();if(!interp||*reinterpret_cast<std::uintptr_t*>(interp)!=table){
                        diagnostic::report(!interp?"binding.sampler-missing":"binding.sampler-type-mismatch","reject wing binding",root,
                            fmt::format("bone={} sequence={} input={:X} actualVtable={:X} expectedVtable={:X}",name.c_str(),i,
                                reinterpret_cast<std::uintptr_t>(interp),interp?*reinterpret_cast<std::uintptr_t*>(interp):0,table));good=false;break;}
                    RE::NiPointer<RE::NiObject> copy;interp->CreateDeepCopy(copy);
                    auto privateInterp=copy?static_cast<RE::NiInterpolator*>(copy.get()):nullptr;
                    const auto resultTable=privateInterp?*reinterpret_cast<std::uintptr_t*>(privateInterp):0;
                    const char* copyFailure=!privateInterp?"null-result":resultTable!=table?"wrong-sampler-type"
                        :originals.contains(privateInterp)?"returned-original-sampler":!clones.insert(privateInterp).second?"reused-private-sampler":nullptr;
                    if(copyFailure){
                        SKSE::log::error("Runtime wing copy failed: reason={} bone={} sequence={} input={:X} inputRefCount={} output={:X} outputVtable={:X} expectedVtable={:X}",
                            copyFailure,name.c_str(),i,reinterpret_cast<std::uintptr_t>(interp),interp->GetRefCount(),reinterpret_cast<std::uintptr_t>(privateInterp),resultTable,table);
                        good=false;break;
                    }
                    t.samplers[i]=RE::NiPointer<RE::NiInterpolator>(privateInterp);
                }
                if(good&&!centerOf(t.samplers[2].get(),t.flutterCenter)){
                    Sample sample{};auto& value=*reinterpret_cast<RE::NiQuatTransform*>(&sample);
                    if(t.samplers[2]->Update1(s->begin[2],node,value))sample=visual::validSample(sample,t.reference,node);else{
                        diagnostic::report("flutter.center-sample-failed","use current bone pose as center",node,name.c_str());sample=t.reference;}
                    t.flutterCenter={sample.qx,sample.qy,sample.qz,sample.w};
                }
                if(good)s->tracks.push_back(std::move(t));
            }
            if(!good){SKSE::log::error("Runtime wing binding rejected: targets/sampler ownership validation failed");continue;}
            for(const auto& g:graph->graphs)if(g)s->characters.push_back(&g->characterInstance);
            if(s->characters.empty()){
                diagnostic::report("binding.character-missing","skip wing model",root,match.profile->id);continue;}
            {std::lock_guard guard(registryMutex);controllers[controller]=s;for(auto c:s->characters)characters[c]=s;}
            impl_->states.push_back(s);
            SKSE::log::info("Runtime wing bound: root={} privateSamplers={} tracks={} durations={:.4f}/{:.4f}/{:.4f}; NIF keys/manager/graph unchanged",
                root->name.c_str(),clones.size(),s->tracks.size(),s->clocks[0].duration,s->clocks[1].duration,s->clocks[2].duration);
        }
        std::lock_guard lock(s->mutex);out.found=true;out.active=out.gliding=out.spreadActive=s->applied>0;
        out.duration=s->clocks[0].duration;out.phase=s->displayedPhase[0];out.frequency=s->displayedRate[0];
        out.flapWeight=s->displayed.flap;out.glideWeight=s->displayed.fast;out.spreadWeight=s->displayed.spread;
        out.spreadPhase=s->displayedPhase[2];out.spreadFrequency=s->displayedRate[2];
        out.graphWeight=s->desired.flap;out.graphGlideWeight=s->desired.fast;out.graphSpreadWeight=s->desired.spread;
        out.glidePhase=s->displayedPhase[1];out.glideFrequency=s->displayedRate[1];out.otherWeight=s->displayed.original;
        out.activeSequence="runtime: power,fast,spread";
    }
    if(!out.found&&rootCount==0)diagnostic::report("binding.no-attached-model","return unavailable visual binding",match.effect,
        fmt::format("adapter={} models={} attached={} roots={}",match.profile->id,modelCount,attachedCount,rootCount));
    return out;
}
bool WingRuntime::blend(float flap,float ownership,float fast){
    if(!std::isfinite(flap)||!std::isfinite(ownership)||!std::isfinite(fast)){
        diagnostic::report("wing.blend-nonfinite","ignore blend command",impl_.get(),fmt::format("flap={} ownership={} fast={}",flap,ownership,fast));return false;}
    const auto weights=waveContributions(std::clamp(flap,0.0f,1.0f),std::clamp(fast,0.0f,1.0f),std::clamp(ownership,0.0f,1.0f));
    for(auto& s:impl_->states){std::lock_guard lock(s->mutex);
        if(s->previewing){s->previewSeed=s->displayed;s->previewFlutterSeed=s->previewFlutter;s->previewHandoff=true;s->previewing=false;}
        s->desired=s->previewHandoff?interpolateWeights(s->previewSeed,waveContributions(std::clamp(flap,0.f,1.f),std::clamp(fast,0.f,1.f),1),ownership):weights;
        s->previewFlutter=s->previewHandoff?previewFlutterHandoff(ownership,s->previewFlutterSeed,s->amplitude):s->amplitude;
        if(ownership>=1)s->previewHandoff=false;
    }
    return !impl_->states.empty();
}
bool WingRuntime::preview(float amount,float period,float amplitude){
    if(!std::isfinite(amount)||!std::isfinite(period)||!std::isfinite(amplitude)||period<=0)return false;
    for(auto& s:impl_->states){std::lock_guard lock(s->mutex);
        const float desiredAmplitude=std::clamp(amplitude,0.f,2.f);
        s->previewFlutter=s->previewing?desiredAmplitude+(s->previewFlutter-desiredAmplitude)*std::exp(-12*s->delta):desiredAmplitude;
        s->previewing=true;s->previewHandoff=false;s->pitch=0;
        const float weight=std::clamp(amount,0.f,1.f);
        s->desired={0,0,weight,1-weight};
        // Keep the authored loop moving gently; preserve its current phase so
        // takeoff/glide continues from the exact displayed wing pose.
        s->clocks[2].rate=s->clocks[2].duration/period;
    }
    return !impl_->states.empty();
}
bool WingRuntime::parkPower(){bool ok=!impl_->states.empty();for(auto& s:impl_->states){std::lock_guard lock(s->mutex);
    const bool parked=s->displayed.flap==0&&s->desired.flap==0&&s->clocks[0].resetHidden(s->displayed.flap);
    if(parked)s->displayedPhase[0]=0;ok=parked&&ok;}return ok;}
bool WingRuntime::parkFast(){bool ok=!impl_->states.empty();for(auto& s:impl_->states){std::lock_guard lock(s->mutex);
    const bool parked=s->displayed.fast==0&&s->desired.fast==0&&s->clocks[1].resetHidden(s->displayed.fast,s->fastEntry);
    if(parked)s->displayedPhase[1]=s->fastEntry;ok=parked&&ok;}return ok;}
void WingRuntime::powerRate(float rate){if(!std::isfinite(rate)||rate<0||rate>40){
    diagnostic::report("wing.playback-rate-invalid","ignore rate command",impl_.get(),{},rate,40);return;}
    for(auto& s:impl_->states){std::lock_guard lock(s->mutex);s->clocks[0].rate=rate;}}
void WingRuntime::loopRate(float fast,float spread){for(auto& s:impl_->states){std::lock_guard lock(s->mutex);
    s->clocks[1].rate=s->originalRates[1]*glidePlaybackScale(s->displayed.fast,fast);s->clocks[2].rate=s->originalRates[2]*spread;}}
void WingRuntime::pitch(float degrees){for(auto& s:impl_->states){std::lock_guard lock(s->mutex);s->pitch=degrees;}}
void WingRuntime::release(bool smooth){
    auto states=impl_->states;
    if(!smooth){std::lock_guard guard(registryMutex);for(const auto& [key,s]:controllers)
        if(s->owner==impl_.get()&&std::find(states.begin(),states.end(),s)==states.end())states.push_back(s);}
    for(auto& s:states){bool fade=false;{
        std::lock_guard lock(s->mutex);fade=smooth&&s->applied>0&&s->displayed.original<0.999999f;
        if(fade){s->releasing=true;s->desired=s->displayed;s->releaseSeconds=0;}else s->cancelled=true;
    }if(!fade)unregisterState(s);}
    impl_->states.clear();
}
}
