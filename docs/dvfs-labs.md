# DVFS / Power / Thermal 實戰情境手冊（以 orin-multicam 為載體）

對應職缺：NVIDIA Tegra Power & Thermal Management Software（JR2023967，2027 RDSS）。
這份是自己練習用的，不是對外文件。

---

## 0. 先讀這段

### 0.1 為什麼要練、練什麼

JD 的動詞是 **Implement**：「在 Linux kernel 與 user space 實作 power / thermal 管理功能」，並和 power architect 做 **use-case power estimation**。
你現有的 `docs/interview-zh.md` 把 DVFS 放在「Assist customers to optimize」那一列：**觀察**頻率、**比較** jetson_clocks、**給客戶取捨**。
這份手冊補的是另外三件事：

| 缺口 | 對應 Lab |
|---|---|
| 解釋「為什麼」是那個頻率（governor 的決策邏輯，不只是結果） | 1、2、3 |
| 自己動手「實作」（user-space daemon、kernel module） | 3.6、5.5、7 |
| 用數據「建模」（thermal 的 R·C、use-case 功耗模型） | 5、8 |

### 0.2 方法：每個 Lab 都是「預測 → 量測 → 解釋 → 一句話」

1. **先預測**：在筆記上寫下你認為會發生什麼、數字大概多少。
2. **再量測**：控制變因，一次只動一個 knob，至少跑 3 次。
3. **解釋**：數字和預測不同時，那才是學到東西的地方；用 trace 或讀原始碼找原因。
4. **一句話**：寫下能在面試講出口的結論（每個 Lab 末尾有建議的方向）。

這和你 `docs/debug-log.md` 的「symptom → hypothesis → evidence → root cause」是同一個紀律。

### 0.3 標記約定（請注意區別）

- **【實測】** 我在你這台板子上（2026-11-25）實際讀到或量到的數字。
- **【假說】** 我的推論，等你驗證，**可能是錯的**。
- **【未驗證】** 指令或路徑存在，但「寫入」動作我沒有執行過（我只做了唯讀檢查和一次 `omc` 基準跑），你第一次執行時請留意。

### 0.4 這塊板子的基本盤【實測】

| 項目 | 值 |
|---|---|
| 平台 | Jetson AGX Orin Developer Kit，L4T R35.4.1，kernel `5.10.120-tegra` |
| 目前 power mode | `MODE_30W`（id 2）：CPU 0–7 online、8–11 offline；CPU 729.6–1728 MHz；GPU max 612 MHz；`TPC_PG_MASK=240` |
| cpufreq | driver `tegra194`；policy0（cpu0–3）、policy4（cpu4–7）、policy8（cpu8–11，offline）；硬體範圍 115.2–2201.6 MHz，共 29 個 OPP |
| CPU governor | `schedutil`；全域 tunable 在 `/sys/devices/system/cpu/cpufreq/schedutil/`：`rate_limit_us=2000`、`capacity_margin=1024`、`iowait_boost_max=1024` |
| GPU devfreq | `17000000.ga10b`，governor `nvhost_podgov`（config 為 `DEVFREQ_GOV_POD_SCALING_V2`），11 個 OPP（306–1300.5 MHz），polling 25 ms；可用 governor 另有 `wmark_active userspace performance simple_ondemand` |
| 其他 devfreq | `15340000.vic`（wmark_active，115.2–729.6 MHz）、`15480000.nvdec`、`154c0000.nvenc`（userspace，115.2–998.4 MHz，我看到時停在 998.4） |
| thermal | CPU-therm（zone0）trips：passive 99 °C、active 70 °C（hyst 8 °C）、critical 104.5 °C；policy `step_wise`；可用 `pid_thermal_gov / power_allocator / step_wise` |
| cpuidle | driver `psci_idle`，governor `menu`；state0 `WFI`（latency 1 µs）、state1 `c7`（**exit latency 5000 µs、target residency 30000 µs**） |
| 電源感測 | INA3221 ×2（`hwmon3`、`hwmon4`），rail 檔案 **只有 root 能讀** |
| User-space daemon | `nvfancontrol`（風扇曲線）、`nvphs`、`nvs-service`；設定在 `/etc/nvfancontrol.conf`、`/etc/nvpmodel.conf` |
| 你有的工具 | `nvpmodel`、`jetson_clocks`、`tegrastats`、`gcc`、kernel headers（`/lib/modules/$(uname -r)/build`）、passwordless sudo |
| 沒有的 | `perf`、`trace-cmd`、`bpftrace`、`stress-ng`（ftrace 可用，要 sudo）；kernel **沒開** `CONFIG_PM_DEBUG`（所以沒有 `pm_test`） |

### 0.5 驗證範圍聲明

我只做了：唯讀 sysfs/debugfs/tracefs 檢查、讀 config、跑了一次 `build/omc --frames 300`（預設設定）並用 `trans_stat`/`time_in_state` 前後差分。
**沒有**改任何頻率、governor、thermal trip、power mode，沒有 build 或載入任何 kernel module，沒有 suspend。
所以每個 Lab 裡「寫入類」的步驟都是【未驗證】，結果以你自己量到的為準。

---

## 1. 實驗紀律與安全網（每次實驗前先看）

### 1.1 可重現性

- **冷卻**：每次 run 前 `tj-therm` 應回到接近基線（我看到閒置約 42 °C）。溫度會影響漏電功耗，也會影響 thermal 行為（Lab 5、8）。
- **N ≥ 3**，報 p50 / p99 / max 的分佈，不要只報平均。**同一條 pipeline 我跑出 GPU infer p50 = 3.33 ms，README 記錄是 2.62 ms**，所以單次數字沒有意義。
- 每次記下：`nvpmodel -q`、`uname -r`、`tj` 起始溫度、風扇 `pwm1`、是否有其他負載。
- **量測時不要在旁邊用 shell 輪詢 sysfs**（見 Lab 0 的觀測者效應）；用 trace 或前後差分。

### 1.2 存狀態 / 還原（參考你自己的 `scripts/bench.sh`）

```bash
# 實驗前
sudo rm -f /tmp/jc.conf && sudo jetson_clocks --store /tmp/jc.conf   # --store 不會覆蓋舊檔
nvpmodel -q                                                           # 目前是 MODE_30W (id 2)

# 實驗後（任何 Lab 結束都做）
sudo jetson_clocks --restore /tmp/jc.conf
sudo nvpmodel -m 2
echo 700 | sudo tee /sys/kernel/debug/17000000.ga10b_scaling/load_target   # 若動過 podgov
# thermal trip / emul_temp / governor 若動過，一定寫回原值（見各 Lab）
```

### 1.3 硬規則

