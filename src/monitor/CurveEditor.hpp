#pragma once
#include "ControllerData.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>
namespace fan {
// Real Nano configuration + measured calibration; no offline example values.
class CurveEditor : public ftxui::ComponentBase {
public:
    void load(const controller::Json& group, const controller::Json& calibration);
    void restore(const controller::Json& points) {draft_=points;capture_.reset();}
    void clear();
    bool ready() const { return !range_.is_null(); }
    bool dirty() const { return draft_!=applied_; }
    bool valid() const;
    void fit();
    void discard() { draft_=applied_; capture_.reset(); }
    controller::Json points() const { return draft_; }
    controller::Json range() const { return range_; }
    void setSize(int width, int height);
    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override { return ready(); }
private:
    controller::Json draft_=controller::Json::array(), applied_=controller::Json::array(), range_=nullptr;
    int selected_=0, width_=60, height_=10;
    ftxui::Box box_;
    ftxui::CapturedMouse capture_;
    std::pair<int,int> cell(const controller::Json& point) const;
    void move(int temperature, int rpm);
};
}
