#include "ifacial_protocol.hpp"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace ifacial {

using fgb::kit::Arkit;
using fgb::kit::ArkitWeights;

namespace {

float ParseFloat(std::string_view text, bool& ok) {
    ok = false;
    const std::string owned{text};
    try {
        const float value = std::stof(owned);
        if (std::isnan(value) || std::isinf(value)) {
            return 0.0f;
        }
        ok = true;
        return value;
    } catch (const std::exception&) {
        return 0.0f;
    }
}

std::vector<std::string_view> SplitPipe(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t found = text.find('|', start);
        const std::size_t stop = found == std::string_view::npos ? text.size() : found;
        out.push_back(text.substr(start, stop - start));
        if (found == std::string_view::npos) {
            break;
        }
        start = found + 1;
    }
    return out;
}

bool ReadTriplets(std::string_view values, float (&out)[3]) {
    std::size_t start = 0;
    int index = 0;
    while (start <= values.size() && index < 3) {
        const std::size_t comma = values.find(',', start);
        const std::size_t stop = comma == std::string_view::npos ? values.size() : comma;
        bool ok = false;
        out[index++] = ParseFloat(values.substr(start, stop - start), ok);
        if (!ok) {
            return false;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return index == 3;
}

void GazeFromRaw(const float (&xyz)[3], float (&direction)[3]) {
    // 沿用 VRCFT 上游模块的口径：水平取 tan(y/90)，垂直取 -tan(x/90)。
    const float x = std::tan(xyz[1] / 90.0f);
    const float y = -std::tan(xyz[0] / 90.0f);
    const float length = std::sqrt(x * x + y * y + 1.0f);
    direction[0] = x / length;
    direction[1] = y / length;
    direction[2] = -1.0f / length;
}

} // namespace

bool ParsePacket(std::string_view text, Packet& out, std::string& error) {
    if (text.size() > 64 * 1024) {
        error = "数据包过大";
        return false;
    }
    out = Packet{};

    for (const std::string_view token : SplitPipe(text)) {
        if (token.empty()) {
            continue;  // 首尾与相邻分隔符产生的空字段
        }

        const std::size_t hash = token.find('#');
        if (hash != std::string_view::npos) {
            const std::string_view name = token.substr(0, hash);
            const std::string_view values = token.substr(hash + 1);
            float triplet[3]{};
            if (name == "=head" || name == "head") {
                std::size_t start = 0;
                int index = 0;
                while (index < 6) {
                    const std::size_t comma = values.find(',', start);
                    const std::size_t stop = comma == std::string_view::npos ? values.size() : comma;
                    bool ok = false;
                    out.head[index] = ParseFloat(values.substr(start, stop - start), ok);
                    if (!ok) {
                        break;
                    }
                    ++index;
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    start = comma + 1;
                }
                out.has_head = index == 6;
            } else if (name == "leftEye" && ReadTriplets(values, triplet)) {
                GazeFromRaw(triplet, out.left_direction);
                out.has_eye_direction = true;
            } else if (name == "rightEye" && ReadTriplets(values, triplet)) {
                GazeFromRaw(triplet, out.right_direction);
                out.has_eye_direction = true;
            } else {
                ++out.fields_skipped;
            }
            continue;
        }

        const std::size_t separator = token.find_first_of("-&");
        if (separator == std::string_view::npos || separator == 0) {
            ++out.fields_skipped;
            continue;
        }
        const std::string_view name = token.substr(0, separator);
        bool ok = false;
        const float value = ParseFloat(token.substr(separator + 1), ok);
        if (!ok) {
            ++out.fields_skipped;
            continue;
        }
        // iFacialMocap 的 0..100 整数口径
        const std::optional<Arkit> slot = fgb::kit::ArkitFromName(name);
        if (!slot.has_value()) {
            ++out.shapes_unknown;
            continue;
        }
        // 上游按镜像视角命名，先换成我们的左右口径再写表
        if (fgb::kit::SetArkitByName(out.weights, fgb::kit::kArkitNames[static_cast<std::size_t>(
                fgb::kit::MirrorLeftRight(*slot))], value / 100.0f)) {
            ++out.shapes_parsed;
        } else {
            ++out.shapes_unknown;
        }
    }

    if (out.shapes_parsed == 0 && !out.has_head && !out.has_eye_direction) {
        error = "包里没有任何可用字段";
        return false;
    }
    return true;
}

void DeclareTargets(fgb::kit::ShapeTable& table) {
    fgb::kit::DeclareUnifiedTargets(table);
}

void WriteFrame(const Packet& packet, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot) {
    slot->attributes |= FGB_ATTR_EXPRESSION;
    fgb::kit::MapArkitToUnified(packet.weights, table, slot);

    // 睁眼度沿用上游公式（眨眼 + 眨眼*眼周紧张），方向则用报文里的真眼动值覆盖。
    fgb::kit::ArkitEyeOptions options;
    options.include_squint = true;
    slot->attributes |= fgb::kit::ApplyArkitEyes(packet.weights, slot, options);

    if (packet.has_eye_direction) {
        // 报文里有真眼动角度时覆盖表情系数推导的方向。
        for (int i = 0; i < 3; ++i) {
            slot->left_eye.gaze_dir[i] = packet.left_direction[i];
            slot->right_eye.gaze_dir[i] = packet.right_direction[i];
        }
        // 手机摄像头估计的眼动角度，比表情系数推导可信，但仍不是设备级眼动追踪。
        slot->left_eye.gaze_confidence = 0.7f;
        slot->right_eye.gaze_confidence = 0.7f;
    }
}

} // namespace ifacial
