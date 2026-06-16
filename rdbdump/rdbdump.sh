g++ -std=c++11 dump.cc -o file_dump || exit

echo "run"

sudo ./file_dump $1
