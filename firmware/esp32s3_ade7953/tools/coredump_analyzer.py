#!/usr/bin/env python3
"""
Core Dump Analyzer for ESP32 Grid Monitor

This script reconstructs core dump data from MQTT messages and provides
basic analysis capabilities.

Usage:
    python coredump_analyzer.py --mqtt-log mqtt_messages.json --output coredump.bin
    python coredump_analyzer.py --analyze coredump.bin --elf firmware.elf

Requirements:
    pip install paho-mqtt
"""

import json
import base64
import argparse
import os
import sys
from typing import Dict, List, Optional
import subprocess


class CoreDumpAnalyzer:
    def __init__(self):
        self.chunks: Dict[int, bytes] = {}
        self.header_info: Optional[Dict] = None
        self.total_chunks: int = 0
        self.total_size: int = 0
        
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
        
    def process_live_mqtt(self, broker: str, port: int = 1883, 
                         username: str = None, password: str = None,
                         timeout: int = 60) -> bool:
        """Listen to live MQTT messages for core dump data"""
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            print("paho-mqtt not installed. Install with: pip install paho-mqtt")
            return False
            
        print(f"Connecting to MQTT broker: {broker}:{port}")
        
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
        client.username_pw_set(username, password)
        client.on_connect = on_connect
        client.on_message = on_message
        
        try:
            client.connect(broker, port, 60)
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


def main():
    parser = argparse.ArgumentParser(description='ESP32 Core Dump Analyzer')
    parser.add_argument('--mqtt-log', help='MQTT log file (JSON format)')
    parser.add_argument('--live-mqtt', help='MQTT broker address for live capture')
    parser.add_argument('--mqtt-port', type=int, default=1883, help='MQTT broker port')
    parser.add_argument('--mqtt-user', help='MQTT username')
    parser.add_argument('--mqtt-pass', help='MQTT password')
    parser.add_argument('--timeout', type=int, default=60, help='Timeout for live MQTT capture')
    parser.add_argument('--output', default='coredump.bin', help='Output file for core dump')
    parser.add_argument('--analyze', help='Analyze existing core dump file')
    parser.add_argument('--elf', help='ELF file for detailed analysis')
    
    args = parser.parse_args()
    
    analyzer = CoreDumpAnalyzer()
    
    if args.analyze:
        # Analyze existing core dump
        analyzer.analyze_coredump(args.analyze, args.elf)
        return
        
    success = False
    
    if args.mqtt_log:
        # Process MQTT log file
        success = analyzer.process_mqtt_log(args.mqtt_log)
    elif args.live_mqtt:
        # Capture live MQTT data
        success = analyzer.process_live_mqtt(
            args.live_mqtt, args.mqtt_port, 
            args.mqtt_user, args.mqtt_pass, args.timeout
        )
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
