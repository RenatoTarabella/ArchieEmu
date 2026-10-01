cmake_minimum_required(VERSION 3.16)
project(ARM C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /wd4100 /D_CRT_SECURE_NO_WARNINGS /utf-8)
else()
    add_compile_options(-Wall -Wextra -O2)
endif()

# Moduli: CPU, bus, HLE di RISC OS
add_library(armcore STATIC
    src/cpu/arm2.c
    src/cpu/arm2_disasm.c
    src/core/bus.c
    src/core/hostdir.c
    src/hle/riscos_swi.c
    src/riscos/module.c
    src/riscos/kernel.c
    src/riscos/hostfs.c
    src/riscos/vdu.c
    src/machine/machine.c
    src/frontend/png.c
)
target_include_directories(armcore PUBLIC src)

add_executable(armemu src/main.c)
target_link_libraries(armemu armcore)

# Test della CPU
add_executable(test_arm2 tests/test_arm2.c)
target_link_libraries(test_arm2 armcore)

# Motore per il confronto con Unicorn (tests/diff_unicorn.py)
add_executable(arm2_oracle tests/arm2_oracle.c)
target_link_libraries(arm2_oracle armcore)

enable_testing()
add_test(NAME cpu COMMAND test_arm2)

# Strumenti
add_executable(modinfo tools/modinfo.c)
target_link_libraries(modinfo armcore)

# Macchina con il BASIC: frontend da console
add_executable(armbasic src/frontend/cli_main.c)
target_link_libraries(armbasic armcore)
target_include_directories(armbasic PRIVATE src/frontend)

# Macchina con il BASIC: finestra Win32
if(WIN32)
    add_executable(armwin WIN32 src/frontend/win32_main.c res/armwin.rc)
    target_link_libraries(armwin armcore winmm)
    if(MSVC)
        set_target_properties(armwin PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()
endif()

# Test della macchina completa con il BASIC
add_executable(test_basic tests/test_basic.c)
target_link_libraries(test_basic armcore)
add_test(NAME basic COMMAND test_basic ${CMAKE_SOURCE_DIR}/third_party/riscos/BASIC ${CMAKE_BINARY_DIR}/test_disc)

# ---------------------------------------------------------------------------
# Macchina Archimedes a basso livello (MEMC, IOC, VIDC, tastiera, CMOS, floppy)
# ---------------------------------------------------------------------------
add_library(archiecore STATIC
    src/archie/memc.c
    src/archie/ioc.c
    src/archie/vidc.c
    src/archie/kbd.c
    src/archie/cmos.c
    src/archie/fdc.c
    src/archie/archie.c
    src/archie/podule.c
    src/archie/hostfs.c
)
target_include_directories(archiecore PUBLIC src src/archie)
target_link_libraries(archiecore armcore)

# test dei singoli chip: ognuno compila solo il proprio modulo
foreach(chip vidc kbd cmos fdc)
    add_executable(test_${chip} tests/test_${chip}.c src/archie/${chip}.c)
    target_include_directories(test_${chip} PRIVATE src/archie)
    add_test(NAME ${chip} COMMAND test_${chip})
endforeach()

add_executable(test_memc tests/test_memc.c src/archie/memc.c)
target_include_directories(test_memc PRIVATE src/archie)
target_link_libraries(test_memc armcore)
add_test(NAME memc COMMAND test_memc)

# Avvio della macchina Archimedes da riga di comando (diagnostica del boot)
add_executable(archie_boot tools/archie_boot.c)
target_link_libraries(archie_boot archiecore)
add_executable(romdis tools/romdis.c)
target_link_libraries(romdis armcore)

# Finestra della macchina Archimedes
if(WIN32)
    add_executable(archie WIN32 src/frontend/archie_win32.c src/frontend/archie_keys.c res/armwin.rc)
    target_include_directories(archie PRIVATE src/frontend)
    target_link_libraries(archie archiecore winmm shell32 comdlg32)
    if(MSVC)
        set_target_properties(archie PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()
endif()

# Le due finestre per macOS (Cocoa): bundle .app con icona e risorse
if(APPLE)
    enable_language(OBJC)
    set(MAC_ICON ${CMAKE_SOURCE_DIR}/res/armwin.icns)
    set_source_files_properties(${MAC_ICON} third_party/riscos/BASIC third_party/riscos/LICENSE
                                PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    file(GLOB MAC_DISC ${CMAKE_SOURCE_DIR}/disc/*)
    set_source_files_properties(${MAC_DISC} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/disc)

    add_executable(armwin MACOSX_BUNDLE src/frontend/armwin_mac.m src/frontend/mac_keys.c
                   ${MAC_ICON} third_party/riscos/BASIC third_party/riscos/LICENSE ${MAC_DISC})
    target_include_directories(armwin PRIVATE src/frontend)
    target_link_libraries(armwin armcore "-framework Cocoa" "-framework Carbon")
    set_target_properties(armwin PROPERTIES OUTPUT_NAME "ArchieEmu BASIC"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/res/mac/Info-basic.plist.in)

    add_executable(archie MACOSX_BUNDLE src/frontend/archie_mac.m src/frontend/archie_keys.c
                   src/frontend/mac_keys.c ${MAC_ICON})
    target_include_directories(archie PRIVATE src/frontend)
    target_link_libraries(archie archiecore "-framework Cocoa" "-framework Carbon" "-framework AudioToolbox")
    set_target_properties(archie PROPERTIES OUTPUT_NAME "ArchieEmu Archimedes"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/res/mac/Info-archie.plist.in)

    foreach(t armwin archie)
        target_compile_options(${t} PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
        set_target_properties(${t} PROPERTIES
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.1" MACOSX_BUNDLE_BUNDLE_VERSION "1.1")
    endforeach()
endif()

# Collaudo della tastiera con la disposizione spagnola di Windows
if(WIN32)
    add_executable(test_keys_es tests/test_keys_es.c src/frontend/archie_keys.c)
    target_include_directories(test_keys_es PRIVATE src/frontend)
    target_link_libraries(test_keys_es archiecore)
endif()
