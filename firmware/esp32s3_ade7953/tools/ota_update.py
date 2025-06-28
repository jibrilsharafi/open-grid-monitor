#!/usr/bin/env python3
"""
Grid Frequency Monitor - OTA Update Tool (HTTP-based)
Uploads firmware to the ESP32 device via HTTP.

For MQTT-based OTA updates with progress monitoring, use mqtt_ota_update.py instead.
"""

import requests
import sys
import os
import datetime

try:
    import colorama
    colorama.init()
    GREEN = colorama.Fore.GREEN
    RED = colorama.Fore.RED
    YELLOW = colorama.Fore.YELLOW
    RESET = colorama.Style.RESET_ALL
except ImportError:
    # User does not have colorama installed, so just print without colors
    GREEN = ""
    RED = ""
    YELLOW = ""
    RESET = ""


def upload_firmware(ip_address, firmware_path, port=8080):
    """Upload firmware to ESP32 via OTA"""

    # Check if firmware file exists
    if not os.path.exists(firmware_path):
        print(f"{RED}✗ Error: Firmware file '{firmware_path}' not found!{RESET}")
        return False

    url = f"http://{ip_address}:{port}/update"

    try:
        with open(firmware_path, 'rb') as firmware_file:
            print(f"\n🚀 {GREEN}Starting upload...{RESET}")

            # Upload firmware
            response = requests.post(
                url,
                data=firmware_file,
                headers={'Content-Type': 'application/octet-stream'},
                timeout=300  # 5 minute timeout
            )

            if response.status_code == 200:
                print(f"{GREEN}✅ Firmware uploaded successfully!{RESET}")
                print(f"{GREEN}🔄 Device will restart automatically{RESET}")
                return True
            else:
                print(f"{RED}✗ Upload failed with status code: {response.status_code}{RESET}")
                print(f"Response: {response.text}")
                return False

    except requests.exceptions.RequestException as e:
        print(f"{RED}✗ Upload failed: {e}{RESET}")
        return False
    except FileNotFoundError:
        print(f"{RED}✗ Firmware file not found: {firmware_path}{RESET}")
        return False
    except Exception as e:
        print(f"{RED}✗ Unexpected error: {e}{RESET}")
        return False


def find_firmware_file():
    """Try to find the firmware file automatically"""
    possible_paths = [
        "build/open-grid-monitor.bin",
        "../build/open-grid-monitor.bin",
        "../../build/open-grid-monitor.bin",
        "open-grid-monitor.bin"
    ]

    for path in possible_paths:
        if os.path.exists(path):
            return os.path.abspath(path)

    return None


def main():
    print(f"{GREEN}Grid Frequency Monitor - OTA Update Tool{RESET}")
    print("=" * 50)

    if len(sys.argv) < 2:
        print("Usage: python3 ota_update.py <ESP32_IP_ADDRESS> [firmware_file]")
        print(f"\n{YELLOW}Example:{RESET}")
        print("  python3 ota_update.py 192.168.1.100")
        print("  python3 ota_update.py 192.168.1.100 build/open-grid-monitor.bin")
        sys.exit(1)

    ip_address = sys.argv[1]

    # Determine firmware file
    if len(sys.argv) >= 3:
        firmware_path = sys.argv[2]
    else:
        firmware_path = find_firmware_file()
        if firmware_path:
            print(f"🔍 {GREEN}Auto-detected firmware file:{RESET} {firmware_path}")
        else:
            print(f"{RED}✗ Error: Could not find firmware file automatically.{RESET}")
            print("Please specify the firmware file path as the second argument.")
            sys.exit(1)

    # Show firmware information before confirmation
    if not os.path.exists(firmware_path):
        print(f"{RED}✗ Error: Firmware file '{firmware_path}' not found!{RESET}")
        sys.exit(1)

    file_size = os.path.getsize(firmware_path)
    build_time = datetime.datetime.fromtimestamp(os.path.getmtime(firmware_path))
    
    print(f"\n📋 {YELLOW}Firmware Information:{RESET}")
    print(f"📁 {YELLOW}File:{RESET} {firmware_path}")
    print(f"📦 {YELLOW}Size:{RESET} {file_size:,} bytes ({file_size / 1024:.1f} KB)")
    print(f"🗓️ {YELLOW}Build time:{RESET} {build_time.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"🎯 {YELLOW}Target device:{RESET} {ip_address}:8080")

    # Confirm upload
    print(f"\n{GREEN}Ready to upload firmware!{RESET}")
    try:
        input(f"Press {GREEN}Enter{RESET} to continue, or {RED}Ctrl+C{RESET} to cancel...")
    except KeyboardInterrupt:
        print(f"\n{YELLOW}Upload cancelled.{RESET}")
        sys.exit(0)

    if upload_firmware(ip_address, firmware_path):
        sys.exit(0)
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()
