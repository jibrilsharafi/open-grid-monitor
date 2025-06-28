#!/usr/bin/env python3
"""
Grid Frequency Monitor - MQTT OTA Update Tool
Hosts firmware file over HTTP and triggers OTA via MQTT.
"""

import sys
import os
import json
import threading
import time
import datetime
from http.server import HTTPServer, SimpleHTTPRequestHandler
from socketserver import TCPServer
import argparse

# Fetch MQTT configuration from environment variables or set defaults
BROKER = "localhost"
PORT = 1883
USERNAME = None
PASSWORD = None
try:
    from dotenv import load_dotenv
    if load_dotenv():
        BROKER = os.environ["MQTT_BROKER"]
        PORT = int(os.environ["MQTT_PORT"])
        USERNAME = os.environ["MQTT_USERNAME"]
        PASSWORD = os.environ["MQTT_PASSWORD"]
    else:
        print("Warning: .env file not found. Using default MQTT settings.")

except ImportError:
    print("Warning: dotenv library not found. Using default MQTT settings.")

BASE_TOPIC = "open_grid_monitor"
DEVICE_DISCOVERY_TIMEOUT = 3  # seconds to wait for device discovery

try:
    import paho.mqtt.client as mqtt
    MQTT_AVAILABLE = True
except ImportError:
    print("Error: paho-mqtt library not found. Install with: pip install paho-mqtt")
    MQTT_AVAILABLE = False
    sys.exit(1)

try:
    import colorama
    colorama.init()
    GREEN = colorama.Fore.GREEN
    RED = colorama.Fore.RED
    YELLOW = colorama.Fore.YELLOW
    BLUE = colorama.Fore.BLUE
    CYAN = colorama.Fore.CYAN
    RESET = colorama.Style.RESET_ALL
except ImportError:
    print("Error: colorama library not found. Install with: pip install colorama")
    GREEN = RED = YELLOW = BLUE = CYAN = RESET = ""


class FirmwareHTTPRequestHandler(SimpleHTTPRequestHandler):
    """Custom HTTP handler that serves only the firmware file"""
    
    def __init__(self, *args, firmware_path=None, **kwargs):
        self.firmware_path = firmware_path
        super().__init__(*args, **kwargs)
    
    def do_GET(self):
        if self.path == '/firmware.bin' and self.firmware_path:
            try:
                with open(self.firmware_path, 'rb') as f:
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/octet-stream')
                    self.send_header('Content-Length', str(os.path.getsize(self.firmware_path)))
                    self.end_headers()
                    self.wfile.write(f.read())
                print(f"{GREEN}✅ Served firmware file to {self.client_address[0]}{RESET}")
            except Exception as e:
                print(f"{RED}❌ Error serving firmware: {e}{RESET}")
                self.send_response(404)
                self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()
    
    def log_message(self, format, *args):
        # Suppress default HTTP server logs
        pass