1. **永遠不要對 `emul_temp` 寫入 ≥ `critical` trip（104500）的值**：kernel 會視為過熱而關機。我建議上限 100000，只動 `CPU-therm` 一個 zone，用完立刻寫回 `0`。
2. **不要停掉風扇**。要實驗風扇控制見 Lab 5.5 的護欄。
3. **kernel module 實驗（Lab 7）**：開第二個終端 `sudo dmesg -w`；bug 可能造成 panic 而重開機，先存檔；不要在 `omc` 執行中載入未測試的 module。
4. **`nvpmodel` 會記住上次的 mode**，重開機後沿用。實驗完一定切回 `-m 2`。**不要用 `--force`**（它會在需要時自動重開機）。
5. **Suspend（Lab 9）只在本機 console 做**，SSH / VS Code remote 會斷線。

---

## 2. 路線圖

| Lab | 主題 | 對應 JD | 要寫的程式 | 粗估 |
|---|---|---|---|---|
| 0 | 儀表：量測不騙人 | system-level debugging | `tools/dvfs_diff.c` | 0.5 天 |
| 1 | 誰說了算：頻率決策的層層疊疊 | Linux PM features | `scripts/knobs.sh` | 0.5 天 |
| 2 | schedutil × 30 fps 週期負載 | scheduler, DVFS | `tools/duty.c` | 1 天 |
| 3 | GPU 停在 306 MHz 的代價 | DVFS, user-space PM | frame-boost daemon | 1–1.5 天 |
| 4 | 媒體引擎與 EMC：用 deadline 反推最低頻率 | power/perf optimization | 無（用現有 omc） | 1 天 |
| 5 | 熱：trip、cooling device、R·C 模型 | thermal mgmt | 熱感知 workload governor | 1.5–2 天 |
| 6 | 閒置的藝術：cpuidle 與 GPU rail-gating | runtime PM | `tools/rg_probe.c` | 1 天 |
| 7 | 寫 kernel code：`omc_pm` 模組 | **Implement … in Linux Kernel** | 延伸 `kdrv-hello` | 2–3 天 |
| 8 | 收斂：use-case 功耗模型 + 自訂 power mode | **usecase power estimation** | 回歸腳本 | 1.5 天 |
| 9 | （選修）Suspend-to-RAM 追蹤 | suspend to RAM | 無 | 0.5 天 |

**時間不夠的最小集合**：0 → 2 → 3 → 5（5.1–5.3）→ 7a → 8a。這一條線同時涵蓋 user space、kernel、thermal、建模。

---

## Lab 0 — 儀表：量測不騙人

**要搞懂**：你量到的是系統，還是你的量測動作？

### 【實測】觀測者效應

系統閒置時，我連續讀 6 次 `policy0/scaling_cur_freq`：`1728000, 729600, 1728000, 960000, 1728000, 729600`；
同一時間 `tegrastats` 顯示 8 顆核全部在 729 MHz。
**我的 `cat` 本身就是 workload**，它喚醒 CPU、觸發 governor 升頻。

### 三種觀測方式

| 方式 | 例子 | 觀測者效應 | 適用 |
|---|---|---|---|
| A. 核心累計計數器，**前後差分** | `cpufreq/stats/{time_in_state,total_trans,trans_table}`、devfreq `trans_stat`、`cpuidle/state*/{usage,time}` | 幾乎無 | 長時間統計（本手冊預設用這個） |
| B. ftrace 事件 | `power:cpu_frequency`、`power:cpu_idle`、`devfreq:devfreq_monitor`、`nvhost_podgov:podgov_*`、`thermal:*` | 小、有時間戳 | 因果分析、時間軸 |
| C. 輪詢 | `tegrastats`、hwmon | 取決於間隔 | 功耗、溫度 |

### 做法

```bash
# A. 差分（我實際用過的）
GPU=/sys/class/devfreq/17000000.ga10b
awk 'NR>2 && NF>=12 {gsub(/\*/,""); print $1, $NF}' $GPU/trans_stat > before.txt   # 每個 OPP 累計 ms
cat /sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state                         # 單位 10 ms
cat /sys/devices/system/cpu/cpu0/cpufreq/stats/total_trans

# B. ftrace（需要 sudo；/sys/kernel/tracing 一般使用者讀不到）
sudo sh -c 'cd /sys/kernel/tracing; echo 0 > tracing_on; echo > trace
  echo 1 > events/power/cpu_frequency/enable
  echo 1 > events/power/cpu_idle/enable
  echo 1 > events/nvhost_podgov/podgov_do_scale/enable
  echo 1 > events/devfreq/devfreq_monitor/enable
  echo 1 > tracing_on'
sudo cat /sys/kernel/tracing/trace_pipe        # 看完 echo 0 > tracing_on，並 disable 這些事件

# C. 功耗：INA3221（root）。VDD_GPU_SOC 我對到 3209 mW vs tegrastats 3208 mW
sudo sh -c 'for i in 1 2 3; do echo $(cat /sys/class/hwmon/hwmon3/in${i}_label) \
  $(( $(cat /sys/class/hwmon/hwmon3/in${i}_input) * $(cat /sys/class/hwmon/hwmon3/curr${i}_input) / 1000 )) mW; done'
# hwmon3: in1=VDD_GPU_SOC in2=VDD_CPU_CV in3=VIN_SYS_5V0；hwmon4: in2=VDDQ_VDD2_1V8AO
# ⚠ 只有 GPU_SOC 這一路我有對到 tegrastats；其他 rail 請你自己對照校準再信
```

### 你要寫：`tools/dvfs_diff.c`（C）

```
dvfs_diff [--cpu-pol 0,4] [--devfreq 17000000.ga10b,15340000.vic,...] -- <cmd> [args]
```

跑完 `<cmd>` 後印出：每個 cpufreq policy 的頻率殘留 %、transitions/s；每個 devfreq 的頻率殘留 %；cpuidle 各 state 的殘留 %；以及（用一條 20 Hz 的取樣 thread，pin 在不跑負載的核）積分出來的能量（J）和平均功率（W）。

### 驗收

用 `dvfs_diff -- build/omc --frames 300` 重現下表（±10%）：

| 【實測】指標（預設 governor、無 `--output`、`MODE_30W`） | 值 |
|---|---|
| cpu0 `total_trans` 增量 | **3175**（`time_in_state` 合計約 13 s，≈ 245 次/s） |
| cpu0 頻率殘留 | **729 MHz 6.33 s、1728 MHz 5.58 s**，其餘 12 個 OPP 合計約 1.1 s（雙峰） |
| GPU 頻率殘留 | 306 MHz **12372 ms**、612 MHz 36 ms、**1300 MHz 44 ms** |
| e2e p50 / p99（四個 cam） | 15.8–21.1 ms / 25.6–27.6 ms |
| infer p50 | GPU cam 3.33 ms、DLA cam 1.86–1.89 ms |

寫下三個問題的答案：為什麼 cpu0 是雙峰？為什麼 GPU 有 44 ms 在 1300 MHz（但 `max_freq` 是 612 MHz）？這兩件事分別留給 Lab 2 和 Lab 3.5。

