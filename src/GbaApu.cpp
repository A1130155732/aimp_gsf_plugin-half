// GBA APU 模拟器实现
// 实现 GBA 音频硬件的核心模拟逻辑

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GbaApu.h"
#include "DebugLog.h"
#include <algorithm>
#include <cstring>

GbaApu::GbaApu()
    : m_masterVolLeft(0), m_masterVolRight(0)
    , m_ch1_left(0), m_ch1_right(0)
    , m_ch2_left(0), m_ch2_right(0)
    , m_ch3_left(0), m_ch3_right(0)
    , m_ch4_left(0), m_ch4_right(0)
    , m_apuEnabled(false)
    , m_outputSampleRate(44100)
    , m_cyclesPerSample(0)
    , m_cycleAccum(0)
    , m_frameSeqTimer(0)
    , m_frameSeqStep(0)
    , m_resampleRatio(1.0)
    , m_resampleAccum(0.0)
    , m_lastLeft(0), m_lastRight(0)
{
    m_ch1.Reset();
    m_ch2.Reset();
    m_ch3.Reset();
    m_ch4.Reset();
    m_fifoA.FullReset();  // 冷启动：完全初始化，含控制寄存器
    m_fifoB.FullReset();
}

GbaApu::~GbaApu() {}

void GbaApu::Init(int outputSampleRate)
{
    m_outputSampleRate = outputSampleRate;
    // GBA 原生采样率 32768 Hz，重采样到目标采样率
    m_resampleRatio = (double)GBA_SAMPLE_RATE / outputSampleRate;
    m_resampleAccum = 0.0;
    // 每个 GBA 原生样本对应的 CPU 周期数
    m_cyclesPerSample = GBA_CPU_FREQ / GBA_SAMPLE_RATE;
    m_cycleAccum = 0;
    Reset();
}

void GbaApu::Reset()
{
    m_ch1.Reset();
    m_ch2.Reset();
    m_ch3.Reset();
    m_ch4.Reset();
    m_ch4.lfsr = 0x7FFF; // 修正：噪声声道 LFSR 初始不能为 0
    m_fifoA.FullReset();  // 冷启动/软复位：完全初始化含控制寄存器
    m_fifoB.FullReset();
    m_masterVolLeft = 7;
    m_masterVolRight = 7;
    m_apuEnabled = true;
    m_frameSeqTimer = 0;
    m_frameSeqStep = 0;
    m_cycleAccum = 0;
    m_resampleAccum = 0.0;
    m_lastLeft = 0;
    m_lastRight = 0;
    WriteIO(GBA_IO_BASE + 0x88, 0x00); // REG_SOUNDBIAS low
    WriteIO(GBA_IO_BASE + 0x89, 0x02); // REG_SOUNDBIAS high (设置中值偏置)
}

