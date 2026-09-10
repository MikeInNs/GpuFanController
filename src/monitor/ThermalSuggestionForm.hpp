#pragma once
#include "ThermalSuggestion.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <array>
#include <charconv>

namespace fan {
// Short, keyboard/mouse-accessible setup dialog. Its state is not persisted.
class ThermalSuggestionForm {
public:
    ThermalSuggestionForm(std::function<void()> cancel,std::function<void()> preview) {
        using namespace ftxui;
        Components fields;
        for(auto& value:values_) {
            auto option=InputOption::Default();option.multiline=false;
            fields.push_back(Input(&value,"degrees C",option));
        }
        inputs_=fields;
        auto check=Checkbox("Include final curve point (requires calibration)",&curve_);
        auto buttons=ButtonOption::Simple();
        buttons.transform=[](const EntryState& s) {
            auto e=text(" "+s.label+" ")|color(Color::Cyan);
            return s.focused?e|inverted|bold:e;
        };
        cancel_=Button("Cancel",std::move(cancel),buttons);
        auto next=Button("Preview suggestions",std::move(preview),buttons);
        auto actions=Container::Horizontal({cancel_,next});
        fields.push_back(check);fields.push_back(actions);
        auto body=Container::Vertical(fields);
        component_=Renderer(body,[this,check,actions] {
            Elements rows{text("Suggest settings from GPU")|bold|color(Color::Cyan),paragraph(context_),separator()};
            const std::array<const char*,4> labels{"Full-speed margin","Warning margin","Critical margin","Target margin (NVIDIA only)"};
            for(std::size_t i=0;i<inputs_.size();++i)
                rows.push_back(hbox({text(labels[i])|size(WIDTH,EQUAL,29),inputs_[i]->Render()|size(WIDTH,EQUAL,8)|bgcolor(Color::GrayDark),text(" C")}));
            rows.push_back(check->Render());
            rows.push_back(paragraph("Margins below reported limits; starting suggestions only. Preview validates values. Nothing is saved here.")|color(Color::Yellow));
            if(!error_.empty()) rows.push_back(paragraph(error_)|color(Color::Red));
            rows.push_back(separator());rows.push_back(actions->Render());
            return vbox(rows)|size(WIDTH,LESS_THAN,70)|size(HEIGHT,LESS_THAN,Terminal::Size().dimy-2)|border|center;
        });
    }
    void open(const std::string& context) {
        context_=context;values_={"15","10","5","10"};curve_=false;error_.clear();cancel_->TakeFocus();
    }
    thermalSuggestion::Margins margins() const {
        std::array<int,4> n{};
        for(std::size_t i=0;i<n.size();++i) {
            const auto& s=values_[i];const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),n[i]);
            if(error!=std::errc{} || end!=s.data()+s.size() || n[i]<1 || n[i]>100)
                throw std::invalid_argument("Each margin must be a whole number from 1 to 100 C.");
        }
        return {n[0],n[1],n[2],n[3]};
    }
    bool includeCurve() const {return curve_;}
    void error(const std::string& error) {error_=error.substr(0,220);}
    ftxui::Component component() const {return component_;}
private:
    std::array<std::string,4> values_{"15","10","5","10"};
    bool curve_=false;
    std::string context_,error_;
    ftxui::Components inputs_;
    ftxui::Component component_,cancel_;
};
}
