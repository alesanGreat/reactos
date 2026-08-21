add_executable(ramdisk_probe ramdisk_probe.c)
set_module_type(ramdisk_probe win32cui)
add_importlibs(ramdisk_probe msvcrt advapi32 kernel32 ntdll)
add_cd_file(TARGET ramdisk_probe DESTINATION reactos/system32 FOR livecd)

add_library(ramdisk_probe_drv MODULE ramdisk_probe_drv.c)
set_module_type(ramdisk_probe_drv kernelmodedriver)
add_importlibs(ramdisk_probe_drv ntoskrnl)
add_cd_file(TARGET ramdisk_probe_drv DESTINATION reactos/system32/drivers FOR livecd)
