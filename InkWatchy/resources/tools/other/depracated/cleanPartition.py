import csv
import sys
import subprocess

def parse_csv(csv_file):
    partitions = []
    with open(csv_file, mode='r') as file:
        reader = csv.DictReader(file)
        for row in reader:
            partitions.append(row)
    return partitions

def calculate_partition_offset(partitions, partition_name):
    offset = 0
    for partition in partitions:
        if partition['#Name'] == partition_name:
            return offset
        if partition['Size']:
            size = int(partition['Size'], 16)
            offset += size
        if partition['Offset']:
            size = int(partition['Offset'], 16)
            offset += size
    raise ValueError(partition_name + " partition not found in the CSV file")

def clear_partition(binary_file, offset, size):
    dd_command = [
        'dd', 'if=/dev/zero', 
        f'of={binary_file}', 
        f'bs=1', 
        f'seek={offset}', 
        f'count={size}', 
        'conv=notrunc'
    ]
    
    print("Executing command:", " ".join(dd_command))  # Print the command
    result = subprocess.run(dd_command, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Error executing dd: {result.stderr}")
    else:
        print("NVS partition cleared successfully")

def main():
    if len(sys.argv) != 4:
        print("Usage: python script.py <csv_file> <binary_file> <partition_name>")
        sys.exit(1)
    
    csv_file = sys.argv[1]
    binary_file = sys.argv[2]
    partition_name = sys.argv[3]
    
    try:
        partitions = parse_csv(csv_file)
        partition_offset = calculate_partition_offset(partitions, partition_name)
        partition_size = int(next(partition['Size'] for partition in partitions if partition['#Name'] == partition_name), 16)
        clear_partition(binary_file, partition_offset, partition_size)
    except ValueError as e:
        print(e)
        sys.exit(1)

if __name__ == "__main__":
    main()