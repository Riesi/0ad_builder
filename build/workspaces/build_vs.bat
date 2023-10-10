call update-workspaces.bat

Rem winget install vswhere

FOR /F "tokens=* USEBACKQ" %%F IN (`vswhere.exe -latest -find "**/Bin/MSBuild.exe"`) DO (
SET msbpath=%%F
)
Rem "%msbpath%" vs2017/pyrogenesis.sln /p:Configuration=Release /p:PlatformTarget=win32
MSBuild.exe build\workspaces\vs2017\pyrogenesis.sln /p:Configuration=Release /p:PlatformTarget=win32
