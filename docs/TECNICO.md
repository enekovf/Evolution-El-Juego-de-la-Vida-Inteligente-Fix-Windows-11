# Detalles técnicos

Cómo funciona el arreglo por dentro. Todas las direcciones son direcciones virtuales del `EVOLUTION.EXE` de la edición española 1.2. SHA-256 del original:
`E9B7250C155212078AEC5F8A4D17DE4694B9ED73D3A2D174353A3ECAC1DF083A`

## Resumen

| Problema en Windows 10/11 | Arreglo |
|---|---|
| El instalador exige Windows 95 | `instalar.ps1` copia el juego sin usarlo |
| El juego lee su configuración de HKLM (necesitaría ser administrador) | Parche de 2 bytes en el exe: lee HKCU. La DLL crea los valores |
| SmartHeap 3.3 se cuelga y el juego tiene errores de memoria | `SH33W32.DLL` propia con un gestor de memoria tolerante |
| Los vídeos salen en negro y cambian la resolución a 640x480x16 | Parches en memoria del reproductor Smacker: se dibujan por GDI en una ventana |
| El juego pide el CD | Se fuerza su modo interno sin CD y las rutas pasan a ser relativas |
| Cuelgue tras algunos desastres | Se evita un bloqueo mutuo entre dos hilos del propio juego |
| La ayuda es WinHelp (.HLP), que Windows 11 no abre | Se convierte a HTML y la DLL la abre en el navegador |
| Mejoras | Vídeos HD, música en calidad de CD, zoom con la rueda, botones extra del ratón |

## El ejecutable

Lo único que se modifica en disco son dos bytes: `push 80000002h` (HKEY_LOCAL_MACHINE) pasa a `push 80000001h` (HKEY_CURRENT_USER) en `0x450041` y `0x460B5B` (offsets de archivo `0x4F441` y `0x5FF5B`). SHA-256 del resultado:
`F9957894E6BC35F26050015EB92959804552AD7B876B64A9CAF43214101DFBF1`

Todo lo demás lo hace la DLL en memoria, al cargarse. Antes de cada parche comprueba los bytes esperados: con otro ejecutable no cambia nada.

## SH33W32.DLL (`dll/sh33w32.c`)

El juego importa 14 ordinales de SmartHeap 3.3. La DLL los implementa y, desde `DllMain`, instala estos arreglos.

### Gestor de memoria

- `HeapAlloc` con una cabecera de 16 bytes (marca de bloque vivo o muerto, y tamaño), 64 bytes de holgura y memoria puesta a cero.
- Las liberaciones pasan por una cuarentena de 8192 bloques con su propio cerrojo. Se ignoran las de bloques que no están vivos.
- **Por qué:** el juego tiene desbordes, libera dos veces algunos bloques y escribe en bloques ya liberados. SmartHeap lo toleraba; el heap de Windows no.
- `_shi_enterCriticalSection` y `_shi_leaveCriticalSection` son cdecl y deben devolver un valor distinto de 0. Si no, el juego no inicializa el pool.
- El flag 8 de `MemReAllocPtr` equivale a `HEAP_REALLOC_IN_PLACE_ONLY`.

### Vídeos (reproductor Smacker)

- **Problema:** el reproductor ponía la pantalla en 640x480x16 y copiaba cada fotograma con `BltFast` a la superficie primaria de DirectDraw, que en Windows actuales queda negra. Además, esperaba cada fotograma en un bucle sin pausa.
- **Parches:**

| Dirección | Cambio |
|---|---|
| `0x450422` | Nivel cooperativo `11h` → `08h` (NORMAL, sin pantalla exclusiva) |
| `0x4504B4` | Se anula `SetDisplayMode`: la resolución de la pantalla no cambia |
| `0x4505AA` | `CreateSurface` → `CreateMovieSurface`, que fuerza RGB565 |
| `0x4506DA` | El formato de píxel se lee de la superficie fuera de pantalla |
| `0x450A1C` | `BltFast` → `VideoPresent`: `StretchBlt` (HALFTONE) por GDI a la ventana, en 4:3 centrado |
| `0x450877` | `SmackWait` → `WaitForFrame`: `Sleep(1)` y un tope de 250 ms (sin él, algunos vídeos colgaban el juego) |

### Vídeos HD

- La llamada a `SmackOpen` (`0x4503B4`) pasa por la DLL. Si existe `MOVIES\HD\<NOMBRE>.mp4` (el nombre del vídeo sin `_30`/`_15`), lo reproduce con MFPlay (Media Foundation) en la ventana del reproductor. Esc lo salta.
- Después deja que el reproductor del juego siga su curso normal: abre el `.SMK`, no dibuja su único fotograma, apaga su sonido y hace que su comprobación de Esc (`0x450883`) devuelva "pulsada", así que termina y libera todo como siempre.
- `videos-hd.ps1` genera los MP4:
  - FFmpeg con `hqdn3d=0.6:0.5:0.9:0.75` y `deband=1thr=0.006:2thr=0.006:3thr=0.006:range=4:blur=1:coupling=1:direction=6.283185`;
  - Real-ESRGAN `realesr-animevideov3` x4;
  - H.264 CRF 14, 30 fps, con el audio original en AAC a 192 kbps.

