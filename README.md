# Hệ Thống Phát Hiện Té Ngã trên ESP32 với TensorFlow Lite và BLE / Telegram

![Fall Detection Poster](images/fall_posterv2.png)

Dự án này triển khai một hệ thống phát hiện té ngã đeo tay sử dụng vi điều khiển **ESP32** kết hợp với cảm biến **IMU MPU6050**. Hệ thống hoạt động theo thời gian thực nhờ sự kết hợp giữa:

1. **Thuật toán lọc ngưỡng cơ bản (DoG - Difference of Gaussians):** Giúp phát hiện nhanh các cú sốc/gia tốc đột ngột để lọc nhiễu ban đầu.
2. **Mô hình học máy TensorFlow Lite Micro:** Chạy trực tiếp trên ESP32 để phân loại cú ngã với độ chính xác cao.
3. **Hệ thống cảnh báo đa kênh:** Đếm ngược còi buzzer tại chỗ (cho phép người dùng nhấn nút vật lý để hủy báo động giả), gửi tin nhắn cảnh báo qua **Telegram API**, và đồng bộ trạng thái lên **Dashboard ERa** qua Wi-Fi.

---

## 1. Yêu Cầu Phần Cứng & Sơ Đồ Đấu Nối

### Linh kiện cần thiết:

- **ESP32 DevKit** (Board phát triển ESP32)
- **Cảm biến IMU MPU6050** (Đo gia tốc và góc nghiêng)
- **Còi báo động (Active Buzzer)**
- **Nút nhấn vật lý (Push Button)** để hủy báo động giả
- Dây nối, Breadboard, cáp nạp Micro-USB

### Sơ đồ nối dây (Pinout):

| Linh kiện       | Chân ESP32 | Mô tả                                                   |
| :-------------- | :--------- | :------------------------------------------------------ |
| **MPU6050 VCC** | 3V3 / 5V   | Nguồn cấp cho cảm biến                                  |
| **MPU6050 GND** | GND        | Mass chung                                              |
| **MPU6050 SDA** | GPIO 21    | Giao tiếp I2C dữ liệu                                   |
| **MPU6050 SCL** | GPIO 22    | Giao tiếp I2C xung nhịp                                 |
| **Buzzer (+)**  | GPIO 19    | Điều khiển còi báo động                                 |
| **Button**      | GPIO 18    | Nút nhấn hủy báo động (Active LOW, sử dụng Pull-up nội) |

---

## 2. Cấu Hình Phần Mềm (Firmware ESP32)

Dự án sử dụng **VS Code** với extension **PlatformIO** để quản lý mã nguồn và nạp code cho ESP32.

### Bước 1: Cài đặt thư viện bổ trợ

1. Mở terminal tại thư mục gốc của dự án.
2. Clone thư viện `FastIMU` vào thư mục `lib/` bằng lệnh sau (đã đổi sang giao thức HTTPS để người dùng không cần cấu hình SSH keys):
   ```bash
   cd lib
   git clone https://github.com/LiquidCGS/FastIMU.git
   ```
3. Các thư viện khác như `TensorFlowLite_ESP32` và `ERa` sẽ được tự động tải về khi build thông qua cấu hình trong file `platformio.ini`.

### Bước 2: Cấu hình thông tin Wi-Fi và Telegram

Mở file `src/env.cpp` và điền các thông số mạng cũng như API của bạn:

```cpp
// Thông tin Wi-Fi nhà bạn
char ssid[] = "TÊN_WIFI_CỦA_BẠN";
char password[] = "MẬT_KHẨU_WIFI";

// Thông tin Telegram Bot API
char telegram_bot_token[] = ""; // Token Bot của bạn
char telegram_chat_id[] = ""; // Chat ID nhận tin nhắn cảnh báo
```

_(Nếu muốn tạo Bot Telegram riêng, bạn có thể chat với `@BotFather` để lấy Token và lấy Chat ID thông qua `@userinfobot`)._

