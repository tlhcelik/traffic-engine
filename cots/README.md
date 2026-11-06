# cots

This folder contains the third-party components (COTS) of Traffic Engine.
Git does not keep the contents of this folder. Only this file is in git.

`build/setup_cots.ps1` fills this folder. Double-click `build/setup_cots.bat` to run it.
The build scripts also run it if this folder is not ready.

For each component, the script does these steps in sequence:

1. It looks in this folder. If the component is here and complete, it uses it.
2. It scans the computer. If it finds the component, it makes a junction (a folder link)
   here, or it copies the files here if they are small.
3. If the component is not on the computer, it downloads the component into this folder.
   It compiles the component with MSVC if necessary (Xerces-C).

| Folder       | Component                     | Use                                    |
|--------------|-------------------------------|----------------------------------------|
| `qt5`        | Qt 5 (MSVC, 64-bit)           | User interface, OpenGL widget, network |
| `boost`      | Boost (headers)               | Spatial index (R-tree), route graph    |
| `xerces-c`   | Apache Xerces-C               | All XML files (configuration, `.te`)   |
| `7zip`       | 7-Zip                         | Compression of old log files           |
| `cmake`      | CMake                         | Build system                           |
| `innosetup`  | Inno Setup (optional)         | Setup file of the release package      |

The script also writes these files:

- `cots_env.bat`: the environment for the build scripts.
- `cots_report.txt`: the result of the last scan.
