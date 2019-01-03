all: da_proc

da_proc: da_proc.cpp
	g++ -std=c++11 -O2 -pthread -o da_proc da_proc.cpp -D_GLIBCXX_USE_CXX11_ABI=0 -lboost_serialization

clean:
	rm da_proc
	rm membership
	rm *.out