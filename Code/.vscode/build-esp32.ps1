echo ""
echo "idf.py.exe build"
echo ""

idf.py.exe build

echo ""
echo "idf.py.exe --port com24 --baud 921600 flash"
echo ""

idf.py.exe --port com24 --baud 921600 flash

echo ""
echo "idf.py.exe --port com24 --monitor-baud 921600 monitor"
echo ""

idf.py.exe --port com24 --baud 921600 monitor
