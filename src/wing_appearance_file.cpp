#include "wing_appearance_file.hpp"
#include <Windows.h>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace wingflight::appearance {
namespace {
std::string trim(std::string_view s){const auto a=s.find_first_not_of(" \t\r\n");if(a==s.npos)return {};return std::string(s.substr(a,s.find_last_not_of(" \t\r\n")-a+1));}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool profileKey(std::string_view s){return s.starts_with("wing_")&&s.size()>10&&s.size()<2048&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==s.npos;}
std::string read(const std::filesystem::path& path){
    if(!std::filesystem::exists(path))return {};
    if(std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("Wing appearance file exceeds 1 MiB; original retained.");
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read wing appearance presets.");
    std::string text((std::istreambuf_iterator<char>(f)),{});if(f.bad())throw std::runtime_error("Cannot finish reading wing appearance presets.");return text;
}
std::string number(float n){char buffer[64];const auto [end,error]=std::to_chars(buffer,buffer+sizeof(buffer),n,std::chars_format::fixed,2);if(error!=std::errc{})throw std::runtime_error("Cannot format wing appearance value.");return {buffer,end};}
}
Profiles parseProfiles(std::string_view text){
    if(text.size()>1024*1024)throw std::runtime_error("Wing appearance file exceeds 1 MiB.");
    if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3);
    Profiles result;std::string section,line;std::set<std::pair<std::string,std::string>> seen;std::istringstream in{std::string(text)};
    while(std::getline(in,line)){
        auto s=trim(line);if(s.empty()||s[0]==';'||s[0]=='#')continue;
        if(s[0]=='['){const auto end=s.find(']');if(end==s.npos)throw std::runtime_error("Malformed appearance section.");section=lower(trim(std::string_view(s).substr(1,end-1)));continue;}
        if(!profileKey(section))continue;
        const auto eq=s.find('=');if(eq==s.npos)throw std::runtime_error("Malformed appearance setting.");
        auto key=lower(trim(std::string_view(s).substr(0,eq)));
        if(key!="normalscalepercent"&&key!="flightscalepercent"&&key!="verticaloffset")continue;
        if(!seen.emplace(section,key).second)throw std::runtime_error("Duplicate wing appearance setting.");
        const auto comment=s.find_first_of(";#",eq+1);const auto value=trim(std::string_view(s).substr(eq+1,comment==s.npos?s.size()-eq-1:comment-eq-1));
        float n{};const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);
        if(error!=std::errc{}||end!=value.data()+value.size()||!std::isfinite(n))throw std::runtime_error("Invalid wing appearance number.");
        auto& p=result[section];if(key=="normalscalepercent")p.normalScale=n;else if(key=="flightscalepercent")p.flightScale=n;else p.verticalOffset=n;
    }
    for(const auto& [key,p]:result)if(!valid(p))throw std::runtime_error("Wing appearance value is out of range.");
    return result;
}
std::string updateProfileText(std::string_view original,const std::string& rawKey,ProfileSettings p){
    const auto key=lower(rawKey);if(!profileKey(key)||!valid(p))throw std::runtime_error("Invalid wing appearance profile.");
    parseProfiles(original);p=rounded(p);
    const std::map<std::string,std::string> fields{{"normalscalepercent",number(p.normalScale)},{"flightscalepercent",number(p.flightScale)},{"verticaloffset",number(p.verticalOffset)}};
    std::string out,section,line;std::set<std::string> written;bool found{};std::istringstream in{std::string(original)};
    auto missing=[&]{for(const auto& [name,value]:fields)if(!written.contains(name)){out+=name+"="+value+"\r\n";written.insert(name);}};
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();auto s=trim(line);
        if(s.starts_with("\xEF\xBB\xBF"))s.erase(0,3);
        if(!s.empty()&&s[0]=='['){if(section==key)missing();const auto end=s.find(']');section=lower(trim(std::string_view(s).substr(1,end-1)));found|=section==key;}
        else if(section==key&&!s.empty()&&s[0]!=';'&&s[0]!='#'){
            const auto eq=s.find('=');if(eq!=s.npos){const auto name=lower(trim(std::string_view(s).substr(0,eq)));if(fields.contains(name)){
                out+=name+"="+fields.at(name)+"\r\n";written.insert(name);continue;
            }}
        }
        out+=line+"\r\n";
    }
    if(section==key)missing();
    if(!found){if(out.empty())out="; Wingflight per-wing appearance. Plugin/local FormID identities survive load-order changes.\r\n";out+="\r\n["+key+"]\r\n";missing();}
    parseProfiles(out);return out;
}
Profiles loadProfiles(const std::filesystem::path& file){return parseProfiles(read(file));}
Profiles saveProfile(const std::filesystem::path& file,const std::string& key,ProfileSettings p){
    const auto original=read(file),text=updateProfileText(original,key,p);auto parsed=parseProfiles(text);
    std::filesystem::create_directories(file.parent_path());auto temp=file;temp+=L".tmp";
    HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Cannot create appearance temporary file");
    DWORD written{};const bool ok=WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(handle);
    const auto error=ok?ERROR_SUCCESS:GetLastError();CloseHandle(handle);
    try{
        if(!ok)throw std::system_error(error,std::system_category(),"Cannot write appearance presets");
        if(read(file)!=original)throw std::runtime_error("Appearance file changed while saving; current file retained.");
        if(!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::system_error(GetLastError(),std::system_category(),"Cannot replace appearance presets");
    }catch(...){DeleteFileW(temp.c_str());throw;}
    return parsed;
}
}
