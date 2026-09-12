# Запускает приложение Буквицы, нажимает в нём клавиши и фотографирует окно.
#
# Зачем отдельный скрипт вместо «запусти и посмотри»: окна Буквицы созданы с
# WS_EX_NOREDIRECTIONBITMAP, всё содержимое рисует композитор DWM, и оно не
# попадает ни в GDI, ни в PrintWindow — даже с PW_RENDERFULLCONTENT кадр
# получается пустым. Единственный способ увидеть такое окно — снять область
# экрана, где оно лежит. Отсюда два следствия: окно поднимается на передний
# план перед каждым снимком, и поверх него ничего не должно лежать.
#
# Использование:
#   tools\drive.ps1 -Do "wait 1500; shot start.png"
#   tools\drive.ps1 -Exe build\x64\Reader\Debug\Reader.exe `
#                   -Arguments "FB3\testdata\nightmare_example.fb3" `
#
# Команды (-Do — одна строка, команды через ';'):
#   wait <мс>         пауза
#   shot [файл]       PNG окна (по умолчанию shot.png, каталог задаёт -ShotDir)
#   key <клавиши>     SendKeys: {PGDN} {PGUP} {HOME} {END} {ESC} ^{ADD} и т.п.
#   press <клавиши>   то же, но без поднятия окна и без паузы после: для пачек
#                     нажатий, где счёт идёт на десятки миллисекунд (листание
#                     внахлёст) — окно уже поднято предыдущей командой
#   type <текст>      набор текста в фокус
#   click <x> <y>     щелчок в точке от левого верхнего угла окна, в пикселях
#   rclick <x> <y>    то же правой кнопкой
#   size              прямоугольник окна: x;y;ширина;высота
#   resize <ш> <в>    задать окну размер (из максимизации выводит); снимок сразу
#                     за ним ловит просвет растяжки — то, что нарисовано до
#                     того, как остров XAML успел переверстаться
#   title             заголовок окна

[CmdletBinding()]
param(
    [string]$Exe = 'build\x64\Reader\Debug\Reader.exe',
    [string]$Arguments = '',
    [string]$Do = 'wait 1500; shot shot.png',
    [string]$ShotDir = '.',
    [switch]$Keep,
    [int]$LaunchTimeoutSec = 30
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Exe))     { $Exe = Join-Path $root $Exe }
if (-not [System.IO.Path]::IsPathRooted($ShotDir)) { $ShotDir = Join-Path $root $ShotDir }
if (-not (Test-Path $Exe)) { throw "Не найден исполняемый файл: $Exe" }
New-Item -ItemType Directory -Force $ShotDir | Out-Null

$startArgs = @{ FilePath = $Exe; PassThru = $true }
if ($Arguments) {
    # Путь к книге может быть относительным — приложение запускается из корня дерева.
    $startArgs.ArgumentList = $Arguments
    $startArgs.WorkingDirectory = $root
}
$process = Start-Process @startArgs

# MainWindowHandle появляется не сразу и, что важнее, появляется раньше, чем
# окно готово показать содержимое: композитору нужен первый кадр.
$deadline = [DateTime]::UtcNow.AddSeconds($LaunchTimeoutSec)
while ($process.MainWindowHandle -eq [IntPtr]::Zero) {
    if ($process.HasExited) { throw "Приложение завершилось до появления окна (код $($process.ExitCode))." }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Окно не появилось за отведённое время.' }
    Start-Sleep -Milliseconds 100
    $process.Refresh()
}
$hwnd = $process.MainWindowHandle

# Приложение, которого не стало, выглядит для остальных команд как окно
# нулевого размера -- и жаловались они именно на размер, пряча настоящую
# новость. Спрашиваем сам процесс: код возврата разом отвечает, вышло оно само
# (0 или свой код) или упало (0xC0000005 и подобные).
function Assert-Alive {
    $process.Refresh()
    if (-not $process.HasExited) { return }

    $code = $process.ExitCode
    throw ("Приложение завершилось: код {0} (0x{1:X8})." -f $code, $code)
}