void GbaApu::WriteIO(uint32_t addr, uint8_t value)
{
    uint32_t reg = addr & 0x3FF;
    switch (reg)
    {
    // 声道1 扫频
    case REG_SOUND1CNT_L:
        m_ch1.sweep_period = (value >> 4) & 7;
        m_ch1.sweep_negate = (value >> 3) & 1;
        m_ch1.sweep_shift  = value & 7;
        break;
    // 声道1 包络/占空比
    case REG_SOUND1CNT_H:
        m_ch1.length = 64 - (value & 63);
        m_ch1.duty   = (value >> 6) & 3;
        break;
    case REG_SOUND1CNT_H + 1:
        m_ch1.env_period = value & 7;
        m_ch1.env_add    = (value >> 3) & 1;
        m_ch1.env_volume = (value >> 4) & 15;
        m_ch1.dac_enabled = (value & 0xF8) != 0;
        if (!m_ch1.dac_enabled) m_ch1.enabled = false;
        break;
    // 声道1 频率低字节
    case REG_SOUND1CNT_X:
        m_ch1.frequency = (m_ch1.frequency & 0x700) | value;
        break;
    // 声道1 频率高字节 + 触发
    case REG_SOUND1CNT_X + 1:
        m_ch1.frequency = (m_ch1.frequency & 0xFF) | ((value & 7) << 8);
        m_ch1.length_enabled = (value >> 6) & 1;
        if (value & 0x80) // 触发
        {
            m_ch1.enabled = m_ch1.dac_enabled;
            m_ch1.volume  = m_ch1.env_volume;
            m_ch1.env_timer = m_ch1.env_period;
            m_ch1.freq_timer = (2048 - m_ch1.frequency) * 4;
            if (m_ch1.length == 0) m_ch1.length = 64;
            m_ch1.sweep_freq = m_ch1.frequency;
            m_ch1.sweep_timer = m_ch1.sweep_period ? m_ch1.sweep_period : 8;
            m_ch1.sweep_enabled = m_ch1.sweep_period || m_ch1.sweep_shift;
        }
        break;

    // 声道2 包络/占空比
    case REG_SOUND2CNT_L:
        m_ch2.length = 64 - (value & 63);
        m_ch2.duty   = (value >> 6) & 3;
        break;
    case REG_SOUND2CNT_L + 1:
        m_ch2.env_period = value & 7;
        m_ch2.env_add    = (value >> 3) & 1;
        m_ch2.env_volume = (value >> 4) & 15;
        m_ch2.dac_enabled = (value & 0xF8) != 0;
        if (!m_ch2.dac_enabled) m_ch2.enabled = false;
        break;
    case REG_SOUND2CNT_H:
        m_ch2.frequency = (m_ch2.frequency & 0x700) | value;
        break;
    case REG_SOUND2CNT_H + 1:
        m_ch2.frequency = (m_ch2.frequency & 0xFF) | ((value & 7) << 8);
        m_ch2.length_enabled = (value >> 6) & 1;
        if (value & 0x80)
        {
            m_ch2.enabled = m_ch2.dac_enabled;
            m_ch2.volume  = m_ch2.env_volume;
            m_ch2.env_timer = m_ch2.env_period;
            m_ch2.freq_timer = (2048 - m_ch2.frequency) * 4;
            if (m_ch2.length == 0) m_ch2.length = 64;
        }
        break;

    // 声道3 开关
    case REG_SOUND3CNT_L:
        m_ch3.dac_enabled = (value >> 7) & 1;
        if (!m_ch3.dac_enabled) m_ch3.enabled = false;
        break;
    case REG_SOUND3CNT_H:
        m_ch3.length = 256 - value;
        break;
    case REG_SOUND3CNT_H + 1:
        m_ch3.volume_shift = (value >> 5) & 3;
        break;
    case REG_SOUND3CNT_X:
        m_ch3.frequency = (m_ch3.frequency & 0x700) | value;
        break;
    case REG_SOUND3CNT_X + 1:
        m_ch3.frequency = (m_ch3.frequency & 0xFF) | ((value & 7) << 8);
        m_ch3.length_enabled = (value >> 6) & 1;
        if (value & 0x80)
        {
            m_ch3.enabled = m_ch3.dac_enabled;
            m_ch3.freq_timer = (2048 - m_ch3.frequency) * 2;
            m_ch3.wave_pos = 0;
            if (m_ch3.length == 0) m_ch3.length = 256;
        }
        break;

    // 声道4 包络
    case REG_SOUND4CNT_L:
        m_ch4.length = 64 - (value & 63);
        break;
    case REG_SOUND4CNT_L + 1:
        m_ch4.env_period = value & 7;
        m_ch4.env_add    = (value >> 3) & 1;
        m_ch4.env_volume = (value >> 4) & 15;
        m_ch4.dac_enabled = (value & 0xF8) != 0;
        if (!m_ch4.dac_enabled) m_ch4.enabled = false;
        break;
    case REG_SOUND4CNT_H:
        m_ch4.divisor_code = value & 7;
        m_ch4.width_mode   = (value >> 3) & 1;
        m_ch4.clock_shift  = (value >> 4) & 15;
        break;
    case REG_SOUND4CNT_H + 1:
        m_ch4.length_enabled = (value >> 6) & 1;
        if (value & 0x80)
        {
            m_ch4.enabled = m_ch4.dac_enabled;
            m_ch4.volume  = m_ch4.env_volume;
            m_ch4.env_timer = m_ch4.env_period;
            m_ch4.lfsr = 0x7FFF;
            if (m_ch4.length == 0) m_ch4.length = 64;
            static const uint8_t divisors[8] = {8,16,32,48,64,80,96,112};
            m_ch4.freq_timer = divisors[m_ch4.divisor_code] << m_ch4.clock_shift;
        }
        break;

    // 主音量控制
    case REG_SOUNDCNT_L:
        m_masterVolRight = value & 7;
        m_masterVolLeft  = (value >> 4) & 7;
        break;
    case REG_SOUNDCNT_L + 1:
        m_ch1_right = (value >> 0) & 1;
        m_ch2_right = (value >> 1) & 1;
        m_ch3_right = (value >> 2) & 1;
        m_ch4_right = (value >> 3) & 1;
        m_ch1_left  = (value >> 4) & 1;
        m_ch2_left  = (value >> 5) & 1;
        m_ch3_left  = (value >> 6) & 1;
        m_ch4_left  = (value >> 7) & 1;
        break;

    // DMA 声音控制
    // ---------------------------------------------------------------
    // GBA SOUNDCNT_H (0x04000082) 16-bit 寄存器完整布局（GBATEK 规格）：
    //   [1:0] DMG 总音量 (0=25%, 1=50%, 2=100%)
    //   [2]   FIFO A 音量 (0=50%, 1=100%)
    //   [3]   FIFO B 音量 (0=50%, 1=100%)
    //   [4:7] 未使用（低字节高半部）
    //   [8]   FIFO A 输出到右声道
    //   [9]   FIFO A 输出到左声道
    //   [10]  FIFO A 定时器选择 (0=Timer0, 1=Timer1)
    //   [11]  FIFO A FIFO 复位
    //   [12]  FIFO B 输出到右声道
    //   [13]  FIFO B 输出到左声道
    //   [14]  FIFO B 定时器选择 (0=Timer0, 1=Timer1)
    //   [15]  FIFO B FIFO 复位
    //
    // 低字节 (0x082)：DMG 音量 + FIFO A/B 音量（bit0-3），bit4-7 未使用
    // 高字节 (0x083)：FIFO A 路由/定时器/复位(bit0-3) + FIFO B 路由/定时器/复位(bit4-7)
    // ---------------------------------------------------------------
    case REG_SOUNDCNT_H:   // 0x082 低字节
        m_fifoA.volume_shift = (value >> 2) & 1; // bit2: FIFO A 音量 (0=50%, 1=100%)
        m_fifoB.volume_shift = (value >> 3) & 1; // bit3: FIFO B 音量 (0=50%, 1=100%)
        // bit4-7 在低字节中未使用，FIFO A 路由/定时/复位在高字节（bit8-11）
        LogDebug(L"[APU] SOUNDCNT_H low=0x%02X: FifoA vol=%d FifoB vol=%d",
                 value, m_fifoA.volume_shift, m_fifoB.volume_shift);
        break;
    case REG_SOUNDCNT_H + 1:  // 0x083 高字节，对应 word 的 bit8-15
        m_fifoA.right_enable = (value >> 0) & 1; // bit8(word):  FIFO A → 右
        m_fifoA.left_enable  = (value >> 1) & 1; // bit9(word):  FIFO A → 左
        m_fifoA.timer_select = (value >> 2) & 1; // bit10(word): FIFO A 定时器
        if (value & 0x08)                          // bit11(word): FIFO A 复位
        {
            m_fifoA.Reset();
            LogDebug(L"[APU] FIFO A Reset via SOUNDCNT_H bit11");
        }
        m_fifoB.right_enable = (value >> 4) & 1; // bit12(word): FIFO B → 右
        m_fifoB.left_enable  = (value >> 5) & 1; // bit13(word): FIFO B → 左
        m_fifoB.timer_select = (value >> 6) & 1; // bit14(word): FIFO B 定时器
        if (value & 0x80)                          // bit15(word): FIFO B 复位
        {
            m_fifoB.Reset();
            LogDebug(L"[APU] FIFO B Reset via SOUNDCNT_H bit15");
        }
        LogDebug(L"[APU] SOUNDCNT_H high=0x%02X: FifoA(r=%d l=%d timer=%d) FifoB(vol=%d r=%d l=%d timer=%d)",
                 value,
                 (int)m_fifoA.right_enable, (int)m_fifoA.left_enable, (int)m_fifoA.timer_select,
                 m_fifoB.volume_shift, (int)m_fifoB.right_enable, (int)m_fifoB.left_enable, (int)m_fifoB.timer_select);
        break;

    // APU 主开关
    case REG_SOUNDCNT_X:
        m_apuEnabled = (value >> 7) & 1;
        if (!m_apuEnabled)
        {
            m_ch1.Reset(); m_ch2.Reset();
            m_ch3.Reset(); m_ch4.Reset();
        }
        break;

    // 波形 RAM
    default:
        if (reg >= REG_WAVE_RAM && reg < REG_WAVE_RAM + 16)
            m_ch3.wave_ram[reg - REG_WAVE_RAM] = value;
        break;
    }
}

