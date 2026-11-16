# Xcaler Disk Analyzer

Aplicación nativa y portátil para Windows (un único `.exe` sin instalador ni dependencias externas) diseñada para analizar el espacio ocupado en disco, mostrando el tamaño de cada carpeta en la raíz (y permitiendo navegar por subcarpetas) para identificar rápidamente qué está consumiendo el almacenamiento.

## Características

- **Nativa y portátil:** Compilada con Win32 API (`x86_64-w64-mingw32-g++ -static`), sin instalador ni runtimes adicionales.
- **Análisis en vivo en segundo plano:** La interfaz se mantiene fluida mientras escanea, ordenando automáticamente las carpetas de mayor a menor tamaño conforme termina cada una.
- **Resumen del disco:** Muestra capacidad total, espacio usado, espacio libre y total analizado.
- **Barra visual de uso:** Columna con barra de color proporcional (verde < 20%, ámbar 20–50%, rojo ≥ 50%).
- **Navegación rápida:**
  - Selector de unidades (`C:\`, `D:\`, etc.) o botón **Examinar...** para cualquier carpeta.
  - Doble clic en cualquier carpeta para entrar y analizar su interior; botón **Subir** para volver al nivel superior.
  - Clic en las cabeceras de columna para ordenar por nombre, tamaño, porcentaje, archivos o subcarpetas.
  - Menú contextual (clic derecho): *Analizar esta carpeta*, *Abrir en el Explorador*, *Copiar ruta*.
- **Exportación a CSV:** Guarda el informe completo en formato CSV (UTF-8 con BOM, compatible con Excel).
- **Segura:** Ignora *junctions* y enlaces simbólicos (*reparse points*) para evitar bucles o conteos duplicados, y maneja carpetas protegidas (`Acceso denegado`) sin detener el análisis.

## Cómo compilar el `.exe` desde Linux

1. Instala el compilador cruzado MinGW-w64:
   ```bash
   sudo apt-get update && sudo apt-get install -y mingw-w64
   ```
2. Compila el ejecutable:
   ```bash
   make
   ```
3. Copia el archivo generado `xcaler-disk-analyzer.exe` a cualquier PC con Windows y ejecútalo directamente.
