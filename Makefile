libmemory_track.so: memory_track.cpp
	c++ -O2 -shared -fPIC -std=c++11 memory_track.cpp -o libmemory_track.so
