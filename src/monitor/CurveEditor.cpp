#include "CurveEditor.hpp"
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace fan {
using namespace ftxui;
void CurveEditor::load(const controller::Json& group, const controller::Json& cal) {
    applied_=draft_=group.at("curve"); range_=controller::rpmRange(cal,group); selected_=0; capture_.reset();
}
void CurveEditor::clear() { draft_=applied_=controller::Json::array(); range_=nullptr; capture_.reset(); }
void CurveEditor::setSize(int width,int height) {
    if(width!=width_ || height!=height_) capture_.reset();
    width_=std::max(12,width); height_=std::max(4,height);
}
bool CurveEditor::valid() const {
    if(!ready() || draft_.empty()) return false;
    for(const auto& p:draft_) if((p["rpm"]<range_["minimum"] && !(p["rpm"]==0 && range_.value("canStop",false))) || p["rpm"]>range_["maximum"] ||
        p["temperatureDeciC"]<0 || p["temperatureDeciC"]>1200) return false;
    return true;
}
void CurveEditor::fit() {
    if(!ready()) return;
    for(auto& p:draft_) if(!(p["rpm"]==0 && range_.value("canStop",false)))
        p["rpm"]=std::clamp(p["rpm"].get<int>(),range_["minimum"].get<int>(),range_["maximum"].get<int>());
}
std::pair<int,int> CurveEditor::cell(const controller::Json& p) const {
    const int lo=range_.value("canStop",false)?0:range_["minimum"].get<int>(),hi=range_["maximum"];
    return {std::clamp(p["temperatureDeciC"].get<int>()*(width_-1)/1200,0,width_-1),
        std::clamp((hi-p["rpm"].get<int>())*(height_-1)/(hi-lo),0,height_-1)};
}
void CurveEditor::move(int temperature,int rpm) {
    if(!ready()) return;
    int lowTemp=0, highTemp=1200, lowRpm=range_.value("canStop",false)?0:range_["minimum"].get<int>(), highRpm=range_["maximum"];
    const int runningMinimum=range_["minimum"];
    if(rpm>0 && rpm<runningMinimum) rpm=rpm<runningMinimum/2?0:runningMinimum;
    if(selected_>0) { lowTemp=draft_[selected_-1]["temperatureDeciC"].get<int>()+1; lowRpm=std::max(lowRpm,draft_[selected_-1]["rpm"].get<int>()); }
    if(selected_+1<static_cast<int>(draft_.size())) { highTemp=draft_[selected_+1]["temperatureDeciC"].get<int>()-1; highRpm=std::min(highRpm,draft_[selected_+1]["rpm"].get<int>()); }
    if(lowTemp>highTemp || lowRpm>highRpm) return; // Explicit Fit is needed for incompatible stored curves.
    draft_[selected_]["temperatureDeciC"]=std::clamp(temperature,lowTemp,highTemp);
    draft_[selected_]["rpm"]=std::clamp(rpm,lowRpm,highRpm);
}
Element CurveEditor::OnRender() {
    if(!ready()) return vbox({text("Calibration required")|bold|color(Color::Yellow),
        paragraph("No complete, usable measured RPM range for this group. Read the Nano, then check Calibration. No nominal RPM values are substituted.")});
    Canvas canvasImage(width_*2,height_*4);
    for(int tick=0;tick<=120;tick+=30) {
        const int x=tick*(width_-1)/120;
        canvasImage.DrawPointLine(x*2,0,x*2,height_*4-1,Color::GrayDark);
    }
    auto draw=[&](const auto& points,Color color) {
        auto prev=cell(points[0]); prev.first=0;
        for(const auto& p:points) { const auto next=cell(p); canvasImage.DrawPointLine(prev.first*2,prev.second*4,next.first*2,next.second*4,color); prev=next; }
        canvasImage.DrawPointLine(prev.first*2,prev.second*4,width_*2-1,prev.second*4,color);
    };
    draw(applied_,Color::GrayLight); draw(draft_,Color::Cyan);
    for(int i=0;i<static_cast<int>(draft_.size());++i) {
        const auto [x,y]=cell(draft_[i]);
        canvasImage.DrawText(x*2,y*4,std::to_string(i+1),[&,i](Pixel& p){p.foreground_color=Color::Black;p.background_color=i==selected_?Color::Yellow:Color::Cyan;});
    }
    const auto& p=draft_[selected_];
    Elements labels;
    for(int row=0;row<height_;++row) {
        std::string label;
        if(row==0) label=range_["maximum"].dump();
        if(row==height_-1) label=range_.value("canStop",false)?"0 STOP":range_["minimum"].dump();
        labels.push_back(text(std::string(6-label.size(),' ')+label+" ")|dim);
    }
    std::ostringstream temperature;temperature<<std::fixed<<std::setprecision(1)<<p["temperatureDeciC"].get<int>()/10.0;
    std::string axis(width_,' ');
    for(int tick=0;tick<=120;tick+=30) {
        const auto label=std::to_string(tick)+"C";
        const int x=std::clamp(tick*(width_-1)/120-static_cast<int>(label.size())/2,0,width_-static_cast<int>(label.size()));
        axis.replace(x,label.size(),label);
    }
    return vbox({text("Measured RPM: "+range_["minimum"].dump()+" - "+range_["maximum"].dump()+" (slower expected fan)"),
        hbox({vbox(labels),canvas(std::move(canvasImage))|reflect(box_)})|border,
        text("        "+axis)|dim,
        text("Point "+std::to_string(selected_+1)+": "+temperature.str()+" C / "+p["rpm"].dump()+" RPM")|color(Color::Yellow),
        text(valid()?"Drag points; 1-4 select; arrows: 0.1 C / 100 RPM":"Stored curve outside measured range: Fit to calibration before editing")|color(valid()?Color::GrayLight:Color::Yellow)});
}
bool CurveEditor::OnEvent(Event e) {
    if(!ready()) return false;
    if(e.is_mouse()) {
        const auto& m=e.mouse();
        if(m.motion==Mouse::Released && capture_) { capture_.reset(); return true; }
        if(m.motion==Mouse::Moved && capture_) {
            if(m.button!=Mouse::Left) { capture_.reset(); return false; }
            int lo=range_.value("canStop",false)?0:range_["minimum"].get<int>(),hi=range_["maximum"];
            move(static_cast<int>(std::lround((m.x-box_.x_min)*1200.0/(width_-1))),
                static_cast<int>(std::lround((hi-(m.y-box_.y_min)*(hi-lo)/static_cast<double>(height_-1))/10))*10);
            return true;
        }
        if(m.button!=Mouse::Left || m.motion!=Mouse::Pressed || !box_.Contain(m.x,m.y)) return false;
        TakeFocus();
        for(int i=0;i<static_cast<int>(draft_.size());++i) {
            auto [x,y]=cell(draft_[i]);
            if(std::abs(x+box_.x_min-m.x)<=2 && std::abs(y+box_.y_min-m.y)<=1) { selected_=i; capture_=CaptureMouse(e); break; }
        }
        return true;
    }
    if(!Focused()) return false;
    if(e.is_character() && e.character().size()==1 && e.character()[0]>='1' && e.character()[0]<'1'+static_cast<int>(draft_.size())) {
        selected_=e.character()[0]-'1'; return true;
    }
    int t=draft_[selected_]["temperatureDeciC"],rpm=draft_[selected_]["rpm"];
    if(e==Event::ArrowLeft) --t; else if(e==Event::ArrowRight) ++t;
    else if(e==Event::ArrowUp) rpm=(rpm==0 && range_.value("canStop",false))?range_["minimum"].get<int>():rpm+100;
    else if(e==Event::ArrowDown) rpm=(rpm==range_["minimum"] && range_.value("canStop",false))?0:rpm-100; else return false;
    move(t,rpm); return true;
}
}
