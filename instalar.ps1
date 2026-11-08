<#
.SYNOPSIS
    Instala "Evolution: El juego de la vida inteligente" (edición española en CD,
    versión 1.2) para que funcione en Windows 10 y 11.

.DESCRIPTION
    Copia el juego desde el CD (o una imagen suya), parchea el ejecutable,
    instala la DLL de este repositorio, convierte la ayuda a HTML y, si el
    origen es una imagen BIN/CUE, pone la música en calidad de CD.
    No necesita permisos de administrador ni programas adicionales.

.PARAMETER Origen
    Unidad del CD (por ejemplo D:), carpeta con el contenido del CD, o una
    imagen .cue, .bin o .iso. Si no se indica, se pregunta.

.PARAMETER Destino
    Carpeta donde se instala el juego. Por defecto, C:\Juegos\Evolution.

.PARAMETER SinAccesoDirecto
    No crea el acceso directo en el escritorio.

.EXAMPLE
    .\instalar.ps1 -Origen D: -Destino "D:\Juegos\Evolution"

.EXAMPLE
    .\instalar.ps1 -Origen "C:\Imagenes\Evolution.cue"
#>
param(
    [string]$Origen,
    [string]$Destino,
    [switch]$SinAccesoDirecto
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot

# Huellas SHA-256 de la edición soportada y del resultado del parche
$HashExeOriginal = 'E9B7250C155212078AEC5F8A4D17DE4694B9ED73D3A2D174353A3ECAC1DF083A'
$HashExeParcheado = 'F9957894E6BC35F26050015EB92959804552AD7B876B64A9CAF43214101DFBF1'
# Pista de audio del CD que corresponde a cada Music\Track N.wav del juego
$PistaDeTrack = @{ 1 = 3; 2 = 4; 3 = 5; 4 = 6; 5 = 2 }

function Paso($texto) { Write-Host ''; Write-Host "==> $texto" -ForegroundColor Cyan }
function Ok($texto) { Write-Host "    $texto" -ForegroundColor Green }
function Aviso($texto) { Write-Host "    $texto" -ForegroundColor Yellow }
function Fallo($texto) { Write-Host ''; Write-Host "ERROR: $texto" -ForegroundColor Red; exit 1 }

Write-Host 'Evolution: El juego de la vida inteligente - Fix Windows 11' -ForegroundColor White

# --- Origen y destino ---------------------------------------------------------
if (-not $Origen) {
    Write-Host ''
    Write-Host 'Indica de dónde copiar el juego: la letra del CD (por ejemplo D:),'
    Write-Host 'una carpeta con el contenido del CD o una imagen .cue, .bin o .iso.'
    $Origen = (Read-Host 'Origen').Trim('"', ' ')
}
if (-not $Destino) {
    $porDefecto = 'C:\Juegos\Evolution'
    $Destino = (Read-Host "Carpeta de instalación [$porDefecto]").Trim('"', ' ')
    if (-not $Destino) { $Destino = $porDefecto }
}
if ($Origen -match '^[A-Za-z]:?$') { $Origen = $Origen.Substring(0, 1) + ':\' }
if (-not (Test-Path -LiteralPath $Origen)) { Fallo "No existe el origen '$Origen'." }
$Destino = [IO.Path]::GetFullPath($Destino)

$imagen = $null
$carpetaCd = $null
$item = Get-Item -LiteralPath $Origen
if (-not $item.PSIsContainer) {
    if ($item.Extension -notin '.cue', '.bin', '.iso') { Fallo 'El origen debe ser una unidad, una carpeta o una imagen .cue, .bin o .iso.' }
    $imagen = $item.FullName
} elseif (Test-Path -LiteralPath (Join-Path $item.FullName 'EVOLUTION\EVOLUTION.EXE')) {
    $carpetaCd = Join-Path $item.FullName 'EVOLUTION'
} elseif (Test-Path -LiteralPath (Join-Path $item.FullName 'EVOLUTION.EXE')) {
    $carpetaCd = $item.FullName
} else {
    Fallo "No encuentro el juego en '$Origen' (falta la carpeta EVOLUTION con EVOLUTION.EXE)."
}

if (Test-Path -LiteralPath (Join-Path $Destino 'EVOLUTION.EXE')) {
    Fallo "Ya hay un juego instalado en '$Destino'. Elige otra carpeta o bórrala antes."
}

# --- Herramientas ---------------------------------------------------------------
Paso 'Preparando las herramientas'
$dll = Join-Path $repo 'bin\SH33W32.DLL'
if (-not (Test-Path -LiteralPath $dll)) { Fallo "Falta $dll. Descarga el repositorio completo." }
Add-Type -Path (Join-Path $repo 'herramientas\Herramientas.cs') -ReferencedAssemblies System.Drawing
Ok 'Listas'

# --- Copia ----------------------------------------------------------------------
Paso "Copiando el juego a $Destino"
New-Item -ItemType Directory -Force -Path $Destino | Out-Null
if ($imagen) {
    $n = [EvoFix.Cd]::CopyFolder($imagen, 'EVOLUTION', $Destino)
    Ok "$n archivos copiados desde la imagen"
} else {
    Copy-Item -Path (Join-Path $carpetaCd '*') -Destination $Destino -Recurse -Force
    Ok 'Archivos copiados desde el CD'
}
# Los archivos de un CD vienen como solo lectura
Get-ChildItem -LiteralPath $Destino -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }

# --- Comprobación de la edición -----------------------------------------------------
Paso 'Comprobando la edición del juego'
$exe = Join-Path $Destino 'EVOLUTION.EXE'
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($hash -ne $HashExeOriginal) {
    Remove-Item -LiteralPath $Destino -Recurse -Force
    Fallo ('Este EVOLUTION.EXE no es el de la edición española 1.2 en CD, ' +
           'que es la única para la que sirve este arreglo. No se ha instalado nada.')
}
Ok 'Edición española 1.2 reconocida'

# --- Parches --------------------------------------------------------------------------
Paso 'Aplicando el arreglo'
Copy-Item -LiteralPath $exe -Destination "$exe.orig"
$sh33 = Join-Path $Destino 'SH33W32.DLL'
Copy-Item -LiteralPath $sh33 -Destination "$sh33.orig"
# El juego lee su configuración de HKEY_LOCAL_MACHINE: se cambia a
# HKEY_CURRENT_USER (push 80000002h -> push 80000001h) para no necesitar
# permisos de administrador.
$bytes = [IO.File]::ReadAllBytes($exe)
foreach ($off in 0x4F441, 0x5FF5B) {
    if ($bytes[$off] -ne 0x68 -or $bytes[$off + 1] -ne 0x02) { Fallo 'El ejecutable no tiene los bytes esperados.' }
    $bytes[$off + 1] = 0x01
}
[IO.File]::WriteAllBytes($exe, $bytes)
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $HashExeParcheado) { Fallo 'El ejecutable parcheado no es el esperado.' }
Ok 'Ejecutable parcheado (copia del original en EVOLUTION.EXE.orig)'
Copy-Item -LiteralPath $dll -Destination $sh33 -Force
Ok 'DLL del arreglo instalada (copia de la original en SH33W32.DLL.orig)'

