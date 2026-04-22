@echo off
chcp 65001 >nul
echo Starting Claude Buddy Web Mode...
echo.
echo HTTP:  http://localhost:9876
echo WS:    ws://localhost:9877
echo.
cd /d "C:\Users\HONOR\m5-paper-buddy"
python tools\claude_code_bridge.py --web --budget 200000 --host 127.0.0.1
