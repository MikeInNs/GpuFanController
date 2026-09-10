#include "AdapterNames.hpp"
#include <regex>
#include <limits>
#include <algorithm>
namespace fan::adapterNames {
namespace {
void require(bool ok,const char* message) {if(!ok) throw std::invalid_argument(message);}
void fields(const nlohmann::json& value,std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size()==keys.size(),"Unexpected adapter-name fields");
    for(auto key:keys) require(value.contains(key),"Missing adapter-name field");
}
}
std::vector<std::uint8_t> encode(const nlohmann::json& names) {
    fields(names,{"generation","groups"});
    const auto& gen=names.at("generation");
    require(gen.is_number_integer() && gen>=0 && gen<=std::numeric_limits<std::uint32_t>::max(),"Invalid adapter-name generation");
    const auto generation=gen.get<std::uint32_t>();
    const auto& groups=names.at("groups");require(groups.is_array() && groups.size()==2,"Expected two adapter names");
    std::vector<std::uint8_t> bytes(94,0);
    for(int i=0;i<4;++i) bytes[i]=generation>>(8*i);
    for(int g=0;g<2;++g) {
        const auto& group=groups[g];fields(group,{"pciAddress","name"});
        require(group.at("name").is_string(),"Adapter name must be text");
        const auto name=group.at("name").get<std::string>();
        require(name.size()<=31,"Adapter name must be at most 31 characters");
        for(unsigned char ch:name) require(ch>=32 && ch<=126,"Adapter name must be printable ASCII");
        std::string pci;
        if(group.at("pciAddress").is_null()) require(name.empty(),"Unmapped adapter name must be empty");
        else {
            require(group.at("pciAddress").is_string(),"Adapter PCI address must be text or null");
            pci=group.at("pciAddress").get<std::string>();
            require(std::regex_match(pci,std::regex("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]")),"Invalid adapter PCI address");
            require(name.find_first_not_of(' ')!=std::string::npos,"Mapped adapter name cannot be empty");
        }
        std::copy(pci.begin(),pci.end(),bytes.begin()+4+45*g);
        std::copy(name.begin(),name.end(),bytes.begin()+17+45*g);
    }
    return bytes;
}
nlohmann::json decode(const std::vector<std::uint8_t>& bytes) {
    require(bytes.size()==94,"Invalid adapter-name payload length");
    std::uint32_t generation=0;for(int i=0;i<4;++i) generation|=std::uint32_t(bytes[i])<<(8*i);
    auto stringAt=[&](int offset,int size) {
        const auto begin=bytes.begin()+offset,end=begin+size,nul=std::find(begin,end,0);
        require(nul!=end && std::all_of(nul,end,[](auto ch){return ch==0;}),"Invalid adapter-name string padding");
        return std::string(begin,nul);
    };
    nlohmann::json result{{"generation",generation},{"groups",nlohmann::json::array()}};
    for(int g=0;g<2;++g) {
        const auto pci=stringAt(4+45*g,13);
        result["groups"].push_back({{"pciAddress",pci.empty()?nlohmann::json(nullptr):nlohmann::json(pci)},{"name",stringAt(17+45*g,32)}});
    }
    require(encode(result)==bytes,"Invalid adapter-name payload");return result;
}
void validateRequest(const nlohmann::json& request) {
    fields(request,{"confirmed","adapterNames"});
    require(request.at("confirmed").is_boolean() && request.at("confirmed").get<bool>(),"Confirm saving adapter names to EEPROM");
    encode(request.at("adapterNames"));
}
}