class MQTTOTAUpdater:
    def __init__(self, mqtt_broker, mqtt_port=1883, mqtt_username=None, mqtt_password=None):
        self.mqtt_broker = mqtt_broker
        self.mqtt_port = mqtt_port
        self.mqtt_username = mqtt_username
        self.mqtt_password = mqtt_password
        self.client = None
        self.connected = False
        self.status_updates = []
        self.discovered_devices = set()
        self.selected_device = None
        self.ota_start_time = None
        self.ota_progress_data = []
        self.firmware_size = 0
        
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            print(f"{GREEN}✅ Connected to MQTT broker{RESET}")
            # Subscribe to device discovery topics
            client.subscribe(f"{BASE_TOPIC}/+/measurement")
            # Subscribe to status topics (will be updated with device ID later)
            if self.selected_device:
                client.subscribe(f"{BASE_TOPIC}/{self.selected_device}/status")
                client.subscribe(f"{BASE_TOPIC}/{self.selected_device}/error")
                client.subscribe(f"{BASE_TOPIC}/{self.selected_device}/logs")
        else:
            print(f"{RED}❌ Failed to connect to MQTT broker: {rc}{RESET}")
            
    def on_disconnect(self, client, userdata, rc):
        self.connected = False
        print(f"{YELLOW}⚠️ Disconnected from MQTT broker{RESET}")
        
    def on_message(self, client, userdata, msg):
        topic = msg.topic
        payload = msg.payload.decode()
        timestamp = datetime.datetime.now().strftime("%H:%M:%S")
        
        # Check for device discovery messages
        if topic.startswith(f"{BASE_TOPIC}/") and topic.endswith("/measurement"):
            device_id = topic.split("/")[1]
            if device_id not in self.discovered_devices:
                self.discovered_devices.add(device_id)
                print(f"{CYAN}🔍 Discovered device: {device_id}{RESET}")
        
        # Handle status messages for selected device
        elif self.selected_device and topic == f"{BASE_TOPIC}/{self.selected_device}/status":
            print(f"{CYAN}[{timestamp}] 📋 Status: {payload}{RESET}")
            
            # Track OTA progress for speed calculation
            if "OTA Progress:" in payload and self.ota_start_time:
                try:
                    # Extract percentage from payload like "OTA Progress: 45% (bytes/total bytes)"
                    if "%" in payload:
                        percent_part = payload.split("OTA Progress:")[1].strip()
                        percent_str = percent_part.split("%")[0].strip()
                        percentage = float(percent_str)
                        elapsed_time = time.time() - self.ota_start_time
                        
                        # Calculate downloaded bytes and speed
                        downloaded_bytes = (percentage / 100.0) * self.firmware_size
                        speed_bps = downloaded_bytes / elapsed_time if elapsed_time > 0 else 0
                        speed_kbps = speed_bps / 1024
                        
                        # Store progress data
                        self.ota_progress_data.append((time.time(), percentage, downloaded_bytes, speed_bps))
                        
                        # Display progress with speed
                        if speed_kbps > 1024:
                            speed_str = f"{speed_kbps/1024:.1f} MB/s"
                        else:
                            speed_str = f"{speed_kbps:.1f} KB/s"
                        
                        print(f"{GREEN}[{timestamp}] 📈 Progress: {percentage:.1f}% - Speed: {speed_str}{RESET}")
                    
                except Exception as e:
                    print(f"{RED}[{timestamp}] ❌ Error parsing OTA progress: {e}{RESET}")
                    
        elif self.selected_device and topic == f"{BASE_TOPIC}/{self.selected_device}/error":
            print(f"{RED}[{timestamp}] ❌ Error: {payload}{RESET}")
        elif self.selected_device and topic == f"{BASE_TOPIC}/{self.selected_device}/status":
            if "OTA" in payload:
                if "Starting OTA" in payload or "OTA started" in payload:
                    self.ota_start_time = time.time()
                    self.ota_progress_data.clear()
                    print(f"{BLUE}[{timestamp}] 📝 Log: {payload.strip()}{RESET}")
                elif "OTA completed" in payload or "OTA finished" in payload or "OTA update completed successfully" in payload:
                    self._show_ota_completion_stats()
                    print(f"{BLUE}[{timestamp}] 📝 Log: {payload.strip()}{RESET}")
                else:
                    print(f"{BLUE}[{timestamp}] 📝 Log: {payload.strip()}{RESET}")
            elif "restart" in payload.lower():
                print(f"{BLUE}[{timestamp}] 📝 Log: {payload.strip()}{RESET}")
        
        self.status_updates.append((timestamp, topic, payload))
        
    def connect(self):
        self.client = mqtt.Client()
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message
        
        if self.mqtt_username:
            self.client.username_pw_set(self.mqtt_username, self.mqtt_password)
            
        try:
            self.client.connect(self.mqtt_broker, self.mqtt_port, 60)
            self.client.loop_start()
            
            # Wait for connection
            timeout = time.time() + 10
            while not self.connected and time.time() < timeout:
                time.sleep(0.1)
                
            return self.connected
        except Exception as e:
            print(f"{RED}❌ MQTT connection error: {e}{RESET}")
            return False
            
    def discover_devices(self, timeout=DEVICE_DISCOVERY_TIMEOUT, auto_mode=False):
        """Discover devices by listening to measurement topics"""
        if not self.connected:
            print(f"{RED}❌ Not connected to MQTT broker{RESET}")
            return []
            
        if auto_mode:
            print(f"{CYAN}🔍 Auto-discovering devices (max {timeout} seconds, stopping when first device found)...{RESET}")
        else:
            print(f"{CYAN}🔍 Discovering devices for {timeout} seconds...{RESET}")
        print(f"{YELLOW}Listening for messages on {BASE_TOPIC}/+/measurement{RESET}")
        
        self.discovered_devices.clear()
        start_time = time.time()
        
        while time.time() - start_time < timeout:
            time.sleep(0.1)
            
            # In auto mode, stop as soon as we find at least one device
            if auto_mode and len(self.discovered_devices) > 0:
                elapsed = time.time() - start_time
                print(f"{GREEN}✅ Found device in {elapsed:.1f} seconds, stopping discovery early{RESET}")
                break
            
        device_list = sorted(list(self.discovered_devices))
        if device_list:
            print(f"{GREEN}✅ Found {len(device_list)} device(s):{RESET}")
            for i, device in enumerate(device_list, 1):
                print(f"  {i}. {device}")
        else:
            print(f"{YELLOW}⚠️ No devices found during discovery period{RESET}")
            
        return device_list
    
    def select_device_auto(self, device_list):
        """Automatically select the first device from the discovered list"""
        if not device_list:
            return None
            
        selected_device = device_list[0]
        self.selected_device = selected_device
        print(f"{GREEN}🤖 Auto-selected first device: {selected_device}{RESET}")
        
        # Subscribe to device-specific topics
        if self.client:
            self.client.subscribe(f"{BASE_TOPIC}/{selected_device}/status")
            self.client.subscribe(f"{BASE_TOPIC}/{selected_device}/error")
            self.client.subscribe(f"{BASE_TOPIC}/{selected_device}/logs")
            print(f"{CYAN}📡 Subscribed to device topics{RESET}")
        
        return selected_device
    
    def select_device_interactive(self, device_list):
        """Let user select a device from the discovered list"""
        if not device_list:
            return None
            
        print(f"\n{YELLOW}Select device for OTA update:{RESET}")
        for i, device in enumerate(device_list, 1):
            print(f"  {GREEN}{i}{RESET}. {device}")
        
        while True:
            try:
                choice = input(f"\nEnter device number (1-{len(device_list)}) or 'q' to quit: ").strip()
                if choice.lower() == 'q':
                    return None
                    
                device_index = int(choice) - 1
                if 0 <= device_index < len(device_list):
                    selected_device = device_list[device_index]
                    self.selected_device = selected_device
                    print(f"{GREEN}✅ Selected device: {selected_device}{RESET}")
                    
                    # Subscribe to device-specific topics
                    if self.client:
                        self.client.subscribe(f"{BASE_TOPIC}/{selected_device}/status")
                        self.client.subscribe(f"{BASE_TOPIC}/{selected_device}/error")
                        self.client.subscribe(f"{BASE_TOPIC}/{selected_device}/logs")
                        print(f"{CYAN}📡 Subscribed to device topics{RESET}")
                    
                    return selected_device
                else:
                    print(f"{RED}❌ Invalid selection. Please enter a number between 1 and {len(device_list)}{RESET}")
            except ValueError:
                print(f"{RED}❌ Invalid input. Please enter a number or 'q' to quit{RESET}")
            except KeyboardInterrupt:
                print(f"\n{YELLOW}Operation cancelled.{RESET}")
                return None
    
    def send_ota_command(self, firmware_url, device_id=None, firmware_size=0):
        if not self.connected:
            print(f"{RED}❌ Not connected to MQTT broker{RESET}")
            return False
            
        target_device = device_id or self.selected_device
        if not target_device:
            print(f"{RED}❌ No device selected{RESET}")
            return False
        
        # Store firmware size for speed calculations
        self.firmware_size = firmware_size
            
        command = {"ota": firmware_url}
        command_json = json.dumps(command)
        command_topic = f"{BASE_TOPIC}/{target_device}/command"
        
        result = self.client.publish(command_topic, command_json)
        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            print(f"{GREEN}✅ OTA command sent to {target_device}: {firmware_url}{RESET}")
            return True
        else:
            print(f"{RED}❌ Failed to send OTA command{RESET}")
            return False
            
    def send_restart_command(self, device_id=None):
        if not self.connected:
            print(f"{RED}❌ Not connected to MQTT broker{RESET}")
            return False
            
        target_device = device_id or self.selected_device
        if not target_device:
            print(f"{RED}❌ No device selected{RESET}")
            return False
            
        command_topic = f"{BASE_TOPIC}/{target_device}/command"
        result = self.client.publish(command_topic, "restart")
        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            print(f"{GREEN}✅ Restart command sent to {target_device}{RESET}")
            return True
        else:
            print(f"{RED}❌ Failed to send restart command{RESET}")
            return False
            
    def disconnect(self):
        if self.client:
            self.client.loop_stop()
            self.client.disconnect()
    
    def _show_ota_completion_stats(self):
        """Display OTA completion statistics including average speed"""
        if not self.ota_start_time or not self.ota_progress_data:
            return
            
        total_time = time.time() - self.ota_start_time
        
        if self.ota_progress_data:
            # Calculate average speed from progress data
            total_bytes = self.ota_progress_data[-1][2] if self.ota_progress_data else self.firmware_size
            avg_speed_bps = total_bytes / total_time if total_time > 0 else 0
            avg_speed_kbps = avg_speed_bps / 1024
            
            if avg_speed_kbps > 1024:
                avg_speed_str = f"{avg_speed_kbps/1024:.1f} MB/s"
            else:
                avg_speed_str = f"{avg_speed_kbps:.1f} KB/s"
                
            print(f"{GREEN}📊 OTA Transfer Statistics:{RESET}")
            print(f"   📦 Total size: {self.firmware_size:,} bytes ({self.firmware_size/1024:.1f} KB)")
            print(f"   ⏱️  Total time: {total_time:.1f} seconds")
            print(f"   🚀 Average speed: {avg_speed_str}{RESET}")
        
        # Reset tracking data
        self.ota_start_time = None
        self.ota_progress_data.clear()


