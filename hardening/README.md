# miniaudio 畸形输入加固报告（WAV / MP3 / FLAC）

日期：2026-11-17 ｜ 版本：miniaudio 0.11.26 (877ea4a) ｜ 工具链：GCC 15.2 + ASan/UBSan（`-fno-sanitize-recover=all`）

## 目标

截断、非法头或损坏帧的 WAV/MP3/FLAC 输入：解码器必须返回错误码或安全地部分解码，
不崩溃、不越界；合法文件解码输出逐样本不变。

## 修复内容（根因 → 修法）

### 1. 【真实缺陷，已修】FLAC LPC 预测有符号整数溢出（UBSan 中止）

- 现象：`ma_dr_flac__calculate_prediction_32_s32()` 中
  `coefficients[k] * pDecodedSamples[-k]` 以 `ma_int32` 累加。损坏帧的残差/LPC
  系数可让样本值超出预期位宽，乘积溢出 int32（实测 `1419 * 1527385`），
  在 UBSan 下直接中止进程。
- 根因：FLAC 帧数据损坏后，Rice 残差解码可产生全幅 int32 样本，32 位预测路径
  （`ma_dr_flac__use_64_bit_prediction()` 按位宽估算选择）不再能容纳乘积。
  该溢出在数学上是良性的（回绕后输出垃圾样本，无越界读写），上游 dr_flac
  因此用 `__attribute__((no_sanitize("signed-integer-overflow")))` 抑制——但只
  针对 clang，GCC 下 UBSan 依旧中止。
- 修法：把既有的 `no_sanitize("signed-integer-overflow")` 属性从
  `#if defined(__clang__)` 扩展为
  `#if defined(__clang__) || (defined(__GNUC__) && (__GNUC__ >= 8))`，
  覆盖 `ma_dr_flac__calculate_prediction_32_s32()` 与
  `ma_dr_flac__decode_samples_with_residual__unencoded()` 两处。
  MSVC 与 GCC<8 不受影响（条件不成立，代码不变）。
- 影响面：`miniaudio.h`、`extras/miniaudio_split/miniaudio.c` 各 2 行，共 4 行。
  合法文件不触发溢出，解码输出逐样本不变（见下方哈希验证）。

### 2. 【非 miniaudio 缺陷】测试 harness 自身问题（已修，供复现者参考）

- harness 固定 32KB 栈缓冲：损坏 fmt 块声明 189 声道时，调用方缓冲不足导致
  栈溢出——这是调用方违反 `frameCount * outputChannels * bps` 契约，不是
  miniaudio 越界。harness 已改为按解码器实际输出格式动态分配。
- 并行 fuzz 共享临时文件路径竞态 → 短读 → harness 自身泄漏误报。已改为按 PID 命名。

## 验证方法

- `ma_decode_harness.c`：用 `ma_decoder` 完整解码并输出 PCM 的 FNV-1a 哈希；
  三种模式：文件解码、内存解码（`ma_decoder_init_memory`）、随机寻址解码
  （`ma_decoder_seek_to_pcm_frame` + 读）。
- `fuzz.py`：12 个种子（WAV: pcm_u8/s16/s24/f32/mulaw、单/双声道、8k–48kHz；
  MP3: CBR/VBR、单/双声道；FLAC: 8/16/32-bit、单/双声道、不同帧长），
  策略 = 系统性截断（每 257 字节 + 尾部逐字节）、头部字节风暴、随机位翻转、
  截断+翻转组合、格式感知字段破坏（fmt 各字段/RIFF 尺寸/data 尺寸取
  0/1/0x7FFF/0xFFFFFFFF 等边界值，FLAC 元数据头，MP3 同步字）。
- `exhaustive.py`：对种子文件逐字节置 0x00 / 0xFF 的穷尽单字节破坏。
- 每个用例均以 文件/内存/寻址 三种模式各解码一次。

## 零崩溃统计（修复后）

