# Nexus-Office - instalador portable (Windows)
# Descarga un CPython completo y autonomo (python-build-standalone) dentro de .\venv,
# instala las dependencias y configura los extras segun el hardware detectado.
# Todo queda dentro de la carpeta del proyecto: se puede mover/copiar a otro equipo.
# Las dependencias se instalan siempre como wheels binarias: nada se compila ($env:NEXUS_ALLOW_BUILD=1 lo permite).
#   -FetchWheels  descarga antes a .\wheelhouse (y el Python a .\runtime) todo lo que se instala, para
#                 reinstalar o copiar la carpeta a otro equipo y usar -Offline
#   -Offline      no usa internet: todo sale de .\runtime y .\wheelhouse (falla si falta algo)
#   -Clean        al final, sin preguntar: borra lo que este equipo no usa y guarda lo usado para reinstalar
#   -NoClean      al final no limpia (por defecto lo pregunta; ver venv\python.exe -m nexus cleanup --help)
#   -Oficina ID   configura la instalacion como la oficina de una pyme (herreria, caldereria, reformas...;
#                 lista: venv\python.exe -m nexus oficina). Tambien con $env:NEXUS_OFICINA
#   -Servicio     instala Nexus-Office como servicio (tarea programada): como administrador arranca con el equipo
#   -Red          el servicio escucha en la red local (0.0.0.0) para entrar desde moviles y otros equipos
#   -Modelos M    auto (por defecto): evalua el hardware y descarga el modelo de chat, documentos y voz adecuados
#                 si hay internet; todos: tambien el de imagen; no: solo muestra la recomendacion
param(
    [string]$PythonVersion = "3.11",
    [string]$PythonSource = "",      # carpeta o .tar.gz local de python-build-standalone (opcional)
    [switch]$SkipExtras,             # no instalar aceleradores/extras segun hardware
    [switch]$Force,                  # reinstalar el runtime aunque exista
    [switch]$FetchWheels,            # preparar wheelhouse\ y runtime\ para instalar sin internet
    [switch]$Offline,                # instalar solo desde runtime\ y wheelhouse\
    [switch]$Clean,                  # limpiar al final sin preguntar
    [switch]$NoClean,                # no limpiar al final
    [string]$Oficina = $env:NEXUS_OFICINA,  # perfil de oficina de pyme (python -m nexus oficina)
    [switch]$Servicio,               # arranque automatico como servicio
    [switch]$Red,                    # el servicio escucha en la red local
    [ValidateSet("auto", "todos", "no")][string]$Modelos = $(if ($env:NEXUS_MODELOS) { $env:NEXUS_MODELOS } else { "auto" })
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Venv  = Join-Path $Root "venv"
$Py    = Join-Path $Venv "python.exe"
$Cache = Join-Path $Root ".cache"
$Wheels = Join-Path $Root "wheelhouse"
$Vendor = Join-Path $Root "vendor\wheels"
if ($env:NEXUS_OFFLINE -and $env:NEXUS_OFFLINE -ne "0") { $Offline = $true }
if ($Offline -and $FetchWheels) { throw "-FetchWheels necesita internet (quita -Offline)" }

function Say($msg, $color = "Cyan") { Write-Host "[Nexus-Office] $msg" -ForegroundColor $color }
New-Item -ItemType Directory -Force $Cache | Out-Null

if ((Test-Path $Py) -and -not $Force) {
    Say "Runtime portable ya presente en venv\ (usa -Force para reinstalar)"
} else {
    if (Test-Path $Venv) { Remove-Item -Recurse -Force $Venv }
    New-Item -ItemType Directory -Force $Cache | Out-Null
    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "aarch64" } else { "x86_64" }
    $archive = $null

    $bundled = Get-ChildItem (Join-Path $Root "runtime") -Filter "cpython-*-$arch-pc-windows-msvc*.tar.gz" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $bundled -and $arch -eq "aarch64") {
        # paquete offline de Windows ARM: CPython x64 (Windows 11 ARM lo emula) con sus wheels win_amd64
        $bundled = Get-ChildItem (Join-Path $Root "runtime") -Filter "cpython-*-x86_64-pc-windows-msvc*.tar.gz" -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $PythonSource -and $bundled) { $PythonSource = $bundled.FullName; Say "Usando runtime incluido (offline): $($bundled.Name)" }
    if ($PythonSource -and (Test-Path $PythonSource -PathType Container)) {
        Say "Copiando runtime local desde $PythonSource"
        Copy-Item -Recurse $PythonSource $Venv
    } else {
        if ($PythonSource) {
            $archive = $PythonSource
        } else {
            if ($Offline) { throw "Modo offline: falta runtime\cpython-*-$arch-pc-windows-msvc-*.tar.gz (preparalo con install.bat -FetchWheels en un equipo con internet)" }
            $pattern = "^cpython-$([regex]::Escape($PythonVersion))\.\d+\+\d+-$arch-pc-windows-msvc-install_only(_stripped)?\.tar\.gz$"
            $url = $null
            try {
                Say "Buscando la ultima version de python-build-standalone ($PythonVersion, $arch)..."
                $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest" -Headers @{ "User-Agent" = "nexus-office" }
                $asset = $rel.assets | Where-Object { $_.name -match $pattern } | Sort-Object { $_.name -notmatch "stripped" } | Select-Object -First 1
                if ($asset) { $url = $asset.browser_download_url }
            } catch { Say "GitHub API no disponible: $($_.Exception.Message)" "Yellow" }
            if (-not $url) {
                $url = "https://github.com/astral-sh/python-build-standalone/releases/download/20261009/cpython-3.11.17+20261009-$arch-pc-windows-msvc-install_only_stripped.tar.gz"
                Say "Usando version fijada de respaldo" "Yellow"
            }
            $archive = Join-Path $Cache "python-standalone.tar.gz"
            if ($FetchWheels) {
                New-Item -ItemType Directory -Force (Join-Path $Root "runtime") | Out-Null
                $archive = Join-Path $Root ("runtime\" + [uri]::UnescapeDataString(($url -split "/")[-1]))
            }
            Say "Descargando $url"
            Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
        }
        $extract = Join-Path $Cache "extract"
        if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
        New-Item -ItemType Directory -Force $extract | Out-Null
        Say "Extrayendo runtime..."
        tar -xzf $archive -C $extract
        if ($LASTEXITCODE -ne 0) { throw "tar fallo al extraer $archive" }
        Move-Item (Join-Path $extract "python") $Venv
        Remove-Item -Recurse -Force $extract
    }
    Set-Content -Path (Join-Path $Venv "NEXUS_PORTABLE.txt") -Value "Runtime CPython autonomo de Nexus-Office. No depende de ninguna instalacion del sistema." -Encoding UTF8
    Say "Runtime instalado: $(& $Py --version)" "Green"
}

