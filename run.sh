#build/qemu-system-xtensa -machine esp32 -drive file=/home/gamboa/microcontroladores/TTGODemo/.pio/build/emulator/flash_image.bin,if=mtd,format=raw -display default,show-cursor=on -nic user,model=esp32_wifi,net=192.168.4.0/24,hostfwd=tcp::16555-192.168.4.1:80 -parallel none -monitor none 
#build/qemu-system-xtensa -machine esp32 -drive file=/home/gamboa/microcontroladores/TTGODemo/.pio/build/emulator/flash_image.bin,if=mtd,format=raw -display default,show-cursor=on -nic user,model=esp32_wifi,net=192.168.4.0/24,hostfwd=tcp::16555-192.168.4.1:80 -parallel none -monitor none -serial /dev/tnt2 -bios /vbox/qemu/pc-bios/esp32-v3-rom-app.bin -bios /vbox/qemu/pc-bios/esp32-v3-rom.bin
build/qemu-system-xtensa -M esp32 \
 -drive file=/home/gamboa/.picsimlab/mdump_ESP32_DevKitC_ESP32.bin,if=mtd,format=raw \
 -serial tcp::5555,server,nowait\
 -global driver=timer.esp32.timg,property=wdt_disable,value=true \
 -qmp tcp:localhost:4444,server,wait=off \
 -global driver=esp32.gpio,property=strap_mode,value=0x0f
 
#-global driver=esp32.gpio,property=strap_mode,value=0x0f \
# -gdb tcp::1234 \
 
 #-drive file=/home/gamboa/.picsimlab/mdump_ESP32_DevKitC_ESP32.efuse,if=none,format=raw,id=efuse \
 #-global driver=nvram.esp32.efuse,property=drive,value=efuse 
 

#{ "execute": "qmp_capabilities" }
#{ "execute": "system_reset" }
#{ "execute": "qom-get", "arguments": { "path": "/machine/soc/gpio", "property": "strap_mode" } }
#{ "execute": "qom-set", "arguments": { "path": "/machine/soc/gpio", "property": "strap_mode", "value": "0x0f" } 
