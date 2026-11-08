<#
.SYNOPSIS
    Genera los vídeos remasterizados (1280x960) de Evolution a partir de los
    vídeos originales del juego instalado.

.DESCRIPTION
    Para cada vídeo Movies\*_30.SMK:
      1. FFmpeg extrae los fotogramas con un filtrado suave (hqdn3d + deband)
         que quita el tramado y las bandas de los 256 colores originales.
      2. Real-ESRGAN (modelo realesr-animevideov3) los amplía x4 con la tarjeta
         gráfica (Vulkan).
      3. FFmpeg los une en un MP4 H.264 a 30 fps con el sonido original.
    El resultado se guarda en Movies\HD\<NOMBRE>.mp4, donde la DLL del arreglo
    lo reproduce en lugar del vídeo original. Los vídeos que ya existen se
    saltan, así que se puede interrumpir y volver a lanzar.

    Si no se indican -FFmpeg y -RealEsrgan, se descargan de sus páginas
    oficiales (unos 150 MB) en %LOCALAPPDATA%\EvolutionFix.

.PARAMETER Juego
    Carpeta del juego instalado. Por defecto, C:\Juegos\Evolution.

.PARAMETER FFmpeg
    Ruta a ffmpeg.exe, si ya lo tienes.

.PARAMETER RealEsrgan
    Ruta a realesrgan-ncnn-vulkan.exe (con su carpeta models al lado), si ya lo tienes.

.PARAMETER Videos
    Solo estos vídeos (por ejemplo: -Videos INTRO,ICE). Por defecto, todos.
#>
param(
    [string]$Juego = 'C:\Juegos\Evolution',
    [string]$FFmpeg,
    [string]$RealEsrgan,
    [string[]]$Videos
)

$ErrorActionPreference = 'Stop'
$UrlFFmpeg = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'
$UrlRealEsrgan = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
$Filtro = 'hqdn3d=0.6:0.5:0.9:0.75,deband=1thr=0.006:2thr=0.006:3thr=0.006:range=4:blur=1:coupling=1:direction=6.283185'

function Paso($texto) { Write-Host ''; Write-Host "==> $texto" -ForegroundColor Cyan }
function Ok($texto) { Write-Host "    $texto" -ForegroundColor Green }
function Fallo($texto) { Write-Host ''; Write-Host "ERROR: $texto" -ForegroundColor Red; exit 1 }

