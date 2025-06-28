#!/usr/bin/env python3
"""
Core Dump Analyzer for ESP32 Grid Monitor

This script reconstructs core dump data from MQTT messages and provides
basic analysis capabilities. It can also request core dumps from devices
via MQTT commands.

MQTT configuration is read from environment variables or .env file:
    MQTT_BROKER (default: localhost)
    MQTT_PORT (default: 1883)
    MQTT_USERNAME (optional)
    MQTT_PASSWORD (optional)

Usage:
    # Request core dump from a specific device (device-id required)
    python coredump_analyzer.py --request --device-id aabbccddeeff --output coredump.bin
    
    # Request core dump from any device (broadcast)
    python coredump_analyzer.py --request --output coredump.bin
    
    # Process existing MQTT log file
    python coredump_analyzer.py --mqtt-log mqtt_messages.json --output coredump.bin
    
    # Live capture from MQTT
    python coredump_analyzer.py --live --output coredump.bin
    
    # Analyze existing core dump
    python coredump_analyzer.py --analyze coredump.bin --elf firmware.elf

Requirements:
    pip install paho-mqtt python-dotenv
"""

import json
import base64
import argparse
import os
import sys
import time
from typing import Dict, List, Optional
import subprocess

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
    print("Info: python-dotenv not available. Install with 'pip install python-dotenv' to use .env files.")

BASE_TOPIC = "open_grid_monitor"


