#include "native_contract.hpp"
#include <array>
#include <hde64.h>

namespace wingflight::runtime {
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
