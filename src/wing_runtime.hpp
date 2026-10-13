#pragma once
#include <memory>
namespace wingflight {
struct WingMatch;
struct WingPlayback;
class WingRuntime {
public:
    WingRuntime();
    ~WingRuntime();
    bool install(float amplitude);
    void advance(float delta);
    WingPlayback prepare(WingMatch match);
    bool blend(float flap,float ownership,float fast);
    bool preview(float amount,float period,float amplitude);
    bool parkPower();
    bool parkFast();
    void powerRate(float rate);
    void loopRate(float fast,float spread);
    void pitch(float degrees);
    void release(bool smooth);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
