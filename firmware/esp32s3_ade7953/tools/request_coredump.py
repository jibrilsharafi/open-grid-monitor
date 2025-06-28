#!/usr/bin/env python3
"""
Request core dumps from ESP32 devices via MQTT.

This script requests core dump data from ESP32 devices and saves it to a file.
MQTT configuration is read from environment variables:
    MQTT_BROKER (default: localhost)
    MQTT_PORT (default: 1883)
    MQTT_USERNAME (optional)
    MQTT_PASSWORD (optional)

Usage:
    # Request from specific device
    python request_coredump.py --device aabbccddeeff
    
    # Request from any device
    python request_coredump.py --any-device
    
    # With custom timeout
    python request_coredump.py --device aabbccddeeff --timeout 180
"""

import argparse
import os
import sys
import time
import json
import base64
from typing import Dict, Optional

# Fetch MQTT configuration from environment variables or set defaults
BROKER = "localhost"
PORT = 1883
USERNAME = None
PASSWORD = None

BASE_TOPIC = "open_grid_monitor"

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

class CoreDumpRequester:
    def __init__(self):
        self.chunks: Dict[int, bytes] = {}
        self.header_info: Optional[Dict] = None
        self.total_chunks: int = 0
        self.total_size: int = 0
        self.request_completed: bool = False
        self.request_timeout: bool = False
        
    def request_coredump(self, device_id: str = None, timeout: int = 120) -> bool:
        """Request core dump from device via MQTT command and capture the response"""
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print("paho-mqtt not installed. Install with: pip install paho-mqtt")
            return False
            
        # Get MQTT configuration from environment
        broker = BROKER
        port = PORT
        username = USERNAME
        password = PASSWORD

        print(f"Requesting core dump from device...")
        print(f"MQTT Broker: {broker}:{port}")
        print(f"Device: {device_id or 'any'}")
        print(f"Timeout: {timeout}s")
        print()
        
        def on_connect(client, userdata, flags, rc):
            if rc == 0:
                print("✓ Connected to MQTT broker")
                # Subscribe to core dump topics
                if device_id:
                    topic_pattern = f"{BASE_TOPIC}/{device_id}/coredump"
                    topic_pattern = f"{BASE_TOPIC}/{device_id}/coredump/chunk/+"
                    status_topic = f"{BASE_TOPIC}/{device_id}/status"
                    error_topic = f"{BASE_TOPIC}/{device_id}/error"
                    command_topic = f"{BASE_TOPIC}/{device_id}/command"
                else:
                    topic_pattern = f"{BASE_TOPIC}/+/coredump/+"
                    status_topic = f"{BASE_TOPIC}/+/status"
                    error_topic = f"{BASE_TOPIC}/+/error"
                    command_topic = f"{BASE_TOPIC}/+/command"

                client.subscribe(topic_pattern)
                client.subscribe(status_topic)
                client.subscribe(error_topic)
                print(f"✓ Subscribed to topics")
                
                # Send core dump request command
                print(f"→ Sending core dump request...")
                client.publish(command_topic, "coredump", qos=1)
            else:
                print(f"✗ Failed to connect to MQTT broker: {rc}")
                
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
                            print(f"✗ Device {message_device_id} has no core dump data")
                            self.request_completed = True
                        elif "starting transmission" in status_msg.lower():
                            print(f"→ Device {message_device_id} is starting core dump transmission")
                            
                    elif message_type == "error":
                        error_msg = msg.payload.decode('utf-8')
                        print(f"✗ Error from {message_device_id}: {error_msg}")
                        if "core dump" in error_msg.lower():
                            self.request_completed = True
                            
                    elif message_type == "coredump":
                        if "/header" in msg.topic:
                            self._process_header(json.loads(msg.payload.decode('utf-8')))
                        elif "/chunk/" in msg.topic:
                            self._process_chunk(json.loads(msg.payload.decode('utf-8')))
                        elif "/complete" in msg.topic:
                            self._process_complete(json.loads(msg.payload.decode('utf-8')))
                            print(f"✓ Core dump transfer completed from device {message_device_id}")
                            self.request_completed = True
                            
            except Exception as e:
                print(f"✗ Error processing MQTT message: {e}")
                
        client = mqtt.Client()
        if username and password:
            client.username_pw_set(username, password)
        client.on_connect = on_connect
        client.on_message = on_message
        
        try:
            client.connect(broker, port, 60)
            client.loop_start()
            
            # Wait for completion or timeout
            start_time = time.time()
            while not self.request_completed and (time.time() - start_time) < timeout:
                time.sleep(1)
                
            if not self.request_completed:
                print(f"✗ Request timed out after {timeout} seconds")
                self.request_timeout = True
                
            client.loop_stop()
            client.disconnect()
            
        except Exception as e:
            print(f"✗ MQTT error: {e}")
            return False
            
        return self._validate_chunks() if not self.request_timeout else False
    
    def _process_header(self, data: Dict):
        """Process core dump header message"""
        self.header_info = data
        print(f"→ Core dump header received:")
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
                print(f"→ Received chunk {chunk_index + 1}/{total_chunks} ({len(decoded_data)} bytes)")
            except Exception as e:
                print(f"✗ Failed to decode chunk {chunk_index}: {e}")
                
    def _process_complete(self, data: Dict):
        """Process core dump completion message"""
        self.total_size = data.get('total_size', 0)
        print(f"✓ Core dump transfer complete: {len(self.chunks)}/{self.total_chunks} chunks")
        
    def _validate_chunks(self) -> bool:
        """Validate that all chunks were received"""
        if self.total_chunks == 0:
            print("✗ No chunks received")
            return False
            
        missing_chunks = []
        for i in range(self.total_chunks):
            if i not in self.chunks:
                missing_chunks.append(i)
                
        if missing_chunks:
            print(f"✗ Missing chunks: {missing_chunks}")
            return False
            
        print(f"✓ All {self.total_chunks} chunks received successfully")
        return True
        
    def save_coredump(self, output_file: str) -> bool:
        """Save reconstructed core dump to file"""
        if not self.chunks:
            print("✗ No core dump data to save")
            return False
            
        try:
            with open(output_file, 'wb') as f:
                for i in range(self.total_chunks):
                    if i in self.chunks:
                        f.write(self.chunks[i])
                        
            file_size = os.path.getsize(output_file)
            print(f"✓ Core dump saved to {output_file} ({file_size} bytes)")
            
            # Save header info as well
            if self.header_info:
                header_file = output_file.replace('.bin', '_header.json')
                with open(header_file, 'w') as f:
                    json.dump(self.header_info, f, indent=2)
                print(f"✓ Header info saved to {header_file}")
                
            return True
            
        except Exception as e:
            print(f"✗ Failed to save core dump: {e}")
            return False


