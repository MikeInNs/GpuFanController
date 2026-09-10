#include "ApiServer.hpp"
#include "fan/Protocol.hpp"
#include "CalibrationControl.hpp"
#include "GroupControl.hpp"
#include "SettingsControl.hpp"
#include "AdapterNames.hpp"
#include "GpuThermalLimits.hpp"
#include <nlohmann/json.hpp>

namespace fan {
ApiServer::ApiServer(const std::filesystem::path& configPath,bool forwarding) : config_(configPath) {
    forwarding_=std::make_unique<TemperatureForwarder>(forwarding,
        [this](const std::string& id)->std::shared_ptr<SerialProbe> {
            std::unique_lock lock(mutex_,std::try_to_lock);
            if(!lock.owns_lock()) return nullptr;
            std::shared_ptr<SerialProbe> result;unsigned matches=0;
            for(const auto& c:inventory_["controllers"]) if(c["controllerId"]==id) {
                ++matches;result=discovery_.share(c["path"]);
            }
            if(matches>1) throw std::runtime_error("Duplicate controller identity; forwarding refused");
            return result;
        },
        [this](const auto& probe) {
            std::unique_lock lock(mutex_,std::try_to_lock);
            if(lock.owns_lock()) discovery_.disconnect(probe);
        },
        [this](std::stop_token stop) {
            std::unique_lock lock(mutex_,std::try_to_lock);
            if(lock.owns_lock() && !stop.stop_requested()) inventory_=discovery_.scan(stop);
        });
    server_.new_task_queue = [] { return new httplib::ThreadPool(2); };
    server_.set_read_timeout(2, 0);
    server_.set_write_timeout(2, 0);
    server_.set_payload_max_length(65536);
    server_.set_pre_routing_handler([](const auto& request, auto& response) {
        if ((request.method == "POST" || request.method == "PUT") &&
            (request.has_header("Origin") || request.get_header_value("Content-Type") != "application/json")) {
            response.status=403;
            response.set_content(R"({"error":"Local JSON clients only; browser writes are not allowed"})","application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server_.set_exception_handler([](const auto&, auto& response, std::exception_ptr exception) {
        try { std::rethrow_exception(exception); }
        catch (const calibration::SafetyRefusal& e) {
            response.status=409;
            response.set_content(nlohmann::json{{"error",e.what()},{"code","calibration_blocked"},{"safety",e.details}}.dump(),"application/json");
        }
        catch (const ConfigConflict& e) { response.status=409; response.set_content(nlohmann::json{{"error",e.what()}}.dump(),"application/json"); }
        catch (const std::invalid_argument& e) { response.status=400; response.set_content(nlohmann::json{{"error",e.what()}}.dump(),"application/json"); }
        catch (const nlohmann::json::exception& e) { response.status=400; response.set_content(nlohmann::json{{"error",e.what()}}.dump(),"application/json"); }
        catch (const std::exception& e) { response.status=500; response.set_content(nlohmann::json{{"error",e.what()}}.dump(),"application/json"); }
    });
    server_.Get("/api/v1/health", [this](const auto&, auto& response) {
        response.set_content(nlohmann::json{{"service", "gpu-fan-controller"},
            {"apiVersion", 1}, {"protocolVersion", protocol::version},
            {"status", "running"}, {"hardwareReady", forwarding_->status()["ready"]}}.dump(), "application/json");
    });
    server_.Get("/api/v1/status", [this](const auto&, auto& response) {
        std::lock_guard lock(mutex_);
        const auto forwarding=forwarding_->status();
        response.set_content(nlohmann::json{{"service", "gpu-fan-controller"},
            {"hardwareReady", forwarding["ready"]}, {"temperatureForwarding",forwarding}, {"controllerUiVersion",1}, {"calibrationApiVersion",1}, {"controllers", inventory_["controllers"]},
            {"gpus",inventory_["gpus"]},{"discoveryErrors",inventory_["errors"]},
            {"configuredControllers", config_.get()["controllers"].size()}, {"configPath",config_.path()},
            {"alertsApiVersion",1},{"alerts",forwarding_->alerts()},{"groupControlsApiVersion",1},{"liveStatusApiVersion",1},{"remainingSettingsApiVersion",1},{"calibrationHardeningApiVersion",1},
            {"message", forwarding["enabled"].get<bool>() ? "Temperature forwarding active; inspect per-controller responsiveness, validity and Nano faults." : "Temperature forwarding disabled (--no-forwarding)."}}
            .dump(), "application/json");
    });
    server_.Get(R"(/api/v1/gpus/([0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7])/thermal-limits)", [this](const auto& request,auto& response) {
        std::unique_lock lock(thermalMutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("GPU thermal query busy; retry shortly");
        response.set_content(GpuThermalLimits{}.read(request.matches[1].str()).dump(),"application/json");
    });
    server_.Get("/api/v1/alerts", [this](const auto&, auto& response) {
        response.set_content(forwarding_->alerts().dump(),"application/json");
    });
    server_.Get(R"(/api/v1/controllers/([0-9a-f]{32})/status)", [this](const auto& request,auto& response) {
        const std::string id=request.matches[1].str();
        std::shared_ptr<SerialProbe> probe;
        {
            std::unique_lock lock(mutex_,std::try_to_lock);
            if(!lock.owns_lock()) throw ConfigConflict("Hardware operation busy; live read skipped");
            unsigned matches=0;
            for(const auto& c:inventory_["controllers"]) if(c["controllerId"]==id) {++matches;probe=discovery_.share(c["path"]);}
            if(matches!=1 || !probe) throw ConfigConflict("Controller unavailable or duplicate identity");
        }
        // Read only the retained, discovery-verified connection. Never open/reset a
        // port here, and never hold the inventory lock during a slow serial read.
        response.set_content(nlohmann::json{{"controllerId",id},{"status",probe->liveStatus()}}.dump(),"application/json");
    });
    server_.Post("/api/v1/discovery", [this](const auto&, auto& response) {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("Hardware/config operation busy; retry shortly");
        inventory_=discovery_.scan();
        response.set_content(inventory_.dump(),"application/json");
    });
    auto remainingOperation=[this](const auto& request,auto& response,bool supply) {
        const auto body=nlohmann::json::parse(request.body);
        if(supply) settingsControl::validateSupply(body);else settingsControl::clearMask(body);
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("Controller operation busy; retry shortly");
        const std::string id=request.matches[1].str();
        std::shared_ptr<SerialProbe> probe;unsigned matches=0;
        for(const auto& c:inventory_["controllers"]) if(c["controllerId"]==id) {++matches;probe=discovery_.share(c["path"]);}
        if(matches!=1 || !probe) throw ConfigConflict("Select exactly one discovered registered controller");
        if(probe->hello()["controllerId"]!=id) throw ConfigConflict("Controller identity changed; scan again");
        auto result=supply?probe->updateSupply(body):probe->clearLatched(body);
        result["controllerId"]=id;
        response.set_content(result.dump(),"application/json");
    };
    server_.Put(R"(/api/v1/controllers/([0-9a-f]{32})/supply)",
        [remainingOperation](const auto& req,auto& res){remainingOperation(req,res,true);});
    server_.Post(R"(/api/v1/controllers/([0-9a-f]{32})/alerts/acknowledge)",
        [remainingOperation](const auto& req,auto& res){remainingOperation(req,res,false);});
    server_.Put(R"(/api/v1/controllers/([0-9a-f]{32})/adapter-names)", [this](const auto& request,auto& response) {
        const auto body=nlohmann::json::parse(request.body);adapterNames::validateRequest(body);
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("Controller operation busy; retry shortly");
        const std::string id=request.matches[1].str();
        std::shared_ptr<SerialProbe> probe;unsigned matches=0;
        for(const auto& c:inventory_["controllers"]) if(c["controllerId"]==id) {++matches;probe=discovery_.share(c["path"]);}
        if(matches!=1 || !probe) throw ConfigConflict("Select exactly one discovered registered controller");
        if(probe->hello()["controllerId"]!=id) throw ConfigConflict("Controller identity changed; scan again");
        auto result=probe->updateAdapterNames(body);result["controllerId"]=id;
        response.set_content(result.dump(),"application/json");
    });
    server_.Post("/api/v1/controllers/claim", [this](const auto& request, auto& response) {
        const auto body=nlohmann::json::parse(request.body);
        const auto path=body.at("path").template get<std::string>();
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("Hardware/config operation busy; retry shortly");
        auto found=std::find_if(inventory_["controllers"].begin(),inventory_["controllers"].end(),[&](const auto& c){return c["path"]==path;});
        if(found==inventory_["controllers"].end()) throw std::invalid_argument("Scan and select a responding controller before registration");
        *found=discovery_.claim(path);
        response.set_content(found->dump(),"application/json");
    });
    server_.Get("/api/v1/config", [this](const auto&, auto& response) {
        std::lock_guard lock(mutex_);
        response.set_content(config_.get().dump(),"application/json");
    });
    auto controllerOperation=[this](const auto& request, auto& response, bool update) {
        const auto body=update?nlohmann::json::parse(request.body):nlohmann::json::object();
        if(update) groupControl::validateEdit(body);
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("Controller operation busy; retry shortly");
        const auto id=request.matches[1].str();
        auto found=std::find_if(inventory_["controllers"].begin(),inventory_["controllers"].end(),[&](const auto& c){return c["controllerId"]==id;});
        if(found==inventory_["controllers"].end()) throw std::invalid_argument("Scan and select a registered controller first");
        if(std::count_if(inventory_["controllers"].begin(),inventory_["controllers"].end(),[&](const auto& c){return c["controllerId"]==id;})!=1)
            throw ConfigConflict("Duplicate controller identity; operation refused");
        const auto path=found->at("path").template get<std::string>();
        auto& probe=discovery_.connected(path);
        try {
            if(probe.hello()["controllerId"]!=id) throw std::runtime_error("Controller identity changed; scan again");
            auto result=update ? probe.updateGroup(std::stoi(request.matches[2].str()),body) : probe.snapshot();
            result["controllerId"]=id;
            result["temperatureForwarding"]=forwarding_->status();
            result["calibrationStartAvailable"]=true; // Capability, not a safety verdict.
            response.set_content(result.dump(),"application/json");
        } catch(const ConfigConflict&) { throw; }
          catch(const std::invalid_argument&) { throw; }
          catch(const nlohmann::json::exception&) { throw; }
          // A UI error must not orphan a connection held by a healthy forwarding
          // worker. The worker owns heartbeat failure/reconnect; scans reconcile
          // dead ports when forwarding is disabled.
          catch(...) { throw; }
    };
    server_.Get(R"(/api/v1/controllers/([0-9a-f]{32})/snapshot)",
        [controllerOperation](const auto& req, auto& res){controllerOperation(req,res,false);});
    server_.Put(R"(/api/v1/controllers/([0-9a-f]{32})/groups/([01]))",
        [controllerOperation](const auto& req, auto& res){controllerOperation(req,res,true);});
    server_.Post(R"(/api/v1/controllers/([0-9a-f]{32})/groups/([01])/mode)",
        [this](const auto& request,auto& response){
            const auto body=nlohmann::json::parse(request.body);groupControl::validateMode(body);
            std::unique_lock lock(mutex_,std::try_to_lock);
            if(!lock.owns_lock()) throw ConfigConflict("Controller operation busy; retry shortly");
            const std::string id=request.matches[1].str();
            std::shared_ptr<SerialProbe> probe;unsigned matches=0;
            for(const auto& c:inventory_["controllers"]) if(c["controllerId"]==id) {++matches;probe=discovery_.share(c["path"]);}
            if(matches!=1 || !probe) throw ConfigConflict("Select exactly one discovered registered controller");
            if(probe->hello()["controllerId"]!=id) throw ConfigConflict("Controller identity changed; scan again");
            auto result=probe->setGroupMode(std::stoi(request.matches[2].str()),body);result["controllerId"]=id;
            response.set_content(result.dump(),"application/json");
        });

    auto calibrationOperation=[this](const auto& request, auto& response, int action) {
        const auto body=action?nlohmann::json::parse(request.body):nlohmann::json::object();
        if(action) calibration::validateRequest(body,action==1);
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()) throw ConfigConflict("Controller operation busy; retry shortly");
        const std::string id=request.matches[1].str();
        std::shared_ptr<SerialProbe> probe;
        unsigned matches=0;
        for(const auto& c:inventory_["controllers"]) if(c["controllerId"]==id) {
            ++matches;probe=discovery_.share(c["path"]);
        }
        if(matches!=1 || !probe) throw ConfigConflict("Select exactly one discovered registered controller");
        // Start/abort verify identity; lightweight reads reuse the retained verified connection.
        if(action && probe->hello()["controllerId"]!=id)
            throw ConfigConflict("Controller identity changed; scan again");
        nlohmann::json state;
        if(action==1) {
            const int group=std::stoi(request.matches[2].str());
            state=probe->startCalibration(group,body,[&]{calibration::requireForwarding(forwarding_->status(),id,group);});
        } else if(action==2) state=probe->abortCalibration();
        else state=probe->status();
        response.set_content(nlohmann::json{{"controllerId",id},{"status",state}}.dump(),"application/json");
    };
    server_.Get(R"(/api/v1/controllers/([0-9a-f]{32})/calibration)",
        [calibrationOperation](const auto& req,auto& res){calibrationOperation(req,res,0);});
    server_.Post(R"(/api/v1/controllers/([0-9a-f]{32})/groups/([01])/calibration/start)",
        [calibrationOperation](const auto& req,auto& res){calibrationOperation(req,res,1);});
    server_.Post(R"(/api/v1/controllers/([0-9a-f]{32})/calibration/abort)",
        [calibrationOperation](const auto& req,auto& res){calibrationOperation(req,res,2);});
    server_.Put("/api/v1/config", [this](const auto& request, auto& response) {
        const auto next=nlohmann::json::parse(request.body);
        std::lock_guard lock(mutex_);
        const auto saved=config_.save(next);
        forwarding_->configure(saved);
        response.set_content(saved.dump(),"application/json");
    });
    forwarding_->configure(config_.get());
}
int ApiServer::bind(int port) {
    if (port == 0) return server_.bind_to_any_port("127.0.0.1");
    return server_.bind_to_port("127.0.0.1", port) ? port : -1;
}
bool ApiServer::listen() { return server_.listen_after_bind(); }
void ApiServer::stop() { server_.stop(); }
}