**面試一句話**：「我不用 shell 輪詢 `scaling_cur_freq`，因為讀取本身會喚醒 CPU；我用核心累計的 `time_in_state` 前後差分和 ftrace。」

---

## Lab 1 — 誰說了算：頻率決策的層層疊疊

**要搞懂**：一個頻率不是某個人決定的，是多個「限制」疊加後的結果。你必須能說出「某個 sysfs 寫入最後落到哪一層、被誰覆蓋」。

### 這台板子上的分層【實測 + 推論】

| 層 | 實體 | 觀察位置 |
|---|---|---|
| power mode（user-space 工具） | `nvpmodel` 依 `/etc/nvpmodel.conf` 寫 sysfs：CPU online、`CPU_A78_x` min/max、`GPU` min/max、`TPC_PG_MASK`、DLA/PVA clock | `nvpmodel -q`；`scaling_{min,max}_freq` |
| cpufreq policy | `policy0/4/8`；`cpuinfo_*`＝硬體、`scaling_*`＝policy 限制 | `/sys/devices/system/cpu/cpufreq/policy*/` |
| governor | `schedutil`（全域 tunable） | `…/cpufreq/schedutil/` |
| driver | `tegra194`（把目標頻率換成 NDIV 寫進硬體） | `scaling_driver` |
| thermal cooling | `thermal-cpufreq-0/1`（max_state 28 ↔ 29 個 OPP）、`thermal-devfreq-0`（max_state 10 ↔ GPU 11 個 OPP【假說】）、`cpu-throttle-alert` | `/sys/class/thermal/cooling_device*` |
| devfreq | 每個 engine 各自的 governor | `/sys/class/devfreq/*/` |

**觀察**：MODE_30W 的 nvpmodel 區塊只有 `CPU_A78_0`、`CPU_A78_1`（沒有 `_2`），而 policy8 的 `scaling_max_freq` 因此仍是 2201600（policy0/4 是 1728000）——nvpmodel 只管有出現在設定檔裡的部分。

### 做法

**1.1 knob 傾印工具**：寫 `scripts/knobs.sh`，把下列全部印成 `key=value`：`policy*/scaling_{governor,min_freq,max_freq}`、`online`、各 devfreq 的 `{governor,min_freq,max_freq}`、`tpc_pg_mask`、`schedutil/*`、podgov 六個 debugfs 值、各 cooling device 的 `cur_state`。

**1.2 逆向 nvpmodel**：**先預測**從 `/etc/nvpmodel.conf` 看 MODE_30W、MODE_15W、MAXN 各會改哪些 knob，再實測：

```bash
scripts/knobs.sh > a.txt
sudo nvpmodel -m 3 ; scripts/knobs.sh > b.txt      # MODE_50W（id 3）；MODE_15W(id 1) 會下線 cpu4-11，先停掉所有負載
sudo nvpmodel -m 2
diff a.txt b.txt
```

預測和 diff 的差異就是你對 nvpmodel 的誤解。

**1.3 「最後寫的人贏嗎？」**【未驗證】

```bash
P=/sys/devices/system/cpu/cpufreq/policy0
echo 1113600 | sudo tee $P/scaling_max_freq
cat $P/scaling_max_freq
sudo nvpmodel -m 2 ; cat $P/scaling_max_freq    # 預測：被還原成 1728000？還是維持 1113600？
```

接著用 trace 看每次「有效上下限」的改變：`events/power/cpu_frequency_limits/enable`。

**1.4 `cur_freq` 的兩個定義**：`scaling_cur_freq` 與 `cpuinfo_cur_freq` 差在哪？【假說】前者是 policy 記錄的「軟體最後一次要求」，後者向 driver 讀「硬體實際」。**去讀 kernel 的 `drivers/cpufreq/cpufreq.c` 和 `tegra194-cpufreq.c` 驗證**，並解釋你在 Lab 0 看到的亂跳。

### 驗收

一張圖：從 `nvpmodel`、user sysfs、thermal cooling、governor、driver 到硬體，標出每一層「用什麼介面寫、影響 min 還是 max、誰會覆蓋誰」。
關鍵概念（在 mainline 5.10）：min/max 是多個 `freq_qos` request 的聚合——**有效 max ＝ 所有 MAX request 取最小，有效 min ＝ 所有 MIN request 取最大**。請在原始碼中找到證據（`include/linux/pm_qos.h`、`kernel/power/qos.c`、`cpufreq_cooling.c`）。

**面試一句話**：「頻率上下限是多個 QoS request 的聚合，user 寫 `scaling_max_freq`、thermal cooling、nvpmodel 是不同的 request，不是互相覆蓋。」（先確認你在 Tegra 這棵 tree 驗證過這句話再說。）

---

## Lab 2 — schedutil × 30 fps 週期負載

**要搞懂**：PELT → util → 目標頻率 → rate limit → driver 這條鏈；以及為什麼「每 33 ms 醒一次的 pipeline」會讓頻率在最低與最高之間擺盪（Lab 0 的雙峰）。

### 原理

- PELT：util 以指數衰減追蹤，**半衰期 32 ms**（約 `y^32 = 0.5`）。frame 週期 33.3 ms ≈ 一個半衰期，閒置 28 ms 後 util 只剩約 `0.5^(28/32) ≈ 55%`。
- schedutil（mainline）：`next_freq ≈ 1.25 × util / capacity × max_freq`，更新受 `rate_limit_us` 限制。
- 這棵 tree 多了 `capacity_margin` 與 `iowait_boost_max`（**據我所知不在 mainline 5.10**）。我讀到 `capacity_margin=1024`，如果解讀為 1.0×，就沒有 mainline 的 1.25× headroom。**【假說】請讀原始碼確認它到底乘在哪。**

### 2.1 合成負載 `tools/duty.c`（先用它，omc 太複雜無法歸因）

```
duty --cpu 3 --period-ms 33.333 --busy-ms 5 --seconds 20 --csv out.csv
```

- 先校準：在 `performance` governor 下，決定一個固定的迴圈次數 `N`，使它花 `busy-ms`。
- 每個週期：`clock_nanosleep(TIMER_ABSTIME)` 對齊 → 執行 `N` 次迴圈 → 記錄花了多少時間（response time）。
- **用「工作量當頻率計」**：`N` 固定，所以 `response_time ∝ 1 / f_eff`；`f_eff ≈ f_max × t_perf / t_measured`。這樣完全不需要讀 sysfs，沒有觀測者效應。

掃描 `busy-ms ∈ {1, 3, 5, 10, 20, 30}` × governor `{performance, schedutil, ondemand, conservative, powersave}`，記錄 response time 的 p50/p99 與 `VDD_CPU_CV` 功耗。

**先預測**：輕負載（1–3 ms）時 schedutil 的 response time 比 performance 慢多少？最壞可能達 `1728/729 ≈ 2.4×`。哪個 busy-ms 之後差距消失？為什麼？

### 2.2 回到 omc（4 cam、30 fps）

掃描並填表（每格 3 次，用 `dvfs_diff` 取數）：

