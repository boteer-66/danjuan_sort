import serial
import time

# ================= 配置区 =================
SERIAL_PORT = 'COM3'  # 换成你设备管理器里的端口号（如 /dev/ttyUSB0）
BAUD_RATE = 115200
IMAGE_PATH = 'test.jpg' # 准备一张几百KB的小图放在同目录下
# ==========================================

def send_image():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2)
        time.sleep(2) # 等待串口稳定
        
        with open(IMAGE_PATH, 'rb') as f:
            img_data = f.read()
            img_size = len(img_data)
            
        print(f"--- 准备发送图片: {IMAGE_PATH} ({img_size} bytes) ---")
        
        # 1. 发送触发指令
        ser.write(b"START_UPLOAD\n")
        time.sleep(0.5)
        
        # 2. 灌入二进制数据
        ser.write(img_data)
        
        print("数据发送完毕，请观察 ESP32 串口监视器...")
        
        # 3. 打印 ESP32 的回显
        while True:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                print(f"[ESP32]: {line}")
                if "上传成功" in line or "上传失败" in line:
                    break
                    
        ser.close()
    except Exception as e:
        print(f"错误: {e}")

if __name__ == "__main__":
    send_image()