@echo off
(
scripts\mksln.bat Debug
cmake --build .\build\ --target "KIWI-mp"
cmake --build .\build\ --target "KIWI-dedi"
echo %cd%
)