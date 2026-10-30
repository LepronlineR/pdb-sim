@echo off
rem Compiles the GLSL shaders here to the SPIR-V the app loads (*.spv).
rem Needs glslang: on the PATH as glslang.exe (it ships with the Vulkan SDK as
rem glslangValidator.exe, which works too), or set GLSLANG to its full path.
setlocal
cd /d "%~dp0"
if not defined GLSLANG (
	where glslang >nul 2>nul && set GLSLANG=glslang
)
if not defined GLSLANG (
	where glslangValidator >nul 2>nul && set GLSLANG=glslangValidator
)
if not defined GLSLANG (
	echo glslang not found: put it on the PATH or set GLSLANG to glslang.exe.
	exit /b 1
)
for %%s in (triangle pbr skybox) do (
	"%GLSLANG%" -V %%s.vert -o %%s.vert.spv || exit /b 1
	"%GLSLANG%" -V %%s.frag -o %%s.frag.spv || exit /b 1
)
echo Shaders compiled.
