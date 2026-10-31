#pragma once
#include <string_view>

namespace wingflight::channel {
inline std::string_view stableWingNodeName(std::string_view name){
    constexpr std::string_view prefix="hdtSSEPhysics_AutoRename_Armor_";
    for(unsigned pass=0;pass<4&&name.starts_with(prefix);++pass){
        const auto end=name.find(' ',prefix.size());
        if(end==name.npos||end==prefix.size()||end+1>=name.size())break;
        const auto digits=name.substr(prefix.size(),end-prefix.size());
        if(digits.size()>16)break;
        bool hex=true;for(char c:digits)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))){hex=false;break;}
        if(!hex)break;
        name.remove_prefix(end+1);
    }
    return name;
}
// Caller supplies only nodes linked to this model's skin, never an actor-wide
// first match. Ambiguous names do not bind an unrelated accessory's bones.
template<class Range,class Name>
auto uniqueWingNode(std::string_view requested,const Range& nodes,Name name){
    typename Range::value_type result{};
    const auto stable=stableWingNodeName(requested);
    if(stable.empty())return result;
    for(auto node:nodes)if(stableWingNodeName(name(node))==stable){
        if(result&&result!=node)return typename Range::value_type{};
        result=node;
    }
    return result;
}
}
