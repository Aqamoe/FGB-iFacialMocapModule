// iFacialMocap → FaceGaze Bridge 原生模块。
//
// 监听 iFacialMocap App 发到本机 49983 端口的 ASCII 数据报，回一次握手，
// 把 52 个 ARKit 系数映射成 Unified Expressions，并给出睁眼度与逐眼方向。
// 不需要 VRCFT，不注入任何进程，也不需要管理员权限。

#include "ifacial_protocol.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp"

#include <cstdint>
#include <string>

namespace {

constexpr std::uint16_t kDefaultPort = 49983;
constexpr std::uint64_t kNoDataTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;

class IFacialSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        if (config.Load(directory, "config.ini", config_error)) {
            port_ = static_cast<std::uint16_t>(config.GetInt("port", port_));
            if (!config.UnknownKeys().empty()) {
                host.Warn("config.ini 有未识别的键：" + config.UnknownKeys()[0]);
            }
        }

        fgb::kit::ShapeTable table;
        ifacial::DeclareTargets(table);
        if (!table.Build(host)) {
            error = "宿主没有提供 shape_index，无法映射 Unified Expressions";
            return false;
        }
        table_ = std::move(table);

        if (!endpoint_.Bind(port_, error)) {
            return false;
        }
        host.Info("iFacialMocap 监听 UDP " + std::to_string(port_) + "，等待手机连接");
        host.StatusStarting("等待 iFacialMocap 数据");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        ifacial::Packet packet;
        bool parsed = false;

        fgb::kit::UdpPacket datagram;
        if (endpoint_.PollLatest(datagram)) {
            const std::string_view text{reinterpret_cast<const char*>(datagram.data.data()),
                                        datagram.data.size()};
            std::string error;
            if (ifacial::ParsePacket(text, packet, error)) {
                parsed = true;
                MaybeHandshake(host, datagram);
                if (packet.shapes_unknown > 0 && packet.shapes_unknown != reported_unknown_) {
                    reported_unknown_ = packet.shapes_unknown;
                    host.Warn("iFacialMocap 上报了 " + std::to_string(packet.shapes_unknown) +
                              " 个无法识别的字段名（模块会忽略它们）");
                }
            } else {
                host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs, "丢弃一包：" + error);
            }
        }

        if (!staleness_.Check(host, endpoint_.LastPacketNs(), now_ns) || !parsed) {
            return false;
        }
        ifacial::WriteFrame(packet, table_, &slot);
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        endpoint_.Close();
        host.StatusStopped("iFacialMocap 已停止");
    }

private:
    // 手机只认「PC 回过握手」这条链路；换设备（不同来源地址）时重发一次。
    void MaybeHandshake(fgb::kit::Host& host, const fgb::kit::UdpPacket& datagram) {
        if (datagram.remote_address == handshake_to_) {
            return;
        }
        std::string error;
        const bool ok = endpoint_.SendTo(ifacial::kHandshake.data(), ifacial::kHandshake.size(),
                                         datagram.remote_address, datagram.remote_port, error);
        if (ok) {
            handshake_to_ = datagram.remote_address;
            host.Info("已向 " + handshake_to_ + ":" + std::to_string(datagram.remote_port) +
                      " 回握手包");
        } else {
            host.Warn("握手包发送失败：" + error);
        }
    }

    fgb::kit::UdpEndpoint endpoint_;
    fgb::kit::ShapeTable table_;
    fgb::kit::Staleness staleness_{kNoDataTimeoutNs, "iFacialMocap"};
    std::uint16_t port_ = kDefaultPort;
    std::uint64_t warn_ns_ = 0;
    std::size_t reported_unknown_ = 0;
    std::string handshake_to_;
};

} // namespace

FGB_KIT_EXPORT(IFacialSource, "com.fgbbridge.ifacialmocap", "iFacialMocap",
               FGB_CAP_EXPRESSION | FGB_CAP_EYES)
