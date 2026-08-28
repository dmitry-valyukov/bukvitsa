# Настраивает и собирает дерево из обычной оболочки.
#
# Пресетам нужны Ninja и cl.exe в PATH — то есть запуск из Developer Command
# Prompt. Скрипт поднимает это окружение сам: выполняет vcvars64.bat в дочернем
# cmd, читает выставленные им переменные обратно и применяет к текущему
# процессу. Ровно так же устроен tools/build.ps1 в wxl, и это не случайно:
# Буквица собирает wxl как подпроект, поэтому требования к тулчейну у них общие
# (`import std;`, значит достаточно свежий MSVC и генератор Ninja).
#
# Использование:
#   tools\build.ps1                  # настроить (если нужно) и собрать Debug
#   tools\build.ps1 -Config Release
#   tools\build.ps1 -Configure       # принудительно перенастроить
#   tools\build.ps1 -Target fb3_tests
#   tools\build.ps1 -Configure -Define BUKVITSA_WXL_ROOT=M:/wxl

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string]$Preset = 'x64',
    [string]$Target,
    [switch]$Configure,

    # Записи кэша для шага настройки, без -D. Остаются в CMakeCache.txt, так что
    # следующий запуск без них сохраняет то, что было задано.
    [string[]]$Define
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Import-VisualStudioEnvironment {
    if ($env:VSCMD_VER) { return }  # уже внутри developer prompt

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe не найден: $vswhere" }

    # -prerelease, чтобы засчитывалась установка Insiders/preview: тулчейн
    # должен быть достаточно новым для `import std;`, а такие обычно там.
    $install = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $install) { throw 'Не найдена установка Visual Studio с тулчейном C++.' }

    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat не найден: $vcvars" }

    & "$env:ComSpec" /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

Import-VisualStudioEnvironment

$buildDir = Join-Path $root "build\$Preset"
if ($Configure -or $Define -or -not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    $configureArgs = @('--preset', $Preset, '-S', $root)
    foreach ($d in $Define) { $configureArgs += "-D$d" }
    cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Каталог сборки по имени, а не `--build --preset`: пресет сборки ищется в
# CMakePresets.json того каталога, где стоит оболочка, — поэтому вызов этого
# скрипта по полному пути откуда-то ещё (а работа в git worktree ровно это и
# значит) настроил бы одно дерево, а собрал другое. Формы равносильны: пресеты
# здесь несут только каталог сборки и конфигурацию.
$buildArgs = @('--build', $buildDir, '--config', $Config)
if ($Target) { $buildArgs += @('--target', $Target) }
cmake @buildArgs
exit $LASTEXITCODE
