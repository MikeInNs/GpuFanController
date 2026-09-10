#include "AmdGpu.hpp"
#include "TemperatureForwarder.hpp"
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace std::chrono_literals;
namespace fs=std::filesystem;
using fan::SteadyClock;
void check(bool value) {if(!value) throw std::runtime_error("AMD assertion failed");}
void write(const fs::path& path,const std::string& value) {
    fs::create_directories(path.parent_path());std::ofstream(path)<<value;
}
struct Fixture {
    fs::path root;
    Fixture() {char name[]="/tmp/gpu-fan-amd-XXXXXX";auto* path=mkdtemp(name);if(!path) throw std::runtime_error("mkdtemp");root=path;}
    ~Fixture() {std::error_code error;fs::remove_all(root,error);}
    fs::path device(const std::string& pci,const std::string& vendor="0x1002") {
        auto path=root/pci;
        write(path/"vendor",vendor+"\n");write(path/"class","0x030000\n");write(path/"device","0x1234\n");
        fs::create_directory_symlink("/fixture/drivers/amdgpu",path/"driver");
        write(path/"hwmon/hwmon7/name","amdgpu\n");
        write(path/"hwmon/hwmon7/temp1_label","edge\n");
        write(path/"hwmon/hwmon7/temp1_input","49050\n");
        return path;
    }
};
int main() {
    Fixture f;fan::AmdGpu amd(f.root);
    const auto first=f.device("0000:01:00.0");
    const auto second=f.device("0000:02:00.0");
    const auto nvidia=f.device("0000:03:00.0","0x10de");
    (void)nvidia;
    f.device("0000:04:00.0","0x8086");
    auto audio=f.device("0000:01:00.1");write(audio/"class","0x040300");
    check(amd.owns("0000:01:00.0") && !amd.owns("../0000:01:00.0") && !amd.owns("0000:01:00.1"));
    check(amd.hasNvidia());
    auto inventory=amd.discover();
    check(inventory.size()==2 && inventory[0]["vendor"].get<std::string>()=="AMD" && inventory[0]["uuid"].is_null());
    check(inventory[0]["temperatureAvailable"].get<bool>() && inventory[0]["experimental"].get<bool>());
    const std::set<std::string> addresses{"0000:01:00.0","0000:02:00.0","0000:03:00.0","../escape"};
    auto sample=amd.sample(addresses);check(sample.temperatures.size()==2 && sample.temperatures.at("0000:01:00.0")==491);
    check(amd.sample({}).temperatures.empty());
    auto sensor=first/"hwmon/hwmon7";
    for(const auto* bad:{"","N/A","nan","125001","-40001","50000oops","99999999999999"}) {
        write(sensor/"temp1_input",bad);sample=amd.sample(addresses);
        check(sample.temperatures.size()==1 && sample.temperatures.contains("0000:02:00.0") && !sample.error.empty());
    }
    for(const auto& [input,expected]:std::array<std::pair<const char*,int>,4>{{{"-40000",-400},{"125000",1250},{"0",0},{"-150",-2}}}) {
        write(sensor/"temp1_input",input);check(amd.sample(addresses).temperatures.at("0000:01:00.0")==expected);
    }
    write(sensor/"temp1_input","50000");write(sensor/"temp2_input","99000");
    fs::permissions(sensor/"temp1_input",fs::perms::none);
    check(!amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    fs::permissions(sensor/"temp1_input",fs::perms::owner_read|fs::perms::owner_write);
    write(sensor/"temp1_label","junction");check(!amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    fs::remove(sensor/"temp1_label");check(amd.sample(addresses).temperatures.at("0000:01:00.0")==500);
    write(sensor/"temp1_fault","1");check(!amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    write(sensor/"temp1_fault","0");
    fs::remove(sensor/"temp1_input");check(!amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    check(!amd.discover()[0]["temperatureAvailable"].get<bool>());
    write(sensor/"temp1_input","51000");
    fs::rename(sensor,first/"hwmon/hwmon99");sensor=first/"hwmon/hwmon99";
    check(amd.sample(addresses).temperatures.at("0000:01:00.0")==510);
    write(first/"hwmon/hwmon3/name","amdgpu");check(!amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    write(first/"hwmon/hwmon3/name","coretemp");check(amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    fs::remove(first/"driver");fs::create_directory_symlink("/fixture/drivers/radeon",first/"driver");
    check(!amd.sample(addresses).temperatures.contains("0000:01:00.0"));
    fs::remove_all(second);check(amd.sample(addresses).temperatures.empty());

    // Each provider keeps its own timestamp; a fresh AMD sample cannot revive NVIDIA.
    const auto now=SteadyClock::now();
    std::array<fan::GpuReadings,2> sources{{{{{"0000:03:00.0",600}},now-3s,{}},{{{"0000:01:00.0",500}},now,{}}}};
    auto groups=nlohmann::json::array({{{"enabled",true},{"gpuPciAddress","0000:03:00.0"}},{{"enabled",true},{"gpuPciAddress","0000:01:00.0"}}});
    auto snapshot=fan::mappedTemperatures(groups,sources,now);
    check(snapshot.valid_mask==2 && snapshot.group1_deci_celsius==0 && snapshot.group2_deci_celsius==500);
    sources[0].sampledAt=now;check(fan::mappedTemperatures(groups,sources,now).valid_mask==3);
    sources[0].temperatures["0000:01:00.0"]=600;check(fan::mappedTemperatures(groups,sources,now).valid_mask==1);
    check(fan::mappedTemperatures(groups,sources,now+3s).valid_mask==0);

    // No real GPU or serial I/O. A blocked NVIDIA provider must not stall AMD.
    std::atomic<int> amdCalls=0;std::atomic<bool> nvidiaStarted=false,nvidiaDone=false;
    {
        fan::TemperatureForwarder runtime(true,[](const auto&)->std::shared_ptr<fan::SerialProbe>{throw std::runtime_error("fixture disconnected");},
            [](const auto&){},[](std::stop_token){},
            [&](const auto&)->fan::GpuReadings {
                nvidiaStarted=true;std::this_thread::sleep_for(2500ms);nvidiaDone=true;
                throw std::runtime_error("fixture NVIDIA failure");
            },[&](const auto& requested) {
                check(requested.contains("0000:01:00.0"));++amdCalls;
                return fan::GpuReadings{{{"0000:01:00.0",500}},SteadyClock::now(),"fixture AMD warning"};
            });
        runtime.configure({{"controllers",nlohmann::json::array({{{"controllerId",std::string(32,'a')},{"groups",groups}}})}});
        const auto deadline=SteadyClock::now()+2s;
        while((amdCalls<2 || !nvidiaStarted) && SteadyClock::now()<deadline) std::this_thread::sleep_for(10ms);
        check(amdCalls>=2 && nvidiaStarted && !nvidiaDone);
        check(runtime.status()["gpuError"].get<std::string>().find("fixture AMD warning")!=std::string::npos);
    }
    std::cout<<"AMD discovery, fixed edge source, malformed/missing sensors, renumbering, mixed freshness and independent sampling passed\n";
}
