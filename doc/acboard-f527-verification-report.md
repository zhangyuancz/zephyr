# GD32F527 MCU 参数验证报告

**验证日期**: 2026-06-11
**分支**: `acboard-f527`
**数据来源**:
- `GD32F527xx Datasheet_Rev1.8.pdf` (Datasheet)
- `GD32F5xx_用户手册_Rev1.2.pdf` (User Manual)

---

## 验证结果汇总

| # | 验证项 | 结果 | 说明 |
|---|--------|------|------|
| 1 | RCU 寄存器偏移 | ✅ 正确 | 所有偏移与用户手册一致 |
| 2 | RCU 位域定义 (AHBPSC/APB1PSC/APB2PSC) | ✅ 正确 | 位位置和掩码正确 |
| 3 | PLL 配置 (PSC=8, N=400, P=2) | ✅ 正确 | 8MHz×400/8/2=200MHz |
| 4 | AHB/APB prescaler (DIV1/DIV2/DIV4) | ✅ 正确 | AHB=200MHz, APB2=100MHz, APB1=50MHz |
| 5 | PLL VCO 范围 | ✅ 合规 | 输入1MHz(≥1MHz), VCO 400MHz(≤500MHz) |
| 6 | PMU high-drive 配置 | ✅ 正确 | 200MHz需要HDEN+HDS, HAL代码包含 |
| 7 | HAL默认宏覆盖 | ✅ 正确 | CMakeLists.txt覆盖为8MHz版本 |
| 8 | 外设基地址 | ✅ 正确 | 所有地址与datasheet memory map一致 |
| 9 | IRQ 中断号 | ✅ 正确 | NUM_IRQS=104, 所有外设IRQ号匹配 |
| 10 | 时钟域分配 | ✅ 正确 | 所有外设时钟门控bit与用户手册一致 |
| 11 | 复位寄存器bit分配 | ✅ 正确 | 与时钟使能寄存器镜像一致 |
| 12 | Pinctrl AF 引脚分配 | ✅ 正确 | USART0/AF7, UART3/AF8, SPI2/AF6 均正确 |
| 13 | SRAM 大小 | ✅ 正确 | VST7=512KB, 与datasheet一致 |
| 14 | Flash 基地址/大小 | ✅ 正确 | Bank0=2MB @ 0x08000000 |
| 15 | Flash erase layout | ⚠️ 待支持 | F527 使用非等长扇区，现有 FMC backend 不适用 |
| 16 | Flash erase time | ⚠️ 待支持 | 应随未来 F527 扇区布局 backend 一并描述 |
| 17 | write-block-size=2 | ✅ 正确 | 半字(16-bit)编程 |
| 18 | NVIC priority bits=4 | ✅ 正确 | Cortex-M33标准 |
| 19 | CPU frequency=200MHz | ✅ 正确 | |
| 20 | HXTAL=8MHz | ✅ 正确 | ACBoard硬件设计值 |

---

## 详细验证

### 1. RCU 寄存器偏移 (已验证 ✅)

| 寄存器 | 偏移 | 用户手册 | 状态 |
|--------|------|----------|------|
| RCU_CTL | 0x00 | 0x00 | ✅ |
| RCU_PLL | 0x04 | 0x04 | ✅ |
| RCU_CFG0 | 0x08 | 0x08 | ✅ |
| RCU_INT | 0x0C | 0x0C | ✅ |
| RCU_AHB1RST | 0x10 | 0x10 | ✅ |
| RCU_AHB2RST | 0x14 | 0x14 | ✅ |
| RCU_AHB3RST | 0x18 | 0x18 | ✅ |
| RCU_APB1RST | 0x20 | 0x20 | ✅ |
| RCU_APB2RST | 0x24 | 0x24 | ✅ |
| RCU_AHB1EN | 0x30 | 0x30 | ✅ |
| RCU_AHB2EN | 0x34 | 0x34 | ✅ |
| RCU_AHB3EN | 0x38 | 0x38 | ✅ |
| RCU_APB1EN | 0x40 | 0x40 | ✅ |
| RCU_APB2EN | 0x44 | 0x44 | ✅ |
| RCU_RSTSCK | 0x74 | 0x74 | ✅ |
| RCU_ADDAPB1RST | 0xE0 | 0xE0 | ✅ |
| RCU_ADDAPB1EN | 0xE4 | 0xE4 | ✅ |

