# Xcaler Disk Analyzer

Desarrollado por **[NovoXcale](https://novoxcale.com)**.

Aplicación nativa y portátil para Windows (un único `.exe` sin instalador ni dependencias externas) diseñada para analizar el espacio ocupado en disco en vivo, mostrando el tamaño de cada carpeta en la raíz y permitiendo navegar por subcarpetas para identificar rápidamente qué consume el almacenamiento.

## Características

- **Nativa y portátil:** Compilada con Win32 API en C++17 (`-static`), sin instalador ni runtimes adicionales.
- **Análisis en vivo en segundo plano:** La interfaz se mantiene fluida mientras escanea, ordenando automáticamente las carpetas de mayor a menor tamaño conforme termina cada una.
- **Resumen del disco:** Muestra capacidad total, espacio usado, espacio libre y total analizado.
- **Barra visual de uso:** Columna con barra de color proporcional (verde `< 20%`, ámbar `20%–50%`, rojo `≥ 50%`).
- **Navegación rápida:**
  - Selector de unidades (`C:\`, `D:\`, etc.), barra de ruta editable o botón **Examinar...** para cualquier carpeta.
  - Doble clic en cualquier carpeta para entrar y analizar su interior; botón **Subir** para volver al nivel superior.
  - Clic en las cabeceras de columna para ordenar por nombre, tamaño, porcentaje, archivos o subcarpetas.
  - Menú contextual (clic derecho): *Analizar esta carpeta*, *Abrir en el Explorador*, *Copiar ruta*.
- **Exportación a CSV:** Guarda el informe completo en formato CSV (UTF-8 con BOM, compatible con Excel).
- **Segura:** Ignora *junctions* y enlaces simbólicos (*reparse points*) para evitar bucles o conteos duplicados, y maneja carpetas protegidas (`Acceso denegado`) sin detener el análisis.

## Descarga y aviso de Windows SmartScreen

Puedes descargar `xcaler-disk-analyzer.exe` desde la sección [**Releases**](../../releases) de GitHub (compilado automáticamente desde este código fuente mediante GitHub Actions).

> **Nota sobre Windows SmartScreen:** Al ser un ejecutable nuevo sin firma digital comercial (*Authenticode*), Windows puede mostrar inicialmente el aviso *"Windows protegió su PC"*.
> - Puedes verificar la integridad del binario comparando su hash con `SHA256SUMS.txt` (`certutil -hashfile xcaler-disk-analyzer.exe SHA256` en Windows).
> - Para ejecutarlo, haz clic en **Más información → Ejecutar de todas formas**, o compílalo tú mismo desde el código fuente con las instrucciones de abajo.

---

## Cómo compilar en Windows

1. Instala **[MSYS2](https://www.msys2.org/)** y abre la terminal **MSYS2 MINGW64**.
2. Instala la cadena de compilación MinGW-w64 y `make`:
   ```bash
   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-make
   ```
3. Clona el repositorio y compila directamente con `g++` y `windres`:
   ```bash
   git clone https://github.com/pananess/xcaler-disk-analyzer.git
   cd xcaler-disk-analyzer
   mkdir -p build
   windres src/resource.rc -O coff -o build/resource.o
   g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -c src/main.cpp -o build/main.o
   g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -c src/scanner.cpp -o build/scanner.o
   g++ build/main.o build/scanner.o build/resource.o -o xcaler-disk-analyzer.exe -municode -mwindows -static -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32
   ```

## Cómo compilar desde Linux (compilación cruzada)

1. Instala MinGW-w64:
   ```bash
   sudo apt-get update && sudo apt-get install -y mingw-w64
   ```
2. Compila el ejecutable:
   ```bash
   make
   ```
3. Se generará `xcaler-disk-analyzer.exe` listo para copiar y ejecutar en Windows.
