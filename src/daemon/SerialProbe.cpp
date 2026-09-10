#include "SerialProbe.hpp"
#include "AdapterNames.hpp"
#include "ControllerData.hpp"
#include "ConfigStore.hpp"
#include "CalibrationControl.hpp"
#include "GroupControl.hpp"
#include <array>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <stdexcept>
#include <cerrno>
#include <optional>

namespace fan {
SerialProbe::SerialProbe(const std::string& path) {
    fd_ = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error("Cannot open serial port (permissions or disconnected)");
    bool exclusive=false;
    try {
        if (flock(fd_, LOCK_EX | LOCK_NB) || ioctl(fd_, TIOCEXCL))
            throw std::runtime_error("Serial port is busy");
        exclusive=true;
        termios settings{};
        if (tcgetattr(fd_, &settings)) throw std::runtime_error("Cannot read serial settings");
        cfmakeraw(&settings);
        cfsetispeed(&settings, B115200); cfsetospeed(&settings, B115200);
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= ~CRTSCTS;
        if (tcsetattr(fd_, TCSANOW, &settings)) throw std::runtime_error("Cannot set serial settings");
        // Opening a Nano may reset it; wait for the bootloader before probing.
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        tcflush(fd_, TCIFLUSH);
    } catch (...) { if(exclusive) ioctl(fd_,TIOCNXCL); close(fd_); fd_ = -1; throw; }
}
SerialProbe::~SerialProbe() { if (fd_ >= 0) { ioctl(fd_, TIOCNXCL); close(fd_); } }
std::vector<SerialProbe::Frame> SerialProbe::readFrames() {
    const auto now=std::chrono::steady_clock::now();
    if(now-lastByte_>std::chrono::milliseconds(250)) receiveBuffer_.clear();
    std::array<std::uint8_t,256> incoming{};
    const auto count=read(fd_,incoming.data(),incoming.size());
    if(count>0) {
        receiveBuffer_.insert(receiveBuffer_.end(),incoming.begin(),incoming.begin()+count);
        lastByte_=now;
    } else if(count<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)
        throw std::runtime_error("Serial receive failed");
    std::vector<Frame> frames;
    auto& b=receiveBuffer_;
    while(b.size()>=7) {
        if(b[0]!=0xA5 || b[1]!=0x5A || b[2]!=protocol::version || b[6]>96) {b.erase(b.begin());continue;}
        const std::size_t size=9+b[6];
        if(b.size()<size) break;
        if(protocol::crc16(std::span(b).subspan(2,size-4))!=(b[size-2]|(b[size-1]<<8))) {b.erase(b.begin());continue;}
        frames.push_back({b[3],static_cast<unsigned short>(b[4]|(b[5]<<8)),{b.begin()+7,b.begin()+static_cast<long>(size)-2}});
        b.erase(b.begin(),b.begin()+static_cast<long>(size));
    }
    return frames;
}
void SerialProbe::retain(const Frame& frame) {
    const auto size=frame.payload.size();
    if(!((frame.type==0x87 && size==18) || (frame.type==0x83 && size==21) || (frame.type==0x84 && size==58))) return;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard lock(eventsMutex_);
    if(events_.size()==512) {events_.erase(0);++droppedEvents_;}
    events_.push_back({{"type",frame.type},{"payload",frame.payload},{"receivedAtMs",ms}});
    activity_.notify();
}
void SerialProbe::receiveEvents() {
    {
        std::lock_guard lock(gate_);
        if(busy_ || priorityWaiting_) return;
        busy_=true;
    }
    struct Release {SerialProbe& p;~Release(){std::lock_guard lock(p.gate_);p.busy_=false;p.available_.notify_all();}} release{*this};
    // One bounded read per pass prevents a noisy port from starving temperature sends.
    pollfd pfd{fd_,POLLIN,0};
    if(poll(&pfd,1,0)<0) {if(errno==EINTR) return;throw std::runtime_error("Serial poll failed");}
    if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL)) throw std::runtime_error("Serial disconnected");
    if(pfd.revents&POLLIN) for(const auto& frame:readFrames()) if(frame.type==0x87) retain(frame);
}
nlohmann::json SerialProbe::takeEvents() {
    std::lock_guard lock(eventsMutex_);
    auto result=nlohmann::json{{"events",std::move(events_)},{"dropped",droppedEvents_}};
    events_=nlohmann::json::array();droppedEvents_=0;return result;
}
bool SerialProbe::hasPendingEvents() {
    std::lock_guard lock(eventsMutex_);
    return !events_.empty() || droppedEvents_;
}
void SerialProbe::waitForActivity(const WakeSignal& wake,std::stop_token stop,
    std::chrono::steady_clock::time_point deadline) {
    activity_.clear();
    int serial=-1;
    {
        std::lock_guard lock(gate_);
        // Never hold the serial gate while sleeping. A UI exchange owns its
        // reply; its completion wakes us to collect retained alerts/status.
        if(!busy_ && !priorityWaiting_) serial=fd_;
    }
    if(hasPendingEvents()) return;
    wake.wait(stop,deadline,activity_.descriptor(),serial);
}
std::vector<std::uint8_t> SerialProbe::exchange(protocol::MessageType type,
    protocol::MessageType reply, const std::vector<std::uint8_t>& payload,
    const std::function<std::vector<std::uint8_t>()>& latest,
    std::chrono::steady_clock::time_point* transmitted, bool onlyIdle) {
    const bool priority=type==protocol::MessageType::Temperatures;
    {
        std::unique_lock lock(gate_);
        if(onlyIdle && (busy_ || priorityWaiting_)) throw ConfigConflict("Serial busy; live read skipped");
        if(priority) ++priorityWaiting_;
        available_.wait(lock,[&]{return !busy_ && (priority || priorityWaiting_==0);});
        if(priority) --priorityWaiting_;
        busy_=true;
    }
    struct Release {
        SerialProbe& owner;
        ~Release() {std::lock_guard lock(owner.gate_);owner.busy_=false;owner.available_.notify_all();owner.activity_.notify();}
    } release{*this};
    const auto seq = ++sequence_;
    const auto request = protocol::encode(type, seq, latest?latest():payload);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::size_t sent = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd_, static_cast<short>(POLLIN | (sent < request.size() ? POLLOUT : 0)), 0};
        if (poll(&pfd, 1, 50) < 0) { if (errno == EINTR) continue; break; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pfd.revents & POLLOUT) {
            const auto count = write(fd_, request.data() + sent, request.size() - sent);
            if (count > 0) {
                sent += static_cast<std::size_t>(count);
                if(sent==request.size() && transmitted) *transmitted=std::chrono::steady_clock::now();
            }
        }
        if (pfd.revents & POLLIN) {
            std::optional<std::vector<std::uint8_t>> response;
            unsigned rejection=0;
            for(const auto& frame:readFrames()) {
                if(frame.type==0x87) retain(frame);
                if(frame.sequence!=seq) continue;
                if(frame.type==0x82 && frame.payload.size()==2 && frame.payload[0]==static_cast<std::uint8_t>(type))
                    rejection=frame.payload[1];
                if(frame.type==static_cast<std::uint8_t>(reply)) {
                    // Keep observations in wire order, including frames AFTER the reply.
                    retain(frame);response=frame.payload;
                }
            }
            if(rejection) throw std::runtime_error("Nano rejected command (ACK "+std::to_string(rejection)+")");
            if(response) return *response;
        }
    }
    throw std::runtime_error("No matching protocol-v2 response");
}
nlohmann::json SerialProbe::hello() {
    const auto p = exchange(protocol::MessageType::Hello, protocol::MessageType::HelloResponse);
    if (p.size() != 32 || p[19] != 2) throw std::runtime_error("Unsupported controller Hello format");
    const char* hex = "0123456789abcdef";
    std::string id;
    for (std::size_t i=0;i<16;++i) { id += hex[p[i] >> 4]; id += hex[p[i] & 15]; }
    const bool claimed = id != std::string(32,'0');
    if (p[30] > 1 || claimed != (p[30] == 1)) throw std::runtime_error("Inconsistent controller identity");
    capabilities_=p[20]|(p[21]<<8);
    return {{"controllerId", claimed ? nlohmann::json(id) : nlohmann::json(nullptr)},
        {"firmware",std::to_string(p[16])+"."+std::to_string(p[17])+"."+std::to_string(p[18])},
        {"protocolVersion",p[19]}, {"inaAddress",p[31]}, {"capabilities",capabilities_.load()}};
}
void SerialProbe::setIdentity(const std::string& id) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i=0;i<32;i+=2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(id.substr(i,2),nullptr,16)));
    const auto ack = exchange(protocol::MessageType::SetControllerId, protocol::MessageType::Acknowledgment, bytes);
    if (ack.size() != 2 || ack[0] != 9 || ack[1] != 0) throw std::runtime_error("Controller identity save rejected");
    if (hello().at("controllerId") != id) throw std::runtime_error("Controller identity readback failed");
}
nlohmann::json SerialProbe::snapshot() {
    using M=protocol::MessageType;
    const bool hardened=(capabilities_.load()&0x40)!=0;
    const auto before=hardened?status():nlohmann::json(nullptr);
    auto config=controller::configuration(exchange(M::GetConfiguration,M::Configuration));
    auto calibrations=nlohmann::json::array();
    for(std::uint8_t g=0;g<2;++g) {
        auto cal=controller::calibration(exchange(M::GetCalibration,M::Calibration,{g}));
        if(cal["group"]!=g) throw std::runtime_error("Calibration group mismatch");
        cal["rpmRange"]=controller::rpmRange(cal,config["groups"][g]);
        calibrations.push_back(cal);
    }
    const auto after=status();
    if(hardened && (calibrations[0]["generation"]!=calibrations[1]["generation"] ||
        before["calibrationPhase"]!=after["calibrationPhase"] || before["calibrationGroup"]!=after["calibrationGroup"]))
        throw ConfigConflict("Calibration changed during snapshot; Read Nano again for a coherent result");
    if(capabilities_.load()&0x100)
        config["adapterNames"]=adapterNames::decode(exchange(M::GetAdapterNames,M::AdapterNames));
    return {{"configuration",config},{"calibration",calibrations},
        {"status",after},
        {"calibrationStartAvailable",false},{"groupControlsAvailable",(capabilities_.load()&0x20)!=0}};
}
nlohmann::json SerialProbe::status() {
    return decodeStatus(exchange(protocol::MessageType::GetStatus,protocol::MessageType::Status));
}
nlohmann::json SerialProbe::updateAdapterNames(const nlohmann::json& request) {
    using M=protocol::MessageType;
    adapterNames::validateRequest(request);
    if(!(capabilities_.load()&0x100)) throw std::invalid_argument("Adapter names require firmware 1.7.0 or newer");
    if(status().at("calibrationActive").get<bool>()) throw ConfigConflict("Finish calibration before saving names");
    auto expected=request.at("adapterNames");
    const auto current=adapterNames::decode(exchange(M::GetAdapterNames,M::AdapterNames));
    if(current.at("generation")!=expected.at("generation")) throw ConfigConflict("Adapter names changed; Read Nano again");
    if(current.at("groups")!=expected.at("groups")) {
        const auto ack=exchange(M::SetAdapterNames,M::Acknowledgment,adapterNames::encode(expected));
        if(ack!=std::vector<std::uint8_t>{static_cast<std::uint8_t>(M::SetAdapterNames),0})
            throw std::runtime_error("Adapter-name write not confirmed; Read Nano before retrying");
        expected["generation"]=expected.at("generation").get<std::uint32_t>()+std::uint32_t{1};
    }
    auto result=snapshot();
    if(result.at("configuration").at("adapterNames")!=expected)
        throw std::runtime_error("Adapter-name readback mismatch; storage not confirmed");
    return result;
}
nlohmann::json SerialProbe::liveStatus() {
    return decodeStatus(exchange(protocol::MessageType::GetStatus,protocol::MessageType::Status,{}, {},nullptr,true));
}
nlohmann::json SerialProbe::decodeStatus(const std::vector<std::uint8_t>& bytes) const {
    auto result=controller::status(bytes);
    const bool hardened=(capabilities_.load()&0x40)!=0;
    result["calibrationHardened"]=hardened;
    result["calibratedMinimumSupported"]=(capabilities_.load()&0x80)!=0;
    result["calibrationStorageConfirmed"]=hardened?nlohmann::json(result["calibrationPhase"]==6 && result["calibrationActive"]==false):nlohmann::json(nullptr);
    return result;
}
nlohmann::json SerialProbe::startCalibration(int group, const nlohmann::json& request,
    const std::function<void()>& requireLiveForwarding) {
    using M=protocol::MessageType;
    calibration::validateRequest(request,true);
    if(group<0 || group>1) throw std::invalid_argument("Invalid calibration group");
    requireLiveForwarding();
    const auto config=controller::configuration(exchange(M::GetConfiguration,M::Configuration));
    const auto cal=controller::calibration(exchange(M::GetCalibration,M::Calibration,{static_cast<std::uint8_t>(group)}));
    if(cal["group"]!=group || config["generation"]!=request["configGeneration"] ||
       cal["generation"]!=request["calibrationGeneration"])
        throw ConfigConflict("Controller configuration/calibration changed; Read Nano before starting");
    calibration::requireSafeStart(config,status(),group,request.value("overrideWarnings",nlohmann::json::array()));
    // Recheck runtime after queueing behind any temperature transaction. No synthetic keepalive.
    const auto ack=exchange(M::StartCalibration,M::Acknowledgment,{},[&]{
        requireLiveForwarding();return std::vector<std::uint8_t>{static_cast<std::uint8_t>(group)};
    });
    if(ack.size()!=2 || ack[0]!=7 || ack[1]!=0)
        throw std::runtime_error("Calibration start not acknowledged; read progress before retrying");
    return status();
}
nlohmann::json SerialProbe::abortCalibration() {
    using M=protocol::MessageType;
    // Deliberately independent of mapping, telemetry, generations and selected group.
    const auto ack=exchange(M::AbortCalibration,M::Acknowledgment);
    if(ack.size()!=2 || ack[0]!=8 || ack[1]!=0)
        throw std::runtime_error("Calibration abort not acknowledged; read progress to verify");
    return status();
}
nlohmann::json SerialProbe::temperatures(const std::function<protocol::TemperatureSnapshot()>& latest,
    std::chrono::steady_clock::time_point& transmitted) {
    using M=protocol::MessageType;
    const auto bytes=exchange(M::Temperatures,M::Heartbeat,{},[&]{return latest().payload();},&transmitted);
    if(bytes.size()!=21 || bytes[20]>1 || bytes[10]>7 || bytes[11]>7)
        throw std::runtime_error("Invalid Nano heartbeat");
    auto u16=[&](int i){return static_cast<unsigned>(bytes[i]) | (static_cast<unsigned>(bytes[i+1])<<8);};
    auto u32=[&](int i){return u16(i) | (static_cast<std::uint32_t>(u16(i+2))<<16);};
    return {{"uptimeMs",u32(0)},{"globalActiveFaults",u16(4)},
        {"groupActiveFaults",nlohmann::json::array({u16(6),u16(8)})},
        {"groupModes",nlohmann::json::array({bytes[10],bytes[11]})},
        {"configGeneration",u32(12)},{"calibrationGeneration",u32(16)},{"inaPresent",bytes[20]!=0}};
}
nlohmann::json SerialProbe::updateGroup(int group, const nlohmann::json& request) {
    using M=protocol::MessageType;
    groupControl::validateEdit(request);
    if(group<0 || group>1) throw std::invalid_argument("Invalid fan group");
    const auto& changes=request.at("changes");
    const bool controls=changes.contains("enabled") || changes.contains("expectedFanMask") ||
        changes.contains("minimumDutyPercent") || changes.contains("startupDutyPercent") || changes.contains("startupTimeMs");
    if(controls && !(capabilities_.load()&0x20)) throw ConfigConflict("Upload firmware 1.3.0 or newer for topology-safe fan-group controls");
    const auto state=controller::status(exchange(M::GetStatus,M::Status));
    if(state["calibrationActive"].get<bool>()) throw ConfigConflict("Calibration active; settings are locked");
    const auto bytes=exchange(M::GetConfiguration,M::Configuration);
    const auto config=controller::configuration(bytes);
    const auto cal=controller::calibration(exchange(M::GetCalibration,M::Calibration,{static_cast<std::uint8_t>(group)}));
    if(config["generation"]!=request.at("configGeneration") || cal["generation"]!=request.at("calibrationGeneration"))
        throw ConfigConflict("Controller configuration/calibration changed; reload before applying");
    if(!(capabilities_.load()&0x80)) {
        if((changes.contains("minimumDutyPercent") && changes["minimumDutyPercent"]<20) ||
           (changes.contains("startupDutyPercent") && changes["startupDutyPercent"]<20))
            throw ConfigConflict("Upload firmware 1.5.0 for measured PWM minima");
        if(changes.contains("curve")) for(const auto& point:changes["curve"])
            if(point.at("rpm")==0) throw ConfigConflict("Upload firmware 1.5.0 for explicit zero-RPM curves");
    }
    const auto next=controller::editConfiguration(bytes,group,request.at("changes"),cal);
    const auto ack=exchange(M::SetConfiguration,M::Acknowledgment,next);
    if(ack.size()!=2 || ack[0]!=3 || ack[1]!=0) throw std::runtime_error("Configuration write was not acknowledged; reload to verify state");
    if(exchange(M::GetConfiguration,M::Configuration)!=next) throw std::runtime_error("Configuration readback differs; reload to verify state");
    auto result=snapshot();
    if(request.at("changes").contains("expectedFanMask") && request.at("changes")["expectedFanMask"]!=config["groups"][group]["expectedFanMask"]) {
        const auto& fresh=result["calibration"][group];
        const auto expectedGeneration=cal["generation"].get<std::uint32_t>()+std::uint32_t{1};
        if(fresh["valid"]!=false || !fresh["points"].empty() || fresh["generation"]!=expectedGeneration)
            throw std::runtime_error("Fan settings changed but calibration invalidation was not verified; Read Nano before proceeding");
    }
    return result;
}
nlohmann::json SerialProbe::setGroupMode(int group,const nlohmann::json& request) {
    using M=protocol::MessageType;
    const auto payload=groupControl::modePayload(group,request);
    if(!(capabilities_.load()&0x20)) throw ConfigConflict("Upload firmware 1.3.0 or newer for fan-group controls");
    const auto config=controller::configuration(exchange(M::GetConfiguration,M::Configuration));
    if(config["generation"]!=request["configGeneration"]) throw ConfigConflict("Nano configuration changed; Read Nano before selecting a mode");
    if(!config["groups"][group]["enabled"].get<bool>()) throw ConfigConflict("Enable the Nano group before selecting an operating mode");
    if(status()["calibrationActive"].get<bool>()) throw ConfigConflict("Calibration active; abort or wait before changing mode");
    const auto ack=exchange(M::SetMode,M::Acknowledgment,payload);
    if(ack.size()!=2 || ack[0]!=6 || ack[1]!=0) throw std::runtime_error("Mode not acknowledged; Read Nano before retrying");
    return {{"requestedMode",request["mode"]},{"timeoutSeconds",request["timeoutSeconds"]},{"status",status()}};
}
}