| 設定 | e2e p99（worst cam） | infer p50 | `VDD_CPU_CV` | cpu0 trans/s | 729↔1728 殘留 |
|---|---|---|---|---|---|
| 預設（schedutil, rate_limit 2000, margin 1024） | | | | | |
| `rate_limit_us` 500 / 10000 | | | | | |
| `capacity_margin` 1280 / 1536 | | | | | |
| governor `performance` / `ondemand` | | | | | |
| `--rt --pin` | | | | | |

```bash
echo 500 | sudo tee /sys/devices/system/cpu/cpufreq/schedutil/rate_limit_us     # 預設 2000，用完寫回
```

### 2.3 讀原始碼

`kernel/sched/cpufreq_schedutil.c`：`sugov_update_single` / `sugov_update_shared`、`get_next_freq`、`sugov_should_update_freq`（rate limit 在這）、`sugov_iowait_boost`；`kernel/sched/pelt.c`。
**問題**：`rate_limit_us=2000` 理論上限 500 次/s，為什麼你量到約 245 次/s？換成 `rate_limit_us=10000` 會變多少？

### 驗收

能用一張時間軸圖解釋「一個 frame 的 5 ms 工作，在 schedutil 下前幾 ms 為什麼跑在低頻」，並用 `duty` 的數據佐證。

**面試一句話**：「週期負載的閒置時間和 PELT 半衰期同量級，util 在下一個 burst 開始時只剩約一半，所以 schedutil 的第一段 burst 跑在偏低頻率；這是 latency 問題，不是 throughput 問題。」

---

## Lab 3 — GPU 停在 306 MHz 的代價

**要搞懂**：load-based governor 優化的是**利用率**，不是 **deadline**。這是你專案已有的觀察（`jetson_clocks`：GPU infer p50 2.6 → 1.6 ms，功耗 6.0 → 6.3 W），現在要解釋它並做出更好的策略。

### 【實測】事實

- 預設下 GPU 99% 的時間在 306 MHz（Lab 0）。README 的 tegrastats：GPU util 30% @ 305 MHz。
- podgov 旋鈕在 **debugfs**：`/sys/kernel/debug/17000000.ga10b_scaling/`：`load_target=700`、`load_max=900`、`bias=80`、`damp=7`、`smooth=10`、`block_window=50000`。
- GPU load 在 `/sys/devices/platform/17000000.ga10b/load`（閒置時讀到 0）。
- trace 事件：`nvhost_podgov:podgov_{load,do_scale,estimate_freq,hint,set_freq_request,…}`（`podgov_load` 有欄位 `load`，`podgov_do_scale` 有 `old_freq/new_freq`）。

### 3.1 為什麼停在 306？**【假說】**

2 個 GPU cam × infer 3.3 ms × 30 fps ≈ 20% 忙碌度（再加 preprocess / overlay kernel ≈ 30%）。若 `load_target=700` 是 0–1000 尺度上的 70%，那 30% 遠低於目標，podgov 沒有理由升頻——**但每個 frame 的 deadline 並不在乎平均忙碌度**。
用 `podgov_load` + `podgov_do_scale` 的 trace 證明或推翻這個說法（尺度是不是 0–1000？`load_target` 到底是什麼意義？）。**去讀 NVIDIA tree 的 pod scaling v2 governor 原始碼**（`grep -ri podgov` 找）。

### 3.2 掃描「頻率下限」，找 Pareto knee

```bash
G=/sys/class/devfreq/17000000.ga10b
echo 510000000 | sudo tee $G/min_freq      # 【未驗證】掃 306/408/510/612 MHz；結束寫回 306000000
```

每個下限跑 3 次，記 infer p50/p99、e2e p99、`VDD_GPU_SOC` 功耗，畫 **功耗 vs p99 latency** 的 Pareto 圖，找出 knee（再升頻不划算的點）。

### 3.3 調 `load_target`

```bash
echo 400 | sudo tee /sys/kernel/debug/17000000.ga10b_scaling/load_target    # 700 → 400 → 200，結束寫回 700
```

這和 3.2 的 `min_freq` 有什麼不同？（提示：一個是改「政策」，一個是改「下限」。哪個在負載突然變大時仍有彈性？）

### 3.4 換 governor

試 `performance`、`simple_ondemand`、`userspace`（寫 `userspace/set_freq`）。每個都回答：它依據什麼訊號？更新週期？為什麼 NVIDIA 要自己寫 podgov 而不是用 simple_ondemand？

### 3.5 謎：為什麼有 44 ms 在 1300 MHz？

`max_freq=612 MHz`，但 `trans_stat` 顯示 1300 MHz 有 44 ms；另外 `/sys/devices/platform/17000000.ga10b/freq_request` 讀到 `1300500000`。
用 trace（`podgov_set_freq_request`、`podgov_hint`、`clock:clock_set_rate`、`devfreq_monitor`）找出**誰在什麼時候要求的**。**我不知道答案**，下面是可以先檢查的方向：是不是只出現在 omc 啟動階段（CUDA context / TensorRT engine 載入）？是不是 GPU 從 rail-gate 醒來時的暫時行為？

### 3.6 實作：frame-boost（user space 版）

做一個 daemon：omc 在每個 frame 抵達時通知它（`eventfd` 或 UNIX datagram），它把 GPU `min_freq` 拉到 612 MHz，約 8 ms 後（`timerfd`）放回 306 MHz。

- daemon：`SCHED_FIFO`、pin 到沒用到的核；`min_freq` 的 fd 保持開啟，用 `pwrite`。
- **量測 `pwrite` 的延遲分佈**（`clock_gettime` 夾住）。【假說】µs 到 ms 級，因為最後要走 devfreq mutex → clock 設定 → BPMP IPC。
- 比較四種：預設 / `jetson_clocks` / 固定 `min_freq` / frame-boost，看 infer p50/p99、功耗、daemon 自己的 CPU 成本。

**這一步的目的**是讓你親手量出 user-space hint 的成本，然後在 Lab 7 用 kernel 實作同一件事，並能量化說明**為什麼產品需要 kernel 介面**。

### 驗收

一張 Pareto 圖（功耗 vs p99），上面至少有 6 個點（預設、固定 4 種 min_freq、frame-boost），並寫出 frame-boost 的淨收益或淨損失。**誠實報告負面結果也算驗收通過**。

**面試一句話**：「load-based governor 看平均利用率，週期性 deadline 負載需要 hint 式的策略；我量過 user-space hint 的成本，所以知道什麼時候該下放到 kernel。」

---

## Lab 4 — 媒體引擎與 EMC：用 deadline 反推最低頻率

**要搞懂**：給定 deadline（4 × 30 fps 的 33 ms），在不超時的前提下，**每個 engine 要多少 MHz 才夠**？降頻真的省電嗎，還是 race-to-idle 更好？

### 【實測】事實與你的程式碼

