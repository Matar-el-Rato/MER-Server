import socket
import struct

# Configuration
SERVER_HOST = "bolty.website"
SERVER_PORT = 8888

# Protocol constants (matching protocol.h)
REQ_REGISTER = 1
REQ_LOGIN = 2
MAX_USERNAME = 12
MAX_PASSWORD = 12

def test_request(req_type, username, password):
    # 1. Create packet
    # Format: byte + 12s + 12s (25 bytes total)
    # The 's' format automatically pads with \0
    packet = struct.pack("B12s12s", req_type, username.encode('ascii'), password.encode('ascii'))
    
    print(f"Connecting to {SERVER_HOST}:{SERVER_PORT}...")
    try:
        # 2. Create socket and connect
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((SERVER_HOST, SERVER_PORT))
            
            # 3. Send packet
            s.sendall(packet)
            
            # 4. Receive response (1 byte status + 128 byte message)
            response_data = s.recv(129)
            if len(response_data) >= 129:
                res_code, res_msg = struct.unpack("B128s", response_data[:129])
                # Decode with 'replace' to avoid crashes if there's junk memory
                res_msg = res_msg.decode('utf-8', errors='replace').split('\x00')[0]
                print(f"Response Code: {res_code}")
                print(f"Message:       {res_msg}")
            else:
                print(f"Error: Received incomplete response ({len(response_data)} bytes)")
                
    except Exception as e:
        print(f"Connection failed: {e}")

if __name__ == "__main__":
    print("--- Testing Register ---")
    test_request(REQ_REGISTER, "bolty_py", "pass123")
    
    # The server currently closes the connection after one request, 
    # so we would need to reconnect for login if we wanted to test both.
    # print("\n--- Testing Login ---")
    # test_request(REQ_LOGIN, "bolty_py", "pass123")
