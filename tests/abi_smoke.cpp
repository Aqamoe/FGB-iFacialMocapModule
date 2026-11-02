// 走真实 ABI 的端到端冒烟：加载 DLL → query → initialize → 合成 UDP → update → 检查提交帧。
// 同时验证模块会向手机回 iFacialMocap 握手包。
//
// 它会绑定 49983 端口，运行前关掉 VRCFT 与其它接收端；因此不挂进 ctest。

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "fgb_module_api.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr std::uint16_t kPort = 49983;
constexpr char kPayload[] =
    "|mouthSmile_L-40|jawOpen-62|eyeBlink_R-25|browInnerUp-80|leftEye#5.0,-2.0,0|rightEye#-4.0,3.0,0|";

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <ifacialmocap.dll 路径>\n", argv[0]);
        return 2;
    }

    HMODULE library = ::LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败，错误码 %lu\n", ::GetLastError());
        return CheckSummary("ifacialmocap-abi-smoke");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    fgb::kit::testing::HostMock host;
    FgbInitParams params = host.MakeParams();
    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module == nullptr) {
        ::FreeLibrary(library);
        return CheckSummary("ifacialmocap-abi-smoke");
    }
    CHECK(module->id == std::string{"com.fgbbridge.ifacialmocap"});
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr);

    void* self = nullptr;
    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    if (self == nullptr) {
        std::printf("initialize 失败：%s\n", host.LastStatusDetail().c_str());
        ::FreeLibrary(library);
        return CheckSummary("ifacialmocap-abi-smoke");
    }

    WSADATA data{};
    CHECK(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET phone = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kPort);
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::sendto(phone, kPayload, static_cast<int>(std::strlen(kPayload)), 0,
                   reinterpret_cast<sockaddr*>(&target), sizeof(target)) != SOCKET_ERROR);

    // 握手：模块必须给来源地址回一包确认内容。
    // 注意时序——回握手包发生在宿主的 update() 调用栈里（ABI 要求提交回到 tick），
    // 所以必须一边 update 一边收，不能只等网络。
    ::u_long mode = 1;
    ::ioctlsocket(phone, FIONBIO, &mode);
    std::string handshake;
    for (int attempt = 0; attempt < 200 && (handshake.empty() || host.committed().empty());
         ++attempt) {
        module->update(self);
        char buffer[256] = {0};
        sockaddr_in from{};
        int from_length = sizeof(from);
        const int got = ::recvfrom(phone, buffer, sizeof(buffer) - 1, 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_length);
        if (got > 0) {
            handshake.assign(buffer, static_cast<std::size_t>(got));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(handshake.find("iFacialMocap_") != std::string::npos);
    CHECK(handshake.find("sendDataVersion=v2") != std::string::npos);

    CHECK(!host.committed().empty());
    if (!host.committed().empty()) {
        const FgbFrameSlot& slot = host.committed().back();
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.62, 1e-5);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullRight")], 0.40, 1e-5);
        CHECK(slot.left_eye.openness < 1.0f);
    }

    ::closesocket(phone);
    ::WSACleanup();
    module->teardown(self);
    ::FreeLibrary(library);
    return CheckSummary("ifacialmocap-abi-smoke");
}