def start_http_server(firmware_path, port=8000):
    """Start HTTP server to serve firmware file"""
    
    def handler(*args, **kwargs):
        return FirmwareHTTPRequestHandler(*args, firmware_path=firmware_path, **kwargs)
    
    try:
        server = HTTPServer(('', port), handler)
        print(f"{GREEN}🌐 HTTP server started on port {port}{RESET}")
        print(f"{CYAN}📁 Serving firmware: {os.path.basename(firmware_path)}{RESET}")
        
        def serve_forever():
            server.serve_forever()
            
        server_thread = threading.Thread(target=serve_forever, daemon=True)
        server_thread.start()
        
        return server, port
    except Exception as e:
        print(f"{RED}❌ Failed to start HTTP server: {e}{RESET}")
        return None, None


def find_firmware_file():
    """Try to find the firmware file automatically"""
    possible_paths = [
        "build/esp32s3_ade7953.bin",
        "../build/esp32s3_ade7953.bin",
        "../../build/esp32s3_ade7953.bin",
        "esp32s3_ade7953.bin"
    ]

    for path in possible_paths:
        if os.path.exists(path):
            return os.path.abspath(path)

    return None


def get_local_ip():
    """Get local IP address"""
    import socket
    try:
        # Connect to a remote server to determine local IP
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except:
        return "127.0.0.1"


