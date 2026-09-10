#include "ControllerData.hpp"
#include "CurveEditor.hpp"
#include "MonitorModel.hpp"
#include "TabStyle.hpp"
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <stdexcept>
using fan::controller::Json;
void check(bool v) {if(!v) throw std::runtime_error("Controller data assertion failed");}
template<class F> void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
int main() {
    for(bool active:{false,true}) for(bool focused:{false,true}) {
        const auto style=fan::mainTabStyle();
        auto entry=style.entries_option.transform({"Groups",false,active,focused,0})|ftxui::bgcolor(ftxui::Color::GrayDark);
        auto screen=ftxui::Screen::Create(ftxui::Dimension::Fixed(6),ftxui::Dimension::Fixed(1));
        ftxui::Render(screen,entry);
        for(int x=0;x<6;++x) {
            const auto& pixel=screen.PixelAt(x,0);
            check(pixel.foreground_color==ftxui::Color::White && pixel.background_color==ftxui::Color::GrayDark);
            check(!pixel.dim && !pixel.inverted && pixel.bold==active && pixel.underlined==focused);
        }
    }
    using namespace fan::controller;
    std::vector<std::uint8_t> config(72,0);
    config[4]=0x10;config[5]=0x27;
    for(int g=0;g<2;++g) {
        const int o=12+30*g;config[o]=1;config[o+1]=3;config[o+2]=4;config[o+3]=20;config[o+4]=100;
        config[o+7]=0xee;config[o+8]=2;config[o+9]=0x52;config[o+10]=3;
        for(int i=0;i<4;++i) {int t=300+i*150,rpm=2500+i*2000;config[o+14+i*4]=t&255;config[o+15+i*4]=t>>8;config[o+16+i*4]=rpm&255;config[o+17+i*4]=rpm>>8;}
    }
    auto group=configuration(config)["groups"][0];
    Json cal{{"valid",true},{"startDutyPercent",{15,15}},{"points",Json::array()}};
    for(int duty=20;duty<=100;duty+=10) cal["points"].push_back({{"dutyPercent",duty},{"fanRpm",{duty*100,duty*90}}});
    check(rpmRange(cal,group)==Json({{"minimum",1800},{"maximum",9000}}));
    auto bad=cal;bad["valid"]=false;check(rpmRange(bad,group).is_null());
    bad=cal;bad["points"][4]["fanRpm"][0]=0;check(rpmRange(bad,group).is_null());
    bad=cal;bad["startDutyPercent"][1]=100;check(rpmRange(bad,group).is_null());
    auto single=group;single["expectedFanMask"]=1;
    check(rpmRange(cal,single)["maximum"]==10000);
    single["minimumDutyPercent"]=40;check(rpmRange(cal,single)["minimum"]==4000);
    auto edited=editConfiguration(config,0,{{"curve",group["curve"]}},cal);
    check(configuration(edited)["generation"]==1);
    check(std::equal(config.begin()+42,config.end(),edited.begin()+42));
    auto curve=group["curve"];curve[3]["rpm"]=9001;
    rejects([&]{editConfiguration(config,0,{{"curve",curve}},cal);});
    curve=group["curve"];curve[1]["temperatureDeciC"]=300;
    rejects([&]{editConfiguration(config,0,{{"curve",curve}},cal);});
    rejects([&]{editConfiguration(config,0,{{"curve",group["curve"]}},bad);});
    rejects([&]{editConfiguration(config,0,{{"faultDelaySeconds",1.5}},cal);});
    rejects([&]{editConfiguration(config,0,{{"warningTemperatureDeciC",900}},cal);});
    check(configuration(editConfiguration(config,0,{{"enabled",false}},cal))["groups"][0]["enabled"]==false);
    rejects([&]{editConfiguration(config,0,{{"enabled",0}},cal);});
    rejects([&]{editConfiguration(config,0,{{"expectedFanMask",0}},cal);});
    rejects([&]{editConfiguration(config,0,{{"expectedFanMask",4}},cal);});
    rejects([&]{editConfiguration(config,0,{{"expectedFanMask",1},{"curve",group["curve"]}},cal);});
    check(configuration(editConfiguration(config,1,{{"expectedFanMask",2}},cal))["groups"][1]["expectedFanMask"]==2);
    const auto controls=editConfiguration(config,1,{{"enabled",false},{"expectedFanMask",0}},cal);
    check(std::equal(config.begin()+4,config.begin()+42,controls.begin()+4));
    const auto pwm=editConfiguration(config,1,{{"minimumDutyPercent",40},{"startupDutyPercent",70},{"startupTimeMs",2000}},cal);
    check(std::equal(config.begin()+4,config.begin()+45,pwm.begin()+4));
    check(std::equal(config.begin()+49,config.end(),pwm.begin()+49));
    check(configuration(pwm)["groups"][1]["startupTimeMs"]==2000);
    check(rpmRange(cal,configuration(pwm)["groups"][1])["minimum"]==3600);
    for(const auto& edit:std::vector<Json>{{{"minimumDutyPercent",19}},{{"startupDutyPercent",101}},{{"startupTimeMs",10001}},
        {{"startupTimeMs",-1}},{{"startupTimeMs",true}},{{"minimumDutyPercent",80},{"startupDutyPercent",70}},
        {{"minimumDutyPercent",40},{"curve",group["curve"]}}}) rejects([&]{editConfiguration(config,0,edit,cal);});
    check(configuration(editConfiguration(config,0,{{"minimumDutyPercent",100},{"startupDutyPercent",100},{"startupTimeMs",0}},cal))["groups"][0]["startupTimeMs"]==0);
    const auto supply=editSupply(config,{{"voltageCriticalLowMv",10500},{"voltageWarningLowMv",11000},{"voltageHighMv",13200}});
    check(std::equal(config.begin()+12,config.end(),supply.begin()+12));
    check(supply[4]==config[4] && supply[5]==config[5]);
    check(configuration(supply)["generation"]==1);
    check(configuration(editSupply(supply,{{"voltageWarningLowMv",10500}}))["voltageWarningLowMv"]==10500);
    for(const auto& edit:std::vector<Json>{{{"voltageCriticalLowMv",5999}},{{"voltageHighMv",16001}},{{"voltageWarningLowMv",14000}},
        {{"voltageCriticalLowMv",12000}},{{"voltageHighMv",11000}},{{"voltageHighMv",true}},{{"hostUpdateTimeoutMs",5000}}})
        rejects([&]{editSupply(supply,edit);});
    for(int n=0;n<72;++n) rejects([&]{configuration(std::vector<std::uint8_t>(n));});
    for(int n=0;n<91;++n) rejects([&]{calibration(std::vector<std::uint8_t>(n));});
    for(int n=0;n<58;++n) rejects([&]{status(std::vector<std::uint8_t>(n));});
    std::vector<std::uint8_t> stat(58);stat[6]=64;
    const auto s=status(stat);check(s["groups"][0]["temperatureDeciC"].is_null());check(s["groups"][0]["currentMa"].is_null());
    fan::CurveEditor editor;check(!editor.ready());editor.load(group,cal);check(editor.ready() && editor.valid() && !editor.dirty());
    group["curve"][3]["rpm"]=12500;editor.load(group,cal);check(!editor.valid());editor.fit();check(editor.dirty() && editor.valid());
    check(editor.points()[3]["rpm"]==9000);editor.discard();check(!editor.dirty());editor.load(group,bad);check(!editor.ready());
    auto measured=cal;measured["measuredMinimum"]=true;measured["stopVerifiedMask"]=3;
    measured["minimumRunningDutyPercent"]=13;measured["startDutyPercent"]={15,25};measured["points"]=Json::array();
    for(int i=0;i<9;++i) {const int duty=100-87*i/8;measured["points"].push_back({{"dutyPercent",duty},{"fanRpm",{duty*100,duty*90}}});}
    auto lowConfig=config;lowConfig[15]=13;lowConfig[16]=28;lowConfig[17]=0x88;lowConfig[18]=0x13;
    const auto lowGroup=configuration(lowConfig)["groups"][0];
    check(rpmRange(measured,lowGroup)==Json({{"minimum",1170},{"maximum",9000},{"canStop",true}}));
    auto zeroCurve=lowGroup["curve"];zeroCurve[0]["rpm"]=0;
    check(configuration(editConfiguration(lowConfig,0,{{"curve",zeroCurve}},measured))["groups"][0]["curve"][0]["rpm"]==0);
    for(const auto& edit:std::vector<Json>{{{"minimumDutyPercent",12}},{{"startupDutyPercent",27}},{{"startupTimeMs",4999}}})
        rejects([&]{editConfiguration(lowConfig,0,edit,measured);});
    rejects([&]{editConfiguration(config,0,{{"curve",zeroCurve}},cal);});
    auto noStop=measured;noStop["stopVerifiedMask"]=1;
    rejects([&]{editConfiguration(lowConfig,0,{{"curve",zeroCurve}},noStop);});
    auto gap=zeroCurve;gap[0]["rpm"]=100;rejects([&]{editConfiguration(lowConfig,0,{{"curve",gap}},measured);});
    auto zeroGroup=lowGroup;zeroGroup["curve"]=zeroCurve;editor.load(zeroGroup,measured);check(editor.valid());
    editor.fit();check(editor.points()[0]["rpm"]==0);
    editor.TakeFocus();check(editor.OnEvent(ftxui::Event::ArrowUp));check(editor.points()[0]["rpm"]==1170);
    check(editor.OnEvent(ftxui::Event::ArrowDown));check(editor.points()[0]["rpm"]==0);
    editor.load(zeroGroup,noStop);check(!editor.valid());editor.fit();check(editor.points()[0]["rpm"]==1170);
    auto corrupt=measured;corrupt["points"][8]["dutyPercent"]=14;check(rpmRange(corrupt,lowGroup).is_null());
    fan::MonitorModel model; model.load({{"schemaVersion",1},{"revision",0},{"controllers",Json::array()}});
    model.merge({{"controllers",Json::array({{{"controllerId",std::string(32,'a')},{"path","/dev/ttyUSB0"},{"firmware","1.2.0"}}})},{"gpus",Json::array()},{"errors",Json::array()}});
    model.map(0,0,"0000:01:00.0");rejects([&]{model.map(0,1,"0000:01:00.0");});check(model.dirty);
    std::cout<<"Calibration-derived ranges, malformed payloads, scoped edits, draft semantics and mappings passed\n";
}