### Sin CD

- `0x438CEE`: `C7 45 B8 01 00 00 00` → `...00`. Activa el modo sin CD que trae el propio juego (`-nocdkjm`).
- **Rutas relativas:** vídeos (`0x44FEEC`, `0x4501E0`) y música (`0x464A7A`, `0x465A94`, `0x465C2E`, y los saltos de `0x464A74`, `0x465A92` y `0x465C2C`).
- **Carpeta actual:** unos stubs fijan el directorio actual en la carpeta del juego antes de usar cada ruta.

### Configuración

La DLL crea `HKCU\Software\Discovery Multimedia\Evolution`: `EXE_DIR` (siempre la carpeta actual del juego), `INSTALL_TYPE=ALLCOLOR` y `CDROM_SPEED=30`. Por eso la carpeta se puede mover sin reinstalar.

### Bloqueo mutuo tras los desastres

- **Causa:** el hilo que carga recursos (`0x4552A0`) tiene el cerrojo de su cola (CRITICAL_SECTION en `0x505E54`) y en `0x46B170` pide el cerrojo de un objeto. Otro hilo tiene ese objeto (`0x46B7xx`) y en `0x455B20` pide la cola. Ninguno avanza.
- **Arreglo:** la entrada de la IAT de `EnterCriticalSection` (`0x50ACD4`) pasa por la DLL. Si el hilo que tiene la cola no consigue otro cerrojo en 100 ms, suelta la cola, toma el otro cerrojo y vuelve a por la cola. Sin contención, el comportamiento es el mismo de siempre.

### Ratón

Un hook `WH_GETMESSAGE` en el hilo principal:

- **Rueda sobre la ventana "Mapa":** AvPág (aumentar zoom) o RePág (disminuir zoom). Hay 3 niveles: 100, 75 y 50 %.
- **Botón central:** `WM_COMMAND 32872` (Lista de criaturas, F5).
- **Botón lateral de arriba (X2):** `32800` (Árbol de la vida, F6).
- **Botón lateral de abajo (X1):** `32785` (Buscar, Ctrl+B).
- **Diálogos:** si hay un diálogo abierto, los botones no hacen nada.

### Ayuda

- **Enganche:** `WinHelpA` (en las importaciones del exe y de MFC42) pasa por la DLL. Si existe `AYUDA\index.html`, la abre en el navegador; si no, llama a la original.
- **Conversión:** la hace `herramientas/Herramientas.cs` (`EvoFix.Ayuda.Convert`), un conversor propio de WinHelp 4.0. Genera una página con los 55 temas, el índice de `EVOLUTION.CNT`, los saltos como enlaces internos y las imágenes (mapas de bits y metarchivos WMF, que se dibujan con GDI). Detalles del formato que costó averiguar:
  - **Hash de los identificadores de tema:** `h = h*43 + v`. `'_'` vale 13, `'0'` vale 10 y el resto de caracteres valen su código menos `0x30`.
  - **TOPICOFFSET de cada registro:** `(bloque << 15)` más la suma de los `TopicLength` de los registros de texto desde el primer registro de ese bloque.

### Diagnóstico

Con un archivo vacío `evo-trace.on` en la carpeta del juego, la DLL escribe `evo-trace.log` con:

- aperturas de archivos y lecturas del registro;
- recursos cargados;
- excepciones;
- un vigilante de cuelgues que vuelca el estado de los hilos si la ventana no responde en 8 segundos. Durante un vídeo es normal que no responda.

## Música

- **El CD es mixto:** la pista 1 es de datos y las 2 a 6 son de audio.
- **Los WAV de `MUSIC`:** son la misma música en IMA ADPCM de 4 bits a 22 kHz.
- **Con una imagen BIN/CUE:** el instalador extrae las pistas de audio (44,1 kHz, 16 bits) y las pone en su lugar. El juego las reproduce sin cambios.
- **Correspondencia**, comprobada comparando el sonido:

| Archivo del juego | Pista del CD |
|---|---|
| `TRACK 1.WAV` | 3 |
| `TRACK 2.WAV` | 4 |
| `TRACK 3.WAV` | 5 |
| `TRACK 4.WAV` | 6 |
| `TRACK 5.WAV` | 2 |

- **Imágenes con el inicio de las pistas mal marcado:** algunas marcan el `INDEX 01` de las pistas 3 a 6 con 2 segundos de retraso. El extractor lo detecta (2 s con sonido justo después de un segundo de silencio) y lo corrige.

## Probado y descartado

- **dgVoodoo2 y DDrawCompat:** no arreglaron los vídeos negros.
- **Declarar el juego DPI-aware** para evitar que Windows lo suavice con escalas como el 125 %: los diálogos del juego tienen posiciones fijas y los textos se descolocan.
- **El parche oficial 1.2 en inglés:** la edición española ya es la 1.2 (lo indica "Product version" del exe) y el parche la pasaría al inglés.
