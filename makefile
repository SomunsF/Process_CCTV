make:
	g++ -std=c++17 main.cpp -o main -lncurses -pthread


delete:
	-rm main process_monitor.log