<#
.SYNOPSIS
    RadioS Windows derlemesi / build: yapılandırma, derleme, testler, runtime\, kurulum ve taşınabilir ZIP.

.DESCRIPTION
    Tek giriş noktası / single entry point for the Windows 10/11 x64 build (MSYS2 UCRT64 in C:\msys64).

      1. Ortam / environment: yalnız sabitlenmiş MSYS2 araçları (C:\msys64\ucrt64\bin\cmake.exe, ctest.exe,
         ninja.exe, gcc.exe); C:\MinGW ve başka derleyiciler PATH'ten çıkarılır, derleyiciyi etkileyen
         değişkenler (CFLAGS, CPATH, PYTHONHOME ...) temizlenir.
      2. MSYS2 paket kilidi / package lock (packaging\windows\msys2-packages.lock): kurulu paketler kilitten
         farklıysa durur (-AllowMsys2Drift yalnız geliştirme içindir).
      3. Release: LGPL FFmpeg (packaging\windows\cache\ffmpeg-9.0.2-lgpl-radios, DECISIONS D10) gerekir;
         -BuildFfmpeg yoksa packaging\windows\build-ffmpeg.ps1 ile üretir.
      4. -Test: Release'te windows-release-tests, Debug'da windows-debug(-gui) ön ayarıyla derler ve ctest
         çalıştırır (önce runtime\python oluşturulur/doğrulanır).
      5. CMake ön ayarıyla (CMakePresets.json) yapılandırma ve derleme.
      6. runtime\ (D4 geliştirme ortamı) radios_runtime hedefiyle güvenli eşitlenir; RadioS açıksa durur.
      7. -Installer / -Portable: dist\RadioS-<sürüm>-windows-x64-setup.exe / .zip (+ .sha256), yalnız Release.

    Herhangi bir adım başarısız olursa betik sıfırdan farklı çıkış koduyla biter / any failing step ends the
    script with a non-zero exit code. Çıkış kodları / exit codes: 0 başarı / success, 1 hata / failure.

.PARAMETER Configuration
    Release (varsayılan / default) ya da Debug.
.PARAMETER Test
    Testleri derleyip ctest ile çalıştırır / build and run the tests (CTest).
.PARAMETER GuiTests
    Debug'da GUI testlerini de çalıştırır (etkileşimli masaüstü; yayın bilgisayarında değil). Release testleri
    GUI testlerini her zaman içerir / Debug: include the GUI tests (Release tests always include them).
.PARAMETER Installer
    NSIS kurulum programını üretir / build the NSIS installer (Release).
.PARAMETER Portable
    Taşınabilir, belirlenimci ZIP üretir / build the deterministic portable ZIP (Release).
.PARAMETER NoRuntime
    runtime\ klasörünü güncellemez / do not refresh runtime\.
.PARAMETER Clean
    Yapılandırmayı sıfırdan yapar (cmake --fresh) / fresh configure.
.PARAMETER BuildFfmpeg
    LGPL FFmpeg önbellekte yoksa build-ffmpeg.ps1 ile derler / build the LGPL FFmpeg when it is missing.
.PARAMETER AllowMsys2Drift
    MSYS2 kilit farkını yalnız uyarı yapar (geliştirme); kurulum paketi yine de kilitten bağımsız üretilir.
    Development only: a lock mismatch becomes a warning, also for the package stage.
.PARAMETER UpdateMsys2Lock
    Kilidi kurulu MSYS2 sürümleriyle günceller ve çıkar (bilinçli 'pacman -Syu' ve başarılı testlerden sonra).
    Record the installed MSYS2 versions in the lock and exit.
.PARAMETER Jobs
    Paralel derleme işi sayısı / parallel build jobs (varsayılan: Ninja'nın seçimi / Ninja's default).

.EXAMPLE
    build-windows.cmd
.EXAMPLE
    build-windows.cmd -Test -Installer -Portable
.EXAMPLE
    build-windows.cmd -Configuration Debug -Test