void GbaApu::WriteIO16(uint32_t addr, uint16_t value)
{
    WriteIO(addr,     (uint8_t)(value & 0xFF));
    WriteIO(addr + 1, (uint8_t)(value >> 8));
}

void GbaApu::WriteIO32(uint32_t addr, uint32_t value)
{
    WriteIO(addr,     (uint8_t)(value & 0xFF));
    WriteIO(addr + 1, (uint8_t)((value >> 8) & 0xFF));
    WriteIO(addr + 2, (uint8_t)((value >> 16) & 0xFF));
    WriteIO(addr + 3, (uint8_t)((value >> 24) & 0xFF));
}

void GbaApu::WriteFifoA(uint32_t data)
{
    m_fifoA.Push((int8_t)(data & 0xFF));
    m_fifoA.Push((int8_t)((data >> 8) & 0xFF));
    m_fifoA.Push((int8_t)((data >> 16) & 0xFF));
    m_fifoA.Push((int8_t)((data >> 24) & 0xFF));
}

void GbaApu::WriteFifoB(uint32_t data)
{
    m_fifoB.Push((int8_t)(data & 0xFF));
    m_fifoB.Push((int8_t)((data >> 8) & 0xFF));
    m_fifoB.Push((int8_t)((data >> 16) & 0xFF));
    m_fifoB.Push((int8_t)((data >> 24) & 0xFF));
}

