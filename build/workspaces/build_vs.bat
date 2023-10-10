setlocal
cd /d %~dp0

call update-workspaces.bat

MSBuild.exe vs2017\pyrogenesis.sln /p:Configuration=Release /p:PlatformTarget=win32
