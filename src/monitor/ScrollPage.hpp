#pragma once
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <algorithm>
#include <functional>
namespace fan {
// Keep the frame's scroll target without asking FTXUI to move the terminal
// cursor to an off-screen row when a read-only document is scrolled.
class ScrollTarget : public ftxui::Node {
public:
    explicit ScrollTarget(ftxui::Element child):Node({std::move(child)}) {}
    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_=children_[0]->requirement();
        requirement_.focused.enabled=false;
    }
    void SetBox(ftxui::Box box) override {Node::SetBox(box);children_[0]->SetBox(box);}
};
// Read-only tables still need a focusable scroll target in a 76x24 SSH terminal.
class ScrollPage : public ftxui::ComponentBase {
public:
    explicit ScrollPage(std::function<ftxui::Element()> render):render_(std::move(render)) {}
    bool Focusable() const override {return true;}
    ftxui::Element OnRender() override {
        return std::make_shared<ScrollTarget>(render_()|ftxui::focusPositionRelative(0,position_))|ftxui::reflect(box_);
    }
    bool OnEvent(ftxui::Event event) override {
        using namespace ftxui;
        if(event.is_mouse()) {
            auto m=event.mouse();
            if(!box_.Contain(m.x,m.y)) return false;
            if(m.button==Mouse::WheelUp) {TakeFocus();position_=std::max(0.f,position_-.1f);return true;}
            if(m.button==Mouse::WheelDown) {TakeFocus();position_=std::min(1.f,position_+.1f);return true;}
            if(m.button==Mouse::Left && m.motion==Mouse::Pressed) {TakeFocus();return true;}
        }
        if(!Focused()) return false;
        if(event==Event::ArrowUp || event==Event::PageUp) position_=std::max(0.f,position_-.1f);
        else if(event==Event::ArrowDown || event==Event::PageDown) position_=std::min(1.f,position_+.1f);
        else if(event==Event::Home) position_=0;
        else if(event==Event::End) position_=1;
        else return false;
        return true;
    }
private:
    std::function<ftxui::Element()> render_;
    float position_=0;
    ftxui::Box box_;
};
}
