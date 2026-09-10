#pragma once
#include "MonitorModel.hpp"
#include "DraftFields.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <array>
#include <charconv>
namespace fan {
// Small three-field forms share interaction, but keep separate scope/draft state.
class SettingsEditor {
public:
    explicit SettingsEditor(bool supply):supply_(supply),component_(std::make_shared<Form>(*this)) {}
    void load(const Json& config,bool available,const Json& calibration=nullptr) {
        if(!available) {clear();return;}
        measured_=!supply_ && !calibration.is_null() && calibration.value("measuredMinimum",false);
        minimum_=measured_?calibration.at("minimumRunningDutyPercent").get<int>():20;
        startup_=minimum_;
        if(measured_) for(int fan=0;fan<2;++fan) if(config.at("expectedFanMask").get<int>()&(1<<fan))
            startup_=std::max(startup_,std::min(100,calibration.at("startDutyPercent")[fan].get<int>()+3));
        const auto names=keys();
        for(int i=0;i<3;++i) fields_[i]=config.at(names[i]).dump();
        saved_=fields_;loaded_=true;replace_=true;
    }
    Json draftValues() const {Json j=Json::object();const auto k=keys();if(loaded_) for(int i=0;i<3;++i) j[k[i]]=draftNumber(fields_[i]);return j;}
    void restore(const Json& j) {const auto k=keys();if(loaded_) for(int i=0;i<3;++i) fields_[i]=draftText(j.at(k[i]));}
    void clear(){loaded_=false;fields_={};saved_=fields_;}
    bool dirty()const{return loaded_ && fields_!=saved_;}
    void discard(){fields_=saved_;replace_=true;}
    Json changes()const {
        if(!loaded_) throw std::invalid_argument("Read Nano with updated daemon and firmware 1.3.0 first");
        std::array<int,3> n{};
        for(int i=0;i<3;++i) {
            const auto [end,error]=std::from_chars(fields_[i].data(),fields_[i].data()+fields_[i].size(),n[i]);
            if(error!=std::errc{} || end!=fields_[i].data()+fields_[i].size()) throw std::invalid_argument("Settings must be whole numbers in the labelled units");
            if(n[i]<(supply_?6000:i==2?(measured_?5000:0):i==0?minimum_:startup_) || n[i]>(supply_?16000:i==2?10000:100)) throw std::invalid_argument("Setting outside labelled/calibrated range");
        }
        if(supply_ && !(n[0]<=n[1] && n[1]<n[2])) throw std::invalid_argument("Supply requires critical-low <= warning-low < high");
        if(!supply_ && n[1]<n[0]) throw std::invalid_argument("Startup PWM must be at least minimum PWM");
        Json result=Json::object();const auto names=keys();
        for(int i=0;i<3;++i) if(fields_[i]!=saved_[i]) result[names[i]]=n[i];
        return result;
    }
    ftxui::Component component()const{return component_;}
    ftxui::Element render() {
        using namespace ftxui;
        if(!loaded_) return paragraph("Read Nano with updated daemon and firmware 1.3.0 to edit these settings.")|dim;
        const std::array<std::string,3> labels=supply_?
            std::array<std::string,3>{"Critical-low supply (6000..16000 mV)","Warning-low supply (6000..16000 mV)","High supply (6000..16000 mV)"}:
            std::array<std::string,3>{"Minimum PWM ("+std::to_string(minimum_)+"..100%)","Startup PWM ("+std::to_string(startup_)+"..100%)",measured_?"Startup duration (5000..10000 ms)":"Startup duration (0..10000 ms)"};
        Elements rows{text(supply_?"Controller-wide supply thresholds - INA CH1":"Selected group PWM and startup settings")|bold,separator()};
        for(int i=0;i<3;++i) {
            auto field=text(fields_[i].empty()?"_":fields_[i])|size(WIDTH,EQUAL,9)|bgcolor(Color::GrayDark)|reflect(boxes_[i]);
            if(selected_==i && component_->Focused()) field=field|inverted|focus;
            rows.push_back(hbox({text(labels[i])|size(WIDTH,EQUAL,43),field}));
        }
        rows.push_back(separator());
        rows.push_back(paragraph(supply_?
            "1000 mV = 1 V. Critical-low <= warning-low < high. These thresholds protect the shared supply, not one group. Save changes includes these controller-wide settings.":
            measured_?"Calibration applied safe minimum/startup PWM and 5000 ms boost. Settings may raise these limits, not lower them. Zero RPM is an explicit curve stop request, never the running minimum.":
            "0 ms disables startup boost. Too little PWM or startup time can stall fans. Default: 20%, 100%, 1000 ms. Recalibrate with firmware 1.5.0 to measure safe PWM limits.")|color(Color::Yellow));
        return vbox(rows);
    }
private:
    std::array<std::string,3> keys()const {
        return supply_?std::array<std::string,3>{"voltageCriticalLowMv","voltageWarningLowMv","voltageHighMv"}:
            std::array<std::string,3>{"minimumDutyPercent","startupDutyPercent","startupTimeMs"};
    }
    class Form:public ftxui::ComponentBase {
    public:
        explicit Form(SettingsEditor& owner):owner_(owner){}
        bool Focusable()const override{return owner_.loaded_;}
        ftxui::Element OnRender()override{return owner_.render();}
        bool OnEvent(ftxui::Event event)override {
            using namespace ftxui;
            if(!owner_.loaded_) return false;
            if(event.is_mouse()) {
                const auto m=event.mouse();
                if(m.button==Mouse::Left && m.motion==Mouse::Pressed)
                    for(int i=0;i<3;++i) if(owner_.boxes_[i].Contain(m.x,m.y)) {TakeFocus();owner_.selected_=i;owner_.replace_=true;return true;}
                return false;
            }
            if(!Focused()) return false;
            auto& field=owner_.fields_[owner_.selected_];
            if(event==Event::ArrowUp || event==Event::TabReverse) {
                if(!owner_.selected_) return false;
                --owner_.selected_;owner_.replace_=true;return true;
            }
            if(event==Event::ArrowDown || event==Event::Tab) {
                if(owner_.selected_==2) return false;
                ++owner_.selected_;owner_.replace_=true;return true;
            }
            if(event==Event::End) {owner_.replace_=false;return true;}
            if(event==Event::Backspace || event==Event::Delete) {
                if(!field.empty()) field.pop_back();
                owner_.replace_=false;return true;
            }
            if(event.is_character() && event.character().size()==1 && event.character()[0]>='0' && event.character()[0]<='9') {
                if(owner_.replace_) field.clear();
                owner_.replace_=false;if(field.size()<5) field+=event.character();return true;
            }
            return false;
        }
    private:SettingsEditor& owner_;
    };
    bool supply_,loaded_=false,replace_=true,measured_=false;
    int minimum_=20,startup_=20;
    int selected_=0;
    std::array<std::string,3> fields_,saved_;
    std::array<ftxui::Box,3> boxes_;
    ftxui::Component component_;
};
}
