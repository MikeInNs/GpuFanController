#include "TerminalUi.hpp"
#include "MonitorModel.hpp"
#include "ConfigurationDraft.hpp"
#include "SaveCoordinator.hpp"
#include "CurveEditor.hpp"
#include "ThresholdEditor.hpp"
#include "ThermalSuggestionForm.hpp"
#include "StatusView.hpp"
#include "LiveView.hpp"
#include "ScrollPage.hpp"
#include "CalibrationProgress.hpp"
#include "CalibrationWarning.hpp"
#include "AlertsView.hpp"
#include "GroupEditor.hpp"
#include "SettingsEditor.hpp"
#include "TabStyle.hpp"
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>
#include <chrono>
#include <algorithm>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>

namespace fan {
namespace {
using namespace ftxui;
using Clock=std::chrono::steady_clock;
ButtonOption buttonStyle() {
    auto option=ButtonOption::Simple();
    option.transform=[](const EntryState& s) { auto e=text(" "+s.label+" ")|color(Color::Cyan); return s.focused?e|inverted|bold:e; };
    return option;
}
class Application {
public:
    explicit Application(const ApiClient& client):client_(client),screen_(ScreenInteractive::Fullscreen()),
        live_([client](const std::string& id){return client.liveStatus(id);}) {
        graph_=std::make_shared<CurveEditor>();
        auto button=[&](const std::string& label,std::function<void()> action) {
            return Button(label,[this,action]{
                if(!pending_.valid()) guarded(action);
                else if(backgroundPoll_ && !queuedAction_) queuedAction_=action; // Preserve the first click and its selection intent.
            },buttonStyle());
        };
        scan_=button("Scan USB",[this]{confirm("A first connection can reset a Nano. Scan USB and GPUs?",[this]{
            captureEditors();syncHost();
            start([this]{return client_.discover();},[this](const Json& result){model_.merge(result);clampSelection();report_="Scan complete. Select a controller, register if needed, then map a GPU.";showDraft();});
        });});
        reload_=button("Reload",[this]{captureEditors();syncHost();reload();});
        quit_=button("Quit",[this]{quit();});
        previous_=button("< Controller",[this]{selectBoard(-1);});
        next_=button("Controller >",[this]{selectBoard(1);});
        group1_=button("Group 1",[this]{selectGroup(0);});
        group2_=button("Group 2",[this]{selectGroup(1);});
        read_=button("Read Nano",[this]{captureEditors();readNano();});
        register_=button("Register Nano",[this]{registerNano();});
        mapPrevious_=button("< GPU",[this]{mapGpu(-1);});
        mapNext_=button("GPU >",[this]{mapGpu(1);});
        fit_=button("Fit to calibration",[this]{graph_->fit();});
        discard_=button("Discard changes",[this]{discardChanges();});
        apply_=button("Save changes",[this]{saveChanges();});
        suggest_=button("Suggest settings from GPU",[this]{
            try {suggestSettings();} catch(const std::exception& e) {notice("Suggestions unavailable",e.what());}
        });
        ackGroup_=button("Ack group",[this]{acknowledge(group_==0?"group1":"group2");});
        ackGlobal_=button("Ack global",[this]{acknowledge("global");});
        ackAll_=button("Ack all",[this]{acknowledge("all");});
        startCalibration_=button("Start calibration",[this]{
            try {startCalibration();} catch(const std::exception& e) {notice("Calibration blocked",e.what());}
        });
        abortCalibration_=button("Abort calibration",[this]{abortCalibration();});
        autoMode_=button("Auto",[this]{setMode("auto");});
        fullMode_=button("Full speed",[this]{setMode("full");});
        offMode_=button("Off",[this]{setMode("off");});
        beeper_=button("Toggle beeper",[this]{
            if(!alertsApi_ || model_.config.is_null()) throw std::invalid_argument("Update/reload the daemon first");
            captureEditors();
            const bool enabled=!model_.config.value("notifications",Json::object()).value("criticalBeep",false);
            model_.config["notifications"]={{"criticalBeep",enabled}};syncHost();
            report_="Beeper preference is a draft; use Save changes to apply it.";
        });
        tabs_=Menu(&tabNames_,&tab_,mainTabStyle());
        auto toolbar=Container::Horizontal({scan_,reload_,quit_});
        auto selection=Container::Horizontal({previous_,next_,group1_,group2_,read_});
        auto mappingControls=Container::Horizontal({register_,mapPrevious_,mapNext_});
        auto mapping=Renderer(mappingControls,[this]{return mappingView();});
        statusTable_=std::make_shared<ScrollPage>([this]{
            if(!liveApi_) return statusView(snapshot_,group_);
            const auto target=selectedId();
            if(target.empty()) return text("Select a registered controller to monitor.");
            return liveStatusView(live_.sample(target),group_);
        });
        overviewTable_=std::make_shared<ScrollPage>([this]{return overviewView(
            model_.config.is_null()?Json::array():model_.config.value("controllers",Json::array()),live_,liveApi_);});
        auto curve=Container::Vertical({graph_,fit_});
        auto curvePage=Renderer(curve,[this]{
            graph_->setSize(std::min(Terminal::Size().dimx,120)-15,std::clamp(Terminal::Size().dimy-20,4,18));
            return vbox({graph_->Render(),fit_->Render()});
        });
        auto settingsTabs=Toggle(&settingsNames_,&settingsTab_);
        auto pwmPage=Renderer(pwmSettings_.component(),[this]{
            Elements rows{pwmSettings_.render()};
            if(!snapshot_.is_null()) {
                const auto& c=snapshot_["calibration"][group_];
                const int mask=snapshot_["configuration"]["groups"][group_]["expectedFanMask"];
                auto start=[&](int fan){const int duty=c["startDutyPercent"][fan];
                    return c["valid"]==true && (mask&(1<<fan)) && duty>0 && duty<100?std::to_string(duty)+"%":"N/A";};
                rows.push_back(paragraph("Measured startup (Fan 1 / Fan 2): "+start(0)+" / "+start(1)+
                    (c.value("measuredMinimum",false)?". Safe boost includes a 3-point margin; N/A uses conservative 100%.":". Reference only; supervise startup after changes."))|dim);
            }
            return vbox(rows);
        });
        auto supplyControls=supplySettings_.component();
        auto supplyPage=Renderer(supplyControls,[this]{return vbox({supplySettings_.render()});});
        auto settingsPages=Container::Tab({thresholds_.component(),pwmPage,supplyPage},&settingsTab_);
        auto thresholdsContainer=Container::Vertical({settingsTabs,settingsPages,suggest_});
        auto thresholds=Renderer(thresholdsContainer,[this,settingsTabs,settingsPages]{return vbox({settingsTabs->Render(),settingsPages->Render(),suggest_->Render()});});
        auto calibrationControls=Container::Horizontal({startCalibration_,abortCalibration_});
        auto calibrationTable=std::make_shared<ScrollPage>([this]{return calibrationView(snapshot_,group_);});
        calibrationTable_=calibrationTable;
        auto calibrationContainer=Container::Vertical({calibrationControls,calibrationTable});
        auto calibration=Renderer(calibrationContainer,[this,calibrationControls,calibrationTable]{
            return vbox({calibrationControls->Render(),calibrationProgressView(snapshot_),separator(),calibrationTable->Render()|vscroll_indicator|yframe|flex});
        });
        alertsTable_=std::make_shared<ScrollPage>([this]{return alertsView(alerts_,alertsError_);});
        auto alertButtons=Container::Horizontal({beeper_,ackGroup_,ackGlobal_,ackAll_});
        auto alertControls=Container::Vertical({alertButtons,alertsTable_});
        auto alertsPage=Renderer(alertControls,[this,alertButtons]{return vbox({alertButtons->Render(),
            text("Beeper preference draft: "+std::string(!model_.config.is_null() && model_.config.value("notifications",Json::object()).value("criticalBeep",false)?"ON":"OFF")+" (Save changes applies)"),
            alertsTable_->Render()|vscroll_indicator|yframe|flex});});
        auto modes=Container::Horizontal({autoMode_,fullMode_,offMode_});
        auto groups=Container::Vertical({groupEditor_.component(),modes});
        auto groupPage=Renderer(groups,[this,modes]{
            Elements rows{groupEditor_.render(group_)};
            if(groupEditor_.ready()) {
                rows.push_back(modes->Render());
                if(!snapshot_.is_null()) {
                    const auto& g=snapshot_["status"]["groups"][group_];
                    rows.push_back(text("Observed mode: "+modeName(g["mode"])+" | PWM: "+g["dutyPercent"].dump()+"% (Read Nano refreshes)"));
                }
                rows.push_back(paragraph("Modes are temporary and require confirmation. Off/Disabled request 0% PWM, not a 12V power cut. Host timeout overrides BOTH groups to 100%. Disabled groups skip local thermal/tach protection. Host GPU mappings are unchanged.")|color(Color::Yellow));
            }
            return vbox(rows);
        });
        pages_=Container::Tab({mapping,statusTable_,curvePage,thresholds,calibration,alertsPage,groupPage,overviewTable_},&tab_);
        auto draftControls=Container::Horizontal({discard_,apply_});
        auto main=Container::Vertical({toolbar,selection,tabs_,pages_,draftControls});
        auto mainRender=Renderer(main,[this,toolbar,selection,draftControls]{
            captureEditors();syncHost();
            Elements rows{hbox({text("GPU FAN CONTROLLER")|bold|color(Color::Cyan),filler(),toolbar->Render()}),separator(),
                text(selectedTitle())|bold,selection->Render(),tabs_->Render()|color(Color::White)|bgcolor(Color::GrayDark),
                ((tab_==4 || tab_==5)?pages_->Render()|flex:pages_->Render()|vscroll_indicator|yframe|flex),separator(),draftControls->Render(),
                text(draftLabel())|color(anyDirty()?Color::Yellow:Color::GrayLight),
                paragraph(!alertsError_.empty()?"ALERT FEED STALE - see Alerts":!alerts_.is_null() && alerts_.value("criticalActive",false)?"CRITICAL FAULT - see Alerts":pending_.valid()?(backgroundPoll_?"Refreshing status...":"Working... please wait (USB scan can take 40 seconds)."):report_)|color(!alertsError_.empty() || (!alerts_.is_null() && alerts_.value("criticalActive",false))?Color::Red:Color::White)|size(HEIGHT,EQUAL,2),
                text("Mouse / Tab / Enter | Esc quit | Nano owns fan control and alarms")|dim};
            return vbox(rows)|size(WIDTH,EQUAL,std::min(Terminal::Size().dimx,120)-2)|border;
        });
        auto confirmStyle=buttonStyle();
        confirmStyle.transform=[this](const EntryState& s) {
            auto e=text(" "+s.label+" ")|color(confirmLabel_=="Override & start"?Color::Red:Color::Cyan);
            return s.focused?e|inverted|bold:e;
        };
        auto yes=Button(&confirmLabel_,[this]{auto action=confirmation_;modal_=0;confirmation_={};guarded(action);},confirmStyle);
        auto no=Button(&cancelLabel_,[this]{modal_=0;confirmation_={};},buttonStyle());
        cancel_=no;
        auto dialogButtons=Container::Horizontal({no,Maybe(yes,[this]{return bool(confirmation_);})});
        auto dialogText=std::make_shared<ScrollPage>([this]{return paragraph(question_);});
        dialogText_=dialogText;
        auto dialogBody=Container::Vertical({dialogText,dialogButtons});
        auto dialog=Renderer(dialogBody,[this,dialogButtons,dialogText]{
            return vbox({text(dialogTitle_)|bold|color(dialogTitle_=="Please confirm"?Color::Yellow:Color::Red),
                dialogText->Render()|vscroll_indicator|yframe|flex,separator(),dialogButtons->Render()})
                |size(WIDTH,LESS_THAN,68)|size(HEIGHT,LESS_THAN,std::max(8,Terminal::Size().dimy-4))|border|center;
        });
        suggestionForm_=std::make_unique<ThermalSuggestionForm>([this]{modal_=0;},[this]{previewSuggestion();});
        auto layers=Container::Tab({mainRender,dialog,suggestionForm_->component()},&modal_);
        root_=Renderer(layers,[this,layers]{
            const auto size=Terminal::Size();
            if(!modal_ && (size.dimx<76 || size.dimy<24)) return vbox({text("GPU FAN CONTROLLER"),text("Resize terminal to at least 76 x 24. Esc quits.")});
            return layers->Render();
        });
        root_=CatchEvent(root_,[this](Event event){
            poll(event==Event::Custom);
            if(event==Event::Custom) return true;
            if(event==Event::Escape) {if(modal_) {modal_=0;confirmation_={};} else quit();return true;}
            if(event==Event::Character('q') && !modal_ && tab_!=3) {quit();return true;}
            if(pending_.valid() && (!backgroundPoll_ || !event.is_mouse())) return true;
            if(modal_==1 && event==Event::Return && (cancel_->Focused() || dialogText_->Focused())) {
                modal_=0;confirmation_={};return true;
            }
            if(modal_==1 && (event==Event::Home || event==Event::End)) {
                dialogText_->TakeFocus();return dialogText_->OnEvent(event);
            }
            if(!modal_ && tab_==4 && (event==Event::Home || event==Event::End)) {
                calibrationTable_->TakeFocus();return calibrationTable_->OnEvent(event);
            }
            if(!modal_ && tab_==5 && (event==Event::Home || event==Event::End)) {
                alertsTable_->TakeFocus();return alertsTable_->OnEvent(event);
            }
            if(!modal_ && (tab_==1 || tab_==7) && (event==Event::Home || event==Event::End)) {
                auto table=tab_==1?statusTable_:overviewTable_;
                table->TakeFocus();return table->OnEvent(event);
            }
            if(!modal_ && (Terminal::Size().dimx<76 || Terminal::Size().dimy<24)) return true;
            return false;
        });
    }
    int run() {
        reload();
        std::jthread ticker([this](std::stop_token stop){while(!stop.stop_requested()) {std::this_thread::sleep_for(std::chrono::milliseconds(100));screen_.PostEvent(Event::Custom);}});
        screen_.Loop(root_);
        return 0;
    }
private:
    const ApiClient& client_;
    ScreenInteractive screen_;
    LiveMonitor live_;
    bool liveApi_=false;
    Component statusTable_,overviewTable_;
    MonitorModel model_;
    ConfigurationDraft draft_;
    std::vector<DraftChange> unconfirmed_;
    std::map<std::string,Clock::time_point> observedAt_;
    std::shared_ptr<CurveEditor> graph_;
    ThresholdEditor thresholds_;
    Component suggest_;
    std::unique_ptr<ThermalSuggestionForm> suggestionForm_;
    Json suggestionContext_;
    GroupEditor groupEditor_;
    SettingsEditor pwmSettings_{false},supplySettings_{true};
    bool remainingSettingsApi_=false;
    std::vector<std::string> settingsNames_{"Temperature / RPM","PWM startup","Supply voltage"};
    int settingsTab_=0;
    Component ackGroup_,ackGlobal_,ackAll_;
    bool groupControlsApi_=false;
    Component autoMode_,fullMode_,offMode_;
    Json snapshot_=nullptr;
    Clock::time_point sampled_{};
    Clock::time_point nextCalibrationPoll_{};
    std::future<Json> pending_;
    std::function<void(const Json&)> completed_;
    std::function<void(const std::exception&)> failed_;
    std::function<void()> queuedAction_;
    std::function<void()> confirmation_;
    std::vector<std::string> tabNames_{"Setup","Status","Curve","Thresholds","Calibration","Alerts","Groups","Overview"};
    Json alerts_=nullptr;
    bool alertsApi_=false,alertPoll_=false;
    std::string alertsError_;
    Clock::time_point nextAlertsPoll_{};
    Component alertsTable_,beeper_;
    int board_=0,group_=0,tab_=0,modal_=0;
    bool uiApi_=false,quitAfterWork_=false,forwardingEnabled_=false;
    bool calibrationApi_=false,refreshCalibration_=false;
    bool backgroundPoll_=false;
    std::string report_="Connecting...",question_;
    std::string dialogTitle_="Please confirm",confirmLabel_="Confirm",cancelLabel_="Cancel";
    Component root_,pages_,tabs_,scan_,reload_,quit_,previous_,next_,group1_,group2_,read_,register_,mapPrevious_,mapNext_,fit_,discard_,apply_,cancel_;
    Component startCalibration_,abortCalibration_;
    Component calibrationTable_;
    Component dialogText_;

