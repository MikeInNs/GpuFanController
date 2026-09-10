#include "NvmlReader.hpp"
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    fan::NvmlReader reader(argv[1],std::chrono::milliseconds(std::stoi(argv[2])));
    for(std::string line;std::getline(std::cin,line);) {
        try {
            const auto request=nlohmann::json::parse(line);
            auto result=reader.sample(request.get<std::set<std::string>>());
            std::cout<<nlohmann::json{{"temperatures",result.temperatures},{"error",result.error}}.dump()<<std::endl;
        } catch(const std::exception& e) {std::cout<<nlohmann::json{{"error",e.what()},{"temperatures",nlohmann::json::object()}}.dump()<<std::endl;}
    }
}