function Ejecutar($exe, [string[]]$argumentos, $log) {
    $p = Start-Process -FilePath $exe -ArgumentList $argumentos -NoNewWindow -Wait -PassThru `
                       -RedirectStandardError $log -RedirectStandardOutput "$log.out"
    if ($p.ExitCode -ne 0) { Fallo "Falló $([IO.Path]::GetFileName($exe)). Detalles en $log" }
}

function Q($s) { '"' + $s + '"' }

$peliculas = Join-Path $Juego 'MOVIES'
if (-not (Test-Path -LiteralPath (Join-Path $Juego 'EVOLUTION.EXE')) -or -not (Test-Path -LiteralPath $peliculas)) {
    Fallo "No encuentro el juego en '$Juego'. Indica la carpeta con -Juego."
}

# --- Herramientas ---------------------------------------------------------------
$cache = Join-Path $env:LOCALAPPDATA 'EvolutionFix'
function Descargar($url, $nombre) {
    $zip = Join-Path $cache "$nombre.zip"
    $dir = Join-Path $cache $nombre
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $cache | Out-Null
        Write-Host "    Descargando $url"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force
        Remove-Item $zip
    }
    return $dir
}

Paso 'Preparando FFmpeg y Real-ESRGAN'
if (-not $FFmpeg) {
    $FFmpeg = Get-ChildItem -LiteralPath (Descargar $UrlFFmpeg 'ffmpeg') -Recurse -Filter ffmpeg.exe | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $RealEsrgan) {
    $RealEsrgan = Get-ChildItem -LiteralPath (Descargar $UrlRealEsrgan 'realesrgan') -Recurse -Filter realesrgan-ncnn-vulkan.exe | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $FFmpeg -or -not (Test-Path -LiteralPath $FFmpeg)) { Fallo 'No encuentro ffmpeg.exe.' }
if (-not $RealEsrgan -or -not (Test-Path -LiteralPath $RealEsrgan)) { Fallo 'No encuentro realesrgan-ncnn-vulkan.exe.' }
$modelos = Join-Path (Split-Path $RealEsrgan) 'models'
if (-not (Test-Path (Join-Path $modelos 'realesr-animevideov3-x4.bin'))) { Fallo "Falta el modelo realesr-animevideov3-x4 en $modelos." }
Ok "FFmpeg: $FFmpeg"
Ok "Real-ESRGAN: $RealEsrgan"

# --- Vídeos -------------------------------------------------------------------------
$hd = Join-Path $peliculas 'HD'
New-Item -ItemType Directory -Force -Path $hd | Out-Null
$trabajo = Join-Path $env:TEMP 'evolution-videos-hd'
$lista = Get-ChildItem -LiteralPath $peliculas -Filter '*_30.SMK' | Sort-Object Length
$inicio = Get-Date
foreach ($smk in $lista) {
    $nombre = $smk.BaseName.ToUpperInvariant() -replace '_30$', ''
    if ($Videos -and ($Videos | ForEach-Object { $_.ToUpperInvariant() }) -notcontains $nombre) { continue }
    $salida = Join-Path $hd "$nombre.mp4"
    if (Test-Path -LiteralPath $salida) { Ok "$nombre ya existe, se salta"; continue }

    Paso "$nombre ($([math]::Round($smk.Length / 1MB, 1)) MB)"
    if (Test-Path $trabajo) { Remove-Item $trabajo -Recurse -Force }
    $limpio = Join-Path $trabajo 'limpio'
    $grande = Join-Path $trabajo 'grande'
    New-Item -ItemType Directory -Force -Path $limpio, $grande | Out-Null
    $t0 = Get-Date

    Write-Host '    1/3 Extrayendo y limpiando fotogramas'
    Ejecutar $FFmpeg @('-y', '-hide_banner', '-i', (Q $smk.FullName), '-map', '0:v:0', '-an', '-vf', $Filtro,
                       '-fps_mode', 'passthrough', '-start_number', '1', (Q (Join-Path $limpio '%04d.png'))) (Join-Path $trabajo '1.log')
    $fotogramas = (Get-ChildItem -LiteralPath $limpio -Filter *.png).Count

    Write-Host "    2/3 Ampliando $fotogramas fotogramas con la tarjeta gráfica"
    Ejecutar $RealEsrgan @('-i', (Q $limpio), '-o', (Q $grande), '-m', (Q $modelos), '-n', 'realesr-animevideov3',
                           '-s', '4', '-t', '256', '-j', '1:4:2', '-f', 'png') (Join-Path $trabajo '2.log')
    if ((Get-ChildItem -LiteralPath $grande -Filter *.png).Count -ne $fotogramas) { Fallo "Real-ESRGAN no generó todos los fotogramas de $nombre." }

    Write-Host '    3/3 Codificando el MP4 con el sonido original'
    $temporal = Join-Path $trabajo "$nombre.mp4"
    Ejecutar $FFmpeg @('-y', '-hide_banner', '-framerate', '30', '-start_number', '1', '-i', (Q (Join-Path $grande '%04d.png')),
                       '-i', (Q $smk.FullName), '-map', '0:v', '-map', '1:a?', '-frames:v', "$fotogramas",
                       '-c:v', 'libx264', '-preset', 'slow', '-crf', '14', '-pix_fmt', 'yuv420p',
                       '-vf', 'scale=out_color_matrix=bt709:out_range=tv,setsar=1',
                       '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'iec61966-2-1', '-color_range', 'tv',
                       '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', (Q $temporal)) (Join-Path $trabajo '3.log')
    Move-Item -LiteralPath $temporal -Destination $salida
    Ok ("Hecho en {0:mm\:ss}" -f ((Get-Date) - $t0))
}
if (Test-Path $trabajo) { Remove-Item $trabajo -Recurse -Force }

Write-Host ''
Write-Host ("Listo en {0:hh\:mm\:ss}. Los vídeos HD están en $hd" -f ((Get-Date) - $inicio)) -ForegroundColor Green
Write-Host 'Para volver a los vídeos originales, borra esa carpeta.'
