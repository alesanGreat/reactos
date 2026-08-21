add_executable(ramdisk_probe ramdisk_probe.c)
set_module_type(ramdisk_probe win32cui)
add_importlibs(ramdisk_probe msvcrt kernel32 ntdll)
add_cd_file(TARGET ramdisk_probe DESTINATION reactos/system32 FOR livecd)
