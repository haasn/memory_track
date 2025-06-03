libmemory_track.so: memory_track.cpp
	c++ -shared -fPIC -std=c++11 memory_track.cpp -o libmemory_track.so
