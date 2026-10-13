#include "settings_file.hpp"
#include "live_settings.hpp"
#include "settings_template.hpp"
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>
#include <sstream>

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
std::string migrateSettingsText(std::string_view original) {
    if(original.size()>1024*1024)throw std::runtime_error("Wingflight.ini exceeds the 1 MiB migration limit; original retained.");
    using Values=std::map<std::pair<std::string,std::string>,std::string>;
    const auto parse=[](std::string_view text){
        Values values;std::string section,line;std::istringstream input{std::string(text)};
        unsigned number{};
        while(std::getline(input,line)){
            ++number;
            if(number==1&&line.starts_with("\xEF\xBB\xBF"))line.erase(0,3);
            const auto first=line.find_first_not_of(" \t\r");
            if(first==line.npos||line[first]==';'||line[first]=='#')continue;
            if(line[first]=='['){
                const auto close=line.find(']',first);
                if(close==line.npos)throw std::runtime_error("Malformed INI section at line "+std::to_string(number)+"; original retained.");
                section=normalized(std::string_view(line).substr(first+1,close-first-1));continue;
            }
            const auto equals=line.find('=',first);
            if(equals==line.npos||section.empty())throw std::runtime_error("Malformed INI setting at line "+std::to_string(number)+"; original retained.");
            const auto key=normalized(std::string_view(line).substr(first,equals-first));
            auto value=line.substr(equals+1);const auto comment=value.find_first_of(";#");
            if(comment!=value.npos)value.resize(comment);
            const auto start=value.find_first_not_of(" \t\r"),end=value.find_last_not_of(" \t\r");
            value=start==value.npos?std::string{}:value.substr(start,end-start+1);
            if(key.empty()||!values.emplace(std::make_pair(section,key),value).second)
                throw std::runtime_error("Duplicate or empty INI key: "+section+"/"+key+"; original retained.");
        }
        return values;
    };
    auto old=parse(original);
    const auto legacy=std::make_pair(std::string("body"),std::string("forwardlean"));
    const auto renamed=std::make_pair(std::string("body"),std::string("unarmedflightposes"));
    if(!old.contains(renamed)&&old.contains(legacy))old[renamed]=old.at(legacy);
    std::string output,section,line;std::istringstream lines{std::string(settingsTemplate)};
    while(std::getline(lines,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        const auto first=line.find_first_not_of(" \t");
        if(first!=line.npos&&line[first]=='[')section=normalized(std::string_view(line).substr(first+1,line.find(']',first)-first-1));
        else if(first!=line.npos&&line[first]!=';'&&line[first]!='#'){
            const auto equals=line.find('=',first);
            if(equals!=line.npos){
                const auto key=std::make_pair(section,normalized(std::string_view(line).substr(first,equals-first)));
                if(auto it=old.find(key);it!=old.end())line=line.substr(0,equals+1)+it->second;
            }
        }
        output+=line+"\r\n";
    }
    return output;
}

Configuration loadAndMigrateConfiguration() {
    const auto path=std::filesystem::absolute("Data/SKSE/Plugins/Wingflight.ini");
    const bool present=std::filesystem::exists(path);
    std::string original;
    if(present){
        if(std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("Wingflight.ini exceeds 1 MiB; original retained.");
        std::ifstream input(path,std::ios::binary);
        if(!input)throw std::runtime_error("Cannot read Wingflight.ini; original retained.");
        original.assign(std::istreambuf_iterator<char>(input),{});
        if(input.bad())throw std::runtime_error("Cannot finish reading Wingflight.ini; original retained.");
    }
    std::string decoded=original;
    if(decoded.starts_with("\xFF\xFE")){
        if((decoded.size()-2)%2)throw std::runtime_error("Invalid UTF-16 INI; original retained.");
        std::wstring wide((decoded.size()-2)/2,L'\0');
        std::memcpy(wide.data(),decoded.data()+2,decoded.size()-2);
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
        if(size<=0)throw std::runtime_error("Cannot decode UTF-16 INI; original retained.");
        decoded.resize(size);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),static_cast<int>(wide.size()),decoded.data(),size,nullptr,nullptr);
    }
    const auto updated=migrateSettingsText(decoded);
    if(updated==original)return loadConfiguration();
    std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=L".migration.tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Cannot create INI migration temporary file; original retained");
    DWORD written{};
    const bool writtenOK=WriteFile(file,updated.data(),static_cast<DWORD>(updated.size()),&written,nullptr)&&written==updated.size()&&FlushFileBuffers(file);
    const auto writeError=writtenOK?ERROR_SUCCESS:GetLastError();CloseHandle(file);
    try{
        if(!writtenOK)throw std::system_error(writeError,std::system_category(),"Cannot write migrated INI");
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,temporary.c_str());
        // Validate the complete candidate with the same parser used at startup.
        // Invalid values or bindings leave both the live INI and its bytes intact.
        auto configuration=loadConfiguration(temporary);
        std::filesystem::path backup;
        if(present){
            std::ifstream current(path,std::ios::binary);
            const std::string currentBytes((std::istreambuf_iterator<char>(current)),{});
            if(!current||currentBytes!=original)throw std::runtime_error("Wingflight.ini changed during migration; current file retained.");
            current.close();
            std::uint64_t hash=14695981039346656037ULL;
            for(unsigned char c:original){hash^=c;hash*=1099511628211ULL;}
            std::ostringstream suffix;suffix<<".pre-migration-"<<std::hex<<hash<<".bak";
            backup=path;backup+=suffix.str();
            if(!CopyFileW(path.c_str(),backup.c_str(),TRUE)){
                const auto error=GetLastError();
                std::ifstream saved(backup,std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(saved)),{});
                if(error!=ERROR_FILE_EXISTS||!saved||bytes!=original)
                    throw std::system_error(error,std::system_category(),"Cannot preserve old INI backup; original retained");
            }
        }
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(),std::system_category(),"Cannot replace Wingflight.ini; original retained");
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
        SKSE::log::info("Wingflight.ini migrated to schema 0.3.14-8: recognized values preserved, ForwardLean renamed to UnarmedFlightPoses, obsolete keys removed, current comments installed; backup={}",backup.empty()?"none (new INI)":backup.string());
        return configuration;
    }catch(...){DeleteFileW(temporary.c_str());throw;}
}

std::string updateLiveSettingsText(std::string_view original, Configuration settings) {
    roundLiveSettings(settings);
    std::map<std::string,std::map<std::string,std::string>> values;
    for(const auto& setting:liveFloatSettings)
        values[normalized(setting.section)][normalized(setting.key)]=numberText(setting.value(settings));
    values["flight"]["disablesprintwhenarmed"]=settings.disableSprintWhenArmed?"1":"0";
    values["collision"]["enabled"]=settings.collision.enabled?"1":"0";
    values["collision"]["damageactors"]=settings.collision.damageActors?"1":"0";
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
