#include "settings_file.hpp"
#include "live_settings.hpp"
#include <Windows.h>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>

namespace wingflight {
namespace {
std::string normalized(std::string_view value) {
    const auto first=value.find_first_not_of(" \t\r");
    if(first==value.npos)return {};
    const auto last=value.find_last_not_of(" \t\r");
    std::string result(value.substr(first,last-first+1));
    for(auto& c:result)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    return result;
}
std::string numberText(float value) {
    char buffer[64];
    const auto [end,error]=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::fixed,2);
    if(error!=std::errc{})throw std::runtime_error("Cannot serialize a setting.");
    return {buffer,end};
}
}
std::string updateLiveSettingsText(std::string_view original, Configuration settings) {
    roundLiveSettings(settings);
    std::map<std::string,std::map<std::string,std::string>> values;
    for(const auto& setting:liveFloatSettings)
        values[normalized(setting.section)][normalized(setting.key)]=numberText(setting.value(settings));
    values["flight"]["disablesprintwhenarmed"]=settings.disableSprintWhenArmed?"1":"0";
    values["lowerbody"]["relaxedarmedlegs"]=settings.legRelaxation.enabled?"1":"0";
    values["lowerbody"]["secondarymotion"]=settings.legRelaxation.secondaryMotion?"1":"0";
    values["keyboard"]["cyclemode"]=std::to_string(settings.modeKey);
    values["keyboard"]["land"]=std::to_string(settings.landKey);
    values["keyboard"]["descend"]=std::to_string(settings.descendKey);
    values["gamepad"]["cyclemode"]=std::to_string(settings.padMode);
    values["gamepad"]["land"]=std::to_string(settings.padLand);
    values["gamepad"]["descend"]=std::to_string(settings.padDescend);
    const auto eol=original.find("\r\n")!=original.npos?"\r\n":"\n";
    std::string output,section;
    std::map<std::string,bool> seenSections;
    std::map<std::string,std::map<std::string,bool>> seenKeys;
    auto appendMissing=[&] {
        if(!values.contains(section))return;
        for(const auto& [key,value]:values.at(section))if(!seenKeys[section][key]) {
            if(!output.empty()&&output.back()!='\n')output+=eol;
            output+=key+"="+value+eol;seenKeys[section][key]=true;
        }
    };
    for(std::size_t begin=0;begin<original.size();) {
        const auto newline=original.find('\n',begin);
        const auto end=newline==original.npos?original.size():newline+1;
        auto line=original.substr(begin,end-begin);
        auto content=line;
        if(begin==0&&content.starts_with("\xEF\xBB\xBF"))content.remove_prefix(3);
        const auto trimmed=normalized(content);
        if(trimmed.starts_with('[')) {
            const auto close=trimmed.find(']');
            if(close!=trimmed.npos) {
                appendMissing();section=normalized(std::string_view(trimmed).substr(1,close-1));
                if(values.contains(section)&&seenSections[section])throw std::runtime_error("Duplicate menu section in Wingflight.ini.");
                seenSections[section]=true;
            }
        } else if(!trimmed.empty()&&!trimmed.starts_with(';')&&!trimmed.starts_with('#')&&values.contains(section)) {
            const auto equals=content.find('=');
            if(equals!=content.npos) {
                const auto key=normalized(content.substr(0,equals));
                if(section=="flight"&&(key=="glidegravity"||key=="verticalspeed")){begin=end;continue;}
                if(section=="freeflight"&&(key=="risedistance"||key=="falldistance"
                    ||key=="recoveryseconds"||key=="bobblendseconds")) {
                    begin=end;continue;
                }
                if(values.at(section).contains(key)) {
                    if(seenKeys[section][key])throw std::runtime_error("Duplicate menu setting in Wingflight.ini.");
                    seenKeys[section][key]=true;
                    output+=std::string(line.substr(0,equals+1))+values.at(section).at(key);
                    const auto comment=content.find_first_of(";#",equals+1);
                    if(comment!=content.npos) {
                        auto suffix=content.substr(comment);
                        output+=" ";output+=suffix;
                    } else if(line.ends_with("\r\n"))output+="\r\n";
                    else if(line.ends_with('\n'))output+='\n';
                    begin=end;continue;
                }
            }
        }
        output+=line;begin=end;
    }
    appendMissing();
    for(const auto& [name,entries]:values)if(!seenSections[name]) {
        if(!output.empty()&&output.back()!='\n')output+=eol;
        output+=std::string(eol)+"["+name+"]"+eol;
        for(const auto& [key,value]:entries)output+=key+"="+value+eol;
    }
    return output;
}

void saveLiveSettings(const std::filesystem::path& path,const Configuration& settings) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("Cannot read Wingflight.ini; no changes were applied.");
    const std::string original((std::istreambuf_iterator<char>(input)),{});
    if(input.bad())throw std::runtime_error("Cannot finish reading Wingflight.ini.");
    input.close();
    const auto updated=updateLiveSettingsText(original,settings);
    if(updated==original)return;
    // Same-directory temporary file keeps replacement atomic on the target volume.
    // MO2 may put writes in Overwrite; the menu reports failures without applying.
    auto temporary=path;temporary+=L".menu.tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Cannot create settings temporary file");
    DWORD written{};
    const bool success=updated.size()<=MAXDWORD && WriteFile(file,updated.data(),static_cast<DWORD>(updated.size()),&written,nullptr)
        && written==updated.size() && FlushFileBuffers(file);
    const auto error=success?ERROR_SUCCESS:GetLastError();CloseHandle(file);
    if(!success) {DeleteFileW(temporary.c_str());throw std::system_error(error,std::system_category(),"Cannot write settings temporary file");}
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto moveError=GetLastError();DeleteFileW(temporary.c_str());
        throw std::system_error(moveError,std::system_category(),"Cannot replace Wingflight.ini");
    }
    // Invalidate Windows profile API caches after replacing the INI.
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
}
}
