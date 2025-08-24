#!/usr/bin/env python3
"""
ANTSDR DMA UDP Receiver Test Script

This script receives UDP packets from the ANTSDR DMA driver
and displays statistics about the received data.
"""

import socket
import struct
import time
import argparse
import signal
import sys

class UDPReceiver:
    def __init__(self, port=12345, buffer_size=4096):
        self.port = port
        self.buffer_size = buffer_size
        self.sock = None
        self.running = True
        self.stats = {
            'packets_received': 0,
            'bytes_received': 0,
            'start_time': None,
            'last_packet_time': None
        }
        
    def signal_handler(self, signum, frame):
        print(f"\nReceived signal {signum}, stopping...")
        self.running = False
        
    def start_receiver(self):
        """Start the UDP receiver"""
        try:
            # Create UDP socket
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # Bind to all interfaces
            self.sock.bind(('', self.port))
            self.sock.settimeout(1.0)  # 1 second timeout
            
            print(f"UDP receiver listening on port {self.port}")
            print("Waiting for data from ANTSDR DMA driver...")
            print("Press Ctrl+C to stop\n")
            
            self.stats['start_time'] = time.time()
            
            while self.running:
                try:
                    data, addr = self.sock.recvfrom(self.buffer_size)
                    self.process_packet(data, addr)
                    
                except socket.timeout:
                    # Timeout is normal, just continue
                    continue
                except Exception as e:
                    print(f"Error receiving data: {e}")
                    break
                    
        except Exception as e:
            print(f"Failed to start receiver: {e}")
            return 1
            
        finally:
            if self.sock:
                self.sock.close()
                
        return 0
        
    def process_packet(self, data, addr):
        """Process received packet"""
        current_time = time.time()
        
        self.stats['packets_received'] += 1
        self.stats['bytes_received'] += len(data)
        self.stats['last_packet_time'] = current_time
        
        # Print statistics every 100 packets
        if self.stats['packets_received'] % 100 == 0:
            self.print_stats()
            
        # Print first packet details
        if self.stats['packets_received'] == 1:
            print(f"First packet received from {addr[0]}:{addr[1]}")
            print(f"Packet size: {len(data)} bytes")
            print(f"First 16 bytes: {data[:16].hex()}")
            print()
            
    def print_stats(self):
        """Print current statistics"""
        elapsed = time.time() - self.stats['start_time']
        
        packets = self.stats['packets_received']
        bytes_total = self.stats['bytes_received']
        
        packets_per_sec = packets / elapsed if elapsed > 0 else 0
        bytes_per_sec = bytes_total / elapsed if elapsed > 0 else 0
        mbps = (bytes_per_sec * 8) / (1024 * 1024)
        
        print(f"\rPackets: {packets:6d} | "
              f"Bytes: {bytes_total:8d} | "
              f"Rate: {packets_per_sec:6.1f} pkt/s | "
              f"Throughput: {mbps:6.2f} Mbps", end='', flush=True)
              
    def print_final_stats(self):
        """Print final statistics"""
        print("\n" + "="*60)
        print("Final Statistics:")
        print("="*60)
        
        elapsed = time.time() - self.stats['start_time']
        packets = self.stats['packets_received']
        bytes_total = self.stats['bytes_received']
        
        print(f"Total packets received: {packets}")
        print(f"Total bytes received:   {bytes_total}")
        print(f"Test duration:          {elapsed:.2f} seconds")
        
        if elapsed > 0:
            packets_per_sec = packets / elapsed
            bytes_per_sec = bytes_total / elapsed
            mbps = (bytes_per_sec * 8) / (1024 * 1024)
            
            print(f"Average packet rate:    {packets_per_sec:.1f} packets/second")
            print(f"Average throughput:     {mbps:.2f} Mbps")
            print(f"Average packet size:    {bytes_total/packets:.1f} bytes" if packets > 0 else "Average packet size: 0 bytes")

def main():
    parser = argparse.ArgumentParser(description='ANTSDR DMA UDP Receiver')
    parser.add_argument('-p', '--port', type=int, default=12345,
                       help='UDP port to listen on (default: 12345)')
    parser.add_argument('-b', '--buffer-size', type=int, default=4096,
                       help='Receive buffer size (default: 4096)')
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Enable verbose output')
    
    args = parser.parse_args()
    
    receiver = UDPReceiver(port=args.port, buffer_size=args.buffer_size)
    
    # Setup signal handlers
    signal.signal(signal.SIGINT, receiver.signal_handler)
    signal.signal(signal.SIGTERM, receiver.signal_handler)
    
    try:
        ret = receiver.start_receiver()
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        ret = 0
    finally:
        receiver.print_final_stats()
        
    return ret

if __name__ == '__main__':
    sys.exit(main())
