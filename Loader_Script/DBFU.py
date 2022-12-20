import serial
import serial.tools.list_ports
from crc import Calculator, Configuration
import os
import sys
from tqdm import tqdm

num_STLinks = 0
port_devices = []

# Initialize firmware update packet headers and responses
EOT = b'\x54'
ACK = b'\x06'
SOH = b'\x01'
START= b'S'

# size of firmware data chunks in bytes
chunksize = 1024

#configure the CRC to be congruent with STM32 CRC
config = Configuration(
    width=16,
    polynomial=0x1021,
    init_value=0x1D0F,
    reverse_input=False,
    final_xor_value=0x00,
    reverse_output=False,
)
calc = Calculator(config, optimized=True)

# Check COM ports for ST Link
com_ports = serial.tools.list_ports.comports()
for port in com_ports:
    if port.manufacturer.startswith('STMicroelectronics'):
        port_name = port.device
        port_devices.append(port.device)
        num_STLinks += 1

# Check if there is more than one STLink, or no ST Links
if num_STLinks == 0:
    print('Error: No STLinks detected')
    print()
    input("Press Enter to continue...")
    quit()
elif num_STLinks > 1:
    print('Multiple ST Links Detected, please select one of the following COM ports:')
    for port in port_devices:
        print(port)
    print()
    print('COM port to connect to:', end=' ')
    port_name = input()


try:
    # Connect to serial port and ask the user for path to binary
    ser = serial.Serial(port_name, 115200)
    print()
    print('Connected to ST Link')
    print()
    print('Enter path to binary:', end=' ')
    path = input()
    print()

    # Prepare file size to send for first data packet
    file_stats = os.stat(path)
    file_size = file_stats.st_size
    file_size_bytes = file_size.to_bytes(3, byteorder='big')
    # Open file and wait for STM32 to send start byte to send 3 byte filesize
    file = open(path, 'rb')
    print('Waiting for device to initiate update...')
    ser.read_until(START)
    ser.write(file_size_bytes)

    print('Starting File Transfer')
    print()
    # File transfer loop
    end_of_file = False
    bytes_sent = 0

    progress_bar = tqdm(total=file_size, unit='Bytes')
    while not end_of_file:
        ser.read_until(ACK)
        chunk = file.read(chunksize)
        num_bytes_read = len(chunk)

        # end of file condition
        if num_bytes_read < chunksize:
            # remaining bytes with dummy data 0xFF
            chunk = chunk + (b'\xFF' * (chunksize - num_bytes_read)) 
            end_of_file = True

        #calculate checksum and wait for ACK to send next packet
        checksum_val = calc.checksum(chunk).to_bytes(2, byteorder='big')
        ser.write(SOH + chunk + checksum_val)
        bytes_sent += num_bytes_read
        progress_bar.update(num_bytes_read)

    # File read complete, wait for ACK then send end of transfer byte
    ser.read_until(ACK)
    ser.write(EOT)
    progress_bar.close()
    print()

    file.close()
    ser.close()

    print('File Transfer Complete')
    print()
    input("Press Enter to continue...")

except FileNotFoundError:
    print('Error: Cannot Open File')
    input("Press Enter to continue...")
    print()
except IOError as e:
    if e.strerror == None:
        print('Error: Invalid Serial Port')
        print()
        input("Press Enter to continue...")
    else:
        print('Error: IOError num {0}: {1}'.format(e.errno, e.strerror))
        print()
        input("Press Enter to continue...")