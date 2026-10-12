#pragma once
#include "lower_body_policy.hpp"
#include <span>

namespace wingflight::vampire {
struct PathNode {
    std::array<float,3> translation{};
    legs::Quaternion rotation;
    float scale{1};
    unsigned track{10}; // 0..9 keyed wings; other nodes keep their installed pose.
};
inline std::array<float,3> tip(std::span<const PathNode> path,const std::array<legs::Quaternion,10>& pose){
    std::array<float,3> position{};
    legs::Quaternion basis;float scale=1;
    for(const auto& node:path){
        const auto matrix=legs::matrix(basis);
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)position[i]+=matrix[i][j]*node.translation[j]*scale;
        basis=legs::multiply(basis,node.track<10?pose[node.track]:node.rotation);
        scale*=node.scale;
    }
    return position;
}
}
