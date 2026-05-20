name: Build Windows VST3

on:
  push:
    branches: [ "main" ]
  workflow_dispatch: 

jobs:
  build-windows:
    name: Build on Windows
    runs-on: windows-latest

    steps:
    # 1. Pobranie Twojego kodu z GitHuba
    - name: Checkout Repository
      uses: actions/checkout@v4

    # 2. Pobranie frameworka JUCE
    - name: Clone JUCE
      run: git clone https://github.com/juce-framework/JUCE.git JUCE

    # 3. Konfiguracja projektu przy użyciu CMake
    - name: Configure CMake
      run: cmake -B build -G "Visual Studio 17 2022"

    # 4. Właściwa kompilacja wtyczki w trybie Release
    - name: Build Plugin
      run: cmake --build build --config Release

    # 5. Bezpieczna instalacja Inno Setup przez Chocolatey (TO ROZWIĄZUJE TWÓJ BŁĄD)
    - name: Install Inno Setup
      run: choco install innosetup --yes

    # 6. Uruchomienie kompilatora instalatora
    - name: Build Windows Installer
      run: "C:\\Program Files (x86)\\Inno Setup 6\\ISCC.exe" installer.iss

    # 7. Spakowanie i udostępnienie gotowego instalatora .exe do pobrania
    - name: Upload Windows Installer Artifact
      uses: actions/upload-artifact@v4
      with:
        name: DRUM_SAT_76-Windows-Installer
        path: DRUM_SAT_76_Windows_Installer.exe