def main():
    parser = argparse.ArgumentParser(description="MQTT OTA Update Tool for Grid Frequency Monitor")
    parser.add_argument("-f", "--firmware", help="Firmware file path")
    parser.add_argument("--http-port", type=int, default=8000, help="HTTP server port (default: 8000)")
    parser.add_argument("--restart-only", action="store_true", help="Send restart command only")
    parser.add_argument("--device", help="Specific device ID to target (skip discovery)")
    parser.add_argument("--discovery-timeout", type=int, default=DEVICE_DISCOVERY_TIMEOUT, 
                       help=f"Device discovery timeout in seconds (default: {DEVICE_DISCOVERY_TIMEOUT})")
    parser.add_argument("--automatic", action="store_true", 
                       help="Automatic mode: discover devices, select first one found, and perform update without user interaction")
    
    args = parser.parse_args()
    
    print(f"{GREEN}Grid Frequency Monitor - MQTT OTA Update Tool{RESET}")
    print("=" * 60)
    
    # Connect to MQTT first
    mqtt_updater = MQTTOTAUpdater(BROKER, PORT, USERNAME, PASSWORD)
    if not mqtt_updater.connect():
        print(f"{RED}❌ Failed to connect to MQTT broker{RESET}")
        sys.exit(1)
    
    # Device selection
    selected_device = None
    if args.device:
        # Use specified device
        selected_device = args.device
        mqtt_updater.selected_device = selected_device
        print(f"{GREEN}🎯 Targeting specific device: {selected_device}{RESET}")
        # Subscribe to device-specific topics
        mqtt_updater.client.subscribe(f"{BASE_TOPIC}/{selected_device}/status")
        mqtt_updater.client.subscribe(f"{BASE_TOPIC}/{selected_device}/error")
        mqtt_updater.client.subscribe(f"{BASE_TOPIC}/{selected_device}/logs")
    else:
        # Discover devices
        print(f"\n{CYAN}🔍 Discovering active devices...{RESET}")
        device_list = mqtt_updater.discover_devices(args.discovery_timeout, auto_mode=args.automatic)
        
        if not device_list:
            print(f"{RED}❌ No devices found. Make sure devices are sending measurement data.{RESET}")
            mqtt_updater.disconnect()
            sys.exit(1)
        
        # Select device based on mode
        if args.automatic:
            selected_device = mqtt_updater.select_device_auto(device_list)
        else:
            selected_device = mqtt_updater.select_device_interactive(device_list)
            
        if not selected_device:
            print(f"{YELLOW}Operation cancelled.{RESET}")
            mqtt_updater.disconnect()
            sys.exit(0)
    
    if args.restart_only:
        # Just send restart command to selected device
        mqtt_updater.send_restart_command()
        time.sleep(2)
        mqtt_updater.disconnect()
        return
    
    # Find firmware file
    if args.firmware:
        firmware_path = args.firmware
    else:
        firmware_path = find_firmware_file()
        if firmware_path:
            print(f"🔍 {GREEN}Auto-detected firmware file:{RESET} {firmware_path}")
        else:
            print(f"{RED}❌ Error: Could not find firmware file automatically.{RESET}")
            print("Please specify the firmware file path with -f/--firmware argument.")
            sys.exit(1)
    
    if not os.path.exists(firmware_path):
        print(f"{RED}❌ Error: Firmware file '{firmware_path}' not found!{RESET}")
        mqtt_updater.disconnect()
        sys.exit(1)
    
    # Show firmware information
    file_size = os.path.getsize(firmware_path)
    build_time = datetime.datetime.fromtimestamp(os.path.getmtime(firmware_path))
    
    print(f"\n📋 {YELLOW}Firmware Information:{RESET}")
    print(f"📁 {YELLOW}File:{RESET} {firmware_path}")
    print(f"📦 {YELLOW}Size:{RESET} {file_size:,} bytes ({file_size / 1024:.1f} KB)")
    print(f"🗓️ {YELLOW}Build time:{RESET} {build_time.strftime('%Y-%m-%d %H:%M:%S')}")
    
    print(f"\n� {YELLOW}Target Device:{RESET}")
    print(f"🔧 {YELLOW}Device ID:{RESET} {selected_device}")
    
    print(f"\n�🌐 {YELLOW}Network Configuration:{RESET}")
    print(f"🔌 {YELLOW}MQTT Broker:{RESET} {BROKER}:{PORT}")
    print(f"🌍 {YELLOW}HTTP Server:{RESET} Port {args.http_port}")
    
    # Get local IP
    local_ip = get_local_ip()
    firmware_url = f"http://{local_ip}:{args.http_port}/firmware.bin"
    print(f"📡 {YELLOW}Firmware URL:{RESET} {firmware_url}")
    
    # Confirm operation
    if args.automatic:
        print(f"\n{GREEN}🤖 Automatic mode: Starting MQTT OTA update for device: {selected_device}!{RESET}")
    else:
        print(f"\n{GREEN}Ready to start MQTT OTA update for device: {selected_device}!{RESET}")
        try:
            input(f"Press {GREEN}Enter{RESET} to continue, or {RED}Ctrl+C{RESET} to cancel...")
        except KeyboardInterrupt:
            print(f"\n{YELLOW}Update cancelled.{RESET}")
            mqtt_updater.disconnect()
            sys.exit(0)
    
    # Start HTTP server
    server, server_port = start_http_server(firmware_path, args.http_port)
    if not server:
        mqtt_updater.disconnect()
        sys.exit(1)
    
    try:
        # Send OTA command
        print(f"\n🚀 {GREEN}Starting MQTT OTA update for device: {selected_device}...{RESET}")
        if mqtt_updater.send_ota_command(firmware_url, firmware_size=file_size):
            print(f"{CYAN}📡 Monitoring OTA progress via MQTT...{RESET}")
            print(f"{YELLOW}⏱️ Waiting for OTA completion...{RESET}")
            
            # Monitor for completion
            start_time = time.time()
            timeout = 300  # 5 minutes timeout
            
            while time.time() - start_time < timeout:
                time.sleep(1)
                
                # Check for restart or completion messages
                for timestamp, topic, payload in mqtt_updater.status_updates[-5:]:
                    if "restarting" in payload.lower() or "restart" in payload.lower():
                        print(f"\n{GREEN}🎉 OTA update completed successfully!{RESET}")
                        print(f"{GREEN}🔄 Device {selected_device} is restarting with new firmware{RESET}")
                        break
                else:
                    continue
                break
            else:
                print(f"\n{YELLOW}⏰ OTA update timeout reached{RESET}")
                print(f"{YELLOW}Check device logs for more information{RESET}")
        
    except KeyboardInterrupt:
        print(f"\n{YELLOW}⚠️ Operation cancelled by user{RESET}")
    
    finally:
        # Cleanup
        print(f"\n{CYAN}🧹 Cleaning up...{RESET}")
        mqtt_updater.disconnect()
        server.shutdown()
        print(f"{GREEN}✅ All done!{RESET}")


if __name__ == "__main__":
    main()
