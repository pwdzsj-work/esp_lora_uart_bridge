# ESP32 UART1/UART2 透明转发工程

本工程依据 `SCH_Schematic1_2026-09-01.pdf` 创建，适用于图中的
ESP32-WROOM-32E：

| 接口 | TX | RX | 默认参数 | 用途 |
|---|---:|---:|---|---|
| UART0 | 原理图 RXD0/TXD0 | 原理图 RXD0/TXD0 | ESP-IDF 控制台默认值 | CH340B 下载和日志 |
| UART1 | GPIO16 | GPIO17 | 9600 8N1 | LoRa 串口 |
| UART2 | GPIO32 | GPIO33 | 9600 8N1 | 转发目标串口 |

固件默认进行双向透明转发：

```text
UART1 RX (GPIO17)  -> UART2 TX (GPIO32)
UART2 RX (GPIO33)  -> UART1 TX (GPIO16)
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

## AP 管理页面和 OTA

V1.2.0 使用双 OTA 分区。首次从旧版本升级时，必须通过串口执行一次完整的
`idf.py flash`，以同时写入新的分区表；之后才可以使用网页 OTA。

设备正常启动后长按 U4 约 3 秒会开启维护热点；松开后再次长按 3 秒会关闭热点：

- 热点名：`UART-Bridge-XXXX`（后四位由设备 MAC 地址生成）
- 密码：`12345678`
- 管理页面：`http://192.168.4.1`

页面显示软件、硬件版本，并可上传 `build/esp32_uart_bridge.bin` 进行 OTA。
只能上传应用 `.bin`，不要上传 merged-flash、bootloader 或 partition-table 文件。
升级期间不要断电，校验成功后设备会自动重启。

页面还显示 SKU `LR-Main-V1.0`，并可保存路由器的 Wi-Fi 名称和密码。保存后设备
以 STA 模式连接路由器；后续重启会自动重连已保存的 Wi-Fi，但不会自动开启 AP。
维护 AP 始终只能通过长按 U4 开启或关闭。

连接路由器后，`GET /api/status` 会常驻返回真实 Wi-Fi MAC、SKU 以及当前软件、
硬件版本。上位机详情页的 OTA 同样需要先长按 U4 约 3 秒进行物理授权，随后使用
`POST /api/ota` 上传应用固件；默认 OTA 密码为 `lora-ota-123`。设备重启后维护
页面和 OTA 写入接口都会关闭，只读状态接口继续运行。

U4 使用仅输入的 GPIO34，按键电路必须保留外部 10 kΩ 上拉电阻。

## 修改配置

串口引脚、波特率、缓冲区大小都集中在 `main/board_config.h`。如果只需要
UART1 转发到 UART2，不需要反向通道，把
`BOARD_UART_BRIDGE_BIDIRECTIONAL` 改为 `0` 后重新编译。

测试时请交叉连接外部串口设备的 TX/RX，并确保设备与本板共地。两个外部串口
都必须使用 3.3 V TTL 电平；不要把 RS-232 的正负电压直接接到 ESP32 引脚。
