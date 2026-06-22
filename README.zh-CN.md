# mouse-trail

为 Wayland 合成器（niri、Sway、Hyprland 等）打造的流星状光标拖尾叠加层，基于 wlr-layer-shell 与 Cairo 构建。

[English](README.md)

---

## 关于本项目

**mouse-trail** 在透明叠加层上渲染一条渐隐的彗星状轨迹，跟随鼠标光标。支持多显示器、实时颜色切换、透明度控制、拖尾宽度/速度可调、HSL 彩虹变色循环，以及自动主题色同步。

> **本项目主要由 [DeepSeek V4 Pro](https://chat.deepseek.com/) 开发与调试，人工在 NixOS + niri 环境上进行测试验证。** 全部 C 代码、协议绑定、bug 修复均由 AI 辅助迭代生成。人类贡献者负责在双屏 HiDPI（1.77× / 1.60× 分数缩放）环境下实地测试，发现边界情况，并指导调试过程。

---

## 特性

- **流星尾迹**：头部亮且宽，尾部随 t³ 渐隐、(1−t)² 渐细
- **多显示器**：为每个输出创建独立的 layer surface
- **实时控制**：通过 Unix socket 实时切换颜色、宽度、透明度、速度
- **HSL 彩虹循环**：连续彩虹色拖尾，可调节循环速度
- **点击透传**：空输入区域——叠加层不阻挡任何鼠标点击
- **静止渐隐**：鼠标停止约 1 秒后拖尾优雅消失
- **屏幕边缘钳制**：拖尾位置始终在桌面边界内，与合成器行为一致
- **逐事件追踪**：逐个处理 evdev 事件，确保快速反转时边缘行为正确
- **配置文件**：`~/.config/mouse-trail/config`，支持 `import=` 引入外部配置
- **主题同步**：自动读取 noctalia 壁纸颜色，实现全局主题一致
- **Toggle 脚本**：`mouse-trail-toggle` 一次运行开启，再次运行关闭，可绑定快捷键
- **日志系统**：带时间戳的 debug/info/warn/error 日志输出，便于诊断

---

## 安装

### NixOS / home-manager

在你的 home-manager imports 中添加模块：

```nix
# ~/.config/home-manager/home.nix
{
  imports = [
    ./mouse-trail/mouse-trail.nix
  ];
}
```

然后重建：

```bash
home-manager switch
```

这将安装三个命令：`mouse-trail`、`mouse-trail-toggle`、`mouse-trail-ctl`。

### 手动编译

```bash
# 依赖：wayland, wayland-protocols, wlroots, cairo, libevdev, pkg-config, gcc
make
```

生成的可执行文件为 `./mouse-trail`。

---

## 使用方法

### 快速开始

```bash
mouse-trail-toggle          # 启动（再次运行即停止）
mouse-trail-ctl help        # 列出所有控制命令
```

### Toggle（快捷键绑定）

在 niri / Sway 配置中添加：

```
Mod+Ctrl+T { spawn-sh "mouse-trail-toggle"; }
```

### 运行时控制

```bash
mouse-trail-ctl color ff6b6b        # 设置颜色（十六进制，可带或不带 #）
mouse-trail-ctl alpha 0.5           # 设置透明度（0-1）
mouse-trail-ctl width 12            # 设置头部半径（像素）
mouse-trail-ctl speed 300           # 设置拖尾持续时间（毫秒）
mouse-trail-ctl color-cycle on      # 开启 HSL 彩虹循环
mouse-trail-ctl color-cycle off     # 关闭
mouse-trail-ctl show                # 显示拖尾
mouse-trail-ctl hide                # 隐藏拖尾
mouse-trail-ctl help                # 显示所有命令及默认值
```

### 命令行选项

```
mouse-trail --help

选项：
  --config PATH       配置文件路径（默认：~/.config/mouse-trail/config）
  --device PATH       输入设备（默认：/dev/input/event2）
  --color RRGGBB      拖尾颜色（默认：ffffff）
  --alpha N           透明度 0-1（默认：1.0）
  --width N           头部半径（像素，默认：8）
  --length N          拖尾持续时间（毫秒，默认：500）
  --min-speed N       静止检测阈值（像素，默认：2）
  --smooth-factor N   EMA 滤波系数 0-1（默认：0.6）
  --color-cycle on|off
  --cycle-speed N     彩虹循环周期（秒，默认：5）
  --socket PATH       控制 socket 路径
  --log-level debug|info|warn|error（默认：info）
  --log-file PATH     日志文件（默认：stderr）
  --ctl "CMD"         向运行中的实例发送命令后退出
```

### 配置文件

默认路径：`~/.config/mouse-trail/config`

```ini
# 光标拖尾配置
# 使用 import=path 引入其他配置文件

color=80c8ff
alpha=1.0
width=8
length=500
min_speed=2
smooth_factor=0.6
color_cycle=off
cycle_speed=5
device=/dev/input/event2
import=/path/to/theme.conf
```

---

## 配置与环境说明

### `remove-without-permission` → `rm`

如果你在**原作者 NixOS 环境之外**的系统上使用本项目，需要将 `mouse-trail/mouse-trail.nix` 中的 `remove-without-permission` 替换为 `rm`。

**原因：** 原作者系统上安装了一个交互式的 `rm` 封装器，因此使用自定义的 `remove-without-permission` 脚本来绕过它。大多数用户的系统没有这个封装器，直接使用 `rm` 即可正常工作。

修改方法，将 `mouse-trail.nix` 中以下行：

```diff
-        remove-without-permission -f "$PIDFILE" "$SOCK"
+        rm -f "$PIDFILE" "$SOCK"
```

（toggle 脚本中出现两次。）

---

## 架构

```
/dev/input/event2 ──► 输入线程（libevdev，逐事件钳制）
                           │
                     trail.pos_x, trail.pos_y
                     轨迹环形缓冲区（绝对全局坐标）
                           │
                           ▼
   Wayland 连接 ◄── 渲染循环（~60Hz, Cairo→wl_shm）
        │
        ▼
   wlr-layer-shell 叠加层（每个 wl_output 一个表面）
   （input_region = 空区域 → 点击透传）
```

### 关键设计决策

- **绝对全局坐标**：轨迹点存储绝对屏幕位置，不做相对平移。消除复合浮点误差。
- **逐事件处理**：每个 evdev 事件独立处理并钳制边缘，与合成器在屏幕边界的光标行为一致。
- **从几何信息计算分数倍率**：缩放比通过 `物理像素 / 逻辑像素` 从 output mode 和 layer surface configure 事件中计算，避免 `wl_output.scale` 仅支持整数倍率的限制。
- **wl_pointer.enter 获取初始位置**：启动时捕获光标的绝对位置（合成器支持时）。若未收到 enter 事件，回退到主显示器中心。

### 文件结构

```
mouse-trail/
├── src/
│   ├── log.h                              # 带时间戳的日志宏
│   ├── trail.h / trail.c                  # 轨迹状态、环形缓冲区、清理
│   ├── main.c                             # Wayland、Cairo、输入、控制、CLI
│   ├── wlr-layer-shell-client-protocol.h/c # 预生成的 wlr-layer-shell v1
│   ├── xdg-shell-client-protocol.c        # 预生成的 xdg-shell（依赖项）
│   └── relative-pointer-client-protocol.h/c # 已生成（未使用，保留参考）
├── mouse-trail.nix                        # Nix home-manager 模块
├── Makefile                               # 手动 gcc 编译
├── README.md                              # 英文 README
├── README.zh-CN.md                        # 本文件（中文 README）
└── test-mouse-trail.sh                    # 基于日志的验证脚本
```

---

## 依赖

| 依赖 | 用途 |
|------|------|
| `wayland` | Wayland 客户端协议 |
| `wayland-protocols` | xdg-shell 协议 |
| `wlroots` | wlr-layer-shell 协议（代码生成的 XML 源文件） |
| `cairo` | 共享内存缓冲区上的 2D 渲染 |
| `libevdev` | 原始鼠标输入事件读取 |

---

## 主题同步

原作者的 NixOS 配置包含从 [noctalia-shell](https://github.com/Noctalia-Development/noctalia-shell) 壁纸颜色自动同步主题的功能：

- `wallpaper-theme-sync.sh` 监视 `~/.config/noctalia/colors.json`
- 壁纸更换时，将 `mPrimary` 主色推送到：
  - DankMaterialShell（DMS 自定义主题）
  - Waybar（CSS `@define-color lyrics`）
  - **mouse-trail**（通过 `mouse-trail-ctl color`）
- `mouse-trail-toggle` 在启动时也会读取当前的 noctalia 颜色

---

## 故障排查

### 拖尾出现在错误位置

这发生在合成器启动时未发送 `wl_pointer.enter` 事件的情况（niri 上较常见）。拖尾会初始化在主显示器中心。将光标移动到任意屏幕边缘——边界钳制会自动将拖尾位置校正到正确位置。

### 拖尾滞后于光标

确保合成器使用 `accel-profile "flat"`（无指针加速度）。拖尾以 1:1 比例追踪原始 evdev 增量。

### 看不到拖尾

1. 检查输入设备：`mouse-trail --device /dev/input/event2`（用 `evtest` 确认设备路径）
2. 查看日志：`mouse-trail --log-level debug --log-file /tmp/trail.log`
3. 确认合成器支持 `wlr-layer-shell-unstable-v1`

### 彩虹循环颜色不对

颜色循环使用 HSL 色彩空间插值。`--cycle-speed` 参数控制循环速度（默认：5 秒一个完整周期）。

---

## 许可

MIT
