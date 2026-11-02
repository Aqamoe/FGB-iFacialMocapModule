#include "ifacial_protocol.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"

#include <cmath>
#include <string>

using namespace fgb::kit;
using ifacial::Packet;
using ifacial::ParsePacket;

namespace {

constexpr std::size_t Slot(Arkit name) { return static_cast<std::size_t>(name); }

const char* kPayload =
    "|mouthSmile_L-40|jawOpen-62|eyeBlink_R-25|eyeSquint_R-50|browDown_L-30|browInnerUp-80|"
    "tongueOut-100|=head#-3.2,1.5,0.4,0.01,0.02,0.03|leftEye#5.0,-2.0,0|rightEye#-4.0,3.0,0|";

} // namespace

int main() {
    Packet packet{};
    std::string error;
    CHECK(ParsePacket(kPayload, packet, error));
    CHECK_EQ(packet.shapes_parsed, static_cast<std::size_t>(7));
    CHECK(packet.has_head);
    CHECK(packet.has_eye_direction);

    // 镜像：上游的 _L 是我们的右侧，_R 是左侧
    CHECK_NEAR(packet.weights[Slot(Arkit::MouthSmileRight)], 0.40, 1e-5);
    CHECK_NEAR(packet.weights[Slot(Arkit::EyeBlinkLeft)], 0.25, 1e-5);
    CHECK_NEAR(packet.weights[Slot(Arkit::EyeSquintLeft)], 0.50, 1e-5);
    CHECK_NEAR(packet.weights[Slot(Arkit::BrowDownRight)], 0.30, 1e-5);
    CHECK_NEAR(packet.weights[Slot(Arkit::JawOpen)], 0.62, 1e-5);
    CHECK_NEAR(packet.weights[Slot(Arkit::BrowInnerUp)], 0.80, 1e-5);
    CHECK_NEAR(packet.weights[Slot(Arkit::TongueOut)], 1.00, 1e-5);
    CHECK_NEAR(packet.head[0], -3.2, 1e-5);

    // ---- 非法与边界 ----
    Packet empty{};
    CHECK(!ParsePacket("|||", empty, error));
    CHECK(!ParsePacket(std::string(70 * 1024, 'a'), empty, error));
    Packet junk{};
    CHECK(!ParsePacket("|nonsense|more nonsense|", junk, error));  // 没有任何可解析字段

    // 单个坏字段不影响同一包里的其它字段
    Packet mixed{};
    CHECK(ParsePacket("|jawOpen-abc|mouthSmile_L-50|leftEye#1,x,3|eyeBlink_L-40|", mixed, error));
    CHECK_EQ(mixed.shapes_parsed, static_cast<std::size_t>(2));
    CHECK(mixed.head[0] == 0.0f);

    // ---- 帧写入 ----
    testing::HostMock host;
    Host logger{host.services()};
    ShapeTable table;
    ifacial::DeclareTargets(table);
    CHECK(table.Build(logger));
    CHECK_EQ(table.ResolvedCount(), table.Size());

    FgbFrameSlot slot{};
    slot.struct_size = sizeof(FgbFrameSlot);
    slot.shape_count = FGB_MAX_SHAPES;
    ifacial::WriteFrame(packet, table, &slot);

    CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
    CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
    CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullRight")], 0.40, 1e-5);
    CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.62, 1e-5);
    CHECK_NEAR(slot.shapes[host.IndexOf("BrowLowererRight")], 0.30, 1e-5);
    CHECK_NEAR(slot.shapes[host.IndexOf("BrowPinchRight")], 0.30, 1e-5);
    CHECK_NEAR(slot.shapes[host.IndexOf("TongueOut")], 1.00, 1e-5);

    // 睁眼度：1 - clamp(blink + blink*squint)，左眼用 _R 的 0.25 与 0.50
    CHECK_NEAR(slot.left_eye.openness, 1.0f - (0.25f + 0.25f * 0.50f), 1e-5);
    CHECK_NEAR(slot.right_eye.openness, 1.0f, 1e-6);
    CHECK_NEAR(slot.left_eye.gaze_confidence, 0.7f, 1e-6);

    // 眼动方向必须是单位向量
    const float length = std::sqrt(slot.left_eye.gaze_dir[0] * slot.left_eye.gaze_dir[0] +
                                   slot.left_eye.gaze_dir[1] * slot.left_eye.gaze_dir[1] +
                                   slot.left_eye.gaze_dir[2] * slot.left_eye.gaze_dir[2]);
    CHECK_NEAR(length, 1.0, 1e-5);
    CHECK(slot.left_eye.gaze_dir[0] < 0.0f);  // 上游口径：水平分量取 tan(y/90)，y=-2 → 负
    CHECK(slot.left_eye.gaze_dir[2] < 0.0f);

    // 没有 leftEye/rightEye 时保留表情系数推导的方向，不伪造 (0,0,-1)
    Packet no_eye{};
    CHECK(ParsePacket("|jawOpen-10|eyeBlink_L-50|", no_eye, error));
    CHECK(!no_eye.has_eye_direction);
    FgbFrameSlot fallback{};
    fallback.struct_size = sizeof(FgbFrameSlot);
    fallback.shape_count = FGB_MAX_SHAPES;
    ifacial::WriteFrame(no_eye, table, &fallback);
    CHECK_NEAR(fallback.left_eye.gaze_confidence, 0.6f, 1e-6);  // Kit 的近似口径
    // eyeBlink_L 镜像到右眼：右眼半闭，左眼不受影响
    CHECK_NEAR(fallback.right_eye.openness, 0.5, 1e-5);
    CHECK_NEAR(fallback.left_eye.openness, 1.0, 1e-5);

    return CheckSummary("ifacialmocap-protocol");
}