void GbaApu::OnTimerOverflow(int timerIndex)
{
    if (timerIndex != 0 && timerIndex != 1)
        return; // FIFO 只能绑定 Timer0/Timer1，Timer2/3 溢出与音频无关

    bool sel = (timerIndex != 0);
    if (m_fifoA.timer_select == sel)
    {
        m_fifoA.Pop();
    }
    if (m_fifoB.timer_select == sel)
    {
        m_fifoB.Pop();
    }
}

void GbaApu::TickFrameSequencer()
{
    // 帧序列器以 512 Hz 运行
    // Step 0,2,4,6: 长度计数器
    // Step 2,6:     扫频
    // Step 7:       包络
    switch (m_frameSeqStep & 7)
    {
    case 0: case 4:
        TickLength(m_ch1); TickLength(m_ch2);
        TickLengthWave(); TickLengthNoise();
        break;
    case 2: case 6:
        TickLength(m_ch1); TickLength(m_ch2);
        TickLengthWave(); TickLengthNoise();
        TickSweep();
        break;
    case 7:
        TickEnvelope(m_ch1); TickEnvelope(m_ch2);
        TickEnvelopeNoise();
        break;
    }
    m_frameSeqStep++;
}

void GbaApu::TickLength(SquareChannel& ch)
{
    if (ch.length_enabled && ch.length > 0)
    {
        ch.length--;
        if (ch.length == 0) ch.enabled = false;
    }
}

void GbaApu::TickLengthWave()
{
    if (m_ch3.length_enabled && m_ch3.length > 0)
    {
        m_ch3.length--;
        if (m_ch3.length == 0) m_ch3.enabled = false;
    }
}

void GbaApu::TickLengthNoise()
{
    if (m_ch4.length_enabled && m_ch4.length > 0)
    {
        m_ch4.length--;
        if (m_ch4.length == 0) m_ch4.enabled = false;
    }
}

void GbaApu::TickEnvelope(SquareChannel& ch)
{
    if (ch.env_period == 0) return;
    if (ch.env_timer > 0) ch.env_timer--;
    if (ch.env_timer == 0)
    {
        ch.env_timer = ch.env_period;
        if (ch.env_add && ch.volume < 15) ch.volume++;
        else if (!ch.env_add && ch.volume > 0) ch.volume--;
    }
}

void GbaApu::TickEnvelopeNoise()
{
    if (m_ch4.env_period == 0) return;
    if (m_ch4.env_timer > 0) m_ch4.env_timer--;
    if (m_ch4.env_timer == 0)
    {
        m_ch4.env_timer = m_ch4.env_period;
        if (m_ch4.env_add && m_ch4.volume < 15) m_ch4.volume++;
        else if (!m_ch4.env_add && m_ch4.volume > 0) m_ch4.volume--;
    }
}

