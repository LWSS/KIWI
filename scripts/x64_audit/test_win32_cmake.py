"""Validate the production native dependency selection without linking the game."""
from pathlib import Path
import os
import subprocess

root = Path.cwd()
out = Path(os.environ['TEMP'])/'kiwi-x64-epic/win32-cmake'
out.mkdir(parents=True, exist_ok=True)
(out/'main.cpp').write_text('int main(){return 0;}\n')
(out/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(NativeDependencyProbe CXX)
add_executable(${PROJECT_NAME} main.cpp)
add_custom_target(update_build_number)
set(SRC_DIR "''' + root.as_posix() + '''/src")
set(DEPS_DIR "''' + root.as_posix() + '''/deps")
set(BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(DXSDK_DIR "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)")
include("''' + root.as_posix() + '''/scripts/pre_build.cmake")
include("''' + root.as_posix() + '''/scripts/post_build.cmake")
''')
for arch, platform, steam in [('x86','Win32','steam_api'),('x64','x64','steam_api64')]:
    build = out/arch
    result = subprocess.run(['cmake','-S',str(out),'-B',str(build),'-G','Visual Studio 17 2022','-A',platform],capture_output=True,text=True)
    assert result.returncode == 0,result.stdout+result.stderr
    project = (build/'NativeDependencyProbe.vcxproj').read_text()
    assert '/machine:'+arch in project
    assert steam+'.lib' in project and steam+'.dll' in project
    assert 'lib/'+arch in project.replace('\\','/')
    assert ('mss64.lib' if arch=='x64' else 'mss32.lib') in project
    print(arch,'PASS: generated machine, DirectX, Miles and Steam paths')
