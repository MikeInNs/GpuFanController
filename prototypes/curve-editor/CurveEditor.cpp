#include "CurveEditor.hpp"
#include <ftxui/component/component_options.hpp>
#include <ftxui/screen/terminal.hpp>
#include <charconv>
#include <algorithm>
#include <sstream>

namespace fan::prototype {
using namespace ftxui;
namespace {
ButtonOption buttonStyle() {
    ButtonOption option;
    option.transform=[](const EntryState& state) {
        auto element=text("["+state.label+"]") | color(Color::Cyan);
        if(state.focused) element=element | inverted | bold;
        return element;
    };
    return option;
}
bool integer(const std::string& value,int& result) {
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),result);
    return error==std::errc{} && end==value.data()+value.size();
}
}
CurveEditor::CurveEditor(std::function<void()> quit) {
    syncFields();
    graph_=std::make_shared<CurveGraph>(model_,selected_,[this] {
        syncFields(); message_="Editing point "+std::to_string(selected_+1)+". Apply demo keeps changes in memory only.";
    });
    InputOption input;
    input.on_enter=[this] { updatePoint(); };
    temperatureInput_=Input(&temperature_,"C",input);
    rpmInput_=Input(&rpm_,"RPM",input);
    update_=Button("Update point",[this]{updatePoint();},buttonStyle());
    apply_=Button("Apply demo",[this] {
        if(!updatePoint()) return;
        model_.apply(); ++applyCount_;
        message_="Applied to DEMO memory only. No controller or file was changed.";
    },buttonStyle());
    reset_=Button("Reset defaults",[this] { model_.reset(); syncFields(); message_="Demo defaults restored to draft; Apply demo to accept."; },buttonStyle());
    quit_=Button("Quit",quit,buttonStyle());
    Components points;
    for(int i=0;i<4;++i) {
        pointButtons_[i]=Button(&pointLabels_[i],[this,i] {selected_=i;syncFields();graph_->TakeFocus();},buttonStyle());
        points.push_back(pointButtons_[i]);
    }
    auto controls=Container::Vertical({graph_,Container::Horizontal(points),
        Container::Horizontal({temperatureInput_,rpmInput_,update_}),Container::Horizontal({reset_,apply_,quit_})});
    component_=Renderer(controls,[this]{return render();});
    component_=CatchEvent(component_,[this,quit](Event event) {
        if(event==Event::Escape) {quit();return true;}
        return small_;
    });
    graph_->TakeFocus();
}
void CurveEditor::syncFields() {
    temperature_=std::to_string(model_.draft()[selected_].temperature);
    rpm_=std::to_string(model_.draft()[selected_].rpm);
}
bool CurveEditor::updatePoint() {
    Point proposed{};
    if(!integer(temperature_,proposed.temperature) || !integer(rpm_,proposed.rpm)) {
        message_="Invalid input: enter whole numbers for temperature and RPM."; return false;
    }
    if(!model_.setExact(selected_,proposed)) {
        message_="Invalid curve: 20-100 C, 0-14000 RPM; temperatures increase, RPM cannot decrease.";
        return false;
    }
    syncFields(); message_="Point updated in draft. Nothing has been sent to hardware."; return true;
}
Element CurveEditor::render() {
    const auto terminal=Terminal::Size();
    const int width=width_?width_:terminal.dimx, height=height_?height_:terminal.dimy;
    small_=width<76 || height<24;
    if(small_) return vbox({text("FAN CURVE - OFFLINE PROTOTYPE") | bold,
        paragraph("Resize terminal to at least 76 columns x 24 rows. Escape quits. No hardware is connected.")}) | border;
    graph_->setSize(std::min(width,120)-13,std::clamp(height-18,6,24));
    Elements selectors{text("Select: ")};
    for(int i=0;i<4;++i) {
        pointLabels_[i]=std::to_string(i+1)+":"+std::to_string(model_.draft()[i].temperature)+"C/"+std::to_string(model_.draft()[i].rpm);
        auto button=pointButtons_[i]->Render() | reflect(pointBoxes_[i]);
        if(i==selected_) button=button | color(Color::Yellow) | bold;
        selectors.push_back(button); selectors.push_back(text("  "));
    }
    const bool pendingFields=temperature_!=std::to_string(model_.draft()[selected_].temperature) || rpm_!=std::to_string(model_.draft()[selected_].rpm);
    auto state=text(model_.dirty() || pendingFields ? " DRAFT CHANGES " : " DEMO APPLIED ") |
        color(Color::Black) | bgcolor(model_.dirty() || pendingFields?Color::Yellow:Color::Cyan);
    return vbox({
        hbox({text(" FAN CURVE ") | bold | color(Color::Cyan),filler(),state}),
        text(" OFFLINE PROTOTYPE | example data | no daemon, Nano or saved config access") | color(Color::Yellow),
        separator(),
        hbox({text(" Target RPM"),filler(),text("Demo range: 20-100 C / 0-14000 RPM ") | dim}),
        graph_->Render(),
        hbox(selectors),
        hbox({text("Point "+std::to_string(selected_+1)+"   Temperature: "),temperatureInput_->Render() | size(WIDTH,EQUAL,6) | bgcolor(Color::GrayDark) | reflect(temperatureBox_),
            text(" C    RPM: "),rpmInput_->Render() | size(WIDTH,EQUAL,7) | bgcolor(Color::GrayDark) | reflect(rpmBox_),text("  "),update_->Render() | reflect(updateBox_)}),
        separator(),
        hbox({reset_->Render() | reflect(resetBox_),text("   "),apply_->Render() | reflect(applyBox_),filler(),quit_->Render() | reflect(quitBox_)}),
        paragraph(message_) | color(Color::Yellow) | size(HEIGHT,EQUAL,2),
        text(" Tab: controls | Graph: 1-4 select, arrows adjust | Esc: quit") | dim,
        text(" Mouse events: "+std::to_string(graph_->mouseEvents)+"   Drag moves: "+std::to_string(graph_->dragMoves)+"   Demo changes disappear on exit.") | dim
    }) | size(WIDTH,EQUAL,std::min(width,120)-2) | border;
}
std::string CurveEditor::report() const {
    std::ostringstream out;
    auto points=[&](const auto& values) {
        out<<'[';
        for(std::size_t i=0;i<values.size();++i) {if(i) out<<',';out<<'['<<values[i].temperature<<','<<values[i].rpm<<']';}
        out<<']';
    };
    auto box=[&](const Box& b) {out<<'['<<b.x_min<<','<<b.y_min<<','<<b.x_max<<','<<b.y_max<<']';};
    out<<"{\"selected\":"<<selected_<<",\"draft\":";points(model_.draft());
    out<<",\"applied\":";points(model_.applied());
    out<<",\"dirty\":"<<(model_.dirty()?"true":"false")<<",\"applyCount\":"<<applyCount_;
    out<<",\"mouseEvents\":"<<graph_->mouseEvents<<",\"dragMoves\":"<<graph_->dragMoves;
    const auto area=graph_->area();
    out<<",\"plot\":["<<area.x<<','<<area.y<<','<<area.width<<','<<area.height<<"],\"pointCells\":[";
    for(int i=0;i<4;++i) {if(i) out<<',';const auto [x,y]=area.cell(model_.draft()[i]);out<<'['<<x<<','<<y<<']';}
    out<<"],\"buttons\":{\"apply\":";box(applyBox_);out<<",\"reset\":";box(resetBox_);
    out<<",\"quit\":";box(quitBox_);out<<",\"update\":";box(updateBox_);out<<"},\"fields\":{\"temperature\":";
    box(temperatureBox_);out<<",\"rpm\":";box(rpmBox_);out<<"},\"pointButtons\":[";
    for(int i=0;i<4;++i) {if(i) out<<',';box(pointBoxes_[i]);}
    out<<"]}";return out.str();
}
}
