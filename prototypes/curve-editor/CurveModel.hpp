#pragma once
#include <array>
#include <cstddef>
#include <utility>

namespace fan::prototype {
struct Point {
    int temperature;
    int rpm;
    bool operator==(const Point&) const = default;
};
class CurveModel {
public:
    static constexpr int minTemperature = 20, maxTemperature = 100, maxRpm = 14000;
    using Points = std::array<Point,4>;
    static constexpr Points defaults{{{30,2500},{50,6500},{70,9500},{85,12500}}};
    const Points& draft() const { return draft_; }
    const Points& applied() const { return applied_; }
    bool dirty() const { return draft_ != applied_; }
    void move(std::size_t index, Point proposed);
    bool setExact(std::size_t index, Point proposed);
    void apply() { applied_=draft_; }
    void reset() { draft_=defaults; }
private:
    Point constrain(std::size_t index, Point proposed) const;
    Points draft_=defaults;
    Points applied_=defaults;
};

// Mouse coordinates are character cells, not the graph's finer braille pixels.
struct PlotArea {
    int x=0, y=0, width=2, height=2;
    std::pair<int,int> cell(Point point) const;
    std::pair<int,int> pixel(Point point) const;
    Point point(int mouseX,int mouseY) const;
};
}
