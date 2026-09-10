#include "CurveModel.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fan::prototype {
Point CurveModel::constrain(std::size_t index, Point proposed) const {
    if(index>=draft_.size()) throw std::out_of_range("Curve point index");
    const int lowTemp=index==0 ? minTemperature : draft_[index-1].temperature+1;
    const int highTemp=index==3 ? maxTemperature : draft_[index+1].temperature-1;
    const int lowRpm=index==0 ? 0 : draft_[index-1].rpm;
    const int highRpm=index==3 ? maxRpm : draft_[index+1].rpm;
    return {std::clamp(proposed.temperature,lowTemp,highTemp),std::clamp(proposed.rpm,lowRpm,highRpm)};
}
void CurveModel::move(std::size_t index, Point proposed) { draft_.at(index)=constrain(index,proposed); }
bool CurveModel::setExact(std::size_t index, Point proposed) {
    if(constrain(index,proposed)!=proposed) return false;
    draft_.at(index)=proposed;
    return true;
}
std::pair<int,int> PlotArea::cell(Point p) const {
    const auto [px,py]=pixel(p);
    return {x+px/2,y+py/4};
}
std::pair<int,int> PlotArea::pixel(Point p) const {
    const int columns=std::max(1,width*2-1),rows=std::max(1,height*4-1);
    return {static_cast<int>(std::lround((p.temperature-CurveModel::minTemperature)*columns/80.0)),
            rows-static_cast<int>(std::lround(p.rpm*rows/static_cast<double>(CurveModel::maxRpm)))};
}
Point PlotArea::point(int mouseX,int mouseY) const {
    const int columns=std::max(1,width-1), rows=std::max(1,height-1);
    const int temperature=CurveModel::minTemperature+static_cast<int>(std::lround(std::clamp(mouseX-x,0,columns)*80.0/columns));
    const double rpm=(rows-std::clamp(mouseY-y,0,rows))*static_cast<double>(CurveModel::maxRpm)/rows;
    return {temperature,static_cast<int>(std::lround(rpm/100.0))*100};
}
}
