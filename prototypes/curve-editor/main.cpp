#include "CurveEditor.hpp"
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <charconv>
#include <string_view>
#include <unistd.h>

int main(int argc,char** argv) try {
    using namespace ftxui;
    bool report=false;
    if(argc==2 && std::string_view(argv[1])=="--help") {
        std::cout<<"Offline mouse/keyboard curve editor. NO hardware or file writes.\n"
            "Usage: fanctl-curve-prototype [--report-on-exit]\n"
            "       fanctl-curve-prototype --snapshot WIDTH HEIGHT\n"
            "       fanctl-curve-prototype --layout-json WIDTH HEIGHT\n";return 0;
    }
    if(argc==4 && (std::string_view(argv[1])=="--snapshot" || std::string_view(argv[1])=="--layout-json")) {
        int width=0,height=0;
        auto parse=[](const char* arg,int& value) {std::string_view s(arg);auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),value);return ec==std::errc{} && end==s.data()+s.size();};
        if(!parse(argv[2],width) || !parse(argv[3],height) || width<1 || height<1 || width>300 || height>100) return 2;
        fan::prototype::CurveEditor editor([]{});editor.fixedViewport(width,height);
        auto screen=Screen::Create(Dimension::Fixed(width),Dimension::Fixed(height));
        Render(screen,editor.component()->Render());
        std::cout<<(std::string_view(argv[1])=="--snapshot"?screen.ToString():editor.report())<<'\n';return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--report-on-exit") report=true;
    else if(argc!=1) return 2;
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {std::cerr<<"Run in an interactive terminal (SSH supported).\n";return 2;}
    auto screen=ScreenInteractive::Fullscreen();
    fan::prototype::CurveEditor editor(screen.ExitLoopClosure());
    screen.Loop(editor.component());
    if(report) std::cout<<"PROTOTYPE_RESULT "<<editor.report()<<'\n';
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
