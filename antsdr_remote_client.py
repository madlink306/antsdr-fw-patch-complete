#!/usr/bin/env python3
"""
ANTSDR Remote Control Client
============================

Simple Python script to control ANTSDR device remotely via UDP commands.
This demonstrates PC-side control of mode setup and transfer operations.

Usage:
    python3 antsdr_remote_client.py <board_ip> [command] [args...]
    
Examples:
    python3 antsdr_remote_client.py 192.168.1.12 info
    python3 antsdr_remote_client.py 192.168.1.12 set_mode 1
    python3 antsdr_remote_client.py 192.168.1.12 set_dac_bypass 1
    python3 antsdr_remote_client.py 192.168.1.12 start
    python3 antsdr_remote_client.py 192.168.1.12 get_stats
"""

import socket
import sys
import time
import argparse

class ANTSDRController:
    def __init__(self, board_ip, control_port=12346, timeout=5.0):
        self.board_ip = board_ip
        self.control_port = control_port
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        
    def send_command(self, command):
        """Send command to ANTSDR and return response"""
        try:
            # Send command
            self.sock.sendto(command.encode(), (self.board_ip, self.control_port))
            
            # Receive response
            response, addr = self.sock.recvfrom(1024)
            return response.decode().strip()
            
        except socket.timeout:
            return f"ERROR: Timeout - no response from {self.board_ip}:{self.control_port}"
        except Exception as e:
            return f"ERROR: {str(e)}"
    
    def get_info(self):
        """Get device information"""
        return self.send_command("info")
    
    def get_status(self):
        """Get current status"""
        return self.send_command("get_status")
    
    def set_mode(self, mode):
        """Set operation mode (0 or 1)"""
        if mode not in [0, 1]:
            return "ERROR: Mode must be 0 or 1"
        return self.send_command(f"set_mode {mode}")
    
    def get_mode(self):
        """Get current operation mode"""
        return self.send_command("get_mode")
    
    def set_dac_bypass(self, enable):
        """Enable/disable PL DAC bypass mode"""
        bypass_val = 1 if enable else 0
        return self.send_command(f"set_dac_bypass {bypass_val}")
    
    def get_dac_bypass(self):
        """Get current DAC bypass status"""
        return self.send_command("get_dac_bypass")
    
    def set_buffer_size(self, size):
        """Set buffer size"""
        if size < 512 or size > 65536 or size % 4 != 0:
            return "ERROR: Buffer size must be 512-65536 bytes, 4-byte aligned"
        return self.send_command(f"set_buffer {size}")
    
    def set_destination(self, dest_ip, dest_port):
        """Set UDP destination for data stream"""
        return self.send_command(f"set_dest {dest_ip} {dest_port}")
    
    def start_streaming(self):
        """Start DMA streaming"""
        return self.send_command("start")
    
    def stop_streaming(self):
        """Stop DMA streaming"""
        return self.send_command("stop")
    
    def get_stats(self):
        """Get transfer statistics"""
        return self.send_command("get_stats")
    
    def reset(self):
        """Reset and stop streaming"""
        return self.send_command("reset")
    
    def monitor_stats(self, duration=10, interval=2):
        """Monitor statistics for specified duration"""
        print(f"Monitoring statistics for {duration} seconds...")
        start_time = time.time()
        
        while time.time() - start_time < duration:
            stats = self.get_stats()
            status = self.get_status()
            print(f"[{time.strftime('%H:%M:%S')}] {status}")
            print(f"[{time.strftime('%H:%M:%S')}] {stats}")
            print("-" * 50)
            time.sleep(interval)

