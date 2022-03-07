
$UV="D:\Maker\keil5\UV4\UV4.exe"

$UV_PROJ_PATH=".\code.uvproj"

echo building

echo "" > .\build_log.txt
cmd /c "$UV -j0 -b $UV_PROJ_PATH -o .\build_log.txt"

cat .\build_log.txt

echo downloading

echo "" > .\download_log.txt
cmd /c "$UV -j0 -f $UV_PROJ_PATH -o .\download_log.txt"

cat .\download_log.txt

cmd /c "del .\build_log.txt"

cmd /c "del .\download_log.txt"

echo done...
