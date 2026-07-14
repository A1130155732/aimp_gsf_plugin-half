// GBA 模拟器核心
// 负责执行 GBA ROM 代码，驱动 APU 产生音频
// 采用简化的 ARM7TDMI CPU 模拟（仅执行音频驱动代码）

#pragma once

#include "GbaApu.h"
#include <cstdint>
#include <vector>
#include <functional>

// ARM7TDMI 寄存器
struct ArmRegisters
{
    uint32_t r[16];     // r0-r15 (r13=SP, r14=LR, r15=PC)
    uint32_t cpsr;      // 当前程序状态寄存器
    uint32_t spsr;      // 保存的程序状态寄存器
    
    uint32_t r13_usr, r14_usr; // User/System 模式
    uint32_t r13_irq, r14_irq; // IRQ 模式
    uint32_t spsr_irq;

    // CPSR 标志位
    bool N() const { return (cpsr >> 31) & 1; }
    bool Z() const { return (cpsr >> 30) & 1; }
    bool C() const { return (cpsr >> 29) & 1; }
    bool V() const { return (cpsr >> 28) & 1; }
    bool T() const { return (cpsr >> 5) & 1; }  // Thumb 模式
    int  Mode() const { return cpsr & 0x1F; }

    void SetN(bool v) { cpsr = (cpsr & ~(1u<<31)) | ((uint32_t)v << 31); }
    void SetZ(bool v) { cpsr = (cpsr & ~(1u<<30)) | ((uint32_t)v << 30); }
    void SetC(bool v) { cpsr = (cpsr & ~(1u<<29)) | ((uint32_t)v << 29); }
    void SetV(bool v) { cpsr = (cpsr & ~(1u<<28)) | ((uint32_t)v << 28); }
    void SetT(bool v) { cpsr = (cpsr & ~(1u<<5))  | ((uint32_t)v << 5);  }

    uint32_t& PC() { return r[15]; }
    uint32_t& SP() { return r[13]; }
    uint32_t& LR() { return r[14]; }
    
    void SwitchMode(int newMode) {
        int oldMode = cpsr & 0x1F;
        if (oldMode == newMode) return;

        // 保存当前模式的寄存器
        if (oldMode == 0x12) { // 从 IRQ 切出
            r13_irq = r[13]; r14_irq = r[14]; spsr_irq = spsr;
        } else { // 从 System/User 切出
            r13_usr = r[13]; r14_usr = r[14];
        }

        // 加载新模式的寄存器
        if (newMode == 0x12) { // 切入 IRQ
            r[13] = r13_irq; r[14] = r14_irq; spsr = spsr_irq;
        } else { // 切入 System/User (0x1F)
            r[13] = r13_usr; r[14] = r14_usr;
        }
        
        cpsr = (cpsr & ~0x1F) | newMode;
    }
};

// GBA 定时器
struct GbaTimer
{
    uint16_t counter;       // 当前计数值
    uint16_t reload;        // 重载值
    bool     enabled;
    bool     irq_enable;
    bool     cascade;       // 级联模式（由上一个定时器溢出触发）
    int      prescaler;     // 预分频 (1, 64, 256, 1024)
    int      prescaler_cnt; // 预分频计数器

    void Reset()
    {
        counter = 0; reload = 0;
        enabled = false; irq_enable = false; cascade = false;
        prescaler = 1; prescaler_cnt = 0;
    }
};

// GBA 中断控制
struct GbaInterrupt
{
    uint16_t ie;    // 中断使能
    uint16_t ifl;   // 中断标志
    bool     ime;   // 主中断使能

    void Reset() { ie = 0; ifl = 0; ime = false; }
};

// GBA DMA 通道
struct GbaDma
{
    uint32_t src;           // SAD 寄存器写入值
    uint32_t dst;           // DAD 寄存器写入值
    uint32_t src_latch;     // DMA 激活时锁存的实际源地址（运行中递增）
    uint32_t dst_latch;     // DMA 激活时锁存的实际目标地址
    uint16_t count;
    uint16_t control;
    bool     enabled;
    bool     repeat;
    int      timing;        // 0=立即, 1=VBlank, 2=HBlank, 3=特殊(Sound FIFO)
    bool     irq_enable;
    int      src_ctrl;      // 0=递增, 1=递减, 2=固定
    int      dst_ctrl;      // 0/3=递增, 1=递减, 2=固定
    bool     word_size;     // false=16bit, true=32bit

    void Reset()
    {
        src = dst = src_latch = dst_latch = 0;
        count = 0; control = 0;
        enabled = false; repeat = false; timing = 0;
        irq_enable = false; src_ctrl = 0; dst_ctrl = 0; word_size = false;
    }
};

// GBA 模拟器主类
class GbaEmulator
{
public:
    GbaEmulator();
    ~GbaEmulator();

    // 加载 ROM 数据
    bool LoadRom(const uint8_t* romData, size_t romSize, uint32_t romOffset = GBA_ROM_BASE, uint32_t entryPoint = GBA_ROM_BASE);

    // 初始化模拟器
    void Init(int outputSampleRate);

    // 重置模拟器
    void Reset();

    // 运行模拟器，生成音频样本
    // 返回生成的立体声样本数
    int RunForSamples(int16_t* buffer, int maxSamples);

