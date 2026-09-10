#pragma once
#include "MonitorModel.hpp"
#include <ftxui/component/component.hpp>
#include <array>

namespace fan {
class GroupEditor {
public:
    GroupEditor() {
        enabledToggle_=ftxui::Toggle(&enabledLabels_,&enabled_);
        fansToggle_=ftxui::Toggle(&fanLabels_,&fans_);
        durationToggle_=ftxui::Toggle(&durationLabels_,&duration_);
        controls_=ftxui::Maybe(ftxui::Container::Vertical({enabledToggle_,fansToggle_,durationToggle_}),[this]{return loaded_;});
    }
    void load(const Json& config,bool supported) {
        loaded_=supported;savedEnabled_=config.at("enabled");savedMask_=config.at("expectedFanMask");discard();
    }
    Json draftValues() const {return loaded_?Json{{"enabled",bool(enabled_)},{"expectedFanMask",masks_[fans_]}}:Json::object();}
    void restore(const Json& j) {if(loaded_) {enabled_=j.at("enabled").get<bool>()?1:0;const int mask=j.at("expectedFanMask");fans_=mask==3?0:mask==1?1:mask==2?2:3;}}
    void clear(){loaded_=false;}
    bool ready()const{return loaded_;}
    bool dirty()const{return loaded_ && (bool(enabled_)!=savedEnabled_ || masks_[fans_]!=savedMask_);}
    bool topologyChanged()const{return loaded_ && masks_[fans_]!=savedMask_;}
    bool disabling()const{return loaded_ && savedEnabled_ && !enabled_;}
    void discard(){enabled_=savedEnabled_?1:0;fans_=savedMask_==3?0:savedMask_==1?1:savedMask_==2?2:3;}
    int timeout()const{return durations_[duration_];}
    Json changes()const {
        if(!loaded_) throw std::invalid_argument("Read Nano with firmware 1.3.0 or newer first");
        if(enabled_ && !masks_[fans_]) throw std::invalid_argument("Enabled groups need at least one expected fan");
        Json result=Json::object();
        if(bool(enabled_)!=savedEnabled_) result["enabled"]=bool(enabled_);
        if(masks_[fans_]!=savedMask_) result["expectedFanMask"]=masks_[fans_];
        return result;
    }
    ftxui::Component component()const{return controls_;}
    ftxui::Element render(int group)const {
        using namespace ftxui;
        if(!loaded_) return paragraph("Read Nano first. Group controls require the updated daemon and firmware 1.3.0 (capability 0x20).");
        return vbox({text("Persistent Nano settings (draft until Save changes)")|bold,
            hbox({text("Group output: "),enabledToggle_->Render()}),
            hbox({text("Expected: "),fansToggle_->Render()}),
            text(group==0?"Fan 1 = D2; Fan 2 = D3":"Fan 1 = D5; Fan 2 = D4")|dim,
            hbox({text("Manual override timeout: "),durationToggle_->Render()}),
            paragraph("Expected fans changes monitoring, not individual outputs. Shared PWM drives both headers. A changed fan selection clears this group's calibration; recalibrate before editing its RPM curve.")|color(Color::Yellow)});
    }
private:
    bool loaded_=false,savedEnabled_=true;
    int savedMask_=3,enabled_=1,fans_=0,duration_=1;
    std::vector<std::string> enabledLabels_{"Disabled","Enabled"};
    std::vector<std::string> fanLabels_{"Both fans","Fan 1 only","Fan 2 only","None"};
    std::vector<std::string> durationLabels_{"1 min","5 min","15 min","60 min"};
    const std::array<int,4> masks_{3,1,2,0},durations_{60,300,900,3600};
    ftxui::Component controls_,enabledToggle_,fansToggle_,durationToggle_;
};
}
