# ESP32 UART1/UART2 透明转发工程

本工程依据 `SCH_Schematic1_2026-08-22.pdf` 创建，适用于图中的
ESP32-WROOM-32E：

| 接口 | TX | RX | 默认参数 | 用途 |
|---|---:|---:|---|---|
| UART0 | 原理图 RXD0/TXD0 | 原理图 RXD0/TXD0 | ESP-IDF 控制台默认值 | CH340B 下载和日志 |
| UART1 | GPIO17 | GPIO16 | 9600 8N1 | 被转发串口 |
| UART2 | GPIO32 | GPIO33 | 9600 8N1 | 转发目标串口 |

固件默认进行双向透明转发：

```text
UART1 RX (GPIO16)  -> UART2 TX (GPIO32)
UART2 RX (GPIO33)  -> UART1 TX (GPIO17)
```

它不会解析、修改或添加协议数据，二进制数据也可以直接透传。UART0 不参与
转发，因此 USB-C/CH340B 仍可用于烧录和查看启动日志。

## 编译和烧录

在已安装 ESP-IDF 5.x 的终端中执行：

```powershell
cd uart_bridge
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 替换为 CH340B 对应的端口。

## 修改配置

串口引脚、波特率、缓冲区大小都集中在 `main/board_config.h`。如果只需要
UART1 转发到 UART2，不需要反向通道，把
`BOARD_UART_BRIDGE_BIDIRECTIONAL` 改为 `0` 后重新编译。

测试时请交叉连接外部串口设备的 TX/RX，并确保设备与本板共地。两个外部串口
都必须使用 3.3 V TTL 电平；不要把 RS-232 的正负电压直接接到 ESP32 引脚。
