from typing import Dict, Optional
import serial
from serial.tools.list_ports import comports
import time
import esptool

class Config:
    """配置参数类"""
    TARGET_VERSION = "openthread-esp32/e9bdd39599-8c30b93fe; esp32h2;  2025-04-15 03:45:41 UTC"
    DATASET_VALUE = "0e0800000000000100004a0300000b35060004001fffe00c0402a0f7f800030000190208e15edd3bb66e908e0708fdac4364043314f20510e15edd3bb66e908eac4364043314f230030d5468726561642d3078653135650102e15e0410a6f5c3f2a687fa35c59ca098001ddc8c"
    FLASH_PARAMS = {
        "baud_rate": 460800,
        "files": {
            "0x0": "./bootloader/bootloader.bin",
            "0x8000": "./partition_table/partition-table.bin",
            "0x10000": "./esp_ot_cli.bin"
        },
        "chip": "esp32h2",
        "flash_mode": "dio",
        "flash_freq": "48m",
        "flash_size": "2MB"
    }

class SerialManager:
    """串口管理类（保持不变）"""
    @staticmethod
    def find_ch343_port() -> Optional[str]:
        for port in comports():
            if "CH343" in port.description:
                print(f"找到 CH343 设备: {port.description} -> {port.device}")
                return port.device
        print("未找到包含 CH343 的串口设备")
        return None

    @staticmethod
    def open_port(port: str, baudrate: int = 115200) -> Optional[serial.Serial]:
        try:
            ser = serial.Serial(port, baudrate, timeout=1)
            print(f"成功打开串口: {port}")
            return ser
        except serial.SerialException as e:
            print(f"无法打开串口 {port}: {e}")
            return None

class ESP32Flasher:
    """固件烧录管理类（保持不变）"""
    @staticmethod
    def _build_flash_args(port: str, params: Dict) -> list:
        return [
            '--port', port,
            '--baud', str(params["baud_rate"]),
            '--before', 'default_reset',
            '--after', 'hard_reset',
            '--chip', params["chip"],
            'write_flash',
            '--flash_mode', params["flash_mode"],
            '--flash_freq', params["flash_freq"],
            '--flash_size', params["flash_size"]
        ] + [item for pair in params["files"].items() for item in pair]

    @classmethod
    def flash_firmware(cls, port: str, params: Dict) -> bool:
        try:
            print("正在擦除 Flash...")
            esptool.main(['--port', port, '--baud', str(params["baud_rate"]), 
                        '--chip', params["chip"], 'erase_flash'])

            print("正在烧录固件...")
            esptool.main(cls._build_flash_args(port, params))
            
            print("固件烧录成功！")
            return True
        except Exception as e:
            print(f"烧录失败: {str(e)}")
            return False

class CommandHandler:
    """命令处理类"""
    @staticmethod
    def send_command(ser: serial.Serial, command: str, 
                    expected: Optional[str] = None, 
                    timeout: float = 5.0,
                    post_delay: float = 0) -> str:
        start_time = time.time()
        ser.write(f"{command}\r\n".encode())
        print(f"[发送] {command}")
        
        response = []
        while time.time() - start_time < timeout:
            line = ser.readline()  # 读取原始字节数据
            try:
                decoded_line = line.decode('utf-8', errors='replace').strip()  # 使用错误容忍解码
            except Exception as e:
                print(f"解码错误: {e}, 原始数据: {line}")
                continue

            if decoded_line:
                print(f"[接收] {decoded_line}")
                response.append(decoded_line)
                if expected and expected in decoded_line:
                    break
        
        time.sleep(post_delay)  # 延时控制
        return "\n".join(response)

class MainController:
    """主控制逻辑类（恢复关键延时）"""
    @staticmethod
    def validate_version(ser: serial.Serial) -> bool:
        # 保持原有2秒设备初始化时间
        time.sleep(2)
        response = CommandHandler.send_command(ser, "version", "Done", post_delay=1)
        return Config.TARGET_VERSION in response

    @staticmethod
    def configure_dataset(ser: serial.Serial) -> bool:
        commands = [
            ("factoryreset", "OT_ROLE: Role: Detached", 2),  # 新增延时参数
            (f"dataset set active {Config.DATASET_VALUE}", "Done", 1),
            ("sdmcapp savedataset", "successfully", 1),
            ("factoryreset", "OT_ROLE: Role: Detached", 2),
            ("dataset active -x", "Done", 1)
        ]
        
        for cmd, expect, delay in commands:
            response = CommandHandler.send_command(
                ser, 
                cmd, 
                expect,
                post_delay=delay  # 应用延时参数
            )
            if expect not in response:
                return False
        return Config.DATASET_VALUE in response

    @classmethod
    def main_flow(cls):
        """恢复所有关键延时"""
        while True:
            if port := SerialManager.find_ch343_port():
                # 打开串口后保持2秒初始化时间
                with SerialManager.open_port(port) as ser:
                    if not ser:
                        continue
                    time.sleep(2)  # 恢复串口初始化延时

                    if not cls.validate_version(ser):
                        ser.close()
                        if ESP32Flasher.flash_firmware(port, Config.FLASH_PARAMS):
                            # 烧录后重新打开时保持2秒延时
                            time.sleep(2)
                            ser = SerialManager.open_port(port)
                            if ser: 
                                time.sleep(2)  # 恢复设备重启等待时间

                    if ser and cls.configure_dataset(ser):
                        input("配置成功，按 Enter 继续...")
                    else:
                        print("! 配置流程异常，重新开始循环")

if __name__ == "__main__":
    MainController.main_flow()