class CoreDumpAnalyzer:
    def __init__(self):
        self.chunks: Dict[int, bytes] = {}
        self.header_info: Optional[Dict] = None
        self.total_chunks: int = 0
        self.total_size: int = 0
        self.request_completed: bool = False
        self.request_timeout: bool = False
        
    def process_mqtt_log(self, log_file: str) -> bool:
        """Process MQTT log file and extract core dump data"""
        print(f"Processing MQTT log file: {log_file}")
        
        try:
            with open(log_file, 'r') as f:
                messages = json.load(f)
        except Exception as e:
            print(f"Error reading log file: {e}")
            return False
            
        coredump_messages = []
        
        # Extract core dump related messages
        for msg in messages:
            if isinstance(msg, dict) and 'topic' in msg:
                topic = msg['topic']
                if '/coredump' in topic:
                    coredump_messages.append(msg)
                    
        if not coredump_messages:
            print("No core dump messages found in log file")
            return False
            
        print(f"Found {len(coredump_messages)} core dump messages")
        
        # Process messages
        for msg in coredump_messages:
            self._process_message(msg)
            
        return self._validate_chunks()
        
    def process_live_mqtt(self, timeout: int = 60) -> bool:
        """Listen to live MQTT messages for core dump data"""
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print("paho-mqtt not installed. Install with: pip install paho-mqtt")
            return False
            
        print(f"Connecting to MQTT broker: {BROKER}:{PORT}")
        
        def on_connect(client, userdata, flags, rc):
            if rc == 0:
                print("Connected to MQTT broker")
                client.subscribe("open_grid_monitor/coredump/+")
            else:
                print(f"Failed to connect to MQTT broker: {rc}")
                
        def on_message(client, userdata, msg):
            message = {
                'topic': msg.topic,
                'payload': msg.payload.decode('utf-8'),
                'timestamp': time.time()
            }
            self._process_message(message)
            
        client = mqtt.Client()
        if USERNAME and PASSWORD:
            client.username_pw_set(USERNAME, PASSWORD)
        client.on_connect = on_connect
        client.on_message = on_message
        
        try:
            client.connect(BROKER, PORT, 60)
            client.loop_start()
            
            print(f"Waiting for core dump messages (timeout: {timeout}s)...")
            time.sleep(timeout)
            
            client.loop_stop()
            client.disconnect()
            
        except Exception as e:
            print(f"MQTT error: {e}")
            return False
            
        return self._validate_chunks()
        
    def _process_message(self, msg: Dict):
        """Process a single MQTT message"""
        topic = msg['topic']
        
        try:
            if 'payload' in msg:
                payload = json.loads(msg['payload'])
            else:
                payload = json.loads(msg.get('data', '{}'))
        except:
            print(f"Failed to parse JSON from topic: {topic}")
            return
            
        if '/header' in topic:
            self._process_header(payload)
        elif '/chunk/' in topic:
            self._process_chunk(payload)
        elif '/complete' in topic:
            self._process_complete(payload)
            
    def _process_header(self, data: Dict):
        """Process core dump header message"""
        self.header_info = data
        print(f"Core dump header received:")
        print(f"  Reset reason: {data.get('reset_reason', 'unknown')}")
        print(f"  Firmware version: {data.get('firmware_version', 'unknown')}")
        print(f"  Partition size: {data.get('partition_size', 0)} bytes")
        
    def _process_chunk(self, data: Dict):
        """Process core dump chunk message"""
        chunk_index = data.get('chunk_index', -1)
        total_chunks = data.get('total_chunks', 0)
        chunk_data = data.get('data', '')
        
        if chunk_index >= 0 and chunk_data:
            try:
                decoded_data = base64.b64decode(chunk_data)
                self.chunks[chunk_index] = decoded_data
                self.total_chunks = max(self.total_chunks, total_chunks)
                print(f"Received chunk {chunk_index}/{total_chunks} ({len(decoded_data)} bytes)")
            except Exception as e:
                print(f"Failed to decode chunk {chunk_index}: {e}")
                
    def _process_complete(self, data: Dict):
        """Process core dump completion message"""
        self.total_size = data.get('total_size', 0)
        print(f"Core dump transfer complete: {len(self.chunks)}/{self.total_chunks} chunks")
        
    def _validate_chunks(self) -> bool:
        """Validate that all chunks were received"""
        if self.total_chunks == 0:
            print("No chunks received")
            return False
            
        missing_chunks = []
        for i in range(self.total_chunks):
            if i not in self.chunks:
                missing_chunks.append(i)
                
        if missing_chunks:
            print(f"Missing chunks: {missing_chunks}")
            return False
            
        print(f"All {self.total_chunks} chunks received successfully")
        return True
        
    def save_coredump(self, output_file: str) -> bool:
        """Save reconstructed core dump to file"""
        if not self.chunks:
            print("No core dump data to save")
            return False
            
        try:
            with open(output_file, 'wb') as f:
                for i in range(self.total_chunks):
                    if i in self.chunks:
                        f.write(self.chunks[i])
                        
            file_size = os.path.getsize(output_file)
            print(f"Core dump saved to {output_file} ({file_size} bytes)")
            
            # Save header info as well
            if self.header_info:
                header_file = output_file.replace('.bin', '_header.json')
                with open(header_file, 'w') as f:
                    json.dump(self.header_info, f, indent=2)
                print(f"Header info saved to {header_file}")
                
            return True
            
        except Exception as e:
            print(f"Failed to save core dump: {e}")
            return False
            
    def analyze_coredump(self, coredump_file: str, elf_file: str = None):
        """Analyze core dump using ESP-IDF tools"""
        if not os.path.exists(coredump_file):
            print(f"Core dump file not found: {coredump_file}")
            return False
            
        # Try to find esp-idf tools
        esp_idf_path = os.environ.get('IDF_PATH')
        if not esp_idf_path:
            print("ESP-IDF path not found. Set IDF_PATH environment variable")
            return False
            
        coredump_tool = os.path.join(esp_idf_path, 'components', 'espcoredump', 'espcoredump.py')
        if not os.path.exists(coredump_tool):
            print(f"Core dump tool not found: {coredump_tool}")
            return False
            
        if elf_file and os.path.exists(elf_file):
            print(f"Analyzing core dump with ELF file: {elf_file}")
            cmd = [
                'python', coredump_tool,
                'info_corefile',
                '--core', coredump_file,
                '--core-format', 'raw',
                elf_file
            ]
        else:
            print("Analyzing core dump without ELF file (limited info)")
            cmd = [
                'python', coredump_tool,
                'info_corefile',
                '--core', coredump_file,
                '--core-format', 'raw'
            ]
            
        try:
            result = subprocess.run(cmd, capture_output=True, text=True)
            print("Core dump analysis:")
            print(result.stdout)
            if result.stderr:
                print("Errors:")
                print(result.stderr)
                
        except Exception as e:
            print(f"Failed to run core dump analysis: {e}")
            
        return True

    def request_coredump_via_mqtt(self, device_id: str = None, timeout: int = 120) -> bool:
        """Request core dump from device via MQTT command and capture the response"""
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print("paho-mqtt not installed. Install with: pip install paho-mqtt")
            return False
            
        print(f"Requesting core dump from device via MQTT...")
        print(f"Broker: {BROKER}:{PORT}, Device: {device_id or 'any'}, Timeout: {timeout}s")
        
        self.request_completed = False
        self.request_timeout = False
        
        def on_connect(client, userdata, flags, rc):
            if rc == 0:
                print("Connected to MQTT broker")
                # Subscribe to core dump topics
                if device_id:
                    topic_pattern = f"open_grid_monitor/{device_id}/coredump/+"
                    status_topic = f"open_grid_monitor/{device_id}/status"
                    error_topic = f"open_grid_monitor/{device_id}/error"
                else:
                    topic_pattern = "open_grid_monitor/+/coredump/+"
                    status_topic = "open_grid_monitor/+/status"
                    error_topic = "open_grid_monitor/+/error"
                    
                client.subscribe(topic_pattern)
                client.subscribe(status_topic)
                client.subscribe(error_topic)
                print(f"Subscribed to: {topic_pattern}")
                
                # Send core dump request command
                if device_id:
                    command_topic = f"open_grid_monitor/{device_id}/command"
                else:
                    # If no device_id specified, try broadcasting to all devices
                    command_topic = "open_grid_monitor/+/command"
                    print("Warning: No device ID specified, sending broadcast command")
                    
                print(f"Sending core dump request to: {command_topic}")
                client.publish(command_topic, "coredump", qos=1)
            else:
                print(f"Failed to connect to MQTT broker: {rc}")
                
        def on_message(client, userdata, msg):
            try:
                topic_parts = msg.topic.split('/')
                if len(topic_parts) >= 3:
                    message_device_id = topic_parts[1]
                    message_type = topic_parts[2]
                    
                    if message_type == "status":
                        status_msg = msg.payload.decode('utf-8')
                        print(f"Status from {message_device_id}: {status_msg}")
                        
                        if "No core dump data available" in status_msg:
                            print(f"Device {message_device_id} has no core dump data")
                            self.request_completed = True
                        elif "starting transmission" in status_msg.lower():
                            print(f"Device {message_device_id} is starting core dump transmission")
                            
                    elif message_type == "error":
                        error_msg = msg.payload.decode('utf-8')
                        print(f"Error from {message_device_id}: {error_msg}")
                        if "core dump" in error_msg.lower():
                            self.request_completed = True
                            
                    elif message_type == "coredump":
                        # Process core dump message using existing logic
                        message = {
                            'topic': msg.topic,
                            'payload': msg.payload.decode('utf-8'),
                            'timestamp': time.time()
                        }
                        self._process_message(message)
                        
                        # Check if this was a completion message
                        if "/complete" in msg.topic:
                            print(f"Core dump transfer completed from device {message_device_id}")
                            self.request_completed = True
                            
            except Exception as e:
                print(f"Error processing MQTT message: {e}")
                
        client = mqtt.Client()
        if USERNAME and PASSWORD:
            client.username_pw_set(USERNAME, PASSWORD)
        client.on_connect = on_connect
        client.on_message = on_message
        
        try:
            client.connect(BROKER, PORT, 60)
            client.loop_start()
            
            # Wait for completion or timeout
            start_time = time.time()
            while not self.request_completed and (time.time() - start_time) < timeout:
                time.sleep(1)
                
            if not self.request_completed:
                print(f"Request timed out after {timeout} seconds")
                self.request_timeout = True
                
            client.loop_stop()
            client.disconnect()
            
        except Exception as e:
            print(f"MQTT error: {e}")
            return False
            
        return self._validate_chunks() if not self.request_timeout else False
    
