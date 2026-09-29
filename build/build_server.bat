@echo off
setlocal

if exist "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
) else if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
) else (
    for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" (
            call "%%i\VC\Auxiliary\Build\vcvarsall.bat" x64
        )
    )
)

if %errorlevel% neq 0 (
    echo Failed to initialize x64 Visual Studio environment.
    exit /b %errorlevel%
)
echo Building XiahServerEx Release x64...
msbuild XiahServerEx.vcxproj /p:Configuration=Release /p:Platform=x64
if %errorlevel% neq 0 (
    echo Server build failed!
    exit /b %errorlevel%
)
echo Copying output to server root...
copy /y Release\XiahServerEx.exe ..\XiahServerEx.exe >nul
if exist Release\XiahServerEx.pdb copy /y Release\XiahServerEx.pdb ..\XiahServerEx.pdb >nul
echo Server build succeeded!
