#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GbaEmulator.h"
#include "DebugLog.h"
#include <cstring>
#include <algorithm>
#include <vector>

#define GBA_CYCLES_PER_FRAME    280896
static constexpr int kNopSeaWarnThreshold  = 4096;    // 连续 NOP 达到此值发出警告
static constexpr int kNopSeaFatalThreshold = 65536;   // 连续 NOP 达到此值视为卡死并停机
// GSF 播放机制：entry_point 已由制作工具注入启动桩，CPU 直接从 entry_point 执行，
// 无需扫描 Sappy 函数，无需魔数注入，无需 BX-NULL 修复。

GbaEmulator::GbaEmulator()
    : m_halted(false)
    , m_outputSampleRate(44100)
    , m_cyclesPerFrame(GBA_CYCLES_PER_FRAME)
    , m_cycleAccum(0)
    , m_lastVcount(0xFF)
    , m_frameCount(0)
    , m_lastPC(0)
    , m_nopSeaCount(0)
    , m_nopSeaRescueTotal(0)
    , m_nopSeaFatal(false)
    , m_runawayFatal(false)
    , m_bxBadJumpLogged(false)
    , m_traceIdx(0)
{
    memset(m_loggedSwi, 0, sizeof(m_loggedSwi));
    memset(m_trace, 0, sizeof(m_trace));
    memset(m_ewram, 0, sizeof(m_ewram));
    memset(m_iwram, 0, sizeof(m_iwram));
    memset(m_ioram, 0, sizeof(m_ioram));
    memset(m_palette, 0, sizeof(m_palette));
    memset(m_vram, 0, sizeof(m_vram));
    memset(m_oam, 0, sizeof(m_oam));
    memset(m_bios, 0, sizeof(m_bios));
    for (int i = 0; i < 4; i++) { m_timers[i].Reset(); m_dma[i].Reset(); }
    m_irq.Reset();
    m_romOffset = GBA_ROM_BASE;
    m_entryPoint = GBA_ROM_BASE;
    InitOpenBios();
}
GbaEmulator::~GbaEmulator() {}
void GbaEmulator::InitOpenBios()
{
    uint32_t swiReturn = 0xE1B0F00E;
    memcpy(m_bios + 0x08, &swiReturn, 4);
    uint32_t irqJump = 0xE51FF004;
    memcpy(m_bios + 0x18, &irqJump, 4);
    uint32_t irqHandler = 0x00000128;
    memcpy(m_bios + 0x1C, &irqHandler, 4);
    static const uint32_t irqCode[] = {
        0xE92D500F,  // 0x128: STMFD SP!, {R0-R3,R12,LR}
        0xE59F1024,  // 0x12C: LDR R1, [PC, #0x24]  ; PC流水线=0x134, 目标=0x158
        0xE5910000,  // 0x130: LDR R0, [R1]           ; 读游戏 IRQ handler 地址
        0xE3500000,  // 0x134: CMP R0, #0
        0x0A000002,  // 0x138: BEQ +2*4 → 0x148（skip）
        0xE1A0E00F,  // 0x13C: MOV LR, PC             ; LR=0x144，BX 后返回此处
        0xE12FFF10,  // 0x140: BX R0                  ; 调用游戏 IRQ handler
        0xE59F0010,  // 0x144: LDR R0, [PC, #0x10]   ; PC流水线=0x14C, 目标=0x15C
        0xE1D010B0,  // 0x148: LDRH R1, [R0]          ; 读 IF（0x04000202）
        0xE1C010B0,  // 0x14C: STRH R1, [R0]          ; 写回清 IF
        0xE8BD500F,  // 0x150: LDMFD SP!, {R0-R3,R12,LR}
        0xE25EF004,  // 0x154: SUBS PC, LR, #4        ; RTI：返回被打断指令下一条
        0x03007FFC,  // 0x158: 字面量：游戏 IRQ handler 指针地址（IWRAM）
        0x04000202,  // 0x15C: 字面量：IF 寄存器地址
    };
    memcpy(m_bios + 0x128, irqCode, sizeof(irqCode));
}
bool GbaEmulator::LoadRom(const uint8_t* romData, size_t romSize, uint32_t romOffset, uint32_t entryPoint)
{
    if (!romData || romSize == 0)
        return false;
    m_romOffset = romOffset;
    m_entryPoint = entryPoint;
    size_t maxSize = GBA_ROM_SIZE;
    size_t copySize = (romSize < maxSize) ? romSize : maxSize;
    m_rom.assign(romData, romData + copySize);
    while (m_rom.size() < maxSize)
    {
        size_t remaining = maxSize - m_rom.size();
        size_t chunk = (copySize < remaining) ? copySize : remaining;
        m_rom.insert(m_rom.end(), romData, romData + chunk);
    }
    LogDebug(L"[LoadRom] romSize=0x%X(%u) romOffset=0x%08X entryPoint=0x%08X",
             (unsigned)romSize, (unsigned)romSize, romOffset, entryPoint);
    return true;
}
void GbaEmulator::Init(int outputSampleRate)
{
    m_outputSampleRate = outputSampleRate;
    m_apu.Init(outputSampleRate);
    Reset();
}
void GbaEmulator::Reset()
{
    memset(m_ewram, 0, sizeof(m_ewram));
    memset(m_iwram, 0, sizeof(m_iwram));
    memset(m_ioram, 0, sizeof(m_ioram));
    for (int i = 0; i < 4; i++) { m_timers[i].Reset(); m_dma[i].Reset(); }
    m_irq.Reset();
    m_irq.ime = true;
    m_apu.Reset();
    m_halted = false;
    m_cycleAccum = 0;
    m_lastVcount = 0xFF;
    // 每次 Reset（新曲目 Open，或向后 seek）都清空看门狗/诊断状态，
    // 避免上一首曲目留下的"放弃自救"/"RUNAWAY"标记传染给这一首。
    m_lastPC = 0;
    m_nopSeaCount = 0;
    m_nopSeaRescueTotal = 0;
    m_nopSeaFatal = false;
    m_runawayFatal = false;
    m_bxBadJumpLogged = false;
    memset(m_loggedSwi, 0, sizeof(m_loggedSwi));
    m_traceIdx = 0;
    memset(m_trace, 0, sizeof(m_trace));
    memset(&m_regs, 0, sizeof(m_regs));
    m_regs.cpsr = 0x1F;         // System 模式
    m_regs.SP() = 0x03007F00;   // System 模式 SP（IWRAM 栈顶）
    m_regs.r[13] = 0x03007F00;  // 同上
    m_regs.r13_irq = 0x03007FA0; // GBA BIOS 标准 IRQ 栈顶
    if (!m_rom.empty())
    {
        uint32_t entry = m_entryPoint;
        if (entry & 1)
        {
            m_regs.SetT(true);
            entry &= ~1u;
            m_regs.PC() = entry + 4;
        }
        else
        {
            m_regs.SetT(false);
            m_regs.PC() = entry + 8;
        }
    }
    m_ioram[REG_SOUNDCNT_X] = 0x80;
    m_apu.WriteIO(GBA_IO_BASE + REG_SOUNDCNT_X, 0x80);
    m_ioram[REG_SOUNDBIAS + 1] = 0x02;
    m_ioram[REG_SOUNDCNT_L] = 0x77;
    m_apu.WriteIO(GBA_IO_BASE + REG_SOUNDCNT_L, 0x77);
    WriteIO(GBA_IO_BASE + 0x84, 0x80);
    WriteIO(GBA_IO_BASE + 0x80, 0x77); 
    WriteIO(GBA_IO_BASE + 0x81, 0xFF);
    WriteIO(GBA_IO_BASE + 0x82, 0x0B);
    WriteIO(GBA_IO_BASE + 0x83, 0x0B);
    WriteIO(GBA_IO_BASE + 0x88, 0x00);
    WriteIO(GBA_IO_BASE + 0x89, 0x02); 
}
uint8_t GbaEmulator::ReadByte(uint32_t addr)
{
    uint32_t region = addr >> 24;
    switch (region)
    {
    case 0x00: // BIOS
        if (addr < 0x4000) return m_bios[addr & 0x3FFF];
        return 0;
    case 0x02: // EWRAM
        return m_ewram[addr & (GBA_EWRAM_SIZE - 1)];
    case 0x03: // IWRAM
        return m_iwram[addr & (GBA_IWRAM_SIZE - 1)];
    case 0x04: // IO
        return ReadIO(addr);
    case 0x05: // Palette
        return m_palette[addr & 0x3FF];
    case 0x06: // VRAM
        return m_vram[addr & 0x17FFF];
    case 0x07: // OAM
        return m_oam[addr & 0x3FF];
    case 0x08: case 0x09: // ROM 0
    case 0x0A: case 0x0B: // ROM 1
    case 0x0C: case 0x0D: // ROM 2
        if (!m_rom.empty())
        {
            uint32_t offset = addr - m_romOffset;
            if (offset < m_rom.size())
                return m_rom[offset];
            return m_rom[offset % m_rom.size()];
        }
        return 0;
    default:
        {
            uint32_t masked = addr & 0x0FFFFFFFu;
            if ((masked >> 24) != region)
                return ReadByte(masked);
        }
        return 0;
    }
}
uint16_t GbaEmulator::ReadHalf(uint32_t addr)
{
    addr &= ~1u;
    return (uint16_t)ReadByte(addr) | ((uint16_t)ReadByte(addr + 1) << 8);
}
uint32_t GbaEmulator::ReadWord(uint32_t addr)
{
    uint32_t alignedAddr = addr & ~3u;
    uint32_t val;
    if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D && !m_rom.empty())
    {
        uint32_t offset = alignedAddr - m_romOffset;
        if (offset + 3 < m_rom.size())
        {
            memcpy(&val, m_rom.data() + offset, 4);
        } else {
            val = 0;
        }
    } else {
        val = (uint32_t)ReadByte(alignedAddr) | 
              ((uint32_t)ReadByte(alignedAddr + 1) << 8) |
              ((uint32_t)ReadByte(alignedAddr + 2) << 16) | 
              ((uint32_t)ReadByte(alignedAddr + 3) << 24);
    }
    uint32_t shift = (addr & 3) * 8;
    if (shift != 0) {
        val = (val >> shift) | (val << (32 - shift));
    }
    return val;
}
void GbaEmulator::WriteByte(uint32_t addr, uint8_t value)
{
    uint32_t region = addr >> 24;
    switch (region)
    {
    case 0x02: m_ewram[addr & (GBA_EWRAM_SIZE - 1)] = value; break;
    case 0x03:
    {
        uint32_t iwramAddr = addr & (GBA_IWRAM_SIZE - 1);
        m_iwram[iwramAddr] = value;
        break;
    }
    case 0x04: WriteIO(addr, value); break;
    case 0x05: m_palette[addr & 0x3FF] = value; break;
    case 0x06: m_vram[addr & 0x17FFF] = value; break;
    case 0x07: m_oam[addr & 0x3FF] = value; break;
    default:
        {
            uint32_t masked = addr & 0x0FFFFFFFu;
            if ((masked >> 24) != region)
                WriteByte(masked, value);
        }
        break;
    }
}
void GbaEmulator::WriteHalf(uint32_t addr, uint16_t value)
{
    addr &= ~1u;
    WriteByte(addr,     (uint8_t)(value & 0xFF));
    WriteByte(addr + 1, (uint8_t)(value >> 8));
}
void GbaEmulator::WriteWord(uint32_t addr, uint32_t value)
{
    addr &= ~3u;
    WriteByte(addr,     (uint8_t)(value & 0xFF));
    WriteByte(addr + 1, (uint8_t)((value >> 8) & 0xFF));
    WriteByte(addr + 2, (uint8_t)((value >> 16) & 0xFF));
    WriteByte(addr + 3, (uint8_t)((value >> 24) & 0xFF));
}
uint8_t GbaEmulator::ReadIO(uint32_t addr)
{
    uint32_t reg = addr & 0x3FF;
    if (reg >= 0x100 && reg <= 0x10F)
    {
        int timerIdx  = (reg - 0x100) >> 2;   // 0..3（4 个 Timer）
        int timerByte = (reg - 0x100) & 3;     // 0=CNT_L低, 1=CNT_L高, 2=CNT_H低, 3=CNT_H高
        if (timerIdx < 4)
        {
            if (timerByte == 0)
                return (uint8_t)(m_timers[timerIdx].counter & 0xFF);
            if (timerByte == 1)
                return (uint8_t)((m_timers[timerIdx].counter >> 8) & 0xFF);
        }
    }
    if (reg == 0x200) return (uint8_t)(m_irq.ie & 0xFF);
    if (reg == 0x201) return (uint8_t)((m_irq.ie >> 8) & 0xFF);
    if (reg == 0x202) return (uint8_t)(m_irq.ifl & 0xFF);        // ← 关键：返回实时 IF
    if (reg == 0x203) return (uint8_t)((m_irq.ifl >> 8) & 0xFF); // ← 关键：返回实时 IF 高字节
    if (reg == 0x208) return (uint8_t)(m_irq.ime ? 1 : 0);
    if (reg == 0x209) return 0;
    if (reg < sizeof(m_ioram))
        return m_ioram[reg];
    return 0;
}
void GbaEmulator::WriteIO(uint32_t addr, uint8_t value)
{
    uint32_t reg = addr & 0x3FF;
    if (reg < sizeof(m_ioram))
    {
        if (reg == 0x006 || reg == 0x007)
        {
            return;
        }
        m_ioram[reg] = value;
    }
    {
        bool logThis = (reg >= 0x060 && reg <= 0x0DF) ||  // APU + DMA
                       (reg >= 0x100 && reg <= 0x10F) ||  // 定时器
                       reg == 0x200 || reg == 0x201 ||    // IE
                       reg == 0x202 || reg == 0x203 ||    // IF
                       reg == 0x208 || reg == 0x209 ||    // IME
                       (m_frameCount < 2);                // 前 2 帧记录全部
        if (logThis)
        {
        }
    }
    if (reg >= 0x060 && reg < 0x0B0)
    {
        m_apu.WriteIO(addr, value);
        if (reg == REG_SOUNDCNT_X && (value & 0x80)) {
        }
    }
    if (reg >= 0x0A0 && reg <= 0x0A3) m_apu.GetFifoA().Push((int8_t)value);
    if (reg >= 0x0A4 && reg <= 0x0A7) m_apu.GetFifoB().Push((int8_t)value);
    if (reg >= 0x0B0 && reg < 0x0E0)
    {
        int ch  = (reg - 0x0B0) / 0x0C;
        int off = (reg - 0x0B0) % 0x0C;
        if (ch >= 0 && ch < 4)
        {
            switch (off)
            {
            case 0: m_dma[ch].src = (m_dma[ch].src & 0xFFFFFF00u) | value; break;
            case 1: m_dma[ch].src = (m_dma[ch].src & 0xFFFF00FFu) | ((uint32_t)value << 8); break;
            case 2: m_dma[ch].src = (m_dma[ch].src & 0xFF00FFFFu) | ((uint32_t)value << 16); break;
            case 3: m_dma[ch].src = (m_dma[ch].src & 0x00FFFFFFu) | ((uint32_t)value << 24); break;
            case 4: m_dma[ch].dst = (m_dma[ch].dst & 0xFFFFFF00u) | value; break;
            case 5: m_dma[ch].dst = (m_dma[ch].dst & 0xFFFF00FFu) | ((uint32_t)value << 8); break;
            case 6: m_dma[ch].dst = (m_dma[ch].dst & 0xFF00FFFFu) | ((uint32_t)value << 16); break;
            case 7: m_dma[ch].dst = (m_dma[ch].dst & 0x00FFFFFFu) | ((uint32_t)value << 24); break;
            case 8:  m_dma[ch].count = (m_dma[ch].count & 0xFF00u) | value; break;
            case 9:  m_dma[ch].count = (m_dma[ch].count & 0x00FFu) | ((uint16_t)value << 8); break;
            case 10: m_dma[ch].control = (m_dma[ch].control & 0xFF00u) | value; break;
            case 11:
            {
                bool wasEnabled = m_dma[ch].enabled;
                m_dma[ch].control = (m_dma[ch].control & 0x00FFu) | ((uint16_t)value << 8);
                m_dma[ch].dst_ctrl   = (m_dma[ch].control >> 5)  & 3;
                m_dma[ch].src_ctrl   = (m_dma[ch].control >> 7)  & 3;
                m_dma[ch].repeat     = (m_dma[ch].control >> 9)  & 1;
                m_dma[ch].word_size  = (m_dma[ch].control >> 10) & 1;
                m_dma[ch].timing     = (m_dma[ch].control >> 12) & 3;
                m_dma[ch].irq_enable = (m_dma[ch].control >> 14) & 1;
                m_dma[ch].enabled    = (m_dma[ch].control >> 15) & 1;
                if (!wasEnabled && m_dma[ch].enabled)
                {
                    m_dma[ch].src_latch = m_dma[ch].src;
                    m_dma[ch].dst_latch = m_dma[ch].dst;
                    if (m_dma[ch].timing == 0)      // 立即传输
                        RunDma(ch);
                }
                break;
            }
            }
        }
    }
    if (reg >= 0x100 && reg < 0x110)
    {
        int timerIdx = (reg - 0x100) >> 2;
        int timerReg = (reg - 0x100) & 3;
        if (timerIdx < 4)
        {
            if (timerReg == 0) // 低字节重载值
            {
                m_timers[timerIdx].reload = (m_timers[timerIdx].reload & 0xFF00) | value;
                if (m_timers[timerIdx].enabled)
                    m_timers[timerIdx].counter =
                        (m_timers[timerIdx].counter & 0xFF00) | value;
            }
            else if (timerReg == 1) // 高字节重载值
            {
                m_timers[timerIdx].reload = (m_timers[timerIdx].reload & 0x00FF) | ((uint16_t)value << 8);
                if (m_timers[timerIdx].enabled)
                    m_timers[timerIdx].counter =
                        (m_timers[timerIdx].counter & 0x00FF) | ((uint16_t)value << 8);
            }
            else if (timerReg == 2) // 控制寄存器低字节
            {
                static const int prescalers[4] = {1, 64, 256, 1024};
                bool wasEnabled = m_timers[timerIdx].enabled;
                m_timers[timerIdx].prescaler = prescalers[value & 3];
                m_timers[timerIdx].cascade   = (value >> 2) & 1;
                m_timers[timerIdx].irq_enable = (value >> 6) & 1;
                m_timers[timerIdx].enabled   = (value >> 7) & 1;
                if (!wasEnabled && m_timers[timerIdx].enabled)
                {
                    m_timers[timerIdx].counter = m_timers[timerIdx].reload;
                }
            }
        }
    }
    if (reg == 0x200) m_irq.ie = (m_irq.ie & 0xFF00) | value;
    if (reg == 0x201) m_irq.ie = (m_irq.ie & 0x00FF) | ((uint16_t)value << 8);
    if (reg == 0x202) m_irq.ifl &= ~value;  // 写1清除
    if (reg == 0x203) m_irq.ifl &= ~((uint16_t)value << 8);
    if (reg == 0x208) m_irq.ime = value & 1;
    if (reg == 0x301 && (value & 0x80) == 0)
        m_halted = true;
}
void GbaEmulator::TickTimers(int cycles)
{
    for (int i = 0; i < 4; i++)
    {
        if (!m_timers[i].enabled || m_timers[i].cascade)
            continue;
        m_timers[i].prescaler_cnt += cycles;
        while (m_timers[i].prescaler_cnt >= m_timers[i].prescaler)
        {
            m_timers[i].prescaler_cnt -= m_timers[i].prescaler;
            uint16_t prev = m_timers[i].counter;
            m_timers[i].counter++;
            if (m_timers[i].counter == 0)
            {
                m_timers[i].counter = m_timers[i].reload;
                TimerOverflow(i);
            }
        }
    }
}
void GbaEmulator::TimerOverflow(int index)
{
    m_apu.OnTimerOverflow(index);
    for (int ch = 0; ch < 4; ch++)
    {
        if (!m_dma[ch].enabled || m_dma[ch].timing != 3) continue;
        bool isFifoA = (m_dma[ch].dst == 0x040000A0u || m_dma[ch].dst_latch == 0x040000A0u);
        bool isFifoB = (m_dma[ch].dst == 0x040000A4u || m_dma[ch].dst_latch == 0x040000A4u);
        bool shouldFire = false;
        if (isFifoA && (int)m_apu.GetFifoA().timer_select == index) shouldFire = true;
        if (isFifoB && (int)m_apu.GetFifoB().timer_select == index) shouldFire = true;
        if (shouldFire)
        {
            RunDma(ch);
        }
    }
    if (index < 3 && m_timers[index + 1].enabled && m_timers[index + 1].cascade)
    {
        m_timers[index + 1].counter++;
        if (m_timers[index + 1].counter == 0)
        {
            m_timers[index + 1].counter = m_timers[index + 1].reload;
            TimerOverflow(index + 1);
        }
    }
    if (m_timers[index].irq_enable)
    {
        m_irq.ifl |= (1 << (3 + index));
        if (m_irq.ime && (m_irq.ie & m_irq.ifl))
            m_halted = false;
    }
}
bool GbaEmulator::CheckCondition(uint32_t cond)
{
    switch (cond)
    {
    case 0x0: return m_regs.Z();
    case 0x1: return !m_regs.Z();
    case 0x2: return m_regs.C();
    case 0x3: return !m_regs.C();
    case 0x4: return m_regs.N();
    case 0x5: return !m_regs.N();
    case 0x6: return m_regs.V();
    case 0x7: return !m_regs.V();
    case 0x8: return m_regs.C() && !m_regs.Z();
    case 0x9: return !m_regs.C() || m_regs.Z();
    case 0xA: return m_regs.N() == m_regs.V();
    case 0xB: return m_regs.N() != m_regs.V();
    case 0xC: return !m_regs.Z() && (m_regs.N() == m_regs.V());
    case 0xD: return m_regs.Z() || (m_regs.N() != m_regs.V());
    case 0xE: return true;
    default:  return false;
    }
}
uint32_t GbaEmulator::BarrelShift(uint32_t value, uint32_t shiftType,
                                   uint32_t shiftAmt, bool& carry)
{
    if (shiftAmt == 0) return value;
    switch (shiftType)
    {
    case 0: // LSL
        if (shiftAmt >= 32) { carry = (shiftAmt == 32) ? (value & 1) : false; return 0; }
        carry = (value >> (32 - shiftAmt)) & 1;
        return value << shiftAmt;
    case 1: // LSR
        if (shiftAmt >= 32) { carry = (shiftAmt == 32) ? ((value >> 31) & 1) : false; return 0; }
        carry = (value >> (shiftAmt - 1)) & 1;
        return value >> shiftAmt;
    case 2: // ASR
        if (shiftAmt >= 32) { carry = (value >> 31) & 1; return carry ? 0xFFFFFFFF : 0; }
        carry = (value >> (shiftAmt - 1)) & 1;
        return (uint32_t)((int32_t)value >> shiftAmt);
    case 3: // ROR
        shiftAmt &= 31;
        if (shiftAmt == 0) return value;
        carry = (value >> (shiftAmt - 1)) & 1;
        return (value >> shiftAmt) | (value << (32 - shiftAmt));
    }
    return value;
}
int GbaEmulator::ExecuteArm()
{
    uint32_t pc = m_regs.PC() - 8; // ARM 流水线：PC 超前 8 字节
    uint32_t instr = ReadWord(pc);
    m_regs.PC() += 4;
    auto GetReg = [&](uint32_t idx) -> uint32_t {
        return (idx == 15u) ? (pc + 8u) : m_regs.r[idx];
    };
    uint32_t cond = instr >> 28;
    if (!CheckCondition(cond))
        return 1;
    uint32_t type = (instr >> 25) & 7;

    if ((instr & 0x0E000000) == 0x0A000000)
    {
        int32_t offset = (int32_t)(instr << 8) >> 6; // 符号扩展并 <<2
        uint32_t target = (pc + 8) + (uint32_t)offset; // ARM PC_pipeline + offset
        if (instr & (1 << 24)) // BL：LR = 当前指令的下一条地址
            m_regs.LR() = pc + 4;
        m_regs.PC() = target + 8; // 补偿流水线：下次 -8 后 = target
        return 3;
    }
    if ((instr & 0x0FFFFFF0) == 0x012FFF10 ||
        (instr & 0x0FFFFFF0) == 0x012FFF30) // BLX
    {
        uint32_t rn = instr & 0xF;
        uint32_t addr = m_regs.r[rn];
        bool isBLX = ((instr & 0x0FFFFFF0) == 0x012FFF30);
        if (isBLX)
            m_regs.LR() = pc + 4;
        uint32_t target = addr & ~1u;
        if (addr & 1)
        {
            m_regs.SetT(true);
            m_regs.PC() = target + 4;
        }
        else
        {
            m_regs.SetT(false);
            m_regs.PC() = target + 8;
        }
        return 3;
    }
    if ((instr & 0x0FC000F0) == 0x00000090)
    {
        bool A = (instr >> 21) & 1;
        bool S = (instr >> 20) & 1;
        uint32_t rd = (instr >> 16) & 0xF; // 目标寄存器
        uint32_t rn = (instr >> 12) & 0xF; // 累加值（MLA）
        uint32_t rs = (instr >> 8)  & 0xF;
        uint32_t rm = instr & 0xF;
        uint32_t result = m_regs.r[rm] * m_regs.r[rs];
        if (A) result += m_regs.r[rn];
        m_regs.r[rd] = result;
        if (S)
        {
            m_regs.SetN(result >> 31);
            m_regs.SetZ(result == 0);
        }
        return 2;
    }
    if ((instr & 0x0F8000F0) == 0x00800090)
    {
        bool signedMul = (instr >> 22) & 1; // U: 0=无符号，1=有符号
        bool A          = (instr >> 21) & 1;
        bool S          = (instr >> 20) & 1;
        uint32_t rdHi = (instr >> 16) & 0xF;
        uint32_t rdLo = (instr >> 12) & 0xF;
        uint32_t rs   = (instr >> 8)  & 0xF;
        uint32_t rm   = instr & 0xF;
        uint64_t result;
        if (signedMul)
        {
            int64_t a = (int64_t)(int32_t)m_regs.r[rm];
            int64_t b = (int64_t)(int32_t)m_regs.r[rs];
            result = (uint64_t)(a * b);
        }
        else
        {
            result = (uint64_t)m_regs.r[rm] * (uint64_t)m_regs.r[rs];
        }
        if (A)
        {
            uint64_t acc = ((uint64_t)m_regs.r[rdHi] << 32) | (uint64_t)m_regs.r[rdLo];
            result += acc;
        }
        m_regs.r[rdLo] = (uint32_t)(result & 0xFFFFFFFFu);
        m_regs.r[rdHi] = (uint32_t)(result >> 32);
        if (S)
        {
            m_regs.SetN((result >> 63) & 1);
            m_regs.SetZ(result == 0);
        }
        return 3;
    }
    if ((instr & 0x0FB00FF0) == 0x01000090)
    {
        bool byteSwap = (instr >> 22) & 1;
        uint32_t rn = (instr >> 16) & 0xF;
        uint32_t rd = (instr >> 12) & 0xF;
        uint32_t rm = instr & 0xF;
        uint32_t addr = m_regs.r[rn];
        if (byteSwap)
        {
            uint8_t oldVal = ReadByte(addr);
            WriteByte(addr, (uint8_t)m_regs.r[rm]);
            m_regs.r[rd] = oldVal;
        }
        else
        {
            uint32_t oldVal = ReadWord(addr);
            WriteWord(addr, m_regs.r[rm]);
            m_regs.r[rd] = oldVal;
        }
        return 4;
    }
    if ((instr & 0x0FBF0FFF) == 0x010F0000)
    {
        bool R = (instr >> 22) & 1;
        uint32_t rd = (instr >> 12) & 0xF;
        m_regs.r[rd] = R ? m_regs.spsr : m_regs.cpsr;
        return 1;
    }
    if ((instr & 0x0DB00000) == 0x01200000)
    {
        bool imm  = (instr >> 25) & 1;
        bool R    = (instr >> 22) & 1;
        uint32_t fieldMask = (instr >> 16) & 0xF;
        uint32_t val;
        if (imm)
        {
            uint32_t rot = ((instr >> 8) & 0xF) * 2;
            uint32_t imm8 = instr & 0xFF;
            val = rot ? ((imm8 >> rot) | (imm8 << (32 - rot))) : imm8;
        }
        else
        {
            val = m_regs.r[instr & 0xF];
        }
        uint32_t mask = 0;
        if (fieldMask & 1) mask |= 0x000000FFu; // control 字段 (c)
        if (fieldMask & 2) mask |= 0x0000FF00u; // 扩展字段 (x)
        if (fieldMask & 4) mask |= 0x00FF0000u; // 状态字段 (s)
        if (fieldMask & 8) mask |= 0xFF000000u; // 标志字段 (f)
        if (R) m_regs.spsr = (m_regs.spsr & ~mask) | (val & mask);
        else   m_regs.cpsr = (m_regs.cpsr & ~mask) | (val & mask);
        return 1;
    }
    if ((instr & 0x0E000090) == 0x00000090 && (((instr >> 5) & 3) != 0))
    {
        bool pre    = (instr >> 24) & 1;
        bool up     = (instr >> 23) & 1;
        bool immOff = (instr >> 22) & 1; // I：1=立即数偏移，0=寄存器偏移
        bool wb     = (instr >> 21) & 1;
        bool load   = (instr >> 20) & 1;
        uint32_t rn = (instr >> 16) & 0xF;
        uint32_t rd = (instr >> 12) & 0xF;
        uint32_t sh = (instr >> 5)  & 3;
        uint32_t offset = immOff
            ? (((instr >> 4) & 0xF0) | (instr & 0xF))
            : m_regs.r[instr & 0xF];
        uint32_t base = GetReg(rn);
        if (rn == 15) base &= ~1u;
        uint32_t addr = base;
        if (pre) addr = up ? addr + offset : addr - offset;
        if (load)
        {
            uint32_t val;
            uint16_t raw = ReadHalf(addr);
            switch (sh)
            {
            case 1:  val = (uint32_t)raw; break;                               // LDRH
            case 2:  val = (uint32_t)(int32_t)(int8_t)ReadByte(addr); break;              // LDRSB
            case 3:  val = (uint32_t)(int32_t)(int16_t)raw; break;// LDRSH
            default: val = 0; break;
            }
            m_regs.r[rd] = val;
        }
        else
        {
            WriteHalf(addr, (uint16_t)m_regs.r[rd]);
        }
        if (!pre) addr = up ? base + offset : base - offset;
        if (wb || !pre) m_regs.r[rn] = addr;
        return load ? 3 : 2;
    }
    if ((instr & 0x0C000000) == 0x00000000)
    {
        uint32_t opcode = (instr >> 21) & 0xF;
        uint32_t s      = (instr >> 20) & 1;
        uint32_t rn     = (instr >> 16) & 0xF;
        uint32_t rd     = (instr >> 12) & 0xF;
        bool     imm    = (instr >> 25) & 1;
        uint32_t op2;
        bool carry = m_regs.C();
        if (imm)
        {
            uint32_t rot = ((instr >> 8) & 0xF) * 2;
            op2 = instr & 0xFF;
            if (rot) op2 = (op2 >> rot) | (op2 << (32 - rot));
        }
        else
        {
            uint32_t rm = instr & 0xF;
            uint32_t shiftType = (instr >> 5) & 3;
            uint32_t shiftAmt;
            if (instr & (1 << 4))
                shiftAmt = m_regs.r[(instr >> 8) & 0xF] & 0xFF;
            else
                shiftAmt = (instr >> 7) & 0x1F;
            op2 = BarrelShift(GetReg(rm), shiftType, shiftAmt, carry);
        }
        uint32_t rnVal = GetReg(rn);
        uint32_t result = 0;
        bool writeResult = true;
        bool aluCarry = m_regs.C();
        bool aluOverflow = m_regs.V();
        uint64_t res64 = 0;
        switch (opcode)
        {
        case 0x0: result = rnVal & op2; break;  // AND
        case 0x1: result = rnVal ^ op2; break;  // EOR
        case 0x2: result = rnVal - op2; aluCarry = (rnVal >= op2);aluOverflow = ((rnVal ^ op2) & (rnVal ^ result)) >> 31;break;  // SUB
        case 0x3: result = op2 - rnVal; aluCarry = (op2 >= rnVal);aluOverflow = ((op2 ^ rnVal) & (op2 ^ result)) >> 31;break;  // RSB
        case 0x4: res64 = (uint64_t)rnVal + op2; result = (uint32_t)res64; aluCarry = (res64 > 0xFFFFFFFFu);aluOverflow = (~(rnVal ^ op2) & (rnVal ^ result)) >> 31;break;  // ADD
        case 0x5: result = rnVal + op2 + (uint32_t)m_regs.C(); break; // ADC
        case 0x6: result = rnVal - op2 - 1 + (uint32_t)m_regs.C(); break; // SBC
        case 0x7: result = op2 - rnVal - 1 + (uint32_t)m_regs.C(); break; // RSC
        case 0x8: result = rnVal & op2; writeResult = false; break; // TST
        case 0x9: result = rnVal ^ op2; writeResult = false; break; // TEQ
        case 0xA: result = rnVal - op2; aluCarry = (rnVal >= op2);aluOverflow = ((rnVal ^ op2) & (rnVal ^ result)) >> 31;writeResult = false; break; // CMP
        case 0xB: res64 = (uint64_t)rnVal + op2;result = (uint32_t)res64; aluCarry = (res64 > 0xFFFFFFFFu);aluOverflow = (~(rnVal ^ op2) & (rnVal ^ result)) >> 31;writeResult = false; break; // CMN
        case 0xC: result = rnVal | op2; break;  // ORR
        case 0xD: result = op2; break;           // MOV
        case 0xE: result = rnVal & ~op2; break;  // BIC
        case 0xF: result = ~op2; break;          // MVN
        }
        if (s)
        {
            m_regs.SetN(result >> 31);
            m_regs.SetZ(result == 0);
            if (opcode >= 0x2 && opcode <= 0x7 || opcode == 0xA || opcode == 0xB) {
                m_regs.SetC(aluCarry); // 算术指令使用减法器产生的 Carry
                m_regs.SetV(aluOverflow);
            } else {
                m_regs.SetC(carry);    // 逻辑指令使用移位器产生的 Carry
            }
        }
        if (writeResult)
        {
            m_regs.r[rd] = result;
            if (rd == 15)
            {
                if (s)
                {
                    uint32_t spsr_val = m_regs.spsr;
                    m_regs.SwitchMode(spsr_val & 0x1F); // 先切换 banked 寄存器组
                    m_regs.cpsr = spsr_val;            // 再完整覆盖 CPSR（含 T/I/F 位）
                    bool thumb_ret = (spsr_val >> 5) & 1;
                    if (thumb_ret)
                    {
                        m_regs.SetT(true);
                        m_regs.PC() = (result & ~1u) + 4;  // Thumb 流水线补偿
                    }
                    else
                    {
                        m_regs.SetT(false);
                        m_regs.PC() = (result & ~3u) + 8;  // ARM 流水线补偿
                    }
                }
                else
                {
                    uint32_t target = result & ~3u;
                    if (result & 1) { m_regs.SetT(true);  m_regs.PC() = target + 4; }
                    else            { m_regs.SetT(false); m_regs.PC() = target + 8; }
                }
            }
        }
        return 1;
    }
    if ((instr & 0x0C000000) == 0x04000000)
    {
        uint32_t rn     = (instr >> 16) & 0xF;
        uint32_t rd     = (instr >> 12) & 0xF;
        bool     load   = (instr >> 20) & 1;
        bool     byte   = (instr >> 22) & 1;
        bool     up     = (instr >> 23) & 1;
        bool     pre    = (instr >> 24) & 1;
        bool     wb     = (instr >> 21) & 1;
        bool     imm    = !((instr >> 25) & 1);
        uint32_t offset;
        bool carry = m_regs.C();
        if (imm)
            offset = instr & 0xFFF;
        else
        {
            uint32_t rm = instr & 0xF;
            uint32_t shiftType = (instr >> 5) & 3;
            uint32_t shiftAmt  = (instr >> 7) & 0x1F;
            offset = BarrelShift(GetReg(rm), shiftType, shiftAmt, carry);
        }
        uint32_t addr = GetReg(rn);
        if (pre) addr = up ? addr + offset : addr - offset;
        if (load)
        {
            m_regs.r[rd] = byte ? ReadByte(addr) : ReadWord(addr);
            if (rd == 15)
            {
                uint32_t val = m_regs.r[rd];
                if (val & 1) { m_regs.SetT(true);  m_regs.PC() = (val & ~1u) + 4; }
                else         { m_regs.SetT(false); m_regs.PC() = (val & ~3u) + 8; }
            }
        }
        else
        {
            if (byte) WriteByte(addr, (uint8_t)m_regs.r[rd]);
            else      WriteWord(addr, m_regs.r[rd]);
        }
        if (!pre) addr = up ? addr + offset : addr - offset;
        if (wb || !pre) m_regs.r[rn] = addr;
        return load ? 3 : 2;
    }
    if ((instr & 0x0E000000) == 0x08000000)
    {
        uint32_t rn    = (instr >> 16) & 0xF;
        bool     load  = (instr >> 20) & 1;
        bool     up    = (instr >> 23) & 1;
        bool     pre   = (instr >> 24) & 1;
        bool     wb    = (instr >> 21) & 1;
        bool     s_bit = (instr >> 22) & 1;  // [v8 修复] S bit：LDM S=1+PC = exception return
        uint16_t rlist = instr & 0xFFFF;
        bool     pc_in_list = (rlist >> 15) & 1;
        uint32_t base = GetReg(rn);
        int count = 0;
        for (int i = 0; i < 16; i++) if (rlist & (1 << i)) count++;
        uint32_t addr;
        if (up)
            addr = pre ? base + 4 : base;
        else
            addr = pre ? base - (uint32_t)(count * 4)
                       : base - (uint32_t)(count * 4) + 4;
        for (int i = 0; i < 16; i++)
        {
            if (!(rlist & (1 << i))) continue;
            if (load)
            {
                m_regs.r[i] = ReadWord(addr);
                if (i == 15)
                {
                    uint32_t val = m_regs.r[15];
                    if (s_bit && pc_in_list)
                    {
                        uint32_t spsr_val = m_regs.spsr;
                        m_regs.SwitchMode(spsr_val & 0x1F);
                        m_regs.cpsr = spsr_val;
                        bool thumb_ret = (spsr_val >> 5) & 1;
                        if (thumb_ret)
                        {
                            m_regs.SetT(true);
                            m_regs.PC() = (val & ~1u) + 4;
                        }
                        else
                        {
                            m_regs.SetT(false);
                            m_regs.PC() = (val & ~3u) + 8;
                        }
                    }
                    else
                    {
                        if (val & 1) { m_regs.SetT(true);  m_regs.PC() = (val & ~1u) + 4; }
                        else         { m_regs.SetT(false); m_regs.PC() = (val & ~3u) + 8; }
                    }
                }
            }
            else
                WriteWord(addr, m_regs.r[i]);
            addr += 4; // 永远沿升序方向遍历块
        }
        if (wb)
            m_regs.r[rn] = up ? base + (uint32_t)(count * 4)
                               : base - (uint32_t)(count * 4);
        return count + 1;
    }
    if ((instr & 0x0F000000) == 0x0F000000)
    {
        uint8_t swiId = (uint8_t)((instr >> 16) & 0xFF);
        if (!HandleSWI(swiId))
        {
            m_regs.LR() = pc + 4;
            m_regs.SetT(false);
            m_regs.PC() = 0x00000008 + 8;
        }
        return 3;
    }
    return 1;
}
int GbaEmulator::ExecuteThumb()
{
    uint32_t pc = m_regs.PC() - 4; // Thumb 流水线：PC 超前 4 字节
    uint16_t instr = ReadHalf(pc);
    m_regs.PC() += 2;
    uint32_t op = instr >> 13;
    if (op == 0 && ((instr >> 11) & 3) != 3)
    {
        uint32_t shiftType = (instr >> 11) & 3;
        uint32_t shiftAmt  = (instr >> 6) & 0x1F;
        uint32_t rs = (instr >> 3) & 7;
        uint32_t rd = instr & 7;
        bool carry = m_regs.C();
        m_regs.r[rd] = BarrelShift(m_regs.r[rs], shiftType, shiftAmt, carry);
        m_regs.SetN(m_regs.r[rd] >> 31);
        m_regs.SetZ(m_regs.r[rd] == 0);
        m_regs.SetC(carry);
        return 1;
    }
    if ((instr >> 11) == 0x3)
    {
        bool sub = (instr >> 9) & 1;
        bool imm = (instr >> 10) & 1;
        uint32_t rs = (instr >> 3) & 7;
        uint32_t rd = instr & 7;
        uint32_t rn  = m_regs.r[rs];
        uint32_t op2 = imm ? ((instr >> 6) & 7) : m_regs.r[(instr >> 6) & 7];
        if (sub)
        {
            m_regs.r[rd] = rn - op2;
            m_regs.SetC(rn >= op2);
            m_regs.SetV(((rn ^ op2) & (rn ^ m_regs.r[rd])) >> 31);
        }
        else
        {
            uint64_t res64 = (uint64_t)rn + op2;
            m_regs.r[rd] = (uint32_t)res64;
            m_regs.SetC(res64 > 0xFFFFFFFFu);
            m_regs.SetV((~(rn ^ op2) & (rn ^ m_regs.r[rd])) >> 31);
        }
        m_regs.SetN(m_regs.r[rd] >> 31);
        m_regs.SetZ(m_regs.r[rd] == 0);
        return 1;
    }
    if (op == 1)
    {
        uint32_t opcode = (instr >> 11) & 3;
        uint32_t rd     = (instr >> 8) & 7;
        uint32_t imm8   = instr & 0xFF;
        uint32_t rn     = m_regs.r[rd];
        switch (opcode)
        {
        case 0: // MOV：只更新 N/Z（无 C/V）
            m_regs.r[rd] = imm8;
            m_regs.SetN(0);
            m_regs.SetZ(imm8 == 0);
            return 1;
        case 1: // CMP：减法，更新全部 N/Z/C/V，不写回
        {
            uint32_t r = rn - imm8;
            m_regs.SetN(r >> 31);
            m_regs.SetZ(r == 0);
            m_regs.SetC(rn >= imm8);                          // 无借位
            m_regs.SetV(((rn ^ imm8) & (rn ^ r)) >> 31);     // 有符号溢出
            return 1;
        }
        case 2: // ADDS：加法，更新 N/Z/C/V，写回
        {
            uint64_t res64 = (uint64_t)rn + imm8;
            m_regs.r[rd] = (uint32_t)res64;
            m_regs.SetN(m_regs.r[rd] >> 31);
            m_regs.SetZ(m_regs.r[rd] == 0);
            m_regs.SetC(res64 > 0xFFFFFFFFu);
            m_regs.SetV((~(rn ^ imm8) & (rn ^ m_regs.r[rd])) >> 31);
            return 1;
        }
        case 3: // SUBS：减法，更新 N/Z/C/V，写回
        {
            m_regs.r[rd] = rn - imm8;
            m_regs.SetN(m_regs.r[rd] >> 31);
            m_regs.SetZ(m_regs.r[rd] == 0);
            m_regs.SetC(rn >= imm8);
            m_regs.SetV(((rn ^ imm8) & (rn ^ m_regs.r[rd])) >> 31);
            return 1;
        }
        }
        return 1;
    }
    if ((instr >> 10) == 0x10)
    {
        uint32_t opcode = (instr >> 6) & 0xF;
        uint32_t rs = (instr >> 3) & 7;
        uint32_t rd = instr & 7;
        uint32_t rn  = m_regs.r[rd];
        uint32_t rop = m_regs.r[rs];
        bool carry = m_regs.C();
        switch (opcode)
        {
        case 0x0: m_regs.r[rd] &= rop; break; // AND
        case 0x1: m_regs.r[rd] ^= rop; break; // EOR
        case 0x2: m_regs.r[rd] = BarrelShift(rn, 0, rop & 0xFF, carry); break; // LSL
        case 0x3: m_regs.r[rd] = BarrelShift(rn, 1, rop & 0xFF, carry); break; // LSR
        case 0x4: m_regs.r[rd] = BarrelShift(rn, 2, rop & 0xFF, carry); break; // ASR
        case 0x5: // ADC: rd = rn + rs + C，更新 N/Z/C/V
        {
            uint64_t res64 = (uint64_t)rn + rop + (uint32_t)carry;
            m_regs.r[rd] = (uint32_t)res64;
            m_regs.SetN(m_regs.r[rd] >> 31);
            m_regs.SetZ(m_regs.r[rd] == 0);
            m_regs.SetC(res64 > 0xFFFFFFFFu);
            m_regs.SetV((~(rn ^ rop) & (rn ^ m_regs.r[rd])) >> 31);
            return 1;
        }
        case 0x6: // SBC: rd = rn - rs - (1-C)，更新 N/Z/C/V
        {
            uint32_t borrow = carry ? 0u : 1u;
            uint64_t res64  = (uint64_t)rn - rop - borrow;
            m_regs.r[rd] = (uint32_t)res64;
            m_regs.SetN(m_regs.r[rd] >> 31);
            m_regs.SetZ(m_regs.r[rd] == 0);
            m_regs.SetC((uint64_t)rn >= (uint64_t)rop + borrow);
            m_regs.SetV(((rn ^ rop) & (rn ^ m_regs.r[rd])) >> 31);
            return 1;
        }
        case 0x7: m_regs.r[rd] = BarrelShift(rn, 3, rop & 0xFF, carry); break; // ROR
        case 0x8: // TST: N/Z 只，不写 C/V
        {
            uint32_t r = rn & rop;
            m_regs.SetN(r >> 31);
            m_regs.SetZ(r == 0);
            return 1;
        }
        case 0x9: // NEG: rd = 0 - rs，更新 N/Z/C/V（等效 RSB #0）
        {
            m_regs.r[rd] = 0u - rop;
            m_regs.SetN(m_regs.r[rd] >> 31);
            m_regs.SetZ(m_regs.r[rd] == 0);
            m_regs.SetC(rop == 0);  // 0-0=0 无借位；其他均借位
            m_regs.SetV((rop & m_regs.r[rd]) >> 31);
            return 1;
        }
        case 0xA: // CMP: rn - rs，不写回，更新 N/Z/C/V
        {
            uint32_t r = rn - rop;
            m_regs.SetN(r >> 31);
            m_regs.SetZ(r == 0);
            m_regs.SetC(rn >= rop);
            m_regs.SetV(((rn ^ rop) & (rn ^ r)) >> 31);
            return 1;
        }
        case 0xB: // CMN: rn + rs，不写回，更新 N/Z/C/V
        {
            uint64_t res64 = (uint64_t)rn + rop;
            uint32_t r = (uint32_t)res64;
            m_regs.SetN(r >> 31);
            m_regs.SetZ(r == 0);
            m_regs.SetC(res64 > 0xFFFFFFFFu);
            m_regs.SetV((~(rn ^ rop) & (rn ^ r)) >> 31);
            return 1;
        }
        case 0xC: m_regs.r[rd] |= rop; break;  // ORR
        case 0xD: m_regs.r[rd] *= rop; break;  // MUL（flag 由下方通用路径设置）
        case 0xE: m_regs.r[rd] &= ~rop; break; // BIC
        case 0xF: m_regs.r[rd] = ~rop; break;  // MVN
        }
        m_regs.SetN(m_regs.r[rd] >> 31);
        m_regs.SetZ(m_regs.r[rd] == 0);
        m_regs.SetC(carry);
        return 1;
    }
    if (op == 3)
    {
        bool     load  = (instr >> 11) & 1;
        bool     byte  = (instr >> 12) & 1;
        uint32_t offset = ((instr >> 6) & 0x1F) << (byte ? 0 : 2);
        uint32_t rb = (instr >> 3) & 7;
        uint32_t rd = instr & 7;
        uint32_t addr = m_regs.r[rb] + offset;
        if (load)
            m_regs.r[rd] = byte ? ReadByte(addr) : ReadWord(addr);
        else
        {
            if (byte) WriteByte(addr, (uint8_t)m_regs.r[rd]);
            else      WriteWord(addr, m_regs.r[rd]);
        }
        return load ? 3 : 2;
    }
    if ((instr >> 9) == 0x5A || (instr >> 9) == 0x5E)
    {
        bool load = (instr >> 11) & 1;
        bool lr_pc = (instr >> 8) & 1;
        uint8_t rlist = instr & 0xFF;
        if (load) // POP
        {
            for (int i = 0; i < 8; i++)
                if (rlist & (1 << i)) { m_regs.r[i] = ReadWord(m_regs.SP()); m_regs.SP() += 4; }
            if (lr_pc)
            {
                uint32_t val = ReadWord(m_regs.SP()); m_regs.SP() += 4;
                if (val & 1) { m_regs.SetT(true);  m_regs.PC() = (val & ~1u) + 4; }
                else         { m_regs.SetT(false); m_regs.PC() = (val & ~3u) + 8; }
            }
        }
        else // PUSH
        {
            if (lr_pc) { m_regs.SP() -= 4; WriteWord(m_regs.SP(), m_regs.LR()); }
            for (int i = 7; i >= 0; i--)
                if (rlist & (1 << i)) { m_regs.SP() -= 4; WriteWord(m_regs.SP(), m_regs.r[i]); }
        }
        return 1;
    }
    if ((instr >> 12) == 0xD)
    {
        uint32_t cond = (instr >> 8) & 0xF;
		if (cond == 0xF) // 1101 1111 iiiiiiii = SWI，不是条件分支
		{
			uint8_t swiId = (uint8_t)(instr & 0xFF);
			if (!HandleSWI(swiId))
			{
				m_regs.LR() = pc + 2;
				m_regs.SetT(false);
				m_regs.PC() = 0x00000008 + 8;
			}
			return 3;
		}
        if (CheckCondition(cond))
        {
            int32_t offset = (int32_t)(int8_t)(instr & 0xFF) * 2;
            uint32_t target = (pc + 4) + (uint32_t)offset;
            m_regs.PC() = target + 4; // 补偿流水线
        }
        return 1;
    }
    if ((instr >> 11) == 0x1C)
    {
        int32_t offset = (int32_t)((instr & 0x7FF) << 21) >> 20;
        uint32_t target = (pc + 4) + (uint32_t)offset;
        m_regs.PC() = target + 4;
        return 1;
    }
    if ((instr >> 11) == 0x1E)
    {
        int32_t offset = (int32_t)((instr & 0x7FF) << 21) >> 9; // SignExtend(imm11)<<12
        m_regs.LR() = (pc + 4) + (uint32_t)offset;
        return 1;
    }
    if ((instr >> 11) == 0x1F)
    {
        uint32_t target = m_regs.LR() + ((instr & 0x7FF) << 1);
        m_regs.LR() = (pc + 2) | 1u; // 返回地址 = 下一条 Thumb 指令地址（bit0=1 标记 Thumb）
        m_regs.SetT(true);            // 始终保持 Thumb 模式
        m_regs.PC() = (target & ~1u) + 4; // 补偿 Thumb 流水线：下次取指 pc = PC-4 = target
        return 3;
    }
    if ((instr >> 11) == 0x1D)
    {
        return 1;
    }
    if ((instr & 0xFF87) == 0x4700)
    {
        uint32_t rs = (instr >> 3) & 0xF;
        uint32_t addr = (rs == 15u) ? (pc + 4u) : m_regs.r[rs];
		if (!m_bxBadJumpLogged)
		{
			uint32_t region = (addr & ~1u) >> 24;
			bool valid = (region==0x00||region==0x02||region==0x03||region==0x04||(region>=0x08&&region<=0x0D));
			if (!valid)
			{
				m_bxBadJumpLogged = true;
				LogDebug(L"[CPU:BADJUMP] BX/BLX R%d=0x%08X at pc=0x%08X(LR调用点=0x%08X) "
                     L"prevInstr[-8..-2]=%04X %04X %04X SP=0x%08X",
                     rs, addr, pc, m_regs.LR(),
                     ReadHalf(pc-8), ReadHalf(pc-4), ReadHalf(pc-2), m_regs.SP());
			}
		}
		uint32_t target = addr & ~1u;
        if (addr & 1) { m_regs.SetT(true);  m_regs.PC() = target + 4; }
        else          { m_regs.SetT(false); m_regs.PC() = target + 8; }
        return 3;
    }
    if ((instr >> 10) == 0x11)
    {
        uint32_t opcode = (instr >> 8) & 3;
        uint32_t rs = (instr >> 3) & 0xF;
        uint32_t rd = (instr & 7) | ((instr >> 4) & 8);
        uint32_t rsVal = (rs == 15u) ? (pc + 4u) : m_regs.r[rs];
        switch (opcode)
        {
        case 0: m_regs.r[rd] += rsVal; break; // ADD
        case 1: // CMP Hi reg：减法，更新 N/Z/C/V，不写回
        {
            uint32_t r = m_regs.r[rd] - rsVal;
            m_regs.SetN(r >> 31);
            m_regs.SetZ(r == 0);
            m_regs.SetC(m_regs.r[rd] >= rsVal);
            m_regs.SetV(((m_regs.r[rd] ^ rsVal) & (m_regs.r[rd] ^ r)) >> 31);
            return 1;
        }
        case 2: m_regs.r[rd] = rsVal; break;  // MOV
        }
        if (rd == 15)
        {
            m_regs.PC() = (m_regs.r[15] & ~1u) + 4;
        }
        return 1;
    }
    if ((instr >> 11) == 0x9)
    {
        uint32_t rd     = (instr >> 8) & 7;
        uint32_t offset = (instr & 0xFF) << 2;
        uint32_t addr = ((m_regs.PC() - 2u) & ~3u) + offset;
        m_regs.r[rd] = ReadWord(addr);
        return 3;
    }
    if ((instr >> 12) == 0x8)
    {
        bool     load   = (instr >> 11) & 1;
        uint32_t offset = ((instr >> 6) & 0x1F) << 1; // imm5 * 2
        uint32_t rb     = (instr >> 3) & 7;
        uint32_t rd     = instr & 7;
        uint32_t addr   = m_regs.r[rb] + offset;
        if (load)
            m_regs.r[rd] = (uint32_t)ReadHalf(addr);
        else
            WriteHalf(addr, (uint16_t)m_regs.r[rd]);
        return load ? 3 : 2;
    }
    if ((instr >> 12) == 0x5 && !((instr >> 9) & 1))
    {
        bool     load = (instr >> 11) & 1;
        bool     byte = (instr >> 10) & 1;
        uint32_t ro   = (instr >> 6) & 7;
        uint32_t rb   = (instr >> 3) & 7;
        uint32_t rd   = instr & 7;
        uint32_t addr = m_regs.r[rb] + m_regs.r[ro];
        if (load)
            m_regs.r[rd] = byte ? (uint32_t)ReadByte(addr) : ReadWord(addr);
        else
        {
            if (byte) WriteByte(addr, (uint8_t)m_regs.r[rd]);
            else      WriteWord(addr, m_regs.r[rd]);
        }
        return load ? 3 : 2;
    }
    if ((instr >> 12) == 0x5 && ((instr >> 9) & 1))
    {
        bool     H    = (instr >> 11) & 1;
        bool     S    = (instr >> 10) & 1;
        uint32_t ro   = (instr >> 6) & 7;
        uint32_t rb   = (instr >> 3) & 7;
        uint32_t rd   = instr & 7;
        uint32_t addr = m_regs.r[rb] + m_regs.r[ro];
        if (!H && !S) // STRH
        {
            WriteHalf(addr, (uint16_t)m_regs.r[rd]);
            return 2;
        }
        else if (!H && S) // LDRSB
        {
            m_regs.r[rd] = (uint32_t)(int32_t)(int8_t)ReadByte(addr);
            return 3;
        }
        else if (H && !S) // LDRH
        {
            m_regs.r[rd] = (uint32_t)ReadHalf(addr);
            return 3;
        }
        else // LDRSH
        {
            m_regs.r[rd] = (uint32_t)(int32_t)(int16_t)ReadHalf(addr);
            return 3;
        }
    }
    if ((instr >> 12) == 0x9)
    {
        bool     load   = (instr >> 11) & 1;
        uint32_t rd     = (instr >> 8) & 7;
        uint32_t offset = (instr & 0xFF) << 2; // imm8 * 4
        uint32_t addr   = m_regs.SP() + offset;
        if (load)
            m_regs.r[rd] = ReadWord(addr);
        else
            WriteWord(addr, m_regs.r[rd]);
        return load ? 3 : 2;
    }
    if ((instr >> 12) == 0xA)
    {
        bool     sp     = (instr >> 11) & 1;
        uint32_t rd     = (instr >> 8) & 7;
        uint32_t offset = (instr & 0xFF) << 2;
        uint32_t base   = sp ? m_regs.SP() : ((pc + 4) & ~3u);
        m_regs.r[rd]    = base + offset;
        return 1;
    }
    if ((instr >> 8) == 0xB0)
    {
        bool     neg    = (instr >> 7) & 1;
        uint32_t offset = (instr & 0x7F) << 2;
        if (neg) m_regs.SP() -= offset;
        else     m_regs.SP() += offset;
        return 1;
    }
    if ((instr >> 12) == 0xC)
    {
        bool     load  = (instr >> 11) & 1;
        uint32_t rb    = (instr >> 8) & 7;
        uint8_t  rlist = (uint8_t)(instr & 0xFF);
        uint32_t addr  = m_regs.r[rb];
        if (load)
        {
            for (int i = 0; i < 8; i++)
                if (rlist & (1 << i)) { m_regs.r[i] = ReadWord(addr); addr += 4; }
        }
        else
        {
            for (int i = 0; i < 8; i++)
                if (rlist & (1 << i)) { WriteWord(addr, m_regs.r[i]); addr += 4; }
        }
        m_regs.r[rb] = addr; // write-back
        return 1;
    }
    LogDebug(L"  [UnknownThumb] PC=0x%08X instr=0x%04X (skipped)", pc, instr);
    return 1;
}
void GbaEmulator::HandleInterrupt()
{
    if (m_halted && m_irq.ifl) {
        m_halted = false;
    }
    uint16_t pending = m_irq.ie & m_irq.ifl;
    if (!m_irq.ime || !pending) return;
    if (m_regs.cpsr & (1u << 7)) return;
    uint32_t oldCpsr = m_regs.cpsr;
    uint32_t returnAddr = m_regs.T() ? m_regs.PC() : m_regs.PC() - 4u;
    uint16_t biosFlags = ReadHalf(0x03007FF8);
    WriteHalf(0x03007FF8, biosFlags | pending);
    m_regs.SwitchMode(0x12); // 切换到 IRQ 模式
    m_regs.spsr = oldCpsr;
    m_regs.SetT(false);
    m_regs.cpsr |= (1u << 7); // 禁用 IRQ（防止嵌套）
    m_regs.r[14] = returnAddr;
    m_regs.r[15] = 0x18 + 8; // 跳转 IRQ 向量 0x00000018，+8 补偿 ARM 流水线
}
int GbaEmulator::RunForSamples(int16_t* buffer, int maxSamples)
{
    if (m_rom.empty()) return 0;
    static constexpr int GBA_CYCLES_PER_LINE = 1232;
    static constexpr int GBA_VBLANK_LINE     = 160;
    static constexpr int GBA_TOTAL_LINES     = 228;
    int samplesGenerated = 0;
    const int maxCyclesPerCall = GBA_CYCLES_PER_FRAME * 4;
    int cyclesThisCall = 0;
    // 注意：NOP-SEA / RUNAWAY / BADJUMP 相关的看门狗状态都已经改成
    // GbaEmulator 的成员变量（m_lastPC / m_nopSeaCount / m_nopSeaFatal /
    // m_runawayFatal 等），在 Reset() 里清零，不再用函数内 static——
    // 后者会被同一进程里所有曲目共用，一首曲子触发了"放弃自救"或
    // "RUNAWAY"，会永久传染给之后播放的所有曲目（包括完全正常的 ROM）。
    // GSF entry_point 已直接编码启动桩，CPU 从 entry_point 执行即可，无需注入
    while (samplesGenerated < maxSamples && cyclesThisCall < maxCyclesPerCall)
    {
        int step = 0;
        if (!m_halted)
        {
            uint32_t currentInstrPC = m_regs.PC() - (m_regs.T() ? 4u : 8u);
            {
                uint32_t region = currentInstrPC >> 24;
                bool validRegion = (region == 0x00 || region == 0x02 || region == 0x03 ||
                                    region == 0x04 || (region >= 0x08 && region <= 0x0D));
                // NOP-SEA 自救彻底放弃后，用和 RUNAWAY 一样的方式提前返回，
                // 避免下面 "VBlank 强制唤醒 CPU" 的逻辑把这个停机状态又撤销掉，
                // 导致 CPU 反复原地踏步、日志被刷屏。
                if (m_nopSeaFatal) { m_halted = true; return 1; }
				if (!validRegion)
                {
                    if (m_runawayFatal) { m_halted = true; return 1; }
					LogDebug(L"[CPU:RUNAWAY] PC=0x%08X REGION=0x%02X T=%d "
                             L"R0=%08X R1=%08X R2=%08X R3=%08X "
                             L"R4=%08X R5=%08X R6=%08X R7=%08X "
                             L"LR=%08X SP=%08X CPSR=%08X "
                             L"IME=%d IE=0x%04X IF=0x%04X "
                             L"Timer0(en=%d reload=0x%04X) Timer1(en=%d reload=0x%04X)",
                             currentInstrPC, region, (int)m_regs.T(),
                             m_regs.r[0], m_regs.r[1], m_regs.r[2], m_regs.r[3],
                             m_regs.r[4], m_regs.r[5], m_regs.r[6], m_regs.r[7],
                             m_regs.LR(), m_regs.SP(), m_regs.cpsr,
                             (int)m_irq.ime, (int)m_irq.ie, (int)m_irq.ifl,
                             (int)m_timers[0].enabled, m_timers[0].reload,
                             (int)m_timers[1].enabled, m_timers[1].reload);
                    m_runawayFatal = true;
                    m_halted = true;
                    DumpTrace(L"RUNAWAY");
					//m_regs.SetT(false);
                    //m_regs.PC() = 0x00000008u + 8u; // BIOS SWI stub: MOV PC, LR
                    //m_nopSeaCount = 0; // 重置 NOP 海计数器
                }
            }
            if (!m_halted && m_regs.T())
            {
                uint16_t fetchedInstr = ReadHalf(currentInstrPC);
                if (fetchedInstr == 0x0000u)
                {
                    // 重要修正：Thumb 下 0x0000 就是 "LSL R0,R0,#0"——一条真正、
                    // 合法、完全无副作用的 NOP。ROM 里出现几十字节连续的 0x0000
                    // 是很正常的事（对齐填充、条件跳过的可选代码块等），CPU 走
                    // 过去不会有任何问题，也不会真的"卡死"——它一定会继续往前
                    // 推进 PC，直到走出这片区域。
                    // 之前把阈值设成 8 步就出手"自救"（伪造 LR 硬跳）太激进了：
                    // Lunar Legend 的实测证明，这里真的只是一段被 bit 测试跳过/
                    // 执行的合法短填充代码，CPU 本可以自己安全地走出去——是我们
                    // 自己的"自救"硬跳把栈弄乱，才引发了后面的 BADJUMP/RUNAWAY。
                    // 现在改成：阈值大幅提高，且不再做任何猜测性质的跳转；
                    // 只有真的长时间（数万步）都走不出去，才说明大概率是真的
                    // 卡在了不可恢复的位置，这时只做安全停机，不再乱跳。
                    m_nopSeaCount++;
                    if (m_nopSeaCount == kNopSeaWarnThreshold)
                    {
                        LogDebug(L"[CPU:NOP-SEA] 连续 %d 步 0x0000 at PC=0x%08X "
                                 L"R0=%08X R1=%08X R2=%08X R3=%08X "
                                 L"LR=%08X SP=%08X CPSR=%08X "
                                 L"IME=%d IE=0x%04X IF=0x%04X "
                                 L"（Thumb 下 0x0000 是合法 NOP，暂不干预，继续观察）",
                                 m_nopSeaCount, currentInstrPC,
                                 m_regs.r[0], m_regs.r[1], m_regs.r[2], m_regs.r[3],
                                 m_regs.LR(), m_regs.SP(), m_regs.cpsr,
                                 (int)m_irq.ime, (int)m_irq.ie, (int)m_irq.ifl);
                        // 打印"刚越过警戒阈值、还完全没有做任何干预"时刻的历史指令，
                        // 方便确认这是不是合法的短填充代码（前后应该都是正常指令）。
                        DumpTrace(L"NOP-SEA达到警戒阈值（未干预）");
                    }
                    else if (m_nopSeaCount == kNopSeaFatalThreshold)
                    {
                        LogDebug(L"[CPU:NOP-SEA] 连续 %d 步 0x0000 仍未走出，"
                                 L"判定为不可恢复的卡死，安全停机（不做任何猜测性跳转）",
                                 m_nopSeaCount);
                        DumpTrace(L"NOP-SEA放弃（安全停机）");
                        m_halted = true;
                        m_nopSeaFatal = true;
                    }
                }
                else
                {
                    if (m_nopSeaCount > 0)
                    {
                        m_nopSeaCount = 0;
                    }
                }
            }
            m_lastPC = currentInstrPC;
            {
                uint32_t rawInstr = m_regs.T() ? (uint32_t)ReadHalf(currentInstrPC) : ReadWord(currentInstrPC);
                PushTrace(currentInstrPC, rawInstr, m_regs.T());
            }
            if (m_regs.T())
                step = ExecuteThumb();
            else
                step = ExecuteArm();
            if (step <= 0) step = 1;
        }
        else
        {
            step = 4; // CPU 暂停，推进时钟
        }
        m_cycleAccum   += step;
        cyclesThisCall += step;
        uint8_t vcount = (uint8_t)((m_cycleAccum / GBA_CYCLES_PER_LINE) % GBA_TOTAL_LINES);
        m_ioram[0x006] = vcount;
        m_ioram[0x007] = 0;
    if (vcount == GBA_VBLANK_LINE && m_lastVcount != GBA_VBLANK_LINE) {
            m_irq.ifl |= 0x0001; // bit0 = VBlank
            m_halted = false; // 强制唤醒
        }
        m_lastVcount = vcount;
        TickTimers(step);
        HandleInterrupt();
        int16_t tempBuf[64];
        int newSamples = m_apu.Tick(step, tempBuf, 32);
        for (int i = 0; i < newSamples && samplesGenerated < maxSamples; i++)
        {
            buffer[samplesGenerated * 2]     = tempBuf[i * 2];
            buffer[samplesGenerated * 2 + 1] = tempBuf[i * 2 + 1];
            samplesGenerated++;
        }
    }
    return samplesGenerated;
}
void GbaEmulator::RunDma(int ch)
{
    GbaDma& d = m_dma[ch];
    if (!d.enabled) return;
    bool isFifoA = (d.dst == 0x040000A0u || d.dst_latch == 0x040000A0u);
    bool isFifoB = (d.dst == 0x040000A4u || d.dst_latch == 0x040000A4u);
    if (d.timing == 3 && (isFifoA || isFifoB))
    {
        DmaFifo& fifo = isFifoA ? m_apu.GetFifoA() : m_apu.GetFifoB();
        if (fifo.NeedsRefill())
        {
            for (int i = 0; i < 4; i++)
            {
                uint32_t data = ReadWord(d.src_latch);
                if (isFifoA) m_apu.WriteFifoA(data);
                else         m_apu.WriteFifoB(data);
                d.src_latch += 4;
            }
        }
    }
    else
    {
        int transferSize = d.word_size ? 4 : 2;
        int count        = d.count ? (int)(uint32_t)d.count : 0x10000;
        for (int i = 0; i < count; i++)
        {
            if (d.word_size)
                WriteWord(d.dst_latch, ReadWord(d.src_latch));
            else
                WriteHalf(d.dst_latch, (uint16_t)ReadHalf(d.src_latch));
            if      (d.src_ctrl == 0) d.src_latch += transferSize;
            else if (d.src_ctrl == 1) d.src_latch -= transferSize;
            if      (d.dst_ctrl == 0 || d.dst_ctrl == 3) d.dst_latch += transferSize;
            else if (d.dst_ctrl == 1)                     d.dst_latch -= transferSize;
        }
        if (!d.repeat)
        {
            d.enabled = false;
        }
        else
        {
            if (d.dst_ctrl == 3) d.dst_latch = d.dst;
        }
    }
}
void GbaEmulator::CheckDmaTiming(int timing)
{
    for (int i = 0; i < 4; i++)
    {
        if (m_dma[i].enabled && m_dma[i].timing == timing)
            RunDma(i);
    }
}
// SWI 0x11/0x12 LZ77UnComp
// 源数据格式（GBATEK 标准 BIOS 压缩格式）：
//   [0]     Reserved(4bit) | Type=1(4bit)
//   [1..3]  解压后大小（24bit，小端）
//   [4..]   压缩数据流：每个 flag 字节的 8 个 bit（MSB 优先）依次表示后面
//           跟着的 8 个块是「原始字节」(0) 还是「引用块」(1)：
//             原始字节：直接复制 1 字节
//             引用块  ：2 字节，[0]高4位=长度-3，[0]低4位<<8|[1]=距离-1，
//                       从已输出的数据中回溯 距离 字节，复制 长度 字节
void GbaEmulator::Lz77UnComp(uint32_t src, uint32_t dst)
{
    uint32_t header = ReadWord(src);
    uint8_t  type   = (uint8_t)((header >> 4) & 0xF);
    uint32_t size   = header >> 8;
    if (type != 1 || size == 0)
    {
        LogDebug(L"[LZ77UnComp] 头部异常 type=%d size=%u（应为 type=1），已跳过", type, size);
        return;
    }

    uint32_t srcPos = src + 4;
    uint32_t dstPos = dst;
    uint32_t written = 0;
    // 极端/损坏数据兜底：解压出的数据量不应超出声明大小的合理上限
    const uint32_t kMaxOutput = size;

    while (written < kMaxOutput)
    {
        uint8_t flags = ReadByte(srcPos++);
        for (int bit = 7; bit >= 0 && written < kMaxOutput; bit--)
        {
            if (flags & (1 << bit))
            {
                uint8_t b0 = ReadByte(srcPos++);
                uint8_t b1 = ReadByte(srcPos++);
                uint32_t length = (uint32_t)(b0 >> 4) + 3;
                uint32_t disp   = ((uint32_t)(b0 & 0xF) << 8 | b1) + 1;
                if (disp > (dstPos - dst) + 1)
                {
                    // 引用越界（数据损坏或参数解析错误），停止以免写坏内存
                    LogDebug(L"[LZ77UnComp] 引用距离越界 disp=%u 当前已输出=%u，中止解压",
                             disp, written);
                    return;
                }
                for (uint32_t i = 0; i < length && written < kMaxOutput; i++)
                {
                    uint8_t val = ReadByte(dstPos - disp);
                    WriteByte(dstPos, val);
                    dstPos++;
                    written++;
                }
            }
            else
            {
                uint8_t val = ReadByte(srcPos++);
                WriteByte(dstPos, val);
                dstPos++;
                written++;
            }
        }
    }
}