$env:NEXUS_OFFLINE = if ($Offline) { "1" } else { "0" }   # tambien lo respetan `nexus setup` y los extras (nexus\wheelhouse.py)
if ($FetchWheels) {
    Say "Descargando wheels (base + voz + NPU) y el runtime para instalar sin internet..."
    Push-Location $Root
    try { & $Py -m nexus wheels --runtime | Out-Host } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { Say "Aviso: el wheelhouse no quedo completo (lo que falte se bajara de internet)" "Yellow" }
}

Say "Instalando dependencias base..."
$req = Join-Path $Root "requirements.txt"
$pipArgs = @("--disable-pip-version-check", "--no-warn-script-location")
if ($env:NEXUS_ALLOW_BUILD -eq "1") { $pipArgs += "--prefer-binary" } else { $pipArgs += "--only-binary=:all:" }
if (Test-Path $Vendor) { $pipArgs += @("--find-links", $Vendor) }
$installed = $false
if (Get-ChildItem $Wheels -Filter *.whl -ErrorAction SilentlyContinue) {
    Say "Usando wheels locales de wheelhouse\ (sin internet)"
    & $Py -m pip install @pipArgs --no-index --find-links $Wheels -r $req | Out-Host
    if ($LASTEXITCODE -eq 0) { $installed = $true }
    elseif ($Offline) { throw "Faltan wheels en wheelhouse\ para este equipo. Preparalas con: install.bat -FetchWheels" }
    else { Say "wheelhouse\ incompleto para este equipo: se completa desde internet" "Yellow"; $pipArgs += @("--find-links", $Wheels) }
} elseif ($Offline) {
    throw "Modo offline sin wheelhouse\. Preparalo con: install.bat -FetchWheels"
}
if (-not $installed) {
    & $Py -m pip install --disable-pip-version-check --no-warn-script-location --upgrade pip | Out-Host
    & $Py -m pip install @pipArgs -r $req | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "No se pudieron instalar las dependencias: falta la wheel de algun paquete para este equipo y no se compila. Si tienes Visual Studio Build Tools, reintenta con `$env:NEXUS_ALLOW_BUILD=1" }
}

Push-Location $Root
try {
    Say "Detectando hardware y generando perfil optimo..."
    & $Py -m nexus detect | Out-File -Encoding utf8 (Join-Path $Cache "hardware-report.json")
    if ($Oficina) {
        Say "Configurando la oficina: perfil $Oficina"
        & $Py -m nexus oficina $Oficina
        if ($LASTEXITCODE -ne 0) { Say "Perfil de oficina no aplicado (lista: venv\python.exe -m nexus oficina)" }
    }
    if (-not $SkipExtras) {
        Say "Instalando extras recomendados para este hardware (puede tardar)..."
        & $Py -m nexus setup --auto
    }
} finally { Pop-Location }

# Limpieza: el paquete offline trae wheels y runtimes para cualquier GPU/NPU; aqui se quita lo que no usa este equipo
$hasPkg = (Get-ChildItem $Wheels -Filter *.whl -ErrorAction SilentlyContinue) -or (Test-Path (Join-Path $Root "bundled\runtimes"))
if (-not $NoClean -and $env:NEXUS_CLEAN -ne "no" -and $hasPkg) {
    Push-Location $Root
    try {
        if ($Clean -or $env:NEXUS_CLEAN -eq "yes") { & $Py -m nexus cleanup --yes }
        elseif ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) { & $Py -m nexus cleanup }
        else { Say "Para quitar lo que este equipo no usa: venv\python.exe -m nexus cleanup" }
    } finally { Pop-Location }
}

Push-Location $Root
try {
    if ($Modelos -eq "no") { Say "Modelo de IA recomendado para este equipo:"; & $Py -m nexus modelos }
    else {
        Say "Evaluando el hardware y eligiendo el modelo de IA..."
        if ($Modelos -eq "todos") { & $Py -m nexus modelos --descargar --todos } else { & $Py -m nexus modelos --descargar }
        if ($LASTEXITCODE -ne 0) { Say "Descarga de modelos incompleta: reintenta con venv\python.exe -m nexus modelos --descargar" "Yellow" }
    }
    if ($Servicio -or $env:NEXUS_SERVICIO -eq "1") {
        Say "Instalando Nexus-Office como servicio..."
        $svc = @("instalar"); if ($Red) { $svc += "--red" }
        & $Py -m nexus servicio @svc
        if ($LASTEXITCODE -ne 0) { Say "No se pudo instalar el servicio (ver docs\SERVICIO.md)" "Yellow" }
    }
} finally { Pop-Location }

Say "Instalacion completada. Ejecuta start.bat para arrancar Nexus-Office." "Green"
