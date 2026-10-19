#include "registered_wings.hpp"
#include <SKSE/SKSE.h>
#include <algorithm>
#include <mutex>
#pragma warning(push)
#pragma warning(disable:4996 4099 5054)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace wingflight {
namespace {
std::mutex channelMenuMutex;
std::vector<WingCandidate> candidates;
std::vector<WingProfile> saved;
WingCandidate selected;
bool selectedValid{},scannerSelection{},busy{};
std::string message="Equip or summon your wings, then scan. Saved rules also apply to NPCs.";
void scan(){
    busy=true;message="Scanning worn equipment and active effect models...";
    SKSE::GetTaskInterface()->AddTask([]{
        auto found=scanWingCandidates(RE::PlayerCharacter::GetSingleton());auto rules=wingRegistrationProfiles();
        std::lock_guard lock(channelMenuMutex);candidates=std::move(found);saved=std::move(rules);busy=false;
        std::erase_if(candidates,[](const auto& c){return !c.compatible;});
        if(scannerSelection){selectedValid=false;scannerSelection=false;}
        message=candidates.empty()?"No compatible wing models found. Invisible wings require a manually written INI.":"Select a model, choose a template, then Apply and Save Registration.";
    });
}
bool slider(const char* name,float& value,float min,float max){return ImGuiMCP::SliderFloat(name,&value,min,max,"%.3f",0);}
}
void renderWingRegistrationMenu(){
    std::lock_guard lock(channelMenuMutex);
    ImGuiMCP::TextWrapped("%s",message.c_str());
    ImGuiMCP::TextWrapped("Registration INIs persist when wings are unequipped. Scanning does not change models or grant flight.");
    ImGuiMCP::BeginDisabled(busy);
    if(ImGuiMCP::Button("Find Current Wings"))scan();
    ImGuiMCP::SameLine();
    if(ImGuiMCP::Button("Reload Registration INIs")){
        busy=true;
        SKSE::GetTaskInterface()->AddTask([]{loadWingRegistrations();auto rules=wingRegistrationProfiles();std::lock_guard lock(channelMenuMutex);saved=std::move(rules);busy=false;message="Registration INIs reloaded. Existing bindings are revalidated.";});
    }
    ImGuiMCP::Separator();ImGuiMCP::TextUnformatted("Compatible wing models");
    unsigned hidden{};
    for(unsigned i=0;i<candidates.size();++i){auto& c=candidates[i];
        // Models that cannot be registered (clothing physics, static meshes) are not offered.
        if(!c.compatible){++hidden;continue;}
        ImGuiMCP::PushID(static_cast<int>(i));
        const auto label=c.name+" ("+c.profile.plugin+")";
        if(ImGuiMCP::Button(label.c_str())){selected=c;selectedValid=true;scannerSelection=true;}
        ImGuiMCP::TextWrapped("%u bones, %u pairs | %s",c.bones,c.pairs,c.reason.c_str());ImGuiMCP::PopID();
    }
    if(hidden)ImGuiMCP::TextWrapped("%u other worn or attached models are not registrable wings (clothing physics, static or unskinned meshes) and are not listed.",hidden);
    ImGuiMCP::Separator();ImGuiMCP::TextUnformatted("Saved registrations");
    if(saved.empty())saved=wingRegistrationProfiles();
    for(unsigned i=0;i<saved.size();++i){ImGuiMCP::PushID(static_cast<int>(i)+1000);
        const auto label=saved[i].id+" ["+channel::templateName(saved[i].wingTemplate)+"]";
        if(ImGuiMCP::Button(label.c_str())){selected={};selected.profile=saved[i];selected.name=saved[i].id;selected.compatible=true;selectedValid=true;scannerSelection=false;}
        ImGuiMCP::PopID();
    }
    if(selectedValid){
        ImGuiMCP::Separator();ImGuiMCP::TextWrapped("Selected: %s",selected.name.c_str());
        auto& p=selected.profile;
        if(p.wingTemplate==channel::Template::invisible)ImGuiMCP::TextWrapped("Invisible: INI-only registration. Ordinary wingbeat flight and sound, with no animation or automatic model binding.");
        else{
            ImGuiMCP::BeginDisabled(!selected.compatible);
            ImGuiMCP::TextUnformatted("Animation template");
            for(auto kind:{channel::Template::authored,channel::Template::awu,channel::Template::fairy,channel::Template::vampireLord}){
                if(ImGuiMCP::Button(channel::templateName(kind))){p.wingTemplate=kind;p.wingPose=channel::defaults(kind);p.continuousFlight=kind==channel::Template::fairy;}
                ImGuiMCP::SameLine();
            }
            ImGuiMCP::NewLine();ImGuiMCP::Text("Current: %s",channel::templateName(p.wingTemplate));
            ImGuiMCP::TextWrapped("Angles are relative to the model pose captured at binding. Apply and Save Registration updates settings in game.");
            auto& q=p.wingPose;
            slider("Whole wing forward/back tilt (degrees)",q.forwardTiltDegrees,-180,180);
            ImGuiMCP::TextWrapped("Positive tilts the whole wings forward; negative tilts them backward. Both sides tilt together at their roots, independently of wingbeat amplitude.");
            if(p.wingTemplate==channel::Template::authored){
                ImGuiMCP::Text("Power: %s | Fast: %s | Spread: %s",p.flapSequence.c_str(),p.glideSequence.empty()?"(power)":p.glideSequence.c_str(),p.spreadSequence.empty()?"(fast)":p.spreadSequence.c_str());
                ImGuiMCP::TextWrapped(selected.authored||!scannerSelection?"Authored plays the model's own wingbeat sequences: the large hovering wingbeat, the small fast-flight wingbeat and the gliding flutter. Change the sequence names in the INI ([Model] PowerSequence, FastSequence, SpreadSequence). If the model has none, the procedural AWU-style wingbeat is used.":
                    "No matching sequences were found in this model; Authored would use the procedural AWU-style wingbeat.");
                const auto pairCount=std::min<std::size_t>(std::max<std::size_t>({p.leftRoots.size(),static_cast<std::size_t>(selected.pairs),1}),channel::PoseSettings::maxPairs);
                if(pairCount>1){
                    ImGuiMCP::Separator();ImGuiMCP::TextUnformatted("Wing pairs");
                    for(unsigned i=0;i<pairCount;++i){ImGuiMCP::PushID(static_cast<int>(i)+2000);
                        if(i<p.leftRoots.size()&&i<p.rightRoots.size())ImGuiMCP::Text("Pair %u: %s / %s",i+1,p.leftRoots[i].c_str(),p.rightRoots[i].c_str());
                        else ImGuiMCP::Text("Pair %u",i+1);
                        slider("Delay (seconds)",q.pairDelaySeconds[i],0,.5f);ImGuiMCP::PopID();
                    }
                    ImGuiMCP::TextWrapped("A small delay lets the animation travel from one pair to the next.");
                }
                ImGuiMCP::Separator();slider("Pose blend (seconds)",q.blendSeconds,.02f,3);
            }else{
            slider("Raised wing angle (degrees)",q.upDegrees,-180,180);slider("Lowered wing angle (degrees)",q.downDegrees,-180,180);
            slider("Gliding wing angle (degrees)",q.spreadDegrees,-180,180);slider("Fast wingbeat amplitude",q.fastAmplitude,0,2);
            ImGuiMCP::TextWrapped("Spread angle and flutter apply while gliding or descending with wings spread, not during ordinary wingbeats. Fairy uses its continuous cycle instead.");
            slider("Flutter angle (degrees)",q.flutterDegrees,0,60);slider("Flutter period (seconds)",q.flutterPeriod,.04f,10);
            ImGuiMCP::TextWrapped("Flutter angle is the sway to either side of Spread. Period is one full sway cycle; shorter is faster. Fairy uses Period for its full wingbeat cycle.");
            slider("Wing flexibility",q.flowStrength,0,1);slider("Root-to-tip delay (seconds)",q.tipDelaySeconds,0,.5f);
            ImGuiMCP::TextWrapped("Flexibility bends the wing from root to tip, synchronized with your actual wingbeat. Delay is the travel time over the whole wing. The root keeps your configured motion; each joint compensates for the rotation inherited from its parent.");
            if(q.flowStrength<=0){
                slider("Joint amplitude factor",q.jointFactor,0,1);slider("Joint delay (seconds)",q.jointDelay,0,.5f);
                ImGuiMCP::TextWrapped("Flexibility is disabled: using the previous joint amplitude/delay controls.");
            }
            if(p.wingTemplate!=channel::Template::fairy){
                ImGuiMCP::Separator();ImGuiMCP::TextUnformatted("Wingbeat articulation");
                slider("Fold when raised (degrees)",q.foldDegrees,-180,180);slider("Fold position (root 0 - tip 1)",q.foldCenter,.1f,.9f);
                slider("Bend when lowered (degrees)",q.strokeBendDegrees,-180,180);slider("Bend position (root 0 - tip 1)",q.strokeBendCenter,.1f,.9f);slider("Feather trail (degrees)",q.featherDegrees,-90,90);slider("Pause when lowered (cycle)",q.bottomHold,0,.45f);
                ImGuiMCP::TextWrapped("Fold curls the raised wing; the downstroke extends it and recovery folds it again. Bend when lowered shares the downstroke with the middle of the wing, so it reaches lower and curls inward without a larger Lowered angle at the root. "
                    "If the wing bends the wrong way, use a negative value. "
                    "Fold position and Bend position choose where along the wing each curl happens.Feathers stay open through the downstroke and trail during recovery. "
                    "Pause holds the lowered wing before recovery. Zero for all five restores the plain hinge stroke. Lift and sound timing are unchanged.");
                const auto pairCount=std::min<std::size_t>(std::max<std::size_t>({p.leftRoots.size(),static_cast<std::size_t>(selected.pairs),1}),channel::PoseSettings::maxPairs);
                if(pairCount>1){
                    ImGuiMCP::Separator();ImGuiMCP::TextUnformatted("Wing pairs");
                    for(unsigned i=0;i<pairCount;++i){ImGuiMCP::PushID(static_cast<int>(i)+2000);
                        if(i<p.leftRoots.size()&&i<p.rightRoots.size())ImGuiMCP::Text("Pair %u: %s / %s",i+1,p.leftRoots[i].c_str(),p.rightRoots[i].c_str());
                        else ImGuiMCP::Text("Pair %u",i+1);
                        slider("Delay (seconds)",q.pairDelaySeconds[i],0,.5f);slider("Amplitude",q.pairScale[i],0,2);
                        ImGuiMCP::PopID();
                    }
                    ImGuiMCP::TextWrapped("Pairs follow the same wingbeat. A small delay lets the motion travel from one pair to the next; amplitude scales the whole motion of that pair.");
                }
                ImGuiMCP::Separator();
            }
            slider("Original sway weight",q.nativeSwayWeight,0,1);slider("Pose blend (seconds)",q.blendSeconds,.02f,3);
            ImGuiMCP::TextUnformatted("Wing rotation direction");
            if(!q.axisDegrees)q.axisDegrees=channel::axisAngles(q.axis);
            auto& direction=*q.axisDegrees;
            bool axisChanged=slider("Axis heading (degrees)",direction[0],-180,180);
            axisChanged|=slider("Axis tilt (degrees)",direction[1],-90,90);
            if(axisChanged)channel::setAxisAngles(q,direction[0],direction[1]);
            ImGuiMCP::TextWrapped("Heading 0 / tilt 0 gives up/down flapping; heading 90 gives the sideways axis; tilt 90 gives the vertical axis. Directions follow the wing attachment and torso. Raised/Lowered angles control the motion range. Pose blend smooths takeover, release and settings changes, without reducing wingbeat amplitude.");
            }
            if(p.continuousFlight)slider("Continuous ascent speed",p.continuousRiseSpeed,.1f,20);
            if(ImGuiMCP::Button("Apply and Save Registration")){
                if(!channel::valid(q))message="Animation settings are invalid. The rotation axis must not be zero.";
                else{busy=true;const auto candidate=p;const bool fromScanner=scannerSelection;
                    SKSE::GetTaskInterface()->AddTask([candidate,fromScanner]{const auto error=saveWingRegistration(candidate,fromScanner);auto rules=wingRegistrationProfiles();
                        std::lock_guard lock(channelMenuMutex);saved=std::move(rules);busy=false;message=error.empty()?"Saved. The same rule now applies to the player and NPCs; unequipping keeps the INI.":error;
                        if(error.empty()){scannerSelection=false;for(const auto& rule:saved)if(rule.plugin==candidate.plugin&&rule.effectIDs==candidate.effectIDs&&rule.armorIDs==candidate.armorIDs){selected.profile=rule;break;}}
                    });}
            }
            ImGuiMCP::EndDisabled();
        }
    }
    ImGuiMCP::EndDisabled();
}
}