// SWI 0x14/0x15 RLUnComp
// 源数据格式：
//   [0]     Reserved(4bit) | Type=3(4bit)
//   [1..3]  解压后大小（24bit，小端）
//   [4..]   压缩数据流：每个块以 1 个 flag 字节开头：
//             flag bit7=0：未压缩块，长度=(flag&0x7F)+1，后面跟对应字节数的原始数据
//             flag bit7=1：压缩块  ，长度=(flag&0x7F)+3，后面跟 1 字节，重复该字节 长度 次
void GbaEmulator::RlUnComp(uint32_t src, uint32_t dst)
{
    uint32_t header = ReadWord(src);
    uint8_t  type   = (uint8_t)((header >> 4) & 0xF);
    uint32_t size   = header >> 8;
    if (type != 3 || size == 0)
    {
        LogDebug(L"[RLUnComp] 头部异常 type=%d size=%u（应为 type=3），已跳过", type, size);
        return;
    }

    uint32_t srcPos = src + 4;
    uint32_t dstPos = dst;
    uint32_t written = 0;

    while (written < size)
    {
        uint8_t flag = ReadByte(srcPos++);
        if (flag & 0x80)
        {
            uint32_t length = (uint32_t)(flag & 0x7F) + 3;
            uint8_t  val    = ReadByte(srcPos++);
            for (uint32_t i = 0; i < length && written < size; i++)
            {
                WriteByte(dstPos++, val);
                written++;
            }
        }
        else
        {
            uint32_t length = (uint32_t)(flag & 0x7F) + 1;
            for (uint32_t i = 0; i < length && written < size; i++)
            {
                WriteByte(dstPos++, ReadByte(srcPos++));
                written++;
            }
        }
    }
}

