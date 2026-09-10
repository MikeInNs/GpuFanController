#include "CurveGraph.hpp"
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <cmath>
#include <string>

namespace fan::prototype {
using namespace ftxui;
CurveGraph::CurveGraph(CurveModel& model,int& selected,std::function<void()> changed)
    :model_(model),selected_(selected),changed_(std::move(changed)) {}
void CurveGraph::setSize(int width,int height) {
    width=std::max(2,width); height=std::max(2,height);
    if(width!=width_ || height!=height_) { capture_.reset(); dragging_=false; }
    width_=width; height_=height;
}
PlotArea CurveGraph::area() const { return {box_.x_min,box_.y_min,box_.x_max-box_.x_min+1,box_.y_max-box_.y_min+1}; }
Element CurveGraph::OnRender() {
    Canvas drawing(width_*2,height_*4);
    PlotArea local{0,0,width_,height_};
    for(int tick=20;tick<=100;tick+=20) {
        const auto [x,y]=local.pixel({tick,0}); (void)y;
        drawing.DrawPointLine(x,0,x,height_*4-1,Color::GrayDark);
    }
    for(int rpm=0;rpm<=14000;rpm+=3500) {
        const auto [x,y]=local.pixel({20,rpm}); (void)x;
        drawing.DrawPointLine(0,y,width_*2-1,y,Color::GrayDark);
    }
    auto drawCurve=[&](const CurveModel::Points& points,Color shade) {
        auto previous=local.pixel({20,points[0].rpm});
        for(const auto& p:points) {
            const auto next=local.pixel(p);
            drawing.DrawPointLine(previous.first,previous.second,next.first,next.second,shade);
            previous=next;
        }
        drawing.DrawPointLine(previous.first,previous.second,width_*2-1,previous.second,shade);
    };
    drawCurve(model_.applied(),Color::GrayLight);
    drawCurve(model_.draft(),Color::Cyan);
    for(int i=0;i<4;++i) {
        const auto [x,y]=local.cell(model_.draft()[i]);
        drawing.DrawText(x*2,y*4,std::to_string(i+1),[i,this](Pixel& pixel) {
            pixel.foreground_color=i==selected_ ? Color::Black : Color::White;
            pixel.background_color=i==selected_ ? Color::Yellow : Color::Blue;
            pixel.bold=true;
        });
    }
    Elements labels;
    for(int row=0;row<height_;++row) {
        std::string label;
        for(int rpm=0;rpm<=14000;rpm+=3500) if(local.cell({20,rpm}).second==row) label=std::to_string(rpm);
        labels.push_back(text(std::string(6-label.size(),' ')+label+" ") | color(Color::GrayLight));
    }
    std::string temperatures(width_,' ');
    for(int tick=20;tick<=100;tick+=20) {
        auto column=local.cell({tick,0}).first;
        const auto value=std::to_string(tick);
        column=std::clamp(column-static_cast<int>(value.size())/2,0,width_-static_cast<int>(value.size()));
        temperatures.replace(column,value.size(),value);
    }
    return vbox({hbox({vbox(labels),canvas(std::move(drawing)) | reflect(box_)}) | border |
                     color(Focused()?Color::Cyan:Color::GrayLight),
                 text("        "+temperatures),text("        Temperature (C)     cyan: draft / grey: applied demo") | dim});
}
bool CurveGraph::OnEvent(Event event) {
    if(event.is_mouse()) {
        const auto& mouse=event.mouse();
        if(!dragging_ && !box_.Contain(mouse.x,mouse.y)) return false;
        ++mouseEvents;
        if(mouse.motion==Mouse::Released) {
            if(!dragging_) return false;
            // Preserve the point on a plain click: only motion events move it.
            dragging_=false; capture_.reset(); return true;
        }
        if(dragging_ && mouse.motion==Mouse::Moved) {
            if(mouse.button!=Mouse::Left) { dragging_=false; capture_.reset(); return false; }
            model_.move(selected_,area().point(mouse.x,mouse.y));
            ++dragMoves; changed_(); return true;
        }
        if(mouse.button==Mouse::Left && mouse.motion==Mouse::Pressed) {
            TakeFocus();
            int closest=-1, best=100000;
            for(int i=0;i<4;++i) {
                const auto [x,y]=area().cell(model_.draft()[i]);
                const int dx=std::abs(mouse.x-x),dy=std::abs(mouse.y-y);
                const int distance=dx*dx+4*dy*dy;
                if(dx<=2 && dy<=1 && distance<best) { closest=i; best=distance; }
            }
            if(closest>=0) {
                selected_=closest; changed_();
                capture_=CaptureMouse(event);
                dragging_=static_cast<bool>(capture_);
            }
            return true;
        }
        return false;
    }
    if(!Focused()) return false;
    if(event.is_character() && event.character().size()==1 && event.character()[0]>='1' && event.character()[0]<='4') {
        selected_=event.character()[0]-'1'; changed_(); return true;
    }
    Point proposed=model_.draft()[selected_];
    if(event==Event::ArrowLeft) --proposed.temperature;
    else if(event==Event::ArrowRight) ++proposed.temperature;
    else if(event==Event::ArrowUp) proposed.rpm+=100;
    else if(event==Event::ArrowDown) proposed.rpm-=100;
    else return false;
    model_.move(selected_,proposed); changed_(); return true;
}
}