def main():
    parser = argparse.ArgumentParser(description='ANTSDR Remote Control Client')
    parser.add_argument('board_ip', help='IP address of ANTSDR board')
    parser.add_argument('command', nargs='?', default='info', 
                      help='Command to execute (default: info)')
    parser.add_argument('args', nargs='*', help='Command arguments')
    parser.add_argument('--port', type=int, default=12346,
                      help='Control port (default: 12346)')
    parser.add_argument('--timeout', type=float, default=5.0,
                      help='Response timeout in seconds (default: 5.0)')
    
    args = parser.parse_args()
    
    # Create controller
    controller = ANTSDRController(args.board_ip, args.port, args.timeout)
    
    # Execute command
    command = args.command.lower()
    
    if command == 'info':
        response = controller.get_info()
        
    elif command == 'status':
        response = controller.get_status()
        
    elif command == 'set_mode':
        if not args.args:
            response = "ERROR: set_mode requires mode argument (0 or 1)"
        else:
            try:
                mode = int(args.args[0])
                response = controller.set_mode(mode)
            except ValueError:
                response = "ERROR: Mode must be a number (0 or 1)"
                
    elif command == 'get_mode':
        response = controller.get_mode()
        
    elif command == 'set_dac_bypass':
        if not args.args:
            response = "ERROR: set_dac_bypass requires enable argument (0 or 1)"
        else:
            try:
                enable = int(args.args[0])
                if enable not in [0, 1]:
                    response = "ERROR: DAC bypass enable must be 0 or 1"
                else:
                    response = controller.set_dac_bypass(enable == 1)
            except ValueError:
                response = "ERROR: DAC bypass enable must be a number (0 or 1)"
                
    elif command == 'get_dac_bypass':
        response = controller.get_dac_bypass()
        
    elif command == 'set_buffer':
        if not args.args:
            response = "ERROR: set_buffer requires size argument"
        else:
            try:
                size = int(args.args[0])
                response = controller.set_buffer_size(size)
            except ValueError:
                response = "ERROR: Buffer size must be a number"
                
    elif command == 'set_dest':
        if len(args.args) < 2:
            response = "ERROR: set_dest requires IP and port arguments"
        else:
            try:
                dest_ip = args.args[0]
                dest_port = int(args.args[1])
                response = controller.set_destination(dest_ip, dest_port)
            except ValueError:
                response = "ERROR: Port must be a number"
                
    elif command == 'start':
        response = controller.start_streaming()
        
    elif command == 'stop':
        response = controller.stop_streaming()
        
    elif command == 'stats':
        response = controller.get_stats()
        
    elif command == 'reset':
        response = controller.reset()
        
    elif command == 'monitor':
        duration = 10
        if args.args:
            try:
                duration = int(args.args[0])
            except ValueError:
                pass
        controller.monitor_stats(duration)
        return
        
    elif command == 'test_sequence':
        print("Running test sequence...")
        print("1. Getting device info...")
        print(f"   {controller.get_info()}")
        
        print("2. Setting mode to 1...")
        print(f"   {controller.set_mode(1)}")
        
        print("3. Setting buffer size to 4096...")
        print(f"   {controller.set_buffer_size(4096)}")
        
        print("4. Setting destination to 192.168.1.100:12345...")
        print(f"   {controller.set_destination('192.168.1.100', 12345)}")
        
        print("5. Starting streaming...")
        print(f"   {controller.start_streaming()}")
        
        print("6. Monitoring for 10 seconds...")
        controller.monitor_stats(10, 2)
        
        print("7. Stopping streaming...")
        print(f"   {controller.stop_streaming()}")
        
        print("8. Final statistics...")
        print(f"   {controller.get_stats()}")
        return
        
    else:
        response = f"ERROR: Unknown command '{command}'"
        print("\nAvailable commands:")
        print("  info                    - Get device information")
        print("  status                  - Get current status")
        print("  set_mode <0|1>          - Set operation mode")
        print("  get_mode                - Get current mode")
        print("  set_buffer <size>       - Set buffer size")
        print("  set_dest <ip> <port>    - Set destination")
        print("  start                   - Start streaming")
        print("  stop                    - Stop streaming")
        print("  stats                   - Get statistics")
        print("  reset                   - Reset and stop")
        print("  monitor [duration]      - Monitor stats")
        print("  test_sequence           - Run full test")
    
    print(response)

if __name__ == "__main__":
    main()