.EXAMPLE
    build-windows.cmd -UpdateMsys2Lock
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Test,
    [switch] $GuiTests,
    [switch] $Installer,
    [switch] $Portable,
    [switch] $NoRuntime,
    [switch] $Clean,
    [switch] $BuildFfmpeg,
    [switch] $AllowMsys2Drift,
    [switch] $UpdateMsys2Lock,
    [ValidateRange(0, 256)]
    [int] $Jobs = 0
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Pinned toolchain location (CMakePresets.json uses the same paths).
$Msys2Root = 'C:\msys64'
$Ucrt64 = Join-Path $Msys2Root 'ucrt64'
$Ucrt64Bin = Join-Path $Ucrt64 'bin'
$UsrBin = Join-Path $Msys2Root 'usr\bin'
$Ucrt64Cmake = ($Ucrt64 -replace '\\', '/')
$FfmpegDirName = 'ffmpeg-9.0.2-lgpl-radios'

class RadiosBuildError : System.Exception {
    RadiosBuildError([string] $message) : base($message) {}
}

function New-BuildError([string] $Turkish, [string] $English) {
    return [RadiosBuildError]::new("$Turkish`n  $English")
}

function Write-Step([string] $Turkish, [string] $English) {
    Write-Host ''
    Write-Host "==> $Turkish / $English" -ForegroundColor Cyan
}

function Write-Note([string] $Turkish, [string] $English) {
    Write-Host "    $Turkish / $English"
}

function Write-BuildWarning([string] $Turkish, [string] $English) {
    Write-Host "UYARI / WARNING: $Turkish / $English" -ForegroundColor Yellow
}

function Format-Argument([string] $Value) {
    if ($Value -match '[\s"]') { return '"' + ($Value -replace '"', '\"') + '"' }
    return $Value
}

# Runs a native program; any non-zero exit code stops the build. stdout/stderr go straight to the
# console (never redirected: Windows PowerShell 5.1 turns redirected native stderr into errors).
function Invoke-Native([string] $Description, [string] $FilePath, [string[]] $Arguments) {
    $shown = ($Arguments | ForEach-Object { Format-Argument $_ }) -join ' '
    Write-Host "    > $(Format-Argument $FilePath) $shown" -ForegroundColor DarkGray
    & $FilePath @Arguments
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw (New-BuildError "$Description başarısız (çıkış kodu $code)." "$Description failed (exit code $code).")
    }
}

function Assert-File([string] $Path, [string] $Hint) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw (New-BuildError "Bulunamadı: $Path ($Hint)" "Not found: $Path ($Hint)")
    }
}