    void guarded(const std::function<void()>& f) {try{if(f) f();}catch(const std::exception& e){report_=e.what();}}
    bool groupDirty() const {return graph_->dirty() || thresholds_.dirty() || groupEditor_.dirty() || pwmSettings_.dirty();}
    bool nanoDirty() const {return groupDirty() || supplySettings_.dirty() || draft_.dirtyControllers()!=0 || !unconfirmed_.empty();}
    bool anyDirty() const {return model_.dirty || draft_.dirty() || nanoDirty();}
    void confirm(const std::string& question,std::function<void()> action,
                 const std::string& title="Please confirm",const std::string& label="Confirm") {
        question_=question;confirmation_=std::move(action);dialogTitle_=title;confirmLabel_=label;
        cancelLabel_=confirmation_?"Cancel":"Close";modal_=1;
        dialogText_->TakeFocus();dialogText_->OnEvent(Event::Home);
        cancel_->TakeFocus(); // Always default to cancellation, including risk overrides.
    }
    void notice(const std::string& title,const std::string& message) {report_=message;confirm(message,{},title);}
    void start(std::function<Json()> work,std::function<void(const Json&)> done,
               std::function<void(const std::exception&)> failed={},bool background=false) {
        completed_=std::move(done);failed_=std::move(failed);backgroundPoll_=background;
        pending_=std::async(std::launch::async,std::move(work));
    }
    void poll(bool allowBackground) {
        std::vector<std::string> targets;
        if(liveApi_) {
            if(tab_==1) {auto target=selectedId();if(!target.empty()) targets.push_back(target);}
            else if(tab_==7 && !model_.config.is_null())
                for(const auto& c:model_.config.at("controllers"))
                    if(c.at("controllerId").is_string()) targets.push_back(c.at("controllerId"));
        }
        live_.tick(targets,allowBackground && !modal_ && !pending_.valid() && !quitAfterWork_);
        if(!pending_.valid()) {
            if(allowBackground && !modal_ && !snapshot_.is_null() && calibrationApi_) {
                if(refreshCalibration_) {
                    refreshCalibration_=false;
                    if(!draft_.hasNano(selectedId()) || !draft_.nanoDirty(selectedId())) readNano();
                    else report_="Calibration ended. Nano draft preserved; Read Nano after applying/discarding it to load results.";
                } else if(tab_==4 && Clock::now()>=nextCalibrationPoll_) {
                    nextCalibrationPoll_=Clock::now()+std::chrono::seconds(1);
                    guarded([this]{const auto target=id();start([this,target]{return client_.calibrationProgress(target);},
                        [this](const Json& result){updateCalibrationProgress(result);},{},true);});
                }
            }
            if(allowBackground && !pending_.valid() && !modal_ && alertsApi_ && Clock::now()>=nextAlertsPoll_) {
                nextAlertsPoll_=Clock::now()+std::chrono::seconds(2);alertPoll_=true;
                start([this]{return client_.alerts();},[this](const Json& result){alerts_=result;alertsError_.clear();},
                    [this](const std::exception& e){alertsError_=e.what();},true);
            }
            return;
        }
        if(pending_.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
        try { auto result=pending_.get(); completed_(result); }
        catch(const std::exception& e) {
            report_=e.what();
            if(failed_) guarded([&]{failed_(e);});
            else snapshot_=nullptr; // Preserve drafts; hide stale telemetry.
        }
        completed_={};failed_={};backgroundPoll_=false;
        if(!alertPoll_) nextCalibrationPoll_=Clock::now()+std::chrono::seconds(1);
        alertPoll_=false;
        auto queued=std::move(queuedAction_);queuedAction_={};
        if(quitAfterWork_) {quitAfterWork_=false;quit();}
        else if(!modal_) guarded(queued);
    }
    Board& board() {if(model_.boards.empty()) throw std::invalid_argument("Scan USB first: no controller selected");return model_.boards.at(board_);}
    std::string selectedId() const {
        if(model_.boards.empty()) return {};
        const auto& value=model_.boards.at(board_).config.at("controllerId");
        return value.is_string()?value.get<std::string>():std::string{};
    }
    std::string id() {
        auto& b=board();
        if(!b.online) throw std::invalid_argument("Controller not discovered; scan USB first");
        if(b.config["controllerId"].is_null()) throw std::invalid_argument("Register the Nano first");
        if(!uiApi_) throw std::invalid_argument("Installed daemon needs updating for Nano UI support; see build/install instructions");
        return b.config["controllerId"];
    }
    void clampSelection() {board_=std::clamp(board_,0,std::max(0,static_cast<int>(model_.boards.size())-1));}
    void clearSnapshot() {snapshot_=nullptr;graph_->clear();thresholds_.clear();groupEditor_.clear();pwmSettings_.clear();supplySettings_.clear();refreshCalibration_=false;}
    void syncHost() {
        if(model_.config.is_null()) return;
        draft_.editHost(model_.proposed());model_.dirty=draft_.hostDirty();
    }
    void captureEditors() {
        const auto target=selectedId();
        if(snapshot_.is_null() || target.empty() || !draft_.hasNano(target)) return;
        if(!graph_->points().empty()) draft_.editGroup(target,group_,"curve",graph_->points());
        for(const auto& form:{thresholds_.draftValues(),groupEditor_.draftValues(),pwmSettings_.draftValues()})
            for(const auto& [key,value]:form.items()) draft_.editGroup(target,group_,key,value);
        const auto supply=supplySettings_.draftValues();
        for(const auto& [key,value]:supply.items()) draft_.editSupply(target,key,value);
    }
    void showDraft() {
        const auto target=selectedId();clearSnapshot();
        if(target.empty() || !draft_.hasNano(target)) return;
        snapshot_=draft_.latestNano(target);sampled_=observedAt_.contains(target)?observedAt_.at(target):Clock::now();
        const auto& base=draft_.baselineNano(target);
        const auto& config=base.at("configuration");const auto& g=config.at("groups").at(group_);
        const auto& cal=base.at("calibration").at(group_);
        graph_->load(g,cal);graph_->restore(draft_.group(target,group_).at("curve"));
        thresholds_.load(g);thresholds_.restore(draft_.group(target,group_));
        groupEditor_.load(g,groupControlsApi_ && base.value("groupControlsAvailable",false));
        groupEditor_.restore(draft_.group(target,group_));
        pwmSettings_.load(g,remainingSettingsApi_ && base.value("groupControlsAvailable",false),cal);
        pwmSettings_.restore(draft_.group(target,group_));
        supplySettings_.load(config,remainingSettingsApi_ && base.value("groupControlsAvailable",false));
        supplySettings_.restore(draft_.supply(target));
    }
    void adopt(const Json& result) {
        const std::string target=result.at("controllerId");
        draft_.observeNano(target,result);observedAt_[target]=Clock::now();showDraft();
    }
    void reload() {
        start([this]{return Json{{"config",client_.config()},{"status",client_.status()}};},[this](const Json& result){
            model_.inventory["controllers"]=result["status"].value("controllers",Json::array());
            // Startup/reload reuses the daemon's discovery cache, without a scan.
            // Older daemons omit these fields; preserve any explicit UI scan then.
            if(result["status"].contains("gpus")) model_.inventory["gpus"]=result["status"]["gpus"];
            if(result["status"].contains("discoveryErrors")) model_.inventory["errors"]=result["status"]["discoveryErrors"];
            draft_.observeHost(result["config"]);model_.load(draft_.host());model_.dirty=draft_.hostDirty();clampSelection();clearSnapshot();
            uiApi_=result["status"].value("controllerUiVersion",0)>=1;
            calibrationApi_=result["status"].value("calibrationApiVersion",0)>=1;
            groupControlsApi_=result["status"].value("groupControlsApiVersion",0)>=1;
            liveApi_=result["status"].value("liveStatusApiVersion",0)>=1;
            remainingSettingsApi_=result["status"].value("remainingSettingsApiVersion",0)>=1;
            alertsApi_=result["status"].value("alertsApiVersion",0)>=1;
            alerts_=result["status"].value("alerts",Json(nullptr));alertsError_.clear();
            forwardingEnabled_=result["status"].value("temperatureForwarding",Json::object()).value("enabled",false);
            showDraft();report_="Saved mappings loaded. Pending edits preserved; Save changes reviews all destinations.";
            if(!uiApi_) report_="Saved mappings loaded. Update the daemon to enable Nano status, curves and thresholds.";
        });
    }
    void readNano() {
        const auto target=id(); captureEditors();clearSnapshot();
        start([this,target]{return client_.snapshot(target);},[this](const Json& result){adopt(result);report_="Nano snapshot loaded. RPM curve bounds come only from usable stored calibration.";});
    }
    void selectBoard(int delta) {
        if(model_.boards.empty()) return;
        captureEditors();syncHost();
        const int count=static_cast<int>(model_.boards.size());board_=(board_+delta+count)%count;showDraft();
    }
    void selectGroup(int next) {
        if(next==group_) return;
        captureEditors();group_=next;showDraft();
    }
    void mapGpu(int delta) {
        auto& b=board();const auto current=b.config["groups"][group_]["gpuPciAddress"];
        std::vector<Json> choices{nullptr};
        for(const auto& g:model_.inventory["gpus"]) {
            const auto pci=g["pciAddress"];bool assigned=false;
            for(const auto& other:model_.boards) for(const auto& mapped:other.config["groups"])
                if(mapped["gpuPciAddress"]==pci && pci!=current) assigned=true;
            if(!assigned) choices.push_back(pci);
        }
        auto found=std::find(choices.begin(),choices.end(),current);
        int index=found==choices.end()?0:static_cast<int>(found-choices.begin());
        const int count=static_cast<int>(choices.size()); index=(index+delta+count)%count;
        model_.map(board_,group_,choices[index]);report_="Mapping draft changed. Save changes reviews daemon and Nano settings together.";
    }
    void registerNano() {
        auto& b=board();
        if(!b.online || !b.config["controllerId"].is_null()) throw std::invalid_argument("Select an online, unregistered Nano");
        const auto path=b.path;
        confirm("Write a permanent controller ID to this Nano's EEPROM?",[this,path]{
            start([this,path]{return client_.claim(path);},[this](const Json& result){
                board().config["controllerId"]=result["controllerId"];
                for(auto& c:model_.inventory["controllers"]) if(c["path"]==result["path"]) c=result;
                model_.dirty=true;report_="ID stored on Nano. Map the groups, then Save changes.";
            });
        });
    }
    void discardChanges() {
        captureEditors();syncHost();
        confirm("Discard all pending edits across the daemon and every controller? This does NOT undo saved or unconfirmed writes. If a previous write was unconfirmed, persistence remains unknown; read/review the hardware before relying on it.",[this]{
            draft_.discardAll();unconfirmed_.clear();model_.load(draft_.host());showDraft();
            report_="Pending changes discarded. Hardware was not changed.";
        });
    }
    void saveChanges() {
        captureEditors();syncHost();
        if(model_.config.is_null()) throw std::invalid_argument("Load daemon configuration first");
        auto plan=std::make_shared<SavePlan>();const auto input=draft_;const auto inventory=model_.inventory;const auto retry=unconfirmed_;
        start([this,plan,input,inventory,retry]{
            *plan=SaveCoordinator(SaveIo::api(client_)).prepare(input,inventory,retry);return Json::object();
        },[this,plan](const Json&){
            draft_=plan->draft;model_.load(draft_.host());model_.dirty=draft_.hostDirty();showDraft();
            if(plan->operations.empty()) {report_="No changes to save.";return;}
            confirm(plan->review,[this,plan]{
                auto result=std::make_shared<SaveResult>();
                start([this,plan,result]{*result=SaveCoordinator(SaveIo::api(client_)).execute(*plan);return Json::object();},
                    [this,result](const Json&){
                        draft_=result->draft;unconfirmed_=result->unconfirmed;model_.load(draft_.host());model_.dirty=draft_.hostDirty();
                        showDraft();nextAlertsPoll_={};notice("Save results",result->report);
                    },[this](const std::exception& e){notice("Save not completed",e.what());});
            },"Review changes");
        },[this](const std::exception& e){notice("Save blocked",e.what());});
    }
    void suggestSettings() {
        captureEditors();syncHost();
        if(!unconfirmed_.empty()) throw std::invalid_argument("Resolve unconfirmed saves before requesting suggestions");
        const auto target=id();const int selectedGroup=group_;
        const auto mapping=board().config.at("groups").at(group_);
        if(!mapping.at("enabled").get<bool>() || !mapping.at("gpuPciAddress").is_string())
            throw std::invalid_argument("Map a GPU to this group first (the mapping may remain a draft)");
        const std::string pci=mapping.at("gpuPciAddress");
        start([this,target,pci] {return Json{{"host",client_.config()},{"snapshot",client_.snapshot(target)},
            {"metadata",client_.thermalLimits(pci)}};},
            [this,target,selectedGroup,mapping,pci](const Json& result) {
                if(selectedId()!=target || group_!=selectedGroup || board().config.at("groups").at(group_)!=mapping)
                    throw std::invalid_argument("Selection changed; request suggestions again");
                if(result.at("host")!=draft_.latestHost()) throw std::invalid_argument("Daemon mappings changed; Reload and review before requesting suggestions");
                if(result.at("snapshot").at("controllerId")!=target || result.at("metadata").at("pciAddress")!=pci)
                    throw std::invalid_argument("Suggestion identity mismatch");
                if(result.at("snapshot").at("status").value("calibrationActive",false))
                    throw std::invalid_argument("Wait for calibration to finish before requesting suggestions");
                adopt(result.at("snapshot"));
                for(const auto& conflict:draft_.conflicts()) if(conflict.controllerId.empty() || conflict.controllerId==target)
                    throw std::invalid_argument("Settings changed; resolve the draft conflict before requesting suggestions");
                suggestionContext_=result;
                suggestionContext_["controllerId"]=target;suggestionContext_["group"]=selectedGroup;
                suggestionContext_["mapping"]=mapping;suggestionContext_["desired"]=draft_.group(target,group_);
                modal_=2;
                const auto& metadata=result.at("metadata");
                const bool amd=metadata.at("vendor")=="AMD";
                suggestionForm_->open("Nano "+target+" / group "+std::to_string(group_+1)+"\nGPU "+pci+
                    (amd?" / AMD edge (experimental)":" / NVIDIA core"));
            },[this](const std::exception& e){notice("Suggestions unavailable",std::string(e.what())+"\nUse updated host binaries; no settings were changed.");});
    }
    void previewSuggestion() {
        try {
            const auto context=suggestionContext_;const int g=context.at("group");
            const auto margins=suggestionForm_->margins();const bool curve=suggestionForm_->includeCurve();
            const auto proposal=thermalSuggestion::build(context.at("metadata"),context.at("snapshot").at("configuration"),
                context.at("snapshot").at("calibration").at(g),g,context.at("desired"),margins,curve);
            const std::string target=context.at("controllerId"),pci=context.at("metadata").at("pciAddress");
            confirm("Nano "+target+" / group "+std::to_string(g+1)+" / GPU "+pci+"\n\n"+proposal.review,
                [this,context,proposal,target,pci,g] {
                    // Recheck after review: external mapping/calibration/limit changes invalidate this proposal.
                    start([this,target,pci]{return Json{{"host",client_.config()},{"snapshot",client_.snapshot(target)},
                        {"metadata",client_.thermalLimits(pci)}};},
                        [this,context,proposal,target,g](const Json& fresh) {
                            const auto& s=fresh.at("snapshot");const auto& old=context.at("snapshot");
                            if(selectedId()!=target || group_!=g || board().config.at("groups").at(g)!=context.at("mapping") ||
                               draft_.group(target,g)!=context.at("desired") || fresh.at("host")!=context.at("host") ||
                               fresh.at("metadata")!=context.at("metadata") || s.at("controllerId")!=target ||
                               s.at("configuration")!=old.at("configuration") || s.at("calibration")!=old.at("calibration") ||
                               s.at("status").value("calibrationActive",false))
                                throw std::invalid_argument("Mapping, settings, calibration or GPU limits changed. Request a fresh proposal.");
                            for(const auto& [key,value]:proposal.changes.items()) draft_.editGroup(target,g,key,value);
                            showDraft();report_="Suggestions added to this group's draft. Review and Save changes to apply them.";
                        },[this](const std::exception& e){notice("Proposal not accepted",std::string(e.what())+"\nNo suggested settings were added or saved.");});
                },"Review GPU suggestions","Accept to draft");
        } catch(const std::exception& e) {suggestionForm_->error(e.what());}
    }
    void acknowledge(const std::string& scope) {
        const auto target=id();
        if(!remainingSettingsApi_) throw std::invalid_argument("Update/reload the daemon to acknowledge Nano latches");
        confirm("Acknowledge and clear "+scope+" latched alerts on Nano "+target+"? This removes stored fault indications in that scope. Active faults, protection and beeping remain; active conditions may immediately re-latch. Daemon history and other controllers are unchanged.",
            [this,target,scope]{start([this,target,scope]{return client_.acknowledgeLatched(target,{{"confirmed",true},{"scope",scope}});},
                [this,target](const Json& result){
                    if(result.at("controllerId")!=target || result.at("acknowledged")!=true) throw std::runtime_error("Latch acknowledgement identity/result mismatch");
                    const auto& s=result.at("status");
                    if(!snapshot_.is_null()) {snapshot_["status"]=s;sampled_=Clock::now();}
                    nextAlertsPoll_={};
                    notice("Latch acknowledgement", "Nano acknowledged the clear request. Readback latched masks: global "+s["latchedFaults"].dump()+", group 1 "+s["groups"][0]["latchedFaults"].dump()+", group 2 "+s["groups"][1]["latchedFaults"].dump()+".\nNonzero masks can be active/re-latched faults or unselected scopes. Active protection and history are unchanged. Status shows current conditions.");
                },[this](const std::exception& e){notice("Latch clear not confirmed",std::string(e.what())+"\nThe command may have executed. Inspect Status before retrying. No automatic retry is sent.");});});
    }
    static std::string modeName(int mode) {
        const std::vector<std::string> names{"Disabled","Auto","Manual","Full speed","Off","Calibration","Failsafe","ALARM"};
        return mode>=0 && mode<static_cast<int>(names.size())?names[mode]:"Unknown";
    }
    void setMode(const std::string& mode) {
        const auto target=id();const int group=group_;
        if(!groupEditor_.ready() || snapshot_.is_null()) throw std::invalid_argument("Read Nano with firmware 1.3.0 or newer first");
        if(nanoDirty()) throw std::invalid_argument("Save or discard pending changes before changing mode");
        if(!snapshot_["configuration"]["groups"][group]["enabled"].get<bool>()) throw std::invalid_argument("Enable the Nano group and Save changes before selecting a mode");
        if(snapshot_["status"]["calibrationActive"].get<bool>()) throw std::invalid_argument("Abort or finish calibration before changing mode");
        const int seconds=mode=="auto"?0:groupEditor_.timeout();
        Json request{{"confirmed",true},{"configGeneration",snapshot_["configuration"]["generation"]},{"mode",mode},{"timeoutSeconds",seconds}};
        std::string warning="Request "+mode+" for Group "+std::to_string(group+1)+"? ";
        warning+=mode=="auto"?"The Nano resumes its temperature/RPM curve (with startup boost).":"This temporary override lasts "+std::to_string(seconds)+" seconds, then returns to Auto. Closing the utility does not cancel it.";
        if(mode=="off") warning+=" WARNING: reduced cooling can overheat the GPU. Only use Off with an idle/unneeded GPU and supervise it. This requests 0% PWM, not a 12V power cut; some fans may keep spinning.";
        warning+=" Nano invalid/critical temperature protection and host-timeout alarm still override the request. No settings or GPU mappings are saved.";
        confirm(warning,[this,target,group,request]{
            start([this,target,group,request]{return client_.setGroupMode(target,group,request);},[this,group](const Json& result){
                snapshot_["status"]=result.at("status");sampled_=Clock::now();
                const auto& state=result.at("status").at("groups").at(group);
                report_="Mode request acknowledged. Observed "+modeName(state.at("mode"))+", PWM "+state.at("dutyPercent").dump()+"%. Safety/startup may override; Read Nano to refresh.";
            },[this](const std::exception& e){snapshot_=nullptr;notice("Mode not confirmed",std::string(e.what())+"\nRead Nano before retrying. A lost reply may mean the mode changed; no automatic retry is sent.");});
        });
    }
    void updateCalibrationProgress(const Json& result) {
        if(snapshot_.is_null()) return;
        if(result.at("controllerId")!=snapshot_.at("controllerId")) throw std::runtime_error("Calibration controller mismatch");
        const auto before=snapshot_.at("status");
        snapshot_["status"]=result.at("status");sampled_=Clock::now();
        const auto& now=snapshot_["status"];
        if(!now.at("calibrationActive").get<bool>() &&
           (before.at("calibrationActive").get<bool>() || before.at("calibrationPhase")!=now.at("calibrationPhase"))) {
            refreshCalibration_=true;
            report_=now.value("calibrationStorageConfirmed",Json(nullptr))==true?
                "Nano confirmed EEPROM storage. Reading the saved results; curve unchanged.":
                "Calibration ended without verified storage success. Reading preserved/current results; curve unchanged.";
        }
    }
    void startCalibration() {
        const auto target=id();
        if(!calibrationApi_) throw std::invalid_argument("Update the daemon to enable calibration controls");
        if(snapshot_.is_null()) throw std::invalid_argument("Read Nano first");
        if(anyDirty()) throw std::invalid_argument("Save changes or discard pending edits before calibration");
        if(snapshot_["status"]["calibrationActive"].get<bool>()) throw std::invalid_argument("Calibration already active; use Abort if needed");
        const int group=group_;
        Json request{{"confirmed",true},{"configGeneration",snapshot_["configuration"]["generation"]},
            {"calibrationGeneration",snapshot_["calibration"][group]["generation"]}};
        const bool hardened=snapshot_["status"].value("calibrationHardened",false);
        const bool measured=snapshot_["status"].value("calibratedMinimumSupported",false);
        const std::string safety=measured?
            " The Nano repeats starts from rest, verifies the running floor, then records nine points over the usable range (8s settling plus 3 stable samples each). Allow several minutes, up to 12 minutes. Successful verified storage also applies calibrated minimum/startup PWM and 5000 ms boost to this group. Failure preserves the old table AND settings. Zero-RPM fan-stop is never enabled automatically.":hardened?
            " The Nano supervises stability, measurements and time limits. Each sweep point settles for 8s then averages 3 stable samples. Allow about 2-3 minutes (5 minute limit). Previous calibration is preserved until EEPROM storage is verified.":
            " WARNING: legacy firmware can discard RAM calibration on abort and cannot confirm storage. Upgrade to firmware 1.4.0 for preservation and supervision.";
        confirm("Calibrate Group "+std::to_string(group+1)+"? Keep the GPU idle and fan 12V power on. This sweeps PWM, briefly stops fans, and tests startup. The Nano watchdog remains active."+safety,
            [this,target,group,request]{submitCalibration(target,group,request);});
    }
    void submitCalibration(const std::string& target,int group,const Json& request) {
        start([this,target,group,request]{return client_.startCalibration(target,group,request);},
            [this](const Json& result){updateCalibrationProgress(result);tab_=4;report_="Start acknowledged. The Nano controls calibration; do not load the GPU.";},
            [this,target,group,request](const std::exception& error){
                const auto* api=dynamic_cast<const ApiError*>(&error);
                if(api && api->statusCode==409 && api->details.value("code","")=="calibration_blocked") {
                    const auto warning=calibrationWarning(api->details.at("safety"));
                    const auto message="Controller "+target+", Group "+std::to_string(group+1)+"\n"+warning.message;
                    if(warning.canOverride) {
                        auto retry=request;
                        auto codes=request.value("overrideWarnings",Json::array());
                        for(const auto& code:warning.overrides) if(std::find(codes.begin(),codes.end(),code)==codes.end()) codes.push_back(code);
                        retry["overrideWarnings"]=codes;
                        confirm(message,[this,target,group,retry]{submitCalibration(target,group,retry);},"Calibration unsafe","Override & start");
                    } else notice("Calibration blocked",message);
                } else {
                    snapshot_=nullptr;
                    notice("Calibration not confirmed",std::string(error.what())+
                        "\nRead Nano to verify state before trying again. A lost reply can mean calibration started. No automatic retry or override is available for this error.");
                }
            });
    }
    void abortCalibration() {
        const auto target=id();
        if(!calibrationApi_) throw std::invalid_argument("Update the daemon to enable calibration controls");
        confirm("Abort the active calibration on this controller (either group)? The Nano determines the resulting fan mode.",
            [this,target]{start([this,target]{return client_.abortCalibration(target);},[this](const Json& result){
                updateCalibrationProgress(result);refreshCalibration_=!snapshot_.is_null();report_="Abort acknowledged. Reading Nano state to verify.";
            });});
    }
    void quit() {
        if(pending_.valid()) {quitAfterWork_=true;return;}
        if(anyDirty()) confirm("Unsaved edits. Discard drafts and quit?",[this]{screen_.ExitLoopClosure()();});
        else screen_.ExitLoopClosure()();
    }
    std::string selectedTitle() const {
        if(tab_==7) return "Overview - all saved controllers";
        if(model_.boards.empty()) return "No controller selected";
        const auto& b=model_.boards[board_];
        return b.config["name"].get<std::string>()+" ("+std::to_string(board_+1)+"/"+std::to_string(model_.boards.size())+")  | Group "+std::to_string(group_+1)+" | "+(b.online?b.path:"not discovered");
    }
    std::string draftLabel() const {
        if(draft_.host().is_null()) return "Daemon configuration unavailable";
        std::string result=draft_.dirty() || !unconfirmed_.empty()?"Unsaved changes: ":"All changes saved";
        if(draft_.hostDirty()) result+="daemon ";
        if(draft_.dirtyControllers()) result+=std::to_string(draft_.dirtyControllers())+" controller(s)";
        if(!unconfirmed_.empty()) result+=" | "+std::to_string(unconfirmed_.size())+" unconfirmed write(s)";
        if(!snapshot_.is_null()) result+=" | Snapshot "+std::to_string(std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-sampled_).count())+"s ago";
        return result;
    }
    Element mappingView() {
        Elements rows{text("Discover devices > Register identity > Assign GPUs > Save")|bold};
        if(!model_.boards.empty()) {
            const auto& b=model_.boards[board_];
            rows.push_back(text("ID: "+(b.config["controllerId"].is_null()?"unregistered":b.config["controllerId"].get<std::string>())+"  Firmware: "+b.firmware));
            for(int g=0;g<2;++g) {
                const auto& pci=b.config["groups"][g]["gpuPciAddress"];
                std::string label="Group "+std::to_string(g+1)+": "+(pci.is_null()?"none (disabled host mapping)":pci.get<std::string>());
                bool named=false;
                for(const auto& gpu:model_.inventory["gpus"]) if(gpu["pciAddress"]==pci) {
                    const auto name=gpu.value("name",std::string{});
                    if(name.empty()) continue;
                    label+=" / "+name;named=true;
                    if(gpu.value("vendor",std::string{})=="AMD") {
                        label+=" [AMD experimental; GPU edge";
                        label+=gpu.value("temperatureAvailable",false)?"; readable at scan]":"; UNAVAILABLE at scan]";
                    }
                }
                if(!snapshot_.is_null() && snapshot_["configuration"].contains("adapterNames")) {
                    const auto& stored=snapshot_["configuration"]["adapterNames"]["groups"][g];
                    if(!stored["pciAddress"].is_null()) {
                        const bool matches=stored["pciAddress"]==pci;
                        const auto name=stored["name"].get<std::string>();
                        if(matches && !named && !name.empty()) {label+=" / "+name+" (Nano)";named=true;}
                        if(!matches) label+=" [stored Nano label belongs to another mapping]";
                    }
                }
                if(!pci.is_null() && !named) label+=" / name unavailable (Scan USB or Read Nano)";
                rows.push_back(paragraph(label)|color(g==group_?Color::Cyan:Color::White));
            }
        }
        rows.push_back(hbox({register_->Render(),mapPrevious_->Render(),mapNext_->Render()}));
        rows.push_back(separator());
        rows.push_back(paragraph(forwardingEnabled_
            ? "Saved mappings feed real GPU temperatures to the Nano. Unmapped groups receive invalid temperatures, not an off command. Nano enable flags are unchanged."
            : "Temperature forwarding is disabled or unavailable in this daemon. Host mappings do not switch fan outputs off; the Nano watchdog can hold fans at 100%.")|color(Color::Yellow));
        if(!model_.inventory["errors"].empty()) rows.push_back(paragraph("Scan warning: "+model_.inventory["errors"][0].get<std::string>())|color(Color::Yellow));
        return vbox(rows);
    }
};
}
int TerminalUi::run() {return Application(client_).run();}
}