void GbaEmulator::PushTrace(uint32_t pc, uint32_t instr, bool thumb)
{
    m_trace[m_traceIdx].pc = pc;
    m_trace[m_traceIdx].instr = instr;
    m_trace[m_traceIdx].thumb = thumb;
    m_traceIdx = (m_traceIdx + 1) % kTraceDepth;
}
void GbaEmulator::DumpTrace(const wchar_t* reason)
{
    LogDebug(L"[CPU:TRACE] 最近 %d 条指令（触发原因：%s，从旧到新）：", kTraceDepth, reason);
    for (int i = 0; i < kTraceDepth; i++)
    {
        int idx = (m_traceIdx + i) % kTraceDepth;
        const TraceEntry& t = m_trace[idx];
        if (t.pc == 0 && t.instr == 0) continue; // 尚未填满的槽位跳过
        LogDebug(L"  [%2d] PC=0x%08X %s instr=0x%0*X",
                 i, t.pc, t.thumb ? L"THUMB" : L"ARM  ",
                 t.thumb ? 4 : 8, t.instr);
    }
}
bool GbaEmulator::HandleSWI(uint8_t id)
{
    switch (id)
    {
    case 0x02: // Halt
        // 和 IntrWait/VBlankIntrWait 不同，Halt 不管理 0x03007FF8 那个
        // BIOS 中断检查标志数组，也不筛选具体等哪个中断——它只是让 CPU
        // 停下来，直到任意一个「已使能」的中断发生（HandleInterrupt 里
        // 已经有 `if (m_halted && m_irq.ifl) m_halted = false;` 这个唤醒
        // 逻辑，不需要额外处理）。
        // 之前这个 SWI 完全没实现：遇到时会跳到一个「立刻返回」的假
        // BIOS 存根，等于 Halt 从来没有真正停下来过——游戏原本期望
        // 「暂停到下一次 Timer/DMA/VBlank 中断」的这段时间被跳过了，
        // CPU 会比真机快得多地反复空转，导致跟音频 FIFO 填充相关的
        // 定时器/DMA 节奏完全对不上：轻则杂音（Robot Taisen OG2 那种
        // 长时间重复的静态采样值 + 偶发爆音），重则接下来走到不该走
        // 到的分支、踩进全零内存（Summon Night 的 NOP-SEA）。
        m_irq.ime = 1;  // 和 0x04/0x05 保持一致，确保后续中断能正常派发
        m_halted = true;
        LogDebug(L"[SWI] 0x02 Halt: IE=0x%04X IF=0x%04X", (int)m_irq.ie, (int)m_irq.ifl);
        return true;
    case 0x04:
        m_irq.ime = 1;  // BIOS 必须在 HALT 前开启 IME
        if (m_regs.r[0] != 0) {
            m_irq.ifl &= ~(uint16_t)m_regs.r[1]; // 清除目标中断标志
            uint16_t biosFlags = ReadHalf(0x03007FF8);
            WriteHalf(0x03007FF8, biosFlags & ~(uint16_t)m_regs.r[1]);
        }
        m_halted = true;
        LogDebug(L"[SWI] 0x04 IntrWait: IME=1, waitNew=%d mask=0x%04X, HALT",
                 (int)m_regs.r[0], (int)m_regs.r[1]);
        return true;
    case 0x05:
        m_irq.ime = 1;         // ← 核心修复：开启 IME，使 IRQ 向量可被调用
        m_irq.ifl &= ~0x0001u; // 清除 VBlank IF，确保等待下一帧
        { uint16_t bf = ReadHalf(0x03007FF8); WriteHalf(0x03007FF8, bf & ~0x0001u); }
        m_halted = true;
        LogDebug(L"[SWI] 0x05 VBlankIntrWait: IME=1, HALT, cleared VBlank IF, IE=0x%04X IF=0x%04X",
                 (int)m_irq.ie, (int)m_irq.ifl);
        return true;
    case 0x06:
        if (m_regs.r[1] != 0)
        {
            int32_t num = (int32_t)m_regs.r[0];
            int32_t den = (int32_t)m_regs.r[1];
            m_regs.r[0] = (uint32_t)(num / den);
            m_regs.r[1] = (uint32_t)(num % den);
            m_regs.r[3] = (uint32_t)std::abs(num / den);
        }
        else LogDebug(L"[SWI:DIV0] r0=%08X r1=%08X at LR=%08X", m_regs.r[0], m_regs.r[1], m_regs.LR());
		return true;
    case 0x07:
        if (m_regs.r[0] != 0)
        {
            int32_t num = (int32_t)m_regs.r[1];
            int32_t den = (int32_t)m_regs.r[0];
            m_regs.r[0] = (uint32_t)(num / den);
            m_regs.r[1] = (uint32_t)(num % den);
            m_regs.r[3] = (uint32_t)std::abs(num / den);
        }
        else LogDebug(L"[SWI:DIV0] r0=%08X r1=%08X at LR=%08X", m_regs.r[0], m_regs.r[1], m_regs.LR());
		return true;
    case 0x08:
        m_regs.r[0] = (uint32_t)(uint16_t)(uint32_t)sqrtf((float)m_regs.r[0]);
        return true;
    case 0x0B:
    {
        uint32_t src  = m_regs.r[0];
        uint32_t dst  = m_regs.r[1];
        uint32_t ctrl = m_regs.r[2];
        bool     fill = (ctrl >> 24) & 1;
        bool     word = (ctrl >> 26) & 1;
        uint32_t cnt  = ctrl & 0x1FFFFF;
        if (word)
        {
            uint32_t val = fill ? ReadWord(src) : 0;
            for (uint32_t i = 0; i < cnt; i++)
            {
                if (!fill) val = ReadWord(src + i * 4);
                WriteWord(dst + i * 4, val);
            }
        }
        else
        {
            uint16_t val = fill ? ReadHalf(src) : 0;
            for (uint32_t i = 0; i < cnt; i++)
            {
                if (!fill) val = ReadHalf(src + i * 2);
                WriteHalf(dst + i * 2, val);
            }
        }
        return true;
    }
    case 0x0C:
    {
        uint32_t src  = m_regs.r[0];
        uint32_t dst  = m_regs.r[1];
        uint32_t ctrl = m_regs.r[2];
        bool     fill = (ctrl >> 24) & 1;
        uint32_t cnt  = ctrl & 0x1FFFFF;
        uint32_t val  = fill ? ReadWord(src) : 0;
        for (uint32_t i = 0; i < cnt; i++)
        {
            if (!fill) val = ReadWord(src + i * 4);
            WriteWord(dst + i * 4, val);
        }
        return true;
    }
    case 0x11: // LZ77UnCompWRAM
    case 0x12: // LZ77UnCompVRAM
    {
        uint32_t src = m_regs.r[0];
        uint32_t dst = m_regs.r[1];
        uint32_t header = ReadWord(src);
        uint32_t size = header >> 8;
        LogDebug(L"[SWI] 0x%02X LZ77UnComp: src=0x%08X dst=0x%08X size=%u", (unsigned int)id, src, dst, size);
        Lz77UnComp(src, dst);
        return true;
    }
    case 0x14: // RLUnCompWRAM
    case 0x15: // RLUnCompVRAM
    {
        uint32_t src = m_regs.r[0];
        uint32_t dst = m_regs.r[1];
        uint32_t header = ReadWord(src);
        uint32_t size = header >> 8;
        LogDebug(L"[SWI] 0x%02X RLUnComp: src=0x%08X dst=0x%08X size=%u", (unsigned int)id, src, dst, size);
        RlUnComp(src, dst);
        return true;
    }
    case 0x00:
    case 0x01:
        return true;
    default:
    {
        // 记录每个未实现的 SWI 号（每个号只记录一次，避免刷屏），
        // 便于定位到底是哪个 BIOS 调用导致游戏卡死/无声。
        if (!m_loggedSwi[id])
        {
            m_loggedSwi[id] = true;
            LogDebug(L"[SWI:UNIMPLEMENTED] id=0x%02X r0=%08X r1=%08X r2=%08X r3=%08X LR=%08X "
                     L"-- 该 BIOS 调用未实现，代码将被跳转到空的 BIOS 存根，"
                     L"如果这就是卡死/静音的原因，请优先补齐此调用",
                     (unsigned int)id, m_regs.r[0], m_regs.r[1], m_regs.r[2], m_regs.r[3], m_regs.LR());
        }
        return false;
    }
    }
}