make:
	g++ -std=c++17 1.cpp -o 1 -lncurses -pthread


delete:
	-rm 1 process_monitor.log