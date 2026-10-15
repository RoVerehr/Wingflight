#pragma once
#include "yaw_policy.hpp"
#include "diagnostics.hpp"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
namespace wingflight {
// Public V2 ABI declarations only; published by ersh for other mods.
// https://github.com/ersh1/TrueDirectionalMovement/blob/57b913ae17fed857c9e281799174898cd66a3e82/src/TrueDirectionalMovementAPI.h
class TdmYawAPI {
public:
    virtual unsigned long GetTDMThreadId() const noexcept=0;
    virtual bool GetDirectionalMovementState() const noexcept=0;
    virtual bool GetTargetLockState() const noexcept=0;
    virtual RE::ActorHandle GetCurrentTarget() const noexcept=0;
    virtual YawResult RequestDisableDirectionalMovement(SKSE::PluginHandle) noexcept=0;
    virtual YawResult RequestDisableHeadtracking(SKSE::PluginHandle) noexcept=0;
    virtual SKSE::PluginHandle GetDisableDirectionalMovementOwner() const noexcept=0;
    virtual SKSE::PluginHandle GetDisableHeadtrackingOwner() const noexcept=0;
    virtual YawResult ReleaseDisableDirectionalMovement(SKSE::PluginHandle) noexcept=0;
    virtual YawResult ReleaseDisableHeadtracking(SKSE::PluginHandle) noexcept=0;
    virtual YawResult RequestYawControl(SKSE::PluginHandle,float) noexcept=0;
    virtual YawResult SetPlayerYaw(SKSE::PluginHandle,float) noexcept=0;
    virtual YawResult ReleaseYawControl(SKSE::PluginHandle) noexcept=0;
};
class FlightFacing {
public:
    void initialize() {
        const auto module=GetModuleHandleW(L"TrueDirectionalMovement.dll");
        if(!module)return;
        using Request=void*(*)(std::uint8_t);
        const auto request=reinterpret_cast<Request>(GetProcAddress(module,"RequestPluginAPI"));
        api_=request?static_cast<TdmYawAPI*>(request(1)):nullptr; // V2 = 1
        SKSE::log::info("Optional TDM yaw interface available={}",api_!=nullptr);
    }
    bool active() const { return lease_.owned; }
    bool targetLocked() const { return api_ && api_->GetTargetLockState(); }
    void release() { if(api_)lease_.release(*api_,SKSE::GetPluginHandle()); }
    void update(RE::PlayerCharacter* player,bool enabled,float desiredYaw,float dt,float response) {
        if(!api_)return; // Native rotation remains untouched without TDM.
        const auto camera=RE::PlayerCamera::GetSingleton();
        if(!enabled || !camera || !camera->IsInThirdPerson() || api_->GetTargetLockState()) {release();return;}
        if(api_->GetTDMThreadId()!=GetCurrentThreadId()){
            diagnostic::report("tdm.thread-mismatch","skip yaw API call",api_,{},GetCurrentThreadId(),api_->GetTDMThreadId());return;}
        const auto previous=lease_.last;const bool wasOwned=lease_.owned;
        if(lease_.acquire(*api_,SKSE::GetPluginHandle())) {
            lease_.set(*api_,SKSE::GetPluginHandle(),facingYaw(player->GetAngleZ(),desiredYaw,dt,response));
        }
        if(previous!=lease_.last || wasOwned!=lease_.owned) {
            SKSE::log::debug("TDM flight facing: owned={} result={}",lease_.owned,static_cast<unsigned>(lease_.last));
        }
    }
private:
    TdmYawAPI* api_{};
    YawLease lease_;
};
}
