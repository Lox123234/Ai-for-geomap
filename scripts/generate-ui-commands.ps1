<#
.SYNOPSIS
    Writes docs/UI_COMMANDS.md from src/ui/commands/commands.json (the single list of commands
    behind the menus, the toolbar and the shortcuts). Run after changing commands.json; the
    workspace test checks that every command id is documented.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$json = Get-Content (Join-Path $root 'src\ui\commands\commands.json') -Raw -Encoding UTF8 | ConvertFrom-Json

$location = @{}
foreach ($menu in $json.menus) {
    $title = $menu.title.Replace('&', '')
    foreach ($id in $menu.items) { if ($id -ne '-') { $location[$id] = @($location[$id]) + "меню «$title»" | Where-Object { $_ } } }
}
foreach ($group in $json.toolbar) {
    foreach ($id in $group.items) { $location[$id] = @($location[$id]) + "панель «$($group.group)»" | Where-Object { $_ } }
}

function Cell([string]$text) { if ([string]::IsNullOrEmpty($text)) { '—' } else { $text.Replace('|', '\|') } }
function StatusText($c) {
    switch ($c.status) {
        'ready' { 'готова' }
        'planned' { "запланирована, этап $($c.stage)" }
        'engine' { 'не поддерживается движком' }
        default { $c.status }
    }
}

$ready = @($json.commands | Where-Object status -eq 'ready').Count
$planned = @($json.commands | Where-Object status -eq 'planned').Count
$engine = @($json.commands | Where-Object status -eq 'engine').Count
$out = New-Object System.Collections.Generic.List[string]
$out.Add('# Команды интерфейса GeoMap AI')
$out.Add('')
$out.Add('Документ создан скриптом `scripts/generate-ui-commands.ps1` из `src/ui/commands/commands.json` — единого списка команд, из которого строятся меню, панель инструментов и горячие клавиши. Не редактируйте его вручную.')
$out.Add('')
$out.Add("Всего команд: $($json.commands.Count): готовых — $ready, запланированных — $planned, не поддерживаемых движком — $engine.")
$out.Add('')
$out.Add('Правила:')
$out.Add('')
$out.Add('- Пункт меню, кнопка панели и горячая клавиша одной команды вызывают одну функцию `runCommand(id)` (`qml/Main.qml`), доступность — `commandEnabled(id)`.')
$out.Add('- Запланированная команда видна, но отключена; в меню показан этап, в подсказке — причина. Команда «не поддерживается движком» — шаг, который OpenDroneMap не выполняет отдельно.')
$out.Add('- Горячие клавиши можно изменить: «Правка → Настройки интерфейса» или «Справка → Горячие клавиши». Конфликты отклоняются.')
$out.Add('- Тесты: `GeoMapAIWorkspaceTests` проверяет целостность списка, иконки, клавиши и этот документ; `GeoMapAIUiTests` загружает настоящее окно и проверяет, что у каждой готовой команды есть обработчик, а запланированные отключены.')
$out.Add('')
$out.Add('## Панель инструментов')
$out.Add('')
$out.Add('| Группа | Кнопки слева направо |')
$out.Add('| --- | --- |')
foreach ($group in $json.toolbar) {
    $names = $group.items | ForEach-Object { $id = $_; $c = $json.commands | Where-Object id -eq $id; if ($c.status -eq 'ready') { $c.title.Replace('…', '') } else { "$($c.title.Replace('…', '')) *(этап $($c.stage))*" } }
    $out.Add("| $($group.group) | $($names -join ', ') |")
}
foreach ($menu in $json.menus) {
    $title = $menu.title.Replace('&', '')
    $out.Add('')
    $out.Add("## Меню «$title»")
    $out.Add('')
    $out.Add('| Команда | Идентификатор | Расположение | Иконка | Клавиши | Состояние | Действие или причина | Доступна, когда | Обработчик | Тест |')
    $out.Add('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    foreach ($id in $menu.items) {
        if ($id -eq '-') { continue }
        $c = $json.commands | Where-Object id -eq $id
        $what = if ($c.status -eq 'ready') { $c.action } else { $c.reason }
        $test = if ($c.status -eq 'ready') { 'UiTests: обработчик, доступность' } else { 'UiTests: отключена с причиной' }
        $out.Add("| $(Cell $c.title) | ``$($c.id)`` | $(Cell (($location[$id] | Select-Object -Unique) -join ', ')) | $(Cell $c.icon) | $(Cell $c.shortcut) | $(StatusText $c) | $(Cell $what) | $(Cell $c.enabledWhen) | $(Cell $c.handler) | $test |")
    }
}
$target = Join-Path $root 'docs\UI_COMMANDS.md'
[IO.File]::WriteAllText($target, (($out -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
Write-Host "Written: $target ($($json.commands.Count) commands)"