# --- environment ---------------------------------------------------------------------------
function Initialize-BuildEnvironment {
    Write-Step 'Derleme ortamı hazırlanıyor' 'Preparing the build environment'
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
        throw (New-BuildError 'RadioS yalnız 64 bit Windows 10/11 üzerinde derlenir.' 'RadioS builds on 64-bit Windows 10/11 only.')
    }
    if ($Root -match '[^\x20-\x7E]' -or $Root.Contains(';')) {
        throw (New-BuildError "Proje yolu ASCII dışı karakter ya da ';' içeriyor: $Root. binutils/windres bu yolları açamaz; projeyi kısa bir ASCII yola taşıyın." `
                              "The project path contains non-ASCII characters or ';': $Root. binutils/windres cannot open such paths; move the project to a short ASCII path.")
    }
    if ($Root.Length -gt 60) {
        Write-BuildWarning "Proje yolu uzun ($($Root.Length) karakter); nesne dosyası yolları MAX_PATH sınırına yaklaşabilir." `
                           "The project path is long ($($Root.Length) characters); object paths may approach MAX_PATH."
    }
    foreach ($tool in 'gcc.exe', 'windres.exe', 'cmake.exe', 'ctest.exe', 'ninja.exe', 'pkg-config.exe', 'objdump.exe', 'python.exe') {
        Assert-File (Join-Path $Ucrt64Bin $tool) 'MSYS2 UCRT64 (README.md: pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,...})'
    }

    # PATH: MSYS2 UCRT64 first; drop C:\MinGW (WinLibs GCC with incompatible runtime DLLs) and any
    # other directory that carries a C compiler.
    $kept = New-Object System.Collections.Generic.List[string]
    $removed = New-Object System.Collections.Generic.List[string]
    foreach ($entry in ($env:PATH -split ';')) {
        $item = $entry.Trim().Trim('"')
        if ($item -eq '') { continue }
        $full = $item.TrimEnd('\')
        if ($full -ieq $Ucrt64Bin -or $full -ieq $UsrBin) { continue }
        $foreign = $full -match '^[A-Za-z]:\\MinGW(\\|$)'
        if (-not $foreign) {
            foreach ($compiler in 'gcc.exe', 'cc.exe', 'clang.exe', 'cl.exe') {
                if (Test-Path -LiteralPath (Join-Path $item $compiler) -PathType Leaf) { $foreign = $true; break }
            }
        }
        if ($foreign) { $removed.Add($item) } else { $kept.Add($item) }
    }
    $env:PATH = (@($Ucrt64Bin, $UsrBin) + $kept.ToArray()) -join ';'
    foreach ($item in $removed) {
        Write-Note "PATH'ten çıkarıldı: $item" "removed from PATH: $item"
    }

    # Variables that silently change compiler, linker, pkg-config or Python behaviour.
    $cleared = @()
    foreach ($name in 'CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CPATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH',
                      'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_SYSROOT_DIR',
                      'CMAKE_GENERATOR', 'CMAKE_TOOLCHAIN_FILE', 'CMAKE_PREFIX_PATH', 'PYTHONHOME', 'PYTHONPATH',
                      'PYTHONSTARTUP', 'PYTHONUSERBASE') {
        if (Test-Path -LiteralPath "Env:$name") {
            Remove-Item -LiteralPath "Env:$name"
            $cleared += $name
        }
    }
    if ($cleared.Count -gt 0) {
        Write-Note "temizlenen ortam değişkenleri: $($cleared -join ', ')" "cleared environment variables: $($cleared -join ', ')"
    }
    $env:PKG_CONFIG_LIBDIR = "$Ucrt64Cmake/lib/pkgconfig;$Ucrt64Cmake/share/pkgconfig"
    $env:PYTHONUTF8 = '1'
    $env:PYTHONDONTWRITEBYTECODE = '1'

    $script:Cmake = Join-Path $Ucrt64Bin 'cmake.exe'
    $script:Ctest = Join-Path $Ucrt64Bin 'ctest.exe'
    $versionLine = (& $script:Cmake --version | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0) {
        throw (New-BuildError "$($script:Cmake) çalışmadı." "$($script:Cmake) did not run.")
    }
    Write-Note "CMake: $versionLine ($($script:Cmake))" "pinned MSYS2 CMake"
}

function Invoke-Msys2Lock([string] $Mode) {
    $policy = 'REQUIRE'
    if ($AllowMsys2Drift) { $policy = 'WARN' }
    Invoke-Native "MSYS2 paket kilidi ($Mode) / MSYS2 package lock ($Mode)" $script:Cmake @(
        "-DMODE=$Mode", "-DPOLICY=$policy", "-DRADIOS_MSYS2_PREFIX=$Ucrt64Cmake",
        "-DLOCK=$(Join-Path $Root 'packaging\windows\msys2-packages.lock')",
        '-P', (Join-Path $Root 'packaging\windows\cmake\msys2-lock.cmake'))
}

function Assert-Ffmpeg {
    $prefix = Join-Path $Root "packaging\windows\cache\$FfmpegDirName"
    $marker = Join-Path $prefix 'lib\pkgconfig\libavcodec.pc'
    $dlls = @()
    if (Test-Path -LiteralPath (Join-Path $prefix 'bin') -PathType Container) {
        $dlls = @(Get-ChildItem -LiteralPath (Join-Path $prefix 'bin') -Filter 'avcodec-*.dll' -File)
    }
    if ((Test-Path -LiteralPath $marker -PathType Leaf) -and $dlls.Count -gt 0) {
        Write-Note "LGPL FFmpeg: $prefix" 'DECISIONS D10'
        return
    }
    if (-not $BuildFfmpeg) {
        throw (New-BuildError "LGPL FFmpeg yok: $prefix. Release ve kurulum paketi bunu ister (DECISIONS D10). '-BuildFfmpeg' ekleyin ya da packaging\windows\build-ffmpeg.ps1 çalıştırın." `
                              "The LGPL FFmpeg is missing: $prefix. Release builds and packages require it (DECISIONS D10). Add '-BuildFfmpeg' or run packaging\windows\build-ffmpeg.ps1.")
    }
    $script = Join-Path $Root 'packaging\windows\build-ffmpeg.ps1'
    Assert-File $script 'ffmpeg-minimal'
    Write-Step 'LGPL FFmpeg derleniyor (uzun sürer)' 'Building the LGPL FFmpeg (takes a while)'
    $shell = (Get-Process -Id $PID).Path
    Invoke-Native 'build-ffmpeg.ps1' $shell @('-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script)
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        throw (New-BuildError "build-ffmpeg.ps1 bitti ama $marker oluşmadı." "build-ffmpeg.ps1 finished but $marker was not created.")
    }
}

function Assert-RuntimeNotRunning {
    $runtimeBin = Join-Path $Root 'runtime\bin'
    $running = @(Get-Process -Name 'RadioS' -ErrorAction SilentlyContinue | Where-Object {
        $path = $null
        try { $path = $_.Path } catch { $path = $null }
        $path -and $path.StartsWith($runtimeBin + '\', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($running.Count -gt 0) {
        throw (New-BuildError "RadioS çalışıyor ($runtimeBin\RadioS.exe, PID $($running[0].Id)); runtime\ güncellenemez. RadioS'u kapatın ya da -NoRuntime kullanın." `
                              "RadioS is running ($runtimeBin\RadioS.exe, PID $($running[0].Id)); runtime\ cannot be updated. Close RadioS or use -NoRuntime.")
    }
}

function Get-BinaryDir([string] $Preset) {
    switch ($Preset) {
        'windows-debug' { return Join-Path $Root 'build' }
        'windows-debug-gui' { return Join-Path $Root 'build\gui' }
        'windows-release' { return Join-Path $Root 'build\release' }
        'windows-release-tests' { return Join-Path $Root 'build\release-tests' }
    }
    throw (New-BuildError "Bilinmeyen ön ayar: $Preset" "Unknown preset: $Preset")
}

function Invoke-Configure([string] $Preset) {
    Write-Step "Yapılandırma ($Preset)" "Configure ($Preset)"
    $arguments = @('--preset', $Preset)
    if ($Clean) { $arguments += '--fresh' }
    $policy = 'REQUIRE'
    if ($AllowMsys2Drift) { $policy = 'WARN' }
    $arguments += "-DRADIOS_MSYS2_LOCK_POLICY=$policy"
    Invoke-Native "cmake --preset $Preset" $script:Cmake $arguments
}

function Invoke-Build([string] $Preset, [string[]] $Targets) {
    $label = $Preset
    if ($Targets.Count -gt 0) { $label = "$Preset : $($Targets -join ', ')" }
    Write-Step "Derleme ($label)" "Build ($label)"
    $arguments = @('--build', '--preset', $Preset)
    if ($Jobs -gt 0) { $arguments += @('--parallel', "$Jobs") }
    if ($Targets.Count -gt 0) { $arguments += '--target'; $arguments += $Targets }
    Invoke-Native "cmake --build --preset $Preset" $script:Cmake $arguments
}

function Install-PythonRuntime([string] $Preset) {
    Write-Step 'runtime\python ve share\zoneinfo (oluştur ya da doğrula)' 'runtime\python and share\zoneinfo (create or verify)'
    Invoke-Native 'cmake --install --component python' $script:Cmake @(
        '--install', (Get-BinaryDir $Preset), '--component', 'python', '--prefix', (Join-Path $Root 'runtime'))
}

function Invoke-Tests([string] $Preset) {
    Install-PythonRuntime $Preset
    Write-Step "Testler ($Preset)" "Tests ($Preset)"
    Invoke-Native "ctest --preset $Preset" $script:Ctest @('--preset', $Preset)
}

function Show-Artifacts([string] $Version) {
    $dist = Join-Path $Root 'dist'
    $names = @()
    if ($Installer) { $names += "RadioS-$Version-windows-x64-setup.exe" }
    if ($Portable) { $names += "RadioS-$Version-windows-x64.zip" }
    foreach ($name in $names) {
        $file = Join-Path $dist $name
        $sum = "$file.sha256"
        Assert-File $file 'radios_installer / radios_portable'
        Assert-File $sum 'radios_installer / radios_portable'
        $expected = ((Get-Content -LiteralPath $sum -TotalCount 1) -split '\s+')[0].ToLowerInvariant()
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            throw (New-BuildError "$name SHA-256 dosyasıyla uyuşmuyor." "$name does not match its .sha256 file.")
        }
        Write-Note "$file" "sha256 $actual"
    }
}

