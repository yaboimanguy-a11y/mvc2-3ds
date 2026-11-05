# Nintendo 3DS (New 3DS and Old 3DS: ARM11 MPCore, ARMv6K, VFPv2, 32-bit).
#
#   cmake -S port -B build/port-3ds -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/port/cmake/3ds-clang.cmake
#   ninja -C build/port-3ds mvc2_3ds_sdcard   (-> build/port-3ds/sdcard/3ds/mvc2/)
#
# The game TUs are compiled by clang (the PS2 float rewrite, ps2fp-cc.pl,
# works on clang's LLVM IR); the link goes through devkitARM's gcc driver
# with 3dsx.specs, so crt0, newlib, libgcc and libctru are devkitPro's own.
# Both use the AAPCS hard-float ABI of devkitARM's 3ds_rules.
#
# Enum size: devkitARM (newlib, libctru, citro3d) uses short enums, and their
# structs depend on it (C3D_Tex packs a GPU_TEXCOLOR bitfield next to its
# size), so all host code gets -fshort-enums here (clang's armv6k-none-eabihf
# default is int-sized enums).  The game TUs get -fno-short-enums after it
# (port/CMakeLists.txt), because the EE's enums are int-sized.  No struct
# with an enum member crosses between the two.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(MVC2_3DS ON)

if(DEFINED ENV{DEVKITPRO})
  set(DEVKITPRO "$ENV{DEVKITPRO}")
else()
  set(DEVKITPRO "/opt/devkitpro")
endif()
set(DEVKITARM "${DEVKITPRO}/devkitARM")
if(NOT EXISTS "${DEVKITARM}/bin/arm-none-eabi-gcc")
  message(FATAL_ERROR "devkitARM not found under ${DEVKITPRO} (install the 3ds-dev group: dkp-pacman -S 3ds-dev)")
endif()

# clang with the ARM backend (Homebrew LLVM, or any LLVM; Apple's clang works
# for compiling but has no llvm-ar)
find_program(MVC2_3DS_CLANG NAMES clang
  HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin ENV LLVM_ROOT NO_DEFAULT_PATH)
if(NOT MVC2_3DS_CLANG)
  find_program(MVC2_3DS_CLANG NAMES clang REQUIRED)
endif()
get_filename_component(_llvm_bin "${MVC2_3DS_CLANG}" DIRECTORY)
set(CMAKE_C_COMPILER "${MVC2_3DS_CLANG}")
set(CMAKE_C_COMPILER_TARGET armv6k-none-eabihf)
find_program(CMAKE_AR NAMES llvm-ar HINTS "${_llvm_bin}" REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib HINTS "${_llvm_bin}" REQUIRED)
set(CMAKE_ASM_COMPILER "${MVC2_3DS_CLANG}")

set(MVC2_3DS_ARCH -mcpu=mpcore -mfloat-abi=hard -mfpu=vfpv2 -marm -mtp=soft)
string(JOIN " " _arch ${MVC2_3DS_ARCH})
set(CMAKE_C_FLAGS_INIT "${_arch} -D__3DS__ -fshort-enums -ffunction-sections -fdata-sections")

# newlib + libctru headers, for host code only (game TUs are -nostdlibinc and
# include no system header)
set(MVC2_3DS_SYSTEM_INCLUDES
  "${DEVKITARM}/arm-none-eabi/include"
  "${DEVKITPRO}/libctru/include")

# link: devkitARM gcc driver (crt0, newlib, libgcc, libsysbase via 3dsx.specs)
set(MVC2_3DS_GCC "${DEVKITARM}/bin/arm-none-eabi-gcc")
set(MVC2_3DS_LINK_ARCH "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft")
set(CMAKE_C_LINK_EXECUTABLE
  "${MVC2_3DS_GCC} -specs=3dsx.specs ${MVC2_3DS_LINK_ARCH} <LINK_FLAGS> <OBJECTS> -o <TARGET> -L${DEVKITPRO}/libctru/lib <LINK_LIBRARIES>")
set(MVC2_3DSXTOOL "${DEVKITPRO}/tools/bin/3dsxtool")
set(MVC2_SMDHTOOL "${DEVKITPRO}/tools/bin/smdhtool")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${DEVKITARM}/arm-none-eabi" "${DEVKITPRO}/libctru")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