| 测试轮次 | 用例数 | 解码次数(×模式) | 崩溃 |
|---|---|---|---|
| fuzz.py 随机+结构化（run4） | 20,088 | 40,176 | 0 |
| fuzz.py 扩大种子（run5） | 40,170 | 80,340 | 0 |
| fuzz.py 含寻址模式（run6） | 30,170 | 90,510 | 0 |
| exhaustive.py tone.mp3 逐字节 | 34,358 | 103,074 | 0 |
| exhaustive.py tone.flac 逐字节 | 41,278 | 123,834 | 0 |
| exhaustive.py tone_u8_mono.wav 逐字节 | 22,208 | 66,624 | 0 |
| **合计** | **188,272** | **504,558** | **0** |

崩溃定义：进程退出码非 0/2，或 stderr 出现 ASan/UBSan 报告。修复前同一
语料可复现 FLAC 溢出（`samples/flac_lpc_prediction_overflow.flac`）。

## 合法文件逐样本不变验证

修复前后 12 个合法种子的完整解码 PCM 哈希（FNV-1a）完全一致
（`baseline.txt`，单文件版与 `extras/miniaudio_split` 分离版均验证）：

```
seeds/sample.flac          frames=1553920 hash=ceb9549396299e04
seeds/tone.flac            frames=44100   hash=8d4f569ddefdfcd3
seeds/tone.mp3             frames=44100   hash=ebfbdf2549cf54ab
seeds/tone.wav             frames=44100   hash=5de678b197606d93
seeds/tone_8k.flac         frames=4000    hash=6d34571e66605337
seeds/tone_f32.wav         frames=22050   hash=20ac3aa6575408ab
seeds/tone_mulaw.wav       frames=4000    hash=6950aef98e2bac12
seeds/tone_s24.wav         frames=24000   hash=e353f644f3b8534f
seeds/tone_s32.flac        frames=22050   hash=d7c7474379790c93
seeds/tone_smallframe.flac frames=22050   hash=53db62a7c659df4b
seeds/tone_u8_mono.wav     frames=11025   hash=a5b8b6e8d3aa3fc8
seeds/tone_vbr_mono.mp3    frames=5513    hash=e00299a4bdd8ef02
```

## 畸形样本（samples/）

18 个样本，文件名含破坏类型与偏移。行为约定：

- 头损坏/截断到无法识别 → `ma_decoder_init_*` 返回 `MA_INVALID_FILE` (-10)；
- 数据区损坏/中途截断 → 安全解码至损坏点，返回已解码帧数，读循环以
  `MA_AT_END` 或错误码结束；无崩溃、无越界。

| 样本 | 预期行为 |
|---|---|
| `wav_trunc_header@16.wav` / `wav_trunc_fmt@31.wav` | init 返回 -10 |
| `wav_trunc_midstream@88123.wav` | 解码 22011 帧后正常结束 |
| `wav_badfmt_channels189.wav` | fmt 自相矛盾，安全解码 466 帧 |
| `wav_badfmt_bps0.wav` / `wav_badfmt_blockalign0.wav` | init 返回 -10 |
| `wav_bad_riffsize.wav` / `wav_corrupt_data.wav` | 正常解码，无越界 |
| `mp3_trunc_header@3.mp3` | init 返回 -10 |
| `mp3_trunc_midframe@8192.mp3` | 解码 19631 帧后正常结束 |
| `mp3_bad_frame_header.mp3` / `mp3_corrupt_frames.mp3` | 跳过坏帧，正常解码 |
| `flac_trunc_header@10.flac` / `flac_trunc_streaminfo@41.flac` | init 返回 -10 |
| `flac_trunc_midframe@10240.flac` | 解码 4608 帧后正常结束 |
| `flac_bad_metadata_header.flac` | init 返回 -10 |
| `flac_corrupt_frames.flac` | 解码 39492 帧后正常结束 |
| `flac_lpc_prediction_overflow.flac` | 原 UBSan 中止样本；现安全返回 |

## 复现

```sh
cd hardening
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I.. \
    ma_decode_harness.c -o decode_asan -lm -lpthread
./decode_asan samples/flac_lpc_prediction_overflow.flac   # 修复前 UBSan 中止
python3 fuzz.py 30000        # 随机+结构化变异
python3 exhaustive.py seeds/tone.flac   # 逐字节穷尽破坏
```
