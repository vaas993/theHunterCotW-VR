@echo off
REM theHunter: Call of the Wild VR - x64 build (the game is a 64-bit process).
setlocal enabledelayedexpansion

set ROOT=%~dp0
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" (
    echo [!] vcvarsall.bat not found at "%VCVARS%"
    exit /b 1
)
call "%VCVARS%" x64 >nul || exit /b 1

set OUT=%ROOT%build
set OBJ=%OUT%\obj
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

set XR=%ROOT%thirdparty\openxr
set MH=%ROOT%thirdparty\minhook

rem /utf-8 : the sources ARE UTF-8, and without this MSVC reads them as the
rem system ANSI codepage. Every non-ASCII character in a string literal then
rem became two or three characters - the panel's bullet separators showed up as
rem "a-EUR-cent" and the scroll arrows as boxes. Nothing in the code was wrong;
rem the compiler had been told the wrong thing about the file it was reading.
set CFLAGS=/nologo /std:c++17 /EHsc /O2 /MT /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX
set NGX=%ROOT%thirdparty\dlss
set INCLUDES=/I"%XR%\include" /I"%MH%\include" /I"%MH%\src" /I"%ROOT%thirdparty" /I"%NGX%\include"

rem A setting that can be read but not written comes back OFF after every exit,
rem which is indistinguishable from a broken feature. Caught here, not in the
rem headset - it has cost four test cycles already.
python "%~dp0tools\checkconfig.py" || exit /b 1

rem Two buttons claim to put you on the tested settings - the panel's and the
rem launcher's - and they are written in different languages in different files.
rem Two recommendations that disagree is a configuration nobody has ever run.
python "%~dp0tools\checkrecommend.py" || exit /b 1

echo === MinHook (x64) ===
cl %CFLAGS% /c /Fo"%OBJ%\\" /I"%MH%\include" ^
   "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1

echo === cotwvr.dll ===
cl %CFLAGS% %INCLUDES% /LD /Fe"%OUT%\cotwvr.dll" /Fo"%OBJ%\\" ^
   "%ROOT%src\cotwvr\dllmain.cpp" ^
   "%ROOT%src\cotwvr\log.cpp" ^
   "%ROOT%src\cotwvr\config.cpp" ^
   "%ROOT%src\cotwvr\apex.cpp" ^
   "%ROOT%src\cotwvr\render_hook.cpp" ^
   "%ROOT%src\cotwvr\vr.cpp" ^
   "%ROOT%src\cotwvr\rdoc.cpp" ^
   "%ROOT%src\cotwvr\camera_probe.cpp" ^
   "%ROOT%src\cotwvr\stereo.cpp" ^
   "%ROOT%src\cotwvr\overlay.cpp" ^
   "%ROOT%src\cotwvr\audio.cpp" ^
   "%ROOT%src\cotwvr\framescan.cpp" ^
   "%ROOT%src\cotwvr\cbscan.cpp" ^
   "%ROOT%src\cotwvr\taa.cpp" ^
   "%ROOT%src\cotwvr\post.cpp" ^
   "%ROOT%src\cotwvr\dlss.cpp" ^
   "%ROOT%src\cotwvr\hwbp.cpp" ^
   "%ROOT%src\cotwvr\jitterhunt.cpp" ^
   "%ROOT%src\cotwvr\frame_hook.cpp" ^
   "%ROOT%src\cotwvr\headtrack.cpp" ^
   "%ROOT%src\cotwvr\gamesettings.cpp" ^
   "%ROOT%src\cotwvr\cheats.cpp" ^
   "%OBJ%\buffer.obj" "%OBJ%\hook.obj" "%OBJ%\trampoline.obj" "%OBJ%\hde64.obj" ^
   /link /SUBSYSTEM:WINDOWS "%XR%\native\x64\release\lib\openxr_loader.lib" ^
   "%NGX%\lib\x64\nvsdk_ngx_s.lib" ^
   d3d11.lib dxgi.lib dxguid.lib user32.lib gdi32.lib ole32.lib shell32.lib xinput.lib ^
   winmm.lib advapi32.lib || exit /b 1

echo === XINPUT9_1_0.dll (proxy) ===
cl %CFLAGS% /LD /Fe"%OUT%\XINPUT9_1_0.dll" /Fo"%OBJ%\\" ^
   "%ROOT%src\proxy\proxy.cpp" ^
   /link /DEF:"%ROOT%src\proxy\xinput9_1_0.def" /SUBSYSTEM:WINDOWS user32.lib || exit /b 1

copy /y "%XR%\native\x64\release\bin\openxr_loader.dll" "%OUT%\" >nul || exit /b 1

echo.
echo Build finished. Artifacts in %OUT%:
for %%F in ("%OUT%\cotwvr.dll" "%OUT%\XINPUT9_1_0.dll" "%OUT%\openxr_loader.dll") do (
    if exist %%F (
        for %%A in (%%F) do echo    %%~nxA  %%~zA bytes  %%~tA
    ) else (
        echo    [!] MISSING %%~nxF
        exit /b 1
    )
)
endlocal
