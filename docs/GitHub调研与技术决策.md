# GitHub 调研与技术决策

调研日期：2026-08-24 至 2026-08-25

## 决策

首版实现“iPad 显示 Windows 真扩展桌面”。蓝牙鼠标始终连接 Windows，
由 Windows 原生多显示器坐标系完成跨屏；不把同一次鼠标事件广播给两台
系统，也不在首版控制 iPadOS 原生应用。

选择 `martinhoess/opendisplay-win` 作为 Windows 发送端基础，固定提交：

`06af3a9333f3acd0a79725adffb07b681abdcaf8`

官方 OpenDisplay iPad 接收端协议参考固定提交：

`abb696ec296dd78bd8d69011da92b2c7c33f3853`

上游公开 v0.1.0 二进制明显早于当前源码，不能直接重新包装。本工程从
上述源码快照构建，并补齐协议 v3 生命周期、网络门禁、驱动安全和安装包。

## 候选项目

| 项目 | 可复用能力 | 决策 |
|---|---|---|
| [opendisplay-win](https://github.com/martinhoess/opendisplay-win) | Windows VDD、DXGI、H.264、触控回传 | 首选底座，但按原型代码审计和加固 |
| [OpenDisplay](https://github.com/peetzweg/opendisplay) | 现成 iPad 接收端、公开 v3 协议 | 首版直接复用，不修改 iOS App |
| [Sunshine](https://github.com/LizardByte/Sunshine) + [Moonlight iOS](https://github.com/moonlight-stream/moonlight-ios) | 成熟低延迟串流 | OpenDisplay 实测不达标时的后备后端 |
| [Apollo](https://github.com/ClassicOldSong/Apollo) | 动态虚拟显示 | 当前 Build 26200 有睡眠唤醒挂起报告，且增加 VDD 冲突，不采用 |
| [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) | 签名虚拟显示器 | 未来驱动适配器，不在首版自动叠装 |
| [WinPad](https://github.com/xmeti/winpad) | Windows 到 iPad WebRTC | 项目过新且没有输入回传，只作参考 |
| [windows-ble-hid](https://github.com/abhishek-raj/windows-ble-hid) | Windows 模拟 BLE HID | 未来原生 iPad 应用控制的独立实验，不纳入首版 |

Deskreen、Weylus、RustDesk、Deskflow 均不能单独提供“Windows 真扩展屏 +
自然跨屏鼠标”的完整体验。spacedesk 和 Duet 是成熟商业基准，但不是本
开源工程的代码基础。

## 本机基线

- Windows x64，Build 26200。
- NVIDIA RTX 4060 Laptop；系统已枚举 NVIDIA H.264 Encoder MFT。
- 当前物理屏 2560×1440、165 Hz、150% 缩放、主屏位于 `(0,0)`。
- `GameViewer Virtual Display Adapter 15.6.5.199` 已安装并启动，但没有
  创建活动虚拟监视器；本工程不得禁用、升级或卸载它。
- 当前网络 `iQOO 13` 的 Windows 类别为 Public。正式传屏必须先由用户
  确认它是私人热点，再显式改成 Private；程序和安装器都不会替用户修改。
- Intel 蓝牙同时支持 Central 与 Peripheral；该事实仅为未来 BLE HID
  路线保留，首版副屏模式不使用蓝牙转发。

## 已识别风险

- OpenDisplay v3 的 TCP 传输没有 TLS 和认证，只能用于可信私有局域网。
- Parsec VDD 会产生不稳定监视器身份，强杀进程可能留下显示器节点；因此
  禁止按名称、通配符或全局数字索引清理，必须使用实例收据精确回滚。
- 上游 BGRA 到 NV12 为 CPU 转换；iPad Pro 原生分辨率 60 fps 是否满足
  延迟指标必须由实机测量决定，失败时切换 Sunshine/Moonlight 后端。
- Windows 端和修改源码按 GPL-3.0 发行；Parsec 驱动二进制的再分发许可
  独立，未确认前不得捆绑。
