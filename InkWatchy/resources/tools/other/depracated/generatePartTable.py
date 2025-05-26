import os
import configparser
import subprocess
import math
import shutil
import sys

# Step 1: Read the configuration and get the needed values
fs_config_path = "in/fsConfig.ini"
config = configparser.ConfigParser()
config.optionxform = str  # Maintain case-sensitive keys
config.read(fs_config_path)

tolerance_percentage = int(config["DEFAULT"]["tolerance"].strip())
flash_size_mb = int(config["DEFAULT"]["flashSize"].strip())
flash_size_bytes = flash_size_mb * 1024 * 1024  # Convert MB to bytes

global_functions_path = "../globalFunctions.sh"
cmd = f"source {global_functions_path} && get_pio_env ../../../.vscode/launch.json"
result = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, executable='/bin/bash')
pio_env = result.stdout.decode().strip()
print(result.stderr.decode('utf-8'))

firmware_path = f"../../../.pio/build/{pio_env}/firmware.bin"

if not os.path.exists(firmware_path):
    shutil.copy("templates/snoititrap-template.csv", "in/partitions.csv")
    print("There is no firmware.bin file. I'm assuming you couldn't compile it because of the partition size. Copying the default partition file and exiting.")
    sys.exit(0)

firmware_size_bytes = int(os.path.getsize(firmware_path))

tolerance_fraction = tolerance_percentage / 100
app_partition_size_bytes = (firmware_size_bytes + int(firmware_size_bytes * tolerance_fraction) + 0xFFF) & ~0xFFF

coredump_partition_size_bytes = 0x10000
nvs_partition_size_bytes = 0x3000
app_partition_offset = 0x10000

current_used_flash = coredump_partition_size_bytes + app_partition_offset + nvs_partition_size_bytes

current_used_flash = current_used_flash + app_partition_size_bytes

os.makedirs("in", exist_ok=True)

with open("in/offset.txt", "w") as f:
    f.write(str(hex(current_used_flash)) + "\n")

#print("current_used_flash: " + str(current_used_flash))
#print("flash_size_bytes: " + str(flash_size_bytes))
spiffs_partition_size_bytes = (flash_size_bytes - current_used_flash - 0xFFF) & ~0xFFF

with open("in/size.txt", "w") as f:
    f.write(str(spiffs_partition_size_bytes) + "\n")

# Create the partition table as a list of strings
partition_table = []

# Add the column headers as the first line
partition_table.append("#Name,Type,SubType,Offset,Size,Flags\n")

# Define the partitions
partition_table.append(f"app,app,factory,0x{app_partition_offset:x},{hex(app_partition_size_bytes)},\n")
partition_table.append(f"coredump,data,coredump,,0x{coredump_partition_size_bytes:x},\n")
partition_table.append(f"nvs,data,nvs,,0x{nvs_partition_size_bytes:x},\n")
partition_table.append(f"littlefs,data,undefined,,{hex(spiffs_partition_size_bytes)},\n")

# Write the partition table to a file
partition_table_path = "in/partitions.csv"

with open(partition_table_path, 'w') as f:
    f.writelines(partition_table)

print("Partition table created successfully.")