function Get-WindowRect {
    Assert-Alive
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($hwnd, [ref]$r)
    return @{ X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

# Окно, которое сейчас за приложение отвечает: само окно или модальный диалог
# поверх него. Пока диалог открыт, главное окно ввод не принимает -- поднимать
# и слать клавиши надо диалогу, иначе набранное уходит в никуда (проверено на
# диалоге открытия книги).
function Get-ActiveWindow {
    $popup = [Win]::GetLastActivePopup($hwnd)
    if ($popup -ne [IntPtr]::Zero -and $popup -ne $hwnd -and [Win]::IsWindowVisible($popup)) {
        return $popup
    }
    return $hwnd
}

function Move-Pointer([int]$x, [int]$y) {
    # MOVE|ABSOLUTE, а не SetCursorPos, и по той же причине, что в перетаскивании
    # ниже: SetCursorPos переставляет курсор, но событий указателя для
    # современного стека ввода не рождает. WinUI такого перемещения не видит,
    # а не увидев его — не считает последующее нажатие своим: кнопка не
    # подсвечивается и Click не приходит.
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $ax = [uint32]([double]$x * 65535 / ($bounds.Width - 1))
    $ay = [uint32]([double]$y * 65535 / ($bounds.Height - 1))
    [Win]::mouse_event(0x8001, $ax, $ay, 0, [UIntPtr]::Zero)
}

function Raise-Window {
    [void][Win]::ShowWindow($hwnd, 5)   # SW_SHOW

    # SetForegroundWindow не всесилен: Windows не отдаёт передний план окну
    # чужого процесса, если пользователь в это время работает в своём. А снимок
    # берётся с экрана, и чужое окно поверх испортит его молча. Поэтому окно
    # объявляется поверх всех — этому запрет не мешает — и возвращается обратно
    # сразу после снимка, чтобы не мешать самому пользователю.
    [void][Win]::SetWindowPos($hwnd, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0013)  # HWND_TOPMOST
    [void][Win]::SetForegroundWindow((Get-ActiveWindow))
    Start-Sleep -Milliseconds 150
}

function Lower-Window {
    [void][Win]::SetWindowPos($hwnd, [IntPtr]::new(-2), 0, 0, 0, 0, 0x0013)  # HWND_NOTOPMOST
}

function Save-Shot([string]$file) {
    Raise-Window
    $rect = Get-WindowRect
    if ($rect.W -le 0 -or $rect.H -le 0) { throw 'У окна нулевой размер.' }

    $bitmap = New-Object System.Drawing.Bitmap $rect.W, $rect.H
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.X, $rect.Y, 0, 0, (New-Object System.Drawing.Size $rect.W, $rect.H))
    $graphics.Dispose()

    $path = Join-Path $ShotDir $file
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    Write-Output "shot: $path ($($rect.W)x$($rect.H))"
}

try {
    foreach ($command in ($Do -split ';')) {
        $parts = $command.Trim() -split '\s+', 2
        $name = $parts[0].ToLower()
        $rest = if ($parts.Count -gt 1) { $parts[1] } else { '' }

        switch ($name) {
            ''      { }
            'wait'  { Start-Sleep -Milliseconds ([int]$rest) }
            'shot'  { Save-Shot ($(if ($rest) { $rest } else { 'shot.png' })) }
            'title' {
                Assert-Alive
                $sb = New-Object System.Text.StringBuilder 512
                [void][Win]::GetWindowTextW($hwnd, $sb, $sb.Capacity)
                Write-Output "title: $($sb.ToString())"
            }
            'size'  { $r = Get-WindowRect; Write-Output "size: $($r.X);$($r.Y);$($r.W);$($r.H)" }
            'resize' {
                $wh = $rest -split '\s+'
                [void][Win]::ShowWindow($hwnd, 9)   # SW_RESTORE: максимизированное окно размера не примет
                # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE: только размер.
                [void][Win]::SetWindowPos($hwnd, [IntPtr]::Zero, 0, 0, [int]$wh[0], [int]$wh[1], 0x0016)
            }
            'key'   { Raise-Window; [System.Windows.Forms.SendKeys]::SendWait($rest); Start-Sleep -Milliseconds 120 }
            'press' { [System.Windows.Forms.SendKeys]::SendWait($rest) }
            'type'  { Raise-Window; [System.Windows.Forms.SendKeys]::SendWait($rest); Start-Sleep -Milliseconds 120 }
            'click' {
                Raise-Window
                $xy = $rest -split '\s+'
                $r = Get-WindowRect
                Move-Pointer ($r.X + [int]$xy[0]) ($r.Y + [int]$xy[1])
                Start-Sleep -Milliseconds 120
                [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
                Start-Sleep -Milliseconds 150
            }
            'drag' {
                # Перетаскивание: нажать в первой точке, доехать до второй,
                # отпустить. Дорога проходится шагами, потому что приложение
                # ведёт перетаскиваемое по событиям движения, а не по концам.
                # Движение — mouse_event MOVE|ABSOLUTE, а не SetCursorPos: тот
                # переставляет курсор, но событий указателя для современного
                # стека ввода не рождает, и WinUI перетаскивания не видит.
                Raise-Window
                $xy = $rest -split '\s+'
                $r = Get-WindowRect
                $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
                $x1 = $r.X + [int]$xy[0]; $y1 = $r.Y + [int]$xy[1]
                $x2 = $r.X + [int]$xy[2]; $y2 = $r.Y + [int]$xy[3]
                $move = {
                    param([double]$x, [double]$y)
                    $ax = [uint32]($x * 65535 / ($bounds.Width - 1))
                    $ay = [uint32]($y * 65535 / ($bounds.Height - 1))
                    [Win]::mouse_event(0x8001, $ax, $ay, 0, [UIntPtr]::Zero)   # MOVE|ABSOLUTE
                }
                & $move $x1 $y1
                Start-Sleep -Milliseconds 80
                [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                Start-Sleep -Milliseconds 80
                $steps = 16
                for ($i = 1; $i -le $steps; $i++) {
                    & $move ($x1 + ($x2 - $x1) * $i / $steps) ($y1 + ($y2 - $y1) * $i / $steps)
                    Start-Sleep -Milliseconds 25
                }
                [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
                Start-Sleep -Milliseconds 150
            }
            'rclick' {
                # Правая кнопка — не роскошь драйвера: ею читалка открывает
                # свой ящик, и проверить это иначе нечем.
                Raise-Window
                $xy = $rest -split '\s+'
                $r = Get-WindowRect
                Move-Pointer ($r.X + [int]$xy[0]) ($r.Y + [int]$xy[1])
                Start-Sleep -Milliseconds 120
                [Win]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)   # RIGHTDOWN
                [Win]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)   # RIGHTUP
                Start-Sleep -Milliseconds 150
            }
            default { throw "Неизвестная команда: $name" }
        }
    }
}
finally {
    # Приложение могло закрыться само — «Выйти из читалки» именно это и
    # делает, — и тогда любому из вызовов ниже не с кем разговаривать. Это
    # не ошибка прогона.
    try {
        if (-not $process.HasExited) { Lower-Window }

        if (-not $Keep -and -not $process.HasExited) {
            # Сначала вежливо: у приложения есть что сохранить (место чтения).
            [void]$process.CloseMainWindow()
            if (-not $process.WaitForExit(3000)) { $process.Kill() }
        }
    } catch {}
}