- `15480000.nvdec` / `154c0000.nvenc`：governor `userspace`，我看到時 `cur_freq=998400000`（最高）；`15340000.vic`：`wmark_active`，729.6 MHz（最高）。
- **omc 預設會強制 NVDEC 全速**：`src/v4l2_dec.c:67` 設 `V4L2_CID_MPEG_VIDEO_MAX_PERFORMANCE=1`；有對應旗標 `--no-max-perf`。**做本 Lab 一律加 `--no-max-perf`**，否則你手動設的頻率可能被它蓋掉。
- NVDEC/NVENC 的 devfreq 有 `userspace/set_freq`。
- EMC：`sudo cat /sys/kernel/debug/clk/emc/clk_rate` 讀到 2133000000（閒置時單次讀取）。我印象中 AGX Orin 64GB 的 LPDDR5 上限是 3200 MHz，但**我沒有查證，也不確定閒置時為何是 2133**；請以實際可用的 rate 為準。`tegrastats` 的 `EMC_FREQ x%@MHz` 是另一個觀察角度。

### 4.1 NVDEC 頻率掃描

```bash
D=/sys/class/devfreq/15480000.nvdec
cat $D/available_frequencies                           # 只能設表裡真實存在的 OPP
echo 499200000 | sudo tee $D/userspace/set_freq       # 【未驗證】掃 998400000 / 704000000 / 499200000 / 307200000 / 204800000
build/omc --no-max-perf --frames 450 --csv out.csv --json out.json
```

用 `--csv`（每個 frame 每個 stage 的時間戳）算 **decode stage 時間 vs NVDEC 頻率**，並記 e2e p99 與功耗。

**先預測**：decode 時間約和頻率成反比，直到某個頻率以下超時？超時前功耗降多少？**能量/frame ＝ P × t / frames** 在哪個頻率最低？

### 4.2 為什麼降頻不一定省能量

NVDEC/NVENC/VIC 大概和 GPU、SOC 共用 `VDD_GPU_SOC` 這條 rail（【假說】，tegrastats 只有這個 rail 名稱）。若電壓不能依 engine 個別調整，降頻只減少動態功耗 `C·V²·f` 的 `f` 項，**漏電不變**，race-to-idle 可能反而更省。用 4.1 的資料驗證，並解釋你看到的結果。

### 4.3 EMC 與 DRAM 競爭（接你 README 的 memcpy storm）

你的 README：memcpy storm 下 e2e max 38.5 → **71.5 ms**，p50 不變。這是頻寬競爭，還是 EMC 頻率不夠？用**控制變因**拆開：

| 條件 | e2e p50 | p99 | max |
|---|---|---|---|
| 預設 | | | |
| 預設 + storm（你已有） | | | 71.5 |
| `jetson_clocks`（EMC 鎖最高） | | | |
| **`jetson_clocks` + storm（新）** | | | |

如果鎖了 EMC 之後 storm 的 max 仍然很高 → 是競爭，不是頻率。同時看 `sudo cat /sys/kernel/debug/clk/emc/clk_rate`、`tegrastats` 的 EMC_FREQ，以及 trace 事件 `bwmgr`、`isomgr`、`interconnect`。

### 驗收

一張表：每個 engine 的「最低可行頻率」與其功耗/能量。再一句判斷：這條 pipeline 適合 race-to-idle 還是 just-in-time。

**面試一句話**：「我用 deadline 反推每個 engine 的最低頻率，並用能量/frame 驗證降頻是否真的省電；隔離 EMC 頻率和頻寬競爭用的是控制變因。」

---

## Lab 5 — 熱：trip、cooling device、R·C 模型

**要搞懂**：thermal 是一個閉迴路控制系統。你要能畫出它、能量化它、也能實作其中一環。

### 【實測】這塊板子的 thermal 結構

- zones：`CPU-therm`(0)、`GPU-therm`(1)、`CV0-2`、`SOC0-2`、`tj-therm`(8)、`Tboard`、`Tdiode`；GPU 與 CV 在我讀的時候是 `-256000`（沒有該 sensor 讀值）。
- `CPU-therm` trips：`1: passive 99000`、`2: active 70000`（hyst 8000）、`0: critical 104500`。
  綁定：`thermal-cpufreq-0/1`、`thermal-devfreq-0`、`cpu-throttle-alert` → trip 1（passive）；`hot-surface-alert` → trip 2（active）。policy `step_wise`。
- `tj-therm`：sysfs 沒有 `trip_point_*`，但有 `k_po/k_pu/k_i/k_d/sustainable_power/integral_cutoff/passive_delay` 等 PID 旋鈕（目前全 0）。
- `CONFIG_THERMAL_WRITABLE_TRIPS=y`，`trip_point_*_temp` 檔案權限 `rw-rw-r--`（root）。`emul_temp` 只有 root 可寫。
- **pwm-fan 沒有綁在任何 zone**。風扇是 user-space daemon `nvfancontrol` 控制的（`/etc/nvfancontrol.conf`：`POLLING_INTERVAL 2`、`FAN_GOVERNOR pid/cont`、`FAN_PROFILE cool/quiet`、`TMARGIN ENABLED`）。這是一個現成的 user-space thermal 管理範例。

### 5.1 畫迴圈

用上面的事實畫出：plant（SoC 熱）→ sensor（`tj`）→ controller（step_wise / PID / nvfancontrol）→ actuator（cpufreq 降階 / devfreq 降階 / fan PWM / HW throttle alert）。
標出每個 sysfs 檔案在迴圈的哪個位置。

### 5.2 觸發 trip（兩種方法，建議 A）

```bash
Z=/sys/class/thermal/thermal_zone0
cat $Z/type $Z/temp ; grep . $Z/trip_point_*
for n in 0 1 2 3 4; do echo cdev$n $(cat $Z/cdev$n/type) trip=$(cat $Z/cdev${n}_trip_point); done

# 方法 A（較安全）：把 passive trip 調低到接近目前溫度，用真負載觸發。【未驗證驅動是否接受寫入】
echo 50000 | sudo tee $Z/trip_point_1_temp        # 原值 99000，結束必須寫回

# 方法 B：emul_temp（毫攝氏）。⚠ 絕不寫 >= 104500。只動 CPU-therm，只能 5 秒內。
echo 100000 | sudo tee $Z/emul_temp ; sleep 5 ; echo 0 | sudo tee $Z/emul_temp
```

觀察：`cooling_device0/cur_state`（0→?）、`policy0/scaling_max_freq`（5.10 讀的是有效 max）、GPU `max_freq`；trace：`thermal:thermal_zone_trip`、`thermal:cdev_update`、`thermal:thermal_temperature`、`power:cpu_frequency_limits`。
問題：step_wise 每個 polling 週期升降幾個 state？溫度回落時它如何決定何時放開？

### 5.3 量測 R·C（建立 thermal 模型）

用 `membw_hog` 加 `omc --fps 0`（capacity 模式）當功耗階躍，**保留 `nvfancontrol` 運作**（不要停它），記錄：

