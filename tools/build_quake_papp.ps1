<#
.SYNOPSIS
    Build Quake (WinQuake) as a PSRAM .papp for RetroESP32-P4.

.DESCRIPTION
    Compiles the WinQuake engine files + PSRAM-app shim files,
    links against newlib + compiler-rt, wraps heap functions to route
    through app_services_t, produces quake.papp.
#>
param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Resolve-IdfEnv.ps1')
Initialize-IdfEnv
$BUILD = "$ROOT\build_papp\quake"
$OUT   = "$ROOT\firmware\quake.papp"

# Compiler
$CC  = "riscv32-esp-elf-gcc"
$CXX = "riscv32-esp-elf-g++"
$OBJ = "riscv32-esp-elf-objcopy"

$ARCH_FLAGS = @(
    "-march=rv32imafc_zicsr_zifencei",
    "-mabi=ilp32f",
    "-mcmodel=medany"
)

$CFLAGS = @(
    "-std=gnu99",
    "-DPAPP_APP_SIDE=1",
    "-DIRAM_ATTR=",
    "-DESP32_QUAKE=1",
    "-DESP_PLATFORM=1",
    "-DCONFIG_IDF_TARGET_ESP32P4=1",
    "-DCONFIG_IDF_TARGET_ARCH_RISCV=1",
    "-Os",
    "-fcommon",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wall",
    "-Wno-format",
    "-Wno-format-overflow",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-maybe-uninitialized",
    "-Wno-implicit-function-declaration",
    "-Wno-pointer-sign",
    "-Wno-int-conversion",
    "-Wno-incompatible-pointer-types",
    "-Wno-dangling-pointer",
    "-Wno-dangling-else",
    "-Wno-array-bounds",
    "-Wno-address",
    "-Wno-restrict",
    "-Wno-builtin-declaration-mismatch",
    "-Wno-char-subscripts",
    "-Wno-sign-compare",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-missing-field-initializers",
    "-Wno-trigraphs"
)

# The MP3 decoder is C++ source, but it is intentionally built without the
# C++ runtime.  The decoder itself uses only the C ABI, fixed-point math, and
# malloc/free supplied by the PAPP linker wrappers.
$CXXFLAGS = @(
    "-std=gnu++17",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-use-cxa-atexit",
    "-DPAPP_APP_SIDE=1",
    "-Os",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-Wno-sign-compare"
)

# Include paths — compat headers FIRST to override ESP-IDF/retro-go
$QUAKE_DIR = "$ROOT\components\quake"
$INCLUDES = @(
    "-I$ROOT\apps\psram_quake\compat",
    "-I$ROOT\components\psram_app_loader\include",
    "-I$QUAKE_DIR\winquake",
    "-I$QUAKE_DIR\esp32quake",
    "-I$QUAKE_DIR"
)

# Linker script and flags
$LD_SCRIPT = "$ROOT\tools\psram_app.ld"
$WRAP_FLAGS = @(
    "-Wl,--wrap=malloc",
    "-Wl,--wrap=free",
    "-Wl,--wrap=calloc",
    "-Wl,--wrap=realloc",
    "-Wl,--wrap=_malloc_r",
    "-Wl,--wrap=_free_r",
    "-Wl,--wrap=_calloc_r",
    "-Wl,--wrap=_realloc_r",
    "-Wl,--wrap=__retarget_lock_init",
    "-Wl,--wrap=__retarget_lock_init_recursive",
    "-Wl,--wrap=__retarget_lock_close",
    "-Wl,--wrap=__retarget_lock_close_recursive",
    "-Wl,--wrap=__retarget_lock_acquire",
    "-Wl,--wrap=__retarget_lock_try_acquire",
    "-Wl,--wrap=__retarget_lock_acquire_recursive",
    "-Wl,--wrap=__retarget_lock_try_acquire_recursive",
    "-Wl,--wrap=__retarget_lock_release",
    "-Wl,--wrap=__retarget_lock_release_recursive"
)
$LDFLAGS = @(
    "-nostartfiles",
    "-nodefaultlibs",
    "-T$LD_SCRIPT",
    "-Wl,--gc-sections",
    "-Wl,--entry=app_entry",
    "-Wl,--no-relax",
    "-Wl,--allow-multiple-definition"
) + $WRAP_FLAGS + @("-lc", "-lgcc", "-lm")

# ── Source files ─────────────────────────────────────────────────────