function Get-ProjectVersion {
    $text = [IO.File]::ReadAllText((Join-Path $Root 'CMakeLists.txt'))
    $match = [regex]::Match($text, 'project\(RadioS\s+VERSION\s+(\d+\.\d+\.\d+)\s')
    if (-not $match.Success) {
        throw (New-BuildError 'CMakeLists.txt içinde project(RadioS VERSION x.y.z ...) bulunamadı.' 'project(RadioS VERSION x.y.z ...) not found in CMakeLists.txt.')
    }
    return $match.Groups[1].Value
}

# --- main ----------------------------------------------------------------------------------
$Root = $PSScriptRoot
$exitCode = 0
$savedOutputEncoding = [Console]::OutputEncoding
$startedAt = Get-Date
try {
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    Set-Location -LiteralPath $Root
    Initialize-BuildEnvironment

    if ($UpdateMsys2Lock) {
        Write-Step 'MSYS2 paket kilidi güncelleniyor' 'Updating the MSYS2 package lock'
        Invoke-Native 'msys2-lock update' $script:Cmake @(
            '-DMODE=update', "-DRADIOS_MSYS2_PREFIX=$Ucrt64Cmake",
            "-DLOCK=$(Join-Path $Root 'packaging\windows\msys2-packages.lock')",
            '-P', (Join-Path $Root 'packaging\windows\cmake\msys2-lock.cmake'))
        Write-Host ''
        Write-Host 'Kilit güncellendi; değişikliği inceleyip kaydedin (git diff packaging/windows/msys2-packages.lock).' -ForegroundColor Green
        Write-Host 'Lock updated; review and commit the change.' -ForegroundColor Green
        exit 0
    }

    if (($Installer -or $Portable) -and $Configuration -ne 'Release') {
        throw (New-BuildError 'Kurulum programı ve ZIP yalnız Release derlemesinden üretilir.' 'The installer and the ZIP are built from a Release build only.')
    }
    if ($Installer) {
        Assert-File (Join-Path $Ucrt64Bin 'makensis.exe') 'pacman -S mingw-w64-ucrt-x86_64-nsis'
    }
    if (-not $NoRuntime) { Assert-RuntimeNotRunning }

    Write-Step 'MSYS2 paketleri doğrulanıyor' 'Verifying the MSYS2 packages'
    Invoke-Msys2Lock 'check'

    $version = Get-ProjectVersion
    Write-Note "RadioS $version, $Configuration" 'version, configuration'

    if ($Configuration -eq 'Release') {
        Assert-Ffmpeg
        if ($Test) {
            Invoke-Configure 'windows-release-tests'
            Invoke-Build 'windows-release-tests' @()
            Invoke-Tests 'windows-release-tests'
        }
        Invoke-Configure 'windows-release'
        Invoke-Build 'windows-release' @()
        if (-not $NoRuntime) {
            Invoke-Build 'windows-release' @('radios_runtime')
        }
        $packageTargets = @()
        if ($Installer) { $packageTargets += 'radios_installer' }
        if ($Portable) { $packageTargets += 'radios_portable' }
        if ($packageTargets.Count -gt 0) {
            Invoke-Build 'windows-release' $packageTargets
            Show-Artifacts $version
        }
    } else {
        $preset = 'windows-debug'
        if ($GuiTests) { $preset = 'windows-debug-gui' }
        Invoke-Configure $preset
        Invoke-Build $preset @()
        if ($Test) { Invoke-Tests $preset }
        if (-not $NoRuntime) {
            Invoke-Build $preset @('radios_runtime')
        }
    }

    $elapsed = [int]((Get-Date) - $startedAt).TotalSeconds
    Write-Host ''
    Write-Host "RadioS derlemesi tamamlandı ($elapsed s) / build finished." -ForegroundColor Green
    if (-not $NoRuntime) {
        Write-Note "Çalıştır: $(Join-Path $Root 'runtime\bin\RadioS.exe')" 'run'
    }
} catch [RadiosBuildError] {
    [Console]::Error.WriteLine('')
    [Console]::Error.WriteLine("RadioS: HATA / ERROR: $($_.Exception.Message)")
    $exitCode = 1
} catch {
    [Console]::Error.WriteLine('')
    [Console]::Error.WriteLine("RadioS: beklenmeyen hata / unexpected error: $($_.Exception.Message)")
    [Console]::Error.WriteLine("  $($_.InvocationInfo.PositionMessage)")
    $exitCode = 1
} finally {
    [Console]::OutputEncoding = $savedOutputEncoding
}
exit $exitCode