```bash
while sleep 1; do echo $(date +%s) $(cat /sys/class/thermal/thermal_zone8/temp) \
  $(cat /sys/class/hwmon/hwmon2/pwm1) ; done > thermal.csv        # 另外用 tegrastats 記功耗
```

擬合：`T(t) = T0 + ΔT·(1 − e^{−t/τ})`，得 `R_th = ΔT / ΔP`（°C/W）、`τ`、`C_th = τ / R_th`。
**只取風扇 PWM 固定的區段**（風扇狀態會改變 R_th）。再用模型預測另一個負載（例如 README 的 4 cam 6.0 W vs capacity 7.4 W）的穩態 tj，並實測驗證誤差。

### 5.4 step_wise vs power_allocator（探索性）

把 `tj-therm` 切到 `power_allocator`，設 `sustainable_power`（mW）與 `k_po/k_pu/k_i/k_d`，看它會不會被接受（cooling device 必須實作 power 介面），比較頻率的抖動幅度與溫度 overshoot；結束寫回 `step_wise`。
**這是探索性的**：我不知道 NVIDIA 的 cooling device 是否支援。若被拒絕，就去讀 `drivers/thermal/gov_power_allocator.c` 找出需要哪些 callback，這本身就是答案。

### 5.5 實作：熱感知 workload governor（user space，建議做）

寫一個 daemon：讀 `tj-therm`（與功率），超過門檻時對 omc 降低負載（例如把一個 GPU cam 切到 DLA、或降 `--fps`），降溫後恢復。

- 要有**遲滯**（hysteresis）和**最小停留時間**，避免震盪；要有逾時保護（讀不到溫度時 fail-safe 降載）。
- 驗收：在 5.2 的方法 A（低 trip）下，比較「只靠 kernel cooling」與「加上你的 governor」的 e2e p99 與 tj 曲線。

**（進階、有風險）解剖 `nvfancontrol`**：讀 `/etc/nvfancontrol.conf`，畫出風扇曲線；注意 `TMARGIN ENABLED` 表示 TEMP 欄位可能不是絕對溫度（請驗證）。**不要**自己取代風扇控制，除非：`ExecStopPost` / signal handler / `atexit` 一律還原為 `systemctl start nvfancontrol`，PWM 永不低於它的最小值，並在有人在場時做。

### 驗收

(1) 控制迴圈圖；(2) 你擬合出的 `R_th`、`τ`，以及用它預測另一個 use case 穩態 tj 的誤差；(3) 一個會動作的 5.5 daemon。

**面試一句話**：「我把 SoC 當一階熱系統量過 R_th 和 τ，可以在上線前預測一個 use case 的穩態溫度；也做過 user-space 的熱感知降載。」

---

## Lab 6 — 閒置的藝術：cpuidle 與 GPU rail-gating

**要搞懂**：省電有一半來自「睡得進去、睡得夠久」。`target_residency` 與 `exit_latency` 就是 break-even 分析。

### 【實測】事實

- cpuidle：`WFI`（latency 1 µs / residency 1 µs）、`c7`（**latency 5000 µs / residency 30000 µs**）；governor `menu`；driver `psci_idle`。閒置時 cpu0 累計：`state0 usage=626310`、`state1 usage=85126`。
- GPU（`/sys/devices/platform/17000000.ga10b/`）：`power/control=auto`、`runtime_status=suspended`、`power/autosuspend_delay_ms=500`、`railgate_enable=1`、`railgate_delay=500`、`is_railgated=yes`、`gpu_powered_on=off`。
- `/dev/cpu_dma_latency` 存在（root，0600）。

### 6.1 cpuidle：frame rate 如何改變睡眠深度

**先預測**：30 fps 時 frame 間隔 33 ms，每個 cam thread 的閒置段通常 < 30 ms，menu governor 預測這個長度後幾乎不會選 `c7`（residency 30 ms）。改成 `--fps 15`（閒置約 66 ms）會出現 `c7`。

```bash
# 前後差分（所有 cpu 的每個 state）
for s in /sys/devices/system/cpu/cpu[0-7]/cpuidle/state*; do
  echo $s $(cat $s/name) usage=$(cat $s/usage) time_us=$(cat $s/time); done
```

比較 `--fps 15 / 30 / 60`、有無 `--pin` 的 `c7` 殘留 % 與 `VDD_CPU_CV`。

**PM QoS**：持有 `/dev/cpu_dma_latency` 寫入 `0`（保持 fd 開啟），預測 `c7 usage` 不再增加；用它估算「禁止深度睡眠」對延遲與功耗的影響。

```c
int fd = open("/dev/cpu_dma_latency", O_WRONLY); int32_t v = 0; write(fd, &v, 4);  /* fd 開著才有效 */
```

### 6.2 GPU rail-gating break-even：`tools/rg_probe.c`

寫一個程式：每隔 `T` 發一個極小 CUDA kernel + 同步，**量第一次 launch 的延遲**，並讀 `is_railgated` 與 `power/runtime_*_time`（若存在）。
**注意**：讀某些 GPU sysfs 檔案本身可能改變電源狀態，請先確認讀 `is_railgated` 不會喚醒 GPU（這又是觀測者效應）；不確定時用前後差分的累計時間，而不是高頻輪詢。
掃 `T ∈ {100, 300, 500, 700, 1000, 3000} ms`，每個 `T` 跑 60 s，記 `VDD_GPU_SOC` 平均功率。

導出：**喚醒延遲**、**每次喚醒的額外能量**、**break-even 間隔** `T_be = E_wake / (P_idle_ungated − P_idle_gated)`；對照 `railgate_delay=500`。

### 6.3 連回你的專案

omc 每 33 ms 就有 GPU 工作，遠小於 `railgate_delay=500`，所以 GPU 在 pipeline 執行時**永遠不會 rail-gate**。
README「What I would do next」的「把兩個 GPU cam batch 成一次 enqueue」，除了減少 launch overhead，**也是「合併工作以拉長閒置段」的省電手法**（就像 timer coalescing）。用 6.2 的 break-even 數字回答：要把閒置拉到多長，rail-gate 才會划算？

### 6.4 讀原始碼

`drivers/cpuidle/governors/menu.c`（`menu_select`、predicted_us、correction factor、latency requirement）；`drivers/base/power/runtime.c`（`pm_runtime_get_sync` / `put_autosuspend`、usage_count、autosuspend timer）。

**面試一句話**：「rail-gating 和 cpuidle 都是 break-even 問題；我量過 GPU 的喚醒延遲與能量，知道多長的閒置才值得睡。」

---

## Lab 7 — 寫 kernel code：`omc_pm` 模組（JD 的核心動詞）

**起點**：你的 `~/kdrv-hello` 已經有 misc device + ioctl + uapi header + 在這台機器上 build 出 `.ko` 的完整流程。
**確認過的前提**：`MODULE_UNLOAD=y`、`MODULE_SIG_FORCE` 沒設、`Module.symvers` 裡這些符號可用：

