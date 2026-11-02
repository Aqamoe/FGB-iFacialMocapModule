#pragma once
// iFacialMocap 的 UDP 文本协议解析与帧写入。
//
// 上游格式（iFacialMocap App → PC:49983，ASCII，以 '|' 分隔）：
//   |mouthSmile_L-84|eyeBlink_R-12|=head#-3.2,1.5,0.4,0.01,0.02,0.03|leftEye#5.0,-2.0,0|...|
// 表情值是 0..100 的整数，姿态与眼动是浮点三元组/六元组。
//
// 注意 L/R：iFacialMocap 按「镜像」视角命名（它的常规用法是当镜子），
// 所以 `*_L` 对应我们的右侧，与 VRCFT 上游模块保持一致。

#include "fgb/kit/arkit.hpp"
#include "fgb/kit/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ifacial {

// 收到第一包后要回给手机的握手内容，iFacialMocap 靠它确认 PC 在线。
inline constexpr std::string_view kHandshake =
    "iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719|sendDataVersion=v2";

struct Packet {
    fgb::kit::ArkitWeights weights{};

    // 眼动方向：HMD 局部单位向量，-Z 前。没有 leftEye/rightEye 字段时保持无效。
    float left_direction[3] = {0.0f, 0.0f, -1.0f};
    float right_direction[3] = {0.0f, 0.0f, -1.0f};
    bool has_eye_direction = false;

    bool has_head = false;
    float head[6]{};

    std::size_t shapes_parsed = 0;
    std::size_t shapes_unknown = 0;
    std::size_t fields_skipped = 0;
};

bool ParsePacket(std::string_view text, Packet& out, std::string& error);
void DeclareTargets(fgb::kit::ShapeTable& table);
void WriteFrame(const Packet& packet, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot);

} // namespace ifacial