def main():
    parser = argparse.ArgumentParser(description='Request core dump from ESP32 device')
    parser.add_argument('--device', help='Device MAC address (without colons, e.g., aabbccddeeff)')
    parser.add_argument('--any-device', action='store_true', help='Request from any available device')
    parser.add_argument('--timeout', type=int, default=120, help='Timeout in seconds')
    parser.add_argument('--output', help='Output filename (auto-generated if not specified)')
    
    args = parser.parse_args()
    
    if not args.device and not args.any_device:
        print("Error: Must specify either --device or --any-device")
        parser.print_help()
        return 1
        
    # Auto-generate output filename
    if not args.output:
        if args.device:
            args.output = f"coredump_{args.device}.bin"
        else:
            timestamp = int(time.time())
            args.output = f"coredump_{timestamp}.bin"
            
    print("ESP32 Core Dump Requester")
    print("=" * 40)
    
    requester = CoreDumpRequester()
    
    try:
        success = requester.request_coredump(args.device, args.timeout)
        
        if success:
            if requester.save_coredump(args.output):
                print(f"\n✓ Core dump request completed successfully!")
                print(f"Output: {args.output}")
                return 0
            else:
                print(f"\n✗ Failed to save core dump")
                return 1
        else:
            print(f"\n✗ Failed to retrieve core dump")
            return 1
            
    except KeyboardInterrupt:
        print("\n✗ Operation cancelled by user")
        return 1


if __name__ == '__main__':
    sys.exit(main())