| 符號 | 匯出 |
|---|---|
| `freq_qos_add_request` / `_update_request` / `_remove_request` | GPL |
| `cpufreq_cpu_get` / `cpufreq_cpu_put` | GPL |
| `devfreq_add_governor` / `devfreq_remove_governor` / `update_devfreq` / `devfreq_update_status` | 非 GPL |
| `thermal_cooling_device_register` / `thermal_zone_bind_cooling_device` / `thermal_zone_get_zone_by_name` / `thermal_zone_get_temp` | GPL |
| `dev_pm_qos_add_request` | GPL |

保持 `MODULE_LICENSE("GPL")`（你的 kdrv-hello 已是）。

> **安全**：另開終端 `sudo dmesg -w`；先在沒有 omc 的情況下載入；每個階段都要能 `rmmod` 乾淨卸載（`dmesg` 不能有警告）。

### 7a — Frame-boost：`FREQ_QOS_MIN`（把 Lab 3.6 下放到 kernel）

- ioctl `OMC_IOC_BOOST { cpu, min_khz, duration_us }`。
- 對該 CPU 的 policy：`cpufreq_cpu_get` → `freq_qos_add_request(&policy->constraints, &req, FREQ_QOS_MIN, khz)`；用 `hrtimer`（或 `delayed_work`）在 `duration_us` 後 `freq_qos_update_request(&req, FREQ_QOS_MIN_DEFAULT_VALUE)`。
- **要處理的坑**（這些就是面試會被追問的）：
  - 重複的 boost 要**延長**而不是疊加；
  - `module_exit` 的順序：先 `hrtimer_cancel`（或 `cancel_delayed_work_sync`），再 `freq_qos_remove_request`，再 `cpufreq_cpu_put`；
  - policy 生命週期：`cpufreq_cpu_get` 在 CPU 下線時可能回 `NULL`（**MODE_30W 的 policy8 是 offline**），要處理；
  - ioctl 與 timer 的並行（鎖的選擇：timer callback 在 softirq/hardirq，不能睡眠）。
- 驗證：`events/power/cpu_frequency_limits` 要能看到你的 request 進出；用 Lab 2 的 `duty` 量 response time 改善，並和 3.6 的 user-space 版比 hint 延遲。

### 7b — 自訂 devfreq governor `omc_gov`

- 實作 `struct devfreq_governor`（`.name`、`.get_target_freq`、`.event_handler`），`devfreq_add_governor()` 註冊。
- **先在 `15480000.nvdec`（userspace 管理、風險最低）上試**，不要一開始就上 GPU：GPU 的 devfreq 由 nvgpu 內部驅動，是否能提供你的 governor 需要的 `get_dev_status`、能否安全切換，**我不確定**。
- 策略：有 hint 時拉到高頻，沒有 hint 時退回依 `last_status`（busy/total）的簡單 load 策略。
- `echo omc_gov | sudo tee /sys/class/devfreq/15480000.nvdec/governor`；並思考 `trans_stat` 如何反映你的策略。

### 7c — 假的 cooling device `omc-throttle`

- `thermal_cooling_device_register(type, devdata, &ops)`，`ops` 實作 `get_max_state/get_cur_state/set_cur_state`；`set_cur_state` 以 `FREQ_QOS_MAX` 查表降頻。
- `thermal_zone_get_zone_by_name("CPU-therm")` → `thermal_zone_bind_cooling_device(tz, trip=1, cdev, upper, lower, weight)`；`module_exit` 要先 unbind 再 unregister。
- 用 Lab 5.2 的方法 A（低 trip）觸發，觀察你的 cdev 與 `thermal-cpufreq-0` **並存時的聚合行為**（呼應 Lab 1）。

### 驗收

- 每階段：`insmod`/`rmmod` 十次無警告；`dmesg` 乾淨；有一個 `tests/` 的 user-space 測試程式。
- 能口頭回答：為什麼用 `freq_qos` 而不是直接寫 `scaling_min_freq`？timer callback 內為什麼不能呼叫會睡眠的函式？ioctl 與 `module_exit` 的競爭怎麼避免？

**面試一句話**：「我實作過用 `freq_qos` 的 frame-boost、自訂 devfreq governor 與 thermal cooling device，也量過它們和 user-space 版本的延遲差異。」

---

## Lab 8 — 收斂：use-case 功耗模型 + 自訂 power mode

**這是 JD 第三點「usecase power estimation」的直接練習。**

### 8a — Use-case 功耗模型

1. 設計 12–16 個 omc 組態：`--streams 1..4` × engines（gpu/dla）× `--fps {15,30,60}` × 有無 `--output`（NVENC）。
2. 每個組態跑 3 次，取 `tegrastats_summary.py` 的活動量（GPU util、VIC、NVDEC/NVENC MHz、CPU util、EMC util）與三條 rail 功率（`VDD_GPU_SOC`、`VDD_CPU_CV`、`VIN_SYS_5V0`）。
3. 擬合 `P_rail ≈ P_idle + Σ coef_i × activity_i`（先用線性回歸）。**留 3 個組態當 hold-out**，預測後再量，報 MAPE。
4. 討論：為什麼活動量對功耗不線性？（電壓階梯、漏電隨溫度上升、共用 rail。）Lab 5 的溫度如何進到這個模型？

**交付**：一頁「給 power architect」的估算表——假設、係數、誤差範圍、已知不準確的情況。（誤差目標請你自訂，我不替你訂數字。）

### 8b — 自訂 power mode `MODE_OMC`

設計空間：CPU max（1.1 / 1.4 / 1.7 GHz）、GPU max/min、（nvpmodel 可管的）DLA/PVA clock、NVDEC/NVENC/VIC（Lab 4）、是否讓核 offline。
目標：**e2e p99 ≤ 33 ms（一個 frame）的前提下，總功耗最低**，並檢查 tj 的熱餘裕。

```bash
cp /etc/nvpmodel.conf ./nvpmodel_omc.conf                 # 在專案內編輯：新增 < POWER_MODEL ID=4 NAME=MODE_OMC >
sudo nvpmodel -p --verbose -f ./nvpmodel_omc.conf          # 只解析、不套用，先確認語法
sudo nvpmodel -f ./nvpmodel_omc.conf -m 4                  # 套用（若它說需要重開機，請慎重，不要用 --force）
sudo nvpmodel -m 2                                          # 還原到系統預設設定檔的 MODE_30W
```

交付：功耗 vs p99 的 Pareto 圖、選定的設定與敏感度（哪個 knob 影響最大）、風險說明。

**面試一句話**：「我對自己的 workload 建了功耗模型並用 hold-out 驗證，再用它在設計空間裡找滿足 deadline 的最低功耗 power mode。」

---

## Lab 9（選修）— Suspend-to-RAM 追蹤