### Bước 3: Cấu hình Dashboard ERa (Tùy chọn)

Trong file `src/main.cpp`, bạn có thể điền mã Auth Token của ERa tại dòng:

```cpp
#define ERA_AUTH_TOKEN "" // Thay bằng token từ ứng dụng ERa của bạn
```

---

## 3. Biên Dịch & Nạp Chương Trình

1. Kết nối ESP32 với máy tính qua cáp USB.
2. Mở dự án bằng **VS Code + PlatformIO**.
3. Chọn biểu tượng PlatformIO ở thanh công cụ bên trái:
   - Nhấn **Build** để biên dịch dự án.
   - Nhấn **Upload** để nạp firmware vào ESP32.
4. Mở **Serial Monitor** (baudrate: `115200`) để theo dõi các log hoạt động của thiết bị.

---

## 4. Quy Trình Hoạt Động Của Thiết Bị

Thiết bị chạy một Máy trạng thái tuần tự (**FSM**) gồm các bước:

1. **STATE_MONITORING (Giám sát):** ESP32 đọc dữ liệu IMU liên tục.
2. **STATE_COUNTDOWN (Đếm ngược):** Khi AI xác nhận có cú ngã xảy ra, còi Buzzer kêu bíp nhanh dần trong **15 giây**.
   - Nếu người dùng nhấn nút vật lý (nối chân GPIO 18), hệ thống sẽ chuyển sang `STATE_CANCELLED` và **không** gửi cảnh báo.
3. **STATE_ALERTING (Báo động):** Nếu không bị hủy sau 15 giây, hệ thống kích hoạt báo động:
   - Gửi tin nhắn cảnh báo tức thời tới **Telegram** qua kết nối Wi-Fi.
   - Đồng bộ trạng thái té ngã lên **ERa Dashboard**.
   - Còi hú liên tục trong **40 giây** trước khi tự động khôi phục lại trạng thái giám sát ban đầu.

---

## 5. Huấn Luyện Lại Mô Hình Học Máy (ML Pipeline)

Nếu bạn muốn thu thập thêm dữ liệu mới hoặc huấn luyện lại mô hình AI:

### Yêu cầu môi trường Python:

Sử dụng thư viện `pipenv` để cài đặt môi trường ảo Python cô lập:

```bash
pipenv install
pipenv shell
```

### Các công cụ Python hỗ trợ trong thư mục `python_src/`:

1. **Thu thập dữ liệu thô:**
   Chạy server nhận dữ liệu trực tiếp từ ESP32 qua Wi-Fi và vẽ đồ thị thời gian thực:

   ```bash
   python python_src/data_collection_server.py
   ```

   _Lưu ý: Để ESP32 truyền dữ liệu thô, hãy sửa cấu hình `#define DATA_COLLECTION_MODE 1` trong file `src/main.cpp` trước khi nạp._

2. **Gán nhãn dữ liệu:**
   Công cụ hỗ trợ trực quan hóa dữ liệu cảm biến kết hợp đồng bộ video để gán nhãn sự kiện té ngã:

   ```bash
   python python_src/data_annotation_tool.py
   ```

   <img src="images/data_collection_server.png" width="600" alt="Data Collection Server screenshot"/>

3. **Huấn luyện mô hình:**
   Huấn luyện và xuất file mô hình dạng Keras và TFLite:

   ```bash
   python python_src/main.py
   ```

   Mô hình được huấn luyện sẽ xuất ra thư mục `python_src/models/model.tflite`.

4. **Nhúng mô hình vào ESP32:**
   Chuyển đổi mô hình TFLite thành mã nguồn mảng C++ bằng lệnh `xxd`:
   ```bash
   xxd -i python_src/models/model.tflite > src/model.cpp
   ```
   Sau đó tiến hành build và nạp lại firmware như mục 3.
