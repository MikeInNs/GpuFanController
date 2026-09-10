#pragma once
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

namespace fan {
inline ftxui::MenuOption mainTabStyle() {
    auto option=ftxui::MenuOption::Toggle();
    option.entries_option.transform=[](const ftxui::EntryState& state) {
        auto entry=ftxui::text(state.label)|ftxui::color(ftxui::Color::White);
        // Keep the bar's background and white text in every state: no dim/invert.
        if(state.active) entry=entry|ftxui::bold;
        if(state.focused) entry=entry|ftxui::underlined;
        return entry;
    };
    return option;
}
}