**【實測】事實**：`/sys/power/state` 有 `freeze mem`；`mem_sleep` 為 `s2idle [deep]`；`rtc0`、`rtc1`、`rtcwake` 存在；`CONFIG_PM_DEBUG` **沒開**（沒有 `pm_test`）；`power:suspend_resume`、`power:device_pm_callback_{start,end}` 事件存在。
**我沒有測過這塊板子能不能被 RTC 喚醒。**

> **只在本機 console 做，不要用 SSH / VS Code remote**（會斷線）。先存好一切。若無法喚醒，需要手動重開機。

```bash
sudo sh -c 'cd /sys/kernel/tracing; echo > trace
  echo 1 > events/power/suspend_resume/enable
  echo 1 > events/power/device_pm_callback_start/enable
  echo 1 > events/power/device_pm_callback_end/enable; echo 1 > tracing_on'
sudo rtcwake -m mem -s 20
# 醒來後
sudo cat /sys/kernel/tracing/trace > suspend_trace.txt
```

練習：算出每個 driver 的 suspend/resume callback 時間，排名找出最慢的；比較 `s2idle` 與 `deep`（`echo s2idle | sudo tee /sys/power/mem_sleep`）的差別；解釋 freeze processes → device suspend（late / noirq）→ syscore → platform → CPU off 這條路徑。

---

## 附錄 A — JD 對照與自我測驗

| JD 條目 | 你能展示的 |
|---|---|
| Implement power and thermal management software in Linux kernel and user space | Lab 7（kernel）、3.6 / 5.5（user space） |
| Collaborate with power architects on usecase power estimation, power/perf optimization | Lab 8 |
| Understanding of Linux PM features（scheduler, DVFS, runtime PM, suspend to RAM） | Lab 2 / 3 / 6 / 9 |
| System-level debugging | 每個 Lab 的「預測→量測→二分」；連到 `debug-log.md` |
| Embedded + ARM | Tegra 平台、`tegra194` cpufreq、PSCI idle、BPMP |
| Linux / open-source community（加分） | 挑一個 mainline 的 `cpufreq` / `thermal` / `devfreq` commit，在你的板子上重現前後差異；讀 LKML 相關 list |

**自我測驗**（做完之後，不看筆記能答出來才算完成）：

1. userspace 寫 `scaling_max_freq`、thermal cooling、nvpmodel 同時存在時，有效頻率上限怎麼決定？
2. schedutil 如何由 PELT util 得到目標頻率？`rate_limit_us` 的作用？為什麼 33 ms 週期的負載會讓 CPU 在最低與最高頻之間擺盪？
3. 為什麼 load-based governor 對週期性 deadline 負載會失敗？你的替代方案與它的代價？
4. devfreq governor 的 `get_target_freq` 與 `event_handler` 各在什麼時機被呼叫？
5. `step_wise` 和 `power_allocator` 差在哪？`sustainable_power` 怎麼估？
6. 如何從一個 use case 的功率預測穩態 tj？`R_th` 與 `τ` 各代表什麼？
7. cpuidle 的 `exit_latency` 與 `target_residency` 是什麼？menu governor 如何預測閒置長度？
8. runtime PM 的 usage count 與 autosuspend 怎麼運作？rail-gating 的 break-even 怎麼算？
9. Suspend-to-RAM 的流程；你怎麼找最慢的 driver？
10. 你的 `omc_pm` 有哪些 race / lifetime 問題，怎麼處理？
11. 為什麼 sysfs 式的升頻 hint 不適合進產品？
12. 你怎麼確保量測沒有被觀測者效應汙染？

---

## 附錄 B — 路徑速查（【實測】存在）

```
# cpufreq
/sys/devices/system/cpu/cpufreq/policy{0,4,8}/{scaling_governor,scaling_{min,max,cur}_freq,cpuinfo_{min,max,cur}_freq,scaling_available_frequencies,scaling_driver}
/sys/devices/system/cpu/cpu0/cpufreq/stats/{time_in_state,total_trans,trans_table}
/sys/devices/system/cpu/cpufreq/schedutil/{rate_limit_us,capacity_margin,iowait_boost_max}
# devfreq
/sys/class/devfreq/{17000000.ga10b,15340000.vic,15480000.nvdec,154c0000.nvenc}/{governor,available_governors,min_freq,max_freq,cur_freq,trans_stat,polling_interval}
/sys/class/devfreq/{15480000.nvdec,154c0000.nvenc}/userspace/set_freq
/sys/kernel/debug/17000000.ga10b_scaling/{load_target,load_max,bias,damp,smooth,block_window}   # root
/sys/devices/platform/17000000.ga10b/{load,freq_request,railgate_enable,railgate_delay,is_railgated,gpu_powered_on,power/*}
# thermal
/sys/class/thermal/thermal_zone*/{type,temp,policy,available_policies,emul_temp,trip_point_*_{temp,type,hyst},cdev*_trip_point}
/sys/class/thermal/cooling_device*/{type,cur_state,max_state}
# idle / PM QoS
/sys/devices/system/cpu/cpu*/cpuidle/state*/{name,latency,residency,usage,time}
/dev/cpu_dma_latency            # root
# power
/sys/class/hwmon/hwmon3/{in1..3_label,in*_input,curr*_input}   # root; hwmon4 同
tegrastats --interval 200 --logfile F ; tegrastats --stop
# tracing（sudo；/sys/kernel/tracing）
events/power/{cpu_frequency,cpu_frequency_limits,cpu_idle,suspend_resume,device_pm_callback_start,device_pm_callback_end,clock_set_rate,dev_pm_qos_*}
events/devfreq/devfreq_monitor
events/nvhost_podgov/podgov_*
events/thermal/{thermal_temperature,thermal_zone_trip,cdev_update,thermal_power_*}
events/{bwmgr,isomgr,interconnect,gk20a,nvhost}
# 工具
nvpmodel -q | -m N | -p [--verbose] -f CONF
jetson_clocks --store F | --restore F | --show
```

## 附錄 C — 要讀的原始碼

kernel 原始碼**不在這台機器上**（只有 headers）。請從 Jetson Linux 35.4.1 的 Driver Package (BSP) Sources 取得（以 NVIDIA 官方下載頁為準）。
建議閱讀順序：

1. `kernel/sched/cpufreq_schedutil.c`、`kernel/sched/pelt.c`
2. `drivers/cpufreq/cpufreq.c`、`drivers/cpufreq/tegra194-cpufreq.c`、`drivers/cpufreq/cpufreq_cooling.c`（或 `drivers/thermal/cpufreq_cooling.c`）
3. `kernel/power/qos.c`、`include/linux/pm_qos.h`（`freq_qos`）
4. `drivers/devfreq/devfreq.c`、`governor_userspace.c`、`governor_simpleondemand.c`、NVIDIA 的 pod scaling v2 governor
5. `drivers/thermal/thermal_core.c`、`gov_step_wise.c`、`gov_power_allocator.c`
6. `drivers/cpuidle/governors/menu.c`、`drivers/base/power/runtime.c`、`kernel/power/suspend.c`
