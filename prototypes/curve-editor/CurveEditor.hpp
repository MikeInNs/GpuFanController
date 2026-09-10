#pragma once
#include "CurveGraph.hpp"
#include <ftxui/component/component.hpp>
#include <functional>
#include <string>

namespace fan::prototype {
class CurveEditor {
public:
    explicit CurveEditor(std::function<void()> quit);
    ftxui::Component component() const { return component_; }
    void fixedViewport(int width,int height) { width_=width; height_=height; }
    std::string report() const;
private:
    void syncFields();
    bool updatePoint();
    ftxui::Element render();
    CurveModel model_;
    int selected_=1, width_=0, height_=0, applyCount_=0;
    bool small_=false;
    std::string temperature_,rpm_,message_="Drag a numbered point, or select it and edit its values.";
    std::shared_ptr<CurveGraph> graph_;
    ftxui::Component component_,temperatureInput_,rpmInput_,update_,apply_,reset_,quit_;
    std::array<ftxui::Component,4> pointButtons_;
    std::array<std::string,4> pointLabels_;
    std::array<ftxui::Box,4> pointBoxes_;
    ftxui::Box temperatureBox_,rpmBox_,updateBox_,applyBox_,resetBox_,quitBox_;
};
}
