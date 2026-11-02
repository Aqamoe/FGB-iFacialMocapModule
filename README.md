# FGB iFacialMocapModule

[![build](https://github.com/Aqamoe/FGB-iFacialMocapModule/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-iFacialMocapModule/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。协议解析、映射与 ABI 生命周期有单元测试，并有一条走真实 ABI
的合成 UDP 端到端冒烟（含握手回包）；真机（iPhone 上的 iFacialMocap）验收待补。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 iFacialMocap 原生模块：
直接接收 iFacialMocap App 发到本机 **49983** 端口的 ASCII 数据报，输出 Unified Expressions
表情、睁眼度与逐眼方向。不需要 VRCFT，不注入进程，也不需要管理员权限。

iFacialMocap input module for FaceGaze Bridge: reads the ASCII UDP stream your
iPhone already sends on port 49983 and outputs Unified Expressions shapes, eye
openness and per-eye gaze. No VRCFT, no injection, no admin rights.

## 协议要点 / Wire format

单个数据报，ASCII，以 `|` 分隔（首尾各有一个空字段）：

```text
|mouthSmile_L-40|jawOpen-62|eyeBlink_R-25|=head#-3.2,1.5,0.4,0.01,0.02,0.03|leftEye#5.0,-2.0,0|...|
```

| 形式 | 含义 |
|---|---|
| `名称-整数` 或 `名称&整数` | ARKit 系数，0..100，模块除以 100 |
| `=head#6 个浮点` | 头姿（本模块解析但暂不输出，见「不提供」） |
| `leftEye#3` / `rightEye#3` | 眼动角度，转成 HMD 局部单位向量（-Z 前） |

**L/R 是镜像的**：iFacialMocap 按「当镜子用」的视角命名，所以 `*_L` 对应我们的右侧。
本模块在写入通用映射表之前先归一到我们的左右口径，与 VRCFT 上游模块保持一致。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 头姿 | `=head` 的 6 个值单位与轴向未验证（上游除以 100 的口径存疑），先不接 |
| 瞳孔 | 报文没有该字段 |
| 2D 注视点 | 报文没有；角度是摄像头估计，不用于 foveation |

## 安装

1. Releases 下载 `ifacialmocap-0.1.0.zip`。
2. FGB →「模块库 → 原生模块 → 安装原生模块 (.zip)」。
3. 「模块」页把 **iFacialMocap** 设为启动（角色 `expression` + `eyes`），重启引擎。
4. 手机开始发送后，模块卡片会提示「已向 <手机IP> 回握手包」并转为活跃。

## 使用前提

- 手机与 PC 在同一 WiFi；App 内指向 PC 的局域网 IP，端口 49983。
- **关闭 VRCFaceTracking**：它和本模块抢同一端口。
- 换端口：模块目录写 `config.ini`，`port = 49984`（App 侧同步修改）。

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFGB_KIT_DIR="../FGB-Module-Kit"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target smoke-test      # 需 49983 空闲
cmake --build build --config Release --target package-modules
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json` 在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。

## 排错

| 现象 | 处理 |
|---|---|
| 「无法绑定 UDP 49983：端口已被占用」 | 关闭 VRCFT 或其它接收端 |
| 「还没收到任何数据包」 | 检查同网段、IP 是否填 PC 地址、防火墙是否放行 |
| 没有握手回包、App 一直转圈 | 模块只在引擎 tick 里回包；确认已「设为启动」并重启引擎 |
| 左右表情反了 | 请在 issue 里附手机系统版本；镜像口径按上游行为实现，需真机确认 |

## 目录结构

```
src/main.cpp                ABI 生命周期、UDP 接收、握手回包、状态上报
src/ifacial_protocol.*      文本协议解析与 Unified Expressions 写入
module/                     fgb-native.json 与图标
tests/                      协议单测、真实 ABI 冒烟
```

## 致谢 / Attribution

- 线格式与 L/R 镜像口径参考 [VRC_iFacialMocap](https://github.com/VRCFaceTracking/VRC_iFacialMocap)；
  本仓库是该线格式的独立 C++ 实现，未包含其源码。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