验证方法：对比 `gd32_regs.h`, `gd32f5xx-clocks.h`, `gd32f5xx.h` 与用户手册第5.3节。

### 2. PLL 时钟树 (已验证 ✅)

```
HXTAL (8MHz)
  │ PLLPSC=8 → VCO输入=1MHz (范围1~2MHz ✅)
  │ PLLN=400  → VCO=400MHz (范围100~500MHz ✅)
  │ PLLP=2    → SYSCLK=200MHz
  │ PLLQ=9    → USB/TRNG/SDIO时钟=44.4MHz
  │
  └─→ AHB=DIV1  → 200MHz (最大200MHz ✅)
       ├─→ APB2=DIV2 → 100MHz (最大100MHz ✅)
       └─→ APB1=DIV4 → 50MHz  (最大50MHz ✅)
```

200MHz 需要 PMU high-drive 模式，已在 `system_clock_200m_8m_hxtal()` 中配置。

### 3. Pin 复用 (已验证 ✅)

| 信号 | GPIO | AF | Datasheet | 状态 |
|------|------|-----|-----------|------|
| USART0_TX | PA9 | AF7 | PA9 AF7=USART0_TX | ✅ |
| USART0_RX | PA10 | AF7 | PA10 AF7=USART0_RX | ✅ |
| UART3_TX | PC10 | AF8 | PC10 AF8=UART3_TX | ✅ |
| UART3_RX | PC11 | AF8 | PC11 AF8=UART3_RX | ✅ |
| SPI2_SCK | PB3 | AF6 | PB3 AF6=SPI2_SCK/I2S2_CK | ✅ |
| SPI2_MISO | PB4 | AF6 | PB4 AF6=SPI2_MISO | ✅ |
| SPI2_MOSI | PB5 | AF6 | PB5 AF6=SPI2_MOSI/I2S2_SD | ✅ |

### 4. 已修复的问题

**问题 1: 片内 Flash backend 类型不匹配**
- F527 使用扇区式 FMC，不能声明为页式 `gd32-nv-flash-v1`。
- 当前仅保留 `soc-nv-flash` 代码存储描述，不注册擦写驱动。
- 后续需要新增 F527 扇区布局 backend 后才能安全启用片内 Flash 擦写。

**问题 2: Pinctrl 头文件封装不匹配**
- 文件: `boards/gd/acboard_f527/acboard-pinctrl.dtsi`
- 原值: `#include <dt-bindings/pinctrl/gd32f527i(g-i-k)xx-pinctrl.h>` (I系列/176脚)
- 修正: `#include <dt-bindings/pinctrl/gd32f527v(e-g-i-k)xx-pinctrl.h>` (V系列/LQFP100)
- 依据: 实际芯片为 GD32F527VST7 (LQFP100)，V系列头文件是正确匹配

### 5. 已知限制 (不需要立即修复)

5.1 **片内 Flash 只读描述** — FMC 驱动尚未适配 F527 扇区布局。当前使用 SPI NOR Flash 保存应用数据。

5.2 **SRAM 声明** — DTSI 声明 512KB 对应 SRAM0+SRAM1+SRAM2 连续区域。ADDSRAM (512KB @ 0x20080000) 和 TCMSRAM (64KB @ 0x10000000) 未声明，如需使用需要单独添加 memory node。

---

## 结论

**GD32F527 的 Zephyr 基础参数配置整体正确。** RCU 寄存器、时钟树、外设地址、中断号和引脚复用等关键参数已核对；片内 Flash 擦写仍需实现专用扇区布局 backend。

**可以进行下一步的项目化工作。**
