@echo off
setlocal

set "ROOT=%~dp0"
rem The KIWI tree keeps the sources directly beside this script.
set "SRCDIR=%ROOT:~0,-1%"
set "OBJDIR=%ROOT%obj"
set "BINDIR=%ROOT%bin"
set "EXENAME=cod2rad64_our.exe"

rem Source-only bundle: provide Embree separately and point EMBREEDIR at it.
rem Example:
rem   set EMBREEDIR=D:\cod2rad\embree\embree-4.4.1.x64.windows
if "%EMBREEDIR%"=="" set "EMBREEDIR=%ROOT%embree\embree-4.4.1.x64.windows"

if not exist "%EMBREEDIR%\include\embree4\rtcore.h" (
    echo EMBREEDIR is not set to an Embree 4 x64 Windows SDK.
    echo Set it first, for example:
    echo   set EMBREEDIR=C:\path\to\embree-4.4.1.x64.windows
    exit /b 1
)

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if errorlevel 1 (
    echo Could not initialize MSVC x64 build tools.
    exit /b 1
)

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%BINDIR%" mkdir "%BINDIR%"
cd /d "%OBJDIR%"
del /q *.obj *.map *.pdb 2>nul

echo === ASSEMBLING x87 trig ===
ml64 /nologo /c /Fo"x87_trig.obj" "%SRCDIR%\x87_trig.asm"
if errorlevel 1 exit /b 1

echo === COMPILING cod2rad64 sources ===
cl /nologo /c /MT /O2 /fp:strict /W0 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRCDIR%" /I"%SRCDIR%\zlib" /I"%SRCDIR%\minizip" /I"%EMBREEDIR%\include" ^
   "%SRCDIR%\aabbtree.c" ^
   "%SRCDIR%\adaptive.c" ^
   "%SRCDIR%\assertive.c" ^
   "%SRCDIR%\bspfile.c" ^
   "%SRCDIR%\cm_tracebox.c" ^
   "%SRCDIR%\cmdlib.c" ^
   "%SRCDIR%\collvec.c" ^
   "%SRCDIR%\cmdline.c" ^
   "%SRCDIR%\cod2rad.c" ^
   "%SRCDIR%\com_files.c" ^
   "%SRCDIR%\com_math.c" ^
   "%SRCDIR%\com_memory.c" ^
   "%SRCDIR%\com_shared.c" ^
   "%SRCDIR%\compile.c" ^
   "%SRCDIR%\dobj.c" ^
   "%SRCDIR%\dvar.c" ^
   "%SRCDIR%\geometry.c" ^
   "%SRCDIR%\groundlight.c" ^
   "%SRCDIR%\lightgrid.c" ^
   "%SRCDIR%\lighting.c" ^
   "%SRCDIR%\lightmap_bleed.c" ^
   "%SRCDIR%\linearmapping.c" ^
   "%SRCDIR%\mapio.c" ^
   "%SRCDIR%\materials.c" ^
   "%SRCDIR%\modelcollision.c" ^
   "%SRCDIR%\pointlights.c" ^
   "%SRCDIR%\poly2d.c" ^
   "%SRCDIR%\polyfile.c" ^
   "%SRCDIR%\targetplatform.c" ^
   "%SRCDIR%\print.c" ^
   "%SRCDIR%\q_parse.c" ^
   "%SRCDIR%\q_shared.c" ^
   "%SRCDIR%\r_image_wavelet.c" ^
   "%SRCDIR%\r_imagedecode.c" ^
   "%SRCDIR%\r_light_load_obj.c" ^
   "%SRCDIR%\r_xsurface.c" ^
   "%SRCDIR%\r_xsurface_load_obj.c" ^
   "%SRCDIR%\scr_memorytree.c" ^
   "%SRCDIR%\scr_stringlist.c" ^
   "%SRCDIR%\threads.c" ^
   "%SRCDIR%\uv_repack.c" ^
   "%SRCDIR%\wavelet_tables.c" ^
   "%SRCDIR%\win_common.c" ^
   "%SRCDIR%\wrappers.c" ^
   "%SRCDIR%\xanim_public.c" ^
   "%SRCDIR%\xmodel.c" ^
   "%SRCDIR%\xmodel_load_obj.c" ^
   "%SRCDIR%\xmodel_utils.c"
if errorlevel 1 exit /b 1

echo === COMPILING Embree bridge ===
cl /nologo /c /MT /O2 /fp:strict /W0 /D_CRT_SECURE_NO_WARNINGS /EHsc ^
   /I"%SRCDIR%" /I"%SRCDIR%\zlib" /I"%SRCDIR%\minizip" /I"%EMBREEDIR%\include" ^
   /Tp"%SRCDIR%\embree_trace.cpp"
if errorlevel 1 exit /b 1

echo === COMPILING zlib ===
cl /nologo /c /MT /O2 /fp:strict /W0 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRCDIR%\zlib" ^
   "%SRCDIR%\zlib\adler32.c" ^
   "%SRCDIR%\zlib\crc32.c" ^
   "%SRCDIR%\zlib\deflate.c" ^
   "%SRCDIR%\zlib\infblock.c" ^
   "%SRCDIR%\zlib\infcodes.c" ^
   "%SRCDIR%\zlib\inffast.c" ^
   "%SRCDIR%\zlib\inflate.c" ^
   "%SRCDIR%\zlib\inftrees.c" ^
   "%SRCDIR%\zlib\infutil.c" ^
   "%SRCDIR%\zlib\trees.c" ^
   "%SRCDIR%\zlib\uncompr.c" ^
   "%SRCDIR%\zlib\zutil.c"
if errorlevel 1 exit /b 1

echo === COMPILING minizip ===
cl /nologo /c /MT /O2 /fp:strict /W0 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRCDIR%\zlib" /I"%SRCDIR%\minizip" ^
   "%SRCDIR%\minizip\ioapi.c" ^
   "%SRCDIR%\minizip\unzip.c"
if errorlevel 1 exit /b 1

echo === LINKING %EXENAME% ===
link /nologo /SUBSYSTEM:CONSOLE /STACK:8388608 /MAP:"%EXENAME%.map" /OUT:"%BINDIR%\%EXENAME%" ^
     *.obj ^
     libcmt.lib kernel32.lib user32.lib gdi32.lib advapi32.lib winmm.lib ^
     "%EMBREEDIR%\lib\embree4.lib" "%EMBREEDIR%\lib\tbb.lib"
if errorlevel 1 exit /b 1

echo === COPYING RUNTIME DEPENDENCIES ===
copy /Y "%EMBREEDIR%\bin\embree4.dll" "%BINDIR%\embree4.dll" >nul
if errorlevel 1 exit /b 1
copy /Y "%EMBREEDIR%\bin\tbb12.dll" "%BINDIR%\tbb12.dll" >nul
if errorlevel 1 exit /b 1
copy /Y "%EMBREEDIR%\bin\tbbmalloc.dll" "%BINDIR%\tbbmalloc.dll" >nul
if errorlevel 1 exit /b 1

echo === BUILD COMPLETE: %BINDIR%\%EXENAME% ===
echo Runtime DLLs copied beside the executable.
