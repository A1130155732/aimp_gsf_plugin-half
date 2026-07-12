// GBA APU (Audio Processing Unit) 模拟器
// 模拟 Game Boy Advance 的音频硬件：
//   - 4 个 DMG 声道（方波 x2、波形、噪声）
//   - 2 个 DMA 直接声音声道（PCM 采样）
//   - 混音输出为 16-bit 立体声 PCM

#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

// GBA 内存映射常量
#define GBA_ROM_BASE        0x08000000
#define GBA_EWRAM_BASE      0x02000000
#define GBA_IWRAM_BASE      0x03000000
#define GBA_IO_BASE         0x04000000
#define GBA_PALETTE_BASE    0x05000000
#define GBA_VRAM_BASE       0x06000000
#define GBA_OAM_BASE        0x07000000

// GBA 内存大小
#define GBA_ROM_SIZE        (32 * 1024 * 1024)  // 32 MB
#define GBA_EWRAM_SIZE      (256 * 1024)         // 256 KB
#define GBA_IWRAM_SIZE      (32 * 1024)          // 32 KB
#define GBA_IO_SIZE         (1024)               // 1 KB

// GBA 时钟频率
#define GBA_CPU_FREQ        16777216    // 16.78 MHz
#define GBA_SAMPLE_RATE     32768       // GBA 原生采样率

// APU 寄存器地址（相对于 IO 基址 0x04000000）
#define REG_SOUND1CNT_L     0x060   // 声道1 扫频控制
#define REG_SOUND1CNT_H     0x062   // 声道1 包络/长度
#define REG_SOUND1CNT_X     0x064   // 声道1 频率/控制
#define REG_SOUND2CNT_L     0x068   // 声道2 包络/长度
#define REG_SOUND2CNT_H     0x06C   // 声道2 频率/控制
#define REG_SOUND3CNT_L     0x070   // 声道3 开关
#define REG_SOUND3CNT_H     0x072   // 声道3 长度/音量
#define REG_SOUND3CNT_X     0x074   // 声道3 频率/控制
#define REG_SOUND4CNT_L     0x078   // 声道4 长度/包络
#define REG_SOUND4CNT_H     0x07C   // 声道4 频率/控制
#define REG_SOUNDCNT_L      0x080   // 主音量/声道开关
#define REG_SOUNDCNT_H      0x082   // DMA 声音控制
#define REG_SOUNDCNT_X      0x084   // 主开关
#define REG_SOUNDBIAS       0x088   // 偏置/采样率
#define REG_WAVE_RAM        0x090   // 波形 RAM（16 字节）
#define REG_FIFO_A          0x0A0   // DMA 声音 FIFO A
#define REG_FIFO_B          0x0A4   // DMA 声音 FIFO B

// 方波声道
struct SquareChannel
{
    bool    enabled;
    bool    dac_enabled;
    uint8_t duty;           // 占空比 (0-3: 12.5%, 25%, 50%, 75%)
    uint8_t length;         // 长度计数器
    bool    length_enabled;
    uint8_t volume;         // 当前音量 (0-15)
    uint8_t env_volume;     // 包络初始音量
    uint8_t env_period;     // 包络周期
    bool    env_add;        // 包络方向（true=增加）
    uint8_t env_timer;
    uint16_t frequency;     // 频率寄存器值 (0-2047)
    uint16_t freq_timer;    // 频率计时器
    uint8_t  duty_pos;      // 占空比位置 (0-7)

    // 扫频（仅声道1）
    bool    sweep_enabled;
    uint8_t sweep_period;
    bool    sweep_negate;
    uint8_t sweep_shift;
    uint8_t sweep_timer;
    uint16_t sweep_freq;

    void Reset()
    {
        enabled = false; dac_enabled = false;
        duty = 0; length = 0; length_enabled = false;
        volume = 0; env_volume = 0; env_period = 0;
        env_add = false; env_timer = 0;
        frequency = 0; freq_timer = 0; duty_pos = 0;
        sweep_enabled = false; sweep_period = 0;
        sweep_negate = false; sweep_shift = 0;
        sweep_timer = 0; sweep_freq = 0;
    }

    int8_t GetSample() const
    {
        if (!enabled || !dac_enabled) return 0;
        static const uint8_t duty_table[4][8] = {
            {0,0,0,0,0,0,0,1},  // 12.5%
            {1,0,0,0,0,0,0,1},  // 25%
            {1,0,0,0,1,1,1,1},  // 50%
            {0,1,1,1,1,1,1,0},  // 75%
        };
        return duty_table[duty & 3][duty_pos & 7] ? (int8_t)volume : 0;
    }
};

// 波形声道（声道3）
struct WaveChannel
{
    bool    enabled;
    bool    dac_enabled;
    uint8_t length;
    bool    length_enabled;
    uint8_t volume_shift;   // 0=静音, 1=100%, 2=50%, 3=25%
    uint16_t frequency;
    uint16_t freq_timer;
    uint8_t  wave_pos;      // 波形位置 (0-31)
    uint8_t  wave_ram[16];  // 波形 RAM

    void Reset()
    {
        enabled = false; dac_enabled = false;
        length = 0; length_enabled = false;
        volume_shift = 0; frequency = 0;
        freq_timer = 0; wave_pos = 0;
        memset(wave_ram, 0, sizeof(wave_ram));
    }

    int8_t GetSample() const
    {
        if (!enabled || !dac_enabled) return 0;
        uint8_t sample = wave_ram[wave_pos >> 1];
        if (wave_pos & 1) sample &= 0x0F;
        else sample >>= 4;
        if (volume_shift == 0) return 0;
        return (int8_t)(sample >> (volume_shift - 1));
    }
};

