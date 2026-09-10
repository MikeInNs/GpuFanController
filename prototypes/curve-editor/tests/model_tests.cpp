#include "CurveModel.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#include <cmath>
using namespace fan::prototype;
void check(bool valid) { if(!valid) throw std::runtime_error("Curve model invariant failed"); }
int main() try {
    CurveModel model;
    check(!model.dirty());
    check(!model.setExact(1,{70,6500}));
    check(!model.setExact(1,{50,2400}));
    check(model.draft()==CurveModel::defaults);
    check(model.setExact(1,{55,7200}));
    check(model.dirty() && model.applied()==CurveModel::defaults);
    model.apply();check(!model.dirty());
    model.reset();check(model.dirty());
    std::mt19937 generator(1234);
    std::uniform_int_distribution<int> temperatures(-100,200),speeds(-10000,30000);
    for(int attempt=0;attempt<20000;++attempt) {
        model.move(attempt%4,{temperatures(generator),speeds(generator)});
        auto values=model.draft();
        for(int i=0;i<4;++i) {
            check(values[i].temperature>=20 && values[i].temperature<=100);
            check(values[i].rpm>=0 && values[i].rpm<=14000);
            if(i) check(values[i].temperature>values[i-1].temperature && values[i].rpm>=values[i-1].rpm);
        }
    }
    PlotArea area{7,5,81,15};
    check(area.point(-100,1000)==Point{20,0});
    check(area.point(1000,-100)==Point{100,14000});
    for(int temperature=20;temperature<=100;++temperature) {
        const auto [x,y]=area.cell({temperature,7000});
        check(area.point(x,y)==Point{temperature,7000});
    }
    std::cout<<"Curve bounds, monotonicity, draft/apply/reset and mouse projection passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