def main():
    parser = argparse.ArgumentParser(description='ESP32 Core Dump Analyzer')
    parser.add_argument('--mqtt-log', help='MQTT log file (JSON format)')
    parser.add_argument('--live', action='store_true', help='Live capture from MQTT (uses environment variables for connection)')
    parser.add_argument('--request', action='store_true', help='Request core dump from device via MQTT')
    parser.add_argument('--device-id', help='Specific device ID to request core dump from (MAC address without colons)')
    parser.add_argument('--timeout', type=int, default=60, help='Timeout for live MQTT capture or core dump request (default: 60s)')
    parser.add_argument('--output', default='coredump.bin', help='Output file for core dump (default: coredump.bin)')
    parser.add_argument('--analyze', help='Analyze existing core dump file')
    parser.add_argument('--elf', help='ELF file for detailed analysis')
    
    args = parser.parse_args()
    
    # Show MQTT configuration for live and request modes
    if args.live or args.request:
        print(f"MQTT Configuration:")
        print(f"  Broker: {BROKER}:{PORT}")
        print(f"  Username: {'***' if USERNAME else 'None'}")
        print(f"  Password: {'***' if PASSWORD else 'None'}")
        print()
    
    analyzer = CoreDumpAnalyzer()
    
    if args.analyze:
        # Analyze existing core dump
        analyzer.analyze_coredump(args.analyze, args.elf)
        return
        
    success = False
    
    if args.mqtt_log:
        # Process MQTT log file
        success = analyzer.process_mqtt_log(args.mqtt_log)
    elif args.live:
        # Capture live MQTT data
        success = analyzer.process_live_mqtt(args.timeout)
    elif args.request:
        # Request core dump from device via MQTT
        success = analyzer.request_coredump_via_mqtt(args.device_id, args.timeout)
    else:
        parser.print_help()
        return
        
    if success:
        # Save the reconstructed core dump
        if analyzer.save_coredump(args.output):
            print(f"\nCore dump reconstruction completed successfully!")
            print(f"To analyze the core dump, run:")
            print(f"  python {sys.argv[0]} --analyze {args.output} --elf firmware.elf")
        else:
            print("Failed to save core dump")
    else:
        print("Failed to reconstruct core dump")


if __name__ == '__main__':
    main()