void GbaApu::TickSweep()
{
    if (!m_ch1.sweep_enabled) return;
    if (m_ch1.sweep_timer > 0) m_ch1.sweep_timer--;
    if (m_ch1.sweep_timer == 0)
    {
        m_ch1.sweep_timer = m_ch1.sweep_period ? m_ch1.sweep_period : 8;
        if (m_ch1.sweep_period > 0)
        {
            uint16_t newFreq = m_ch1.sweep_freq >> m_ch1.sweep_shift;
            if (m_ch1.sweep_negate)
                newFreq = m_ch1.sweep_freq - newFreq;
            else
                newFreq = m_ch1.sweep_freq + newFreq;

            if (newFreq > 2047)
                m_ch1.enabled = false;
            else if (m_ch1.sweep_shift > 0)
            {
                m_ch1.sweep_freq = newFreq;
                m_ch1.frequency  = newFreq;
            }
        }
    }
}

void GbaApu::MixSample(int16_t& left, int16_t& right)
{
    if (!m_apuEnabled) { left = right = 0; return; }

    int mixLeft = 0, mixRight = 0;

    // DMG 声道混音
    int8_t s1 = m_ch1.GetSample();
    int8_t s2 = m_ch2.GetSample();
    int8_t s3 = m_ch3.GetSample();
    int8_t s4 = m_ch4.GetSample();

    if (m_ch1_left)  mixLeft  += s1;
    if (m_ch1_right) mixRight += s1;
    if (m_ch2_left)  mixLeft  += s2;
    if (m_ch2_right) mixRight += s2;
    if (m_ch3_left)  mixLeft  += s3;
    if (m_ch3_right) mixRight += s3;
    if (m_ch4_left)  mixLeft  += s4;
    if (m_ch4_right) mixRight += s4;

    // 应用主音量 (0-7 -> 乘以 (vol+1)/8)
    // 修正：DMG 声道原始范围较小，乘以 16 左右即可
    mixLeft  = mixLeft  * (m_masterVolLeft  + 1) * 16;
    mixRight = mixRight * (m_masterVolRight + 1) * 16;

    // DMA 声音混音（音量更大，直接叠加）
    int32_t dmaA = (int32_t)m_fifoA.current_sample;
    int32_t dmaB = (int32_t)m_fifoB.current_sample;

    int dmaShiftA = m_fifoA.volume_shift ? 0 : 1; // 0=100%, 1=50%
    int dmaShiftB = m_fifoB.volume_shift ? 0 : 1;

    // DMA 样本是 8-bit 有符号，范围 -128 到 127
    // 扩展到 16-bit 范围需要乘以 256
    if (m_fifoA.left_enable)  mixLeft  += (dmaA * 256) >> dmaShiftA;
    if (m_fifoA.right_enable) mixRight += (dmaA * 256) >> dmaShiftA;
    if (m_fifoB.left_enable)  mixLeft  += (dmaB * 256) >> dmaShiftB;
    if (m_fifoB.right_enable) mixRight += (dmaB * 256) >> dmaShiftB;

    // 限幅到 16-bit 范围
    if (mixLeft < -32768) mixLeft = -32768;
    if (mixLeft > 32767) mixLeft = 32767;
    left = (int16_t)mixLeft;

    if (mixRight < -32768) mixRight = -32768;
    if (mixRight > 32767) mixRight = 32767;
    right = (int16_t)mixRight;

    // DEBUG：APU 混音节点（每 32768 样本记录一次，约 1 秒）
    static int s_mixDbgCount = 0;
    if (++s_mixDbgCount >= 32768)
    {
        s_mixDbgCount = 0;
        LogDebug(L"[APU:Mix] enabled=%d masterL=%d masterR=%d "
                 L"ch1(en=%d dac=%d vol=%d) ch2(en=%d dac=%d vol=%d) "
                 L"ch3(en=%d dac=%d vsh=%d) ch4(en=%d dac=%d vol=%d) "
                 L"fifoA(cur=%d cnt=%d lr=%d%d timer=%d vol=%d) "
                 L"fifoB(cur=%d cnt=%d lr=%d%d timer=%d vol=%d) "
                 L"out L=%d R=%d",
                 (int)m_apuEnabled, m_masterVolLeft, m_masterVolRight,
                 (int)m_ch1.enabled, (int)m_ch1.dac_enabled, m_ch1.volume,
                 (int)m_ch2.enabled, (int)m_ch2.dac_enabled, m_ch2.volume,
                 (int)m_ch3.enabled, (int)m_ch3.dac_enabled, m_ch3.volume_shift,
                 (int)m_ch4.enabled, (int)m_ch4.dac_enabled, m_ch4.volume,
                 (int)dmaA, m_fifoA.count, (int)m_fifoA.left_enable, (int)m_fifoA.right_enable,
                 (int)m_fifoA.timer_select, m_fifoA.volume_shift,
                 (int)dmaB, m_fifoB.count, (int)m_fifoB.left_enable, (int)m_fifoB.right_enable,
                 (int)m_fifoB.timer_select, m_fifoB.volume_shift,
                 (int)left, (int)right);
    }
}

