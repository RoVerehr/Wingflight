#pragma once
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
namespace wingflight {
inline std::vector<std::string> wingSoundFiles(const std::string& text){
    std::vector<std::string> files;std::istringstream input(text);std::string item;
    while(std::getline(input,item,';')){
        const auto first=item.find_first_not_of(" \t"),last=item.find_last_not_of(" \t");
        if(first==item.npos)continue;
        auto file=item.substr(first,last-first+1);std::replace(file.begin(),file.end(),'/','\\');
        files.push_back(std::move(file));
    }
    return files;
}
inline std::string wingSoundFileList(const std::vector<std::string>& files){
    std::string text;for(const auto& file:files){if(!text.empty())text+=';';text+=file;}return text;
}
}
