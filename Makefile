all:
	g++ -std=c++17 systemc_server.cpp -lsystemc -lm -o systemc_server \
    -I/home/x/implementations/systemc-crqa/systemc/install/include \
    -L/home/x/implementations/systemc-crqa/systemc/install/lib

clean:
	rm crqa-model
