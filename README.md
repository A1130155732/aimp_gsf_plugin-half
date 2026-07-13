[![English](https://img.shields.io/badge/English-README-blue?style=flat-square)](./README_EN.md)

# AIMP GSF 插件

> **声明**：本人是编程小白，本项目由我向 AI 提交需求，经多个 AI 接力协作完成（因免费积分有限）。  
> 描述可能存在疏漏或与实际情况不符，敬请谅解。
>
> **注意**：本仓库已关闭 Issue 提交和 Pull Request，因我暂无能力处理，感谢理解。
>
> 开发初期曾使用《超级机器人大战OG2》的音乐调试，偶然命中了一个可用内存地址，一度让我误以为思路正确，导致后续尝试扩展支持其他游戏音乐时的大量修复均告失败。  
> 之后重构了逻辑，目前插件**仅能播放部分游戏的 `.minigsf` 音乐**（有时会有噪音/程序卡死），详见下方个人测试列表。
>
> **欢迎感兴趣的开发者 Fork 并修复！感谢！**

---

## 简介

基于 foobar2000 的 `foo_input_gsf` 实现原理，为 AIMP 播放器开发的 GBA 音乐格式（`.minigsf`）解码插件。

---

## 演示视频

[哔哩哔哩](https://www.bilibili.com/video/BV1Q2Nu68Eqk)

---

## 功能特性

- 支持 `.minigsf` 格式解析（实测仅部分音乐可行）
- 支持 PSF 标签信息读取（标题、艺术家、专辑、时长等）
- 立体声 16‑bit 音频输出（44.1 kHz）
- 集成 GBA 音频处理单元（APU）及 ARM7TDMI 模拟器

---

## 已知限制 / 问题

- 插件未对 `gsflib` 外部库的引用做充分处理，可能影响兼容性。
- 当前仅能播放部分游戏的音乐。个人测试结果如下（**非完整测试列表**，仅挑选了我个人喜欢的几首音乐，因此可能出现同一游戏中部分音乐可播、部分不可播的情况）：
  1. 《公主联盟》 – 正常播放
  2. 《塞尔达传说 缩小帽》 – 正常播放
  3. 《宝可梦 蓝宝石》 – 正常播放
  4. 《正邪幻想曲 Zero》 – 正常播放
  5. 《约束之地》 – 正常播放
  6. 《超级机器人大战OG2》 – 大部分时间是噪音（重构后就不支持了，太搞了）
  7. 《逆转裁判》 – 正常播放
  8. 《铸剑物语》 – 没有声音
  9. 《铸剑物语3》 – 没有声音
  10. 《露娜传奇》 – 没有声音
  11. 《青之天外》 – 正常播放
  12. 《龙战士》 – 正常播放
  13. 《恶魔城：晓月之圆舞曲》 – **程序卡死**

---

## 快速开始

### 编译环境

- Windows 10 / 11
- Visual Studio 2019 或 2022（需安装“C++ 桌面开发”工作负载）
- Windows SDK 10.0

### 编译步骤

1. 用 Visual Studio 打开项目文件 `aimp_gsf.vcxproj`
2. 选择配置：`Release`，平台：`x86`（**必须**，因为 AIMP 为 32 位程序）
3. 生成解决方案（`Ctrl+Shift+B` 或菜单“生成”）
4. 将编译生成的 `aimp_gsf.dll` 复制到 AIMP 插件目录（例如 `C:\Program Files (x86)\AIMP\Plugins\aimp_gsf`）
5. 重启 AIMP 播放器，若程序检测不到插件，需手动到插件管理界面勾选启用

### 运行时依赖

- 系统需具备 `zlib.dll`（插件动态加载）。若缺失，请从 [zlib.net](https://zlib.net/) 获取，并将其置于系统 PATH 环境变量所包含的目录或 AIMP 安装目录下。

---

## 项目结构
```txt
aimp_gsf_plugin/
├── src/ # 插件源代码
│ ├── Plugin.h/cpp # AIMP 插件主接口
│ ├── GsfDecoder.h/cpp # GSF 音频解码器
│ ├── GsfFormat.h/cpp # GSF/PSF 文件格式解析
│ ├── GbaApu.h/cpp # GBA APU 模拟
│ ├── GbaEmulator.h/cpp # GBA 模拟器（ARM7TDMI + 外设）
│ ├── miniz.h/cpp # zlib 解压封装（运行时加载 zlib.dll）
│ ├── aimp_gsf.def # DLL 导出定义
│ ├── dllmain.cpp # DLL 入口点
│ └── DebugLog.h # 调试日志辅助
│
├── sdk/AIMP/ # AIMP SDK 头文件
├── aimp_gsf.slnx # Visual Studio 解决方案文件
├── aimp_gsf.vcxproj # 项目文件
│
├── aimp_gsf.dll # 预编译 DLL（可直接放入插件目录试用）
├── GBA精选.zip # 我喜欢的 GBA 音乐
├── gsf_plugin_src-v21-仅支持OG2.zip # 重构前的版本（仅支持 OG2），含预编译 DLL
├── README.md # 本文档
├── README_EN.md # 英文版（翻译支持：DeepSeek）
└── LICENSE # 许可证文件
```

---

## 版本历史

### v1 (2026-07-12)
- 发现原有思路存在问题，将模拟器逻辑重构。实测仅支持部分游戏音乐播放，大量修复尝试以失败告终。

### v47 (2026-07-11)
- 尝试扩展支持其他 `.minigsf` 游戏音乐，但未能成功。被偶然命中的可用地址 `08002DAC` 带偏了思路，这期间的修复全部无效。
- 主要修改了 `GbaEmulator.cpp/.h`，其他文件基本无变动。

### v21 (2026-07-09)
- 修复重复播放/切歌后产生噪音的问题（根源为静态变量 `s_m4aInjected` 在 `Reset()` 中未重置，现已修正）。
- 仅支持 OG2。

### v1 (2026-07-04)
- 初始版本，基于 `foo_input_gsf` 代码移植至 AIMP。(存疑，我没有对应源码，只告诉AI用`foo_input_gsf`的原理制作插件)

---

## 技术细节

- **音频参数**：44.1 kHz / 立体声 / 16‑bit PCM
- **GSF 格式**：属于 PSF 家族，针对 GBA 音频驱动压缩存储。miniGSF 是 GSF 的子集，通常依赖外部 `gsflib` 库提供完整功能。本插件对该库的集成尚不完善。
- **模拟核心**：包含 ARM7TDMI 解释器及 GBA APU 混音器。

---

## 致谢

- [kode54](https://www.foobar2000.org/components/view/foo_input_gsf) – foobar2000 `foo_gsf` 插件作者
- [Artem Izmaylov](https://www.aimp.ru/) – AIMP 播放器及 SDK 提供者
- Neill Corlett – GSF 格式规范（公有领域文档）
- ChatGPT，Claude，DeepSeek，Manus，秒哒，语构（排名不分先后，按首字母排序）

---

## 联系方式

- 如有问题或版权疑虑，请联系 B站用户：**西藏狗粮**

---

## 许可证

本项目代码基于 **GNU General Public License v2** 发布（与 `foo_input_gsf` 保持一致）。  
GSF 格式规范及文档属于公有领域。