int GbaApu::Tick(int cycles, int16_t* outputBuffer, int maxSamples)
{
    int samplesGenerated = 0;

    while (cycles > 0 && samplesGenerated < maxSamples)
    {
        int step = (cycles < (m_cyclesPerSample - m_cycleAccum)) ? cycles : (m_cyclesPerSample - m_cycleAccum);

        // 推进方波声道
        for (int i = 0; i < step; i++)
        {
            // 方波声道1
            if (m_ch1.freq_timer > 0)
            {
                m_ch1.freq_timer--;
                if (m_ch1.freq_timer == 0)
                {
                    m_ch1.freq_timer = (2048 - m_ch1.frequency) * 4;
                    m_ch1.duty_pos = (m_ch1.duty_pos + 1) & 7;
                }
            }
            // 方波声道2
            if (m_ch2.freq_timer > 0)
            {
                m_ch2.freq_timer--;
                if (m_ch2.freq_timer == 0)
                {
                    m_ch2.freq_timer = (2048 - m_ch2.frequency) * 4;
                    m_ch2.duty_pos = (m_ch2.duty_pos + 1) & 7;
                }
            }
            // 波形声道
            if (m_ch3.freq_timer > 0)
            {
                m_ch3.freq_timer--;
                if (m_ch3.freq_timer == 0)
                {
                    m_ch3.freq_timer = (2048 - m_ch3.frequency) * 2;
                    m_ch3.wave_pos = (m_ch3.wave_pos + 1) & 31;
                }
            }
            // 噪声声道
            if (m_ch4.freq_timer > 0)
            {
                m_ch4.freq_timer--;
                if (m_ch4.freq_timer == 0)
                {
                    static const uint8_t divisors[8] = {8,16,32,48,64,80,96,112};
                    m_ch4.freq_timer = divisors[m_ch4.divisor_code] << m_ch4.clock_shift;
                    uint16_t xorBit = (m_ch4.lfsr ^ (m_ch4.lfsr >> 1)) & 1;
                    m_ch4.lfsr = (m_ch4.lfsr >> 1) | (xorBit << 14);
                    if (m_ch4.width_mode)
                    {
                        m_ch4.lfsr &= ~0x40;
                        m_ch4.lfsr |= xorBit << 6;
                    }
                }
            }
        }

        m_cycleAccum += step;
        cycles -= step;

        // 帧序列器（每 GBA_CPU_FREQ/512 = 32768 周期触发一次）
        m_frameSeqTimer += step;
        if (m_frameSeqTimer >= GBA_CPU_FREQ / 512)
        {
            m_frameSeqTimer -= GBA_CPU_FREQ / 512;
            TickFrameSequencer();
        }

        // 生成一个 GBA 原生样本
        if (m_cycleAccum >= m_cyclesPerSample)
        {
            m_cycleAccum -= m_cyclesPerSample;

            int16_t nativeLeft, nativeRight;
            MixSample(nativeLeft, nativeRight);

            // 简单线性重采样
            m_resampleAccum += 1.0;
            while (m_resampleAccum >= m_resampleRatio && samplesGenerated < maxSamples)
            {
                outputBuffer[samplesGenerated * 2]     = nativeLeft;
                outputBuffer[samplesGenerated * 2 + 1] = nativeRight;
                samplesGenerated++;
                m_resampleAccum -= m_resampleRatio;
            }
            m_lastLeft  = nativeLeft;
            m_lastRight = nativeRight;
        }
    }

    return samplesGenerated;
}