# WinQuake engine (from CMakeLists.txt — same set)
$WINQUAKE_SRCS = @(
    "chase.c","cmd.c","common.c","console.c","crc.c","cvar.c","draw.c",
    "host.c","host_cmd.c","keys.c","mathlib.c","menu.c","model.c",
    "nonintel.c","screen.c","sbar.c","zone.c","view.c","wad.c","world.c",
    "cl_demo.c","cl_input.c","cl_main.c","cl_parse.c","cl_tent.c",
    "d_edge.c","d_fill.c","d_init.c","d_modech.c","d_part.c",
    "d_polyse.c","d_scan.c","d_sky.c","d_sprite.c","d_surf.c",
    "d_vars.c","d_zpoint.c",
    "net_loop.c","net_main.c","net_vcr.c",
    "pr_cmds.c","pr_edict.c","pr_exec.c",
    "r_aclip.c","r_alias.c","r_bsp.c","r_light.c","r_draw.c",
    "r_efrag.c","r_edge.c","r_misc.c","r_main.c","r_sky.c",
    "r_sprite.c","r_surf.c","r_part.c","r_vars.c",
    "sv_main.c","sv_phys.c","sv_move.c","sv_user.c",
    "cd_null.c","net_none.c","snd_mem.c"
)

# PSRAM app shims (in apps/psram_quake/)
$PAPP_DIR = "$ROOT\apps\psram_quake"
$MP3_DIR = "$PAPP_DIR\third_party\micro-mp3\opencore-mp3dec"
$PAPP_SRCS = @(
    "papp_main.c",
    "papp_vid.c",
    "papp_snd.c",
    "papp_input.c",
    "papp_sys.c",
    "papp_rg_stubs.c",
    "papp_syscalls.c"
)

$MP3_SRCS = @(
    "pvmp3_alias_reduction.cpp",
    "pvmp3_crc.cpp",
    "pvmp3_dct_16.cpp",
    "pvmp3_dct_6.cpp",
    "pvmp3_dct_9.cpp",
    "pvmp3_decode_header.cpp",
    "pvmp3_decode_huff_cw.cpp",
    "pvmp3_dequantize_sample.cpp",
    "pvmp3_equalizer.cpp",
    "pvmp3_framedecoder.cpp",
    "pvmp3_get_main_data_size.cpp",
    "pvmp3_get_scale_factors.cpp",
    "pvmp3_get_side_info.cpp",
    "pvmp3_getbits.cpp",
    "pvmp3_huffman_decoding.cpp",
    "pvmp3_huffman_parsing.cpp",
    "pvmp3_imdct_synth.cpp",
    "pvmp3_mdct_18.cpp",
    "pvmp3_mdct_6.cpp",
    "pvmp3_mpeg2_get_scale_data.cpp",
    "pvmp3_mpeg2_get_scale_factors.cpp",
    "pvmp3_mpeg2_stereo_proc.cpp",
    "pvmp3_normalize.cpp",
    "pvmp3_poly_phase_synthesis.cpp",
    "pvmp3_polyphase_filter_window.cpp",
    "pvmp3_reorder.cpp",
    "pvmp3_seek_synch.cpp",
    "pvmp3_stereo_proc.cpp",
    "pvmp3_tables.cpp"
)

# ── Build ────────────────────────────────────────────────────────────

if ($Clean -and (Test-Path $BUILD)) {
    Remove-Item -Recurse -Force $BUILD
}
New-Item -ItemType Directory -Force -Path $BUILD | Out-Null

$ALL_OBJS = @()
$errors = 0

# Compile WinQuake engine sources
Write-Host "Compiling WinQuake engine ($($WINQUAKE_SRCS.Count) files)..." -ForegroundColor Cyan
foreach ($src in $WINQUAKE_SRCS) {
    $obj = "$BUILD\$($src -replace '\.c$','.o')"
    $srcpath = "$QUAKE_DIR\winquake\$src"
    $args = $ARCH_FLAGS + $CFLAGS + $INCLUDES + @("-c", "-o", $obj, $srcpath)
    $proc = Start-Process -FilePath $CC -ArgumentList $args -NoNewWindow -Wait -PassThru -RedirectStandardError "$BUILD\$src.err"
    if ($proc.ExitCode -ne 0) {
        Write-Host "  FAIL: $src" -ForegroundColor Red
        Get-Content "$BUILD\$src.err" | Select-Object -First 10
        $errors++
    }
    $ALL_OBJS += $obj
}

# Compile PSRAM app shim sources
Write-Host "Compiling PSRAM app shims ($($PAPP_SRCS.Count) files)..." -ForegroundColor Cyan
foreach ($src in $PAPP_SRCS) {
    $obj = "$BUILD\$($src -replace '\.c$','.o')"
    $srcpath = "$PAPP_DIR\$src"
    $args = $ARCH_FLAGS + $CFLAGS + $INCLUDES + @("-c", "-o", $obj, $srcpath)
    $proc = Start-Process -FilePath $CC -ArgumentList $args -NoNewWindow -Wait -PassThru -RedirectStandardError "$BUILD\$src.err"
    if ($proc.ExitCode -ne 0) {
        Write-Host "  FAIL: $src" -ForegroundColor Red
        Get-Content "$BUILD\$src.err" | Select-Object -First 10
        $errors++
    }
    $ALL_OBJS += $obj
}

