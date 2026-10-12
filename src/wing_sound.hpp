#pragma once
#include <cmath>
#include "diagnostics.hpp"

namespace wingflight {
// Follow the displayed Ni sequence, not the Gamebryo generator's elapsed clock.
// A stroke may become visible after its marker, but never emit a late recovery
// sound. Invisible parked clocks and the initial entry hold remain silent.
class WingStrokeSound {
public:
    void reset() { *this = {}; }
    bool update(bool active, float phase, float weight, float rate, float marker, float downEnd) {
        if (!active || !std::isfinite(phase) || phase<0 || phase>1
            || !std::isfinite(weight) || !std::isfinite(rate)
            || !std::isfinite(marker) || !std::isfinite(downEnd)
            || marker<0 || marker>=downEnd || downEnd>1) {
            if(active)diagnostic::report("sound.stroke-phase-invalid","reset stroke sound tracker",this,"power/fast",phase,marker);
            reset();return false;
        }
        const bool known=lastPhase_>=0;
        const bool wrapped=known && phase+0.01f<lastPhase_;
        const bool advancing=known && (wrapped || phase>lastPhase_+0.000001f);
        const bool crossed=known && (wrapped || lastPhase_<marker) && phase>=marker;
        const bool visibleCrossing=crossed && lastWeight_>=0.5f;
        if (wrapped) { played_=false; }
        lastPhase_=phase;
        lastWeight_=weight;
        if (!advancing || played_ || weight<0.5f || rate<=0.011f
            || phase<marker || (phase>=downEnd && !visibleCrossing)) { return false; }
        played_=true;return true;
    }
private:
    float lastPhase_{-1};
    float lastWeight_{};
    bool played_{};
};
// AWU's expanded/flutter loop has two swing markers per cycle. Follow the
// actually displayed private clock and contribution, never a hidden generator.
class WingFlutterSound {
public:
    void reset(){lastPhase_=-1;lastWeight_=0;}
    bool update(bool active,float phase,float weight,float rate,float first,float second){
        if(!active||!std::isfinite(phase)||phase<0||phase>=1||!std::isfinite(weight)||!std::isfinite(rate)
            ||!std::isfinite(first)||!std::isfinite(second)||first<0||first>=second||second>=1){
            if(active)diagnostic::report("sound.flutter-phase-invalid","reset flutter sound tracker",this,"spread",phase,first);
            reset();return false;}
        const float previous=lastPhase_,previousWeight=lastWeight_;
        const bool wrapped=previous>=0&&phase+0.01f<previous;
        const bool advancing=previous>=0&&(wrapped||phase>previous+0.000001f);
        lastPhase_=phase;lastWeight_=weight;
        if(!advancing||weight<0.5f||previousWeight<0.5f||rate<=0.011f)return false;
        auto crossed=[&](float marker){return wrapped?(previous<marker||phase>=marker):(previous<marker&&phase>=marker);};
        // A hitch may span both keys. Consume their crossings, but do not burst
        // two coincident sounds when the displayed frame has already caught up.
        return crossed(first)||crossed(second);
    }
private:
    float lastPhase_{-1},lastWeight_{};
};
}
