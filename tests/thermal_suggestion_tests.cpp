#include "ThermalSuggestion.hpp"
#include "GpuThermalLimits.hpp"
#include "AmdGpu.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using Json=nlohmann::json;
namespace fs=std::filesystem;
int checks=0;
void check(bool ok) {++checks;if(!ok) throw std::runtime_error("Thermal check "+std::to_string(checks)+" failed");}
template<class F> void rejects(F f) {bool failed=false;try {f();}catch(const std::exception&) {failed=true;}check(failed);}
const std::string pci="0000:01:00.0";
const std::string logText=R"(==============NVSMI LOG==============
GPU 00000000:01:00.0
    Temperature
        GPU Current Temp                  : 50 C
        GPU T.Limit Temp                  : N/A
        GPU Shutdown Temp                 : 102 C
        GPU Slowdown Temp                 : 97 C
        GPU Max Operating Temp            : N/A
        GPU Target Temperature            : 92 C
        Memory Current Temp               : N/A
)";
void write(const fs::path& path,const std::string& s) {fs::create_directories(path.parent_path());std::ofstream(path)<<s;}
struct Temp {
    fs::path root;
    Temp() {char p[]="/tmp/fan-thermal-XXXXXX";const auto r=mkdtemp(p);if(!r) throw std::runtime_error("mkdtemp");root=r;}
    ~Temp() {fs::remove_all(root);}
};
int main() {
    auto meta=fan::GpuThermalLimits::parseNvidia(pci,logText);
    check(meta["limits"]["operatingDeciC"].is_null());check(meta["limits"]["targetDeciC"]==920);
    rejects([&]{fan::GpuThermalLimits::parseNvidia("0000:02:00.0",logText);});
    rejects([&]{fan::GpuThermalLimits::parseNvidia("../escape",logText);});
    rejects([&]{fan::GpuThermalLimits::parseNvidia(pci,logText+"GPU 00000000:01:00.0\n");});
    rejects([&]{fan::GpuThermalLimits::parseNvidia(pci,logText+"        GPU Slowdown Temp : 97 C\n");});
    for(const auto* invalid:{"garbage","97","0 C","201 C","-1 C","97.5 C"}) {
        auto bad=logText;bad.replace(bad.find("97 C"),4,invalid);
        rejects([&]{fan::GpuThermalLimits::parseNvidia(pci,bad);});
    }
    const auto relative=fan::GpuThermalLimits::parseNvidia(pci,
        "GPU 00000000:01:00.0\n    Temperature\n        GPU Slowdown T.Limit Temp : -5 C\n");
    check(relative["limits"]["slowdownDeciC"].is_null());
    Json group={{"enabled",true},{"expectedFanMask",3},{"minimumDutyPercent",20},{"startupDutyPercent",100},
        {"startupTimeMs",1000},{"warningTemperatureDeciC",750},{"criticalTemperatureDeciC",850},
        {"rpmLowThresholdPercent",60},{"faultDelaySeconds",3},{"currentDeviationPercent",40},
        {"curve",Json::array({{{"temperatureDeciC",300},{"rpm",2500}},{{"temperatureDeciC",500},{"rpm",4500}},
            {{"temperatureDeciC",700},{"rpm",6500}},{{"temperatureDeciC",850},{"rpm",8500}}})}};
    Json config={{"generation",1},{"hostUpdateTimeoutMs",10000},{"voltageWarningLowMv",11000},
        {"voltageCriticalLowMv",10500},{"voltageHighMv",13200},{"groups",Json::array({group,group})}};
    Json cal={{"generation",7},{"valid",true},{"startDutyPercent",{15,15}},{"points",Json::array()}};
    for(int d=20;d<=100;d+=10) cal["points"].push_back({{"dutyPercent",d},{"fanRpm",{d*100,d*90}}});
    fan::thermalSuggestion::Margins m;
    auto build=[&](const Json& data,const Json& draft,bool curve=false) {return fan::thermalSuggestion::build(data,config,cal,0,draft,m,curve);};
    auto p=build(meta,group,true);
    check(p.changes["warningTemperatureDeciC"]==870 && p.changes["criticalTemperatureDeciC"]==920);
    check(p.changes["curve"][3]["temperatureDeciC"]==820 && p.changes["curve"][3]["rpm"]==9000);
    for(int i=0;i<3;++i) check(p.changes["curve"][i]==group["curve"][i]);
    check(build(meta,group).changes.size()==2);check(group["curve"][3]["rpm"]==8500);
    auto absent=cal;cal["valid"]=false;check(build(meta,group).changes.size()==2);
    rejects([&]{build(meta,group,true);});cal=absent;
    auto draft=group;draft["expectedFanMask"]=1;rejects([&]{build(meta,draft,true);});
    draft=group;draft["minimumDutyPercent"]=30;rejects([&]{build(meta,draft,true);});
    draft=group;draft["curve"][2]["temperatureDeciC"]=830;rejects([&]{build(meta,draft,true);});
    draft=group;draft["curve"][1]["rpm"]=100;rejects([&]{build(meta,draft,true);});
    draft=group;draft["curve"][1]["rpm"]=7000;rejects([&]{build(meta,draft,true);});
    auto bad=meta;bad["sensor"]="gpu-junction";rejects([&]{build(bad,group);});
    rejects([&]{build(relative,group);});
    bad=meta;bad["limits"]["slowdownDeciC"]=nullptr;rejects([&]{build(bad,group);});
    bad=meta;bad["limits"]["targetDeciC"]=980;rejects([&]{build(bad,group);});
    bad=meta;bad["limits"]["shutdownDeciC"]=960;rejects([&]{build(bad,group);});
    bad=meta;bad["limits"]["operatingDeciC"]=950;bad["limits"]["targetDeciC"]=nullptr;
    check(build(bad,group).changes["criticalTemperatureDeciC"]==900);
    bad["limits"]["operatingDeciC"]=980;rejects([&]{build(bad,group);});
    m.critical=10;rejects([&]{build(meta,group);});m.critical=0;rejects([&]{build(meta,group);});m={};
    m.full=2;rejects([&]{auto noTarget=meta;noTarget["limits"]["targetDeciC"]=nullptr;build(noTarget,group);});m={};
    m.target=100;rejects([&]{build(meta,group);});m={};
    bad=meta;bad["limits"]["slowdownDeciC"]=1500;bad["limits"]["shutdownDeciC"]=1600;bad["limits"]["targetDeciC"]=nullptr;
    rejects([&]{build(bad,group);});
    Temp f;const auto device=f.root/pci,sensor=device/"hwmon/hwmon3";
    write(device/"vendor","0x1002");write(device/"class","0x030000");
    fs::create_directory_symlink("/fixture/drivers/amdgpu",device/"driver");
    write(sensor/"name","amdgpu");write(sensor/"temp1_label","edge");write(sensor/"temp1_input","50000");
    write(sensor/"temp1_crit","95000");write(sensor/"temp1_emergency","105000");write(sensor/"temp2_crit","110000");
    fan::AmdGpu amd(f.root);auto a=amd.thermalLimits(pci);
    check(a["sensor"]=="gpu-edge" && a["limits"]["criticalDeciC"]==950 && a["limits"]["targetDeciC"].is_null());
    check(build(a,group).changes["criticalTemperatureDeciC"]==900);
    fs::remove(sensor/"temp1_crit");a=amd.thermalLimits(pci);check(a["limits"]["criticalDeciC"].is_null());
    rejects([&]{build(a,group);});
    for(const auto* invalid:{"0","N/A","-1","200001","95000oops"}) {
        write(sensor/"temp1_crit",invalid);rejects([&]{amd.thermalLimits(pci);});
    }
    write(sensor/"temp1_crit","95000");write(sensor/"temp1_label","junction");rejects([&]{amd.thermalLimits(pci);});
    write(sensor/"temp1_label","edge");write(sensor/"temp1_fault","1");rejects([&]{amd.thermalLimits(pci);});
    fs::remove(sensor/"temp1_fault");fs::remove(sensor/"temp1_emergency");check(amd.thermalLimits(pci)["limits"]["shutdownDeciC"].is_null());
    fs::rename(sensor,device/"hwmon/hwmon77");check(amd.thermalLimits(pci)["limits"]["criticalDeciC"]==950);
    std::cout<<checks<<" thermal limit and suggestion checks passed\n";
}