    // 获取 APU 引用（用于外部访问）
    GbaApu& GetApu() { return m_apu; }

    // 内存读写
    uint8_t  ReadByte(uint32_t addr);
    uint16_t ReadHalf(uint32_t addr);
    uint32_t ReadWord(uint32_t addr);
    void WriteByte(uint32_t addr, uint8_t value);
    void WriteHalf(uint32_t addr, uint16_t value);
    void WriteWord(uint32_t addr, uint32_t value);

private:
    // 内存区域
    std::vector<uint8_t> m_rom;     // GBA ROM (最大 32MB)
    uint32_t m_romOffset;           // ROM 基址
    uint32_t m_entryPoint;          // 程序入口点
    uint8_t m_ewram[GBA_EWRAM_SIZE]; // 外部 WRAM (256KB)
    uint8_t m_iwram[GBA_IWRAM_SIZE]; // 内部 WRAM (32KB)
    uint8_t m_ioram[0x400];          // IO 寄存器
    uint8_t m_palette[0x400];        // 调色板 RAM
    uint8_t m_vram[0x18000];         // VRAM (96KB)
    uint8_t m_oam[0x400];            // OAM (1KB)
    uint8_t m_bios[0x4000];          // BIOS (16KB，使用开放 BIOS)

    // CPU 状态
    ArmRegisters m_regs;
    bool         m_halted;      // CPU 暂停（等待中断）

    // 外设
    GbaApu      m_apu;
    GbaTimer    m_timers[4];
    GbaInterrupt m_irq;
    GbaDma      m_dma[4];

    // 时钟
    int     m_outputSampleRate;
    int     m_cyclesPerFrame;   // 每帧 CPU 周期数（280896）
    int     m_cycleAccum;       // 全局累积周期数（跨帧，用于计算 VCOUNT）
    uint8_t m_lastVcount;       // 上一 VCOUNT 值，用于 VBlank 边沿检测（跨帧持久化）
    int     m_frameCount;       // 已完成帧数（调试用）

    // 调试/看门狗状态 —— 必须是"每个对象一份"，而不是函数内 static。
    // 之前这些是函数内 static 局部变量，等于整个进程共用一份：
    // 只要有一首曲子触发了"放弃自救"/"RUNAWAY"，这个标记就会永久保持置位，
    // 导致后面播放的所有曲目（哪怕 ROM 完全正常）一进 RunForSamples 就被
    // 直接判死刑、立即返回，表现为"莫名其妙全程静音"。
    // 现在改成成员变量，并在 Reset() 里清零，确保每次真正重新开始播放
    // （每个对象/每次 Open/每次向后 seek 触发的 Reset）都是干净状态。
    uint32_t m_lastPC;
    int      m_nopSeaCount;
    int      m_nopSeaRescueTotal;
    bool     m_nopSeaFatal;
    bool     m_runawayFatal;
    bool     m_bxBadJumpLogged;
    bool     m_loggedSwi[256];

    // 最近执行过的指令历史（环形缓冲区），在 NOP-SEA/RUNAWAY 触发时打印出来，
    // 方便在没有反汇编的情况下，看清楚 CPU 究竟是从哪条指令、哪个分支
    // 走进了不该去的地方，而不是只能看到出事那一刻的寄存器快照。
    static constexpr int kTraceDepth = 48;
    struct TraceEntry { uint32_t pc; uint32_t instr; bool thumb; };
    TraceEntry m_trace[kTraceDepth];
    int        m_traceIdx;
    void       PushTrace(uint32_t pc, uint32_t instr, bool thumb);
    void       DumpTrace(const wchar_t* reason);

    // CPU 执行
    int  ExecuteArm();      // 执行一条 ARM 指令，返回周期数
    int  ExecuteThumb();    // 执行一条 Thumb 指令，返回周期数
    void HandleInterrupt(); // 处理中断

    // 定时器
    void TickTimers(int cycles);
    void TimerOverflow(int index);

    // DMA
    void RunDma(int channel);
    void CheckDmaTiming(int timing);

    // SWI 软件模拟（替代缺失的 BIOS）
    // 返回 true 表示已处理，false 表示需跳转 BIOS 向量
    bool HandleSWI(uint8_t id);

    // BIOS 解压缩例程（SWI 0x11/0x12 LZ77UnComp, SWI 0x14/0x15 RLUnComp）
    // 很多商业游戏的音源驱动/音乐数据以这些格式压缩存放在 ROM 中，
    // 若不实现，游戏在启动时调用这些 SWI 会得不到预期的数据/代码，
    // 导致 CPU 后续执行到未初始化（全零）的内存区域。
    void Lz77UnComp(uint32_t src, uint32_t dst);
    void RlUnComp(uint32_t src, uint32_t dst);

    // IO 寄存器处理
    uint8_t  ReadIO(uint32_t addr);
    void     WriteIO(uint32_t addr, uint8_t value);

    // ARM 指令解码辅助
    bool CheckCondition(uint32_t cond);
    void UpdateFlags(uint32_t result, bool carry = false, bool overflow = false);
    uint32_t BarrelShift(uint32_t value, uint32_t shiftType, uint32_t shiftAmt, bool& carry);

    // 开放 BIOS 初始化
    void InitOpenBios();
};