# Compile the PAPP MP3 wrapper and the portable OpenCore decoder.
Write-Host "Compiling PAPP MP3 decoder ($($MP3_SRCS.Count + 1) C++ files)..." -ForegroundColor Cyan
$mp3_include = $INCLUDES + @("-I$MP3_DIR", "-I$MP3_DIR\oscl")
$mp3_wrapper = "papp_mp3.cpp"
$wrapper_obj = "$BUILD\mp3_papp_mp3.o"
$wrapper_args = $ARCH_FLAGS + $CXXFLAGS + $mp3_include + @("-c", "-o", $wrapper_obj, "$PAPP_DIR\$mp3_wrapper")
$proc = Start-Process -FilePath $CXX -ArgumentList $wrapper_args -NoNewWindow -Wait -PassThru -RedirectStandardError "$BUILD\mp3_papp_mp3.err"
if ($proc.ExitCode -ne 0) {
    Write-Host "  FAIL: $mp3_wrapper" -ForegroundColor Red
    Get-Content "$BUILD\mp3_papp_mp3.err" | Select-Object -First 20
    $errors++
}
$ALL_OBJS += $wrapper_obj

foreach ($src in $MP3_SRCS) {
    $obj = "$BUILD\mp3_$($src -replace '\.cpp$','.o')"
    $srcpath = "$MP3_DIR\$src"
    $args = $ARCH_FLAGS + $CXXFLAGS + $mp3_include + @("-c", "-o", $obj, $srcpath)
    $proc = Start-Process -FilePath $CXX -ArgumentList $args -NoNewWindow -Wait -PassThru -RedirectStandardError "$BUILD\mp3_$src.err"
    if ($proc.ExitCode -ne 0) {
        Write-Host "  FAIL: $src" -ForegroundColor Red
        Get-Content "$BUILD\mp3_$src.err" | Select-Object -First 10
        $errors++
    }
    $ALL_OBJS += $obj
}

if ($errors -gt 0) {
    Write-Host "`n$errors file(s) failed to compile. Aborting." -ForegroundColor Red
    exit 1
}

Write-Host "Compiled $($ALL_OBJS.Count) object files." -ForegroundColor Green

# Link
Write-Host "Linking..." -ForegroundColor Cyan
$ELF = "$BUILD\quake.elf"
$link_args = $ARCH_FLAGS + $ALL_OBJS + $LDFLAGS + @("-o", $ELF)
$proc = Start-Process -FilePath $CXX -ArgumentList $link_args -NoNewWindow -Wait -PassThru -RedirectStandardError "$BUILD\link.err"
if ($proc.ExitCode -ne 0) {
    Write-Host "  Link FAILED:" -ForegroundColor Red
    Get-Content "$BUILD\link.err" | Select-Object -First 30
    exit 1
}
Write-Host "  Linked: $ELF" -ForegroundColor Green

# Extract flat binary
$BIN = "$BUILD\quake.bin"
& riscv32-esp-elf-objcopy -O binary $ELF $BIN
if (-not (Test-Path $BIN)) {
    Write-Host "  objcopy FAILED" -ForegroundColor Red
    exit 1
}
$binSize = (Get-Item $BIN).Length
Write-Host "  Binary: $BIN ($binSize bytes)" -ForegroundColor Green

# Extract BSS size from ELF
$NM = "riscv32-esp-elf-nm"
$nmOut = & $NM $ELF 2>$null
$bssEndLine = $nmOut | Where-Object { $_ -match '\b_bss_end\b' }
if ($bssEndLine) {
    $bssEnd = [Convert]::ToUInt64(($bssEndLine.Trim() -split '\s+')[0], 16)
    $linkBase = 0x4A000000
    $binaryEndAddr = $linkBase + $binSize
    $bssToZero = [Math]::Max(0, $bssEnd - $binaryEndAddr)
    Write-Host "  _bss_end=0x$($bssEnd.ToString('X')), binary covers 0x$($linkBase.ToString('X'))-0x$($binaryEndAddr.ToString('X'))" -ForegroundColor Gray
    Write-Host "  BSS to zero: $bssToZero bytes" -ForegroundColor Gray
} else {
    $bssToZero = 0
    Write-Host "  Warning: _bss_end not found in ELF; bss_size=0" -ForegroundColor Yellow
}

# Pack .papp
Write-Host "Packing .papp..." -ForegroundColor Cyan
python "$ROOT\tools\pack_papp.py" $BIN $OUT --bss-size $bssToZero
Write-Host "  Output: $OUT ($((Get-Item $OUT).Length) bytes)" -ForegroundColor Green

Write-Host "`nBuild complete! Copy $OUT to /sd/roms/papp/ on the SD card." -ForegroundColor Green
