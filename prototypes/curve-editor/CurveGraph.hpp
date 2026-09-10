#pragma once
#include "CurveModel.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>
#include <functional>

namespace fan::prototype {
class CurveGraph : public ftxui::ComponentBase {
public:
    CurveGraph(CurveModel& model,int& selected,std::function<void()> changed);
    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override { return true; }
    void setSize(int width,int height);
    PlotArea area() const;
    int mouseEvents=0, dragMoves=0;
private:
    CurveModel& model_;
    int& selected_;
    std::function<void()> changed_;
    int width_=60, height_=10;
    ftxui::Box box_;
    ftxui::CapturedMouse capture_;
    bool dragging_=false;
};
}