# --- Ayuda ------------------------------------------------------------------------------
Paso 'Convirtiendo la ayuda a HTML'
$resumen = [EvoFix.Ayuda]::Convert((Join-Path $Destino 'EVOLUTION.HLP'), (Join-Path $Destino 'AYUDA'), (Join-Path $Destino 'EVOLUTION.CNT'))
Ok "Ayuda creada en AYUDA\index.html ($resumen)"

# --- Música en calidad de CD ----------------------------------------------------------------
Paso 'Música en calidad de CD'
if ($imagen -and [IO.Path]::GetExtension($imagen) -eq '.cue') {
    $tmp = Join-Path $env:TEMP ('evolution-audio-' + [guid]::NewGuid())
    try {
        $n = [EvoFix.Cd]::ExtractAudio($imagen, $tmp, 'Pista ')
        $todas = $true
        foreach ($t in $PistaDeTrack.Keys) { if (-not (Test-Path (Join-Path $tmp ('Pista {0:00}.wav' -f $PistaDeTrack[$t])))) { $todas = $false } }
        if ($n -ge 5 -and $todas) {
            foreach ($t in $PistaDeTrack.Keys) {
                Copy-Item -LiteralPath (Join-Path $tmp ('Pista {0:00}.wav' -f $PistaDeTrack[$t])) `
                          -Destination (Join-Path $Destino "MUSIC\TRACK $t.WAV") -Force
            }
            Ok 'Música del CD instalada (44,1 kHz, 16 bits)'
        } else {
            Aviso 'La imagen no tiene las 5 pistas de audio: se mantiene la música original del juego.'
        }
    } finally {
        if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    }
} else {
    Aviso 'Solo se puede sacar de una imagen .cue/.bin: se mantiene la música original del juego.'
}

# --- Acceso directo ----------------------------------------------------------------------------
if (-not $SinAccesoDirecto) {
    Paso 'Creando el acceso directo en el escritorio'
    $sh = New-Object -ComObject WScript.Shell
    $lnk = $sh.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Evolution.lnk'))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $Destino
    $lnk.Save()
    Ok 'Evolution.lnk'
}

Write-Host ''
Write-Host "Listo. Para jugar, abre $exe" -ForegroundColor Green
Write-Host 'Opcional: para los vídeos remasterizados en HD, ejecuta videos-hd.ps1 (ver README).'
