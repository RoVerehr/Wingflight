#include "native_contract.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <hde64.h>

namespace wingflight::runtime {
namespace {
template<class Visitor> void instructions(std::span<const std::uint8_t> code,Visitor visit) {
    for(std::size_t offset=0;offset<code.size();){
        std::array<std::uint8_t,16> bytes{};
        std::memcpy(bytes.data(),code.data()+offset,(std::min)(bytes.size(),code.size()-offset));
        hde64s instruction{};const auto size=hde64_disasm(bytes.data(),&instruction);
        if(!size||(instruction.flags&F_ERROR)||size>code.size()-offset)break;
        visit(offset,instruction);offset+=size;
    }
}
std::int32_t displacement(const hde64s& i){
    return (i.flags&F_DISP8)?static_cast<std::int8_t>(i.disp.disp8)
        :(i.flags&F_DISP32)?static_cast<std::int32_t>(i.disp.disp32):0;
}
}
bool fallingPredicate(std::span<const std::uint8_t> code,std::uint32_t processOffset){
    bool process{},context{},airState{},vertical{},scale{},negative{},compare{},result{};
    instructions(code,[&](std::size_t,const hde64s& i){
        const bool memory=(i.flags&F_MODRM)&&i.modrm_mod!=3;
        const auto offset=displacement(i);
        process|=memory&&i.rex_w&&i.opcode==0x8B&&i.modrm_rm==1&&!i.rex_b&&offset==static_cast<std::int32_t>(processOffset);
        context|=memory&&i.opcode==0x8D&&offset==0x1E0;
        airState|=(i.opcode==0x83&&i.modrm_reg==7&&i.imm.imm8==2)
            ||(i.opcode==0x3D&&i.imm.imm32==2);
        vertical|=memory&&i.p_rep==0xF3&&i.opcode==0x0F&&i.opcode2==0x10&&offset==0x98;
        scale|=memory&&i.p_rep==0xF3&&i.opcode==0x0F&&i.opcode2==0x59;
        negative|=memory&&i.opcode==0x0F&&i.opcode2==0x57;
        compare|=i.opcode==0x0F&&(i.opcode2==0x2F||i.opcode2==0x2E);
        result|=i.opcode==0x0F&&((i.opcode2>=0x40&&i.opcode2<=0x4F)||(i.opcode2>=0x90&&i.opcode2<=0x9F));
    });
    return process&&context&&airState&&vertical&&scale&&negative&&compare&&result;
}
SprintFallCall sprintFallCall(std::uintptr_t address,std::span<const std::uint8_t> code,
    std::uintptr_t falling,std::uintptr_t overencumbered,std::uint32_t requestFlagOffset){
    SprintFallCall previous{},found{};unsigned matches{};bool request{},clearBit{};
    instructions(code,[&](std::size_t offset,const hde64s& i){
        request|=(i.flags&F_MODRM)&&i.modrm_mod!=3&&displacement(i)==static_cast<std::int32_t>(requestFlagOffset);
        clearBit|=((i.opcode==0x80&&i.modrm_reg==4)||i.opcode==0x24)&&i.imm.imm8==0xFE;
        if(i.opcode==0xE8){
            const auto target=address+offset+i.len+static_cast<std::int32_t>(i.imm.imm32);
            if(target==overencumbered&&previous.target==falling){found=previous;++matches;}
            previous={offset,target,i.len};
        }else if(i.opcode==0xFF&&(i.modrm_reg==2||i.modrm_reg==3))previous={};
    });
    return request&&clearBit&&matches==1?found:SprintFallCall{};
}
SprintFallCall sprintActionFallCall(std::uintptr_t address,std::span<const std::uint8_t> code,
    std::uintptr_t falling,std::uintptr_t overencumbered){
    SprintFallCall previous{},found{};unsigned matches{},action{};
    bool actorType{},actionData{},stamina{},sprintMask{},actorState{},cancel{};
    instructions(code,[&](std::size_t offset,const hde64s& i){
        const bool memory=(i.flags&F_MODRM)&&i.modrm_mod!=3;
        const auto field=displacement(i);
        actorType|=memory&&i.opcode==0x80&&i.modrm_reg==7&&field==0x1A&&i.imm.imm8==0x3E;
        actionData|=memory&&i.rex_w&&i.opcode==0x8B&&field==0x18;
        // DEFAULT_OBJECTS::kActionSprintStart == 65. A later action selector
        // ends this branch; do not accept another action's falling predicate.
        if(i.opcode==0xB9){action=i.imm.imm32;stamina=sprintMask=actorState=false;}
        if(action==65){
            stamina|=i.opcode==0xBA&&i.imm.imm32==0x1A;
            sprintMask|=i.opcode==0xBA&&i.imm.imm32==0x100;
            actorState|=memory&&i.rex_w&&i.opcode==0x8D&&field==0xC0;
        }
        cancel|=action==65&&memory&&i.opcode==0xC7&&field==0x38&&i.imm.imm32==0xFFFFFFFF;
        if(i.opcode==0xE8){
            const auto target=address+offset+i.len+static_cast<std::int32_t>(i.imm.imm32);
            if(action==65&&stamina&&sprintMask&&actorState&&target==overencumbered&&previous.target==falling){
                found=previous;++matches;
            }
            previous={offset,target,i.len};
        }else if(i.opcode==0xFF&&(i.modrm_reg==2||i.modrm_reg==3))previous={};
    });
    return actorType&&actionData&&cancel&&matches==1?found:SprintFallCall{};
}
std::vector<SprintFallCall> directCalls(std::uintptr_t address,std::span<const std::uint8_t> code){
    std::vector<SprintFallCall> calls;
    instructions(code,[&](std::size_t offset,const hde64s& i){
        if(i.opcode==0xE8)calls.push_back({offset,address+offset+i.len+static_cast<std::int32_t>(i.imm.imm32),i.len});
    });
    return calls;
}
SprintFallCall replacementSprintFallCall(std::uintptr_t address,std::span<const std::uint8_t> code,
    std::uintptr_t falling,std::uintptr_t sprintChanged,const PointerReader& pointer){
    std::array<std::uintptr_t,16> registers{};
    SprintFallCall found{};unsigned matches{},changes{};bool stamina{},clearRequest{};
    instructions(code,[&](std::size_t offset,const hde64s& i){
        const auto reg=i.modrm_reg+(i.rex_r?8:0),rm=i.modrm_rm+(i.rex_b?8:0);
        const bool rip=(i.flags&F_MODRM)&&i.modrm_mod==0&&i.modrm_rm==5&&!i.rex_b;
        const auto indirect=[&](){return pointer(address+offset+i.len+displacement(i)).value_or(0);};
        stamina|=i.opcode==0xBA&&i.imm.imm32==0x1A;
        clearRequest|=i.opcode==0x80&&i.modrm_mod!=3&&i.modrm_reg==4&&i.imm.imm8==0xFE;
        if(i.opcode==0x8B&&i.rex_w)registers[reg]=i.modrm_mod==3?registers[rm]:rip?indirect():0;
        else if(i.opcode==0x89&&i.rex_w&&i.modrm_mod==3)registers[rm]=registers[reg];
        else if((i.opcode>=0xB8&&i.opcode<=0xBF))registers[i.opcode-0xB8+(i.rex_b?8:0)]=0;
        else if(i.opcode==0x8D||i.opcode==0x8A||(i.opcode==0x0F&&(i.opcode2==0xB6||i.opcode2==0xB7)))registers[reg]=0;
        else if((i.opcode==0x31||i.opcode==0x33)&&i.modrm_mod==3)registers[i.opcode==0x31?rm:reg]=0;
        std::uintptr_t target{};
        const bool call=i.opcode==0xE8||(i.opcode==0xFF&&i.modrm_reg==2);
        if(i.opcode==0xE8)target=address+offset+i.len+static_cast<std::int32_t>(i.imm.imm32);
        else if(call)target=i.modrm_mod==3?registers[rm]:rip?indirect():0;
        if(call){
            if(target==falling){found={offset,target,i.len};++matches;}
            if(target==sprintChanged)++changes;
            for(auto r:{0,1,2,8,9,10,11})registers[r]=0;
        }
    });
    return matches==1&&changes==1&&stamina&&clearRequest?found:SprintFallCall{};
}
NativeContract inspectContract(std::span<const std::uint8_t> code) {
    NativeContract result;
    std::array<bool,16> self{},data{};self[1]=true;data[2]=true;
    for(std::size_t offset=0;offset+16<=code.size();) {
        hde64s instruction{};const auto size=hde64_disasm(code.data()+offset,&instruction);
        if(!size||(instruction.flags&F_ERROR))break;
        if((instruction.flags&F_MODRM)&&instruction.modrm_mod!=3) {
            const unsigned base=instruction.modrm_rm==4
                ?instruction.sib_base+(instruction.rex_b?8:0):instruction.modrm_rm+(instruction.rex_b?8:0);
            const bool relative=instruction.modrm_mod==0&&(instruction.modrm_rm==5
                ||(instruction.modrm_rm==4&&instruction.sib_base==5));
            const auto displacement=(instruction.flags&F_DISP8)?static_cast<std::int32_t>(static_cast<std::int8_t>(instruction.disp.disp8))
                :(instruction.flags&F_DISP32)?static_cast<std::int32_t>(instruction.disp.disp32):0;
            if(!relative&&base<16) {
                if(data[base]&&displacement==0&&instruction.p_rep==0xF3&&instruction.opcode==0x0F&&instruction.opcode2==0x10)
                    result.pointerTime=true;
                if(self[base]) {
                    result.keys|=displacement==0x58;
                    result.count|=displacement==0x60;
                    result.initialized|=displacement==0x6C;
                }
            }
        }
        if((instruction.opcode==0x89||instruction.opcode==0x8B)&&instruction.rex_w) {
            const unsigned reg=instruction.modrm_reg+(instruction.rex_r?8:0);
            const unsigned rm=instruction.modrm_rm+(instruction.rex_b?8:0);
            if(instruction.modrm_mod==3) {
                const auto to=instruction.opcode==0x89?rm:reg,from=instruction.opcode==0x89?reg:rm;
                self[to]=self[from];data[to]=data[from];
            } else if(instruction.opcode==0x8B) {self[reg]=false;data[reg]=false;}
        }
        offset+=size;
        if(instruction.opcode==0xC3||instruction.opcode==0xC2)break;
    }
    return result;
}
}