// 噪声声道（声道4）
struct NoiseChannel
{
    bool    enabled;
    bool    dac_enabled;
    uint8_t length;
    bool    length_enabled;
    uint8_t volume;
    uint8_t env_volume;
    uint8_t env_period;
    bool    env_add;
    uint8_t env_timer;
    uint8_t clock_shift;
    bool    width_mode;     // false=15位, true=7位 LFSR
    uint8_t divisor_code;
    uint16_t lfsr;          // 线性反馈移位寄存器
    uint32_t freq_timer;

    void Reset()
    {
        enabled = false; dac_enabled = false;
        length = 0; length_enabled = false;
        volume = 0; env_volume = 0; env_period = 0;
        env_add = false; env_timer = 0;
        clock_shift = 0; width_mode = false;
        divisor_code = 0; lfsr = 0x7FFF; freq_timer = 0;
    }

    int8_t GetSample() const
    {
        if (!enabled || !dac_enabled) return 0;
        return (lfsr & 1) ? 0 : (int8_t)volume;
    }
};

// DMA 直接声音 FIFO
// v11: 添加 internalRemaining 计数器，匹配 mGBA GBAAudioSampleFIFO() 行为
struct DmaFifo
{
    int8_t  buffer[32];
    int     read_pos;
    int     write_pos;
    int     count;
    int8_t  current_sample;
    int     internalRemaining;  // v11: 当前已取出但未输出到 DAC 的剩余字节数(0-3)
    bool    timer_select;   // 使用 Timer 0 或 Timer 1
    bool    left_enable;
    bool    right_enable;
    int     volume_shift;   // 0=50%, 1=100%

    // 仅清空 FIFO 数据缓冲区（对应硬件 FIFO Reset bit）
    // 不修改控制寄存器（right_enable/left_enable/timer_select/volume_shift）
    void Reset()
    {
        memset(buffer, 0, sizeof(buffer));
        read_pos = 0; write_pos = 0; count = 0;
        current_sample = 0;
        internalRemaining = 0;
    }

    // 完全初始化（仅用于模拟器冷启动，含控制寄存器）
    void FullReset()
    {
        memset(buffer, 0, sizeof(buffer));
        read_pos = 0; write_pos = 0; count = 0;
        current_sample = 0;
        internalRemaining = 0;
        timer_select = false;
        left_enable = false; right_enable = false;
        volume_shift = 0;
    }

    void Push(int8_t sample)
    {
        if (count < 32)
        {
            buffer[write_pos] = sample;
            write_pos = (write_pos + 1) & 31;
            count++;
        }
    }

    int8_t Pop()
    {
        if (count > 0)
        {
            current_sample = buffer[read_pos];
            read_pos = (read_pos + 1) & 31;
            count--;
        }
        return current_sample;
    }

    bool NeedsRefill() const { return count <= 16; }
};

// GBA APU 主类
class GbaApu
{
public:
    GbaApu();
    ~GbaApu();

    // 初始化 APU，设置输出采样率
    void Init(int outputSampleRate);

    // 重置 APU 状态
    void Reset();

    // 写入 IO 寄存器
    void WriteIO(uint32_t addr, uint8_t value);
    void WriteIO16(uint32_t addr, uint16_t value);
    void WriteIO32(uint32_t addr, uint32_t value);

    // 读取 IO 寄存器
    uint8_t ReadIO(uint32_t addr);

    // 推进 APU 时钟（cycles = CPU 时钟周期数）
    // 返回生成的立体声样本数
    int Tick(int cycles, int16_t* outputBuffer, int maxSamples);

    // 通知 DMA 声音 FIFO 触发（由 Timer 溢出触发）
    void OnTimerOverflow(int timerIndex);

    // 写入 DMA FIFO 数据（32位，一次推入4字节）
    void WriteFifoA(uint32_t data);
    void WriteFifoB(uint32_t data);

    // 直接访问 FIFO（供 GbaEmulator::WriteIO 逐字节写入使用）
    DmaFifo& GetFifoA() { return m_fifoA; }
    DmaFifo& GetFifoB() { return m_fifoB; }

private:
    SquareChannel   m_ch1;      // 方波声道1（带扫频）
    SquareChannel   m_ch2;      // 方波声道2
    WaveChannel     m_ch3;      // 波形声道
    NoiseChannel    m_ch4;      // 噪声声道
    DmaFifo         m_fifoA;    // DMA 声音 A
    DmaFifo         m_fifoB;    // DMA 声音 B

    uint8_t m_masterVolLeft;    // 主音量左 (0-7)
    uint8_t m_masterVolRight;   // 主音量右 (0-7)
    uint8_t m_ch1_left, m_ch1_right;
    uint8_t m_ch2_left, m_ch2_right;
    uint8_t m_ch3_left, m_ch3_right;
    uint8_t m_ch4_left, m_ch4_right;
    bool    m_apuEnabled;

    int     m_outputSampleRate;
    int     m_cyclesPerSample;  // 每个输出样本对应的 CPU 周期数
    int     m_cycleAccum;       // 累积的 CPU 周期

    // 帧序列器（512 Hz）
    int     m_frameSeqTimer;
    int     m_frameSeqStep;

    // 内部时钟推进
    void TickFrameSequencer();
    void TickSquare(SquareChannel& ch);
    void TickWave();
    void TickNoise();
    void TickEnvelope(SquareChannel& ch);
    void TickEnvelopeNoise();
    void TickSweep();
    void TickLength(SquareChannel& ch);
    void TickLengthWave();
    void TickLengthNoise();

    // 混音
    void MixSample(int16_t& left, int16_t& right);

    // 重采样（从 GBA 原生采样率到输出采样率）
    double  m_resampleRatio;
    double  m_resampleAccum;
    int16_t m_lastLeft, m_lastRight;
};
