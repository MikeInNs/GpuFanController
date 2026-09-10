#pragma once
#include "MonitorModel.hpp"
#include "DraftFields.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <array>
#include <charconv>
namespace fan {
class ThresholdEditor {
public:
    ThresholdEditor():component_(std::make_shared<Form>(*this)) {}
    void load(const Json& group) {
        for(std::size_t i=0;i<keys_.size();++i) fields_[i]=group.at(keys_[i]).dump();
        saved_=fields_;loaded_=true;replace_=true;
    }
    Json draftValues() const {Json j=Json::object();if(loaded_) for(std::size_t i=0;i<keys_.size();++i) j[keys_[i]]=draftNumber(fields_[i]);return j;}
    void restore(const Json& j) {if(loaded_) for(std::size_t i=0;i<keys_.size();++i) fields_[i]=draftText(j.at(keys_[i]));}
    void clear() {loaded_=false;for(auto& f:fields_) f.clear();saved_=fields_;}
    bool dirty() const {return loaded_ && fields_!=saved_;}
    void discard() {fields_=saved_;replace_=true;}
    Json changes() const {
        if(!loaded_) throw std::invalid_argument("Read the Nano first");
        Json result=Json::object();
        for(std::size_t i=0;i<keys_.size();++i) {
            int value=0;auto [end,error]=std::from_chars(fields_[i].data(),fields_[i].data()+fields_[i].size(),value);
            if(error!=std::errc{} || end!=fields_[i].data()+fields_[i].size()) throw std::invalid_argument("Thresholds must be integers (temperatures in tenths C)");
            if(value<low_[i] || value>high_[i]) throw std::invalid_argument("Threshold outside labelled range");
            result[keys_[i]]=value;
        }
        if(result[keys_[0]]>=result[keys_[1]]) throw std::invalid_argument("Warning temperature must be below critical");
        return result;
    }
    ftxui::Component component() const {return component_;}
    ftxui::Element render() {
        using namespace ftxui;
        if(!loaded_) return text("Read the Nano to edit its alarm thresholds.")|dim;
        Elements rows{text("Nano alarm thresholds: click a value and type to replace it.")|bold,separator()};
        for(std::size_t i=0;i<fields_.size();++i) {
            auto field=text(fields_[i].empty()?"_":fields_[i])|size(WIDTH,EQUAL,9)|bgcolor(Color::GrayDark)|reflect(boxes_[i]);
            if(static_cast<int>(i)==selected_ && component_->Focused()) field=field|inverted|focus;
            rows.push_back(hbox({text(labels_[i])|size(WIDTH,EQUAL,43),field}));
        }
        rows.push_back(separator());
        rows.push_back(paragraph("750 = 75.0 C. Tab/arrows select fields; typing replaces the selected value. Backspace edits. Current deviation uses the group calibration.")|dim);
        return vbox(rows);
    }
private:
    class Form : public ftxui::ComponentBase {
    public:
        explicit Form(ThresholdEditor& owner):owner_(owner) {}
        bool Focusable() const override {return owner_.loaded_;}
        ftxui::Element OnRender() override {return owner_.render();}
        bool OnEvent(ftxui::Event event) override {
            using namespace ftxui;
            if(!owner_.loaded_) return false;
            if(event.is_mouse()) {
                const auto& m=event.mouse();
                if(m.button!=Mouse::Left || m.motion!=Mouse::Pressed) return false;
                for(int i=0;i<5;++i) if(owner_.boxes_[i].Contain(m.x,m.y)) {
                    TakeFocus();owner_.selected_=i;owner_.replace_=true;return true;
                }
                return false;
            }
            if(!Focused()) return false;
            auto& s=owner_.fields_[owner_.selected_];
            if(event==Event::ArrowUp || event==Event::TabReverse) {
                if(owner_.selected_==0) return false;
                --owner_.selected_;owner_.replace_=true;return true;
            }
            if(event==Event::ArrowDown || event==Event::Tab) {
                if(owner_.selected_==4) return false;
                ++owner_.selected_;owner_.replace_=true;return true;
            }
            if(event==Event::End) {owner_.replace_=false;return true;}
            if(event==Event::Backspace || event==Event::Delete) {
                if(!s.empty()) s.pop_back();
                owner_.replace_=false;return true;
            }
            if(event.is_character() && event.character().size()==1 && event.character()[0]>='0' && event.character()[0]<='9') {
                if(owner_.replace_) s.clear();
                owner_.replace_=false;
                if(s.size()<5) s+=event.character();
                return true;
            }
            return false;
        }
    private:ThresholdEditor& owner_;
    };
    const std::array<std::string,5> keys_{"warningTemperatureDeciC","criticalTemperatureDeciC","rpmLowThresholdPercent","faultDelaySeconds","currentDeviationPercent"};
    const std::array<std::string,5> labels_{"Warning temperature (0..1199 tenths C)","Critical temperature (1..1200 tenths C)","Low RPM threshold (20..95% of target)","Fault delay (1..30 seconds)","Current deviation (10..100%)"};
    const std::array<int,5> low_{0,1,20,1,10},high_{1199,1200,95,30,100};
    std::array<std::string,5> fields_,saved_;
    std::array<ftxui::Box,5> boxes_;
    int selected_=0;
    bool loaded_=false,replace_=true;
    ftxui::Component component_;
};
}
